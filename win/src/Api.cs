using System.Diagnostics;
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;

namespace Clipback;

// The JSON clipback.exe sends (src/server.rs, src/clips.rs). Field names are snake_case there.

sealed class Status
{
    public bool Paused { get; set; }
    public bool Live { get; set; }
    public long? OldestUs { get; set; }
    public long? NewestUs { get; set; }
    public long RingBytes { get; set; }
    public long RingCap { get; set; }
    public List<LiveAudio> Audio { get; set; } = [];
    public string ClipsDir { get; set; } = "";
    /// <summary>[Mbps, game exe or null] while recording</summary>
    public JsonElement? Encode { get; set; }
    public int NewApps { get; set; }
    public long NowUs { get; set; }
}

sealed class LiveAudio
{
    public string Exe { get; set; } = "";
    public string? Track { get; set; }
    public long SinceUs { get; set; }
}

sealed class Clip
{
    /// <summary>Path under the clips folder with forward slashes, e.g. "Clips 2026/Overwatch 2026-11-15 21-04-10.mp4"</summary>
    public string Id { get; set; } = "";
    public string Name { get; set; } = "";
    public long Size { get; set; }
    /// <summary>Modified time, ms since 1970</summary>
    public long Mtime { get; set; }
    public ClipInfo? Info { get; set; }
    public string Game { get; set; } = "";
    public string? GameExe { get; set; }
}

sealed class ClipInfo
{
    public double Duration { get; set; }
    public int Width { get; set; }
    public int Height { get; set; }
    public double Fps { get; set; }
    public List<string> Tracks { get; set; } = [];
    public string? Game { get; set; }
    public string? GameExe { get; set; }
}

sealed class SaveResult
{
    public string Path { get; set; } = "";
    public string? Id { get; set; }
    public List<string> Tracks { get; set; } = [];
}

[JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.SnakeCaseLower)]
[JsonSerializable(typeof(Status))]
[JsonSerializable(typeof(List<Clip>))]
[JsonSerializable(typeof(ClipInfo))]
[JsonSerializable(typeof(SaveResult))]
[JsonSerializable(typeof(JsonElement))]
partial class Json : JsonSerializerContext;

/// <summary>An error clipback sent back ({"error": "…"}), or "clipback isn't running"</summary>
sealed class ApiException(string message) : Exception(message);

/// <summary>The local HTTP API of clipback.exe, the only link between the recorder and this window</summary>
static partial class Api
{
    public static readonly int Port = ReadPort();
    public static string Base => $"http://127.0.0.1:{Port}";

    static readonly HttpClient http = new(new SocketsHttpHandler { UseProxy = false, PooledConnectionLifetime = TimeSpan.FromMinutes(5) })
    {
        BaseAddress = new Uri($"http://127.0.0.1:{Port}"),
        Timeout = TimeSpan.FromSeconds(60),
    };

    [GeneratedRegex(@"^\s*port\s*=\s*(\d+)")] private static partial Regex PortRx();

    /// <summary>`port` from %APPDATA%\clipback\config.toml (top level, before any [section]); 47820 if unset</summary>
    static int ReadPort()
    {
        try
        {
            var file = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "clipback", "config.toml");
            foreach (var line in File.ReadLines(file))
            {
                if (line.TrimStart().StartsWith('[')) break;
                var m = PortRx().Match(line);
                if (m.Success && int.TryParse(m.Groups[1].Value, out var p) && p is > 0 and < 65536) return p;
            }
        }
        catch { }
        return 47820;
    }

    public static string Enc(string s) => Uri.EscapeDataString(s);

    static async Task<HttpResponseMessage> Send(HttpMethod method, string path, CancellationToken ct = default)
    {
        HttpResponseMessage r;
        try { r = await http.SendAsync(new HttpRequestMessage(method, path), ct); }
        catch (HttpRequestException) { throw new ApiException("clipback isn't running"); }
        if (!r.IsSuccessStatusCode)
        {
            var text = await r.Content.ReadAsStringAsync(ct);
            var msg = r.ReasonPhrase ?? $"HTTP {(int)r.StatusCode}";
            try { if (JsonDocument.Parse(text).RootElement.TryGetProperty("error", out var e)) msg = e.GetString() ?? msg; }
            catch { if (text.Length is > 0 and < 300) msg = text; }
            r.Dispose();
            throw new ApiException(msg);
        }
        return r;
    }

    static async Task<T> Get<T>(string path, System.Text.Json.Serialization.Metadata.JsonTypeInfo<T> type, CancellationToken ct = default)
    {
        using var r = await Send(HttpMethod.Get, path, ct);
        return (await r.Content.ReadFromJsonAsync(type, ct))!;
    }

    static async Task Post(string path)
    {
        using var _ = await Send(HttpMethod.Post, path);
    }

    public static Task<Status> Status() => Get("/api/status", Json.Default.Status);
    public static Task<List<Clip>> Clips() => Get("/api/clips", Json.Default.ListClip);
    public static Task<ClipInfo> Info(string id) => Get($"/api/info?id={Enc(id)}", Json.Default.ClipInfo);
    public static Task Pause(bool pause) => Post(pause ? "/api/pause" : "/api/resume");
    public static Task Copy(string id) => Post($"/api/copy?id={Enc(id)}");
    public static Task Reveal(string id) => Post($"/api/reveal?id={Enc(id)}");

    public static async Task<SaveResult> Save(int secs)
    {
        using var r = await Send(HttpMethod.Post, $"/api/save?secs={secs}");
        return (await r.Content.ReadFromJsonAsync(Json.Default.SaveResult))!;
    }

    /// <summary>A file's bytes (sprite JPEG, icon PNG), or null if there isn't one</summary>
    public static async Task<byte[]?> Bytes(string path, CancellationToken ct = default)
    {
        try
        {
            using var r = await http.GetAsync(path, ct);
            return r.IsSuccessStatusCode ? await r.Content.ReadAsByteArrayAsync(ct) : null;
        }
        catch (HttpRequestException) { return null; }
    }

    /// <summary>The scrub strip: SPRITE_FRAMES (24) frames side by side, each 320 px wide (src/clips.rs)</summary>
    public const int SpriteFrames = 24;
    public static string SpriteUrl(Clip c) => $"/api/sprite?id={Enc(c.Id)}&m={c.Mtime}";
    public static string IconUrl(string exe) => $"/api/icon?exe={Enc(exe)}";

    /// <summary>Opens a page of the web UI in the default browser (temporary, until it's native too)</summary>
    public static void OpenPage(string pathAndQuery)
    {
        try { Process.Start(new ProcessStartInfo(Base + pathAndQuery) { UseShellExecute = true })?.Dispose(); } catch { }
    }
}
