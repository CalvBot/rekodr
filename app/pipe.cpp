// The pipe: a local two-way connection for programs that drive clipback and show its state, such as the Stream
// Deck plugin (streamdeck\). It is a named pipe, \\.\pipe\clipback, so nothing listens on a network port and only
// programs on this PC can reach it.
//
// A client sends commands, one per line, the same ones `clipback.exe <command>` takes ("save 30s", "toggle",
// "open"). clipback sends lines of JSON:
//   {"type":"state","paused":false,"recording":true,"game":"Overwatch","oldest_us":…,"saving":0,"error":""}
//       when a client connects and whenever any of it changes. oldest_us is the oldest footage in the buffer
//       (0 = none), so a client can show how much the buffer holds.
//   {"type":"saved","ok":true,"kind":"clip","file":"C:\\…\\Overwatch 2026-11-16 21-04-10.mp4"}
//   {"type":"saved","ok":false,"kind":"clip","error":"…"}
//       when a save finishes. kind is clip, session, keep or share.
//
// One thread waits for clients and each client has a thread reading its commands, which go to the main thread as
// EV_COMMAND events. Lines are written by the main thread (and the first state line by the accepting thread), all
// under the lock; a client that doesn't read is dropped rather than waited for.

struct PipeServer {
    SRWLOCK lock;        // clients and state
    HANDLE  clients[8];  // 0 = free
    HANDLE  stop;        // set when clipback quits
    HANDLE  thread;      // the accepting thread
    HANDLE  write_done;  // for overlapped writes (one at a time, under the lock)
    wchar_t name[64];
    char    state[1024]; // the last state line, sent to each client as it connects
};
static PipeServer g_pipe;

// src as the inside of a JSON string.
static void json_escape(const char *src, char *out, i32 cap)
{
    i32 n = 0;
    for (; *src && n + 7 < cap; src++) {
        u8 c = (u8)*src;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = (char)c;
        } else if (c < 0x20) {
            n += snprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
}

// Call with the lock held. A client that can't take the line within a quarter of a second is disconnected (its
// reading thread then closes it).
static void pipe_write(HANDLE h, const char *text)
{
    OVERLAPPED ov = {};
    ov.hEvent = g_pipe.write_done;
    ResetEvent(ov.hEvent);
    DWORD n = 0;
    if (WriteFile(h, text, (DWORD)strlen(text), &n, &ov)) return;
    if (GetLastError() == ERROR_IO_PENDING && WaitForSingleObject(ov.hEvent, 250) == WAIT_OBJECT_0) return;
    CancelIoEx(h, &ov);
    GetOverlappedResult(h, &ov, &n, TRUE);
    DisconnectNamedPipe(h);
}

// Sends one line (ending in \n) to every client.
static void pipe_send(const char *line)
{
    if (!g_pipe.thread) return;
    AcquireSRWLockExclusive(&g_pipe.lock);
    for (i32 i = 0; i < COUNT(g_pipe.clients); i++)
        if (g_pipe.clients[i]) pipe_write(g_pipe.clients[i], line);
    ReleaseSRWLockExclusive(&g_pipe.lock);
}

// Main thread: sends the state if it has changed since it was last sent. Called once a second and after every
// command and event.
static void pipe_publish()
{
    if (!g_pipe.thread) return;
    const Recorder &r = g_app.recorder;
    char name[128] = "", game[256], error[600], line[1024];
    if (g_app.audio.game.s[0]) audio_display_name(g_app.config, g_app.audio.game.s, name, sizeof name);
    json_escape(name, game, sizeof game);
    json_escape(r.last_error.s, error, sizeof error);
    snprintf(line, sizeof line,
             "{\"type\":\"state\",\"paused\":%s,\"recording\":%s,\"game\":\"%s\",\"oldest_us\":%lld,\"saving\":%d,"
             "\"error\":\"%s\"}\n",
             r.paused ? "true" : "false", r.recording ? "true" : "false", game, r.oldest_us,
             g_app.saving.jobs_running, error);
    AcquireSRWLockExclusive(&g_pipe.lock);
    bool changed = strcmp(line, g_pipe.state) != 0;
    if (changed) {
        str_set(g_pipe.state, line);
        for (i32 i = 0; i < COUNT(g_pipe.clients); i++)
            if (g_pipe.clients[i]) pipe_write(g_pipe.clients[i], line);
    }
    ReleaseSRWLockExclusive(&g_pipe.lock);
}

// Main thread: a save has finished.
static void pipe_saved(SaveKind kind, bool ok, const char *file_or_error)
{
    if (kind == SAVE_PREVIEW) return;
    static const char *const kinds[] = {"clip", "session", "keep", "share"};
    char text[1100], line[1300];
    json_escape(file_or_error, text, sizeof text);
    snprintf(line, sizeof line, "{\"type\":\"saved\",\"ok\":%s,\"kind\":\"%s\",\"%s\":\"%s\"}\n", ok ? "true" : "false",
             kinds[kind], ok ? "file" : "error", text);
    pipe_send(line);
}

// One client's commands, a line each, until it goes away.
static DWORD WINAPI pipe_client_thread(void *arg)
{
    i32 slot = (i32)(intptr_t)arg;
    HANDLE h = g_pipe.clients[slot];
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    char buf[1024];
    i32 used = 0;
    for (;;) {
        DWORD n = 0;
        ResetEvent(ov.hEvent);
        if (!ReadFile(h, buf + used, (DWORD)(sizeof buf - 1 - used), &n, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) break;
            HANDLE wait[2] = {ov.hEvent, g_pipe.stop};
            if (WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(h, &ov);
                GetOverlappedResult(h, &ov, &n, TRUE);
                break;
            }
            if (!GetOverlappedResult(h, &ov, &n, FALSE)) break;
        }
        if (!n) break;
        used += (i32)n;
        buf[used] = 0;
        char *start = buf;
        for (char *nl; (nl = strchr(start, '\n')) != 0; start = nl + 1) {
            *nl = 0;
            char *line = trim(start);
            if (!*line) continue;
            DEV_LOG("pipe: \"%s\"", line);
            event_post_text(EV_COMMAND, line);
        }
        used = (i32)(buf + used - start);
        memmove(buf, start, (size_t)used);
        if (used >= (i32)sizeof buf - 1) used = 0; // a line too long to be a command
    }
    CloseHandle(ov.hEvent);
    AcquireSRWLockExclusive(&g_pipe.lock);
    g_pipe.clients[slot] = 0;
    CloseHandle(h);
    ReleaseSRWLockExclusive(&g_pipe.lock);
    DEV_LOG("pipe: a client left");
    return 0;
}

static DWORD WINAPI pipe_accept_thread(void *)
{
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(0, TRUE, FALSE, 0);
    for (;;) {
        HANDLE h = CreateNamedPipeW(g_pipe.name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 8192, 8192, 0, 0);
        if (h == INVALID_HANDLE_VALUE) {
            log_line("pipe: couldn't create %ls (%lu); Stream Deck keys won't show clipback's state", g_pipe.name,
                     GetLastError());
            break;
        }
        ResetEvent(ov.hEvent);
        bool connected = ConnectNamedPipe(h, &ov) != 0;
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (err == ERROR_IO_PENDING) {
                HANDLE wait[2] = {ov.hEvent, g_pipe.stop};
                connected = WaitForMultipleObjects(2, wait, FALSE, INFINITE) == WAIT_OBJECT_0;
            }
        }
        if (!connected) { // quitting, or the client had already gone
            bool stopping = WaitForSingleObject(g_pipe.stop, 0) == WAIT_OBJECT_0;
            CancelIoEx(h, &ov);
            CloseHandle(h);
            if (stopping) break;
            continue;
        }
        AcquireSRWLockExclusive(&g_pipe.lock);
        i32 slot = -1;
        for (i32 i = 0; i < COUNT(g_pipe.clients) && slot < 0; i++)
            if (!g_pipe.clients[i]) slot = i;
        if (slot >= 0) {
            g_pipe.clients[slot] = h;
            if (g_pipe.state[0]) pipe_write(h, g_pipe.state);
        }
        ReleaseSRWLockExclusive(&g_pipe.lock);
        if (slot < 0) {
            log_line("pipe: too many clients; one was turned away");
            CloseHandle(h);
            continue;
        }
        DEV_LOG("pipe: a client connected");
        CloseHandle(thread_start(pipe_client_thread, (void *)(intptr_t)slot));
    }
    CloseHandle(ov.hEvent);
    return 0;
}

// A test copy (CLIPBACK_HOME) has a pipe of its own, named after its home, so it never answers for the real one.
static void pipe_start()
{
    InitializeSRWLock(&g_pipe.lock);
    wchar_t home[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"CLIPBACK_HOME", home, MAX_PATH) && home[0]) {
        u32 h = 2166136261u;
        for (wchar_t *c = home; *c; c++) h = (h ^ (u32)towlower(*c)) * 16777619u;
        swprintf(g_pipe.name, COUNT(g_pipe.name), L"\\\\.\\pipe\\clipback-test-%08x", h);
    } else {
        wcscpy_s(g_pipe.name, L"\\\\.\\pipe\\clipback");
    }
    g_pipe.stop = CreateEventW(0, TRUE, FALSE, 0);
    g_pipe.write_done = CreateEventW(0, TRUE, FALSE, 0);
    g_pipe.thread = thread_start(pipe_accept_thread, 0);
}

static void pipe_stop()
{
    if (!g_pipe.thread) return;
    SetEvent(g_pipe.stop);
    WaitForSingleObject(g_pipe.thread, 1000);
}
