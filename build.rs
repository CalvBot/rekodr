// Embeds clipback's icons and name (Explorer, Task Manager, Startup apps, the taskbar).
// Icon 1 is the app icon; 2 and 3 are the tray icons (recording, paused), loaded by tray.rs.
// The .ico files are drawn by assets/make_icons.py.
fn main() {
    for f in ["assets/clipback.ico", "assets/tray-recording.ico", "assets/tray-paused.ico"] {
        println!("cargo:rerun-if-changed={f}");
    }
    let mut res = winresource::WindowsResource::new();
    res.set_icon_with_id("assets/clipback.ico", "1");
    res.set_icon_with_id("assets/tray-recording.ico", "2");
    res.set_icon_with_id("assets/tray-paused.ico", "3");
    res.set("ProductName", "clipback");
    res.set("FileDescription", "clipback replay buffer");
    if let Err(e) = res.compile() {
        println!("cargo:warning=could not embed the icons: {e}");
    }
}
