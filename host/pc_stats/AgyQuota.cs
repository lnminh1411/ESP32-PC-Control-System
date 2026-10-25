// esp32-agyquota: Per-user helper querying Google Antigravity local language servers
// and writing model quota statistics to C:\ProgramData\ESP32PcStats\agy_quota.txt
// for consumption by esp32-pcstats.exe via @source.
//
// Also keeps the Claude Code OAuth token fresh: shortly before it expires it
// does the same refresh Claude Code does and writes the new token back to
// ~/.claude/.credentials.json (only the token fields). And it reads the
// Antigravity Flash quota (5-hour / weekly) from Google with agy's stored
// login (read-only; an expired access token is renewed in memory only).
// Tokens are never logged.
//
// Target: .NET Framework 4 (C# 5 syntax only).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Management;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using Microsoft.Win32.SafeHandles;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using Windows.Media.Control;
using Windows.Storage.Streams;

static class AgyQuota {
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool AttachConsole(int dwProcessId);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern IntPtr GetStdHandle(int nStdHandle);

  const int ATTACH_PARENT_PROCESS = -1;
  const int STD_OUTPUT_HANDLE = -11;

  [DllImport("iphlpapi.dll", SetLastError = true)]
  static extern uint GetExtendedTcpTable(
      IntPtr pTcpTable,
      ref int pdwSize,
      bool bOrder,
      int ulAf,
      int TableClass,
      uint reserved);

  const int AF_INET = 2;
  const int TCP_TABLE_OWNER_PID_ALL = 5;
  const int MIB_TCP_STATE_LISTEN = 2;
  const uint ERROR_INSUFFICIENT_BUFFER = 122;

  static readonly DateTime UnixEpoch = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);

  class ModelFamily {
    public string Name;
    public Regex Pattern;
  }

  class FamilyState {
    public string FamilyKey;
    public string DisplayName;
    public bool HasQuota;
    public double RemainingFraction;
    public DateTime? ResetTimeUtc;
  }

  class AccountState {
    public string Email;
    public string Plan;
    public DateTime LastFetchedUtc;
    public bool Live;   // a running client answered for this account in the last poll
    public Dictionary<string, FamilyState> Families = new Dictionary<string, FamilyState>(StringComparer.OrdinalIgnoreCase);
  }

  class ConfigData {
    public int IntervalMs = 60000;
    public List<string> Accounts = new List<string>();
    public Regex ExcludeRegex = new Regex("claude|gpt|oss", RegexOptions.IgnoreCase);
    public string OutputPath = @"C:\ProgramData\ESP32PcStats\agy_quota.txt";
    public List<ModelFamily> Families = new List<ModelFamily>();
  }

  struct ClientProcess {
    public int Pid;
    public string Csrf;
  }

  class RawModelConfig {
    public string Label;
    public bool Disabled;
    public double? RemainingFraction;
    public DateTime? ResetTimeUtc;
  }

  class ParsedUserStatus {
    public string Email;
    public string PlanName;
    public Dictionary<string, FamilyState> Families = new Dictionary<string, FamilyState>(StringComparer.OrdinalIgnoreCase);
  }

  // ---- Claude Code token keep-alive ----
  const double ClaudeRefreshLeadMin = 10;      // the CLI only refreshes a token close to expiry
  const double ClaudeRetryMin = 5;             // between attempts while it keeps failing
  static DateTime lastClaudeAttemptUtc = DateTime.MinValue;
  static DateTime lastRefreshWarnUtc = DateTime.MinValue;

  // expiresAt / refreshTokenExpiresAt (UTC) from .credentials.json; false if unreadable
  static bool ReadClaudeExpiry(out DateTime accessUtc, out DateTime refreshUtc) {
    accessUtc = refreshUtc = DateTime.MinValue;
    string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), @".claude\.credentials.json");
    if (!File.Exists(path)) return false;
    try {
      Dictionary<string, object> root = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(path)) as Dictionary<string, object>;
      object o;
      if (root == null || !root.TryGetValue("claudeAiOauth", out o)) return false;
      Dictionary<string, object> oauth = o as Dictionary<string, object>;
      if (oauth == null || !oauth.TryGetValue("expiresAt", out o)) return false;
      accessUtc = UnixEpoch.AddMilliseconds(Convert.ToDouble(o, CultureInfo.InvariantCulture));
      if (oauth.TryGetValue("refreshTokenExpiresAt", out o) && o != null)
        refreshUtc = UnixEpoch.AddMilliseconds(Convert.ToDouble(o, CultureInfo.InvariantCulture));
      return true;
    } catch (Exception ex) {
      Log("claude: cannot read token expiry: " + ex.GetType().Name);   // never the content
      return false;
    }
  }

  const string ClaudeTokenUrl = "https://platform.claude.com/v1/oauth/token";
  const string ClaudeClientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";   // Claude Code's public client

  static void ClaudeKeepAlive() {
    DateTime accessUtc, refreshUtc;
    if (!ReadClaudeExpiry(out accessUtc, out refreshUtc)) return;
    DateTime now = DateTime.UtcNow;

    if (refreshUtc != DateTime.MinValue && refreshUtc - now < TimeSpan.FromDays(2) && now - lastRefreshWarnUtc > TimeSpan.FromHours(12)) {
      lastRefreshWarnUtc = now;
      Log("claude: refresh token expires " + refreshUtc.ToLocalTime().ToString("ddd HH:mm", CultureInfo.InvariantCulture) +
          " -- run `claude auth login` before then");
    }
    if (accessUtc - now > TimeSpan.FromMinutes(ClaudeRefreshLeadMin)) return;
    if (now - lastClaudeAttemptUtc < TimeSpan.FromMinutes(ClaudeRetryMin)) return;
    lastClaudeAttemptUtc = now;

    try {
      RefreshClaudeToken();
    } catch (WebException ex) {
      HttpWebResponse r = ex.Response as HttpWebResponse;
      int code = r != null ? (int)r.StatusCode : 0;
      Log("claude: token refresh failed (" + (code != 0 ? code.ToString(CultureInfo.InvariantCulture) : ex.Status.ToString()) + ")" +
          (code == 400 || code == 401 ? " -- signed out? run `claude auth login`" : ""));
    } catch (Exception ex) {
      Log("claude: token refresh error: " + ex.GetType().Name);   // never the content
    }
  }

  // Same request Claude Code makes. The refresh token rotates, so the new one
  // is written back at once; if another process refreshed meanwhile, its
  // token wins and ours is dropped.
  static void RefreshClaudeToken() {
    string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), @".claude\.credentials.json");
    JavaScriptSerializer js = new JavaScriptSerializer();
    Dictionary<string, object> oauth = ClaudeOauth(js, path);
    if (oauth == null || !oauth.ContainsKey("refreshToken")) return;
    string usedRefresh = Convert.ToString(oauth["refreshToken"], CultureInfo.InvariantCulture);

    List<string> scopes = new List<string>();
    object sc;
    if (oauth.TryGetValue("scopes", out sc) && sc is System.Collections.IEnumerable && !(sc is string))
      foreach (object o in (System.Collections.IEnumerable)sc) scopes.Add(Convert.ToString(o, CultureInfo.InvariantCulture));

    Dictionary<string, object> req = new Dictionary<string, object>();
    req["grant_type"] = "refresh_token";
    req["refresh_token"] = usedRefresh;
    req["client_id"] = ClaudeClientId;
    if (scopes.Count > 0) req["scope"] = string.Join(" ", scopes.ToArray());
    Dictionary<string, object> resp = js.DeserializeObject(PostJson(ClaudeTokenUrl, js.Serialize(req), null)) as Dictionary<string, object>;
    if (resp == null || !resp.ContainsKey("access_token") || !resp.ContainsKey("expires_in")) {
      Log("claude: token refresh returned no token");
      return;
    }

    // re-read: write only onto the state we refreshed from
    Dictionary<string, object> root = js.DeserializeObject(File.ReadAllText(path)) as Dictionary<string, object>;
    oauth = root != null && root.ContainsKey("claudeAiOauth") ? root["claudeAiOauth"] as Dictionary<string, object> : null;
    if (oauth == null || Convert.ToString(oauth["refreshToken"], CultureInfo.InvariantCulture) != usedRefresh) {
      Log("claude: credentials changed during refresh, keeping the other process's token");
      return;
    }
    long nowMs = (long)(DateTime.UtcNow - UnixEpoch).TotalMilliseconds;
    oauth["accessToken"] = resp["access_token"];
    if (resp.ContainsKey("refresh_token") && resp["refresh_token"] != null) oauth["refreshToken"] = resp["refresh_token"];
    long expiresAt = nowMs + (long)(Convert.ToDouble(resp["expires_in"], CultureInfo.InvariantCulture) * 1000);
    oauth["expiresAt"] = expiresAt;
    if (resp.ContainsKey("refresh_token_expires_in") && resp["refresh_token_expires_in"] != null)
      oauth["refreshTokenExpiresAt"] = nowMs + (long)(Convert.ToDouble(resp["refresh_token_expires_in"], CultureInfo.InvariantCulture) * 1000);

    string tmp = path + ".tmp." + Guid.NewGuid().ToString("N");
    File.WriteAllText(tmp, js.Serialize(root), new UTF8Encoding(false));
    File.Replace(tmp, path, null);
    Log("claude: token refreshed, valid until " + UnixEpoch.AddMilliseconds(expiresAt).ToLocalTime().ToString("ddd HH:mm", CultureInfo.InvariantCulture));
  }

  static Dictionary<string, object> ClaudeOauth(JavaScriptSerializer js, string path) {
    Dictionary<string, object> root = js.DeserializeObject(File.ReadAllText(path)) as Dictionary<string, object>;
    return root != null && root.ContainsKey("claudeAiOauth") ? root["claudeAiOauth"] as Dictionary<string, object> : null;
  }

  static string PostJson(string url, string body, Dictionary<string, string> headers) {
    ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;   // TLS 1.2
    HttpWebRequest req = (HttpWebRequest)WebRequest.Create(url);
    req.Method = "POST";
    req.ContentType = "application/json";
    req.Timeout = 15000;
    req.ReadWriteTimeout = 15000;
    if (headers != null) {
      foreach (KeyValuePair<string, string> h in headers) {
        if (h.Key == "User-Agent") req.UserAgent = h.Value;
        else req.Headers[h.Key] = h.Value;
      }
    }
    byte[] data = Encoding.UTF8.GetBytes(body);
    req.ContentLength = data.Length;
    using (Stream rs = req.GetRequestStream()) rs.Write(data, 0, data.Length);
    using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
    using (StreamReader rd = new StreamReader(resp.GetResponseStream(), Encoding.UTF8)) return rd.ReadToEnd();
  }

  // ---- Antigravity Flash quota from Google (agy's stored login) ----
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct CREDENTIAL {
    public uint Flags, Type;
    public string TargetName, Comment;
    public System.Runtime.InteropServices.ComTypes.FILETIME LastWritten;
    public uint CredentialBlobSize;
    public IntPtr CredentialBlob;
    public uint Persist, AttributeCount;
    public IntPtr Attributes;
    public string TargetAlias, UserName;
  }
  [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool CredReadW(string target, uint type, uint flags, out IntPtr credential);
  [DllImport("advapi32.dll")]
  static extern void CredFree(IntPtr buffer);

  class CloudQuota {
    public double FivePct = double.NaN, WeekPct = double.NaN;   // remaining %
    public DateTime? FiveReset, WeekReset;
  }

  const string CloudBase = "https://cloudcode-pa.googleapis.com/v1internal:";
  const string CloudUserAgent = "antigravity-cli/1.4.7 windows/amd64";
  static readonly Dictionary<string, CloudQuota> cloudQuota = new Dictionary<string, CloudQuota>(StringComparer.OrdinalIgnoreCase);
  static readonly Dictionary<string, string> cloudProject = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
  static DateTime lastCloudPollUtc = DateTime.MinValue;
  static string memToken, memTokenFor;     // access token renewed here, never written anywhere
  static DateTime memTokenUntil;
  static string[] agyClient;               // {id, secret} read from agy.exe, kept out of the repo
  static bool cloudErrLogged;

  // agy's login blob: {"token":{access_token, refresh_token, expiry, ...}, "id_token_email"}
  static Dictionary<string, object> ReadAgyLogin() {
    IntPtr p;
    if (!CredReadW("gemini:antigravity", 1, 0, out p)) return null;
    try {
      CREDENTIAL c = (CREDENTIAL)Marshal.PtrToStructure(p, typeof(CREDENTIAL));
      byte[] b = new byte[c.CredentialBlobSize];
      Marshal.Copy(c.CredentialBlob, b, 0, b.Length);
      return new JavaScriptSerializer().DeserializeObject(Encoding.UTF8.GetString(b)) as Dictionary<string, object>;
    } finally {
      CredFree(p);
    }
  }

  // agy's installed-app OAuth clients (id + secret pairs) out of agy.exe
  static List<string[]> AgyClientCandidates() {
    List<string[]> list = new List<string[]>();
    string exe = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"agy\bin\agy.exe");
    if (!File.Exists(exe)) return list;
    string text = Encoding.ASCII.GetString(File.ReadAllBytes(exe));
    List<string> ids = new List<string>(), secrets = new List<string>();
    foreach (Match m in Regex.Matches(text, @"[0-9]{6,}-[a-z0-9]{20,}\.apps\.googleusercontent\.com"))
      if (!ids.Contains(m.Value)) ids.Add(m.Value);
    foreach (Match m in Regex.Matches(text, @"GOCSPX-[A-Za-z0-9_-]{28}"))
      if (!secrets.Contains(m.Value)) secrets.Add(m.Value);
    foreach (string id in ids)
      foreach (string sec in secrets) list.Add(new string[] { id, sec });
    return list;
  }

  // Google does not rotate the refresh token here, so agy's stored login
  // stays valid and untouched
  static string RefreshAgyToken(string refreshToken) {
    List<string[]> tries = agyClient != null ? new List<string[]> { agyClient } : AgyClientCandidates();
    foreach (string[] c in tries) {
      try {
        ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
        HttpWebRequest req = (HttpWebRequest)WebRequest.Create("https://oauth2.googleapis.com/token");
        req.Method = "POST";
        req.ContentType = "application/x-www-form-urlencoded";
        req.Timeout = 15000;
        byte[] data = Encoding.ASCII.GetBytes("grant_type=refresh_token&client_id=" + Uri.EscapeDataString(c[0]) +
            "&client_secret=" + Uri.EscapeDataString(c[1]) + "&refresh_token=" + Uri.EscapeDataString(refreshToken));
        req.ContentLength = data.Length;
        using (Stream rs = req.GetRequestStream()) rs.Write(data, 0, data.Length);
        string body;
        using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
        using (StreamReader rd = new StreamReader(resp.GetResponseStream(), Encoding.UTF8)) body = rd.ReadToEnd();
        Dictionary<string, object> r = new JavaScriptSerializer().DeserializeObject(body) as Dictionary<string, object>;
        if (r == null || !r.ContainsKey("access_token")) continue;
        agyClient = c;
        memTokenUntil = DateTime.UtcNow.AddSeconds(Convert.ToDouble(r["expires_in"], CultureInfo.InvariantCulture) - 120);
        return Convert.ToString(r["access_token"], CultureInfo.InvariantCulture);
      } catch (WebException) {
        // wrong pair (invalid_client) or signed out: try the next one
      }
    }
    return null;
  }

  static void PollCloudQuota() {
    if (DateTime.UtcNow - lastCloudPollUtc < TimeSpan.FromMinutes(2)) return;
    lastCloudPollUtc = DateTime.UtcNow;
    try {
      Dictionary<string, object> login = ReadAgyLogin();
      if (login == null || !login.ContainsKey("token")) return;
      string email = login.ContainsKey("id_token_email") ? Convert.ToString(login["id_token_email"], CultureInfo.InvariantCulture) : "";
      Dictionary<string, object> tok = login["token"] as Dictionary<string, object>;
      if (tok == null || string.IsNullOrEmpty(email)) return;

      // agy's own access token while it is valid, else one renewed in memory
      string token = null;
      DateTime expiry;
      if (tok.ContainsKey("expiry") && DateTime.TryParse(Convert.ToString(tok["expiry"], CultureInfo.InvariantCulture), CultureInfo.InvariantCulture,
              DateTimeStyles.AdjustToUniversal, out expiry) && expiry - DateTime.UtcNow > TimeSpan.FromMinutes(2)) {
        token = Convert.ToString(tok["access_token"], CultureInfo.InvariantCulture);
      } else if (memToken != null && memTokenFor == email && DateTime.UtcNow < memTokenUntil) {
        token = memToken;
      } else if (tok.ContainsKey("refresh_token")) {
        token = memToken = RefreshAgyToken(Convert.ToString(tok["refresh_token"], CultureInfo.InvariantCulture));
        memTokenFor = email;
      }
      if (token == null) {
        if (!cloudErrLogged) { cloudErrLogged = true; Log("agy cloud: no usable token (signed out of agy?)"); }
        return;
      }

      Dictionary<string, string> h = new Dictionary<string, string>();
      h["Authorization"] = "Bearer " + token;
      h["User-Agent"] = CloudUserAgent;
      JavaScriptSerializer js = new JavaScriptSerializer();

      string project;
      if (!cloudProject.TryGetValue(email, out project)) {
        Dictionary<string, object> lc = js.DeserializeObject(PostJson(CloudBase + "loadCodeAssist",
            "{\"metadata\":{\"ideType\":\"ANTIGRAVITY\",\"platform\":\"WINDOWS_AMD64\",\"pluginType\":\"GEMINI\"}}", h)) as Dictionary<string, object>;
        object po = lc != null && lc.ContainsKey("cloudaicompanionProject") ? lc["cloudaicompanionProject"] : null;
        Dictionary<string, object> pd = po as Dictionary<string, object>;
        project = pd != null ? GetString(pd, "id") : Convert.ToString(po, CultureInfo.InvariantCulture);
        if (string.IsNullOrEmpty(project)) return;
        cloudProject[email] = project;
      }
      Dictionary<string, object> pb = new Dictionary<string, object>();
      pb["project"] = project;
      string projBody = js.Serialize(pb);
      CloudQuota q = new CloudQuota();

      // Gemini group of the quota summary, when Google sends one
      Dictionary<string, object> sum = js.DeserializeObject(PostJson(CloudBase + "retrieveUserQuotaSummary", projBody, h)) as Dictionary<string, object>;
      if (sum != null && sum.ContainsKey("groups")) {
        foreach (object go in (System.Collections.IEnumerable)sum["groups"]) {
          Dictionary<string, object> g = go as Dictionary<string, object>;
          if (g == null || !g.ContainsKey("buckets")) continue;
          string gname = (GetString(g, "displayName") ?? "") + " " + (GetString(g, "description") ?? "");
          if (gname.IndexOf("Gemini", StringComparison.OrdinalIgnoreCase) < 0) continue;
          foreach (object bo in (System.Collections.IEnumerable)g["buckets"]) {
            Dictionary<string, object> bk = bo as Dictionary<string, object>;
            if (bk == null) continue;
            double frac = GetDouble(bk, "remainingFraction") ?? 1.0;
            DateTime? reset = ParseUtc(GetString(bk, "resetTime"));
            string win = GetString(bk, "window") ?? "";
            if (win == "5h") { q.FivePct = frac * 100; q.FiveReset = reset; }
            else if (win == "weekly") { q.WeekPct = frac * 100; q.WeekReset = reset; }
          }
        }
      }

      // else the per-model value: Google returns the tighter of the two
      // windows, and its reset time says which one it is
      if (double.IsNaN(q.FivePct) && double.IsNaN(q.WeekPct)) {
        Dictionary<string, object> fm = js.DeserializeObject(PostJson(CloudBase + "fetchAvailableModels", projBody, h)) as Dictionary<string, object>;
        Dictionary<string, object> models = fm != null && fm.ContainsKey("models") ? fm["models"] as Dictionary<string, object> : null;
        if (models != null) {
          foreach (KeyValuePair<string, object> kv in models) {
            if (kv.Key.IndexOf("flash", StringComparison.OrdinalIgnoreCase) < 0 || kv.Key.IndexOf("lite", StringComparison.OrdinalIgnoreCase) >= 0) continue;
            Dictionary<string, object> m = kv.Value as Dictionary<string, object>;
            Dictionary<string, object> qi = m != null && m.ContainsKey("quotaInfo") ? m["quotaInfo"] as Dictionary<string, object> : null;
            double? frac = GetDouble(qi, "remainingFraction");
            DateTime? reset = ParseUtc(GetString(qi, "resetTime"));
            if (frac == null || reset == null) continue;
            if (reset.Value - DateTime.UtcNow <= TimeSpan.FromHours(5.1)) { q.FivePct = frac.Value * 100; q.FiveReset = reset; }
            else { q.WeekPct = frac.Value * 100; q.WeekReset = reset; }
            break;
          }
        }
      }
      lock (cloudQuota) cloudQuota[email] = q;
      cloudErrLogged = false;
    } catch (WebException ex) {
      HttpWebResponse r = ex.Response as HttpWebResponse;
      if (r != null && (int)r.StatusCode == 401) memToken = null;
      if (!cloudErrLogged) {
        cloudErrLogged = true;
        Log("agy cloud: quota request failed (" + (r != null ? ((int)r.StatusCode).ToString(CultureInfo.InvariantCulture) : ex.Status.ToString()) + ")");
      }
    } catch (Exception ex) {
      if (!cloudErrLogged) { cloudErrLogged = true; Log("agy cloud: " + ex.GetType().Name); }
    }
  }

  static DateTime? ParseUtc(string s) {
    DateTime dt;
    if (!string.IsNullOrEmpty(s) && DateTime.TryParse(s, CultureInfo.InvariantCulture,
            DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out dt)) return dt;
    return null;
  }

  static void Log(string message) {
    try {
      string logDir = @"C:\ProgramData\ESP32PcStats";
      if (!Directory.Exists(logDir)) Directory.CreateDirectory(logDir);
      string logPath = Path.Combine(logDir, "agy_quota.log");
      string oldPath = logPath + ".old";

      if (File.Exists(logPath)) {
        FileInfo fi = new FileInfo(logPath);
        if (fi.Length >= 1024 * 1024) {
          try {
            if (File.Exists(oldPath)) File.Delete(oldPath);
            File.Move(logPath, oldPath);
          } catch { }
        }
      }

      string line = string.Format(CultureInfo.InvariantCulture, "[{0:yyyy-MM-dd HH:mm:ss}] {1}\r\n", DateTime.UtcNow, message);
      File.AppendAllText(logPath, line, Encoding.UTF8);
    } catch { }
  }

  static List<string> GetDefaultAccounts() {
    List<string> list = new List<string>();
    try {
      string userProfile = Environment.GetEnvironmentVariable("USERPROFILE");
      if (string.IsNullOrEmpty(userProfile)) return list;
      string accDir = Path.Combine(userProfile, @".gemini\antigravity-cli\accounts");
      if (Directory.Exists(accDir)) {
        string[] entries = Directory.GetFileSystemEntries(accDir);
        List<string> names = new List<string>();
        for (int i = 0; i < entries.Length; i++) {
          string name = Path.GetFileName(entries[i]);
          if (!string.IsNullOrEmpty(name) && !names.Contains(name)) {
            names.Add(name);
          }
        }
        names.Sort(StringComparer.OrdinalIgnoreCase);
        list.AddRange(names);
      }
    } catch (Exception ex) {
      Log("GetDefaultAccounts error: " + ex.Message);
    }
    return list;
  }

  static ConfigData LoadConfig(string iniPath) {
    ConfigData cfg = new ConfigData();
    cfg.Families.Add(new ModelFamily { Name = "flash", Pattern = new Regex("flash", RegexOptions.IgnoreCase) });
    cfg.Families.Add(new ModelFamily { Name = "pro", Pattern = new Regex(@"\bpro\b|argon|ultra", RegexOptions.IgnoreCase) });

    if (!File.Exists(iniPath)) {
      cfg.Accounts = GetDefaultAccounts();
      return cfg;
    }

    try {
      string[] lines = File.ReadAllLines(iniPath);
      bool explicitFamilies = false;
      List<ModelFamily> customFamilies = new List<ModelFamily>();

      for (int i = 0; i < lines.Length; i++) {
        string raw = lines[i];
        raw = Regex.Replace(raw, @"(^|\s)#(\s.*|$)", "").Trim();
        if (raw.Length == 0) continue;

        int eq = raw.IndexOf('=');
        if (eq < 0) continue;
        string k = raw.Substring(0, eq).Trim().ToLowerInvariant();
        string v = raw.Substring(eq + 1).Trim();

        if (k == "interval_ms") {
          int ms;
          if (int.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out ms) && ms >= 1000) {
            cfg.IntervalMs = ms;
          }
        } else if (k == "accounts") {
          string[] accs = v.Split(new char[] { ',' }, StringSplitOptions.RemoveEmptyEntries);
          cfg.Accounts.Clear();
          for (int a = 0; a < accs.Length; a++) {
            string email = accs[a].Trim();
            if (email.Length > 0 && !cfg.Accounts.Contains(email)) {
              cfg.Accounts.Add(email);
            }
          }
        } else if (k == "exclude") {
          if (!string.IsNullOrEmpty(v)) {
            try {
              cfg.ExcludeRegex = new Regex(v, RegexOptions.IgnoreCase);
            } catch (Exception ex) {
              Log("Invalid exclude regex: " + ex.Message);
            }
          }
        } else if (k == "output") {
          if (!string.IsNullOrEmpty(v)) {
            cfg.OutputPath = v;
          }
        } else {
          try {
            Regex r = new Regex(v, RegexOptions.IgnoreCase);
            if (!explicitFamilies) {
              customFamilies.Clear();
              explicitFamilies = true;
            }
            customFamilies.Add(new ModelFamily { Name = k, Pattern = r });
          } catch (Exception ex) {
            Log("Invalid regex for family '" + k + "': " + ex.Message);
          }
        }
      }

      if (explicitFamilies && customFamilies.Count > 0) {
        cfg.Families = customFamilies;
      }
    } catch (Exception ex) {
      Log("Error reading config: " + ex.Message);
    }

    if (cfg.Accounts.Count == 0) {
      cfg.Accounts = GetDefaultAccounts();
    }

    return cfg;
  }

  static void ReloadPreviousFile(string path, List<string> configuredEmails, Dictionary<string, AccountState> accountsData) {
    if (!File.Exists(path)) return;
    try {
      string[] lines = File.ReadAllLines(path);
      long fileUpdatedUnix = 0;
      Dictionary<string, string> kv = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
      for (int i = 0; i < lines.Length; i++) {
        string line = lines[i].Trim();
        if (line.Length == 0 || line.StartsWith("#")) continue;
        int eq = line.IndexOf('=');
        if (eq < 0) continue;
        string k = line.Substring(0, eq).Trim();
        string v = line.Substring(eq + 1).Trim();
        kv[k] = v;
        if (string.Equals(k, "agy.updated", StringComparison.OrdinalIgnoreCase)) {
          long.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out fileUpdatedUnix);
        }
      }

      DateTime fileUpdatedUtc = fileUpdatedUnix > 0
          ? UnixEpoch.AddSeconds(fileUpdatedUnix)
          : File.GetLastWriteTimeUtc(path);

      Regex slotRegex = new Regex(@"^agy(\d+)\.(.+)$", RegexOptions.IgnoreCase);
      Dictionary<int, Dictionary<string, string>> slotProps = new Dictionary<int, Dictionary<string, string>>();
      foreach (KeyValuePair<string, string> entry in kv) {
        Match m = slotRegex.Match(entry.Key);
        if (m.Success) {
          int slot = int.Parse(m.Groups[1].Value);
          string prop = m.Groups[2].Value.ToLowerInvariant();
          Dictionary<string, string> dict;
          if (!slotProps.TryGetValue(slot, out dict)) {
            dict = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            slotProps[slot] = dict;
          }
          dict[prop] = entry.Value;
        }
      }

      foreach (KeyValuePair<int, Dictionary<string, string>> sp in slotProps) {
        Dictionary<string, string> props = sp.Value;
        string email;
        if (!props.TryGetValue("email", out email) || string.IsNullOrEmpty(email)) continue;

        if (!configuredEmails.Contains(email)) {
          configuredEmails.Add(email);
        }

        AccountState acc = new AccountState();
        acc.Email = email;
        string plan;
        if (props.TryGetValue("plan", out plan)) acc.Plan = plan;

        long ageS = 0;
        string ageStr;
        if (props.TryGetValue("age_s", out ageStr)) {
          long.TryParse(ageStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out ageS);
        }
        acc.LastFetchedUtc = fileUpdatedUtc.AddSeconds(-ageS);

        HashSet<string> famKeys = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (string propName in props.Keys) {
          int us = propName.IndexOf('_');
          if (us > 0) {
            famKeys.Add(propName.Substring(0, us));
          }
        }

        foreach (string fam in famKeys) {
          FamilyState fs = new FamilyState();
          fs.FamilyKey = fam;
          string nameVal;
          if (props.TryGetValue(fam + "_name", out nameVal)) fs.DisplayName = nameVal;

          string pctStr;
          if (props.TryGetValue(fam + "_pct", out pctStr)) {
            double pct;
            if (double.TryParse(pctStr, NumberStyles.Float, CultureInfo.InvariantCulture, out pct)) {
              fs.HasQuota = true;
              fs.RemainingFraction = pct / 100.0;
            }
          }

          string resetSStr;
          if (props.TryGetValue(fam + "_reset_s", out resetSStr)) {
            long resetS;
            if (long.TryParse(resetSStr, NumberStyles.Integer, CultureInfo.InvariantCulture, out resetS)) {
              fs.ResetTimeUtc = fileUpdatedUtc.AddSeconds(resetS);
            }
          }

          acc.Families[fam] = fs;
        }

        accountsData[email] = acc;
      }
    } catch (Exception ex) {
      Log("ReloadPreviousFile error: " + ex.Message);
    }
  }

  static List<int> GetListeningPorts(int pid) {
    List<int> ports = new List<int>();
    int size = 0;
    uint ret = GetExtendedTcpTable(IntPtr.Zero, ref size, false, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    for (int attempt = 0; attempt < 3; attempt++) {
      if (size <= 0) break;
      IntPtr pTable = Marshal.AllocHGlobal(size);
      try {
        ret = GetExtendedTcpTable(pTable, ref size, false, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
        if (ret == 0) {
          int numEntries = Marshal.ReadInt32(pTable, 0);
          IntPtr rowPtr = new IntPtr(pTable.ToInt64() + 4);
          for (int i = 0; i < numEntries; i++) {
            int state = Marshal.ReadInt32(rowPtr, 0);
            if (state == MIB_TCP_STATE_LISTEN) {
              int rowPid = Marshal.ReadInt32(rowPtr, 20);
              if (rowPid == pid) {
                uint localAddr = (uint)Marshal.ReadInt32(rowPtr, 4);
                if ((localAddr & 0xFF) == 127 || localAddr == 0) {
                  int portRaw = Marshal.ReadInt32(rowPtr, 8);
                  int port = ((portRaw & 0xFF) << 8) | ((portRaw >> 8) & 0xFF);
                  if (port > 0 && !ports.Contains(port)) {
                    ports.Add(port);
                  }
                }
              }
            }
            rowPtr = new IntPtr(rowPtr.ToInt64() + 24);
          }
          break;
        } else if (ret != ERROR_INSUFFICIENT_BUFFER) {
          break;
        }
      } finally {
        Marshal.FreeHGlobal(pTable);
      }
    }
    return ports;
  }

  static List<ClientProcess> FindClientProcesses() {
    List<ClientProcess> list = new List<ClientProcess>();
    try {
      using (var q = new ManagementObjectSearcher(
          "SELECT ProcessId, Name, CommandLine FROM Win32_Process WHERE Name = 'language_server.exe' OR Name = 'agy.exe'")) {
        foreach (ManagementObject o in q.Get()) {
          try {
            int pid = Convert.ToInt32(o["ProcessId"]);
            string cmd = o["CommandLine"] != null ? o["CommandLine"].ToString() : "";

            string csrf = null;
            if (!string.IsNullOrEmpty(cmd)) {
              Match m = Regex.Match(cmd, @"--csrf_token[= ]""?([^\s""]+)""?");
              if (m.Success) {
                csrf = m.Groups[1].Value;
              }
            }

            ClientProcess cp = new ClientProcess();
            cp.Pid = pid;
            cp.Csrf = csrf;
            list.Add(cp);
          } catch (Exception ex) {
            Log("Error inspecting process: " + ex.Message);
          }
        }
      }
    } catch (Exception ex) {
      Log("WMI Win32_Process search failed: " + ex.Message);
    }
    return list;
  }

  static string QueryLanguageServer(int port, string csrf) {
    string url = string.Format(CultureInfo.InvariantCulture, "http://127.0.0.1:{0}/exa.language_server_pb.LanguageServerService/GetUserStatus", port);
    byte[] body = Encoding.UTF8.GetBytes("{\"metadata\":{\"ideName\":\"antigravity\",\"extensionName\":\"antigravity\",\"locale\":\"en\"}}");

    HttpWebRequest req = (HttpWebRequest)WebRequest.Create(url);
    req.Method = "POST";
    req.ContentType = "application/json";
    req.Headers["Connect-Protocol-Version"] = "1";
    if (!string.IsNullOrEmpty(csrf)) {
      req.Headers["X-Codeium-Csrf-Token"] = csrf;
    }
    req.Timeout = 3000;
    req.ReadWriteTimeout = 3000;
    req.Proxy = null;
    req.ContentLength = body.Length;

    using (Stream reqStream = req.GetRequestStream()) {
      reqStream.Write(body, 0, body.Length);
    }

    using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
    using (Stream respStream = resp.GetResponseStream())
    using (StreamReader reader = new StreamReader(respStream, Encoding.UTF8)) {
      return reader.ReadToEnd();
    }
  }

  static string GetString(Dictionary<string, object> dict, string key) {
    if (dict == null) return null;
    object val;
    if (dict.TryGetValue(key, out val) && val != null) return val.ToString();
    return null;
  }

  static double? GetDouble(Dictionary<string, object> dict, string key) {
    if (dict == null) return null;
    object val;
    if (dict.TryGetValue(key, out val) && val != null) {
      if (val is double) return (double)val;
      if (val is float) return (double)(float)val;
      if (val is int) return (double)(int)val;
      if (val is long) return (double)(long)val;
      if (val is decimal) return (double)(decimal)val;
      double d;
      if (double.TryParse(val.ToString(), NumberStyles.Float, CultureInfo.InvariantCulture, out d)) return d;
    }
    return null;
  }

  static string CleanDisplayName(string label) {
    if (string.IsNullOrEmpty(label)) return "";
    string cleaned = Regex.Replace(label, @"\s*\([^)]*\)$", "");
    return cleaned.Trim();
  }

  static int CompareModels(RawModelConfig a, RawModelConfig b) {
    int cmp = a.RemainingFraction.Value.CompareTo(b.RemainingFraction.Value);
    if (cmp != 0) return cmp;
    if (a.ResetTimeUtc.HasValue && b.ResetTimeUtc.HasValue) {
      return a.ResetTimeUtc.Value.CompareTo(b.ResetTimeUtc.Value);
    }
    if (a.ResetTimeUtc.HasValue) return -1;
    if (b.ResetTimeUtc.HasValue) return 1;
    return 0;
  }

  static ParsedUserStatus ParseUserStatus(string json, List<ModelFamily> families, Regex excludeRegex) {
    var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
    Dictionary<string, object> root = serializer.Deserialize<Dictionary<string, object>>(json);
    if (root == null) return null;

    object usObj;
    if (!root.TryGetValue("userStatus", out usObj)) return null;
    Dictionary<string, object> userStatus = usObj as Dictionary<string, object>;
    if (userStatus == null) return null;

    ParsedUserStatus result = new ParsedUserStatus();
    result.Email = GetString(userStatus, "email");

    object psObj;
    if (userStatus.TryGetValue("planStatus", out psObj)) {
      Dictionary<string, object> planStatus = psObj as Dictionary<string, object>;
      if (planStatus != null) {
        object piObj;
        if (planStatus.TryGetValue("planInfo", out piObj)) {
          Dictionary<string, object> planInfo = piObj as Dictionary<string, object>;
          if (planInfo != null) {
            result.PlanName = GetString(planInfo, "planName");
          }
        }
      }
    }
    if (result.PlanName == null) result.PlanName = "";

    List<RawModelConfig> models = new List<RawModelConfig>();
    object cascadeObj;
    if (userStatus.TryGetValue("cascadeModelConfigData", out cascadeObj)) {
      Dictionary<string, object> cascadeData = cascadeObj as Dictionary<string, object>;
      if (cascadeData != null) {
        object configsObj;
        if (cascadeData.TryGetValue("clientModelConfigs", out configsObj)) {
          System.Collections.IEnumerable list = configsObj as System.Collections.IEnumerable;
          if (list != null) {
            foreach (object item in list) {
              Dictionary<string, object> c = item as Dictionary<string, object>;
              if (c == null) continue;

              RawModelConfig mc = new RawModelConfig();
              mc.Label = GetString(c, "label") ?? "";
              object disObj;
              if (c.TryGetValue("disabled", out disObj) && disObj is bool) {
                mc.Disabled = (bool)disObj;
              }

              object quotaObj;
              if (c.TryGetValue("quotaInfo", out quotaObj)) {
                Dictionary<string, object> qi = quotaObj as Dictionary<string, object>;
                if (qi != null) {
                  mc.RemainingFraction = GetDouble(qi, "remainingFraction");
                  string rtStr = GetString(qi, "resetTime");
                  if (!string.IsNullOrEmpty(rtStr)) {
                    DateTime dt;
                    if (DateTime.TryParse(rtStr, CultureInfo.InvariantCulture,
                        DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out dt)) {
                      mc.ResetTimeUtc = dt;
                    }
                  }
                }
              }
              models.Add(mc);
            }
          }
        }
      }
    }

    Dictionary<string, List<RawModelConfig>> famModels = new Dictionary<string, List<RawModelConfig>>(StringComparer.OrdinalIgnoreCase);
    for (int i = 0; i < families.Count; i++) {
      famModels[families[i].Name] = new List<RawModelConfig>();
    }

    for (int i = 0; i < models.Count; i++) {
      RawModelConfig m = models[i];
      if (string.IsNullOrEmpty(m.Label)) continue;
      if (excludeRegex != null && excludeRegex.IsMatch(m.Label)) continue;

      for (int f = 0; f < families.Count; f++) {
        ModelFamily fam = families[f];
        if (fam.Pattern != null && fam.Pattern.IsMatch(m.Label)) {
          famModels[fam.Name].Add(m);
          break;
        }
      }
    }

    for (int f = 0; f < families.Count; f++) {
      ModelFamily fam = families[f];
      List<RawModelConfig> matching = famModels[fam.Name];
      if (matching.Count == 0) continue;

      List<RawModelConfig> withQuota = new List<RawModelConfig>();
      for (int i = 0; i < matching.Count; i++) {
        if (!matching[i].Disabled && matching[i].RemainingFraction.HasValue) {
          withQuota.Add(matching[i]);
        }
      }

      FamilyState fs = new FamilyState();
      fs.FamilyKey = fam.Name;

      if (withQuota.Count > 0) {
        RawModelConfig best = withQuota[0];
        for (int i = 1; i < withQuota.Count; i++) {
          if (CompareModels(withQuota[i], best) < 0) {
            best = withQuota[i];
          }
        }
        fs.HasQuota = true;
        fs.RemainingFraction = best.RemainingFraction.Value;
        fs.ResetTimeUtc = best.ResetTimeUtc;
        fs.DisplayName = CleanDisplayName(best.Label);
      } else {
        fs.HasQuota = false;
        fs.DisplayName = CleanDisplayName(matching[0].Label);
      }

      result.Families[fam.Name] = fs;
    }

    return result;
  }

  static void UpdateAccount(
      ParsedUserStatus status,
      List<string> configuredEmails,
      Dictionary<string, AccountState> accountsData) {
    if (string.IsNullOrEmpty(status.Email)) return;

    if (!configuredEmails.Contains(status.Email)) {
      configuredEmails.Add(status.Email);
    }

    AccountState acc;
    if (!accountsData.TryGetValue(status.Email, out acc)) {
      acc = new AccountState();
      acc.Email = status.Email;
      accountsData[status.Email] = acc;
    }

    acc.Plan = status.PlanName;
    acc.LastFetchedUtc = DateTime.UtcNow;
    foreach (KeyValuePair<string, FamilyState> kv in status.Families) {
      acc.Families[kv.Key] = kv.Value;
    }
  }

  static void Poll(
      List<ModelFamily> families,
      Regex excludeRegex,
      List<string> configuredEmails,
      Dictionary<string, AccountState> accountsData,
      out int liveClients) {
    liveClients = 0;
    foreach (AccountState a in accountsData.Values) a.Live = false;
    List<ClientProcess> procs = FindClientProcesses();

    for (int pIdx = 0; pIdx < procs.Count; pIdx++) {
      ClientProcess cp = procs[pIdx];
      List<int> ports = GetListeningPorts(cp.Pid);
      for (int i = 0; i < ports.Count; i++) {
        int port = ports[i];
        string json = null;
        try {
          json = QueryLanguageServer(port, cp.Csrf);
        } catch {
          continue;
        }

        if (string.IsNullOrEmpty(json)) continue;

        try {
          ParsedUserStatus status = ParseUserStatus(json, families, excludeRegex);
          if (status != null && !string.IsNullOrEmpty(status.Email)) {
            liveClients++;
            UpdateAccount(status, configuredEmails, accountsData);
            AccountState liveAcc;
            if (accountsData.TryGetValue(status.Email, out liveAcc)) liveAcc.Live = true;
            break;
          }
        } catch (Exception ex) {
          Log("Error parsing language server response: " + ex.Message);
        }
      }
    }
  }

  static List<string> GenerateOutputLines(
      int liveClients,
      List<ModelFamily> families,
      List<string> configuredEmails,
      Dictionary<string, AccountState> accountsData) {
    List<string> lines = new List<string>();
    lines.Add("agy.live=" + liveClients.ToString(CultureInfo.InvariantCulture));

    DateTime nowUtc = DateTime.UtcNow;
    long nowUnix = (long)(nowUtc - UnixEpoch).TotalSeconds;
    lines.Add("agy.updated=" + nowUnix.ToString(CultureInfo.InvariantCulture));

    // agy.active: the account(s) signed in on a running client right now
    List<string> active = new List<string>();
    for (int i = 0; i < configuredEmails.Count; i++) {
      AccountState a;
      if (accountsData.TryGetValue(configuredEmails[i], out a) && a.Live) active.Add(a.Email ?? configuredEmails[i]);
    }
    lines.Add("agy.active=" + (active.Count > 0 ? string.Join(",", active.ToArray()) : "none"));

    for (int i = 0; i < configuredEmails.Count; i++) {
      string email = configuredEmails[i];
      AccountState acc;
      if (!accountsData.TryGetValue(email, out acc)) continue;

      int slotNum = i + 1;
      string pfx = "agy" + slotNum.ToString(CultureInfo.InvariantCulture) + ".";

      lines.Add(pfx + "email=" + (acc.Email ?? ""));
      lines.Add(pfx + "plan=" + (acc.Plan ?? ""));
      lines.Add(pfx + "live=" + (acc.Live ? "1" : "0"));

      long ageS = Math.Max(0, (long)Math.Round((nowUtc - acc.LastFetchedUtc).TotalSeconds));
      lines.Add(pfx + "age_s=" + ageS.ToString(CultureInfo.InvariantCulture));

      CloudQuota cq;
      bool haveCq;
      lock (cloudQuota) haveCq = cloudQuota.TryGetValue(acc.Email ?? email, out cq);
      if (haveCq) {
        if (!double.IsNaN(cq.FivePct)) lines.Add(pfx + "flash_5h_pct=" + cq.FivePct.ToString("0.0", CultureInfo.InvariantCulture));
        if (cq.FiveReset.HasValue) lines.Add(pfx + "flash_5h_reset_at=" + cq.FiveReset.Value.ToLocalTime().ToString("HH:mm", CultureInfo.InvariantCulture));
        if (!double.IsNaN(cq.WeekPct)) lines.Add(pfx + "flash_wk_pct=" + cq.WeekPct.ToString("0.0", CultureInfo.InvariantCulture));
        if (cq.WeekReset.HasValue) lines.Add(pfx + "flash_wk_reset_at=" + cq.WeekReset.Value.ToLocalTime().ToString("ddd HH:mm", CultureInfo.InvariantCulture));
      }

      for (int f = 0; f < families.Count; f++) {
        ModelFamily fam = families[f];
        FamilyState fs;
        if (!acc.Families.TryGetValue(fam.Name, out fs)) continue;

        if (fs.HasQuota) {
          double pct = Math.Round(fs.RemainingFraction * 100.0, 1);
          lines.Add(pfx + fam.Name + "_pct=" + pct.ToString("0.0", CultureInfo.InvariantCulture));
          if (fs.ResetTimeUtc.HasValue) {
            long resetS = Math.Max(0, (long)Math.Round((fs.ResetTimeUtc.Value - nowUtc).TotalSeconds));
            lines.Add(pfx + fam.Name + "_reset_s=" + resetS.ToString(CultureInfo.InvariantCulture));
            lines.Add(pfx + fam.Name + "_reset_at=" + fs.ResetTimeUtc.Value.ToLocalTime().ToString("ddd HH:mm", CultureInfo.InvariantCulture));
          }
        }
        if (!string.IsNullOrEmpty(fs.DisplayName)) {
          lines.Add(pfx + fam.Name + "_name=" + fs.DisplayName);
        }
      }
    }

    return lines;
  }

  static void WriteOutputFile(string outputPath, List<string> lines) {
    string dir = Path.GetDirectoryName(outputPath);
    if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) {
      Directory.CreateDirectory(dir);
    }

    string content = string.Join("\r\n", lines.ToArray()) + "\r\n";
    string tempPath = outputPath + ".tmp." + Guid.NewGuid().ToString("N");

    try {
      File.WriteAllText(tempPath, content, new UTF8Encoding(false));   // no BOM: it would glue onto the first key
      for (int i = 0; i < 5; i++) {
        try {
          if (File.Exists(outputPath)) {
            try {
              File.Replace(tempPath, outputPath, null);
              return;
            } catch {
              File.Delete(outputPath);
              File.Move(tempPath, outputPath);
              return;
            }
          } else {
            File.Move(tempPath, outputPath);
            return;
          }
        } catch (Exception ex) {
          if (i == 4) {
            Log("WriteOutputFile failed: " + ex.Message);
          } else {
            Thread.Sleep(50);
          }
        }
      }
    } finally {
      if (File.Exists(tempPath)) {
        try { File.Delete(tempPath); } catch { }
      }
    }
  }

  const string MediaOutputPath = @"C:\ProgramData\ESP32PcStats\media.txt";
  const string MediaArtPath = @"C:\ProgramData\ESP32PcStats\media_art.bin";

  static uint lastArtId = 0;
  static string lastTrackKey = null;
  static byte[] currentArtBytes = null;
  static uint[] crcTable;

  static uint ComputeCrc32(byte[] data) {
    if (data == null || data.Length == 0) return 0;
    if (crcTable == null) {
      crcTable = new uint[256];
      for (uint i = 0; i < 256; i++) {
        uint c = i;
        for (int j = 0; j < 8; j++) {
          c = (c & 1) != 0 ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crcTable[i] = c;
      }
    }
    uint crc = 0xFFFFFFFFu;
    for (int i = 0; i < data.Length; i++) {
      crc = crcTable[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
  }

  static T AwaitWinRt<T>(Windows.Foundation.IAsyncOperation<T> op) {
    return System.WindowsRuntimeSystemExtensions.AsTask(op).Result;
  }

  static string CleanText(string s) {
    if (s == null) return "";
    return s.Replace('\r', ' ').Replace('\n', ' ').Replace('\t', ' ').Trim();
  }

  static string GetFriendlySource(string aumid) {
    if (string.IsNullOrEmpty(aumid)) return "";
    string lower = aumid.ToLowerInvariant();
    if (lower.Contains("spotify")) return "Spotify";
    if (lower.Contains("chrome")) return "Chrome";
    if (lower.Contains("edge")) return "Edge";
    if (lower.Contains("firefox")) return "Firefox";
    if (lower.Contains("brave")) return "Brave";
    if (lower.Contains("opera")) return "Opera";
    if (lower.Contains("vivaldi")) return "Vivaldi";
    if (lower.Contains("vlc")) return "VLC";
    if (lower.Contains("foobar2000")) return "foobar2000";

    string tail = aumid;
    int slash = tail.LastIndexOfAny(new char[] { '\\', '/' });
    if (slash >= 0) tail = tail.Substring(slash + 1);
    int bang = tail.LastIndexOf('!');
    if (bang >= 0) tail = tail.Substring(bang + 1);
    if (tail.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
      tail = tail.Substring(0, tail.Length - 4);
    return CleanText(tail);
  }

  static GlobalSystemMediaTransportControlsSession PickSession(GlobalSystemMediaTransportControlsSessionManager mgr) {
    if (mgr == null) return null;
    GlobalSystemMediaTransportControlsSession current = null;
    try {
      current = mgr.GetCurrentSession();
    } catch { }

    if (current != null) {
      try {
        var info = current.GetPlaybackInfo();
        if (info != null && info.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing) {
          return current;
        }
      } catch { }
    }

    try {
      var sessions = mgr.GetSessions();
      if (sessions != null) {
        for (int i = 0; i < sessions.Count; i++) {
          var s = sessions[i];
          if (s == null) continue;
          try {
            var info = s.GetPlaybackInfo();
            if (info != null && info.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing) {
              return s;
            }
          } catch { }
        }
      }
    } catch { }

    return current;
  }

  static byte[] ProcessThumbnail(IRandomAccessStreamReference thumbRef) {
    if (thumbRef == null) return null;
    var stream = AwaitWinRt(thumbRef.OpenReadAsync());
    if (stream == null) return null;
    using (stream)
    using (var netStream = System.IO.WindowsRuntimeStreamExtensions.AsStreamForRead(stream))
    using (var img = Image.FromStream(netStream)) {
      int crop = Math.Min(img.Width, img.Height);
      int x = (img.Width - crop) / 2;
      int y = (img.Height - crop) / 2;
      using (var bmp = new Bitmap(144, 144, PixelFormat.Format24bppRgb)) {
        using (var g = Graphics.FromImage(bmp)) {
          g.InterpolationMode = InterpolationMode.HighQualityBicubic;
          g.PixelOffsetMode = PixelOffsetMode.HighQuality;
          g.SmoothingMode = SmoothingMode.HighQuality;
          g.DrawImage(img, new Rectangle(0, 0, 144, 144), new Rectangle(x, y, crop, crop), GraphicsUnit.Pixel);
        }
        var bd = bmp.LockBits(new Rectangle(0, 0, 144, 144), ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        byte[] raw = new byte[bd.Stride * 144];
        Marshal.Copy(bd.Scan0, raw, 0, raw.Length);
        bmp.UnlockBits(bd);

        byte[] rgb565 = new byte[144 * 144 * 2];
        int dest = 0;
        for (int row = 0; row < 144; row++) {
          int offset = row * bd.Stride;
          for (int col = 0; col < 144; col++) {
            byte b = raw[offset + col * 3];
            byte g_val = raw[offset + col * 3 + 1];
            byte r = raw[offset + col * 3 + 2];
            ushort val = (ushort)(((r >> 3) << 11) | ((g_val >> 2) << 5) | (b >> 3));
            rgb565[dest++] = (byte)(val >> 8);
            rgb565[dest++] = (byte)(val & 0xFF);
          }
        }
        return rgb565;
      }
    }
  }

  static void WriteOutputFileBytes(string outputPath, byte[] data) {
    string dir = Path.GetDirectoryName(outputPath);
    if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) {
      try { Directory.CreateDirectory(dir); } catch { }
    }
    string tempPath = outputPath + ".tmp." + Guid.NewGuid().ToString("N");
    try {
      File.WriteAllBytes(tempPath, data);
      for (int i = 0; i < 5; i++) {
        try {
          if (File.Exists(outputPath)) {
            try {
              File.Replace(tempPath, outputPath, null);
              return;
            } catch {
              File.Delete(outputPath);
              File.Move(tempPath, outputPath);
              return;
            }
          } else {
            File.Move(tempPath, outputPath);
            return;
          }
        } catch (Exception ex) {
          if (i == 4) {
            Log("WriteOutputFileBytes failed: " + ex.Message);
          } else {
            Thread.Sleep(50);
          }
        }
      }
    } finally {
      if (File.Exists(tempPath)) {
        try { File.Delete(tempPath); } catch { }
      }
    }
  }

  static List<string> PollMedia(GlobalSystemMediaTransportControlsSessionManager mgr) {
    var lines = new List<string>();
    GlobalSystemMediaTransportControlsSession session = PickSession(mgr);
    if (session == null) {
      lastArtId = 0;
      lastTrackKey = null;
      currentArtBytes = null;
      lines.Add("media.ok=0");
      WriteOutputFile(MediaOutputPath, lines);
      return lines;
    }

    string source = GetFriendlySource(session.SourceAppUserModelId);
    bool playing = false;
    try {
      var info = session.GetPlaybackInfo();
      playing = info != null && info.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;
    } catch { }

    string title = "";
    string artist = "";
    string album = "";
    IRandomAccessStreamReference thumbRef = null;

    try {
      var mediaProps = AwaitWinRt(session.TryGetMediaPropertiesAsync());
      if (mediaProps != null) {
        title = CleanText(mediaProps.Title);
        artist = CleanText(mediaProps.Artist);
        album = CleanText(mediaProps.AlbumTitle);
        thumbRef = mediaProps.Thumbnail;
      }
    } catch (Exception ex) {
      Log("media properties error: " + ex.Message);
    }

    if (title.Length > 80) title = title.Substring(0, 80);
    if (artist.Length > 80) artist = artist.Substring(0, 80);
    if (album.Length > 80) album = album.Substring(0, 80);

    int pos_s = 0;
    int len_s = 0;
    try {
      var timeline = session.GetTimelineProperties();
      if (timeline != null) {
        TimeSpan pos = timeline.Position;
        if (playing && timeline.LastUpdatedTime != DateTimeOffset.MinValue) {
          TimeSpan elapsed = DateTimeOffset.UtcNow - timeline.LastUpdatedTime;
          if (elapsed > TimeSpan.Zero) {
            pos = pos + elapsed;
          }
        }
        if (pos < TimeSpan.Zero) pos = TimeSpan.Zero;
        if (timeline.EndTime > TimeSpan.Zero && pos > timeline.EndTime) pos = timeline.EndTime;

        pos_s = (int)pos.TotalSeconds;
        len_s = (int)timeline.EndTime.TotalSeconds;
        if (pos_s < 0) pos_s = 0;
        if (len_s < 0) len_s = 0;
      }
    } catch (Exception ex) {
      Log("media timeline error: " + ex.Message);
    }

    string trackKey = source + "|" + title + "|" + artist + "|" + album;
    bool trackChanged = (trackKey != lastTrackKey);

    if (thumbRef == null) {
      lastArtId = 0;
      currentArtBytes = null;
      lastTrackKey = trackKey;
    } else if (trackChanged || lastArtId == 0) {
      lastTrackKey = trackKey;
      try {
        byte[] rgb565 = ProcessThumbnail(thumbRef);
        if (rgb565 != null && rgb565.Length == 41472) {
          uint crc = ComputeCrc32(rgb565);
          if (crc != 0) {
            if (crc != lastArtId || !File.Exists(MediaArtPath)) {
              WriteOutputFileBytes(MediaArtPath, rgb565);
            }
            lastArtId = crc;
            currentArtBytes = rgb565;
          }
        }
      } catch (Exception ex) {
        Log("media thumbnail process error: " + ex.Message);
      }
    }

    lines.Add("media.ok=1");
    lines.Add("media.playing=" + (playing ? "1" : "0"));
    lines.Add("media.title=" + title);
    lines.Add("media.artist=" + artist);
    lines.Add("media.album=" + album);
    lines.Add("media.source=" + source);
    lines.Add("media.pos_s=" + pos_s);
    lines.Add("media.len_s=" + len_s);
    lines.Add("media.art_id=" + lastArtId.ToString(CultureInfo.InvariantCulture));

    WriteOutputFile(MediaOutputPath, lines);
    return lines;
  }

  static void SaveCurrentArtToPng(string pngPath) {
    byte[] rgb565 = currentArtBytes;
    if (rgb565 == null || rgb565.Length != 41472) {
      if (File.Exists(MediaArtPath)) {
        try {
          byte[] disk = File.ReadAllBytes(MediaArtPath);
          if (disk.Length == 41472) rgb565 = disk;
        } catch { }
      }
    }
    if (rgb565 == null || rgb565.Length != 41472) {
      rgb565 = new byte[41472];
    }

    try {
      string dir = Path.GetDirectoryName(pngPath);
      if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) {
        try { Directory.CreateDirectory(dir); } catch { }
      }
      using (var bmp = new Bitmap(144, 144, PixelFormat.Format24bppRgb)) {
        var bd = bmp.LockBits(new Rectangle(0, 0, 144, 144), ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
        byte[] raw = new byte[bd.Stride * 144];
        int src = 0;
        for (int row = 0; row < 144; row++) {
          int offset = row * bd.Stride;
          for (int col = 0; col < 144; col++) {
            ushort val = (ushort)((rgb565[src] << 8) | rgb565[src + 1]);
            src += 2;
            int r5 = (val >> 11) & 0x1F;
            int g6 = (val >> 5) & 0x3F;
            int b5 = val & 0x1F;
            raw[offset + col * 3] = (byte)((b5 * 527 + 23) >> 6);
            raw[offset + col * 3 + 1] = (byte)((g6 * 259 + 33) >> 6);
            raw[offset + col * 3 + 2] = (byte)((r5 * 527 + 23) >> 6);
          }
        }
        Marshal.Copy(raw, 0, bd.Scan0, raw.Length);
        bmp.UnlockBits(bd);
        bmp.Save(pngPath, ImageFormat.Png);
      }
    } catch (Exception ex) {
      Log("SaveCurrentArtToPng error: " + ex.Message);
    }
  }

  static void RunMediaTest() {
    AttachConsole(ATTACH_PARENT_PROCESS);
    IntPtr stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdHandle != IntPtr.Zero && stdHandle.ToInt64() != -1) {
      SafeFileHandle safeHandle = new SafeFileHandle(stdHandle, false);
      FileStream fs = new FileStream(safeHandle, FileAccess.Write);
      StreamWriter writer = new StreamWriter(fs, Console.OutputEncoding);
      writer.AutoFlush = true;
      Console.SetOut(writer);
    }

    GlobalSystemMediaTransportControlsSessionManager mgr = null;
    try {
      mgr = AwaitWinRt(GlobalSystemMediaTransportControlsSessionManager.RequestAsync());
    } catch (Exception ex) {
      Log("RunMediaTest RequestAsync error: " + ex.Message);
    }

    for (int step = 0; step < 5; step++) {
      DateTime t0 = DateTime.UtcNow;
      if (mgr == null) {
        try {
          mgr = AwaitWinRt(GlobalSystemMediaTransportControlsSessionManager.RequestAsync());
        } catch { }
      }
      List<string> lines = PollMedia(mgr);
      for (int i = 0; i < lines.Count; i++) {
        Console.WriteLine(lines[i]);
      }
      SaveCurrentArtToPng(@"K:\scratch\media_art.png");
      if (step < 4) {
        int elapsed = (int)(DateTime.UtcNow - t0).TotalMilliseconds;
        int wait = 1000 - elapsed;
        if (wait > 0) Thread.Sleep(wait);
      }
    }
  }

  static void MediaLoop() {
    GlobalSystemMediaTransportControlsSessionManager mgr = null;
    while (true) {
      DateTime t0 = DateTime.UtcNow;
      try {
        if (mgr == null) {
          mgr = AwaitWinRt(GlobalSystemMediaTransportControlsSessionManager.RequestAsync());
        }
        PollMedia(mgr);
      } catch (Exception ex) {
        Log("media error: " + ex.Message);
        mgr = null;
      }
      int elapsed = (int)(DateTime.UtcNow - t0).TotalMilliseconds;
      int wait = 1000 - elapsed;
      if (wait < 100) wait = 100;
      Thread.Sleep(wait);
    }
  }

  static void RunOnce(string configPath) {
    AttachConsole(ATTACH_PARENT_PROCESS);
    IntPtr stdHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (stdHandle != IntPtr.Zero && stdHandle.ToInt64() != -1) {
      SafeFileHandle safeHandle = new SafeFileHandle(stdHandle, false);
      FileStream fs = new FileStream(safeHandle, FileAccess.Write);
      StreamWriter writer = new StreamWriter(fs, Console.OutputEncoding);
      writer.AutoFlush = true;
      Console.SetOut(writer);
    }

    ConfigData cfg = LoadConfig(configPath);
    Dictionary<string, AccountState> accountsData = new Dictionary<string, AccountState>(StringComparer.OrdinalIgnoreCase);
    ReloadPreviousFile(cfg.OutputPath, cfg.Accounts, accountsData);

    Thread mediaThread = new Thread(MediaLoop) { IsBackground = true, Name = "esp32_media" };
    mediaThread.Start();

    int liveClients = 0;
    try {
      Poll(cfg.Families, cfg.ExcludeRegex, cfg.Accounts, accountsData, out liveClients);
    } catch (Exception ex) {
      Log("Poll error: " + ex.Message);
    }
    try { ClaudeKeepAlive(); } catch (Exception ex) { Log("claude: keep-alive error: " + ex.Message); }
    try { PollCloudQuota(); } catch (Exception ex) { Log("agy cloud: " + ex.GetType().Name); }

    List<string> lines = GenerateOutputLines(liveClients, cfg.Families, cfg.Accounts, accountsData);
    try {
      WriteOutputFile(cfg.OutputPath, lines);
    } catch (Exception ex) {
      Log("Write output error: " + ex.Message);
    }

    for (int i = 0; i < lines.Count; i++) {
      Console.WriteLine(lines[i]);
    }
  }

  static void RunLoop(string configPath) {
    DateTime lastConfigWriteTime = DateTime.MinValue;
    ConfigData cfg = null;
    Dictionary<string, AccountState> accountsData = new Dictionary<string, AccountState>(StringComparer.OrdinalIgnoreCase);

    if (File.Exists(configPath)) {
      lastConfigWriteTime = File.GetLastWriteTimeUtc(configPath);
    }
    cfg = LoadConfig(configPath);

    ReloadPreviousFile(cfg.OutputPath, cfg.Accounts, accountsData);

    while (true) {
      DateTime t0 = DateTime.UtcNow;

      if (File.Exists(configPath)) {
        DateTime writeTime = File.GetLastWriteTimeUtc(configPath);
        if (writeTime != lastConfigWriteTime) {
          lastConfigWriteTime = writeTime;
          cfg = LoadConfig(configPath);
        }
      }

      int liveClients = 0;
      try {
        Poll(cfg.Families, cfg.ExcludeRegex, cfg.Accounts, accountsData, out liveClients);
      } catch (Exception ex) {
        Log("Poll error: " + ex.Message);
      }
      try { ClaudeKeepAlive(); } catch (Exception ex) { Log("claude: keep-alive error: " + ex.Message); }
      try { PollCloudQuota(); } catch (Exception ex) { Log("agy cloud: " + ex.GetType().Name); }

      try {
        List<string> lines = GenerateOutputLines(liveClients, cfg.Families, cfg.Accounts, accountsData);
        WriteOutputFile(cfg.OutputPath, lines);
      } catch (Exception ex) {
        Log("Write output error: " + ex.Message);
      }

      int elapsed = (int)(DateTime.UtcNow - t0).TotalMilliseconds;
      int wait = cfg.IntervalMs - elapsed;
      if (wait < 500) wait = 500;

      int slept = 0;
      while (slept < wait) {
        int chunk = Math.Min(500, wait - slept);
        Thread.Sleep(chunk);
        slept += chunk;
      }
    }
  }

  [STAThread]
  static void Main(string[] args) {
    bool once = false;
    bool media = false;
    for (int i = 0; i < args.Length; i++) {
      if (string.Equals(args[i], "--once", StringComparison.OrdinalIgnoreCase)) {
        once = true;
        break;
      }
      if (string.Equals(args[i], "--media", StringComparison.OrdinalIgnoreCase)) {
        media = true;
        break;
      }
    }

    if (media) {
      RunMediaTest();
      return;
    }

    string exeDir = AppDomain.CurrentDomain.BaseDirectory;
    string configPath = Path.Combine(exeDir, "agy_quota.ini");

    if (once) {
      RunOnce(configPath);
      return;
    }

    bool createdNew;
    using (Mutex mutex = new Mutex(true, @"Local\ESP32AgyQuota", out createdNew)) {
      if (!createdNew) {
        return;
      }
      RunLoop(configPath);
    }
  }
}
