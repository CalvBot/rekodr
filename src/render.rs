//! The editor's export: trim, keyframed zoom, per-track volume, and a size preset. Runs ffmpeg in
//! the background; the UI polls the job for progress.

use crate::{clips, state, util};
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::sync::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};

#[derive(Debug, Clone, Deserialize)]
pub struct Key {
    /// Seconds into the source clip.
    pub t: f64,
    /// 1 = no zoom.
    pub z: f64,
    /// Centre of the view, 0–1 across and down.
    pub x: f64,
    pub y: f64,
}

#[derive(Debug, Clone, Deserialize)]
pub struct TrackMix {
    pub index: usize,
    /// 0–2.
    pub volume: f64,
    pub muted: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Request {
    pub id: String,
    pub start: f64,
    pub end: f64,
    /// "original", "discord10" or "discord50".
    pub preset: String,
    #[serde(default)]
    pub zoom: Vec<Key>,
    #[serde(default)]
    pub tracks: Vec<TrackMix>,
    #[serde(default)]
    pub copy: bool,
}

#[derive(Debug, Clone, Default, Serialize)]
pub struct Job {
    pub progress: f64,
    pub done: bool,
    pub error: Option<String>,
    pub id: Option<String>,
    pub path: Option<String>,
    pub copied: bool,
}

static JOBS: Mutex<Option<HashMap<u64, Job>>> = Mutex::new(None);
static NEXT: AtomicU64 = AtomicU64::new(1);

fn update(job: u64, f: impl FnOnce(&mut Job)) {
    let mut jobs = JOBS.lock().unwrap();
    f(jobs.get_or_insert_with(HashMap::new).entry(job).or_default());
}

pub fn job(id: u64) -> Option<Job> {
    JOBS.lock().unwrap().as_ref()?.get(&id).cloned()
}

pub fn start(req: Request) -> Result<u64> {
    let src = clips::resolve(&req.id)?;
    let info = clips::info(&src)?;
    if !(req.end > req.start) {
        bail!("the end must be after the start");
    }
    let id = NEXT.fetch_add(1, Ordering::Relaxed);
    update(id, |_| {});
    std::thread::spawn(move || {
        let result = run(id, &req, &src, &info);
        update(id, |j| {
            j.done = true;
            match result {
                Ok((path, copied)) => {
                    j.progress = 1.0;
                    j.id = clips::id_of(&path);
                    j.path = Some(path.display().to_string());
                    j.copied = copied;
                }
                Err(e) => {
                    util::log(&format!("export failed: {e:#}"));
                    j.error = Some(format!("{e:#}"));
                }
            }
        });
    });
    Ok(id)
}

fn smooth_expr(keys: &[(f64, f64)], t: &str) -> String {
    let Some(last) = keys.last() else { return "0".into() };
    let mut e = format!("{:.5}", last.1);
    for w in keys.windows(2).rev() {
        let ((ta, a), (tb, b)) = (w[0], w[1]);
        let u = format!("clip(({t}-{ta:.4})/{:.4},0,1)", (tb - ta).max(1e-3));
        let seg = format!("({a:.5}+({:.5})*{u}*{u}*(3-2*{u}))", b - a);
        e = format!("if(lt({t},{tb:.4}),{seg},{e})");
    }
    e
}

/// The zoom as a `perspective` filter: the view rectangle's corners, per frame, smoothly between
/// keyframes. None when there's no zoom at all.
pub fn zoom_filter(keys: &[Key], fps: f64, start: f64) -> Option<String> {
    if keys.iter().all(|k| (k.z - 1.0).abs() < 1e-3) {
        return None;
    }
    let mut keys = keys.to_vec();
    keys.sort_by(|a, b| a.t.total_cmp(&b.t));
    let t = format!("(in/{fps:.5}+{start:.4})");
    let pick = |f: fn(&Key) -> f64| smooth_expr(&keys.iter().map(|k| (k.t, f(k))).collect::<Vec<_>>(), &t);
    let z = format!("max(1,{})", pick(|k| k.z));
    let (cx, cy) = (pick(|k| k.x), pick(|k| k.y));
    let x = format!("clip({cx}*W-W/(2*{z}),0,W-W/{z})");
    let y = format!("clip({cy}*H-H/(2*{z}),0,H-H/{z})");
    let (x1, y1) = (format!("{x}+W/{z}"), format!("{y}+H/{z}"));
    Some(format!(
        "perspective=x0='{x}':y0='{y}':x1='{x1}':y1='{y}':x2='{x}':y2='{y1}':x3='{x1}':y3='{y1}':interpolation=linear:eval=frame"
    ))
}

/// The audio graph: the original Mix if nothing changed, otherwise the chosen tracks re-mixed.
fn audio_filter(req: &Request, titles: &[String]) -> Option<String> {
    let clipback_layout = titles.len() > 1 && titles[0] == "Mix";
    let first_own = if clipback_layout { 1 } else { 0 };
    let setting = |i: usize| req.tracks.iter().find(|t| t.index == i);
    let untouched = (first_own..titles.len()).all(|i| setting(i).is_none_or(|t| !t.muted && (t.volume - 1.0).abs() < 1e-3));
    if clipback_layout && untouched {
        return Some("[0:a:0]anull[a]".into());
    }
    let chosen: Vec<(usize, f64)> = (first_own..titles.len())
        .filter_map(|i| match setting(i) {
            Some(t) if t.muted || t.volume <= 0.0 => None,
            Some(t) => Some((i, t.volume)),
            None => Some((i, 1.0)),
        })
        .collect();
    match chosen.len() {
        0 => None,
        1 => Some(format!("[0:a:{}]volume={:.3}[a]", chosen[0].0, chosen[0].1)),
        n => {
            let mut g = String::new();
            for (i, v) in &chosen {
                g.push_str(&format!("[0:a:{i}]volume={v:.3}[v{i}];"));
            }
            let labels: String = chosen.iter().map(|(i, _)| format!("[v{i}]")).collect();
            g.push_str(&format!("{labels}amix=inputs={n}:normalize=0:dropout_transition=0,alimiter=limit=0.98:level=0[a]"));
            Some(g)
        }
    }
}

fn unique(path: PathBuf) -> PathBuf {
    if !path.exists() {
        return path;
    }
    let stem = path.file_stem().unwrap_or_default().to_string_lossy().to_string();
    (2..).map(|n| path.with_file_name(format!("{stem} {n}.mp4"))).find(|p| !p.exists()).unwrap()
}

fn run(job: u64, req: &Request, src: &Path, info: &clips::Info) -> Result<(PathBuf, bool)> {
    let start = req.start.max(0.0);
    let end = req.end.min(info.duration.max(req.end));
    let dur = end - start;
    let audio = audio_filter(req, &info.tracks);

    // Size presets: the bitrate that fits, then a resolution and frame rate that suit it.
    let target_mb = match req.preset.as_str() {
        "discord10" => Some(10.0),
        "discord50" => Some(50.0),
        _ => None,
    };
    let audio_kbps = if target_mb == Some(10.0) { 96 } else { 160 };
    let video_kbps = target_mb.map(|mb| ((mb * 8000.0 * 0.92) / dur - audio_kbps as f64).max(150.0) as u64);
    let mut vf = Vec::new();
    if let Some(z) = zoom_filter(&req.zoom, info.fps.max(1.0), start) {
        vf.push(z);
    }
    if let Some(kbps) = video_kbps {
        let height = if kbps < 1200 { 480 } else if kbps < 3500 { 720 } else { 1080 };
        vf.push(format!("scale=-2:'min({height},ih)':flags=lanczos"));
        if kbps < 3000 && info.fps > 31.0 {
            vf.push("fps=30".into());
        }
    }
    vf.push("format=yuv420p".into());
    let mut graph = format!("[0:v]{}[v]", vf.join(","));
    if let Some(a) = &audio {
        graph.push_str(";\n");
        graph.push_str(a);
    }

    let stem = src.file_stem().unwrap_or_default().to_string_lossy().to_string();
    let suffix = match req.preset.as_str() {
        "discord10" => " (10MB)",
        "discord50" => " (50MB)",
        _ => " edit",
    };
    let out = unique(src.with_file_name(format!("{stem}{suffix}.mp4")));
    let tmp = std::env::temp_dir().join(format!("clipback-render-{}-{job}.txt", std::process::id()));
    std::fs::write(&tmp, &graph)?;

    let mut cmd = util::command(state::ffmpeg());
    cmd.args(["-hide_banner", "-loglevel", "error", "-nostats", "-progress", "pipe:1", "-y"]);
    cmd.args(["-ss", &format!("{start:.3}"), "-i"]).arg(src);
    cmd.args(["-t", &format!("{dur:.3}")]);
    cmd.arg("-/filter_complex").arg(&tmp);
    cmd.args(["-map", "[v]", "-c:v", "h264_nvenc", "-preset", "p5"]);
    match video_kbps {
        Some(k) => cmd.args(["-rc", "vbr", "-multipass", "fullres", "-b:v", &format!("{k}k"), "-maxrate", &format!("{}k", k * 3 / 2), "-bufsize", &format!("{}k", k * 2)]),
        None => cmd.args(["-rc", "vbr", "-cq", "19", "-b:v", "0"]),
    };
    if audio.is_some() {
        cmd.args(["-map", "[a]", "-c:a", "aac", "-b:a", &format!("{audio_kbps}k"), "-ar", "48000"]);
    }
    // The source's tags (game, title) carry over by default.
    cmd.args(["-movflags", "+faststart"]).arg(&out);
    cmd.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = cmd.spawn().context("starting ffmpeg")?;
    util::job().adopt(&child);
    let stderr = child.stderr.take().unwrap();
    let errors = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = std::io::Read::read_to_string(&mut BufReader::new(stderr), &mut s);
        s
    });
    for line in BufReader::new(child.stdout.take().unwrap()).lines().map_while(Result::ok) {
        if let Some(us) = line.strip_prefix("out_time_us=").and_then(|v| v.parse::<f64>().ok()) {
            update(job, |j| j.progress = (us / 1e6 / dur).clamp(0.0, 0.99));
        }
    }
    let status = child.wait()?;
    let _ = std::fs::remove_file(&tmp);
    if !status.success() {
        let _ = std::fs::remove_file(&out);
        bail!("ffmpeg: {}", errors.join().unwrap_or_default().trim());
    }
    let copied = req.copy && lifelog::clip::copy_file_to_clipboard(&out).is_ok();
    util::log(&format!("exported {}", out.display()));
    clips::warm(out.clone());
    Ok((out, copied))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn no_zoom_means_no_filter() {
        assert!(zoom_filter(&[Key { t: 0.0, z: 1.0, x: 0.5, y: 0.5 }], 60.0, 0.0).is_none());
        assert!(zoom_filter(&[], 60.0, 0.0).is_none());
        let f = zoom_filter(&[Key { t: 1.0, z: 1.0, x: 0.5, y: 0.5 }, Key { t: 2.0, z: 2.0, x: 0.3, y: 0.7 }], 60.0, 0.5).unwrap();
        assert!(f.starts_with("perspective=x0='"), "{f}");
        assert!(f.contains("(in/60.00000+0.5000)"), "{f}");
    }

    #[test]
    fn smooth_steps_between_keys() {
        let e = smooth_expr(&[(0.0, 1.0), (2.0, 3.0)], "T");
        assert_eq!(e, "if(lt(T,2.0000),(1.00000+(2.00000)*clip((T-0.0000)/2.0000,0,1)*clip((T-0.0000)/2.0000,0,1)*(3-2*clip((T-0.0000)/2.0000,0,1))),3.00000)");
    }

    #[test]
    fn audio_uses_mix_unless_changed() {
        let titles: Vec<String> = ["Mix", "Mic", "Communication", "Game"].map(String::from).to_vec();
        let mut req = Request { id: String::new(), start: 0.0, end: 1.0, preset: "original".into(), zoom: vec![], tracks: vec![], copy: false };
        assert_eq!(audio_filter(&req, &titles).unwrap(), "[0:a:0]anull[a]");
        req.tracks = vec![TrackMix { index: 2, volume: 1.0, muted: true }];
        let g = audio_filter(&req, &titles).unwrap();
        assert!(g.contains("[v1][v3]amix=inputs=2"), "{g}");
        assert!(!g.contains("0:a:2") && !g.contains("0:a:0"), "{g}");
    }
}
