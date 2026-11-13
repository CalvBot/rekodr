//! "Save the last N seconds": pick the segments covering the range and stream-copy them into one MP4.
//! Works straight from the ring folder, so it needs no connection to the running recorder.

use crate::config::{format_duration, Config};
use crate::ring::{self, Segment};
use anyhow::{bail, Context, Result};
use chrono::{DateTime, Duration as ChronoDuration, Local};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

/// Segments needed for [now - secs, now], given every segment currently in the ring.
/// `open_last` says the newest file is still being written and must be left out.
fn select(mut segs: Vec<Segment>, now: DateTime<Local>, secs: u64, segment_secs: u32, open_last: bool) -> Vec<Segment> {
    if open_last {
        segs.pop();
    }
    let from = now - ChronoDuration::seconds(secs as i64);
    let seg_len = ChronoDuration::seconds(segment_secs as i64);
    segs.retain(|s| s.start + seg_len > from && s.start <= now);
    segs
}

/// Line for ffmpeg's concat list: forward slashes, single quotes escaped.
fn concat_line(path: &Path) -> String {
    let p = path.to_string_lossy().replace('\\', "/").replace('\'', r"'\''");
    format!("file '{p}'\n")
}

pub fn save_last(cfg: &Config, ffmpeg: &Path, secs: u64) -> Result<PathBuf> {
    let now = Local::now();
    let before = ring::list(&cfg.ring_dir)?;
    let Some(newest) = before.last().map(|s| s.path.clone()) else {
        bail!("the buffer at {} is empty — is `clipback run` running?", cfg.ring_dir.display());
    };

    // The newest segment is still being written. Wait for the recorder to move on to the next one so the
    // clip includes everything up to the moment of the request; if it never does, the recorder is stopped
    // and the newest segment is already complete.
    let deadline = Instant::now() + Duration::from_millis(cfg.segment_secs as u64 * 1000 + 1500);
    let mut segs = before;
    let mut open_last = false;
    while Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(150));
        segs = ring::list(&cfg.ring_dir)?;
        if segs.last().is_some_and(|s| s.path != newest) {
            open_last = true;
            break;
        }
    }

    let chosen = select(segs, now, secs, cfg.segment_secs, open_last);
    if chosen.is_empty() {
        bail!("nothing recorded in the last {}", format_duration(secs));
    }

    std::fs::create_dir_all(&cfg.save_dir).with_context(|| format!("creating {}", cfg.save_dir.display()))?;
    let stem = format!("clip_{}_{}", now.format("%Y%m%d_%H%M%S"), format_duration(secs));
    let out = cfg.save_dir.join(format!("{stem}.mp4"));
    let list_path = std::env::temp_dir().join(format!("clipback_{stem}_{}.txt", std::process::id()));
    let list: String = chosen.iter().map(|s| concat_line(&s.path)).collect();
    std::fs::write(&list_path, list).context("writing concat list")?;

    let mut cmd = Command::new(ffmpeg);
    cmd.args(["-hide_banner", "-loglevel", "error", "-y"])
        .args(["-f", "concat", "-safe", "0", "-i"])
        .arg(&list_path)
        .args(["-c", "copy", "-movflags", "+faststart"]);
    if cfg.codec == "hevc" {
        cmd.args(["-tag:v", "hvc1"]); // so Apple/Discord players accept it
    }
    let result = cmd.arg(&out).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::piped()).output();
    let _ = std::fs::remove_file(&list_path);
    let output = result.context("running ffmpeg")?;
    if !output.status.success() {
        let _ = std::fs::remove_file(&out);
        bail!("ffmpeg export failed: {}", String::from_utf8_lossy(&output.stderr).trim());
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::TimeZone;

    fn seg(sec: u32) -> Segment {
        let start = Local.with_ymd_and_hms(2026, 1, 1, 12, 0, sec).unwrap();
        Segment { path: PathBuf::from(format!("seg_{sec:02}.mkv")), start, size: 1 }
    }

    #[test]
    fn selects_covering_segments() {
        // Segments start at :00, :02 … :20; the one at :20 is still open.
        let segs: Vec<Segment> = (0..=10).map(|i| seg(i * 2)).collect();
        let now = Local.with_ymd_and_hms(2026, 1, 1, 12, 0, 21).unwrap();
        let chosen = select(segs.clone(), now, 10, 2, true);
        // Last 10 s = :11–:21 → segments :10 … :18 (the :10 one holds :11; :20 is open).
        let starts: Vec<String> = chosen.iter().map(|s| s.start.format("%S").to_string()).collect();
        assert_eq!(starts, ["10", "12", "14", "16", "18"]);

        // Recorder stopped: the newest segment is complete and included.
        let chosen = select(segs.clone(), now, 4, 2, false);
        let starts: Vec<String> = chosen.iter().map(|s| s.start.format("%S").to_string()).collect();
        assert_eq!(starts, ["16", "18", "20"]);

        // Asking for more than exists returns everything closed.
        assert_eq!(select(segs, now, 3600, 2, true).len(), 10);
    }

    #[test]
    fn concat_escaping() {
        assert_eq!(concat_line(Path::new(r"G:\clip's\seg_1.mkv")), "file 'G:/clip'\\''s/seg_1.mkv'\n");
    }
}
