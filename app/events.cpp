// Worker threads → main thread. A worker fills an Event and calls event_post; the main thread is woken by a
// WM_APP_EVENT message and handles every queued event in order (main.cpp, handle_event). Workers never touch App.

#define WM_APP_EVENT (WM_APP + 1)

enum EventKind : u8 {
    EV_NONE,
    EV_COMMAND, // text: a command line from a second launch, a hotkey or the tray ("save 30s", "open" …)
    EV_LOG,     // text: a message to show in the status line
    // One kind per module; `a` is the module's own sub-kind and the module's *_on_event handles it.
    EV_RECORDER,
    EV_AUDIO,
    EV_SAVE,
    EV_LIBRARY, // library.cpp and player.cpp: scans, thumbnails, Media Foundation events
    EV_TIMELINE,
    EV_GAMEDB,  // gamedb.cpp: the games list was loaded (b: how many), or art that was asked for has arrived
};

struct Event {
    EventKind kind;
    i64       a, b;
    char      text[512];
};

struct EventQueue {
    SRWLOCK lock;
    Event   items[1024];
    u32     head, tail; // head == tail: empty
    u32     dropped;
};

static EventQueue g_events;

static void event_post(const Event &e)
{
    AcquireSRWLockExclusive(&g_events.lock);
    u32 next = (g_events.tail + 1) % COUNT(g_events.items);
    bool full = next == g_events.head;
    if (full) {
        g_events.dropped++;
    } else {
        g_events.items[g_events.tail] = e;
        g_events.tail = next;
    }
    ReleaseSRWLockExclusive(&g_events.lock);
    if (!full) PostMessageW(g_app.msg_hwnd, WM_APP_EVENT, 0, 0);
}

static void event_post_text(EventKind kind, const char *text)
{
    Event e = {};
    e.kind = kind;
    str_set(e.text, text);
    event_post(e);
}

// Main thread only. Returns false when the queue is empty.
static bool event_take(Event *out)
{
    AcquireSRWLockExclusive(&g_events.lock);
    bool any = g_events.head != g_events.tail;
    if (any) {
        *out = g_events.items[g_events.head];
        g_events.head = (g_events.head + 1) % COUNT(g_events.items);
    }
    u32 dropped = g_events.dropped;
    g_events.dropped = 0;
    ReleaseSRWLockExclusive(&g_events.lock);
    if (dropped) log_line("event queue full: %u events dropped", dropped);
    return any;
}
