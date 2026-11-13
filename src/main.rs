mod config;
mod export;
mod hotkey;
mod recorder;
mod ring;

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use config::{format_duration, parse_duration, Config};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Duration;

#[derive(Parser)]
#[command(version, about = "Hands-off long replay buffer")]
struct Cli {
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Record the screen into the rolling buffer and listen for hotkeys (leave this running)
    Run,
    /// Save the last N seconds/minutes/hours as an MP4, e.g. `clipback save 2m`
    Save { duration: String },
    /// Show how much is in the buffer
    Status,
    /// Print the settings file location
    Config,
}

pub fn log(msg: &str) {
    println!("[{}] {msg}", chrono::Local::now().format("%H:%M:%S"));
}

fn main() {
    if let Err(e) = real_main() {
        eprintln!("error: {e:#}");
        std::process::exit(1);
    }
}

fn real_main() -> Result<()> {
    let cli = Cli::parse();
    if let Cmd::Config = cli.command {
        println!("{}", config::config_path().display());
        return Ok(());
    }
    let cfg = Config::load()?;
    match cli.command {
        Cmd::Run => run(cfg),
        Cmd::Save { duration } => {
            let secs = parse_duration(&duration)?;
            let out = export::save_last(&cfg, &cfg.find_ffmpeg()?, secs)?;
            println!("{}", out.display());
            Ok(())
        }
        Cmd::Status => status(&cfg),
        Cmd::Config => unreachable!(),
    }
}

/// True if another `clipback run` already holds the instance lock. The lock lives as long as this process.
fn already_running() -> bool {
    use windows_sys::Win32::Foundation::{GetLastError, ERROR_ALREADY_EXISTS};
    use windows_sys::Win32::System::Threading::CreateMutexW;
    let name: Vec<u16> = "Local\\clipback-recorder\0".encode_utf16().collect();
    unsafe {
        let handle = CreateMutexW(std::ptr::null(), 0, name.as_ptr());
        handle.is_null() || GetLastError() == ERROR_ALREADY_EXISTS
    }
}

fn run(cfg: Config) -> Result<()> {
    if already_running() {
        bail!("clipback is already running");
    }
    let ffmpeg = cfg.find_ffmpeg()?;
    std::fs::create_dir_all(&cfg.ring_dir).with_context(|| format!("creating {}", cfg.ring_dir.display()))?;
    log(&format!(
        "buffer: {} (cap {} GB, about {:.0} h at {} Mbps) · clips: {}",
        cfg.ring_dir.display(),
        cfg.max_ring_gb,
        cfg.max_ring_gb * 8000.0 / cfg.bitrate_mbps / 3600.0,
        cfg.bitrate_mbps,
        cfg.save_dir.display()
    ));

    let cfg = Arc::new(cfg);
    let stop = Arc::new(AtomicBool::new(false));
    let main_thread = hotkey::current_thread_id();
    {
        let stop = stop.clone();
        ctrlc::set_handler(move || {
            stop.store(true, Ordering::Relaxed);
            hotkey::quit(main_thread);
        })
        .context("installing Ctrl+C handler")?;
    }

    let recorder = {
        let (cfg, ffmpeg, stop) = (cfg.clone(), ffmpeg.clone(), stop.clone());
        std::thread::spawn(move || recorder::run(&cfg, &ffmpeg, &stop))
    };

    // Pruner: keeps the ring under its cap. Detached; it only ever deletes whole old segments.
    {
        let (cfg, stop) = (cfg.clone(), stop.clone());
        std::thread::spawn(move || {
            while !stop.load(Ordering::Relaxed) {
                if let Err(e) = ring::prune(&cfg.ring_dir, cfg.max_ring_bytes()) {
                    log(&format!("pruning failed: {e:#}"));
                }
                std::thread::sleep(Duration::from_secs(20));
            }
        });
    }

    for h in &cfg.hotkey {
        log(&format!("hotkey {} saves the last {}", h.keys, h.save));
    }
    let keys: Vec<String> = cfg.hotkey.iter().map(|h| h.keys.clone()).collect();
    hotkey::run_loop(&keys, |i| {
        let Some(secs) = cfg.hotkey.get(i).and_then(|h| parse_duration(&h.save).ok()) else { return };
        let (cfg, ffmpeg) = (cfg.clone(), ffmpeg.clone());
        std::thread::spawn(move || match export::save_last(&cfg, &ffmpeg, secs) {
            Ok(path) => {
                log(&format!("saved {}", path.display()));
                hotkey::beep(true);
            }
            Err(e) => {
                log(&format!("save failed: {e:#}"));
                hotkey::beep(false);
            }
        });
    });

    log("stopping");
    stop.store(true, Ordering::Relaxed);
    let _ = recorder.join();
    Ok(())
}

fn status(cfg: &Config) -> Result<()> {
    let segs = ring::list(&cfg.ring_dir)?;
    let (Some(first), Some(last)) = (segs.first(), segs.last()) else {
        println!("Buffer at {} is empty.", cfg.ring_dir.display());
        return Ok(());
    };
    let bytes: u64 = segs.iter().map(|s| s.size).sum();
    let recorded = segs.len() as u64 * cfg.segment_secs as u64;
    let live = (chrono::Local::now() - last.start).num_seconds() < cfg.segment_secs as i64 + 5;
    println!("Recording:  {}", if live { "yes" } else { "no (newest segment is old)" });
    println!("Buffer:     {:.2} GB of {} GB in {} segments", bytes as f64 / 1e9, cfg.max_ring_gb, segs.len());
    println!("Footage:    {}", format_duration(recorded));
    println!("Oldest:     {}", first.start.format("%Y-%m-%d %H:%M:%S"));
    println!("Newest:     {}", last.start.format("%Y-%m-%d %H:%M:%S"));
    Ok(())
}
