using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Clipback;

/// <summary>One row of /api/apps (src/apps.rs AppRow)</summary>
sealed record AppInfo(string Exe, string Category, long? LastHeardUs, bool New);

/// <summary>The routes the Options screen uses. Settings stay a JSON tree, so fields this window doesn't know
/// about go back to clipback unchanged when it saves.</summary>
static partial class Api
{
    /// <summary>Like Send, with an optional JSON body</summary>
    static async Task<HttpResponseMessage> SendJson(HttpMethod method, string path, JsonNode? body = null)
    {
        using var req = new HttpRequestMessage(method, path);
        if (body != null) req.Content = new StringContent(body.ToJsonString(), Encoding.UTF8, "application/json");
        HttpResponseMessage r;
        try { r = await http.SendAsync(req); }
        catch (HttpRequestException) { throw new ApiException("clipback isn't running"); }
        if (!r.IsSuccessStatusCode)
        {
            var text = await r.Content.ReadAsStringAsync();
            var msg = r.ReasonPhrase ?? $"HTTP {(int)r.StatusCode}";
            try { if (JsonDocument.Parse(text).RootElement.TryGetProperty("error", out var e)) msg = e.GetString() ?? msg; }
            catch { if (text.Length is > 0 and < 300) msg = text; }
            r.Dispose();
            throw new ApiException(msg);
        }
        return r;
    }

    public static async Task<JsonNode> GetJson(string path)
    {
        using var r = await SendJson(HttpMethod.Get, path);
        var text = await r.Content.ReadAsStringAsync();
        return JsonNode.Parse(text) ?? throw new ApiException("empty answer from clipback");
    }

    static async Task PostJson(string path, JsonNode? body = null)
    {
        using var _ = await SendJson(HttpMethod.Post, path, body);
    }

    /// <summary>{"config": {…}, "path": "…\config.toml"}</summary>
    public static Task<JsonNode> Settings() => GetJson("/api/settings");
    public static Task SaveSettings(JsonNode config) => PostJson("/api/settings", config);
    public static Task<JsonNode> Monitors() => GetJson("/api/monitors");
    public static Task<JsonNode> Encoders() => GetJson("/api/encoders");
    public static Task SetCategory(string exe, string category) => PostJson($"/api/app-category?exe={Enc(exe)}&category={Enc(category)}");
    public static Task SetGameBitrate(string exe, string mbps) => PostJson($"/api/game-bitrate?exe={Enc(exe)}&mbps={Enc(mbps)}");
    public static Task RevealApp(string exe) => PostJson($"/api/app-reveal?exe={Enc(exe)}");

    /// <summary>Every app clipback knows (new ones first), and the exes playing sound right now</summary>
    public static async Task<(List<AppInfo> apps, HashSet<string> live)> Apps()
    {
        var j = await GetJson("/api/apps");
        var apps = new List<AppInfo>();
        foreach (var a in j["apps"]?.AsArray() ?? [])
        {
            if (a == null) continue;
            var heard = a["last_heard"] is JsonValue v && v.TryGetValue<long>(out var us) ? us : (long?)null;
            apps.Add(new AppInfo((string?)a["exe"] ?? "", (string?)a["category"] ?? "app", heard, (bool?)a["new"] ?? false));
        }
        var live = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var a in j["live"]?.AsArray() ?? [])
            if ((string?)a?["exe"] is { } exe) live.Add(exe);
        return (apps, live);
    }
}
