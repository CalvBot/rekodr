//! The local web UI (clips, editor, options) on 127.0.0.1, shown in a small Edge app window.
//! Only pages served from here may call the API (Host and Origin are checked).

use crate::{apps, clips, render, ring, state, util};
use anyhow::{Context, Result, anyhow};
use serde_json::{Value, json};
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;
use std::sync::atomic::Ordering;
use tiny_http::{Header, Method, Request, Response, Server, StatusCode};

const INDEX: &str = include_str!("../ui/index.html");
const EDITOR: &str = include_str!("../ui/editor.html");
const OPTIONS: &str = include_str!("../ui/options.html");
const STYLE: &str = include_str!("../ui/style.css");
const COMMON: &str = include_str!("../ui/common.js");
const ICONS: &str = include_str!("../ui/icons.js");
const FAVICON: &[u8] = include_bytes!("../assets/favicon.png");

fn header(name: &str, value: &str) -> Header {
    Header::from_bytes(name.as_bytes(), value.as_bytes()).unwrap()
}

fn trusted(req: &Request, port: u16) -> bool {
    let ours = [format!("127.0.0.1:{port}"), format!("localhost:{port}")];
    let get = |name: &'static str| req.headers().iter().find(|h| h.field.equiv(name)).map(|h| h.value.as_str().to_string());
    let host_ok = get("Host").is_some_and(|h| ours.contains(&h));
    let origin_ok = get("Origin").is_none_or(|o| ours.iter().any(|h| o == format!("http://{h}")));
    host_ok && origin_ok
}

fn decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < bytes.len() && s.is_char_boundary(i + 3) => {
                match u8::from_str_radix(&s[i + 1..i + 3], 16) {
                    Ok(b) => {
                        out.push(b);
                        i += 2;
                    }
                    Err(_) => out.push(b'%'),
                }
            }
            b => out.push(b),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn query(url: &str, name: &str) -> Option<String> {
    let q = url.split_once('?')?.1;
    q.split('&').filter_map(|kv| kv.split_once('=')).find(|(k, _)| *k == name).map(|(_, v)| decode(v))
}

type Reply = (u16, &'static str, Vec<u8>);

fn ok_json(v: Value) -> Reply {
    (200, "application/json", v.to_string().into_bytes())
}

fn err_json(code: u16, e: impl std::fmt::Display) -> Reply {
    (code, "application/json", json!({ "error": e.to_string() }).to_string().into_bytes())
}

/// A file, with byte ranges so video seeking is instant.
fn send_file(req: Request, path: &Path, content_type: &str, cache: bool) {
    let Ok(mut file) = std::fs::File::open(path) else {
        let _ = req.respond(Response::from_string("not found").with_status_code(404));
        return;
    };
    let len = file.metadata().map(|m| m.len()).unwrap_or(0);
    let range = req
        .headers()
        .iter()
        .find(|h| h.field.equiv("Range"))
        .and_then(|h| h.value.as_str().strip_prefix("bytes=").map(str::to_string))
        .and_then(|r| {
            let (a, b) = r.split_once('-')?;
            let start: u64 = a.parse().ok()?;
            let end: u64 = if b.is_empty() { len.saturating_sub(1) } else { b.parse().ok()? };
            (start <= end && end < len).then_some((start, end))
        });
    let mut headers = vec![header("Content-Type", content_type), header("Accept-Ranges", "bytes")];
    if cache {
        headers.push(header("Cache-Control", "max-age=31536000, immutable"));
    }
    let (code, start, count) = match range {
        Some((s, e)) => {
            headers.push(header("Content-Range", &format!("bytes {s}-{e}/{len}")));
            (206, s, e - s + 1)
        }
        None => (200, 0, len),
    };
    if file.seek(SeekFrom::Start(start)).is_err() {
        return;
    }
    let body = file.take(count);
    let _ = req.respond(Response::new(StatusCode(code), headers, body, Some(count as usize), None));
}

/// Icons for the Options page: lifelog's cached 32×32 RGBA, as PNG.
fn icon_png(exe: &str) -> Option<Vec<u8>> {
    let rgba = lifelog::apps::icon_rgba(exe)?;
    Some(png(32, 32, &rgba))
}

fn png(w: u32, h: u32, rgba: &[u8]) -> Vec<u8> {
    fn crc(data: &[u8]) -> u32 {
        let mut c = 0xFFFF_FFFFu32;
        for &b in data {
            c ^= b as u32;
            for _ in 0..8 {
                c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 };
            }
        }
        !c
    }
    fn chunk(out: &mut Vec<u8>, kind: &[u8], data: &[u8]) {
        out.extend((data.len() as u32).to_be_bytes());
        let mut body = kind.to_vec();
        body.extend(data);
        out.extend(&body);
        out.extend(crc(&body).to_be_bytes());
    }
    let mut raw = Vec::with_capacity((w * h * 4 + h) as usize);
    for row in rgba.chunks((w * 4) as usize) {
        raw.push(0);
        raw.extend(row);
    }
    let mut out = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut ihdr = Vec::new();
    ihdr.extend(w.to_be_bytes());
    ihdr.extend(h.to_be_bytes());
    ihdr.extend([8, 6, 0, 0, 0]);
    chunk(&mut out, b"IHDR", &ihdr);
    chunk(&mut out, b"IDAT", &miniz_oxide::deflate::compress_to_vec_zlib(&raw, 6));
    chunk(&mut out, b"IEND", &[]);
    out
}

fn status() -> Value {
    let cfg = state::cfg();
    let segs = ring::video_segments(&cfg.video_ring());
    json!({
        "paused": state::paused(),
        "live": state::VIDEO_LIVE.load(Ordering::Relaxed),
        "oldest_us": segs.first().map(|s| s.start_us),
        "newest_us": segs.last().map(|s| s.end_us),
        "ring_bytes": state::RING_BYTES.load(Ordering::Relaxed),
        "ring_cap": cfg.max_ring_bytes(),
        "audio": *state::LIVE_AUDIO.lock().unwrap(),
        "clips_dir": cfg.clips_dir,
        "encode": *state::VIDEO_ENCODE.lock().unwrap(),
        "encoder": *state::VIDEO_ENCODER.lock().unwrap(),
        "new_apps": apps::new_count(&cfg),
        "now_us": util::now_us(),
    })
}

/// What each vendor's test encode found, the setting, and what it resolves to now.
fn encoders_json() -> Value {
    use crate::encoders::{self, ORDER};
    let video = state::cfg().video;
    let found = encoders::detected();
    let chosen = encoders::choose(&video.encoder, &video.codec, &found);
    json!({
        "setting": video.encoder,
        "codec": video.codec,
        "order": ORDER,
        "detected": found,
        "chosen": chosen.map(|v| json!({ "vendor": v, "name": v.encoder(&video.codec) })),
        "active": *state::VIDEO_ENCODER.lock().unwrap(),
    })
}

fn body_json(req: &mut Request) -> Result<Value> {
    let mut s = String::new();
    req.as_reader().read_to_string(&mut s)?;
    Ok(serde_json::from_str(&s)?)
}

fn handle(mut req: Request) {
    let port = state::cfg().port;
    if !trusted(&req, port) {
        let _ = req.respond(Response::from_string("not allowed").with_status_code(403));
        return;
    }
    let url = req.url().to_string();
    let path = url.split('?').next().unwrap_or("").to_string();
    let get = *req.method() == Method::Get;
    let id = query(&url, "id");

    // Files: streamed with ranges.
    if get {
        let file = match path.as_str() {
            "/media" => id.as_deref().map(|i| clips::resolve(i).map(|p| (p, "video/mp4", false))),
            "/api/sprite" => id.as_deref().map(|i| clips::resolve(i).and_then(|p| clips::sprite(&p)).map(|p| (p, "image/jpeg", true))),
            "/api/track" => id.as_deref().map(|i| {
                let n = query(&url, "i").and_then(|v| v.parse().ok()).unwrap_or(0);
                clips::resolve(i).and_then(|p| clips::track(&p, n)).map(|p| (p, "audio/mp4", true))
            }),
            _ => None,
        };
        if let Some(file) = file {
            match file {
                Ok((p, ct, cache)) => send_file(req, &p, ct, cache),
                Err(e) => {
                    let _ = req.respond(Response::from_string(format!("{e:#}")).with_status_code(404));
                }
            }
            return;
        }
    }

    let reply: Reply = match (get, path.as_str()) {
        (true, "/") => (200, "text/html; charset=utf-8", INDEX.into()),
        (true, "/editor") => (200, "text/html; charset=utf-8", EDITOR.into()),
        (true, "/options") => (200, "text/html; charset=utf-8", OPTIONS.into()),
        (true, "/style.css") => (200, "text/css; charset=utf-8", STYLE.into()),
        (true, "/common.js") => (200, "text/javascript; charset=utf-8", COMMON.into()),
        (true, "/icons.js") => (200, "text/javascript; charset=utf-8", ICONS.into()),
        (true, "/favicon.png") | (true, "/favicon.ico") => (200, "image/png", FAVICON.to_vec()),
        (true, "/api/status") => ok_json(status()),
        (true, "/api/clips") => ok_json(json!(clips::list())),
        (true, "/api/info") => match id.as_deref().map(clips::resolve).transpose().and_then(|p| p.ok_or(anyhow!("no id"))).and_then(|p| clips::info(&p)) {
            Ok(info) => ok_json(json!(info)),
            Err(e) => err_json(404, format!("{e:#}")),
        },
        (true, "/api/icon") => match query(&url, "exe").and_then(|e| icon_png(&e)) {
            Some(png) => (200, "image/png", png),
            None => (404, "text/plain", b"no icon".to_vec()),
        },
        (true, "/api/settings") => ok_json(json!({ "config": state::cfg(), "path": crate::config::config_path() })),
        (true, "/api/encoders") => ok_json(encoders_json()),
        (true, "/api/monitors") => ok_json(json!(crate::monitors::list())),
        (true, "/api/apps") => {
            let cfg = state::cfg();
            ok_json(json!({ "apps": apps::rows(&cfg), "live": *state::LIVE_AUDIO.lock().unwrap() }))
        }
        (true, "/api/job") => match query(&url, "job").and_then(|j| j.parse().ok()).and_then(render::job) {
            Some(job) => ok_json(json!(job)),
            None => err_json(404, "no such job"),
        },
        (false, "/api/settings") => match body_json(&mut req)
            .and_then(|v| serde_json::from_value::<crate::config::Config>(v).context("bad settings"))
            .and_then(state::set_cfg)
        {
            Ok(()) => ok_json(json!({ "ok": true })),
            Err(e) => err_json(400, format!("{e:#}")),
        },
        (false, "/api/app-reveal") => match query(&url, "exe").and_then(|e| lifelog::apps::path_of(&e)) {
            Some(p) => {
                util::reveal(&p);
                ok_json(json!({ "ok": true }))
            }
            None => err_json(404, "Not sure where that app is. It's found once it runs while clipback or lifelog is on."),
        },
        (false, "/api/app-category") => {
            let exe = query(&url, "exe").unwrap_or_default();
            let cat = match query(&url, "category").as_deref() {
                Some("communication") => Some(apps::Category::Communication),
                Some("game") => Some(apps::Category::Game),
                Some("app") => Some(apps::Category::App),
                Some("ignored") => Some(apps::Category::Ignored),
                _ => None,
            };
            match cat.ok_or(anyhow!("unknown category")).and_then(|c| apps::set_category(&exe, c)) {
                Ok(()) => ok_json(json!({ "ok": true })),
                Err(e) => err_json(400, format!("{e:#}")),
            }
        }
        (false, "/api/game-bitrate") => {
            // Saved as soon as it's typed, like the app categories. No value = use the normal bitrate.
            let exe = query(&url, "exe").unwrap_or_default().to_lowercase();
            let mbps = query(&url, "mbps").and_then(|m| m.parse::<f64>().ok()).filter(|m| *m > 0.0);
            let mut cfg = state::cfg();
            match mbps {
                Some(m) => cfg.video.game_bitrates.insert(exe, m),
                None => cfg.video.game_bitrates.remove(&exe),
            };
            match state::set_cfg(cfg) {
                Ok(()) => ok_json(json!({ "ok": true })),
                Err(e) => err_json(400, format!("{e:#}")),
            }
        }
        (false, "/api/pause") => {
            state::set_paused(true);
            ok_json(status())
        }
        (false, "/api/quit") => {
            crate::tray::quit();
            ok_json(json!({ "ok": true }))
        }
        (false, "/api/resume") => {
            state::set_paused(false);
            ok_json(status())
        }
        (false, "/api/toggle") => {
            state::set_paused(!state::paused());
            ok_json(status())
        }
        (false, "/api/open") => {
            let page = match query(&url, "page").as_deref() {
                Some("options") => "/options",
                _ => "/",
            };
            open_window(page);
            ok_json(json!({ "ok": true }))
        }
        (false, "/api/save") => {
            let secs = query(&url, "secs").and_then(|s| crate::config::parse_duration(&s).ok()).unwrap_or(30);
            match crate::export::save_last(secs) {
                Ok(saved) => {
                    crate::hotkey::beep(true);
                    crate::tray::notify("Clip saved", &saved.path.file_name().unwrap_or_default().to_string_lossy());
                    ok_json(json!({ "path": saved.path, "id": clips::id_of(&saved.path), "tracks": saved.tracks, "secs": saved.secs }))
                }
                Err(e) => {
                    crate::hotkey::beep(false);
                    err_json(500, format!("{e:#}"))
                }
            }
        }
        (false, "/api/copy") => match id.as_deref().map(clips::resolve).transpose().and_then(|p| p.ok_or(anyhow!("no id"))) {
            Ok(p) => match lifelog::clip::copy_file_to_clipboard(&p) {
                Ok(()) => ok_json(json!({ "ok": true })),
                Err(e) => err_json(500, format!("{e:#}")),
            },
            Err(e) => err_json(404, format!("{e:#}")),
        },
        (false, "/api/reveal") => match id.as_deref().map(clips::resolve).transpose().and_then(|p| p.ok_or(anyhow!("no id"))) {
            Ok(p) => {
                util::reveal(&p);
                ok_json(json!({ "ok": true }))
            }
            Err(e) => err_json(404, format!("{e:#}")),
        },
        (false, "/api/render") => match body_json(&mut req)
            .and_then(|v| serde_json::from_value::<render::Request>(v).context("bad request"))
            .and_then(render::start)
        {
            Ok(job) => ok_json(json!({ "job": job })),
            Err(e) => err_json(400, format!("{e:#}")),
        },
        _ => (404, "text/plain", b"not found".to_vec()),
    };
    let (code, ct, body) = reply;
    let mut resp = Response::from_data(body).with_status_code(code).with_header(header("Content-Type", ct));
    if ct.starts_with("text/html") || ct.starts_with("application/json") {
        resp = resp.with_header(header("Cache-Control", "no-store"));
    }
    let _ = req.respond(resp);
}

/// Serves on its own threads until clipback exits.
pub fn serve(port: u16) -> Result<()> {
    let server = Server::http(("127.0.0.1", port)).map_err(|e| anyhow!("UI port {port}: {e}"))?;
    std::thread::Builder::new().name("ui".into()).spawn(move || {
        for req in server.incoming_requests() {
            std::thread::spawn(move || handle(req));
        }
    })?;
    keep_window_icon(); // a window may still be open from before a restart
    Ok(())
}

fn edge() -> Option<std::path::PathBuf> {
    [r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe", r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"]
        .into_iter()
        .map(std::path::PathBuf::from)
        .find(|p| p.is_file())
}

/// clipback-ui's arguments for an editor page: "/editor?id=<clip>&t=<seconds>" gives
/// `--edit <clip> [--t <seconds>]`.
fn editor_args(page: &str) -> Option<Vec<String>> {
    if !page.starts_with("/editor?") {
        return None;
    }
    let id = query(page, "id").filter(|id| !id.is_empty())?;
    let mut args = vec!["--edit".to_string(), id];
    if let Some(t) = query(page, "t").and_then(|t| t.parse::<f64>().ok()).filter(|t| t.is_finite()) {
        args.extend(["--t".to_string(), t.to_string()]);
    }
    Some(args)
}

/// Opens a page of the UI in its own small window (Edge app mode with a separate profile, so the
/// editor's playback can be kept out of the recording), or the default browser.
pub fn open_window(page: &str) {
    // The native clips window, when it's installed next to us (PLAN.md "Native window app").
    // Options and the editor are still web pages until they're rebuilt there too.
    if page == "/" {
        let ui = std::env::current_exe().ok().and_then(|e| Some(e.parent()?.join("clipback-ui.exe")));
        if let Some(ui) = ui.filter(|p| p.is_file()) {
            if std::process::Command::new(&ui).spawn().is_ok() {
                return;
            }
        }
    }
    // The native editor too: "/editor?id=..&t=.." opens it in the clips window
    if let Some(args) = editor_args(page) {
        let ui = std::env::current_exe().ok().and_then(|e| Some(e.parent()?.join("clipback-ui.exe")));
        if let Some(ui) = ui.filter(|p| p.is_file()) {
            if std::process::Command::new(&ui).args(args).spawn().is_ok() {
                return;
            }
        }
    }
    let url = format!("http://127.0.0.1:{}{page}", state::cfg().port);
    let Some(edge) = edge() else {
        let _ = std::process::Command::new("rundll32").args(["url.dll,FileProtocolHandler", &url]).spawn();
        return;
    };
    let profile = crate::config::local_dir().join("window");
    let child = std::process::Command::new(edge)
        .arg(format!("--app={url}"))
        .arg(format!("--user-data-dir={}", profile.display()))
        .args(["--window-size=1200,820", "--no-first-run", "--no-default-browser-check", "--autoplay-policy=no-user-gesture-required"])
        .spawn();
    if let Ok(child) = child {
        // A second window is handed to the running one and this process exits; only a fresh
        // start becomes the window's browser.
        let own = crate::audio::OWN_BROWSER_PID.load(Ordering::Relaxed);
        if own == 0 || !lifelog::watcher::processes().contains_key(&own) {
            crate::audio::OWN_BROWSER_PID.store(child.id(), Ordering::Relaxed);
        }
        keep_window_icon();
    }
}

/// Edge gives an app window its 16 px favicon, blown up and blurry in the taskbar. While the
/// window is open, this swaps in clipback's own icon at the sizes the taskbar wants (and again
/// whenever Edge puts the favicon back, e.g. after moving between pages). It also notes which
/// Edge process owns the window, which `open_window` can't know if the window outlived a restart.
pub fn keep_window_icon() {
    use std::sync::atomic::AtomicBool;
    static RUNNING: AtomicBool = AtomicBool::new(false);
    if RUNNING.swap(true, Ordering::SeqCst) {
        return;
    }
    std::thread::spawn(|| {
        let mut missing = 0;
        // Give the window up to 20 s to appear, then stop a few seconds after it closes.
        while missing < 40 && !state::stopping() {
            if fix_windows() { missing = 0 } else { missing += 1 }
            std::thread::sleep(std::time::Duration::from_millis(500));
        }
        RUNNING.store(false, Ordering::SeqCst);
    });
}

/// Every page title ends with this (ui/common.js). Edge app windows show the bare page title,
/// while a normal browser tab adds " - Microsoft Edge" etc., so only our window matches.
const TITLE_END: &str = " · clipback";

/// Puts the app icon on clipback's Edge windows; false if there are none.
fn fix_windows() -> bool {
    use windows_sys::Win32::Foundation::{HWND, LPARAM};
    use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
    use windows_sys::Win32::UI::HiDpi::{GetDpiForWindow, GetSystemMetricsForDpi};
    use windows_sys::Win32::UI::WindowsAndMessaging::*;
    unsafe extern "system" fn each(hwnd: HWND, lp: LPARAM) -> i32 {
        unsafe {
            let found = &mut *(lp as *mut Vec<(HWND, u32)>);
            let mut buf = [0u16; 512];
            let n = GetWindowTextW(hwnd, buf.as_mut_ptr(), buf.len() as i32);
            if n > 0 && IsWindowVisible(hwnd) != 0 && String::from_utf16_lossy(&buf[..n as usize]).ends_with(TITLE_END) {
                let mut pid = 0;
                GetWindowThreadProcessId(hwnd, &mut pid);
                found.push((hwnd, pid));
            }
        }
        1
    }
    let mut found: Vec<(HWND, u32)> = Vec::new();
    unsafe { EnumWindows(Some(each), &mut found as *mut _ as LPARAM) };
    if found.is_empty() {
        return false;
    }
    let procs = lifelog::watcher::processes();
    found.retain(|(_, pid)| procs.get(pid).is_some_and(|p| p.exe == "msedge.exe"));
    for &(hwnd, pid) in &found {
        crate::audio::OWN_BROWSER_PID.store(pid, Ordering::Relaxed);
        unsafe {
            let dpi = GetDpiForWindow(hwnd);
            for (kind, metric) in [(ICON_BIG, SM_CXICON), (ICON_SMALL, SM_CXSMICON)] {
                // LR_SHARED: Windows loads each size once and hands back the same handle.
                let px = GetSystemMetricsForDpi(metric, dpi);
                let icon = LoadImageW(GetModuleHandleW(std::ptr::null()), 1 as *const u16, IMAGE_ICON, px, px, LR_SHARED);
                if !icon.is_null() && SendMessageW(hwnd, WM_GETICON, kind as usize, 0) != icon as isize {
                    SendMessageW(hwnd, WM_SETICON, kind as usize, icon as isize);
                }
            }
        }
    }
    !found.is_empty()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_queries() {
        assert_eq!(query("/x?id=Clips%202026%2Fa+b.mp4&i=2", "id").unwrap(), "Clips 2026/a b.mp4");
        assert_eq!(query("/x?id=a&i=2", "i").unwrap(), "2");
        assert_eq!(decode("100%"), "100%");
        assert_eq!(decode("%E2%9C%93"), "✓");
    }

    #[test]
    fn png_is_well_formed() {
        let p = png(1, 1, &[255, 0, 0, 255]);
        assert!(p.starts_with(b"\x89PNG"));
        assert!(p.ends_with(&[0xAE, 0x42, 0x60, 0x82]), "IEND crc");
    }

    #[test]
    fn editor_pages_open_the_native_editor() {
        let a = editor_args("/editor?id=Clips%202026%2Fa%20b.mp4&t=12.5").unwrap();
        assert_eq!(a, ["--edit", "Clips 2026/a b.mp4", "--t", "12.5"]);
        assert_eq!(editor_args("/editor?id=x.mp4").unwrap(), ["--edit", "x.mp4"]);
        assert!(editor_args("/editor?t=3").is_none());
        assert!(editor_args("/options").is_none());
    }
}
