//! Which track an app's audio goes on. Communication apps and ignored apps are clipback's own lists;
//! the game list is lifelog's (found from Steam, Epic, … plus your own choices), so both tools agree.

use crate::config::Config;
use serde::Serialize;
use std::collections::BTreeMap;
use std::sync::Mutex;
use std::time::{Duration, Instant};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize)]
#[serde(rename_all = "lowercase")]
pub enum Category {
    Communication,
    Game,
    App,
    Ignored,
}

/// lifelog's game list, re-read at most every 10 s.
static GAMES: Mutex<Option<(Instant, Vec<String>, Vec<String>)>> = Mutex::new(None);

fn lifelog_toml() -> Option<toml::Table> {
    let text = std::fs::read_to_string(lifelog::config::config_path()).ok()?;
    text.parse::<toml::Table>().ok()
}

fn list(table: &toml::Table, section: &str, key: &str) -> Vec<String> {
    table
        .get(section)
        .and_then(|s| s.get(key))
        .and_then(|v| v.as_array())
        .map(|a| a.iter().filter_map(|v| v.as_str()).map(|s| s.to_lowercase()).collect())
        .unwrap_or_default()
}

fn lifelog_port() -> u16 {
    lifelog_toml().and_then(|t| t.get("api_port").and_then(|v| v.as_integer())).unwrap_or(47800) as u16
}

/// Game names from lifelog's scan of the launchers (exe → "Counter-Strike 2"), asked for at most
/// every 10 minutes. Empty if lifelog isn't running.
static NAMES: Mutex<Option<(Instant, BTreeMap<String, String>)>> = Mutex::new(None);

fn game_names() -> BTreeMap<String, String> {
    if let Some((at, names)) = NAMES.lock().unwrap().as_ref() {
        if at.elapsed() < Duration::from_secs(600) {
            return names.clone();
        }
    }
    let mut names = BTreeMap::new();
    if let Ok(body) = lifelog::api::call(lifelog_port(), "GET", "/api/installed") {
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(&body) {
            for g in v["games"].as_array().into_iter().flatten() {
                let Some(name) = g["name"].as_str() else { continue };
                for exe in g["exes"].as_array().into_iter().flatten().filter_map(|e| e.as_str()) {
                    names.entry(exe.to_lowercase()).or_insert_with(|| name.to_string());
                }
            }
        }
    }
    *NAMES.lock().unwrap() = Some((Instant::now(), names.clone()));
    names
}

/// A readable name for an app: the launcher's game name if known, else "Overwatch" from overwatch.exe.
pub fn display_name(exe: &str) -> String {
    let exe = exe.to_lowercase();
    if let Some(name) = game_names().get(&exe) {
        return clean(name);
    }
    let stem = exe.trim_end_matches(".exe");
    let mut chars = stem.chars();
    match chars.next() {
        Some(c) => c.to_uppercase().chain(chars).collect(),
        None => stem.to_string(),
    }
}

/// Safe in a file name.
fn clean(name: &str) -> String {
    name.chars().filter(|c| !r#"<>:"/\|?*"#.contains(*c) && !c.is_control()).collect::<String>().trim().to_string()
}

/// (games, not games) from lifelog's settings.
pub fn games() -> (Vec<String>, Vec<String>) {
    let mut cache = GAMES.lock().unwrap();
    if let Some((at, games, not)) = cache.as_ref() {
        if at.elapsed() < Duration::from_secs(10) {
            return (games.clone(), not.clone());
        }
    }
    let table = lifelog_toml().unwrap_or_default();
    let (games, not) = (list(&table, "processing", "games"), list(&table, "processing", "not_games"));
    *cache = Some((Instant::now(), games.clone(), not.clone()));
    (games, not)
}

pub fn category(cfg: &Config, exe: &str) -> Category {
    let exe = exe.to_lowercase();
    if cfg.audio.ignored_apps.contains(&exe) {
        Category::Ignored
    } else if cfg.audio.communication_apps.contains(&exe) {
        Category::Communication
    } else if games().0.contains(&exe) {
        Category::Game
    } else {
        Category::App
    }
}

fn encode(exe: &str) -> String {
    exe.replace('%', "%25").replace(' ', "%20").replace('&', "%26").replace('+', "%2B")
}

/// Marks an app as a game or not, in lifelog's list. Goes through the running lifelog service
/// so its own copy stays right; edits its settings file directly if it isn't running.
fn set_game(exe: &str, game: bool) -> anyhow::Result<()> {
    let port = lifelog_port();
    let path = format!("/{}?app={}", if game { "game" } else { "not-game" }, encode(exe));
    if lifelog::api::call(port, "POST", &path).is_err() {
        let (mut games, mut not) = games();
        games.retain(|g| g != exe);
        not.retain(|g| g != exe);
        if game {
            games.push(exe.to_string());
        } else {
            not.push(exe.to_string());
        }
        games.sort();
        not.sort();
        lifelog::config::save_list("processing", "games", &games)?;
        lifelog::config::save_list("processing", "not_games", &not)?;
    }
    *GAMES.lock().unwrap() = None;
    Ok(())
}

/// Puts an app in one category (removing it from the others).
pub fn set_category(exe: &str, category: Category) -> anyhow::Result<()> {
    let exe = exe.trim().to_lowercase();
    if exe.is_empty() {
        return Ok(());
    }
    let mut cfg = crate::state::cfg();
    cfg.audio.communication_apps.retain(|a| *a != exe);
    cfg.audio.ignored_apps.retain(|a| *a != exe);
    match category {
        Category::Communication => cfg.audio.communication_apps.push(exe.clone()),
        Category::Ignored => cfg.audio.ignored_apps.push(exe.clone()),
        _ => {}
    }
    cfg.audio.communication_apps.sort();
    cfg.audio.ignored_apps.sort();
    let is_game = games().0.contains(&exe);
    if (category == Category::Game) != is_game {
        set_game(&exe, category == Category::Game)?;
    }
    crate::state::set_cfg(cfg)
}

// ---- Apps seen playing audio, for the Options page ----

fn seen_path() -> std::path::PathBuf {
    crate::config::local_dir().join("seen_apps.txt")
}

static SEEN: Mutex<Option<BTreeMap<String, i64>>> = Mutex::new(None);

fn with_seen<T>(f: impl FnOnce(&mut BTreeMap<String, i64>) -> T) -> T {
    let mut guard = SEEN.lock().unwrap();
    let map = guard.get_or_insert_with(|| {
        std::fs::read_to_string(seen_path())
            .unwrap_or_default()
            .lines()
            .filter_map(|l| l.split_once('\t'))
            .filter_map(|(exe, t)| Some((exe.to_string(), t.parse().ok()?)))
            .collect()
    });
    f(map)
}

/// Notes that an app played audio (saved at most once a minute per app).
pub fn note_seen(exe: &str) {
    let now = crate::util::now_us();
    let changed = with_seen(|map| {
        let old = map.insert(exe.to_string(), now);
        old.is_none_or(|t| now - t > 60_000_000)
    });
    if changed {
        let text: String = with_seen(|map| map.iter().map(|(e, t)| format!("{e}\t{t}\n")).collect());
        let _ = std::fs::create_dir_all(crate::config::local_dir());
        let _ = std::fs::write(seen_path(), text);
    }
}

#[derive(Serialize)]
pub struct AppRow {
    pub exe: String,
    pub category: Category,
    /// Last time it played audio (µs), if ever.
    pub last_heard: Option<i64>,
}

/// Every app clipback knows about: heard ones, plus everything in the lists.
pub fn rows(cfg: &Config) -> Vec<AppRow> {
    let mut map: BTreeMap<String, Option<i64>> = with_seen(|m| m.iter().map(|(e, t)| (e.clone(), Some(*t))).collect());
    for exe in cfg.audio.communication_apps.iter().chain(&cfg.audio.ignored_apps).chain(&games().0) {
        map.entry(exe.clone()).or_insert(None);
    }
    let mut rows: Vec<AppRow> =
        map.into_iter().map(|(exe, last_heard)| AppRow { category: category(cfg, &exe), exe, last_heard }).collect();
    rows.sort_by(|a, b| b.last_heard.cmp(&a.last_heard).then_with(|| a.exe.cmp(&b.exe)));
    rows
}
