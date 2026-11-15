//! Settings in %APPDATA%\clipback\config.toml (created with defaults on first run, rewritten by the
//! Options page). The live copy is in `state`, so changes apply without a restart.

use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Stdio;

#[derive(Deserialize, Serialize, Clone, Debug)]
#[serde(default)]
pub struct Config {
    /// Rolling buffer (video and audio). Everything in here is clipback's and gets deleted as it wraps.
    #[serde(alias = "ring_dir")]
    pub buffer_dir: PathBuf,
    /// Clips are saved to `<clips_dir>\Clips <year>`.
    pub clips_dir: PathBuf,
    /// Buffer cap in GB; the oldest footage is deleted beyond it.
    #[serde(alias = "max_ring_gb")]
    pub max_buffer_gb: f64,
    /// Port of the local web UI (127.0.0.1 only).
    pub port: u16,
    /// Path to ffmpeg.exe; empty = find it.
    pub ffmpeg: String,
    pub start_with_windows: bool,
    pub video: Video,
    pub audio: Audio,
    pub hotkeys: Vec<Hotkey>,
}

#[derive(Deserialize, Serialize, Clone, Debug, PartialEq)]
#[serde(default)]
pub struct Video {
    /// Monitor: DXGI output index on the main GPU (0 is normally the main screen).
    pub output_idx: u32,
    pub fps: u32,
    /// h264 (edits anywhere, e.g. Premiere), hevc or av1. All NVENC.
    pub codec: String,
    pub bitrate_mbps: f64,
    pub draw_mouse: bool,
    /// Bitrate while this game is being played (lower-case exe → Mbps).
    pub game_bitrates: BTreeMap<String, f64>,
    /// The settings below only apply when this is on; otherwise the defaults are used.
    pub advanced: bool,
    /// NVENC preset p1 (fastest) … p7 (best quality).
    pub preset: String,
    /// "vbr", "cbr" or "cq" (constant quality: the bitrate becomes a cap).
    pub rate_control: String,
    /// Constant-quality level, 0–51 (lower is better).
    pub cq: u32,
    /// Peak bitrate as a multiple of the target (VBR and CQ).
    pub peak_factor: f64,
    /// Adaptive quantisation: spends bits on flat areas (sky, dark scenes) that would band.
    pub spatial_aq: bool,
}

/// The encoder settings actually used, after "advanced" and the per-game bitrate are applied.
#[derive(Clone, Debug, PartialEq)]
pub struct Encode {
    pub preset: String,
    pub rate_control: String,
    pub cq: u32,
    pub bitrate_mbps: f64,
    pub peak_factor: f64,
    pub spatial_aq: bool,
    /// The game whose bitrate is in use, if any.
    pub game: Option<String>,
}

impl Video {
    /// `playing` is the game being played (lower-case exe), if any.
    pub fn encode(&self, playing: Option<&str>) -> Encode {
        let own = playing.and_then(|g| self.game_bitrates.get(g).map(|m| (g.to_string(), *m)));
        let (game, bitrate) = match own {
            Some((g, m)) => (Some(g), m),
            None => (None, self.bitrate_mbps),
        };
        let d = Video::default();
        let src = if self.advanced { self } else { &d };
        Encode {
            preset: src.preset.clone(),
            rate_control: src.rate_control.clone(),
            cq: src.cq,
            bitrate_mbps: bitrate,
            peak_factor: src.peak_factor,
            spatial_aq: src.spatial_aq,
            game,
        }
    }
}

#[derive(Deserialize, Serialize, Clone, Debug)]
#[serde(default)]
pub struct Audio {
    pub mic: bool,
    /// Lower-case exe names mixed together onto the Communication track (track 3).
    pub communication_apps: Vec<String>,
    /// Never recorded.
    pub ignored_apps: Vec<String>,
}

#[derive(Deserialize, Serialize, Clone, Debug, PartialEq)]
pub struct Hotkey {
    /// e.g. "F13" or "Ctrl+Alt+F9"
    pub keys: String,
    /// "save 30s" | "save 2m" | … | "open" (clips window) | "pause" (toggle recording)
    pub action: String,
}

pub const SEGMENT_SECS: u32 = 2;

impl Default for Config {
    fn default() -> Self {
        Self {
            buffer_dir: PathBuf::from(r"G:\clipback\buffer"),
            clips_dir: default_videos(),
            max_buffer_gb: 200.0,
            port: 47820,
            ffmpeg: String::new(),
            start_with_windows: false,
            video: Video::default(),
            audio: Audio::default(),
            // One hotkey, the same as NVIDIA's "save instant replay", so it's where hands already go.
            // Stream Deck keys use `clipbackw save 90s` etc. instead of hotkeys.
            hotkeys: vec![Hotkey { keys: "Alt+F10".into(), action: "save 30s".into() }],
        }
    }
}

impl Default for Video {
    fn default() -> Self {
        Self {
            output_idx: 0,
            fps: 60,
            codec: "h264".into(),
            bitrate_mbps: 20.0,
            draw_mouse: true,
            game_bitrates: BTreeMap::new(),
            advanced: false,
            preset: "p4".into(),
            rate_control: "vbr".into(),
            cq: 23,
            peak_factor: 2.0,
            spatial_aq: false,
        }
    }
}

impl Default for Audio {
    fn default() -> Self {
        let comms = [
            "discord.exe", "discordptb.exe", "discordcanary.exe", "ms-teams.exe", "teams.exe", "zoom.exe",
            "slack.exe", "ts3client_win64.exe", "teamspeak.exe", "skype.exe", "telegram.exe", "whatsapp.exe",
            "signal.exe", "mumble.exe", "guilded.exe", "element.exe", "vesktop.exe",
        ];
        Self { mic: true, communication_apps: comms.iter().map(|s| s.to_string()).collect(), ignored_apps: Vec::new() }
    }
}

/// The user's Videos folder (it may have been moved off C:).
fn default_videos() -> PathBuf {
    known_videos_folder().unwrap_or_else(|| {
        let home = std::env::var_os("USERPROFILE").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(r"C:\"));
        home.join("Videos")
    })
}

fn known_videos_folder() -> Option<PathBuf> {
    use windows_sys::Win32::System::Registry::{HKEY_CURRENT_USER, RRF_RT_REG_SZ, RegGetValueW};
    let key: Vec<u16> =
        r"Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders".encode_utf16().chain([0]).collect();
    let value: Vec<u16> = "My Video\0".encode_utf16().collect();
    let mut buf = vec![0u16; 1024];
    let mut bytes = (buf.len() * 2) as u32;
    // Expandable values (%USERPROFILE%\Videos) come back expanded, typed as REG_SZ.
    let r = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            key.as_ptr(),
            value.as_ptr(),
            RRF_RT_REG_SZ,
            std::ptr::null_mut(),
            buf.as_mut_ptr() as *mut _,
            &mut bytes,
        )
    };
    if r != 0 {
        return None;
    }
    let len = buf.iter().position(|c| *c == 0).unwrap_or(0);
    (len > 0).then(|| PathBuf::from(String::from_utf16_lossy(&buf[..len])))
}

pub fn app_dir() -> PathBuf {
    let base = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    base.join("clipback")
}

pub fn config_path() -> PathBuf {
    app_dir().join("config.toml")
}

/// %LOCALAPPDATA%\clipback: logs, thumbnails, the editor's browser profile.
pub fn local_dir() -> PathBuf {
    let base = std::env::var_os("LOCALAPPDATA").map(PathBuf::from).unwrap_or_else(std::env::temp_dir);
    base.join("clipback")
}

impl Config {
    pub fn load() -> Result<Self> {
        let path = config_path();
        if !path.exists() {
            let cfg = Config::default();
            cfg.save()?;
            return Ok(cfg);
        }
        let text = std::fs::read_to_string(&path).with_context(|| format!("reading {}", path.display()))?;
        let mut cfg: Config = toml::from_str(&text).with_context(|| format!("parsing {}", path.display()))?;
        cfg.normalise();
        cfg.validate()?;
        Ok(cfg)
    }

    pub fn save(&self) -> Result<()> {
        self.validate()?;
        let path = config_path();
        std::fs::create_dir_all(app_dir())?;
        let text = format!(
            "# clipback settings. Easiest to change from the tray icon's Options page.\n\
             # hotkey actions: \"save 30s\" / \"save 2m\" / \"save 1h\" (any length), \"open\", \"pause\"\n\n{}",
            toml::to_string_pretty(self)?
        );
        std::fs::write(&path, text).with_context(|| format!("writing {}", path.display()))
    }

    pub fn normalise(&mut self) {
        for list in [&mut self.audio.communication_apps, &mut self.audio.ignored_apps] {
            for app in list.iter_mut() {
                *app = app.trim().to_lowercase();
            }
            list.retain(|a| !a.is_empty());
            list.sort();
            list.dedup();
        }
        let games = std::mem::take(&mut self.video.game_bitrates);
        self.video.game_bitrates =
            games.into_iter().map(|(k, v)| (k.trim().to_lowercase(), v)).filter(|(k, _)| !k.is_empty()).collect();
    }

    pub fn validate(&self) -> Result<()> {
        if !matches!(self.video.codec.as_str(), "h264" | "hevc" | "av1") {
            bail!("codec must be h264, hevc or av1 (got {:?})", self.video.codec);
        }
        if self.video.fps == 0 || self.video.fps > 240 || self.video.bitrate_mbps <= 0.0 {
            bail!("fps must be 1–240 and bitrate above zero");
        }
        let v = &self.video;
        if !matches!(v.preset.as_str(), "p1" | "p2" | "p3" | "p4" | "p5" | "p6" | "p7") {
            bail!("preset must be p1–p7 (got {:?})", v.preset);
        }
        if !matches!(v.rate_control.as_str(), "vbr" | "cbr" | "cq") {
            bail!("rate control must be vbr, cbr or cq (got {:?})", v.rate_control);
        }
        if v.cq > 51 || !(1.0..=4.0).contains(&v.peak_factor) {
            bail!("quality level must be 0–51 and peak 1–4× the bitrate");
        }
        if v.game_bitrates.values().any(|m| !(1.0..=300.0).contains(m)) {
            bail!("game bitrates must be 1–300 Mbps");
        }
        if self.max_buffer_gb < 1.0 {
            bail!("buffer size must be at least 1 GB");
        }
        for h in &self.hotkeys {
            crate::hotkey::parse(&h.keys)?;
            parse_action(&h.action)?;
        }
        Ok(())
    }

    pub fn encoder(&self) -> &'static str {
        match self.video.codec.as_str() {
            "hevc" => "hevc_nvenc",
            "av1" => "av1_nvenc",
            _ => "h264_nvenc",
        }
    }

    pub fn max_ring_bytes(&self) -> u64 {
        (self.max_buffer_gb * 1e9) as u64
    }

    pub fn year_dir(&self, year: i32) -> PathBuf {
        self.clips_dir.join(format!("Clips {year}"))
    }

    pub fn video_ring(&self) -> PathBuf {
        self.buffer_dir.join("video")
    }

    pub fn audio_ring(&self) -> PathBuf {
        self.buffer_dir.join("audio")
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

/// ffprobe next to ffmpeg.
pub fn ffprobe_for(ffmpeg: &Path) -> PathBuf {
    if ffmpeg == Path::new("ffmpeg") {
        return PathBuf::from("ffprobe");
    }
    ffmpeg.with_file_name("ffprobe.exe")
}

fn runs(exe: &Path) -> bool {
    crate::util::command(exe)
        .arg("-version")
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

#[derive(Debug, Clone, PartialEq)]
pub enum Action {
    Save(u64),
    Open,
    Pause,
}

pub fn parse_action(s: &str) -> Result<Action> {
    let s = s.trim().to_lowercase();
    match s.as_str() {
        "open" => Ok(Action::Open),
        "pause" => Ok(Action::Pause),
        _ => match s.strip_prefix("save") {
            Some(rest) => Ok(Action::Save(parse_duration(rest)?)),
            None => bail!("unknown hotkey action {s:?} (use \"save 30s\", \"open\" or \"pause\")"),
        },
    }
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

/// 150 → "2m30s".
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
    fn durations_and_actions() {
        assert_eq!(parse_duration("90").unwrap(), 90);
        assert_eq!(parse_duration("2m").unwrap(), 120);
        assert_eq!(parse_duration("1.5H").unwrap(), 5400);
        assert!(parse_duration("0").is_err());
        assert_eq!(format_duration(150), "2m30s");
        assert_eq!(parse_action("save 30s").unwrap(), Action::Save(30));
        assert_eq!(parse_action("Open").unwrap(), Action::Open);
        assert!(parse_action("dance").is_err());
    }

    #[test]
    fn game_bitrate_and_advanced() {
        let mut v = Video::default();
        v.game_bitrates.insert("cs2.exe".into(), 40.0);
        v.game_bitrates.insert("overwatch.exe".into(), 35.0);
        v.preset = "p7".into();
        let e = v.encode(Some("overwatch.exe"));
        assert_eq!((e.bitrate_mbps, e.game.as_deref()), (35.0, Some("overwatch.exe")));
        assert_eq!(e.preset, "p4"); // advanced is off
        v.advanced = true;
        let e = v.encode(Some("notepad.exe"));
        assert_eq!((e.bitrate_mbps, e.game, e.preset.as_str()), (20.0, None, "p7"));
    }

    #[test]
    fn default_round_trips() {
        let cfg = Config::default();
        let text = toml::to_string_pretty(&cfg).unwrap();
        let back: Config = toml::from_str(&text).unwrap();
        back.validate().unwrap();
        assert_eq!(back.hotkeys, cfg.hotkeys);
        assert_eq!(back.video.codec, "h264");
        assert!(back.audio.communication_apps.contains(&"discord.exe".to_string()));
    }
}
