//! The saved clips: listing the `Clips <year>` folders, probing them, and the scrub-strip
//! thumbnails (one image of 24 frames across the clip), cached in %LOCALAPPDATA%\clipback\thumbs.

use crate::state;
use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use std::hash::{Hash, Hasher};
use std::path::{Component, Path, PathBuf};
use std::process::Stdio;
use std::sync::{Condvar, Mutex};
use std::time::UNIX_EPOCH;

pub const SPRITE_FRAMES: u32 = 24;
pub const SPRITE_WIDTH: u32 = 320;
const VIDEO_EXTS: [&str; 4] = ["mp4", "mkv", "mov", "webm"];

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Info {
    pub duration: f64,
    pub width: u32,
    pub height: u32,
    pub fps: f64,
    /// Audio track titles, in order.
    pub tracks: Vec<String>,
}

#[derive(Serialize)]
pub struct Clip {
    /// Path relative to the clips folder, with forward slashes.
    pub id: String,
    pub name: String,
    pub size: u64,
    /// Modified time, ms since 1970.
    pub mtime: i64,
    pub info: Option<Info>,
}

fn thumbs_dir() -> PathBuf {
    crate::config::local_dir().join("thumbs")
}

fn is_video(p: &Path) -> bool {
    p.extension().and_then(|e| e.to_str()).is_some_and(|e| VIDEO_EXTS.contains(&e.to_ascii_lowercase().as_str()))
}

/// Cache key for a file: changes when the file does.
fn key(path: &Path) -> String {
    let meta = std::fs::metadata(path).ok();
    let mut h = std::collections::hash_map::DefaultHasher::new();
    path.hash(&mut h);
    meta.as_ref().map(|m| m.len()).hash(&mut h);
    meta.and_then(|m| m.modified().ok()).and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_millis()).hash(&mut h);
    format!("{:016x}", h.finish())
}

/// A clip id from the UI → its path, refusing anything outside the `Clips <year>` folders.
pub fn resolve(id: &str) -> Result<PathBuf> {
    let rel = Path::new(id);
    let mut parts = rel.components();
    let ok_first = matches!(parts.next(), Some(Component::Normal(d)) if d.to_string_lossy().starts_with("Clips "));
    let ok_rest = rel.components().skip(1).all(|c| matches!(c, Component::Normal(_)));
    if !ok_first || !ok_rest || rel.components().count() < 2 || !is_video(rel) {
        bail!("not a clip: {id}");
    }
    let path = state::cfg().clips_dir.join(rel);
    if !path.is_file() {
        bail!("clip not found: {id}");
    }
    Ok(path)
}

pub fn id_of(path: &Path) -> Option<String> {
    let rel = path.strip_prefix(&state::cfg().clips_dir).ok()?;
    Some(rel.to_string_lossy().replace('\\', "/"))
}

/// Every clip, newest first.
pub fn list() -> Vec<Clip> {
    let root = state::cfg().clips_dir;
    let mut out = Vec::new();
    let Ok(dirs) = std::fs::read_dir(&root) else { return out };
    for dir in dirs.flatten() {
        let name = dir.file_name().to_string_lossy().to_string();
        if !name.starts_with("Clips ") || !dir.path().is_dir() {
            continue;
        }
        let Ok(files) = std::fs::read_dir(dir.path()) else { continue };
        for f in files.flatten() {
            let path = f.path();
            let Ok(meta) = f.metadata() else { continue };
            if !meta.is_file() || !is_video(&path) {
                continue;
            }
            let mtime = meta.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_millis() as i64).unwrap_or(0);
            out.push(Clip {
                id: format!("{name}/{}", f.file_name().to_string_lossy()),
                name: path.file_stem().unwrap_or_default().to_string_lossy().to_string(),
                size: meta.len(),
                mtime,
                info: cached_info(&path),
            });
        }
    }
    out.sort_by(|a, b| b.mtime.cmp(&a.mtime));
    out
}

fn cached_info(path: &Path) -> Option<Info> {
    let text = std::fs::read_to_string(thumbs_dir().join(format!("{}.json", key(path)))).ok()?;
    serde_json::from_str(&text).ok()
}

fn ratio(s: &str) -> f64 {
    match s.split_once('/') {
        Some((a, b)) => a.parse::<f64>().unwrap_or(0.0) / b.parse::<f64>().unwrap_or(1.0).max(1e-9),
        None => s.parse().unwrap_or(0.0),
    }
}

/// Duration, size and audio tracks (ffprobe, cached).
pub fn info(path: &Path) -> Result<Info> {
    if let Some(info) = cached_info(path) {
        return Ok(info);
    }
    let out = crate::util::command(crate::config::ffprobe_for(&state::ffmpeg()))
        .args(["-v", "error", "-show_entries", "format=duration:stream=codec_type,width,height,avg_frame_rate,r_frame_rate:stream_tags=title,handler_name"])
        .args(["-of", "json"])
        .arg(path)
        .stdin(Stdio::null())
        .output()
        .context("running ffprobe")?;
    if !out.status.success() {
        bail!("ffprobe failed: {}", String::from_utf8_lossy(&out.stderr).trim());
    }
    let v: serde_json::Value = serde_json::from_slice(&out.stdout)?;
    let streams = v["streams"].as_array().cloned().unwrap_or_default();
    let video = streams.iter().find(|s| s["codec_type"] == "video");
    let mut info = Info {
        duration: v["format"]["duration"].as_str().and_then(|d| d.parse().ok()).unwrap_or(0.0),
        width: video.and_then(|s| s["width"].as_u64()).unwrap_or(0) as u32,
        height: video.and_then(|s| s["height"].as_u64()).unwrap_or(0) as u32,
        fps: video
            .map(|s| {
                let avg = ratio(s["avg_frame_rate"].as_str().unwrap_or("0"));
                if avg > 0.0 { avg } else { ratio(s["r_frame_rate"].as_str().unwrap_or("0")) }
            })
            .unwrap_or(0.0),
        tracks: Vec::new(),
    };
    for (n, s) in streams.iter().filter(|s| s["codec_type"] == "audio").enumerate() {
        let tags = &s["tags"];
        let title = tags["title"].as_str().or(tags["handler_name"].as_str()).filter(|t| !t.is_empty() && *t != "SoundHandler");
        info.tracks.push(title.map(str::to_string).unwrap_or_else(|| format!("Track {}", n + 1)));
    }
    let _ = std::fs::create_dir_all(thumbs_dir());
    let _ = std::fs::write(thumbs_dir().join(format!("{}.json", key(path))), serde_json::to_string(&info)?);
    Ok(info)
}

/// At most two ffmpeg thumbnail jobs at once, so a page full of new clips doesn't swamp the PC.
static SLOTS: (Mutex<u32>, Condvar) = (Mutex::new(0), Condvar::new());

fn with_slot<T>(f: impl FnOnce() -> T) -> T {
    let (lock, cv) = &SLOTS;
    let mut busy = cv.wait_while(lock.lock().unwrap(), |n| *n >= 2).unwrap();
    *busy += 1;
    drop(busy);
    let out = f();
    *lock.lock().unwrap() -= 1;
    cv.notify_one();
    out
}

/// The scrub strip: SPRITE_FRAMES frames side by side, evenly across the clip (JPEG).
pub fn sprite(path: &Path) -> Result<PathBuf> {
    let file = thumbs_dir().join(format!("{}.jpg", key(path)));
    if file.is_file() {
        return Ok(file);
    }
    let info = info(path)?;
    with_slot(|| {
        if file.is_file() {
            return Ok(file.clone());
        }
        let tmp = file.with_extension("tmp.jpg");
        let dur = info.duration.max(0.1);
        let vf = format!(
            // tpad holds the last frame so the strip still fills when the last keyframe is early.
            "tpad=stop_mode=clone:stop_duration=3,fps={}/{dur:.3},scale={SPRITE_WIDTH}:-2:flags=bilinear,tile={SPRITE_FRAMES}x1",
            SPRITE_FRAMES
        );
        // Keyframes only (one every 2 s in clipback's clips) makes this fast; short or odd clips
        // without enough keyframes get a full decode instead.
        for keyframes_only in [true, false] {
            let mut cmd = crate::util::command(state::ffmpeg());
            cmd.args(["-hide_banner", "-loglevel", "error", "-y"]);
            if keyframes_only && dur > 12.0 {
                cmd.args(["-skip_frame", "nokey"]);
            }
            cmd.arg("-i").arg(path).args(["-an", "-vf", &vf, "-frames:v", "1", "-q:v", "6"]).arg(&tmp);
            let ok = cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null()).status().is_ok_and(|s| s.success());
            if ok && tmp.is_file() {
                std::fs::rename(&tmp, &file)?;
                return Ok(file.clone());
            }
        }
        bail!("couldn't make thumbnails for {}", path.display())
    })
}

/// One audio track on its own (stream copy, cached) so the editor can set each track's volume.
pub fn track(path: &Path, index: usize) -> Result<PathBuf> {
    let file = thumbs_dir().join(format!("{}_a{index}.m4a", key(path)));
    if file.is_file() {
        return Ok(file);
    }
    let _ = std::fs::create_dir_all(thumbs_dir());
    let tmp = thumbs_dir().join(format!("{}_a{index}.tmp.m4a", key(path)));
    let ok = crate::util::command(state::ffmpeg())
        .args(["-hide_banner", "-loglevel", "error", "-y", "-i"])
        .arg(path)
        .args(["-map", &format!("0:a:{index}"), "-c", "copy", "-vn"])
        .arg(&tmp)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success());
    if !ok {
        bail!("couldn't extract audio track {index}");
    }
    std::fs::rename(&tmp, &file)?;
    Ok(file)
}

/// Makes a new clip's thumbnails in the background, so the clips page shows it straight away.
pub fn warm(path: PathBuf) {
    std::thread::spawn(move || {
        let _ = sprite(&path);
    });
}

/// Drops cached thumbnails for files that no longer exist (or changed), keeping the newest 2000.
pub fn tidy_cache() {
    let Ok(entries) = std::fs::read_dir(thumbs_dir()) else { return };
    let mut files: Vec<(std::time::SystemTime, PathBuf)> =
        entries.flatten().filter_map(|e| Some((e.metadata().ok()?.modified().ok()?, e.path()))).collect();
    files.sort_by(|a, b| b.0.cmp(&a.0));
    let live: std::collections::HashSet<String> = list().iter().filter_map(|c| resolve(&c.id).ok()).map(|p| key(&p)).collect();
    for (i, (_, path)) in files.iter().enumerate() {
        let name = path.file_name().unwrap_or_default().to_string_lossy();
        let k = name.split(['.', '_']).next().unwrap_or("");
        if i >= 2000 * 3 || !live.contains(k) {
            let _ = std::fs::remove_file(path);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids_stay_inside_the_clip_folders() {
        for bad in ["../x.mp4", "Clips 2026/../../x.mp4", "C:/x.mp4", "Other/x.mp4", "Clips 2026/x.exe", "x.mp4"] {
            assert!(resolve(bad).is_err(), "{bad}");
        }
        assert_eq!(ratio("60/1"), 60.0);
        assert_eq!(ratio("30000/1001").round(), 30.0);
    }
}
