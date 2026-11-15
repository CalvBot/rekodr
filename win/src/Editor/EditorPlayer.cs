using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;

namespace Clipback;

/// <summary>
/// The editor's video, in a child clipback-ui process (started with --editor-player) like the clips player
/// (PlayerProcess): the decoder and graphics memory go away with it when the editor closes. Besides the picture it
/// plays each audio track on its own at its own volume, previews the zoom (the Media Engine shows just the zoomed
/// part of each frame, on the graphics card), stops at the trim's end, and passes mouse drags on the picture back
/// for aiming the zoom. Lines of text over stdin/stdout.
/// </summary>
sealed class EditorPlayer : IDisposable
{
    readonly Control host;
    Process? proc;
    IntPtr child;

    public double Duration { get; private set; }
    public double Position { get; private set; }
    public bool Paused { get; private set; } = true;
    public Size VideoSize { get; private set; }
    public string? Error { get; private set; }
    /// <summary>A new state line arrived (UI thread)</summary>
    public event Action? Changed;
    /// <summary>Mouse on the picture: 'd'own, 'm'ove (while held) or 'u'p, at 0–1 across and down (UI thread)</summary>
    public event Action<char, double, double>? Mouse;

    public EditorPlayer(Control host) => this.host = host;
#if DEBUG
    /// <summary>Each audio track's position, playing state and error, for the test hooks</summary>
    public string TracksDebug = "";
#endif

    public void Open(string path, double at)
    {
        var psi = new ProcessStartInfo(Environment.ProcessPath!, ["--editor-player", host.Handle.ToString(CultureInfo.InvariantCulture)])
        {
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            CreateNoWindow = true,
        };
        proc = Process.Start(psi)!;
        var p = proc;
        new Thread(() => Read(p)) { IsBackground = true, Name = "editor player out" }.Start();
        Position = at;
        Send($"open {F(at)} {path}");
    }

    static string F(double v) => v.ToString("0.#####", CultureInfo.InvariantCulture);

    void Send(string line)
    {
        try { proc?.StandardInput.WriteLine(line); proc?.StandardInput.Flush(); } catch { }
    }

    /// <summary>Plays from where it is, or from the trim's start if it's outside it</summary>
    public void Play() { Send("play"); Paused = false; }
    public void Pause() { Send("pause"); Paused = true; }
    public void Seek(double s) { s = Math.Clamp(s, 0, Duration > 0 ? Duration : s); Send($"seek {F(s)}"); Position = s; seekedAt = Environment.TickCount64; }
    /// <summary>Playing stops at `to` and goes back to `from`</summary>
    public void Bounds(double from, double to) => Send($"bounds {F(from)} {F(to)}");
    public void Keys(IEnumerable<ZoomKey> keys) => Send("keys " + ZoomView.Pack(keys));
    /// <summary>A track to play: url null = the video's own sound</summary>
    public void Track(int index, string? url) => Send($"track {index} {url ?? "-"}");
    /// <summary>The video's own sound off (the tracks play instead)</summary>
    public void MuteVideo() => Send("mutevideo");
    public void Volume(int index, double volume, bool muted) => Send($"vol {index} {F(volume)} {(muted ? 1 : 0)}");

    long seekedAt;

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
        var v = arg.Split(' ');
        double D(int i) => double.Parse(v[i], CultureInfo.InvariantCulture);
        switch (parts[0])
        {
            case "hwnd":
                child = (IntPtr)long.Parse(arg, CultureInfo.InvariantCulture);
                Resized();
                break;
            case "state":
                // Just after a seek from here, the player may still report where it was
                if (Environment.TickCount64 - seekedAt > 250) Position = D(0);
                Duration = D(1);
                Paused = v[2] == "1";
                VideoSize = new Size((int)D(3), (int)D(4));
                break;
            case "error": Error = arg; break;
#if DEBUG
            case "tracks": TracksDebug = arg; return;
#endif
            case "mouse": Mouse?.Invoke(v[0][0], D(1), D(2)); return;
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

/// <summary>The --editor-player process: the picture as a child window of the editor's video panel, plus one
/// sound-only engine per audio track, kept in step with the picture. Exits when told to or when its input closes.</summary>
sealed class EditorHost : Control
{
    readonly IntPtr parent;
    readonly MediaEngine video;
    readonly Dictionary<int, MediaEngine> tracks = [];
    readonly Dictionary<int, (double volume, bool muted)> mix = [];
    readonly System.Windows.Forms.Timer tick = new() { Interval = 15 };
    List<ZoomKey> keys = [];
    double from, to = double.MaxValue;
    double? pendingSeek;
    bool playing, dragging;
    string lastState = "";

    public static void Run(IntPtr parent)
    {
        using var host = new EditorHost(parent);
        Application.Run();
    }

    EditorHost(IntPtr parent)
    {
        this.parent = parent;
        BackColor = Color.Black;
        CreateControl();
        video = new MediaEngine(this);
        video.Changed += OnVideoChanged;
        tick.Tick += (_, _) => Tick();
        Out("hwnd " + Handle.ToString(CultureInfo.InvariantCulture));
        new Thread(ReadCommands) { IsBackground = true, Name = "editor player in" }.Start();
    }

    protected override CreateParams CreateParams
    {
        get
        {
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
        video?.Resized();
    }

    static void Out(string line)
    {
        try { Console.Out.WriteLine(line); Console.Out.Flush(); } catch { }
    }

    static string F(double v) => v.ToString("0.####", CultureInfo.InvariantCulture);

    // ---------- mouse: aiming the zoom is done by the editor ----------

    void Mouse(char kind, MouseEventArgs e) =>
        Out($"mouse {kind} {F(e.X / (double)Math.Max(1, Width))} {F(e.Y / (double)Math.Max(1, Height))}");

    protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { dragging = true; Mouse('d', e); } }
    protected override void OnMouseMove(MouseEventArgs e) { if (dragging) Mouse('m', e); }
    protected override void OnMouseUp(MouseEventArgs e) { if (dragging) { dragging = false; Mouse('u', e); } }

    // ---------- playback ----------

    double Position => pendingSeek ?? video.Position;

    void SeekAll(double t)
    {
        // While a seek is under way, only the latest asked for is kept: dragging along the timeline never queues up
        if (video.Seeking) pendingSeek = t;
        else { pendingSeek = null; video.Seek(t); }
        foreach (var a in tracks.Values) a.Seek(t);
        ShowView();
    }

    void OnVideoChanged()
    {
        if (pendingSeek is { } t && !video.Seeking) { pendingSeek = null; video.Seek(t); }
        if (video.Ended && playing) { Stop(); SeekAll(from); }
        Report();
    }

    void Play()
    {
        var pos = Position;
        if (pos >= to - 0.05 || pos < from) SeekAll(from);
        playing = true;
        video.Play();
        foreach (var a in tracks.Values) { a.Seek(Position); a.Play(); }
        tick.Start();
    }

    void Stop()
    {
        playing = false;
        video.Pause();
        foreach (var a in tracks.Values) a.Pause();
    }

    void Tick()
    {
        if (playing)
        {
            var pos = video.Position;
            if (pos >= to) { Stop(); SeekAll(from); }
            else
                foreach (var a in tracks.Values)
                    if (Math.Abs(a.Position - pos) > 0.08) a.Seek(pos);
        }
        ShowView();
        Report();
        if (!playing) tick.Stop();
    }

    /// <summary>The zoom at the current moment, drawn by the Media Engine as the part of the frame it shows</summary>
    void ShowView()
    {
        var v = ZoomView.At(keys, Position);
        video.SetView(v.Zoomed ? v.Part : null);
        Cursor = v.Zoomed ? Cursors.SizeAll : Cursors.Default;
    }

    /// <summary>Volumes as set, scaled down together if any is over 100% (the preview can't go louder than full);
    /// the export uses them as set</summary>
    void ApplyMix()
    {
        var loudest = Math.Max(1, mix.Values.Where(m => !m.muted).Select(m => m.volume).DefaultIfEmpty(0).Max());
        foreach (var (i, m) in mix)
        {
            var engine = tracks.TryGetValue(i, out var a) ? a : i == ownTrack ? video : null;
            engine?.SetVolume(m.muted ? 0 : m.volume / loudest);
        }
    }

    /// <summary>The track that is the video's own sound (a clip with one track), or -1</summary>
    int ownTrack = -1;

    void Report()
    {
        var s = video.VideoSize;
        var state = string.Format(CultureInfo.InvariantCulture, "state {0:0.###} {1:0.###} {2} {3} {4}",
            Position, video.Duration, playing ? 0 : 1, s.Width, s.Height);
        var error = video.Error ?? tracks.Where(t => t.Value.Error != null).Select(t => $"Can't play audio track {t.Key}").FirstOrDefault();
        if (error != null && error != lastError) { lastError = error; Out("error " + error); }
        if (state != lastState) { lastState = state; Out(state); }
#if DEBUG
        var dbg = "tracks " + string.Join(" ", tracks.Select(a => $"{a.Key}:{Math.Round(a.Value.Position, 1).ToString(CultureInfo.InvariantCulture)}:{(a.Value.Paused ? "p" : "P")}:{a.Value.Error ?? "ok"}"));
        if (dbg != lastTracks) { lastTracks = dbg; Out(dbg); }
#endif
    }

    string? lastError;
#if DEBUG
    string lastTracks = "";
#endif

    void ReadCommands()
    {
        string? line;
        while ((line = Console.In.ReadLine()) != null)
        {
            var l = line;
            if (l == "quit") break;
            try { BeginInvoke(() => Command(l)); } catch { break; }
        }
        try { BeginInvoke(Application.ExitThread); } catch { Environment.Exit(0); }
    }

    void Command(string line)
    {
        var parts = line.Split(' ', 3);
        double D(int i) => double.Parse(parts[i], CultureInfo.InvariantCulture);
        try
        {
            switch (parts[0])
            {
                case "open":
                    video.Open(parts[2], D(1), autoPlay: false);
                    tick.Start();
                    break;
                case "track":
                    var index = int.Parse(parts[1], CultureInfo.InvariantCulture);
                    mix[index] = (1, false);
                    if (parts[2] == "-") ownTrack = index;
                    else
                    {
                        var a = new MediaEngine(this, audioOnly: true);
                        a.Open(parts[2], Position, autoPlay: false);
                        tracks[index] = a;
                    }
                    break;
                case "mutevideo": video.SetMuted(true); break;
                case "vol":
                    var v = parts[2].Split(' ');
                    mix[int.Parse(parts[1], CultureInfo.InvariantCulture)] = (double.Parse(v[0], CultureInfo.InvariantCulture), v[1] == "1");
                    ApplyMix();
                    break;
                case "play": Play(); break;
                case "pause": Stop(); break;
                case "seek": SeekAll(D(1)); break;
                case "bounds": from = D(1); to = D(2); break;
                case "keys": keys = ZoomView.Unpack(parts.Length > 1 ? string.Join(' ', parts[1..]) : ""); ShowView(); break;
            }
        }
        catch (Exception ex) { Out("error " + ex.Message); }
        Report();
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            tick.Dispose();
            foreach (var a in tracks.Values) a.Dispose();
            video.Dispose();
        }
        base.Dispose(disposing);
    }
}
