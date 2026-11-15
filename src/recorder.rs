//! Keeps ffmpeg recording the screen into the video ring, restarting it whenever it stops
//! (desktop capture dies on the lock screen, UAC prompts and display mode changes), when
//! recording is paused/resumed, when the video settings change, or when a game with its own
//! bitrate starts or quits.

use crate::config::{Config, Encode, SEGMENT_SECS};
use crate::encoders::{self, Vendor};
use crate::state;
use crate::util::{self, log};
use anyhow::{Context, Result};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Stdio};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

const RETRY_DELAY: Duration = Duration::from_secs(2);
/// How often to check whether the game being played (and so the bitrate) changed.
const GAME_CHECK: Duration = Duration::from_secs(3);

fn ffmpeg_log() -> PathBuf {
    crate::config::local_dir().join("ffmpeg-video.log")
}

/// Everything before the output files: ddagrab → the encoder → 2 s segments. Frame 0's
/// wall-clock time is printed once (the `print` in setpts) and saved as the run's anchor, which
/// lines the video up with the audio streams.
pub fn args(cfg: &Config, enc: &Encode, vendor: Vendor) -> Vec<String> {
    let v = &cfg.video;
    let graph = format!(
        "ddagrab=output_idx={}:framerate={}:draw_mouse={},setpts='PTS+0*if(eq(N\\,0)\\,print(RTCTIME)\\,0)'{}",
        v.output_idx,
        v.fps,
        v.draw_mouse as u8,
        encoders::capture_filter(vendor)
    );
    let s = |a: &[&str]| a.iter().map(|x| x.to_string()).collect::<Vec<_>>();
    let mut a = s(&["-hide_banner", "-nostats", "-loglevel", "info", "-init_hw_device", "d3d11va", "-filter_complex"]);
    a.push(graph);
    a.extend(encoders::record_args(vendor, &v.codec, enc));
    // No B-frames and a keyframe every segment: easy to cut, quick to scrub in an editor.
    a.extend(s(&["-bf", "0", "-g", &(v.fps * SEGMENT_SECS).to_string()]));
    a.extend(s(&["-f", "segment", "-segment_time", &SEGMENT_SECS.to_string(), "-segment_format", "matroska"]));
    a.extend(s(&["-reset_timestamps", "0", "-segment_list_type", "csv", "-segment_list"]));
    a
}

fn spawn(cfg: &Config, enc: &Encode, vendor: Vendor, ffmpeg: &Path, run: &Path) -> Result<Child> {
    let mut cmd = util::command(ffmpeg);
    cmd.args(args(cfg, enc, vendor))
        .arg(run.join("list.csv"))
        .arg(run.join("v_%05d.mkv"))
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::piped());
    let child = cmd.spawn().with_context(|| format!("starting {}", ffmpeg.display()))?;
    util::job().adopt(&child);
    Ok(child)
}

/// Reads ffmpeg's messages: saves the anchor, logs the rest (minus the per-segment chatter).
fn watch_stderr(child: &mut Child, run: PathBuf) -> std::thread::JoinHandle<String> {
    let stderr = child.stderr.take().expect("piped");
    std::thread::spawn(move || {
        let mut log = std::fs::OpenOptions::new().create(true).append(true).open(ffmpeg_log()).ok();
        let mut last = String::new();
        for line in BufReader::new(stderr).lines().map_while(Result::ok) {
            if line.starts_with("[Eval @") {
                if let Some(us) = line.rsplit(' ').next().and_then(|v| v.trim().parse::<f64>().ok()) {
                    let _ = std::fs::write(run.join("anchor.txt"), format!("{}", us.round() as i64));
                }
                continue;
            }
            if line.contains("Opening '") || line.trim().is_empty() || line.starts_with("  ") {
                continue;
            }
            if let Some(f) = log.as_mut() {
                let _ = writeln!(f, "{}", line);
            }
            last = line;
        }
        last
    })
}

/// Asks ffmpeg to finish the current segment and exit; kills it if it doesn't.
fn stop_gracefully(child: &mut Child) {
    if let Some(mut stdin) = child.stdin.take() {
        let _ = stdin.write_all(b"q");
    }
    util::wait_or_kill(child, 5);
}

fn sleep_while(d: Duration, keep_waiting: impl Fn() -> bool) {
    let end = Instant::now() + d;
    while Instant::now() < end && keep_waiting() {
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// The encoder settings for the game being played now.
fn encode_now(cfg: &Config) -> Encode {
    cfg.video.encode(crate::activity::current().as_deref())
}

/// Everything that needs a new ffmpeg when it changes.
fn same_capture(a: &Config, b: &Config) -> bool {
    let (x, y) = (&a.video, &b.video);
    x.output_idx == y.output_idx
        && x.fps == y.fps
        && x.codec == y.codec
        && x.encoder == y.encoder
        && x.draw_mouse == y.draw_mouse
        && a.buffer_dir == b.buffer_dir
}

/// Runs until clipback quits.
pub fn run() {
    let _ = std::fs::create_dir_all(crate::config::local_dir());
    let _ = std::fs::write(ffmpeg_log(), "");
    // Only changes of state are logged, so hours on the lock screen don't flood the log.
    let mut healthy = true;
    let mut last_encoder = "";
    while !state::stopping() {
        if state::paused() {
            sleep_while(Duration::from_secs(1), || state::paused() && !state::stopping());
            continue;
        }
        let cfg = state::cfg();
        let mut generation = state::VIDEO_GENERATION.load(Ordering::Relaxed);
        let enc = encode_now(&cfg);
        let Some(vendor) = encoders::pick(&cfg.video) else {
            if healthy {
                log(&format!("no working {} encoder found (see the encoders line above)", cfg.video.codec));
                healthy = false;
            }
            sleep_while(RETRY_DELAY, || !state::stopping());
            continue;
        };
        let name = vendor.encoder(&cfg.video.codec);
        if name != last_encoder {
            log(&match vendor {
                Vendor::Cpu => format!("encoding with {name} on the CPU (no GPU encoder): this costs game frames"),
                _ => format!("encoding with {name}"),
            });
            last_encoder = name;
        }
        let run = cfg.video_ring().join(chrono::Local::now().format("%Y%m%d_%H%M%S_%3f").to_string());
        let mut child = match std::fs::create_dir_all(&run)
            .map_err(anyhow::Error::from)
            .and_then(|_| spawn(&cfg, &enc, vendor, &state::ffmpeg(), &run))
        {
            Ok(c) => c,
            Err(e) => {
                if healthy {
                    log(&format!("video recorder failed to start: {e:#}"));
                    healthy = false;
                }
                sleep_while(RETRY_DELAY, || !state::stopping());
                continue;
            }
        };
        let stderr = watch_stderr(&mut child, run.clone());
        *state::VIDEO_ENCODE.lock().unwrap() = Some((enc.bitrate_mbps, enc.game.clone()));
        *state::VIDEO_ENCODER.lock().unwrap() = Some(name);
        let started = Instant::now();
        let mut checked = Instant::now();
        let mut announced = false;
        let exit = loop {
            let mut restart = false;
            let now_gen = state::VIDEO_GENERATION.load(Ordering::Relaxed);
            if now_gen != generation || checked.elapsed() > GAME_CHECK {
                generation = now_gen;
                checked = Instant::now();
                let new = state::cfg();
                let new_enc = encode_now(&new);
                // A detection retry can find a GPU whose driver wasn't ready at start.
                restart = !same_capture(&cfg, &new) || new_enc != enc || encoders::pick(&new.video) != Some(vendor);
                if restart && new_enc.game != enc.game {
                    log(&match &new_enc.game {
                        Some(g) => format!("{} bitrate: {} Mbps", crate::apps::display_name(g), new_enc.bitrate_mbps),
                        None => format!("back to {} Mbps", new_enc.bitrate_mbps),
                    });
                }
            }
            if state::stopping() || state::paused() || restart {
                stop_gracefully(&mut child);
                break None;
            }
            match child.try_wait() {
                Ok(Some(status)) => break Some(status.to_string()),
                Ok(None) => {}
                Err(e) => break Some(e.to_string()),
            }
            if !announced && started.elapsed() > Duration::from_secs(4) {
                log(if healthy { "recording video" } else { "recording video again" });
                announced = true;
                healthy = true;
                state::VIDEO_LIVE.store(true, Ordering::Relaxed);
            }
            std::thread::sleep(Duration::from_millis(200));
        };
        state::VIDEO_LIVE.store(false, Ordering::Relaxed);
        *state::VIDEO_ENCODE.lock().unwrap() = None;
        *state::VIDEO_ENCODER.lock().unwrap() = None;
        let last = stderr.join().unwrap_or_default();
        if let Some(status) = exit {
            if healthy {
                log(&format!(
                    "video capture stopped ({status}): {last} — retrying every {}s (normal while locked)",
                    RETRY_DELAY.as_secs()
                ));
                healthy = false;
            }
            sleep_while(RETRY_DELAY, || !state::stopping());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The whole NVENC command as it was before other vendors, so it can't drift.
    #[test]
    fn nvenc_command_unchanged() {
        let cfg = Config::default();
        let a = args(&cfg, &cfg.video.encode(None), Vendor::Nvenc).join(" ");
        assert_eq!(
            a,
            "-hide_banner -nostats -loglevel info -init_hw_device d3d11va -filter_complex \
             ddagrab=output_idx=0:framerate=60:draw_mouse=1,setpts='PTS+0*if(eq(N\\,0)\\,print(RTCTIME)\\,0)' \
             -c:v h264_nvenc -preset p4 -rc vbr -b:v 20000k -maxrate 40000k -bufsize 40000k \
             -bf 0 -g 120 -f segment -segment_time 2 -segment_format matroska \
             -reset_timestamps 0 -segment_list_type csv -segment_list"
        );
    }

    #[test]
    fn cpu_downloads_frames() {
        let mut cfg = Config::default();
        cfg.video.codec = "av1".into();
        let a = args(&cfg, &cfg.video.encode(None), Vendor::Cpu).join(" ");
        assert!(a.contains("print(RTCTIME)\\,0)',hwdownload,format=bgra,format=yuv420p -c:v libsvtav1 -preset 10"), "{a}");
        assert!(a.contains("-bf 0 -g 120 -f segment -segment_time 2"), "{a}");
    }
}
