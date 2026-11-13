//! Global hotkeys via RegisterHotKey and a message loop on the calling thread.

use anyhow::{bail, Result};
use windows_sys::Win32::System::Diagnostics::Debug::MessageBeep;
use windows_sys::Win32::System::Threading::GetCurrentThreadId;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    RegisterHotKey, UnregisterHotKey, MOD_ALT, MOD_CONTROL, MOD_NOREPEAT, MOD_SHIFT, MOD_WIN,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    GetMessageW, PostThreadMessageW, MB_ICONHAND, MB_OK, MSG, WM_HOTKEY, WM_QUIT,
};

/// "Ctrl+Alt+F9" → (modifier flags, virtual key code).
pub fn parse(keys: &str) -> Result<(u32, u32)> {
    let mut mods = 0;
    let mut vk = None;
    for part in keys.split('+').map(|p| p.trim().to_ascii_lowercase()) {
        match part.as_str() {
            "ctrl" | "control" => mods |= MOD_CONTROL,
            "alt" => mods |= MOD_ALT,
            "shift" => mods |= MOD_SHIFT,
            "win" | "super" => mods |= MOD_WIN,
            key => {
                if vk.is_some() {
                    bail!("hotkey {keys:?} has more than one non-modifier key");
                }
                vk = Some(match key {
                    "home" => 0x24,
                    "end" => 0x23,
                    "insert" => 0x2D,
                    "delete" => 0x2E,
                    "pageup" => 0x21,
                    "pagedown" => 0x22,
                    "pause" => 0x13,
                    "scrolllock" => 0x91,
                    "printscreen" => 0x2C,
                    k if k.len() == 1 && k.as_bytes()[0].is_ascii_alphanumeric() => {
                        k.as_bytes()[0].to_ascii_uppercase() as u32
                    }
                    k => match k.strip_prefix('f').and_then(|n| n.parse::<u32>().ok()) {
                        Some(n @ 1..=24) => 0x70 + n - 1,
                        _ => bail!("unknown key {key:?} in hotkey {keys:?}"),
                    },
                });
            }
        }
    }
    match vk {
        Some(vk) => Ok((mods, vk)),
        None => bail!("hotkey {keys:?} has no key"),
    }
}

pub fn current_thread_id() -> u32 {
    unsafe { GetCurrentThreadId() }
}

/// Makes `run_loop` on the given thread return.
pub fn quit(thread_id: u32) {
    unsafe { PostThreadMessageW(thread_id, WM_QUIT, 0, 0) };
}

pub fn beep(ok: bool) {
    unsafe { MessageBeep(if ok { MB_OK } else { MB_ICONHAND }) };
}

/// Registers the hotkeys and blocks, calling `on_press(index)` for each press, until `quit` is called.
/// Hotkeys that can't be registered (already taken by another app) are reported and skipped.
pub fn run_loop(hotkeys: &[String], mut on_press: impl FnMut(usize)) {
    let mut registered = Vec::new();
    for (i, keys) in hotkeys.iter().enumerate() {
        match parse(keys) {
            Ok((mods, vk)) => {
                if unsafe { RegisterHotKey(std::ptr::null_mut(), i as i32, mods | MOD_NOREPEAT, vk) } != 0 {
                    registered.push(i as i32);
                } else {
                    crate::log(&format!("hotkey {keys} is already in use by another app; skipped"));
                }
            }
            Err(e) => crate::log(&format!("{e}; skipped")),
        }
    }
    unsafe {
        let mut msg: MSG = std::mem::zeroed();
        while GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) > 0 {
            if msg.message == WM_HOTKEY {
                on_press(msg.wParam);
            }
        }
        for id in registered {
            UnregisterHotKey(std::ptr::null_mut(), id);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses() {
        assert_eq!(parse("Ctrl+Alt+F9").unwrap(), (MOD_CONTROL | MOD_ALT, 0x78));
        assert_eq!(parse("win + shift + r").unwrap(), (MOD_WIN | MOD_SHIFT, 'R' as u32));
        assert_eq!(parse("F24").unwrap(), (0, 0x87));
        assert!(parse("Ctrl+Alt").is_err());
        assert!(parse("Ctrl+F25").is_err());
        assert!(parse("Ctrl+A+B").is_err());
    }
}
