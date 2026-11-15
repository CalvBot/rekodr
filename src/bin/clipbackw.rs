//! The clipback app with no console window: what starts at login. Lives in the tray.
//! With a command (`clipbackw.exe save 90s`, `open`, `toggle` …) it tells the running copy
//! instead and exits, so a Stream Deck "Open" key can trigger clips without a console flashing.
#![windows_subsystem = "windows"]

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if let Some(cmd) = args.first().filter(|c| c.as_str() != "run") {
        let result = clipback::remote::command(cmd, args.get(1).map(String::as_str))
            .unwrap_or_else(|| Err(anyhow::anyhow!("unknown command {cmd:?}")));
        if let Err(e) = result {
            clipback::util::log(&format!("clipbackw {}: {e:#}", args.join(" ")));
            message(&format!("{e:#}"));
            std::process::exit(1);
        }
        return;
    }
    if let Err(e) = clipback::run_service() {
        clipback::util::log(&format!("{e:#}"));
        std::process::exit(1);
    }
}

fn message(text: &str) {
    use windows_sys::Win32::UI::WindowsAndMessaging::{MB_ICONWARNING, MB_SETFOREGROUND, MB_TOPMOST, MessageBoxW};
    let wide = |s: &str| s.encode_utf16().chain([0]).collect::<Vec<u16>>();
    let (text, title) = (wide(text), wide("clipback"));
    unsafe { MessageBoxW(std::ptr::null_mut(), text.as_ptr(), title.as_ptr(), MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND) };
}
