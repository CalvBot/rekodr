//! clipback from a terminal: `clipback` runs it here (with a log in the console); the other
//! commands talk to the running copy.

use anyhow::{Result, bail};
use clipback::config::{self, Config, format_duration, parse_duration};

const HELP: &str = "clipback: hands-off long replay buffer

  clipback              run (tray icon, hotkeys, recording) with a console log
  clipback save 2m      save the last 2 minutes (30s, 10m, 1h …) from the running copy
  clipback pause|resume pause or resume recording
  clipback open         open the clips window
  clipback quit         stop the running copy
  clipback status       what's in the buffer
  clipback config       print the settings file's path";

fn main() {
    if let Err(e) = real_main() {
        eprintln!("error: {e:#}");
        std::process::exit(1);
    }
}

fn call(method: &str, path: &str) -> Result<serde_json::Value> {
    let port = Config::load()?.port;
    let body = lifelog::api::call(port, method, path)
        .map_err(|_| anyhow::anyhow!("clipback isn't running (start it with `clipback`)"))?;
    let v: serde_json::Value = serde_json::from_str(&body).unwrap_or(serde_json::Value::Null);
    if let Some(e) = v["error"].as_str() {
        bail!("{e}");
    }
    Ok(v)
}

fn real_main() -> Result<()> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(String::as_str) {
        None | Some("run") => clipback::run_service(),
        Some("save") => {
            let secs = parse_duration(args.get(1).map(String::as_str).unwrap_or("30s"))?;
            let v = call("POST", &format!("/api/save?secs={secs}"))?;
            println!("{}", v["path"].as_str().unwrap_or("saved"));
            if let Some(t) = v["tracks"].as_array() {
                let names: Vec<&str> = t.iter().filter_map(|t| t.as_str()).collect();
                println!("audio tracks: {}", names.join(", "));
            }
            Ok(())
        }
        Some(cmd @ ("pause" | "resume" | "quit")) => call("POST", &format!("/api/{cmd}")).map(|_| ()),
        Some("open") => {
            call("GET", "/api/status")?;
            let port = Config::load()?.port;
            let _ = std::process::Command::new("rundll32").args(["url.dll,FileProtocolHandler", &format!("http://127.0.0.1:{port}/")]).spawn();
            Ok(())
        }
        Some("status") => {
            let v = call("GET", "/api/status")?;
            let state = if v["paused"] == true { "paused" } else if v["live"] == true { "recording" } else { "waiting for the screen" };
            println!("State:   {state}");
            println!("Buffer:  {:.1} GB of {:.0} GB", v["ring_bytes"].as_f64().unwrap_or(0.0) / 1e9, v["ring_cap"].as_f64().unwrap_or(0.0) / 1e9);
            if let (Some(old), Some(now)) = (v["oldest_us"].as_i64(), v["now_us"].as_i64()) {
                println!("Footage: last {}", format_duration(((now - old) / 1_000_000).max(0) as u64));
            }
            for a in v["audio"].as_array().into_iter().flatten() {
                println!("Audio:   {} → {}", a["exe"].as_str().unwrap_or("?"), a["track"].as_str().unwrap_or("?"));
            }
            Ok(())
        }
        Some("config") => {
            println!("{}", config::config_path().display());
            Ok(())
        }
        Some("-h" | "--help" | "help") => {
            println!("{HELP}");
            Ok(())
        }
        Some(other) => bail!("unknown command {other:?}\n\n{HELP}"),
    }
}
