//! Commands sent to the running copy over its local API: what `clipback save 2m` and
//! `clipbackw.exe save 2m` (a Stream Deck "Open" key, no console) both use.

use crate::config::{Config, parse_duration};
use anyhow::{Result, anyhow, bail};
use serde_json::Value;

pub fn call(method: &str, path: &str) -> Result<Value> {
    let port = Config::load()?.port;
    let body = lifelog::api::call(port, method, path).map_err(|_| anyhow!("clipback isn't running"))?;
    let v: Value = serde_json::from_str(&body).unwrap_or(Value::Null);
    if let Some(e) = v["error"].as_str() {
        bail!("{e}");
    }
    Ok(v)
}

/// Runs one of the simple commands. Returns None for a command this doesn't know.
pub fn command(cmd: &str, arg: Option<&str>) -> Option<Result<Value>> {
    Some(match cmd {
        "save" => parse_duration(arg.unwrap_or("30s")).and_then(|secs| call("POST", &format!("/api/save?secs={secs}"))),
        "open" => call("POST", "/api/open"),
        "options" => call("POST", "/api/open?page=options"),
        "pause" | "resume" | "toggle" | "quit" => call("POST", &format!("/api/{cmd}")),
        _ => return None,
    })
}
