//! Small shared pieces: logging, the clock, quiet child processes.

use std::io::Write;
use std::os::windows::process::CommandExt;
use std::path::Path;
use std::process::{Child, Command};
use std::sync::Mutex;
use windows_sys::Win32::System::JobObjects::{
    AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
    JobObjectExtendedLimitInformation, SetInformationJobObject,
};

static LOG: Mutex<()> = Mutex::new(());

pub fn log_path() -> std::path::PathBuf {
    crate::config::local_dir().join("clipback.log")
}

/// Prints and appends to %LOCALAPPDATA%\clipback\clipback.log (the tray app has no terminal).
pub fn log(msg: &str) {
    let line = format!("[{}] {msg}", chrono::Local::now().format("%Y-%m-%d %H:%M:%S"));
    let _guard = LOG.lock().unwrap();
    println!("{line}");
    let path = log_path();
    if std::fs::metadata(&path).is_ok_and(|m| m.len() > 5 * 1024 * 1024) {
        let _ = std::fs::rename(&path, path.with_extension("old.log"));
    }
    let _ = std::fs::create_dir_all(crate::config::local_dir());
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "{line}");
    }
}

/// A command that never opens a console window.
pub fn command(exe: impl AsRef<std::ffi::OsStr>) -> Command {
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;
    let mut cmd = Command::new(exe);
    cmd.creation_flags(CREATE_NO_WINDOW);
    cmd
}

/// Wall-clock time in microseconds since 1970.
pub fn now_us() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_micros() as i64)
        .unwrap_or(0)
}

/// The performance counter in 100 ns units: the clock WASAPI stamps audio packets with.
pub fn qpc_100ns() -> i64 {
    use windows_sys::Win32::System::Performance::{QueryPerformanceCounter, QueryPerformanceFrequency};
    let (mut count, mut freq) = (0i64, 0i64);
    unsafe {
        QueryPerformanceCounter(&mut count);
        QueryPerformanceFrequency(&mut freq);
    }
    if freq == 0 {
        return 0;
    }
    (count as i128 * 10_000_000 / freq as i128) as i64
}

/// Converts a WASAPI packet time to wall-clock µs.
pub fn qpc_to_wall_us(qpc_100ns_time: u64) -> i64 {
    let (now, qpc) = (now_us(), qpc_100ns());
    now - (qpc - qpc_100ns_time as i64) / 10
}

/// A job object that kills its child processes (ffmpeg) when clipback exits for any reason,
/// so nothing keeps recording unsupervised.
pub struct KillOnExitJob(windows_sys::Win32::Foundation::HANDLE);

unsafe impl Send for KillOnExitJob {}
unsafe impl Sync for KillOnExitJob {}

impl KillOnExitJob {
    pub fn new() -> Self {
        unsafe {
            let job = CreateJobObjectW(std::ptr::null(), std::ptr::null());
            if !job.is_null() {
                let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
                info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(
                    job,
                    JobObjectExtendedLimitInformation,
                    &info as *const _ as *const _,
                    std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32,
                );
            }
            Self(job)
        }
    }

    pub fn adopt(&self, child: &Child) {
        use std::os::windows::io::AsRawHandle;
        if !self.0.is_null() {
            unsafe { AssignProcessToJobObject(self.0, child.as_raw_handle() as _) };
        }
    }
}

/// One job for the whole process; it is never closed until clipback exits.
pub fn job() -> &'static KillOnExitJob {
    static JOB: std::sync::OnceLock<KillOnExitJob> = std::sync::OnceLock::new();
    JOB.get_or_init(KillOnExitJob::new)
}

/// Waits up to `secs` for a child to exit, then kills it.
pub fn wait_or_kill(child: &mut Child, secs: u64) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(secs);
    while std::time::Instant::now() < deadline {
        if matches!(child.try_wait(), Ok(Some(_))) {
            return;
        }
        std::thread::sleep(std::time::Duration::from_millis(50));
    }
    let _ = child.kill();
    let _ = child.wait();
}

/// "Overwatch.exe" → "overwatch"; safe in file names.
pub fn file_safe(name: &str) -> String {
    name.trim_end_matches(".exe")
        .chars()
        .map(|c| if c.is_alphanumeric() || c == '-' || c == '_' { c } else { '_' })
        .take(40)
        .collect()
}

/// Opens Explorer with the file selected.
pub fn reveal(path: &Path) {
    let mut arg = std::ffi::OsString::from("/select,\"");
    arg.push(path.as_os_str());
    arg.push("\"");
    let _ = Command::new("explorer.exe").raw_arg(arg).spawn();
}
