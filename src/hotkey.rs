//! Global hotkeys (F13–F24 suit a Stream Deck). Registered to the tray window, so presses arrive as
//! WM_HOTKEY there, and re-registered whenever the settings change.

use anyhow::{Result, bail};
use windows_sys::Win32::Foundation::HWND;
use windows_sys::Win32::System::Diagnostics::Debug::MessageBeep;
use windows_sys::Win32::UI::Input::KeyboardAndMouse::{
    MOD_ALT, MOD_CONTROL, MOD_NOREPEAT, MOD_SHIFT, MOD_WIN, RegisterHotKey, UnregisterHotKey,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{MB_ICONHAND, MB_OK};

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
                    k if k.len() == 1 && k.as_bytes()[0].is_ascii_alphanumeric() => k.as_bytes()[0].to_ascii_uppercase() as u32,
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

pub fn beep(ok: bool) {
    unsafe { MessageBeep(if ok { MB_OK } else { MB_ICONHAND }) };
}

/// Registers the hotkeys to `hwnd` with ids 1.., replacing `old`. Keys another app already holds
/// are logged and skipped.
pub fn register(hwnd: HWND, keys: &[String], old: &mut Vec<i32>) {
    for id in old.drain(..) {
        unsafe { UnregisterHotKey(hwnd, id) };
    }
    for (i, k) in keys.iter().enumerate() {
        let id = i as i32 + 1;
        match parse(k) {
            Ok((mods, vk)) => {
                if unsafe { RegisterHotKey(hwnd, id, mods | MOD_NOREPEAT, vk) } != 0 {
                    old.push(id);
                } else {
                    crate::util::log(&format!("hotkey {k} is already used by another app; skipped"));
                }
            }
            Err(e) => crate::util::log(&format!("{e}; skipped")),
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
        assert_eq!(parse("F13").unwrap(), (0, 0x7C));
        assert_eq!(parse("F24").unwrap(), (0, 0x87));
        assert!(parse("Ctrl+Alt").is_err());
        assert!(parse("Ctrl+F25").is_err());
        assert!(parse("Ctrl+A+B").is_err());
    }
}
