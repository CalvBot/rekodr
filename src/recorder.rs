//! Keeps ffmpeg recording the screen into the video ring, restarting it whenever it stops
//! (desktop capture dies on the lock screen, UAC prompts and display mode changes), when
//! recording is paused/resumed, or when the video settings change.

use crate::config::{Config, SEGMENT_SECS};
use crate::state;
use crate::util::{self, log};
use anyhow::{Context, Result};
use std::io::{BufRead, BufReader, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Stdio};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

const RETRY_DELAY: Duration = Duration::from_secs(2);

fn ffmpeg_log() -> PathBuf {
    crate::config::local_dir().join("ffmpeg-video.log")
}

/// ddagrab → NVENC → 2 s segments. Frame 0's wall-clock time is printed once (the `print` in
/// setpts) and saved as the run's anchor, which lines the video up with the audio streams.
fn spawn(cfg: &Config, ffmpeg: &Path, run: &Path) -> Result<Child> {
    let v = &cfg.video;
    let bitrate = format!("{}k", (v.bitrate_mbps * 1000.0).round() as u64);
    let peak = format!("{}k", (v.bitrate_mbps * 2000.0).round() as u64);
    let graph = format!(
        "ddagrab=output_idx={}:framerate={}:draw_mouse={},setpts='PTS+0*if(eq(N\\,0)\\,print(RTCTIME)\\,0)'",
        v.output_idx, v.fps, v.draw_mouse as u8
    );
    let mut cmd = util::command(ffmpeg);
    cmd.args(["-hide_banner", "-nostats", "-loglevel", "info"])
        .args(["-init_hw_device", "d3d11va"])
        .args(["-filter_complex", &graph])
        .args(["-c:v", cfg.encoder(), "-preset", "p4", "-rc", "vbr"])
        .args(["-b:v", &bitrate, "-maxrate", &peak, "-bufsize", &peak])
        // No B-frames and a keyframe every segment: easy to cut, quick to scrub in an editor.
        .args(["-bf", "0", "-g", &(v.fps * SEGMENT_SECS).to_string()])
        .args(["-f", "segment", "-segment_time", &SEGMENT_SECS.to_string(), "-segment_format", "matroska"])
        .args(["-reset_timestamps", "0", "-segment_list_type", "csv", "-segment_list"])
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

/// Runs until clipback quits.
pub fn run() {
    let _ = std::fs::create_dir_all(crate::config::local_dir());
    let _ = std::fs::write(ffmpeg_log(), "");
    // Only changes of state are logged, so hours on the lock screen don't flood the log.
    let mut healthy = true;
    while !state::stopping() {
        if state::paused() {
            sleep_while(Duration::from_secs(1), || state::paused() && !state::stopping());
            continue;
        }
        let cfg = state::cfg();
        let generation = state::VIDEO_GENERATION.load(Ordering::Relaxed);
        let run = cfg.video_ring().join(chrono::Local::now().format("%Y%m%d_%H%M%S_%3f").to_string());
        let mut child = match std::fs::create_dir_all(&run).map_err(anyhow::Error::from).and_then(|_| spawn(&cfg, &state::ffmpeg(), &run)) {
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
        let started = Instant::now();
        let mut announced = false;
        let exit = loop {
            let restart = state::VIDEO_GENERATION.load(Ordering::Relaxed) != generation;
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
