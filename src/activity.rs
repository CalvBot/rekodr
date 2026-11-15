//! Which game is being played, over time. Every couple of seconds: the game whose window is in
//! front, else the last one played if it's still running, else any running game, else the desktop.
//! Changes go to `activity.tsv` in the buffer (one "µs<TAB>exe" a line, empty exe = desktop), so a
//! saved clip can be named and tagged after the game it shows, and the bitrate can follow the game.

use crate::apps::{self, Category};
use crate::state;
use crate::util::{log, now_us};
use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::Duration;

const POLL: Duration = Duration::from_secs(2);

/// The game being played now (lower-case exe), or None on the desktop.
static CURRENT: Mutex<Option<String>> = Mutex::new(None);

pub fn current() -> Option<String> {
    CURRENT.lock().unwrap().clone()
}

fn file(buffer_dir: &Path) -> PathBuf {
    buffer_dir.join("activity.tsv")
}

fn foreground_pid() -> u32 {
    use windows_sys::Win32::UI::WindowsAndMessaging::{GetForegroundWindow, GetWindowThreadProcessId};
    let mut pid = 0;
    unsafe {
        let hwnd = GetForegroundWindow();
        if !hwnd.is_null() {
            GetWindowThreadProcessId(hwnd, &mut pid);
        }
    }
    pid
}

/// The game to call "being played", given what's running and what was before.
fn pick(cfg: &crate::config::Config, prev: Option<&str>) -> Option<String> {
    let procs = lifelog::watcher::processes();
    let is_game = |exe: &str| apps::category(cfg, exe) == Category::Game;
    if let Some(p) = procs.get(&foreground_pid()) {
        if is_game(&p.exe) {
            return Some(p.exe.clone());
        }
    }
    let mut running: Vec<&str> = procs.values().map(|p| p.exe.as_str()).filter(|e| is_game(e)).collect();
    running.sort();
    running.dedup();
    match prev {
        Some(p) if running.contains(&p) => Some(p.to_string()),
        _ => running.first().map(|s| s.to_string()),
    }
}

fn append(path: &Path, us: i64, exe: &str) {
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "{us}\t{exe}");
    }
}

/// Runs until clipback quits.
pub fn run() {
    let mut last: Option<Option<String>> = None;
    let mut last_dir = PathBuf::new();
    while !state::stopping() {
        let cfg = state::cfg();
        let now = pick(&cfg, last.as_ref().and_then(|l| l.as_deref()));
        let path = file(&cfg.buffer_dir);
        // A fresh line on start (and if the buffer moves) so the file always says what was on.
        if last.as_ref() != Some(&now) || path != last_dir {
            append(&path, now_us(), now.as_deref().unwrap_or(""));
            if last.is_some() {
                log(&match &now {
                    Some(g) => format!("playing {}", apps::display_name(g)),
                    None => "on the desktop".into(),
                });
            }
            *CURRENT.lock().unwrap() = now.clone();
            last = Some(now);
            last_dir = path;
        }
        std::thread::sleep(POLL);
    }
}

fn read(buffer_dir: &Path) -> Vec<(i64, String)> {
    let text = std::fs::read_to_string(file(buffer_dir)).unwrap_or_default();
    text.lines()
        .filter_map(|l| l.split_once('\t'))
        .filter_map(|(t, e)| Some((t.parse().ok()?, e.to_string())))
        .collect()
}

/// Seconds spent on each game within [from, to], most first. Desktop time isn't counted.
pub fn games_in(buffer_dir: &Path, from: i64, to: i64) -> Vec<(String, f64)> {
    let entries = read(buffer_dir);
    let mut time: HashMap<String, i64> = HashMap::new();
    for (i, (start, exe)) in entries.iter().enumerate() {
        let end = entries.get(i + 1).map(|e| e.0).unwrap_or(i64::MAX);
        let overlap = end.min(to) - (*start).max(from);
        if !exe.is_empty() && overlap > 0 {
            *time.entry(exe.clone()).or_default() += overlap;
        }
    }
    let mut out: Vec<(String, f64)> = time.into_iter().map(|(e, us)| (e, us as f64 / 1e6)).collect();
    out.sort_by(|a, b| b.1.total_cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
    out
}

/// Drops lines older than the buffer, keeping the one that says what was on at its start.
pub fn prune(buffer_dir: &Path, oldest_us: i64) {
    let entries = read(buffer_dir);
    let keep_from = entries.iter().rposition(|(t, _)| *t <= oldest_us).unwrap_or(0);
    if keep_from == 0 {
        return;
    }
    let text: String = entries[keep_from..].iter().map(|(t, e)| format!("{t}\t{e}\n")).collect();
    let _ = std::fs::write(file(buffer_dir), text);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn time_per_game() {
        let dir = std::env::temp_dir().join(format!("clipback-activity-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(file(&dir), "0\t\n10000000\tcs2.exe\n40000000\t\n50000000\toverwatch.exe\n").unwrap();
        let g = games_in(&dir, 5_000_000, 55_000_000);
        assert_eq!(g, vec![("cs2.exe".to_string(), 30.0), ("overwatch.exe".to_string(), 5.0)]);
        assert!(games_in(&dir, 0, 9_000_000).is_empty()); // desktop only
        prune(&dir, 45_000_000);
        assert_eq!(read(&dir).first().map(|e| e.0), Some(40_000_000));
        let _ = std::fs::remove_dir_all(&dir);
    }
}
