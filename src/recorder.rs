//! Keeps ffmpeg recording the screen into the ring, restarting it whenever it stops
//! (desktop capture dies on the lock screen, UAC prompts and display mode changes).

use crate::config::{log_path, Config};
use crate::ring;
use anyhow::{Context, Result};
use std::fs::File;
use std::io::Write;
use std::os::windows::io::AsRawHandle;
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};
use windows_sys::Win32::System::JobObjects::{
    AssignProcessToJobObject, CreateJobObjectW, JobObjectExtendedLimitInformation, SetInformationJobObject,
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE,
};

const RETRY_DELAY: Duration = Duration::from_secs(2);

/// A job object that kills ffmpeg if we die without stopping it, so it can never record unsupervised.
struct KillOnExitJob(windows_sys::Win32::Foundation::HANDLE);

impl KillOnExitJob {
    fn new() -> Self {
        unsafe {
            let job = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            if !job.is_null() {
                let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(
                    job,
                    JobObjectExtendedLimitInformation,
                    &info as *const _ as *const _,
                    std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                );
            }
            Self(job)
        }
    }

    fn adopt(&self, child: &Child) {
        if !self.0.is_null() {
            unsafe { AssignProcessToJobObject(self.0, child.as_raw_handle() as _) };
        }
    }
}

fn spawn(cfg: &Config, ffmpeg: &Path) -> Result<Child> {
    let log = File::create(log_path()).context("creating ffmpeg log")?;
    let bitrate = format!("{}k", (cfg.bitrate_mbps * 1000.0).round() as u64);
    let peak = format!("{}k", (cfg.bitrate_mbps * 2000.0).round() as u64);
    let grab = format!(
        "ddagrab=output_idx={}:framerate={}:draw_mouse={}",
        cfg.output_idx, cfg.fps, cfg.draw_mouse as u8
    );
    let mut cmd = Command::new(ffmpeg);
    cmd.args(["-hide_banner", "-loglevel", "warning", "-nostats"])
        .args(["-init_hw_device", "d3d11va"])
        .args(["-filter_complex", &grab])
        .args(["-c:v", cfg.encoder(), "-preset", "p4", "-rc", "vbr"])
        .args(["-b:v", &bitrate, "-maxrate", &peak, "-bufsize", &peak])
        .args(["-g", &(cfg.fps * cfg.segment_secs).to_string()])
        .args(["-f", "segment", "-segment_time", &cfg.segment_secs.to_string()])
        .args(["-segment_format", "matroska", "-reset_timestamps", "1", "-strftime", "1"])
        .arg(cfg.ring_dir.join(ring::FFMPEG_PATTERN))
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::from(log));
    cmd.spawn().with_context(|| format!("starting {}", ffmpeg.display()))
}

/// Asks ffmpeg to finish the current segment and exit; kills it if it doesn't.
fn stop_gracefully(child: &mut Child) {
    if let Some(mut stdin) = child.stdin.take() {
        let _ = stdin.write_all(b"q");
    }
    let deadline = Instant::now() + Duration::from_secs(5);
    while Instant::now() < deadline {
        if matches!(child.try_wait(), Ok(Some(_))) {
            return;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    let _ = child.kill();
    let _ = child.wait();
}

fn last_log_line() -> String {
    std::fs::read_to_string(log_path())
        .ok()
        .and_then(|t| t.lines().rev().find(|l| !l.trim().is_empty()).map(|l| l.trim().to_string()))
        .unwrap_or_default()
}

fn sleep_unless_stopped(d: Duration, stop: &AtomicBool) {
    let end = Instant::now() + d;
    while Instant::now() < end && !stop.load(Ordering::Relaxed) {
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// Runs until `stop` is set.
pub fn run(cfg: &Config, ffmpeg: &Path, stop: &AtomicBool) {
    let job = KillOnExitJob::new();
    // Only report changes of state, so hours on the lock screen don't flood the console.
    let mut healthy = true;
    while !stop.load(Ordering::Relaxed) {
        let mut child = match spawn(cfg, ffmpeg) {
            Ok(c) => c,
            Err(e) => {
                if healthy {
                    crate::log(&format!("recorder failed to start: {e:#}"));
                    healthy = false;
                }
                sleep_unless_stopped(RETRY_DELAY, stop);
                continue;
            }
        };
        job.adopt(&child);
        let started = Instant::now();
        let mut announced = false;
        let status = loop {
            if stop.load(Ordering::Relaxed) {
                stop_gracefully(&mut child);
                return;
            }
            match child.try_wait() {
                Ok(Some(status)) => break status.to_string(),
                Ok(None) => {}
                Err(e) => break e.to_string(),
            }
            if !announced && started.elapsed() > Duration::from_secs(4) {
                crate::log(if healthy { "recording" } else { "recording again" });
                announced = true;
                healthy = true;
            }
            std::thread::sleep(Duration::from_millis(200));
        };
        if healthy {
            crate::log(&format!(
                "capture stopped ({status}): {} — retrying every {}s (normal while locked)",
                last_log_line(),
                RETRY_DELAY.as_secs()
            ));
            healthy = false;
        }
        sleep_unless_stopped(RETRY_DELAY, stop);
    }
}
