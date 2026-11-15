namespace Clipback;

/// <summary>
/// Debug builds only: drives the window for screenshots without touching the mouse, from CLIPBACK_UI_TEST,
/// e.g. "game=Other;page=1;hover=2,0.5" or "play=0,0.3". Steps run once, after the first clips load.
/// </summary>
static class TestHooks
{
    public static void Run(ClipsForm form, Action<string, string> step)
    {
        var script = Environment.GetEnvironmentVariable("CLIPBACK_UI_TEST");
        if (string.IsNullOrEmpty(script)) return;
        foreach (var part in script.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = part.Split('=', 2);
            step(kv[0].Trim(), kv.Length > 1 ? kv[1].Trim() : "");
        }
    }
}
