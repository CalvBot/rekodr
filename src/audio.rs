//! Every app that plays sound gets its own capture stream (WASAPI process loopback, the app's whole
//! process tree), plus one for the mic. Each stream is encoded to Opus by its own small ffmpeg into
//! `ring\audio\<stream>`, timed against the wall clock from WASAPI's packet timestamps so it lines
//! up with the video to within a few ms.
//!
//! Which apps play, process trees and icons come from lifelog's watcher.

use crate::apps::{self, Category};
use crate::ring::AudioMeta;
use crate::state::{self, LiveAudio};
use crate::util::{self, log};
use anyhow::{Context, Result, anyhow};
use std::collections::{HashMap, VecDeque};
use std::io::Write;
use std::process::{Child, ChildStdin, Stdio};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::time::{Duration, Instant};
use wasapi::{AudioClient, DeviceEnumerator, Direction, SampleType, StreamMode, WaveFormat, initialize_mta};

const RATE: i64 = 48_000;
const CHANNELS: usize = 2;
const FRAME_BYTES: usize = CHANNELS * 4;
/// A stream is closed once its app has been silent this long.
const LINGER: Duration = Duration::from_secs(30);
/// Late audio is dropped and gaps are filled with silence beyond these, keeping the stream on time.
const GAP_US: i64 = 20_000;
const LATE_US: i64 = 40_000;

/// The editor window's browser process: its playback must never be recorded back into the buffer.
pub static OWN_BROWSER_PID: AtomicU32 = AtomicU32::new(0);

#[derive(Clone)]
enum Source {
    Mic,
    /// Root PID; child processes are included.
    App(u32),
}

/// Writes one stream's audio to its own ffmpeg (Opus, 10 s segments), keeping it on the wall clock.
struct Writer {
    meta: AudioMeta,
    child: Child,
    stdin: ChildStdin,
    frames: i64,
}

impl Writer {
    fn open(meta: AudioMeta) -> Result<Writer> {
        let cfg = state::cfg();
        let stamp = chrono::Local::now().format("%Y%m%d_%H%M%S_%3f");
        let dir = cfg.audio_ring().join(format!("{stamp}_{}_{}", util::file_safe(&meta.exe), meta.pid));
        std::fs::create_dir_all(&dir).with_context(|| format!("creating {}", dir.display()))?;
        std::fs::write(dir.join("meta.json"), serde_json::to_string(&meta)?)?;
        let mut child = util::command(state::ffmpeg())
            .args(["-hide_banner", "-loglevel", "error"])
            .args(["-f", "f32le", "-ar", "48000", "-ac", "2", "-i", "pipe:0"])
            .args(["-c:a", "libopus", "-b:a", "128k", "-application", "audio"])
            .args(["-f", "segment", "-segment_time", "10", "-segment_format", "matroska"])
            // Small clusters, flushed: a clip can read the segment still being written.
            .args(["-segment_format_options", "cluster_time_limit=1000", "-flush_packets", "1"])
            .args(["-reset_timestamps", "0", "-segment_list_type", "csv", "-segment_list"])
            .arg(dir.join("list.csv"))
            .arg(dir.join("a_%05d.mka"))
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(std::fs::File::create(dir.join("ffmpeg.log")).map(Stdio::from).unwrap_or(Stdio::null()))
            .spawn()
            .context("starting the audio encoder")?;
        util::job().adopt(&child);
        let stdin = child.stdin.take().expect("piped");
        Ok(Writer { meta, child, stdin, frames: 0 })
    }

    fn expected_us(&self) -> i64 {
        self.meta.start_us + self.frames * 1_000_000 / RATE
    }

    fn write(&mut self, bytes: &[u8]) -> Result<()> {
        self.stdin.write_all(bytes).context("audio encoder stopped")?;
        self.frames += (bytes.len() / FRAME_BYTES) as i64;
        Ok(())
    }

    fn silence(&mut self, frames: i64) -> Result<()> {
        let zeros = vec![0u8; (frames.max(0) as usize) * FRAME_BYTES];
        self.write(&zeros)
    }

    /// Adds a packet that started at `t_us`: pads a gap before it, or trims it if it overlaps.
    fn place(&mut self, bytes: &[u8], t_us: i64) -> Result<()> {
        let diff = t_us - self.expected_us();
        let mut bytes = bytes;
        if diff > GAP_US {
            self.silence(diff * RATE / 1_000_000)?;
        } else if diff < -LATE_US {
            let drop = ((-diff * RATE / 1_000_000) as usize * FRAME_BYTES).min(bytes.len());
            bytes = &bytes[drop..];
        }
        self.write(bytes)
    }

    /// Nothing arrived for a while: fill with silence up to shortly before now.
    fn idle(&mut self) -> Result<()> {
        let behind = util::now_us() - 150_000 - self.expected_us();
        if behind > 300_000 {
            self.silence(behind * RATE / 1_000_000)?;
        }
        Ok(())
    }

    fn finish(self) {
        let Writer { mut child, stdin, .. } = self;
        drop(stdin);
        util::wait_or_kill(&mut child, 10);
    }
}

enum Next {
    Stop,
    /// The default mic changed: capture from the new one.
    Restart,
}

fn default_mic_id() -> Option<String> {
    DeviceEnumerator::new().ok()?.get_default_device(Direction::Capture).ok()?.get_id().ok()
}

fn capture(source: &Source, exe: &str, stop: &AtomicBool) -> Result<Next> {
    let err = |e: wasapi::WasapiError| anyhow!("{e}");
    let format = WaveFormat::new(32, 32, SampleType::Float, RATE as usize, CHANNELS, None);
    let mut mic_id = None;
    let (mut client, kind, pid) = match source {
        Source::Mic => {
            let device = DeviceEnumerator::new().map_err(err)?.get_default_device(Direction::Capture).map_err(err)?;
            log(&format!("mic: {}", device.get_friendlyname().unwrap_or_else(|_| "default device".into())));
            mic_id = device.get_id().ok();
            (device.get_iaudioclient().map_err(err)?, "mic", 0)
        }
        Source::App(pid) => (AudioClient::new_application_loopback_client(*pid, true).map_err(err)?, "app", *pid),
    };
    let buffer_hns = if mic_id.is_some() { 2_000_000 } else { 0 };
    let mode = StreamMode::EventsShared { autoconvert: true, buffer_duration_hns: buffer_hns };
    client.initialize_client(&format, Direction::Capture, mode, None).map_err(err)?;
    let event = client.set_get_eventhandle().map_err(err)?;
    let capture = client.get_audiocaptureclient().map_err(err)?;
    client.start_stream().map_err(err)?;

    let mut writer: Option<Writer> = None;
    let mut bytes: VecDeque<u8> = VecDeque::new();
    let mut packet: Vec<u8> = Vec::new();
    let mut checked = Instant::now();
    let result = (|| -> Result<Next> {
        while !stop.load(Ordering::Relaxed) {
            if mic_id.is_some() && checked.elapsed() >= Duration::from_secs(3) {
                checked = Instant::now();
                if default_mic_id().is_some_and(|now| Some(&now) != mic_id.as_ref()) {
                    log("the default mic changed; switching");
                    return Ok(Next::Restart);
                }
            }
            let got_event = event.wait_for_event(200).is_ok();
            let mut any = false;
            while capture.get_next_packet_size().map_err(err)?.unwrap_or(0) > 0 {
                let before = bytes.len();
                let info = capture.read_from_device_to_deque(&mut bytes).map_err(err)?;
                let new = bytes.len() - before;
                packet.clear();
                packet.extend(bytes.drain(..));
                if info.flags.silent {
                    packet.iter_mut().for_each(|b| *b = 0);
                }
                debug_assert_eq!(packet.len(), new);
                let frames = (packet.len() / FRAME_BYTES) as i64;
                let t_us = if info.timestamp > 0 && !info.flags.timestamp_error {
                    util::qpc_to_wall_us(info.timestamp)
                } else {
                    util::now_us() - frames * 1_000_000 / RATE
                };
                if writer.is_none() {
                    let meta = AudioMeta { kind: kind.into(), exe: exe.into(), pid, start_us: t_us };
                    writer = Some(Writer::open(meta)?);
                }
                writer.as_mut().unwrap().place(&packet, t_us)?;
                any = true;
            }
            if !any && !got_event {
                if let Some(w) = writer.as_mut() {
                    w.idle()?;
                }
            }
        }
        Ok(Next::Stop)
    })();
    client.stop_stream().ok();
    if let Some(w) = writer {
        w.finish();
    }
    result
}

struct Stream {
    exe: String,
    since_us: i64,
    stop: Arc<AtomicBool>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl Stream {
    fn start(source: Source, exe: String) -> Stream {
        let stop = Arc::new(AtomicBool::new(false));
        let (s, name) = (stop.clone(), exe.clone());
        let thread = std::thread::Builder::new()
            .name(format!("audio-{exe}"))
            .spawn(move || {
                let _ = initialize_mta();
                while !s.load(Ordering::Relaxed) {
                    match capture(&source, &name, &s) {
                        Ok(Next::Stop) => break,
                        Ok(Next::Restart) => {}
                        Err(e) => {
                            log(&format!("{name}: audio capture stopped: {e:#}"));
                            if matches!(source, Source::App(_)) {
                                break; // the watcher starts a new stream if the app plays again
                            }
                            let until = Instant::now() + Duration::from_secs(5);
                            while Instant::now() < until && !s.load(Ordering::Relaxed) {
                                std::thread::sleep(Duration::from_millis(100));
                            }
                        }
                    }
                }
            })
            .expect("spawn audio thread");
        Stream { exe, since_us: util::now_us(), stop, thread: Some(thread) }
    }

    fn finished(&self) -> bool {
        self.thread.as_ref().is_none_or(|t| t.is_finished())
    }
}

impl Drop for Stream {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
    }
}

struct Running {
    stream: Stream,
    last_playing: Instant,
}

fn is_own(exe: &str) -> bool {
    exe.starts_with("clipback")
}

/// Is `pid` our own editor window's browser (or one of its children)?
fn in_own_browser(pid: u32, procs: &HashMap<u32, lifelog::watcher::Proc>) -> bool {
    let own = OWN_BROWSER_PID.load(Ordering::Relaxed);
    own != 0 && (pid == own || lifelog::watcher::root_of(pid, procs) == own)
}

pub fn track_name(cfg: &crate::config::Config, exe: &str) -> String {
    if exe == "mic" {
        return "Mic".into();
    }
    match apps::category(cfg, exe) {
        Category::Communication => "Communication".into(),
        _ => exe.into(),
    }
}

/// Runs until clipback quits.
pub fn run() {
    let _ = initialize_mta();
    let own_pid = std::process::id();
    let mut mic: Option<Stream> = None;
    let mut streams: HashMap<u32, Running> = HashMap::new();
    while !state::stopping() {
        let cfg = state::cfg();
        if state::paused() {
            mic = None;
            streams.clear();
            state::LIVE_AUDIO.lock().unwrap().clear();
            std::thread::sleep(Duration::from_millis(500));
            continue;
        }
        if cfg.audio.mic && mic.as_ref().is_none_or(Stream::finished) {
            mic = Some(Stream::start(Source::Mic, "mic".into()));
        } else if !cfg.audio.mic {
            mic = None;
        }

        let procs = lifelog::watcher::processes();
        lifelog::apps::note_processes(procs.iter().map(|(pid, p)| (*pid, p.exe.as_str())));
        let now = Instant::now();
        for pid in lifelog::watcher::playing_pids() {
            // PID 0 is "system sounds".
            if pid == 0 || pid == own_pid {
                continue;
            }
            let Some(proc) = procs.get(&pid) else { continue };
            let exe = proc.exe.clone();
            if is_own(&exe) || in_own_browser(pid, &procs) || apps::category(&cfg, &exe) == Category::Ignored {
                continue;
            }
            let root = lifelog::watcher::root_of(pid, &procs);
            if let Some(running) = streams.get_mut(&root) {
                running.last_playing = now;
                continue;
            }
            log(&format!("recording {exe} audio (pid {root})"));
            apps::note_seen(&exe);
            lifelog::apps::keep_icons(std::iter::once(exe.as_str()));
            streams.insert(root, Running { stream: Stream::start(Source::App(root), exe), last_playing: now });
        }
        streams.retain(|pid, running| {
            let exe = &running.stream.exe;
            let alive = procs.get(pid).is_some_and(|p| &p.exe == exe);
            let ignored = apps::category(&cfg, exe) == Category::Ignored;
            let keep = alive && !ignored && !running.stream.finished() && now.duration_since(running.last_playing) < LINGER;
            if !keep {
                log(&format!("{exe} audio stream closed"));
            }
            keep
        });

        let mut live: Vec<LiveAudio> = mic
            .iter()
            .map(|m| LiveAudio { exe: "mic".into(), track: "Mic".into(), since_us: m.since_us })
            .collect();
        live.extend(streams.values().map(|r| LiveAudio {
            exe: r.stream.exe.clone(),
            track: track_name(&cfg, &r.stream.exe),
            since_us: r.stream.since_us,
        }));
        *state::LIVE_AUDIO.lock().unwrap() = live;
        std::thread::sleep(Duration::from_secs(1));
    }
    drop(mic);
    streams.clear();
}
