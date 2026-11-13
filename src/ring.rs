//! The on-disk ring: a flat folder of seg_YYYYMMDD_HHMMSS.mkv files, named by ffmpeg with their local start time.

use anyhow::{Context, Result};
use chrono::{DateTime, Local, NaiveDateTime, TimeZone};
use std::path::{Path, PathBuf};

pub const PREFIX: &str = "seg_";
pub const EXT: &str = ".mkv";
/// strftime pattern handed to ffmpeg's segment muxer.
pub const FFMPEG_PATTERN: &str = "seg_%Y%m%d_%H%M%S.mkv";
const TIME_FMT: &str = "%Y%m%d_%H%M%S";

#[derive(Debug, Clone)]
pub struct Segment {
    pub path: PathBuf,
    pub start: DateTime<Local>,
    pub size: u64,
}

/// Start time encoded in a segment file name, or None if the file isn't one of ours.
pub fn parse_name(name: &str) -> Option<DateTime<Local>> {
    let stamp = name.strip_prefix(PREFIX)?.strip_suffix(EXT)?;
    let naive = NaiveDateTime::parse_from_str(stamp, TIME_FMT).ok()?;
    Local.from_local_datetime(&naive).earliest()
}

/// All segments in the ring, oldest first. Files that don't match our naming are ignored.
pub fn list(dir: &Path) -> Result<Vec<Segment>> {
    let mut segs = Vec::new();
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(segs),
        Err(e) => return Err(e).with_context(|| format!("reading {}", dir.display())),
    };
    for entry in entries {
        let entry = entry?;
        let name = entry.file_name();
        let Some(start) = name.to_str().and_then(parse_name) else { continue };
        let Ok(meta) = entry.metadata() else { continue };
        if meta.is_file() {
            segs.push(Segment { path: entry.path(), start, size: meta.len() });
        }
    }
    segs.sort_by(|a, b| a.start.cmp(&b.start).then_with(|| a.path.cmp(&b.path)));
    Ok(segs)
}

/// Deletes the oldest segments until the ring fits in `max_bytes`. Returns (files deleted, bytes freed).
pub fn prune(dir: &Path, max_bytes: u64) -> Result<(usize, u64)> {
    let segs = list(dir)?;
    let mut total: u64 = segs.iter().map(|s| s.size).sum();
    let (mut count, mut freed) = (0, 0);
    // Never touch the newest few: ffmpeg is still writing the last one.
    let deletable = segs.len().saturating_sub(5);
    for seg in &segs[..deletable] {
        if total <= max_bytes {
            break;
        }
        if std::fs::remove_file(&seg.path).is_ok() {
            total -= seg.size;
            freed += seg.size;
            count += 1;
        }
    }
    Ok((count, freed))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        let t = parse_name("seg_20261113_202350.mkv").unwrap();
        assert_eq!(t.format(TIME_FMT).to_string(), "20261113_202350");
        assert!(parse_name("clip_20261113_202350.mkv").is_none());
        assert!(parse_name("seg_20261113_202350.mp4").is_none());
        assert!(parse_name("seg_garbage.mkv").is_none());
    }

    #[test]
    fn prune_removes_oldest_only() {
        let dir = std::env::temp_dir().join(format!("clipback-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        for i in 0..10 {
            std::fs::write(dir.join(format!("seg_20260101_0000{i:02}.mkv")), vec![0u8; 1000]).unwrap();
        }
        std::fs::write(dir.join("keep.txt"), b"not ours").unwrap();

        let (count, freed) = prune(&dir, 7000).unwrap();
        assert_eq!((count, freed), (3, 3000));
        let left = list(&dir).unwrap();
        assert_eq!(left.len(), 7);
        assert!(left[0].path.ends_with("seg_20260101_000003.mkv"));

        // The newest five survive even if the cap is tiny.
        prune(&dir, 0).unwrap();
        assert_eq!(list(&dir).unwrap().len(), 5);
        assert!(dir.join("keep.txt").exists());
        std::fs::remove_dir_all(&dir).unwrap();
    }
}
