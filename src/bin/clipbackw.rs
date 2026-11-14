//! The clipback app with no console window: what starts at login. Lives in the tray.
#![windows_subsystem = "windows"]

fn main() {
    if let Err(e) = clipback::run_service() {
        clipback::util::log(&format!("{e:#}"));
        std::process::exit(1);
    }
}
