using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Clipback;

/// <summary>A zoom keyframe, the same as the web editor's and render.rs's Key</summary>
sealed class ZoomKey
{
    /// <summary>Seconds into the clip</summary>
    public double T;
    /// <summary>1 = no zoom</summary>
    public double Z = 1;
    /// <summary>Centre of the view, 0–1 across and down</summary>
    public double X = 0.5, Y = 0.5;
}

/// <summary>The view at a moment, worked out from the keyframes the same way as the export (ui/common.js keyValue)</summary>
readonly record struct ZoomView(double Z, double Cx, double Cy, double Ox, double Oy)
{
    public bool Zoomed => Z > 1.0005;
    /// <summary>The part of the frame on screen, 0–1</summary>
    public RectangleF Part => new((float)Ox, (float)Oy, (float)(1 / Z), (float)(1 / Z));

    public static ZoomView At(IReadOnlyList<ZoomKey> keys, double t)
    {
        var z = Math.Max(1, Value(keys, t, k => k.Z, 1));
        double cx = Value(keys, t, k => k.X, 0.5), cy = Value(keys, t, k => k.Y, 0.5);
        double ox = Math.Min(Math.Max(cx - 1 / (2 * z), 0), 1 - 1 / z), oy = Math.Min(Math.Max(cy - 1 / (2 * z), 0), 1 - 1 / z);
        return new ZoomView(z, cx, cy, ox, oy);
    }

    /// <summary>Smoothstep between keyframes, held before the first and after the last</summary>
    static double Value(IReadOnlyList<ZoomKey> keys, double t, Func<ZoomKey, double> field, double none)
    {
        if (keys.Count == 0) return none;
        if (t <= keys[0].T) return field(keys[0]);
        for (var i = 0; i < keys.Count - 1; i++)
        {
            ZoomKey a = keys[i], b = keys[i + 1];
            if (t < b.T)
            {
                var u = Math.Clamp((t - a.T) / Math.Max(1e-3, b.T - a.T), 0, 1);
                return field(a) + (field(b) - field(a)) * u * u * (3 - 2 * u);
            }
        }
        return field(keys[^1]);
    }

    /// <summary>"t,z,x,y;t,z,x,y" for the player process</summary>
    public static string Pack(IEnumerable<ZoomKey> keys) =>
        string.Join(";", keys.Select(k => string.Join(",", new[] { k.T, k.Z, k.X, k.Y }.Select(v => v.ToString("0.#####", CultureInfo.InvariantCulture)))));

    public static List<ZoomKey> Unpack(string s)
    {
        var list = new List<ZoomKey>();
        foreach (var part in s.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var v = part.Split(',').Select(x => double.Parse(x, CultureInfo.InvariantCulture)).ToArray();
            if (v.Length == 4) list.Add(new ZoomKey { T = v[0], Z = v[1], X = v[2], Y = v[3] });
        }
        return list;
    }
}

/// <summary>One audio track's mix, as /api/render takes it</summary>
sealed class TrackMix
{
    public int Index;
    /// <summary>0–2</summary>
    public double Volume = 1;
    public bool Muted;
}

/// <summary>An export's state from /api/job</summary>
sealed class RenderJob
{
    public double Progress;
    public bool Done, Copied;
    public string? Error, Id, Path;
}

static partial class Api
{
    /// <summary>Starts an export (src/render.rs): the same JSON the web editor sends. Returns the job number.</summary>
    public static async Task<long> Render(string id, double start, double end, string preset, IEnumerable<ZoomKey> zoom, IEnumerable<TrackMix> tracks, bool copy)
    {
        using var buf = new MemoryStream();
        using (var w = new Utf8JsonWriter(buf))
        {
            w.WriteStartObject();
            w.WriteString("id", id);
            w.WriteNumber("start", start);
            w.WriteNumber("end", end);
            w.WriteString("preset", preset);
            w.WriteStartArray("zoom");
            foreach (var k in zoom)
            {
                w.WriteStartObject();
                w.WriteNumber("t", k.T); w.WriteNumber("z", k.Z); w.WriteNumber("x", k.X); w.WriteNumber("y", k.Y);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteStartArray("tracks");
            foreach (var t in tracks)
            {
                w.WriteStartObject();
                w.WriteNumber("index", t.Index); w.WriteNumber("volume", t.Volume); w.WriteBoolean("muted", t.Muted);
                w.WriteEndObject();
            }
            w.WriteEndArray();
            w.WriteBoolean("copy", copy);
            w.WriteEndObject();
        }
        HttpResponseMessage r;
        try { r = await http.PostAsync("/api/render", new ByteArrayContent(buf.ToArray()) { Headers = { { "Content-Type", "application/json" } } }); }
        catch (HttpRequestException) { throw new ApiException("clipback isn't running"); }
        using (r)
        {
            var text = await r.Content.ReadAsStringAsync();
            using var doc = Parse(text);
            var root = doc?.RootElement;
            if (root?.TryGetProperty("error", out var e) == true) throw new ApiException(e.GetString() ?? "Export failed");
            if (!r.IsSuccessStatusCode || root?.TryGetProperty("job", out var job) != true) throw new ApiException(r.ReasonPhrase ?? "Export failed");
            return job.GetInt64();
        }
    }

    public static async Task<RenderJob> Job(long job)
    {
        using var r = await Send(HttpMethod.Get, $"/api/job?job={job}");
        using var doc = Parse(await r.Content.ReadAsStringAsync()) ?? throw new ApiException("Bad reply from clipback");
        var j = doc.RootElement;
        string? S(string k) => j.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        bool B(string k) => j.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.True;
        return new RenderJob
        {
            Progress = j.TryGetProperty("progress", out var p) && p.ValueKind == JsonValueKind.Number ? p.GetDouble() : 0,
            Done = B("done"), Copied = B("copied"), Error = S("error"), Id = S("id"), Path = S("path"),
        };
    }

    static JsonDocument? Parse(string text)
    {
        try { return JsonDocument.Parse(text); } catch { return null; }
    }

    /// <summary>One audio track of a clip on its own (cached by clipback), for the editor's per-track volume</summary>
    public static string TrackUrl(string id, int index) => $"{Base}/api/track?id={Enc(id)}&i={index}";
    public static string SpriteUrl(string id) => $"/api/sprite?id={Enc(id)}";
}

static class EdFmt
{
    /// <summary>"1:05.3", like the web editor's fmtTime(s, true)</summary>
    public static string Time(double s)
    {
        if (!double.IsFinite(s) || s < 0) s = 0;
        var tenths = (long)Math.Round(s * 10);
        long h = tenths / 36000, m = tenths / 600 % 60;
        var sec = (tenths % 600) / 10.0;
        var secs = sec.ToString("00.0", CultureInfo.InvariantCulture);
        return h > 0 ? $"{h}:{m:00}:{secs}" : $"{m}:{secs}";
    }
}

static partial class Icons
{
    /// <summary>The speaker icons the editor's mute buttons use (Lucide volume-2 / volume-x), added on first use</summary>
    public static void AddEditorIcons()
    {
        const string speaker = "<path d='M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z' />";
        Svg.TryAdd("volume-2", speaker + "<path d='M16 9a5 5 0 0 1 0 6' /><path d='M19.364 18.364a9 9 0 0 0 0-12.728' />");
        Svg.TryAdd("volume-x", speaker + "<line x1='22' x2='16' y1='9' y2='15' /><line x1='16' x2='22' y1='9' y2='15' />");
    }
}
