// Rekodr's version, read by rekodr.rc (the exe's version resource) and by the code (the installer's Apps entry,
// the update check). Both lines change together.
#define REKODR_VERSION_NUM 0, 2, 0, 0
#define REKODR_VERSION "0.2.0"

// The name people read: the window, the tray, toasts, Options, setup, the installer's questions, its Apps entry and
// Start menu entry, the exe's version resource, the Cloudflare token it makes, the link page and the card's "via"
// line, and the folders it keeps (%APPDATA%, %LOCALAPPDATA%, Temp, Programs and the save folder under Videos). Ids
// (the exe, settings keys, registry values, pipes, window classes, credentials, the bucket's prefix) say "rekodr".
// ASCII with no %, &, < or " (it goes into printf formats and, unescaped, the link page's HTML).
#define APP_NAME "Rekodr"
