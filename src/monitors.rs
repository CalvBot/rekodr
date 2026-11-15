//! The monitors ddagrab can record, in its `output_idx` order (the outputs of the first GPU), with
//! the names Windows shows in Display settings.

use serde::Serialize;
use windows::Win32::Devices::Display::{
    DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, DISPLAYCONFIG_MODE_INFO,
    DISPLAYCONFIG_PATH_INFO, DISPLAYCONFIG_SOURCE_DEVICE_NAME, DISPLAYCONFIG_TARGET_DEVICE_NAME,
    DisplayConfigGetDeviceInfo, GetDisplayConfigBufferSizes, QDC_ONLY_ACTIVE_PATHS, QueryDisplayConfig,
};
use windows::Win32::Graphics::Dxgi::{CreateDXGIFactory1, IDXGIFactory1};
use windows::Win32::Graphics::Gdi::{DEVMODEW, ENUM_CURRENT_SETTINGS, EnumDisplaySettingsW};
use windows::Win32::UI::HiDpi::{DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, SetThreadDpiAwarenessContext};
use windows::core::PCWSTR;

#[derive(Serialize, Clone, Debug)]
pub struct Monitor {
    pub idx: u32,
    pub name: String,
    pub width: i32,
    pub height: i32,
    pub hz: u32,
    pub primary: bool,
}

fn wstr(w: &[u16]) -> String {
    String::from_utf16_lossy(&w[..w.iter().position(|c| *c == 0).unwrap_or(w.len())])
}

/// GDI device name (\.\DISPLAY1) → monitor name ("DELL S2721DGF").
fn friendly_names() -> Vec<(String, String)> {
    let mut out = Vec::new();
    unsafe {
        let (mut np, mut nm) = (0u32, 0u32);
        if GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut np, &mut nm).is_err() {
            return out;
        }
        let mut paths = vec![DISPLAYCONFIG_PATH_INFO::default(); np as usize];
        let mut modes = vec![DISPLAYCONFIG_MODE_INFO::default(); nm as usize];
        if QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &mut np, paths.as_mut_ptr(), &mut nm, modes.as_mut_ptr(), None)
            .is_err()
        {
            return out;
        }
        for p in &paths[..np as usize] {
            let mut src = DISPLAYCONFIG_SOURCE_DEVICE_NAME::default();
            src.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32;
            src.header.adapterId = p.sourceInfo.adapterId;
            src.header.id = p.sourceInfo.id;
            let mut tgt = DISPLAYCONFIG_TARGET_DEVICE_NAME::default();
            tgt.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = size_of::<DISPLAYCONFIG_TARGET_DEVICE_NAME>() as u32;
            tgt.header.adapterId = p.targetInfo.adapterId;
            tgt.header.id = p.targetInfo.id;
            if DisplayConfigGetDeviceInfo(&mut src.header) == 0 && DisplayConfigGetDeviceInfo(&mut tgt.header) == 0 {
                let name = wstr(&tgt.monitorFriendlyDeviceName);
                if !name.is_empty() {
                    out.push((wstr(&src.viewGdiDeviceName), name));
                }
            }
        }
    }
    out
}

pub fn list() -> Vec<Monitor> {
    let mut out = Vec::new();
    let names = friendly_names();
    unsafe {
        // Real pixels, not the sizes Windows shows to apps that don't know about display scaling.
        let before = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        list_outputs(&names, &mut out);
        SetThreadDpiAwarenessContext(before);
    }
    out
}

unsafe fn list_outputs(names: &[(String, String)], out: &mut Vec<Monitor>) {
    unsafe {
        let Ok(factory) = CreateDXGIFactory1::<IDXGIFactory1>() else { return };
        let Ok(adapter) = factory.EnumAdapters1(0) else { return };
        let mut i = 0;
        while let Ok(output) = adapter.EnumOutputs(i) {
            if let Ok(desc) = output.GetDesc() {
                let device = wstr(&desc.DeviceName);
                let r = desc.DesktopCoordinates;
                let mut mode = DEVMODEW { dmSize: size_of::<DEVMODEW>() as u16, ..Default::default() };
                let wide: Vec<u16> = device.encode_utf16().chain([0]).collect();
                let hz = if EnumDisplaySettingsW(PCWSTR(wide.as_ptr()), ENUM_CURRENT_SETTINGS, &mut mode).as_bool() {
                    mode.dmDisplayFrequency
                } else {
                    0
                };
                let name = names
                    .iter()
                    .find(|(d, _)| d.eq_ignore_ascii_case(&device))
                    .map(|(_, n)| n.clone())
                    .unwrap_or_else(|| device.trim_start_matches(r"\.\").to_string());
                out.push(Monitor {
                    idx: i,
                    name,
                    width: r.right - r.left,
                    height: r.bottom - r.top,
                    hz,
                    primary: r.left == 0 && r.top == 0,
                });
            }
            i += 1;
        }
    }
}
