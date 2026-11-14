//! The on-disk ring buffer.
//!
//! ```text
//! ring\video\<run>\v_00000.mkv …   one folder per ffmpeg run (restarts after the lock screen etc.)
//!                  list.csv        "v_00000.mkv,0.000000,2.000000" per finished segment (ffmpeg writes it)
//!                  anchor.txt      wall-clock µs of the run's first frame
//! ring\audio\<stream>\a_00000.mka  one folder per app audio stream (or the mic), Opus, 10 s segments
//!                  list.csv
//!                  meta.json       exe, kind, wall-clock µs of the first sample
//! ```
//! Times are all wall-clock µs, so video and every audio stream line up exactly.

use anyhow::Result;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct VideoSeg {
    pub path: PathBuf,
    pub start_us: i64,
    pub end_us: i64,
    pub size: u64,
    /// The segment's own timestamps (seconds from its run's start), for ffmpeg's concat list.
    pub inpoint: f64,
    pub outpoint: f64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AudioMeta {
    /// "mic" or "app"
    pub kind: String,
    /// Lower-case exe ("mic" for the mic).
    pub exe: String,
    pub pid: u32,
    /// Wall-clock µs of the first sample.
    pub start_us: i64,
}

#[derive(Debug, Clone)]
pub struct AudioSeg {
    pub path: PathBuf,
    /// Offsets from the stream start, µs. `end_us` is None for the segment still being written.
    pub start_us: i64,
    pub end_us: Option<i64>,
    pub size: u64,
}

#[derive(Debug, Clone)]
pub struct AudioStream {
    pub dir: PathBuf,
    pub meta: AudioMeta,
    pub segs: Vec<AudioSeg>,
}

impl AudioStream {
    /// Wall-clock span covered by the remaining segments (end None = still recording).
    pub fn span(&self) -> Option<(i64, Option<i64>)> {
        let first = self.segs.first()?;
        let last = self.segs.last()?;
        Some((self.meta.start_us + first.start_us, last.end_us.map(|e| self.meta.start_us + e)))
    }
}

/// (file name, start s, end s) lines of a segment list.
fn read_list(path: &Path) -> Vec<(String, f64, f64)> {
    let text = std::fs::read_to_string(path).unwrap_or_default();
    text.lines()
        .filter_map(|l| {
            let mut parts = l.rsplitn(3, ',');
            let end: f64 = parts.next()?.trim().parse().ok()?;
            let start: f64 = parts.next()?.trim().parse().ok()?;
            let name = parts.next()?.trim().trim_matches('"').to_string();
            Some((name, start, end))
        })
        .collect()
}

fn subdirs(dir: &Path) -> Vec<PathBuf> {
    let Ok(entries) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut dirs: Vec<PathBuf> = entries.flatten().filter(|e| e.path().is_dir()).map(|e| e.path()).collect();
    dirs.sort();
    dirs
}

/// Media files in a folder, sorted by name.
fn media_files(dir: &Path, ext: &str) -> Vec<(String, u64)> {
    let Ok(entries) = std::fs::read_dir(dir) else { return Vec::new() };
    let mut files: Vec<(String, u64)> = entries
        .flatten()
        .filter_map(|e| {
            let name = e.file_name().to_string_lossy().to_string();
            name.ends_with(ext).then(|| (name, e.metadata().map(|m| m.len()).unwrap_or(0)))
        })
        .collect();
    files.sort();
    files
}

/// All finished video segments, oldest first.
pub fn video_segments(ring: &Path) -> Vec<VideoSeg> {
    let mut out = Vec::new();
    for run in subdirs(ring) {
        let Some(anchor) = std::fs::read_to_string(run.join("anchor.txt")).ok().and_then(|t| t.trim().parse::<i64>().ok())
        else {
            continue;
        };
        let files = media_files(&run, ".mkv");
        for (name, start, end) in read_list(&run.join("list.csv")) {
            let Some((_, size)) = files.iter().find(|(n, _)| *n == name) else { continue };
            out.push(VideoSeg {
                path: run.join(&name),
                start_us: anchor + (start * 1e6).round() as i64,
                end_us: anchor + (end * 1e6).round() as i64,
                size: *size,
                inpoint: start,
                outpoint: end,
            });
        }
    }
    out.sort_by_key(|s| s.start_us);
    out
}

pub fn audio_streams(ring: &Path) -> Vec<AudioStream> {
    let mut out = Vec::new();
    for dir in subdirs(ring) {
        let Some(meta) = std::fs::read_to_string(dir.join("meta.json"))
            .ok()
            .and_then(|t| serde_json::from_str::<AudioMeta>(&t).ok())
        else {
            continue;
        };
        let files = media_files(&dir, ".mka");
        let list = read_list(&dir.join("list.csv"));
        let mut segs = Vec::new();
        let mut next_start = 0i64;
        for (name, size) in &files {
            match list.iter().find(|(n, _, _)| n == name) {
                Some((_, s, e)) => {
                    let (s, e) = ((s * 1e6).round() as i64, (e * 1e6).round() as i64);
                    segs.push(AudioSeg { path: dir.join(name), start_us: s, end_us: Some(e), size: *size });
                    next_start = e;
                }
                // Not listed yet: the one being written (only ever the last file).
                None => segs.push(AudioSeg { path: dir.join(name), start_us: next_start, end_us: None, size: *size }),
            }
        }
        // A file missing from the list in the middle means ffmpeg died mid-segment; keep only the
        // run of segments that are contiguous from the end.
        if let Some(pos) = segs.iter().rposition(|s| s.end_us.is_none()) {
            if pos + 1 != segs.len() {
                segs.drain(..=pos);
            }
        }
        out.push(AudioStream { dir, meta, segs });
    }
    out
}

/// Total bytes in the ring.
pub fn size(ring: &Path) -> u64 {
    fn walk(dir: &Path) -> u64 {
        let Ok(entries) = std::fs::read_dir(dir) else { return 0 };
        entries
            .flatten()
            .map(|e| match e.file_type() {
                Ok(t) if t.is_dir() => walk(&e.path()),
                _ => e.metadata().map(|m| m.len()).unwrap_or(0),
            })
            .sum()
    }
    walk(ring)
}

/// Deletes the oldest footage (video and audio alike) until the ring fits in `max_bytes`, then
/// removes folders that are empty and finished. Returns bytes freed.
pub fn prune(ring_dir: &Path, max_bytes: u64) -> Result<u64> {
    let video_dir = ring_dir.join("video");
    let audio_dir = ring_dir.join("audio");
    let mut total = size(ring_dir);
    let mut freed = 0;
    if total > max_bytes {
        // Every finished file, by end time.
        let mut files: Vec<(i64, u64, PathBuf)> =
            video_segments(&video_dir).into_iter().map(|s| (s.end_us, s.size, s.path)).collect();
        for stream in audio_streams(&audio_dir) {
            for seg in stream.segs {
                if let Some(end) = seg.end_us {
                    files.push((stream.meta.start_us + end, seg.size, seg.path));
                }
            }
        }
        files.sort_by_key(|f| f.0);
        // The newest minute is never touched, whatever the cap.
        let keep_after = crate::util::now_us() - 60_000_000;
        for (end, size, path) in files {
            if total <= max_bytes || end > keep_after {
                break;
            }
            if std::fs::remove_file(&path).is_ok() {
                total = total.saturating_sub(size);
                freed += size;
            }
        }
    }
    // Folders with no media left whose list hasn't changed for a while are finished.
    for dir in subdirs(&video_dir).into_iter().chain(subdirs(&audio_dir)) {
        let has_media = !media_files(&dir, ".mkv").is_empty() || !media_files(&dir, ".mka").is_empty();
        let idle = std::fs::metadata(dir.join("list.csv"))
            .or_else(|_| std::fs::metadata(&dir))
            .and_then(|m| m.modified())
            .is_ok_and(|t| t.elapsed().is_ok_and(|e| e.as_secs() > 300));
        if !has_media && idle {
            let _ = std::fs::remove_dir_all(&dir);
        }
    }
    Ok(freed)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("clipback-test-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn reads_video_runs() {
        let ring = tmp("video");
        let run = ring.join("20260101_000000_000");
        std::fs::create_dir_all(&run).unwrap();
        std::fs::write(run.join("anchor.txt"), "1000000000").unwrap();
        std::fs::write(run.join("list.csv"), "v_00000.mkv,0.000000,2.000000\nv_00001.mkv,2.000000,4.000000\n").unwrap();
        for i in 0..3 {
            std::fs::write(run.join(format!("v_{i:05}.mkv")), [0u8; 10]).unwrap();
        }
        let segs = video_segments(&ring);
        assert_eq!(segs.len(), 2, "the unlisted (open) segment is left out");
        assert_eq!(segs[1].start_us, 1_002_000_000);
        assert_eq!(segs[1].end_us, 1_004_000_000);
        std::fs::remove_dir_all(&ring).unwrap();
    }

    #[test]
    fn reads_audio_streams_and_prunes_oldest() {
        let ring = tmp("prune");
        let a = ring.join("audio").join("s1");
        std::fs::create_dir_all(&a).unwrap();
        let meta = AudioMeta { kind: "app".into(), exe: "game.exe".into(), pid: 1, start_us: 5_000_000 };
        std::fs::write(a.join("meta.json"), serde_json::to_string(&meta).unwrap()).unwrap();
        std::fs::write(a.join("list.csv"), "a_00000.mka,0.0,10.0\na_00001.mka,10.0,20.0\n").unwrap();
        for i in 0..3 {
            std::fs::write(a.join(format!("a_{i:05}.mka")), vec![0u8; 1000]).unwrap();
        }
        let streams = audio_streams(&ring.join("audio"));
        assert_eq!(streams[0].segs.len(), 3);
        assert_eq!(streams[0].segs[2].start_us, 20_000_000);
        assert_eq!(streams[0].segs[2].end_us, None);
        assert_eq!(streams[0].span(), Some((5_000_000, None)));

        let freed = prune(&ring, 2500).unwrap(); // 3 segments + small meta/list files
        assert_eq!(freed, 1000);
        let left = audio_streams(&ring.join("audio"));
        assert_eq!(left[0].segs.len(), 2);
        assert_eq!(left[0].segs[0].start_us, 10_000_000);
        std::fs::remove_dir_all(&ring).unwrap();
    }
}
