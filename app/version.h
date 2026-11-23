// clipback's version, read by clipback.rc (the exe's version resource) and by the code (the installer's Apps entry,
// the update check). Both lines change together.
#define CLIPBACK_VERSION_NUM 0, 2, 0, 0
#define CLIPBACK_VERSION "0.2.0"

// The name the player sees: the window, the tray, toasts, Options, setup, the installer's questions, its Apps entry
// and Start menu entry, the exe's version resource, the link page and the Cloudflare token it makes. Files,
// folders, settings keys, registry keys, pipes, window classes, the bucket's prefix and log lines say "clipback"
// whatever this is. ASCII with no %, &, < or " (it goes into printf formats and, unescaped, the link page's HTML).
#define APP_NAME "clipback"
