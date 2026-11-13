//! Settings, read from %APPDATA%\clipback\config.toml (created with defaults on first run).

use anyhow::{bail, Context, Result};
use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

#[derive(Deserialize, Clone, Debug)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    /// Folder holding the rolling buffer of segments. Everything named seg_*.mkv in here is ours to delete.
    pub ring_dir: PathBuf,
    /// Where saved clips go.
    pub save_dir: PathBuf,
    /// Buffer cap; the oldest segments are deleted beyond this.
    pub max_ring_gb: f64,
    /// Which monitor to record (0 = first output of the default GPU, normally the main screen).
    pub output_idx: u32,
    pub fps: u32,
    /// av1 | hevc | h264 (all NVENC).
    pub codec: String,
    pub bitrate_mbps: f64,
    /// Segment and keyframe interval. A saved clip starts on a segment boundary.
    pub segment_secs: u32,
    pub draw_mouse: bool,
    /// Path to ffmpeg.exe; empty = find it automatically.
    pub ffmpeg: String,
    pub hotkey: Vec<Hotkey>,
}

#[derive(Deserialize, Clone, Debug)]
#[serde(deny_unknown_fields)]
pub struct Hotkey {
    /// e.g. "Ctrl+Alt+F9"
    pub keys: String,
    /// How much to save, e.g. "30s", "2m", "1h".
    pub save: String,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            ring_dir: PathBuf::from(r"G:\clipback\ring"),
            save_dir: default_save_dir(),
            max_ring_gb: 200.0,
            output_idx: 0,
            fps: 60,
            codec: "av1".into(),
            bitrate_mbps: 12.0,
            segment_secs: 2,
            draw_mouse: true,
            ffmpeg: String::new(),
            hotkey: vec![
                Hotkey { keys: "Ctrl+Alt+F9".into(), save: "30s".into() },
                Hotkey { keys: "Ctrl+Alt+F10".into(), save: "2m".into() },
                Hotkey { keys: "Ctrl+Alt+F11".into(), save: "10m".into() },
            ],
        }
    }
}

fn default_save_dir() -> PathBuf {
    let home = std::env::var_os("USERPROFILE").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(r"C:\"));
    home.join("Videos").join("clipback")
}

pub fn config_path() -> PathBuf {
    let base = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    base.join("clipback").join("config.toml")
}

/// Where the recorder's ffmpeg output is logged.
pub fn log_path() -> PathBuf {
    config_path().with_file_name("ffmpeg.log")
}

fn default_file_text() -> String {
    let d = Config::default();
    let mut s = format!(
        "# clipback settings. Restart `clipback run` after editing.\n\n\
         # Rolling buffer folder. Everything named seg_*.mkv in here gets deleted as the buffer wraps.\n\
         ring_dir = '{ring}'\n\
         # Where saved clips go.\n\
         save_dir = '{save}'\n\
         # Buffer size cap in GB (12 Mbps is about 5.4 GB per hour).\n\
         max_ring_gb = {cap}\n\n\
         # Monitor to record: 0 is normally the main screen.\n\
         output_idx = {idx}\n\
         fps = {fps}\n\
         # av1, hevc or h264 (NVENC)\n\
         codec = \"{codec}\"\n\
         bitrate_mbps = {br}\n\
         # Segment / keyframe length in seconds. Clips start on a segment boundary.\n\
         segment_secs = {seg}\n\
         draw_mouse = {mouse}\n\
         # Path to ffmpeg.exe; leave empty to find it automatically.\n\
         ffmpeg = \"\"\n",
        ring = d.ring_dir.display(),
        save = d.save_dir.display(),
        cap = d.max_ring_gb,
        idx = d.output_idx,
        fps = d.fps,
        codec = d.codec,
        br = d.bitrate_mbps,
        seg = d.segment_secs,
        mouse = d.draw_mouse,
    );
    s.push_str("\n# Global hotkeys: save the last N seconds/minutes/hours.\n");
    for h in &d.hotkey {
        s.push_str(&format!("[[hotkey]]\nkeys = \"{}\"\nsave = \"{}\"\n\n", h.keys, h.save));
    }
    s
}

impl Config {
    /// Loads the config, writing the default file first if there isn't one.
    pub fn load() -> Result<Self> {
        let path = config_path();
        if !path.exists() {
            if let Some(dir) = path.parent() {
                std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
            }
            std::fs::write(&path, default_file_text()).with_context(|| format!("writing {}", path.display()))?;
            println!("Wrote default settings to {}", path.display());
        }
        let text = std::fs::read_to_string(&path).with_context(|| format!("reading {}", path.display()))?;
        let cfg: Config = toml::from_str(&text).with_context(|| format!("parsing {}", path.display()))?;
        cfg.validate()?;
        Ok(cfg)
    }

    fn validate(&self) -> Result<()> {
        if !matches!(self.codec.as_str(), "av1" | "hevc" | "h264") {
            bail!("codec must be av1, hevc or h264 (got {:?})", self.codec);
        }
        if self.fps == 0 || self.segment_secs == 0 || self.bitrate_mbps <= 0.0 {
            bail!("fps, segment_secs and bitrate_mbps must be above zero");
        }
        if self.max_ring_gb < 1.0 {
            bail!("max_ring_gb must be at least 1");
        }
        for h in &self.hotkey {
            parse_duration(&h.save).with_context(|| format!("hotkey {:?}", h.keys))?;
        }
        Ok(())
    }

    pub fn encoder(&self) -> &'static str {
        match self.codec.as_str() {
            "hevc" => "hevc_nvenc",
            "h264" => "h264_nvenc",
            _ => "av1_nvenc",
        }
    }

    pub fn max_ring_bytes(&self) -> u64 {
        (self.max_ring_gb * 1e9) as u64
    }

    /// Finds ffmpeg: the configured path, then PATH, then winget's links folder.
    pub fn find_ffmpeg(&self) -> Result<PathBuf> {
        if !self.ffmpeg.is_empty() {
            let p = PathBuf::from(&self.ffmpeg);
            if !p.is_file() {
                bail!("ffmpeg not found at configured path {}", p.display());
            }
            return Ok(p);
        }
        if runs(Path::new("ffmpeg")) {
            return Ok(PathBuf::from("ffmpeg"));
        }
        if let Some(local) = std::env::var_os("LOCALAPPDATA") {
            let p = PathBuf::from(local).join(r"Microsoft\WinGet\Links\ffmpeg.exe");
            if runs(&p) {
                return Ok(p);
            }
        }
        bail!("ffmpeg not found. Install it (winget install Gyan.FFmpeg) or set `ffmpeg` in {}", config_path().display())
    }
}

fn runs(exe: &Path) -> bool {
    Command::new(exe)
        .arg("-version")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

/// "90" / "30s" / "2m" / "1h" → seconds.
pub fn parse_duration(s: &str) -> Result<u64> {
    let t = s.trim().to_ascii_lowercase();
    let (num, mult) = match t.chars().last() {
        Some('s') => (&t[..t.len() - 1], 1.0),
        Some('m') => (&t[..t.len() - 1], 60.0),
        Some('h') => (&t[..t.len() - 1], 3600.0),
        _ => (t.as_str(), 1.0),
    };
    let n: f64 = num.trim().parse().with_context(|| format!("bad duration {s:?} (use e.g. 30s, 2m, 1h)"))?;
    if !(n > 0.0) || !n.is_finite() {
        bail!("duration must be above zero (got {s:?})");
    }
    Ok((n * mult).round().max(1.0) as u64)
}

/// 150 → "2m30s"; used in file names and messages.
pub fn format_duration(secs: u64) -> String {
    let (h, m, s) = (secs / 3600, secs % 3600 / 60, secs % 60);
    let mut out = String::new();
    if h > 0 {
        out.push_str(&format!("{h}h"));
    }
    if m > 0 {
        out.push_str(&format!("{m}m"));
    }
    if s > 0 || out.is_empty() {
        out.push_str(&format!("{s}s"));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn durations() {
        assert_eq!(parse_duration("90").unwrap(), 90);
        assert_eq!(parse_duration("30s").unwrap(), 30);
        assert_eq!(parse_duration("2m").unwrap(), 120);
        assert_eq!(parse_duration("1.5H").unwrap(), 5400);
        assert!(parse_duration("0").is_err());
        assert!(parse_duration("abc").is_err());
        assert_eq!(format_duration(150), "2m30s");
        assert_eq!(format_duration(3600), "1h");
        assert_eq!(format_duration(30), "30s");
    }

    #[test]
    fn default_file_parses() {
        let cfg: Config = toml::from_str(&default_file_text()).unwrap();
        cfg.validate().unwrap();
        assert_eq!(cfg.hotkey.len(), 3);
        assert_eq!(cfg.ring_dir, Config::default().ring_dir);
    }
}
