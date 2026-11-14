//! The tray icon (red: recording, grey with a pause sign: paused), its menu, the global hotkeys
//! and save notifications. Runs the message loop on the calling thread until Quit.

use crate::config::{Action, format_duration, parse_action};
use crate::{hotkey, state, util::log};
use std::sync::Mutex;
use std::sync::atomic::{AtomicPtr, AtomicU32, Ordering};
use windows_sys::Win32::Foundation::{HWND, LPARAM, LRESULT, POINT, WPARAM};
use windows_sys::Win32::System::LibraryLoader::GetModuleHandleW;
use windows_sys::Win32::UI::Shell::{
    NIF_ICON, NIF_INFO, NIF_MESSAGE, NIF_TIP, NIIF_NOSOUND, NIM_ADD, NIM_DELETE, NIM_MODIFY, NOTIFYICONDATAW,
    Shell_NotifyIconW,
};
use windows_sys::Win32::UI::WindowsAndMessaging::*;

const WM_TRAY: u32 = WM_APP + 1;
const WM_REFRESH: u32 = WM_APP + 2;
const WM_NOTIFY_MSG: u32 = WM_APP + 3;
const NIN_BALLOONUSERCLICK: u32 = WM_USER + 5;

const CMD_OPEN: usize = 1;
const CMD_PAUSE: usize = 2;
const CMD_OPTIONS: usize = 3;
const CMD_FOLDER: usize = 4;
const CMD_QUIT: usize = 5;
const CMD_SAVE: usize = 100; // + index into SAVE_CHOICES

const SAVE_CHOICES: [u64; 4] = [30, 120, 600, 1800];

static HWND_: AtomicPtr<std::ffi::c_void> = AtomicPtr::new(std::ptr::null_mut());
static TASKBAR_CREATED: AtomicU32 = AtomicU32::new(u32::MAX);
static PENDING_NOTE: Mutex<Option<(String, String)>> = Mutex::new(None);

struct Ui {
    recording: HICON,
    paused: HICON,
    hotkeys: Vec<i32>,
    /// What clicking the last notification opens.
    note_target: Option<String>,
}

static UI: Mutex<Option<UiCell>> = Mutex::new(None);
struct UiCell(Ui);
unsafe impl Send for UiCell {}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

fn copy_wide(dst: &mut [u16], text: &str) {
    let w: Vec<u16> = text.encode_utf16().take(dst.len() - 1).collect();
    dst[..w.len()].copy_from_slice(&w);
    dst[w.len()] = 0;
}

/// A 32×32 icon drawn here: a red dot, or a grey dot with a pause sign.
fn make_icon(paused: bool) -> HICON {
    const S: usize = 32;
    let mut px = vec![0u8; S * S * 4];
    let (c, r) = (15.5f32, 13.5f32);
    for y in 0..S {
        for x in 0..S {
            let (dx, dy) = (x as f32 - c, y as f32 - c);
            let d = (dx * dx + dy * dy).sqrt();
            let cover = (r + 0.5 - d).clamp(0.0, 1.0);
            if cover <= 0.0 {
                continue;
            }
            let (mut rgb, ring) = if paused { ([120u8, 120, 128], false) } else { ([232u8, 38, 48], d > r - 2.5) };
            if ring {
                rgb = [255, 255, 255];
            }
            if paused && dy.abs() < 6.5 && ((dx > -5.5 && dx < -1.5) || (dx > 1.5 && dx < 5.5)) {
                rgb = [255, 255, 255];
            }
            // Bottom-up BGRA.
            let i = ((S - 1 - y) * S + x) * 4;
            px[i..i + 4].copy_from_slice(&[rgb[2], rgb[1], rgb[0], (cover * 255.0) as u8]);
        }
    }
    let mut res = Vec::with_capacity(40 + px.len() + S * 4);
    for v in [40u32, S as u32, (S * 2) as u32] {
        res.extend(v.to_le_bytes());
    }
    res.extend(1u16.to_le_bytes()); // planes
    res.extend(32u16.to_le_bytes()); // bits
    res.extend([0u8; 24]); // compression … important colours
    res.extend(&px);
    res.extend(vec![0u8; S * 4]); // AND mask
    unsafe { CreateIconFromResourceEx(res.as_ptr(), res.len() as u32, 1, 0x0003_0000, 32, 32, LR_DEFAULTCOLOR) }
}

fn tooltip() -> String {
    if state::paused() {
        return "clipback: paused".into();
    }
    if !state::VIDEO_LIVE.load(Ordering::Relaxed) {
        return "clipback: waiting for the screen".into();
    }
    let live = state::LIVE_AUDIO.lock().unwrap();
    let apps: Vec<String> =
        live.iter().filter(|a| a.exe != "mic").map(|a| a.exe.trim_end_matches(".exe").to_string()).collect();
    if apps.is_empty() { "clipback: recording".into() } else { format!("clipback: recording + {}", apps.join(", ")) }
}

fn icon_data(hwnd: HWND) -> NOTIFYICONDATAW {
    let mut nid: NOTIFYICONDATAW = unsafe { std::mem::zeroed() };
    nid.cbSize = std::mem::size_of::<NOTIFYICONDATAW>() as u32;
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid
}

fn update_icon(hwnd: HWND, ui: &Ui, add: bool) {
    let mut nid = icon_data(hwnd);
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = if state::paused() { ui.paused } else { ui.recording };
    copy_wide(&mut nid.szTip, &tooltip());
    unsafe { Shell_NotifyIconW(if add { NIM_ADD } else { NIM_MODIFY }, &nid) };
}

/// Re-reads state: icon, tooltip, hotkeys. Safe to call from any thread.
pub fn refresh() {
    let hwnd = HWND_.load(Ordering::Relaxed);
    if !hwnd.is_null() {
        unsafe { PostMessageW(hwnd, WM_REFRESH, 0, 0) };
    }
}

/// Shows a Windows notification from the tray icon. Clicking it opens the clips page.
pub fn notify(title: &str, text: &str) {
    *PENDING_NOTE.lock().unwrap() = Some((title.to_string(), text.to_string()));
    let hwnd = HWND_.load(Ordering::Relaxed);
    if !hwnd.is_null() {
        unsafe { PostMessageW(hwnd, WM_NOTIFY_MSG, 0, 0) };
    }
}

/// Asks the message loop to end (Ctrl+C, Quit).
pub fn quit() {
    state::STOP.store(true, Ordering::Relaxed);
    let hwnd = HWND_.load(Ordering::Relaxed);
    if !hwnd.is_null() {
        unsafe { PostMessageW(hwnd, WM_CLOSE, 0, 0) };
    }
}

/// Saves the last `secs` on a worker thread, with a beep and a notification.
pub fn save(secs: u64) {
    std::thread::spawn(move || match crate::export::save_last(secs) {
        Ok(saved) => {
            log(&format!("saved {} ({:.0}s, tracks: {})", saved.path.display(), saved.secs, saved.tracks.join(", ")));
            hotkey::beep(true);
            let name = saved.path.file_name().unwrap_or_default().to_string_lossy().to_string();
            notify("Clip saved", &format!("{name}\n{} · {} audio tracks", format_duration(saved.secs.round() as u64), saved.tracks.len()));
        }
        Err(e) => {
            log(&format!("save failed: {e:#}"));
            hotkey::beep(false);
            notify("Clip not saved", &format!("{e:#}"));
        }
    });
}

fn run_action(action: Action) {
    match action {
        Action::Save(secs) => save(secs),
        Action::Open => crate::server::open_window("/"),
        Action::Pause => state::set_paused(!state::paused()),
    }
}

fn show_menu(hwnd: HWND) {
    unsafe {
        let menu = CreatePopupMenu();
        let add = |id: usize, text: &str, flags: u32| {
            let w = wide(text);
            AppendMenuW(menu, MF_STRING | flags, id, w.as_ptr());
        };
        add(CMD_OPEN, "Open clips", 0);
        for (i, secs) in SAVE_CHOICES.iter().enumerate() {
            add(CMD_SAVE + i, &format!("Save last {}", format_duration(*secs)), if state::paused() { MF_GRAYED } else { 0 });
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, std::ptr::null());
        add(CMD_PAUSE, if state::paused() { "Resume recording" } else { "Pause recording" }, 0);
        add(CMD_OPTIONS, "Options…", 0);
        add(CMD_FOLDER, "Open clips folder", 0);
        AppendMenuW(menu, MF_SEPARATOR, 0, std::ptr::null());
        add(CMD_QUIT, "Quit", 0);
        let mut pt = POINT { x: 0, y: 0 };
        GetCursorPos(&mut pt);
        SetForegroundWindow(hwnd);
        let cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd, std::ptr::null());
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
        match cmd as usize {
            CMD_OPEN => crate::server::open_window("/"),
            CMD_PAUSE => state::set_paused(!state::paused()),
            CMD_OPTIONS => crate::server::open_window("/options"),
            CMD_FOLDER => {
                let cfg = state::cfg();
                let year = cfg.year_dir(chrono::Datelike::year(&chrono::Local::now()));
                let dir = if year.is_dir() { year } else { cfg.clips_dir };
                let _ = std::process::Command::new("explorer.exe").arg(dir).spawn();
            }
            CMD_QUIT => quit(),
            c if (CMD_SAVE..CMD_SAVE + SAVE_CHOICES.len()).contains(&c) => save(SAVE_CHOICES[c - CMD_SAVE]),
            _ => {}
        }
    }
}

fn with_ui<T>(f: impl FnOnce(&mut Ui) -> T) -> Option<T> {
    UI.lock().unwrap().as_mut().map(|c| f(&mut c.0))
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wparam: WPARAM, lparam: LPARAM) -> LRESULT {
    match msg {
        WM_TRAY => {
            match lparam as u32 {
                WM_LBUTTONUP => crate::server::open_window("/"),
                WM_RBUTTONUP => show_menu(hwnd),
                NIN_BALLOONUSERCLICK => {
                    let target = with_ui(|ui| ui.note_target.take()).flatten();
                    crate::server::open_window(target.as_deref().unwrap_or("/"));
                }
                _ => {}
            }
            0
        }
        WM_HOTKEY => {
            let i = wparam as usize;
            if let Some(h) = state::cfg().hotkeys.get(i.wrapping_sub(1)) {
                if let Ok(action) = parse_action(&h.action) {
                    run_action(action);
                }
            }
            0
        }
        WM_REFRESH => {
            let keys: Vec<String> = state::cfg().hotkeys.iter().map(|h| h.keys.clone()).collect();
            with_ui(|ui| {
                hotkey::register(hwnd, &keys, &mut ui.hotkeys);
                update_icon(hwnd, ui, false);
            });
            0
        }
        WM_NOTIFY_MSG => {
            if let Some((title, text)) = PENDING_NOTE.lock().unwrap().take() {
                let mut nid = icon_data(hwnd);
                nid.uFlags = NIF_INFO;
                nid.dwInfoFlags = NIIF_NOSOUND;
                copy_wide(&mut nid.szInfoTitle, &title);
                copy_wide(&mut nid.szInfo, &text);
                unsafe { Shell_NotifyIconW(NIM_MODIFY, &nid) };
                with_ui(|ui| ui.note_target = Some("/".into()));
            }
            0
        }
        WM_TIMER => {
            with_ui(|ui| update_icon(hwnd, ui, false));
            0
        }
        WM_CLOSE => {
            unsafe {
                Shell_NotifyIconW(NIM_DELETE, &icon_data(hwnd));
                PostQuitMessage(0);
            }
            0
        }
        m if m == TASKBAR_CREATED.load(Ordering::Relaxed) => {
            // Explorer restarted: put the icon back.
            with_ui(|ui| update_icon(hwnd, ui, true));
            0
        }
        _ => unsafe { DefWindowProcW(hwnd, msg, wparam, lparam) },
    }
}

/// Creates the icon, registers the hotkeys and runs the message loop until Quit.
pub fn run() -> anyhow::Result<()> {
    unsafe {
        let hinst = GetModuleHandleW(std::ptr::null());
        let class = wide("clipback-tray");
        let wc = WNDCLASSW {
            lpfnWndProc: Some(wndproc),
            hInstance: hinst,
            lpszClassName: class.as_ptr(),
            ..std::mem::zeroed()
        };
        RegisterClassW(&wc);
        let title = wide("clipback");
        let hwnd = CreateWindowExW(
            0,
            class.as_ptr(),
            title.as_ptr(),
            WS_OVERLAPPED,
            0,
            0,
            0,
            0,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            hinst,
            std::ptr::null(),
        );
        if hwnd.is_null() {
            anyhow::bail!("couldn't create the tray window");
        }
        TASKBAR_CREATED.store(RegisterWindowMessageW(wide("TaskbarCreated").as_ptr()), Ordering::Relaxed);
        *UI.lock().unwrap() = Some(UiCell(Ui {
            recording: make_icon(false),
            paused: make_icon(true),
            hotkeys: Vec::new(),
            note_target: None,
        }));
        HWND_.store(hwnd, Ordering::Relaxed);
        let keys: Vec<String> = state::cfg().hotkeys.iter().map(|h| h.keys.clone()).collect();
        with_ui(|ui| {
            update_icon(hwnd, ui, true);
            hotkey::register(hwnd, &keys, &mut ui.hotkeys);
        });
        for h in &state::cfg().hotkeys {
            log(&format!("hotkey {} → {}", h.keys, h.action));
        }
        // Tooltip refresh (which apps are being recorded).
        SetTimer(hwnd, 1, 3000, None);

        let mut msg: MSG = std::mem::zeroed();
        while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        HWND_.store(std::ptr::null_mut(), Ordering::Relaxed);
    }
    Ok(())
}
