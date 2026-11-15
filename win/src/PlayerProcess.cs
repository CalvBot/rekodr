using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Clipback;

/// <summary>
/// Video playback in a second clipback-ui process (started with --player), drawn as a child window inside the
/// player's video panel. The graphics driver and decoder cost 100+ MB that Windows never hands back while the
/// process lives; in a process of its own it's all gone the moment the player closes, and the clips window stays
/// small. The two talk in lines of text over the child's stdin/stdout.
/// </summary>
sealed class PlayerProcess : IDisposable
{
    readonly Control host;
    Process? proc;
    IntPtr child;

    public double Duration { get; private set; }
    public double Position { get; private set; }
    public bool Paused { get; private set; } = true;
    public bool Ended { get; private set; }
    public Size VideoSize { get; private set; }
    public string? Error { get; private set; }
    /// <summary>A new state line arrived (UI thread)</summary>
    public event Action? Changed;
    /// <summary>The picture was clicked (UI thread)</summary>
    public event Action? Clicked;

    public PlayerProcess(Control host) => this.host = host;

    public void Open(string path, double at)
    {
        var psi = new ProcessStartInfo(Environment.ProcessPath!, ["--player", host.Handle.ToString(CultureInfo.InvariantCulture)])
        {
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        };
        proc = Process.Start(psi)!;
        var p = proc;
        new Thread(() => Read(p)) { IsBackground = true, Name = "player out" }.Start();
        Send($"open {F(at)} {path}");
    }

    static string F(double v) => v.ToString("0.###", CultureInfo.InvariantCulture);

    void Send(string line)
    {
        try { proc?.StandardInput.WriteLine(line); proc?.StandardInput.Flush(); } catch { }
    }

    public void Play() { Send("play"); Paused = false; Ended = false; }
    public void Pause() { Send("pause"); Paused = true; }
    public void Seek(double s) { Send($"seek {F(s)}"); Position = s; }

    [DllImport("user32.dll")] static extern bool MoveWindow(IntPtr h, int x, int y, int w, int hgt, bool repaint);

    /// <summary>Fits the picture to the host panel</summary>
    public void Resized()
    {
        if (child != IntPtr.Zero) MoveWindow(child, 0, 0, host.ClientSize.Width, host.ClientSize.Height, true);
    }

    void Read(Process p)
    {
        try
        {
            string? line;
            while ((line = p.StandardOutput.ReadLine()) != null)
            {
                var l = line;
                try { host.BeginInvoke(() => { if (p == proc) Handle(l); }); } catch (InvalidOperationException) { return; }
            }
        }
        catch { }
    }

    void Handle(string line)
    {
        var parts = line.Split(' ', 2);
        var arg = parts.Length > 1 ? parts[1] : "";
        switch (parts[0])
        {
            case "hwnd":
                child = (IntPtr)long.Parse(arg, CultureInfo.InvariantCulture);
                Resized();
                break;
            case "state":
                var v = arg.Split(' ');
                double D(int i) => double.Parse(v[i], CultureInfo.InvariantCulture);
                Position = D(0);
                Duration = D(1);
                Paused = v[2] == "1";
                Ended = v[3] == "1";
                VideoSize = new Size((int)D(4), (int)D(5));
                break;
            case "error": Error = arg; break;
            case "click": Clicked?.Invoke(); return;
        }
        Changed?.Invoke();
    }

    public void Dispose()
    {
        var p = proc;
        proc = null;
        child = IntPtr.Zero;
        if (p == null) return;
        try
        {
            p.StandardInput.WriteLine("quit");
            p.StandardInput.Close();
            if (!p.WaitForExit(1000)) p.Kill();
        }
        catch { try { p.Kill(); } catch { } }
        p.Dispose();
    }
}

/// <summary>The --player process: a child window in the clips window's video panel, running the Media Engine and
/// taking commands on stdin. It exits when told to, or when the clips window goes away.</summary>
sealed class PlayerHost : Control
{
    readonly IntPtr parent;
    readonly MediaEngine engine;
    readonly System.Windows.Forms.Timer tick = new() { Interval = 50 };
    string lastState = "";

    /// <summary>Runs the player until it's told to quit</summary>
    public static void Run(IntPtr parent)
    {
        using var host = new PlayerHost(parent);
        Application.Run();
    }

    PlayerHost(IntPtr parent)
    {
        this.parent = parent;
        BackColor = Color.Black;
        CreateControl();
        engine = new MediaEngine(this);
        engine.Changed += Report;
        Click += (_, _) => Out("click");
        tick.Tick += (_, _) => Report();
        Out("hwnd " + Handle.ToString(CultureInfo.InvariantCulture));
        new Thread(ReadCommands) { IsBackground = true, Name = "player in" }.Start();
    }

    protected override CreateParams CreateParams
    {
        get
        {
            // A real child of the clips window's video panel, though it lives in this process
            var cp = base.CreateParams;
            cp.Parent = parent;
            cp.Style = 0x40000000 | 0x10000000 | 0x04000000; // WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS
            cp.Width = 16;
            cp.Height = 9;
            return cp;
        }
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        engine?.Resized();
    }

    static void Out(string line)
    {
        try { Console.Out.WriteLine(line); Console.Out.Flush(); } catch { }
    }

    void Report()
    {
        var s = engine.VideoSize;
        var state = string.Format(CultureInfo.InvariantCulture, "state {0:0.###} {1:0.###} {2} {3} {4} {5}",
            engine.Position, engine.Duration, engine.Paused ? 1 : 0, engine.Ended ? 1 : 0, s.Width, s.Height);
        if (engine.Error != null) Out("error " + engine.Error);
        if (state != lastState) { lastState = state; Out(state); }
    }

    void ReadCommands()
    {
        string? line;
        while ((line = Console.In.ReadLine()) != null)
        {
            var l = line;
            if (l == "quit") break;
            try { BeginInvoke(() => Command(l)); } catch { break; }
        }
        // Told to stop, or the clips window is gone
        try { BeginInvoke(Application.ExitThread); } catch { Environment.Exit(0); }
    }

    void Command(string line)
    {
        var parts = line.Split(' ', 3);
        double D(int i) => double.Parse(parts[i], CultureInfo.InvariantCulture);
        switch (parts[0])
        {
            case "open":
                try { engine.Open(parts[2], D(1)); }
                catch (Exception ex) { Out("error Can't play this clip: " + ex.Message); }
                tick.Start();
                break;
            case "play": engine.Play(); break;
            case "pause": engine.Pause(); break;
            case "seek": engine.Seek(D(1)); break;
        }
        Report();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) { tick.Dispose(); engine.Dispose(); }
        base.Dispose(disposing);
    }
}
