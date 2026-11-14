//! Process-wide state shared by the recorder, the audio watcher, the tray and the web UI.

use crate::config::Config;
use serde::Serialize;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Mutex, RwLock};

static CONFIG: RwLock<Option<Config>> = RwLock::new(None);
static FFMPEG: RwLock<Option<PathBuf>> = RwLock::new(None);

/// Set by the tray / UI / hotkey: nothing is recorded while paused.
pub static PAUSED: AtomicBool = AtomicBool::new(false);
/// Set when quitting.
pub static STOP: AtomicBool = AtomicBool::new(false);
/// Bumped when video settings change: the recorder restarts ffmpeg with the new ones.
pub static VIDEO_GENERATION: AtomicU64 = AtomicU64::new(0);
/// Size of the ring on disk, updated by the pruner.
pub static RING_BYTES: AtomicU64 = AtomicU64::new(0);
/// The video recorder has a live ffmpeg producing frames.
pub static VIDEO_LIVE: AtomicBool = AtomicBool::new(false);

pub fn init(cfg: Config, ffmpeg: PathBuf) {
    *CONFIG.write().unwrap() = Some(cfg);
    *FFMPEG.write().unwrap() = Some(ffmpeg);
}

pub fn cfg() -> Config {
    CONFIG.read().unwrap().clone().expect("config loaded")
}

pub fn ffmpeg() -> PathBuf {
    FFMPEG.read().unwrap().clone().expect("ffmpeg found")
}

/// Saves new settings and applies them.
pub fn set_cfg(new: Config) -> anyhow::Result<()> {
    new.save()?;
    let old = cfg();
    let video_changed = old.video.output_idx != new.video.output_idx
        || old.video.fps != new.video.fps
        || old.video.codec != new.video.codec
        || old.video.bitrate_mbps != new.video.bitrate_mbps
        || old.video.draw_mouse != new.video.draw_mouse
        || old.ring_dir != new.ring_dir;
    let startup_changed = old.start_with_windows != new.start_with_windows;
    *CONFIG.write().unwrap() = Some(new.clone());
    if video_changed {
        VIDEO_GENERATION.fetch_add(1, Ordering::Relaxed);
    }
    if startup_changed {
        crate::startup::set(new.start_with_windows)?;
    }
    crate::tray::refresh();
    Ok(())
}

pub fn paused() -> bool {
    PAUSED.load(Ordering::Relaxed)
}

pub fn set_paused(paused: bool) {
    if PAUSED.swap(paused, Ordering::Relaxed) != paused {
        crate::util::log(if paused { "paused" } else { "resumed" });
        crate::tray::refresh();
    }
}

pub fn stopping() -> bool {
    STOP.load(Ordering::Relaxed)
}

/// An audio stream being recorded right now, for the UI.
#[derive(Clone, Serialize)]
pub struct LiveAudio {
    pub exe: String,
    pub track: String,
    pub since_us: i64,
}

pub static LIVE_AUDIO: Mutex<Vec<LiveAudio>> = Mutex::new(Vec::new());
