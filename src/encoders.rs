//! Which video encoder to use: NVIDIA NVENC, AMD AMF, Intel QSV or the CPU. Each is tried once at
//! start with a one-frame test encode, because ffmpeg lists every encoder it was built with
//! whether or not the GPU is there. The test goes through the same D3D11 device ddagrab captures
//! on, so a second GPU that can't take the captured frames (an AMD iGPU next to an NVIDIA card)
//! counts as missing.

use crate::config::{Encode, Video};
use crate::{state, util};
use serde::Serialize;
use std::collections::BTreeMap;
use std::path::Path;
use std::process::Stdio;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Vendor {
    Nvenc,
    Amf,
    Qsv,
    Cpu,
}

/// Tried in this order; the first that works is used.
pub const ORDER: [Vendor; 4] = [Vendor::Nvenc, Vendor::Amf, Vendor::Qsv, Vendor::Cpu];
pub const CODECS: [&str; 3] = ["h264", "hevc", "av1"];

impl Vendor {
    pub fn key(self) -> &'static str {
        match self {
            Vendor::Nvenc => "nvenc",
            Vendor::Amf => "amf",
            Vendor::Qsv => "qsv",
            Vendor::Cpu => "cpu",
        }
    }

    pub fn parse(s: &str) -> Option<Vendor> {
        ORDER.into_iter().find(|v| v.key() == s)
    }

    /// ffmpeg's name for this vendor's encoder of `codec`.
    pub fn encoder(self, codec: &str) -> &'static str {
        match (self, codec) {
            (Vendor::Nvenc, "hevc") => "hevc_nvenc",
            (Vendor::Nvenc, "av1") => "av1_nvenc",
            (Vendor::Nvenc, _) => "h264_nvenc",
            (Vendor::Amf, "hevc") => "hevc_amf",
            (Vendor::Amf, "av1") => "av1_amf",
            (Vendor::Amf, _) => "h264_amf",
            (Vendor::Qsv, "hevc") => "hevc_qsv",
            (Vendor::Qsv, "av1") => "av1_qsv",
            (Vendor::Qsv, _) => "h264_qsv",
            (Vendor::Cpu, "hevc") => "libx265",
            (Vendor::Cpu, "av1") => "libsvtav1",
            (Vendor::Cpu, _) => "libx264",
        }
    }
}

/// The vendors whose test encode worked, per codec, in `ORDER`.
#[derive(Clone, Debug, Default, Serialize)]
pub struct Detected(pub BTreeMap<String, Vec<Vendor>>);

impl Detected {
    pub fn works(&self, codec: &str, v: Vendor) -> bool {
        self.0.get(codec).is_some_and(|l| l.contains(&v))
    }

    fn any_gpu(&self) -> bool {
        self.0.values().flatten().any(|v| *v != Vendor::Cpu)
    }
}

/// The vendor for `codec` under the `video.encoder` setting: a forced one is used as is (it may
/// fail, which the recorder logs), "auto" takes the first that passed the test.
pub fn choose(setting: &str, codec: &str, found: &Detected) -> Option<Vendor> {
    match Vendor::parse(setting) {
        Some(v) => Some(v),
        None => ORDER.into_iter().find(|v| found.works(codec, *v)),
    }
}

/// The vendor to record `video` with, waiting for detection if it's still running.
pub fn pick(video: &Video) -> Option<Vendor> {
    choose(&video.encoder, &video.codec, &detected())
}

// ---- detection --------------------------------------------------------------------------------

static RESULT: Mutex<Option<Detected>> = Mutex::new(None);
static READY: Condvar = Condvar::new();
static STARTED: AtomicBool = AtomicBool::new(false);
/// A GPU driver can still be loading when Windows starts clipback, so finding none is retried.
const RETRIES: u32 = 4;
const RETRY_EVERY: Duration = Duration::from_secs(30);

/// Starts detection in the background (once). The recorder restarts if a later retry finds a GPU.
pub fn start() {
    if STARTED.swap(true, Ordering::SeqCst) {
        return;
    }
    std::thread::Builder::new()
        .name("encoders".into())
        .spawn(|| {
            let mut last = None;
            for attempt in 0..=RETRIES {
                let took = Instant::now();
                let found = detect(&state::ffmpeg());
                let summary = summary(&found);
                if last.as_ref() != Some(&summary) {
                    util::log(&format!("encoders ({:.1}s): {summary}", took.elapsed().as_secs_f64()));
                }
                let gpu = found.any_gpu();
                *RESULT.lock().unwrap() = Some(found);
                READY.notify_all();
                last = Some(summary);
                if gpu || attempt == RETRIES {
                    break;
                }
                let end = Instant::now() + RETRY_EVERY;
                while Instant::now() < end && !state::stopping() {
                    std::thread::sleep(Duration::from_millis(500));
                }
                if state::stopping() {
                    break;
                }
            }
        })
        .expect("spawning encoder detection");
}

/// The latest detection result (starting it and waiting for it if needed).
pub fn detected() -> Detected {
    start();
    let mut g = RESULT.lock().unwrap();
    while g.is_none() {
        g = READY.wait(g).unwrap();
    }
    g.clone().unwrap()
}

/// "h264 nvenc, cpu · hevc nvenc, cpu · av1 nvenc, cpu"
fn summary(d: &Detected) -> String {
    CODECS
        .iter()
        .map(|c| {
            let list: Vec<&str> = d.0.get(*c).map(|l| l.iter().map(|v| v.key()).collect()).unwrap_or_default();
            format!("{c} {}", if list.is_empty() { "none".into() } else { list.join(", ") })
        })
        .collect::<Vec<_>>()
        .join(" · ")
}

/// Tests every vendor for every codec: one thread per codec, about 0.3 s per test.
pub fn detect(ffmpeg: &Path) -> Detected {
    let threads: Vec<_> = CODECS
        .iter()
        .map(|codec| {
            let ffmpeg = ffmpeg.to_path_buf();
            std::thread::spawn(move || {
                let ok: Vec<Vendor> = ORDER.into_iter().filter(|v| test_encode(&ffmpeg, *v, codec)).collect();
                (codec.to_string(), ok)
            })
        })
        .collect();
    Detected(threads.into_iter().filter_map(|t| t.join().ok()).collect())
}

fn test_encode(ffmpeg: &Path, v: Vendor, codec: &str) -> bool {
    let Ok(mut child) = util::command(ffmpeg)
        .args(probe_args(v, codec))
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    else {
        return false;
    };
    util::job().adopt(&child);
    let deadline = Instant::now() + Duration::from_secs(15);
    while Instant::now() < deadline {
        if let Ok(Some(status)) = child.try_wait() {
            return status.success();
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    let _ = child.kill();
    let _ = child.wait();
    false
}

fn strings(a: &[&str]) -> Vec<String> {
    a.iter().map(|s| s.to_string()).collect()
}

/// One small frame through the same path a recording takes: uploaded to the capture D3D11 device
/// (ddagrab's frames are BGRA textures there), then mapped like `capture_filter` does.
pub fn probe_args(v: Vendor, codec: &str) -> Vec<String> {
    let mut a = strings(&["-hide_banner", "-loglevel", "error"]);
    let vf = match v {
        Vendor::Cpu => "format=yuv420p".to_string(),
        _ => {
            a.extend(strings(&["-init_hw_device", "d3d11va"]));
            format!("format=bgra,hwupload{}", capture_filter(v))
        }
    };
    a.extend(strings(&["-f", "lavfi", "-i", "color=s=256x256:d=0.1", "-vf"]));
    a.push(vf);
    a.extend(strings(&["-frames:v", "1", "-c:v", v.encoder(codec), "-f", "null", "-"]));
    a
}

// ---- recording --------------------------------------------------------------------------------

/// What goes after ddagrab in the capture graph. NVENC and AMF take its D3D11 textures as they
/// are; QSV maps them to its own surfaces (its encoders want NV12); the CPU needs them in memory.
pub fn capture_filter(v: Vendor) -> &'static str {
    match v {
        Vendor::Nvenc | Vendor::Amf => "",
        Vendor::Qsv => ",hwmap=derive_device=qsv,format=qsv,vpp_qsv=format=nv12",
        Vendor::Cpu => ",hwdownload,format=bgra,format=yuv420p",
    }
}

/// 0 (p1, fastest) … 6 (p7, best).
fn preset_step(preset: &str) -> usize {
    preset.strip_prefix('p').and_then(|n| n.parse::<usize>().ok()).map(|n| n.clamp(1, 7) - 1).unwrap_or(3)
}

/// The encoder and its rate control for recording. The keyframe interval, B-frames and the
/// segment muxer are the recorder's and the same for every vendor.
pub fn record_args(v: Vendor, codec: &str, enc: &Encode) -> Vec<String> {
    let kbps = |mbps: f64| format!("{}k", (mbps * 1000.0).round() as u64);
    let bitrate = kbps(enc.bitrate_mbps);
    let peak = kbps(enc.bitrate_mbps * if enc.rate_control == "cbr" { 1.0 } else { enc.peak_factor });
    let step = preset_step(&enc.preset);
    let cq = enc.cq.to_string();
    let mut a = strings(&["-c:v", v.encoder(codec)]);
    let mut add = |s: &[&str]| a.extend(strings(s));
    match v {
        Vendor::Nvenc => {
            add(&["-preset", &enc.preset]);
            match enc.rate_control.as_str() {
                // Constant quality: the bitrate is only a cap, so quiet scenes take far less space.
                "cq" => add(&["-rc", "vbr", "-cq", &cq, "-b:v", "0", "-maxrate", &peak, "-bufsize", &peak]),
                "cbr" => add(&["-rc", "cbr", "-b:v", &bitrate, "-maxrate", &bitrate, "-bufsize", &bitrate]),
                _ => add(&["-rc", "vbr", "-b:v", &bitrate, "-maxrate", &peak, "-bufsize", &peak]),
            }
            if enc.spatial_aq {
                add(&["-spatial-aq", "1"]);
            }
        }
        Vendor::Amf => {
            // AMF has three speed levels (AV1 a fourth); p1–p7 spread over them.
            let quality = match step {
                0 | 1 => "speed",
                2..=4 => "balanced",
                _ if codec == "av1" && step == 6 => "high_quality",
                _ => "quality",
            };
            add(&["-quality", quality]);
            match enc.rate_control.as_str() {
                // No capped constant quality in AMF: fixed QP, on AV1's 0–255 scale there.
                "cq" => {
                    let qp = if codec == "av1" { (enc.cq * 5).min(255).to_string() } else { cq.clone() };
                    add(&["-rc", "cqp", "-qp_i", &qp, "-qp_p", &qp]);
                }
                "cbr" => add(&["-rc", "cbr", "-b:v", &bitrate, "-maxrate", &bitrate, "-bufsize", &bitrate]),
                _ => add(&["-rc", "vbr_peak", "-b:v", &bitrate, "-maxrate", &peak, "-bufsize", &peak]),
            }
            if enc.spatial_aq {
                add(if codec == "av1" { &["-aq_mode", "caq"] } else { &["-vbaq", "1"] });
            }
        }
        Vendor::Qsv => {
            let presets = ["veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"];
            add(&["-preset", presets[step]]);
            // QSV picks its mode from what's set: maxrate = bitrate is CBR, a quality level with a
            // bitrate is QVBR (the closest it has to a capped constant quality), else VBR.
            match enc.rate_control.as_str() {
                "cq" => add(&["-global_quality", &cq, "-b:v", &bitrate, "-maxrate", &peak, "-bufsize", &peak]),
                "cbr" => add(&["-b:v", &bitrate, "-maxrate", &bitrate, "-bufsize", &bitrate]),
                _ => add(&["-b:v", &bitrate, "-maxrate", &peak, "-bufsize", &peak]),
            }
        }
        Vendor::Cpu => {
            // Fast presets only: this runs on the cores the game wants.
            if codec == "av1" {
                let presets = ["12", "11", "10", "10", "9", "8", "7"];
                add(&["-preset", presets[step]]);
            } else {
                let presets = ["ultrafast", "superfast", "veryfast", "veryfast", "faster", "fast", "medium"];
                add(&["-preset", presets[step]]);
            }
            match enc.rate_control.as_str() {
                // CRF capped by the peak bitrate. SVT-AV1's scale goes to 63.
                "cq" => {
                    let crf = if codec == "av1" { (enc.cq * 63 / 51).to_string() } else { cq.clone() };
                    add(&["-crf", &crf, "-maxrate", &peak, "-bufsize", &peak]);
                }
                // SVT-AV1 takes a peak only with CRF, and its CBR needs a low-delay mode too slow to
                // keep up, so both are plain VBR there.
                _ if codec == "av1" => add(&["-b:v", &bitrate]),
                "cbr" => add(&["-b:v", &bitrate, "-maxrate", &bitrate, "-bufsize", &bitrate]),
                _ => add(&["-b:v", &bitrate, "-maxrate", &peak, "-bufsize", &peak]),
            }
            // No extra keyframes on scene cuts, which would make segments longer than 2 s.
            match codec {
                "hevc" => add(&["-x265-params", "log-level=error:scenecut=0"]),
                "av1" => {}
                _ => add(&["-sc_threshold", "0"]),
            }
        }
    }
    a
}

// ---- editor exports (always H.264) ------------------------------------------------------------

/// The pixel format the export graph ends in: QSV's H.264 encoder takes only NV12.
pub fn render_pix_fmt(v: Vendor) -> &'static str {
    if v == Vendor::Qsv { "nv12" } else { "yuv420p" }
}

/// H.264 for an editor export: a target bitrate for the size presets, else high constant quality.
pub fn render_args(v: Vendor, video_kbps: Option<u64>) -> Vec<String> {
    let mut a = strings(&["-c:v", v.encoder("h264")]);
    let sized = video_kbps.map(|k| (format!("{k}k"), format!("{}k", k * 3 / 2), format!("{}k", k * 2)));
    let mut add = |s: &[&str]| a.extend(strings(s));
    match (v, &sized) {
        (Vendor::Nvenc, _) => {
            add(&["-preset", "p5"]);
            match &sized {
                Some((b, m, s)) => add(&["-rc", "vbr", "-multipass", "fullres", "-b:v", b, "-maxrate", m, "-bufsize", s]),
                None => add(&["-rc", "vbr", "-cq", "19", "-b:v", "0"]),
            }
        }
        (Vendor::Amf, Some((b, m, s))) => add(&["-quality", "quality", "-rc", "vbr_peak", "-b:v", b, "-maxrate", m, "-bufsize", s]),
        (Vendor::Amf, None) => add(&["-quality", "quality", "-rc", "cqp", "-qp_i", "19", "-qp_p", "19"]),
        (Vendor::Qsv, Some((b, m, s))) => add(&["-preset", "slower", "-b:v", b, "-maxrate", m, "-bufsize", s]),
        (Vendor::Qsv, None) => add(&["-preset", "slower", "-global_quality", "19"]),
        (Vendor::Cpu, Some((b, m, s))) => add(&["-preset", "medium", "-b:v", b, "-maxrate", m, "-bufsize", s]),
        (Vendor::Cpu, None) => add(&["-preset", "medium", "-crf", "19"]),
    }
    a
}

#[cfg(test)]
mod tests {
    use super::*;

    fn enc(rc: &str, aq: bool) -> Encode {
        Encode {
            preset: "p4".into(),
            rate_control: rc.into(),
            cq: 23,
            bitrate_mbps: 20.0,
            peak_factor: 2.0,
            spatial_aq: aq,
            game: None,
        }
    }

    fn line(a: &[String]) -> String {
        a.join(" ")
    }

    /// The exact arguments recording used before other vendors existed.
    #[test]
    fn nvenc_args_unchanged() {
        assert_eq!(
            line(&record_args(Vendor::Nvenc, "h264", &enc("vbr", false))),
            "-c:v h264_nvenc -preset p4 -rc vbr -b:v 20000k -maxrate 40000k -bufsize 40000k"
        );
        assert_eq!(
            line(&record_args(Vendor::Nvenc, "hevc", &enc("cbr", true))),
            "-c:v hevc_nvenc -preset p4 -rc cbr -b:v 20000k -maxrate 20000k -bufsize 20000k -spatial-aq 1"
        );
        let mut e = enc("cq", false);
        e.preset = "p7".into();
        assert_eq!(
            line(&record_args(Vendor::Nvenc, "av1", &e)),
            "-c:v av1_nvenc -preset p7 -rc vbr -cq 23 -b:v 0 -maxrate 40000k -bufsize 40000k"
        );
        assert_eq!(capture_filter(Vendor::Nvenc), "");
        assert_eq!(
            line(&render_args(Vendor::Nvenc, Some(1000))),
            "-c:v h264_nvenc -preset p5 -rc vbr -multipass fullres -b:v 1000k -maxrate 1500k -bufsize 2000k"
        );
        assert_eq!(line(&render_args(Vendor::Nvenc, None)), "-c:v h264_nvenc -preset p5 -rc vbr -cq 19 -b:v 0");
    }

    #[test]
    fn amf_args() {
        assert_eq!(
            line(&record_args(Vendor::Amf, "h264", &enc("vbr", true))),
            "-c:v h264_amf -quality balanced -rc vbr_peak -b:v 20000k -maxrate 40000k -bufsize 40000k -vbaq 1"
        );
        assert_eq!(
            line(&record_args(Vendor::Amf, "hevc", &enc("cbr", false))),
            "-c:v hevc_amf -quality balanced -rc cbr -b:v 20000k -maxrate 20000k -bufsize 20000k"
        );
        let mut e = enc("cq", true);
        e.preset = "p7".into();
        assert_eq!(
            line(&record_args(Vendor::Amf, "av1", &e)),
            "-c:v av1_amf -quality high_quality -rc cqp -qp_i 115 -qp_p 115 -aq_mode caq"
        );
        e.preset = "p1".into();
        assert!(line(&record_args(Vendor::Amf, "h264", &e)).contains("-quality speed -rc cqp -qp_i 23 -qp_p 23"));
        assert_eq!(capture_filter(Vendor::Amf), "");
        assert_eq!(line(&render_args(Vendor::Amf, None)), "-c:v h264_amf -quality quality -rc cqp -qp_i 19 -qp_p 19");
    }

    #[test]
    fn qsv_args() {
        assert_eq!(
            line(&record_args(Vendor::Qsv, "h264", &enc("vbr", true))),
            "-c:v h264_qsv -preset medium -b:v 20000k -maxrate 40000k -bufsize 40000k"
        );
        assert_eq!(
            line(&record_args(Vendor::Qsv, "av1", &enc("cq", false))),
            "-c:v av1_qsv -preset medium -global_quality 23 -b:v 20000k -maxrate 40000k -bufsize 40000k"
        );
        assert!(capture_filter(Vendor::Qsv).contains("hwmap=derive_device=qsv,format=qsv"));
        assert_eq!(render_pix_fmt(Vendor::Qsv), "nv12");
        assert_eq!(
            line(&render_args(Vendor::Qsv, Some(4000))),
            "-c:v h264_qsv -preset slower -b:v 4000k -maxrate 6000k -bufsize 8000k"
        );
    }

    #[test]
    fn cpu_args() {
        assert_eq!(
            line(&record_args(Vendor::Cpu, "h264", &enc("vbr", false))),
            "-c:v libx264 -preset veryfast -b:v 20000k -maxrate 40000k -bufsize 40000k -sc_threshold 0"
        );
        assert_eq!(
            line(&record_args(Vendor::Cpu, "hevc", &enc("cq", false))),
            "-c:v libx265 -preset veryfast -crf 23 -maxrate 40000k -bufsize 40000k -x265-params log-level=error:scenecut=0"
        );
        assert_eq!(
            line(&record_args(Vendor::Cpu, "av1", &enc("cbr", false))),
            "-c:v libsvtav1 -preset 10 -b:v 20000k"
        );
        assert_eq!(
            line(&record_args(Vendor::Cpu, "av1", &enc("cq", false))),
            "-c:v libsvtav1 -preset 10 -crf 28 -maxrate 40000k -bufsize 40000k"
        );
        assert_eq!(capture_filter(Vendor::Cpu), ",hwdownload,format=bgra,format=yuv420p");
        assert_eq!(line(&render_args(Vendor::Cpu, None)), "-c:v libx264 -preset medium -crf 19");
    }

    #[test]
    fn auto_takes_the_first_that_works() {
        let mut d = Detected::default();
        d.0.insert("h264".into(), vec![Vendor::Qsv, Vendor::Cpu]);
        d.0.insert("av1".into(), vec![Vendor::Cpu]);
        assert_eq!(choose("auto", "h264", &d), Some(Vendor::Qsv));
        assert_eq!(choose("auto", "av1", &d), Some(Vendor::Cpu));
        assert_eq!(choose("auto", "hevc", &d), None);
        assert_eq!(choose("nvenc", "hevc", &d), Some(Vendor::Nvenc)); // forced, even untested
        assert_eq!(summary(&d), "h264 qsv, cpu · hevc none · av1 cpu");
    }

    /// Runs the real test encodes: `cargo test --release -- --ignored --nocapture detect_here`.
    #[test]
    #[ignore]
    fn detect_here() {
        let ffmpeg = crate::config::Config::default().find_ffmpeg().unwrap();
        let took = Instant::now();
        let d = detect(&ffmpeg);
        println!("{} in {:.1}s", summary(&d), took.elapsed().as_secs_f64());
        let cfg = crate::config::Config::default();
        for v in ORDER {
            for codec in CODECS {
                let mut c = cfg.clone();
                c.video.codec = codec.into();
                println!("{} {codec}: {}", v.key(), crate::recorder::args(&c, &c.video.encode(None), v).join(" "));
            }
        }
    }

    #[test]
    fn probes_go_through_the_capture_device() {
        let p = line(&probe_args(Vendor::Amf, "hevc"));
        assert!(p.contains("-init_hw_device d3d11va") && p.contains("-vf format=bgra,hwupload -frames:v 1 -c:v hevc_amf"), "{p}");
        let p = line(&probe_args(Vendor::Cpu, "h264"));
        assert!(!p.contains("d3d11va") && p.contains("-c:v libx264 -f null -"), "{p}");
    }
}
