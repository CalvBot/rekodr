using System.Runtime.InteropServices;

namespace Clipback;

/// <summary>
/// Windows' Media Foundation Media Engine (the player behind the HTML video element), drawing straight into a
/// window. Decoding is on the graphics card and costs a few MB, and it plays the clip's first audio track (Mix).
/// Events arrive on a Media Foundation thread; Changed is raised on the UI thread.
/// </summary>
sealed class MediaEngine : IDisposable
{
    IMFMediaEngine? engine;
    readonly Notify notify;
    readonly Control host;

    /// <summary>Something changed (loaded, played, paused, ended, error): read the properties again</summary>
    public event Action? Changed;
    public string? Error { get; private set; }

    /// <summary>audioOnly: sound only (one of the editor's tracks), no picture and no window</summary>
    public MediaEngine(Control host, bool audioOnly = false)
    {
        this.host = host;
        notify = new Notify(this);
        if (MFStartup(0x00020070, 0) < 0) throw new InvalidOperationException("Media Foundation isn't available");
        var factory = (IMFMediaEngineClassFactory)Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("b44392da-499b-446b-a4cb-005fead0e6d5"))!)!;
        Check(MFCreateAttributes(out var attrs, 4));
        // Our own D3D11 device with video support, so decoding stays on the graphics card (frames in video memory)
        if (!audioOnly && MakeDeviceManager() is { } manager)
        {
            var dxgi = MF_MEDIA_ENGINE_DXGI_MANAGER;
            Check(attrs.SetUnknown(ref dxgi, manager));
            this.manager = manager;
        }
        var key = MF_MEDIA_ENGINE_CALLBACK;
        Check(attrs.SetUnknown(ref key, notify));
        if (!audioOnly)
        {
            key = MF_MEDIA_ENGINE_PLAYBACK_HWND;
            Check(attrs.SetUINT64(ref key, (ulong)host.Handle));
            key = MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT;
            Check(attrs.SetUINT32(ref key, 87)); // DXGI_FORMAT_B8G8R8A8_UNORM
        }
        Check(factory.CreateInstance(audioOnly ? 1u : 0u, attrs, out engine)); // 1: MF_MEDIA_ENGINE_AUDIOONLY
        Marshal.ReleaseComObject(attrs);
        Marshal.ReleaseComObject(factory);
    }

    static void Check(int hr) { if (hr < 0) Marshal.ThrowExceptionForHR(hr); }

    object? manager;

    static object? MakeDeviceManager()
    {
        IntPtr device = IntPtr.Zero, context = IntPtr.Zero;
        try
        {
            // D3D_DRIVER_TYPE_HARDWARE; VIDEO_SUPPORT | BGRA_SUPPORT
            if (D3D11CreateDevice(IntPtr.Zero, 1, IntPtr.Zero, 0x800 | 0x20, IntPtr.Zero, 0, 7, out device, out _, out context) < 0) return null;
            var d = Marshal.GetObjectForIUnknown(device);
            if (d is ID3D10Multithread mt) mt.SetMultithreadProtected(true);
            if (MFCreateDXGIDeviceManager(out var token, out var manager) < 0) return null;
            var ok = manager.ResetDevice(d, token) >= 0;
            Marshal.ReleaseComObject(d);
            if (!ok) { Marshal.ReleaseComObject(manager); return null; }
            return manager;
        }
        catch { return null; }
        finally
        {
            if (device != IntPtr.Zero) Marshal.Release(device);
            if (context != IntPtr.Zero) Marshal.Release(context);
        }
    }

    public void Open(string path, double startAt, bool autoPlay = true)
    {
        pendingSeek = startAt;
        Error = null;
        engine?.SetAutoPlay(autoPlay);
        if (!autoPlay) engine?.SetPreload(4); // MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC: load and show the first frame while paused
        var bstr = Marshal.StringToBSTR(path);
        try { Check(engine!.SetSource(bstr)); }
        finally { Marshal.FreeBSTR(bstr); }
    }

    double pendingSeek;

    public double Duration { get { var d = engine?.GetDuration() ?? 0; return double.IsFinite(d) ? d : 0; } }
    public double Position => engine?.GetCurrentTime() ?? 0;
    public bool Paused => engine == null || engine.IsPaused();
    public bool Ended => engine != null && engine.IsEnded();
    public Size VideoSize
    {
        get
        {
            if (engine == null || engine.GetNativeVideoSize(out var w, out var h) < 0) return Size.Empty;
            return new Size((int)w, (int)h);
        }
    }

    public void Play() => engine?.Play();
    public void Pause() => engine?.Pause();
    public void Seek(double s) => engine?.SetCurrentTime(Math.Clamp(s, 0, Math.Max(0, Duration - 0.01)));
    public bool Seeking => engine != null && engine.IsSeeking();
    /// <summary>0–1</summary>
    public void SetVolume(double v) => engine?.SetVolume(Math.Clamp(v, 0, 1));
    public void SetMuted(bool muted) => engine?.SetMuted(muted);

    /// <summary>The part of the frame to show (0–1 across and down), or null for all of it: the editor's zoom</summary>
    public void SetView(RectangleF? part)
    {
        if (part == view) return;
        view = part;
        Resized();
    }

    RectangleF? view;
    IntPtr viewRect;

    /// <summary>Call when the host window changes size: the picture is redrawn to fill it</summary>
    public void Resized()
    {
        if (engine is not IMFMediaEngineEx ex) return;
        var r = new RECT { right = host.ClientSize.Width, bottom = host.ClientSize.Height };
        var black = 0xFF000000u;
        var src = IntPtr.Zero;
        if (view is { } v)
        {
            // MFVideoNormalizedRect: left, top, right, bottom as floats
            if (viewRect == IntPtr.Zero) viewRect = Marshal.AllocHGlobal(16);
            Marshal.Copy(new[] { v.Left, v.Top, v.Right, v.Bottom }, 0, viewRect, 4);
            src = viewRect;
        }
        ex.UpdateVideoStream(src, ref r, ref black);
    }

    /// <summary>
    /// Called on a Media Foundation thread, which must not call back into the engine (the engine object lives on
    /// the UI thread, so that would wait on it while it may be waiting on the engine): everything is posted.
    /// </summary>
    void OnEvent(uint e)
    {
        if (e is 5 or 8 or 9 or 10 or 13 or 17 or 19 or 21 or 1000)
            try { host.BeginInvoke(() => Handle(e)); } catch (InvalidOperationException) { }
    }

    void Handle(uint e)
    {
        if (engine == null) return;
        if (e == 10 && pendingSeek > 0.05) // LOADEDMETADATA: start where the thumbnail was scrubbed to
        {
            engine.SetCurrentTime(pendingSeek);
            pendingSeek = 0;
        }
        if (e == 5) // ERROR
        {
            var hr = engine.GetError(out var err);
            Error = hr >= 0 && err != null ? $"Can't play this clip (error {err.GetErrorCode()})" : "Can't play this clip";
        }
        if (e is 10 or 1000) Resized();
        Changed?.Invoke();
    }

    public void Dispose()
    {
        if (engine == null) return;
        try { engine.Shutdown(); } catch { }
        Marshal.ReleaseComObject(engine);
        engine = null;
        if (manager != null) Marshal.ReleaseComObject(manager);
        manager = null;
        if (viewRect != IntPtr.Zero) Marshal.FreeHGlobal(viewRect);
        viewRect = IntPtr.Zero;
    }

    // ---------- interop (mfmediaengine.h) ----------

    static readonly Guid MF_MEDIA_ENGINE_CALLBACK = new("c60381b8-83a4-41f8-a3d0-de05076849a9");
    static readonly Guid MF_MEDIA_ENGINE_PLAYBACK_HWND = new("d988879b-67c9-4d92-baa7-6eadd446039d");
    static readonly Guid MF_MEDIA_ENGINE_DXGI_MANAGER = new("065702da-1094-486d-8617-ee7cc4ee4648");
    static readonly Guid MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT = new("5066893c-8cf9-42bc-8b8a-472212e52726");

    [DllImport("mfplat.dll")] static extern int MFStartup(uint version, uint flags);
    [DllImport("mfplat.dll")] static extern int MFCreateAttributes(out IMFAttributes attributes, uint initialSize);

    [DllImport("mfplat.dll")] static extern int MFCreateDXGIDeviceManager(out uint token, out IMFDXGIDeviceManager manager);
    [DllImport("d3d11.dll")]
    static extern int D3D11CreateDevice(IntPtr adapter, int driverType, IntPtr software, uint flags, IntPtr featureLevels, uint levels, uint sdkVersion,
        out IntPtr device, out int featureLevel, out IntPtr context);

    [ComImport, Guid("eb533d5d-2db6-40f8-97a9-494692014f07"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFDXGIDeviceManager
    {
        void CloseDeviceHandle(); void GetVideoService(); void LockDevice(); void OpenDeviceHandle();
        [PreserveSig] int ResetDevice([MarshalAs(UnmanagedType.IUnknown)] object device, uint token);
        void TestDevice(); void UnlockDevice();
    }

    [ComImport, Guid("9b7e4e00-342c-4106-a19f-4f2704f689f0"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface ID3D10Multithread
    {
        void Enter(); void Leave();
        [PreserveSig] int SetMultithreadProtected([MarshalAs(UnmanagedType.Bool)] bool protect);
        void GetMultithreadProtected();
    }

    [StructLayout(LayoutKind.Sequential)]
    struct RECT { public int left, top, right, bottom; }

    [ComVisible(true)]
    sealed class Notify(MediaEngine owner) : IMFMediaEngineNotify
    {
        public int EventNotify(uint e, IntPtr param1, uint param2)
        {
            owner.OnEvent(e);
            return 0;
        }
    }

    [ComImport, Guid("fee7c112-e776-42b5-9bbf-0048524e2bd5"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMFMediaEngineNotify
    {
        [PreserveSig] int EventNotify(uint e, IntPtr param1, uint param2);
    }

    [ComImport, Guid("4D645ACE-26AA-4688-9BE1-DF3516990B93"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFMediaEngineClassFactory
    {
        [PreserveSig] int CreateInstance(uint flags, IMFAttributes attributes, out IMFMediaEngine engine);
        void CreateTimeRange(); void CreateError();
    }

    [ComImport, Guid("fc0e10d2-ab2a-4501-a951-06bb1075184c"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFMediaError
    {
        [PreserveSig] ushort GetErrorCode();
        void GetExtendedErrorCode(); void SetErrorCode(); void SetExtendedErrorCode();
    }

    // Every method in vtable order; only the ones used have real signatures
    [ComImport, Guid("98a1b0bb-03eb-4935-ae7c-93c1fa0e1c93"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFMediaEngine
    {
        [PreserveSig] int GetError(out IMFMediaError? error);
        void SetErrorCode(); void SetSourceElements();
        [PreserveSig] int SetSource(IntPtr url);
        void GetCurrentSource(); void GetNetworkState(); void GetPreload();
        [PreserveSig] int SetPreload(int preload);
        void GetBuffered(); void Load(); void CanPlayType();
        void GetReadyState();
        [PreserveSig][return: MarshalAs(UnmanagedType.Bool)] bool IsSeeking();
        [PreserveSig] double GetCurrentTime();
        [PreserveSig] int SetCurrentTime(double seconds);
        void GetStartTime();
        [PreserveSig] double GetDuration();
        [PreserveSig][return: MarshalAs(UnmanagedType.Bool)] bool IsPaused();
        void GetDefaultPlaybackRate(); void SetDefaultPlaybackRate(); void GetPlaybackRate(); void SetPlaybackRate(); void GetPlayed(); void GetSeekable();
        [PreserveSig][return: MarshalAs(UnmanagedType.Bool)] bool IsEnded();
        void GetAutoPlay();
        [PreserveSig] int SetAutoPlay([MarshalAs(UnmanagedType.Bool)] bool autoPlay);
        void GetLoop(); void SetLoop();
        [PreserveSig] int Play();
        [PreserveSig] int Pause();
        void GetMuted();
        [PreserveSig] int SetMuted([MarshalAs(UnmanagedType.Bool)] bool muted);
        void GetVolume();
        [PreserveSig] int SetVolume(double volume);
        void HasVideo(); void HasAudio();
        [PreserveSig] int GetNativeVideoSize(out uint x, out uint y);
        void GetVideoAspectRatio();
        [PreserveSig] int Shutdown();
        void TransferVideoFrame(); void OnVideoStreamTick();
    }

    [ComImport, Guid("83015ead-b1e6-40d0-a98a-37145ffe1ad1"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFMediaEngineEx
    {
        // IMFMediaEngine's 42 methods come first in the vtable
        void M1(); void M2(); void M3(); void M4(); void M5(); void M6(); void M7(); void M8(); void M9(); void M10();
        void M11(); void M12(); void M13(); void M14(); void M15(); void M16(); void M17(); void M18(); void M19(); void M20();
        void M21(); void M22(); void M23(); void M24(); void M25(); void M26(); void M27(); void M28(); void M29(); void M30();
        void M31(); void M32(); void M33(); void M34(); void M35(); void M36(); void M37(); void M38(); void M39(); void M40();
        void M41(); void M42();
        void SetSourceFromByteStream(); void GetStatistics();
        [PreserveSig] int UpdateVideoStream(IntPtr srcRect, ref RECT dstRect, ref uint borderColor);
    }

    [ComImport, Guid("2cd2d921-c447-44a7-a13c-4adabfc247e3"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMFAttributes
    {
        void GetItem(); void GetItemType(); void CompareItem(); void Compare(); void GetUINT32(); void GetUINT64();
        void GetDouble(); void GetGUID(); void GetStringLength(); void GetString(); void GetAllocatedString();
        void GetBlobSize(); void GetBlob(); void GetAllocatedBlob(); void GetUnknown(); void SetItem(); void DeleteItem(); void DeleteAllItems();
        [PreserveSig] int SetUINT32(ref Guid key, uint value);
        [PreserveSig] int SetUINT64(ref Guid key, ulong value);
        void SetDouble(); void SetGUID(); void SetString(); void SetBlob();
        [PreserveSig] int SetUnknown(ref Guid key, [MarshalAs(UnmanagedType.IUnknown)] object value);
        void LockStore(); void UnlockStore(); void GetCount(); void GetItemByIndex(); void CopyAllItems();
    }
}
