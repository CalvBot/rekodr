//! clipback: a hands-off long replay buffer. The screen goes to a rolling buffer on disk, every
//! app's audio (and the mic) to its own stream, and a hotkey saves the last stretch as an MP4 with
//! one audio track per source.

pub mod apps;
pub mod audio;
pub mod clips;
pub mod config;
pub mod export;
pub mod hotkey;
pub mod recorder;
pub mod remote;
pub mod render;
pub mod ring;
pub mod server;
pub mod startup;
pub mod state;
pub mod tray;
pub mod util;

use anyhow::{Context, Result, bail};
use std::sync::atomic::Ordering;
use std::time::Duration;
use util::log;

/// True if another copy already holds the instance lock (held for the life of this process).
fn already_running() -> bool {
    use windows_sys::Win32::Foundation::{ERROR_ALREADY_EXISTS, GetLastError};
    use windows_sys::Win32::System::Threading::CreateMutexW;
    let name: Vec<u16> = "Local\\clipback-recorder\0".encode_utf16().collect();
    unsafe {
        let handle = CreateMutexW(std::ptr::null(), 0, name.as_ptr());
        handle.is_null() || GetLastError() == ERROR_ALREADY_EXISTS
    }
}

/// Records, serves the UI and shows the tray icon until Quit (or Ctrl+C).
pub fn run_service() -> Result<()> {
    let cfg = config::Config::load()?;
    if already_running() {
        // Already going: just show it.
        server::open_window("/");
        bail!("clipback is already running");
    }
    let ffmpeg = cfg.find_ffmpeg()?;
    std::fs::create_dir_all(cfg.video_ring()).with_context(|| format!("creating {}", cfg.ring_dir.display()))?;
    std::fs::create_dir_all(cfg.audio_ring())?;
    state::init(cfg.clone(), ffmpeg);
    if cfg.start_with_windows {
        let _ = startup::set(true); // keeps the path current if the exe moved
    }
    log(&format!(
        "clipback {} · buffer {} (cap {} GB, about {:.0} h) · clips {}",
        env!("CARGO_PKG_VERSION"),
        cfg.ring_dir.display(),
        cfg.max_ring_gb,
        cfg.max_ring_gb * 8000.0 / cfg.video.bitrate_mbps / 3600.0,
        cfg.clips_dir.display()
    ));

    let recorder = std::thread::Builder::new().name("video".into()).spawn(recorder::run)?;
    let audio = std::thread::Builder::new().name("audio".into()).spawn(audio::run)?;
    std::thread::Builder::new().name("prune".into()).spawn(|| {
        let mut ticks = 0u32;
        while !state::stopping() {
            let cfg = state::cfg();
            if let Err(e) = ring::prune(&cfg.ring_dir, cfg.max_ring_bytes()) {
                log(&format!("pruning failed: {e:#}"));
            }
            state::RING_BYTES.store(ring::size(&cfg.ring_dir), Ordering::Relaxed);
            if ticks % 180 == 0 {
                clips::tidy_cache(); // hourly
            }
            ticks += 1;
            for _ in 0..20 {
                if state::stopping() {
                    break;
                }
                std::thread::sleep(Duration::from_secs(1));
            }
        }
    })?;
    if let Err(e) = server::serve(cfg.port) {
        log(&format!("{e:#} — the clips window won't work"));
    }
    ctrlc::set_handler(tray::quit).context("installing Ctrl+C handler")?;

    let result = tray::run();
    log("stopping");
    state::STOP.store(true, Ordering::Relaxed);
    let _ = recorder.join();
    let _ = audio.join();
    result
}
