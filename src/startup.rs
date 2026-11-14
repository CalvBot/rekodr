//! Start with Windows: a value under HKCU\…\Run pointing at clipbackw.exe.

use anyhow::{Result, bail};
use windows_sys::Win32::System::Registry::{
    HKEY, HKEY_CURRENT_USER, KEY_SET_VALUE, REG_SZ, RegCloseKey, RegDeleteValueW, RegOpenKeyExW, RegSetValueExW,
};

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

/// The windowless exe next to this one.
fn app_exe() -> Result<std::path::PathBuf> {
    let me = std::env::current_exe()?;
    Ok(me.with_file_name("clipbackw.exe"))
}

pub fn set(enabled: bool) -> Result<()> {
    let key_path = wide(r"Software\Microsoft\Windows\CurrentVersion\Run");
    let name = wide("clipback");
    unsafe {
        let mut key: HKEY = std::ptr::null_mut();
        if RegOpenKeyExW(HKEY_CURRENT_USER, key_path.as_ptr(), 0, KEY_SET_VALUE, &mut key) != 0 {
            bail!("can't open the Run key");
        }
        let result = if enabled {
            let value = wide(&format!("\"{}\"", app_exe()?.display()));
            RegSetValueExW(key, name.as_ptr(), 0, REG_SZ, value.as_ptr() as *const u8, (value.len() * 2) as u32)
        } else {
            let r = RegDeleteValueW(key, name.as_ptr());
            if r == 2 { 0 } else { r } // already gone
        };
        RegCloseKey(key);
        if result != 0 {
            bail!("updating the Run key failed ({result})");
        }
    }
    Ok(())
}
