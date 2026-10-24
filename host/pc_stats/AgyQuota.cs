// esp32-agyquota: Per-user helper querying Google Antigravity local language servers
// and writing model quota statistics to C:\ProgramData\ESP32PcStats\agy_quota.txt
// for consumption by esp32-pcstats.exe via @source.
//
// Target: .NET Framework 4 (C# 5 syntax only).
using System;
using System.Collections.Generic;
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

    int liveClients = 0;
    try {
      Poll(cfg.Families, cfg.ExcludeRegex, cfg.Accounts, accountsData, out liveClients);
    } catch (Exception ex) {
      Log("Poll error: " + ex.Message);
    }

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
    for (int i = 0; i < args.Length; i++) {
      if (string.Equals(args[i], "--once", StringComparison.OrdinalIgnoreCase)) {
        once = true;
        break;
      }
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
