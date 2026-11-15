//! Saving a clip: the video segments covering the range are stream-copied (instant), and every audio
//! stream that overlaps is cut to match and laid out as tracks:
//!
//! 1. Mix: everything   2. Mic   3. Communication (Discord, Teams, …)   4+. one per other app
//!
//! Tracks 1–3 are always there (silent if nothing played), so an editing layout never shifts.

use crate::apps::{self, Category};
use crate::config::{Config, SEGMENT_SECS};
use crate::ring::{self, AudioStream, VideoSeg};
use crate::state;
use crate::util::{self, now_us};
use anyhow::{Context, Result, bail};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::Stdio;
use std::time::{Duration, Instant};

/// A stretch of continuous video. Recording gaps (lock screen) are cut out, so each piece sits at
/// `offset_us` in the clip.
#[derive(Debug, Clone, PartialEq)]
struct Piece {
    start_us: i64,
    end_us: i64,
    offset_us: i64,
}

fn pieces(segs: &[VideoSeg]) -> Vec<Piece> {
    let mut out: Vec<Piece> = Vec::new();
    let mut offset = 0;
    for s in segs {
        match out.last_mut() {
            Some(p) if (s.start_us - p.end_us).abs() <= 50_000 => {
                offset += s.end_us - p.end_us;
                p.end_us = s.end_us;
            }
            _ => {
                out.push(Piece { start_us: s.start_us, end_us: s.end_us, offset_us: offset });
                offset += s.end_us - s.start_us;
            }
        }
    }
    out
}

#[derive(Debug)]
struct Track {
    title: String,
    /// Indexes into the streams list.
    streams: Vec<usize>,
}

pub const DESKTOP: &str = "Desktop";

fn pretty(exe: &str) -> String {
    apps::display_name(exe)
}

/// Mic, Communication, then one per app: games first (the one played most first), then the rest.
fn tracks(cfg: &Config, streams: &[AudioStream], played: &[(String, f64)]) -> Vec<Track> {
    let mut mic = Track { title: "Mic".into(), streams: Vec::new() };
    let mut comms = Track { title: "Communication".into(), streams: Vec::new() };
    let mut others: Vec<(bool, String, Track)> = Vec::new();
    for (i, s) in streams.iter().enumerate() {
        if s.meta.kind == "mic" {
            mic.streams.push(i);
            continue;
        }
        match apps::category(cfg, &s.meta.exe) {
            Category::Communication => comms.streams.push(i),
            Category::Ignored => {}
            cat => match others.iter_mut().find(|(_, exe, _)| *exe == s.meta.exe) {
                Some((_, _, t)) => t.streams.push(i),
                None => {
                    let game = cat == Category::Game;
                    let title = if game { format!("{} (game)", pretty(&s.meta.exe)) } else { pretty(&s.meta.exe) };
                    others.push((game, s.meta.exe.clone(), Track { title, streams: vec![i] }));
                }
            },
        }
    }
    // Stable: games first, by time played; otherwise in order of first sound.
    let rank = |exe: &str| played.iter().position(|(e, _)| e == exe).unwrap_or(usize::MAX);
    others.sort_by_key(|(game, exe, _)| (!*game, rank(exe)));
    let mut out = vec![mic, comms];
    out.extend(others.into_iter().map(|(_, _, t)| t));
    out
}

fn concat_line(path: &Path) -> String {
    let p = path.to_string_lossy().replace('\\', "/").replace('\'', r"'\''");
    format!("file '{p}'\n")
}

fn secs(us: i64) -> String {
    format!("{:.6}", us as f64 / 1e6)
}

/// The filter graph for the audio tracks. Input 0 is the video; stream `i` is input `i + 1`.
fn audio_graph(pieces: &[Piece], streams: &[AudioStream], tracks: &[Track], total_us: i64) -> String {
    // Which (stream, piece) overlaps exist, so each stream is split exactly as often as it's used.
    let mut parts: Vec<Vec<(usize, i64, i64, i64)>> = vec![Vec::new(); streams.len()]; // (piece, from, to, delay) per stream
    for (i, s) in streams.iter().enumerate() {
        let Some((first, last)) = s.span() else { continue };
        let base = first; // the concat input starts at the first remaining segment
        let avail_end = last.unwrap_or(i64::MAX);
        for (k, p) in pieces.iter().enumerate() {
            let (from, to) = (p.start_us.max(base), p.end_us.min(avail_end));
            if to - from > 1000 {
                parts[i].push((k, from - base, to - base, p.offset_us + (from - p.start_us)));
            }
        }
    }
    let mut g = String::new();
    let mut part_labels: Vec<Vec<String>> = vec![Vec::new(); streams.len()];
    for (i, list) in parts.iter().enumerate() {
        if list.is_empty() {
            continue;
        }
        let srcs: Vec<String> = if list.len() == 1 {
            vec![format!("[{}:a]", i + 1)]
        } else {
            let names: Vec<String> = (0..list.len()).map(|n| format!("[s{i}_{n}]")).collect();
            let _ = writeln!(g, "[{}:a]asplit={}{};", i + 1, list.len(), names.concat());
            names
        };
        for (n, (src, (_, from, to, delay))) in srcs.iter().zip(list).enumerate() {
            let label = format!("[p{i}_{n}]");
            let _ = writeln!(
                g,
                "{src}atrim=start={}:end={},asetpts=PTS-STARTPTS,aformat=sample_fmts=fltp:sample_rates=48000:channel_layouts=stereo,adelay={}S:all=1{label};",
                secs(*from),
                secs(*to),
                delay * 48_000 / 1_000_000
            );
            part_labels[i].push(label);
        }
    }
    let total = secs(total_us);
    for (t, track) in tracks.iter().enumerate() {
        let inputs: Vec<&String> = track.streams.iter().flat_map(|i| &part_labels[*i]).collect();
        match inputs.len() {
            0 => {
                let _ = writeln!(g, "anullsrc=r=48000:cl=stereo,atrim=duration={total}[t{t}];");
            }
            1 => {
                let _ = writeln!(g, "{}apad=whole_dur={total},atrim=duration={total}[t{t}];", inputs[0]);
            }
            n => {
                let joined: String = inputs.iter().map(|s| s.as_str()).collect();
                let _ = writeln!(
                    g,
                    "{joined}amix=inputs={n}:normalize=0:dropout_transition=0,apad=whole_dur={total},atrim=duration={total}[t{t}];"
                );
            }
        }
        let _ = writeln!(g, "[t{t}]asplit=2[o{t}][m{t}];");
    }
    let mix: String = (0..tracks.len()).map(|t| format!("[m{t}]")).collect();
    let _ = write!(
        g,
        "{mix}amix=inputs={}:normalize=0:dropout_transition=0,alimiter=limit=0.98:level=0:latency=1,atrim=duration={total}[mix]",
        tracks.len()
    );
    g
}

pub struct Saved {
    pub path: PathBuf,
    pub tracks: Vec<String>,
    pub secs: f64,
}

fn unique(path: PathBuf) -> PathBuf {
    if !path.exists() {
        return path;
    }
    let stem = path.file_stem().unwrap_or_default().to_string_lossy().to_string();
    for n in 2.. {
        let p = path.with_file_name(format!("{stem} ({n}).mp4"));
        if !p.exists() {
            return p;
        }
    }
    unreachable!()
}

pub fn save_last(secs: u64) -> Result<Saved> {
    let to = now_us();
    save_range(to - secs as i64 * 1_000_000, to)
}

/// Saves wall-clock range [from, to] into `Clips\<year>`.
pub fn save_range(from_us: i64, to_us: i64) -> Result<Saved> {
    let cfg = state::cfg();
    let ffmpeg = state::ffmpeg();
    let video_ring = cfg.video_ring();

    // The segment covering `to` is still being written: wait for it to finish (a couple of seconds
    // at most), so the clip ends at the moment the button was pressed.
    if state::VIDEO_LIVE.load(std::sync::atomic::Ordering::Relaxed) {
        let deadline = Instant::now() + Duration::from_millis(SEGMENT_SECS as u64 * 1000 + 2500);
        while Instant::now() < deadline {
            if ring::video_segments(&video_ring).last().is_some_and(|s| s.end_us >= to_us - 100_000) {
                break;
            }
            std::thread::sleep(Duration::from_millis(150));
        }
    }

    let segs: Vec<VideoSeg> =
        ring::video_segments(&video_ring).into_iter().filter(|s| s.end_us > from_us && s.start_us < to_us).collect();
    if segs.is_empty() {
        bail!("nothing recorded in that time — is recording paused?");
    }
    let pieces = pieces(&segs);
    let total_us = pieces.iter().map(|p| p.end_us - p.start_us).sum::<i64>();
    let (clip_start, clip_end) = (segs[0].start_us, segs.last().unwrap().end_us);

    let streams: Vec<AudioStream> = ring::audio_streams(&cfg.audio_ring())
        .into_iter()
        .filter(|s| match s.span() {
            Some((start, end)) => start < clip_end && end.is_none_or(|e| e > clip_start),
            None => false,
        })
        .collect();
    // The game played most in the clip names and tags it; no game at all is "Desktop".
    let played = crate::activity::games_in(&cfg.buffer_dir, clip_start, clip_end);
    let tracks = tracks(&cfg, &streams, &played);
    let names: Vec<String> = played.iter().map(|(exe, _)| pretty(exe)).collect();
    let label = names.first().cloned().unwrap_or_else(|| DESKTOP.into());
    let start_local = chrono::DateTime::from_timestamp_micros(clip_start)
        .unwrap_or_default()
        .with_timezone(&chrono::Local);
    let dir = cfg.year_dir(chrono::Datelike::year(&start_local));
    std::fs::create_dir_all(&dir).with_context(|| format!("creating {}", dir.display()))?;
    let out = unique(dir.join(format!("{label} {}.mp4", start_local.format("%Y-%m-%d %H-%M-%S"))));

    // Lists and the filter graph go in temp files (graphs get long).
    let tmp = std::env::temp_dir().join(format!("clipback-{}-{}", std::process::id(), now_us()));
    std::fs::create_dir_all(&tmp)?;
    let result = (|| -> Result<()> {
        // Segments keep the run's timestamps; in/out points let concat line them up end to end.
        let list: String = segs
            .iter()
            .map(|s| format!("{}inpoint {:.6}\noutpoint {:.6}\n", concat_line(&s.path), s.inpoint, s.outpoint))
            .collect();
        std::fs::write(tmp.join("video.txt"), list)?;
        let mut cmd = util::command(&ffmpeg);
        cmd.args(["-hide_banner", "-loglevel", "error", "-y"]);
        cmd.args(["-f", "concat", "-safe", "0", "-i"]).arg(tmp.join("video.txt"));
        for (i, s) in streams.iter().enumerate() {
            let list = tmp.join(format!("audio{i}.txt"));
            std::fs::write(&list, s.segs.iter().map(|seg| concat_line(&seg.path)).collect::<String>())?;
            cmd.args(["-f", "concat", "-safe", "0", "-i"]).arg(&list);
        }
        std::fs::write(tmp.join("graph.txt"), audio_graph(&pieces, &streams, &tracks, total_us))?;
        cmd.arg("-/filter_complex").arg(tmp.join("graph.txt"));
        cmd.args(["-map", "0:v", "-c:v", "copy"]);
        if cfg.video.codec == "hevc" {
            cmd.args(["-tag:v", "hvc1"]); // so Apple / browser players accept it
        }
        cmd.args(["-map", "[mix]"]);
        for t in 0..tracks.len() {
            cmd.args(["-map", &format!("[o{t}]")]);
        }
        cmd.args(["-c:a", "aac", "-b:a", "192k", "-ar", "48000"]);
        let titles = std::iter::once("Mix".to_string()).chain(tracks.iter().map(|t| t.title.clone()));
        for (i, title) in titles.enumerate() {
            cmd.arg(format!("-metadata:s:a:{i}")).arg(format!("title={title}"));
            cmd.arg(format!("-metadata:s:a:{i}")).arg(format!("handler_name={title}"));
            cmd.arg(format!("-disposition:a:{i}")).arg(if i == 0 { "default" } else { "0" });
        }
        // Genre = the game (Explorer can show, sort and group by it); the exe names are in the
        // comment so the clips window can show icons.
        let exes: Vec<&str> = played.iter().map(|(e, _)| e.as_str()).collect();
        cmd.arg("-metadata").arg(format!("title={label} · {}", start_local.format("%-d %b %Y %H:%M")));
        cmd.arg("-metadata").arg(format!("genre={label}"));
        cmd.arg("-metadata").arg(format!("description={}", if names.is_empty() { DESKTOP.to_string() } else { names.join(", ") }));
        cmd.arg("-metadata").arg(format!("comment=clipback; games={}", exes.join(",")));
        cmd.args(["-movflags", "+faststart"]);
        cmd.arg(&out).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::piped());
        let output = cmd.output().context("running ffmpeg")?;
        if !output.status.success() {
            let _ = std::fs::remove_file(&out);
            let _ = std::fs::copy(tmp.join("graph.txt"), crate::config::local_dir().join("last-failed-graph.txt"));
            bail!("ffmpeg export failed: {}", String::from_utf8_lossy(&output.stderr).trim());
        }
        Ok(())
    })();
    let _ = std::fs::remove_dir_all(&tmp);
    result?;

    crate::clips::warm(out.clone());
    Ok(Saved {
        path: out,
        tracks: std::iter::once("Mix".to_string()).chain(tracks.into_iter().map(|t| t.title)).collect(),
        secs: total_us as f64 / 1e6,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ring::{AudioMeta, AudioSeg};

    fn seg(start_s: i64) -> VideoSeg {
        VideoSeg { path: PathBuf::from("x"), start_us: start_s * 1_000_000, end_us: (start_s + 2) * 1_000_000, size: 1, inpoint: 0.0, outpoint: 2.0 }
    }

    #[test]
    fn gaps_split_pieces() {
        let p = pieces(&[seg(0), seg(2), seg(4), seg(100), seg(102)]);
        assert_eq!(
            p,
            vec![
                Piece { start_us: 0, end_us: 6_000_000, offset_us: 0 },
                Piece { start_us: 100_000_000, end_us: 104_000_000, offset_us: 6_000_000 },
            ]
        );
    }

    fn stream(kind: &str, exe: &str, start_s: i64) -> AudioStream {
        AudioStream {
            dir: PathBuf::from("d"),
            meta: AudioMeta { kind: kind.into(), exe: exe.into(), pid: 1, start_us: start_s * 1_000_000 },
            segs: vec![AudioSeg { path: PathBuf::from("a"), start_us: 0, end_us: None, size: 1 }],
        }
    }

    #[test]
    fn track_layout_is_fixed() {
        let cfg = Config::default();
        let streams =
            vec![stream("app", "brave.exe", 0), stream("app", "discord.exe", 0), stream("mic", "mic", 0), stream("app", "brave.exe", 5)];
        let t = tracks(&cfg, &streams, &[]);
        let titles: Vec<&str> = t.iter().map(|t| t.title.as_str()).collect();
        assert_eq!(titles, ["Mic", "Communication", "Brave"]);
        assert_eq!(t[0].streams, [2]);
        assert_eq!(t[1].streams, [1]);
        assert_eq!(t[2].streams, [0, 3], "both Brave streams on one track");

        // Nothing playing still gives Mic and Communication.
        assert_eq!(tracks(&cfg, &[], &[]).len(), 2);
    }

    #[test]
    fn graph_places_audio_on_the_video_timeline() {
        let pieces = vec![
            Piece { start_us: 10_000_000, end_us: 20_000_000, offset_us: 0 },
            Piece { start_us: 50_000_000, end_us: 60_000_000, offset_us: 10_000_000 },
        ];
        // Mic started at 5 s and is still going: used in both pieces.
        let streams = vec![stream("mic", "mic", 5)];
        let tracks = vec![Track { title: "Mic".into(), streams: vec![0] }, Track { title: "Communication".into(), streams: vec![] }];
        let g = audio_graph(&pieces, &streams, &tracks, 20_000_000);
        assert!(g.contains("[1:a]asplit=2[s0_0][s0_1];"), "{g}");
        assert!(g.contains("[s0_0]atrim=start=5.000000:end=15.000000"), "{g}");
        assert!(g.contains("adelay=0S"), "{g}");
        assert!(g.contains("[s0_1]atrim=start=45.000000:end=55.000000"), "{g}");
        assert!(g.contains("adelay=480000S"), "second piece starts 10 s into the clip: {g}");
        assert!(g.contains("anullsrc=r=48000:cl=stereo,atrim=duration=20.000000[t1]"), "{g}");
        assert!(g.ends_with("[mix]"));
    }
}
