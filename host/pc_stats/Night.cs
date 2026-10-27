// Night mode, service side. The board decides when night mode is on and owns
// the sleep / hibernate timers; the helper carries it out through PcHwCtl
// (E:\PC Hardware Controller, docs/ipc.md#process-lasso, #activity).
//
// "!night 1": `lasso save day` keeps the normal config as PcHwCtl profile
// "day". Then every running process and every whole-exe rule outside the keep
// list (HOST_NIGHT_KEEP and Process Lasso's ProBalance exclusions,
// OocExclusions) gets `aff set <exe> <HOST_NIGHT_CORES>` (E-cores 14-17) and
// `eco on <exe>` (Windows efficiency mode; PcHwCtl sweeps new instances).
// Except a heavy exe: one averaging HOST_NIGHT_HEAVY_CORES cores (1.0 = one
// core busy, summed over its instances, from PcHwCtl's `proc top`) for
// HOST_NIGHT_HEAVY_S. It runs untouched until it averages under
// HOST_NIGHT_CALM_CORES for HOST_NIGHT_CALM_S, then gets pinned. The check
// runs all night: a pinned exe that turns heavy (a render started at 2 am)
// gets its day rule back and efficiency mode lifted. The same goes for the
// foreground full-screen app (pc.fg_full / pc.fg_exe from the user half: a
// game can be GPU-bound and CPU-light). Exes in HOST_NIGHT_ALWAYS stay pinned
// even while heavy. New process names are picked up every 10 s.
// Then `power plan night`: PcHwCtl's "PcHwCtl Night" Windows power plan (5 %
// processor cap, half the cores parked, docs/ipc.md#the-pchwctl-night-plan).
// "!night 0": `lasso load day` puts the normal config back (every process on
// its own rule, or every CPU), `eco off all` lifts efficiency mode and
// `power plan day` switches to PcHwCtl's day plan (power.dayPlan, High
// performance).
//
// pc.heavy / pc.heavy_by (every exe, kept ones too, all day): how many exes
// are heavy and the busiest one. The board's night mode won't sleep or
// hibernate the PC while pc.heavy >= 1.
//
// The board repeats its state every 30 s; after HOST_NIGHT_NO_WORD_MIN with
// no word (board gone mid-night) the helper releases by itself. night.on
// (next to log.txt) marks night engaged and lists what this night pinned, so
// a helper restart mid-night neither saves the night config as "day" nor
// forgets to restore it.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

partial class PcStats {
  const string NightCores = HostSettings.NIGHT_CORES;   // Process Lasso core list, e.g. "14-17"
  const int NightTimeoutMs = 15000;                     // `lasso load` sets every ruled process's affinity
  const int HeavySampleMs = 10000;
  // What the helper's own night code left while engaged: the original prolasso.ini
  const string OldLassoBackup = HostSettings.LASSO_INI + ".esp32-night";

  // Never pinned (settings.h HOST_NIGHT_KEEP). Process Lasso's own ProBalance
  // exclusions (OocExclusions) are added from the ini.
  static readonly HashSet<string> NightKeep = new HashSet<string>(HostSettings.List(HostSettings.NIGHT_KEEP), StringComparer.OrdinalIgnoreCase);
  static readonly HashSet<string> NightAlways = new HashSet<string>(ExeList(HostSettings.NIGHT_ALWAYS), StringComparer.OrdinalIgnoreCase);

  // Everything below belongs to the night thread, except what the
  // volatile / Interlocked fields hand to others.
  static DateTime nightLastWordUtc = DateTime.UtcNow;
  static volatile bool nightWanted;   // the board's last word
  static readonly AutoResetEvent nightKick = new AutoResetEvent(false);
  static bool nightOn;                // == night.on exists
  static bool oldBackupLogged;
  // exe (lower case, no .exe) -> the name `aff set` / `eco` got this night
  static readonly Dictionary<string, string> nightPinned = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
  // the day's whole-exe rules, exe -> {rule name, cpus}: what an unpinned exe goes back to
  static readonly Dictionary<string, string[]> nightDayRules = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);
  static readonly HashSet<string> nightLeft = new HashSet<string>(StringComparer.OrdinalIgnoreCase);   // left alone right now, for the log

  static volatile int heavyCount = -1;   // -1 until PcHwCtl answered `proc top`
  static volatile string heavyBy = "";

  class ExeLoad {
    public readonly List<KeyValuePair<DateTime, double>> Samples = new List<KeyValuePair<DateTime, double>>();
    public bool Heavy;
    public double Last;
  }
  static readonly Dictionary<string, ExeLoad> exeLoads = new Dictionary<string, ExeLoad>(StringComparer.OrdinalIgnoreCase);

  static string NightMarker { get { return Path.Combine(Path.GetDirectoryName(logPath), "night.on"); } }

  static string[] ExeList(string commaList) {
    string[] l = HostSettings.List(commaList);
    for (int i = 0; i < l.Length; i++) l[i] = RuleExe(l[i]);
    return l;
  }

  static void NightStart() {
    new Thread(NightLoop) { IsBackground = true, Name = "night" }.Start();
  }

  static void HandleNightLine(string line) {
    Match m = Regex.Match(line.Trim(), @"^!night\s+([01])$");
    if (!m.Success) { Log("night: unrecognized line"); return; }
    nightLastWordUtc = DateTime.UtcNow;
    bool on = m.Groups[1].Value == "1";
    if (on != nightWanted) { nightWanted = on; nightKick.Set(); }
  }

  static void NightLoop() {
    try { NightLoad(); } catch (Exception ex) { Log("night: night.on unreadable: " + ex.Message); }
    nightWanted = nightOn;   // until the board says otherwise
    DateTime nextSample = DateTime.MinValue;
    while (true) {
      bool sampled = false;
      if (DateTime.UtcNow >= nextSample) {
        nextSample = DateTime.UtcNow.AddMilliseconds(HeavySampleMs);
        try { SampleHeavy(); sampled = true; } catch (Exception ex) { Log("night: proc top: " + ex.Message); }
      }
      try {
        if (nightOn && (DateTime.UtcNow - nightLastWordUtc).TotalMinutes >= HostSettings.NIGHT_NO_WORD_MIN) {
          nightWanted = false;
          NightRelease("no word from the board for " + HostSettings.NIGHT_NO_WORD_MIN + " min");
        } else if (nightWanted && !nightOn) {
          NightEngage();
        } else if (!nightWanted && nightOn) {
          NightRelease("board");
        } else if (nightOn && sampled) {
          NightPass(false);
        }
      } catch (Exception ex) {
        Log("night: " + ex.GetType().Name + ": " + ex.Message);
      }
      nightKick.WaitOne(HeavySampleMs);
    }
  }

  // ---- heavy exes ----

  static void SampleHeavy() {
    Dictionary<string, object> data;
    string r = PipeRequest(HostSettings.HWCTL_PIPE, "proc top 50", HostSettings.HWCTL_TIMEOUT_MS, out data);
    if (r != "ok") { heavyCount = -1; heavyBy = ""; return; }
    DateTime now = DateTime.UtcNow;
    var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    object list;
    if (data != null && data.TryGetValue("processes", out list) && list is System.Collections.IEnumerable) {
      foreach (object o in (System.Collections.IEnumerable)list) {
        var p = o as Dictionary<string, object>;
        if (p == null || !p.ContainsKey("exe") || !p.ContainsKey("cores")) continue;
        string exe = RuleExe(Convert.ToString(p["exe"], CultureInfo.InvariantCulture));
        if (exe.Length == 0 || exe.Equals("idle", StringComparison.OrdinalIgnoreCase)) continue;
        ExeLoad l;
        if (!exeLoads.TryGetValue(exe, out l)) exeLoads[exe] = l = new ExeLoad();
        l.Last = Convert.ToDouble(p["cores"], CultureInfo.InvariantCulture);
        l.Samples.Add(new KeyValuePair<DateTime, double>(now, l.Last));
        seen.Add(exe);
      }
    }
    // not in the top 50: next to nothing
    var gone = new List<string>();
    int keepS = Math.Max(HostSettings.NIGHT_HEAVY_S, HostSettings.NIGHT_CALM_S) + 30;
    foreach (KeyValuePair<string, ExeLoad> kv in exeLoads) {
      ExeLoad l = kv.Value;
      if (!seen.Contains(kv.Key)) { l.Last = 0; l.Samples.Add(new KeyValuePair<DateTime, double>(now, 0)); }
      l.Samples.RemoveAll(s => (now - s.Key).TotalSeconds > keepS);
      if (!l.Heavy && Mean(l, now, HostSettings.NIGHT_HEAVY_S) >= HostSettings.NIGHT_HEAVY_CORES) {
        l.Heavy = true;
        Log("night: " + kv.Key + " heavy (" + Mean(l, now, HostSettings.NIGHT_HEAVY_S).ToString("0.0", CultureInfo.InvariantCulture) + " cores)");
      } else if (l.Heavy && Mean(l, now, HostSettings.NIGHT_CALM_S) < HostSettings.NIGHT_CALM_CORES) {
        l.Heavy = false;
        Log("night: " + kv.Key + " calm again");
      }
      if (!l.Heavy && !seen.Contains(kv.Key) && l.Samples.TrueForAll(s => s.Value == 0)) gone.Add(kv.Key);
    }
    foreach (string g in gone) exeLoads.Remove(g);

    int n = 0;
    string by = "";
    double top = -1;
    foreach (KeyValuePair<string, ExeLoad> kv in exeLoads) {
      if (!kv.Value.Heavy) continue;
      n++;
      if (kv.Value.Last > top) { top = kv.Value.Last; by = kv.Key + ".exe " + kv.Value.Last.ToString("0.0", CultureInfo.InvariantCulture); }
    }
    heavyCount = n;
    heavyBy = by;
  }

  // Mean cores over the last windowS, NaN until the samples span (almost) all of it
  static double Mean(ExeLoad l, DateTime now, int windowS) {
    double sum = 0;
    int count = 0;
    DateTime oldest = now;
    foreach (KeyValuePair<DateTime, double> s in l.Samples) {
      if ((now - s.Key).TotalSeconds > windowS) continue;
      sum += s.Value;
      count++;
      if (s.Key < oldest) oldest = s.Key;
    }
    if (count == 0 || (now - oldest).TotalSeconds < windowS - 2 * HeavySampleMs / 1000) return double.NaN;
    return sum / count;
  }

  static bool IsHeavy(string exe) {
    ExeLoad l;
    return exeLoads.TryGetValue(exe, out l) && l.Heavy;
  }

  // The foreground full-screen app from the user half's idle feed, or null
  static string FullScreenExe() {
    string[] feed = GetHelperFeed("idle");
    if (feed == null) return null;
    string exe = null;
    bool full = false;
    foreach (string line in feed) {
      if (line.StartsWith("pc.fg_full=", StringComparison.Ordinal)) full = line.EndsWith("=1", StringComparison.Ordinal);
      else if (line.StartsWith("pc.fg_exe=", StringComparison.Ordinal)) exe = RuleExe(line.Substring(10).Trim());
    }
    return full && !string.IsNullOrEmpty(exe) ? exe : null;
  }

  // ---- night ----

  static void NightEngage() {
    if (File.Exists(OldLassoBackup)) {
      // saving that night config as "day" would lose the real one
      if (!oldBackupLogged) Log("night: " + OldLassoBackup + " is the original config an older helper left mid-night; copy it back over prolasso.ini, delete it, then night mode works");
      oldBackupLogged = true;
      return;
    }
    using (var c = new PipeClient(HostSettings.HWCTL_PIPE, NightTimeoutMs)) {
      Dictionary<string, object> data;
      string r = c.Request("lasso save day", out data);
      if (r != "ok") { Log("night: lasso save day -> " + r + ", night not engaged"); return; }
      nightDayRules.Clear();
      r = c.Request("aff list", out data);
      object rules;
      if (r == "ok" && data != null && data.TryGetValue("rules", out rules) && rules is System.Collections.IEnumerable) {
        foreach (object o in (System.Collections.IEnumerable)rules) {
          var rule = o as Dictionary<string, object>;
          if (rule == null || !rule.ContainsKey("name") || !rule.ContainsKey("cpus")) continue;
          string name = Convert.ToString(rule["name"], CultureInfo.InvariantCulture);
          if (name.IndexOf(" (", StringComparison.Ordinal) >= 0) continue;   // per-instance rules stay
          nightDayRules[RuleExe(name)] = new[] { name, Convert.ToString(rule["cpus"], CultureInfo.InvariantCulture) };
        }
      }
      nightPinned.Clear();
      nightLeft.Clear();
      nightOn = true;
      NightSave();   // before the first change
      NightPass(true, c);
      r = c.Request("power plan night", out data);
      Log("night: power plan night -> " + r);
    }
  }

  // Pins what should be pinned and unpins what turned heavy. first: the
  // engage, which also moves the day's rules for exes that aren't running.
  static void NightPass(bool first, PipeClient c = null) {
    var keep = new HashSet<string>(NightKeep, StringComparer.OrdinalIgnoreCase);
    foreach (string n in LassoExclusions()) keep.Add(RuleExe(n));
    string fullScreen = FullScreenExe();

    // exe -> name for `aff set`: an existing whole-exe rule as written, else <process>.exe
    var want = new SortedDictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    int skipped = 0;
    if (first) {
      foreach (KeyValuePair<string, string[]> kv in nightDayRules) {
        if (keep.Contains(kv.Key)) continue;
        // `aff set` takes one word ending in .exe ("memory compression" or
        // "x.exe.3.0.new" would become a new rule)
        if (!AffName(kv.Value[0]) || !kv.Value[0].EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) { skipped++; continue; }
        want[kv.Key] = kv.Value[0];
      }
    }
    var running = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    foreach (Process p in Process.GetProcesses()) {
      using (p) {
        if (p.Id <= 4) continue;
        running.Add(p.ProcessName);
        if (keep.Contains(p.ProcessName) || want.ContainsKey(p.ProcessName)) continue;
        if (!AffName(p.ProcessName)) { if (first) skipped++; continue; }
        string[] rule;
        want[p.ProcessName] = nightDayRules.TryGetValue(p.ProcessName, out rule) && AffName(rule[0]) ? rule[0] : p.ProcessName.ToLowerInvariant() + ".exe";
      }
    }

    bool own = c == null;
    int pinned = 0, unpinned = 0, changed = 0, refused = 0;
    string firstRefusal = null;
    var left = new List<string>();
    try {
      foreach (KeyValuePair<string, string> kv in want) {
        string exe = kv.Key;
        bool exempt = !NightAlways.Contains(exe) && (IsHeavy(exe) || exe.Equals(fullScreen, StringComparison.OrdinalIgnoreCase));
        if (exempt) {
          string had;
          if (nightPinned.TryGetValue(exe, out had) && had != null) continue;   // unpinned below
          if (nightLeft.Add(exe) && !first) Log("night: " + exe + " left alone (" + (IsHeavy(exe) ? "heavy" : "full screen") + ")");
          left.Add(exe);
          continue;
        }
        if (nightPinned.ContainsKey(exe)) continue;
        if (c == null) c = new PipeClient(HostSettings.HWCTL_PIPE, NightTimeoutMs);
        Dictionary<string, object> data;
        string r = c.Request("aff set " + kv.Value + " " + NightCores, out data);
        if (r == "ok") {
          changed += Changed(data);
          r = c.Request("eco on " + kv.Value, out data);
          changed += Changed(data);
          nightPinned[exe] = kv.Value;
          pinned++;
          if (nightLeft.Remove(exe) && !first) Log("night: " + exe + " calm, pinned");
        } else {
          refused++;
          if (firstRefusal == null) firstRefusal = kv.Value + ": " + r;
          nightPinned[exe] = null;   // not again this night
        }
      }
      // pinned exes that turned heavy, or went full screen
      foreach (string exe in new List<string>(nightPinned.Keys)) {
        string name = nightPinned[exe];
        if (name == null || NightAlways.Contains(exe) || !running.Contains(exe)) continue;
        bool heavy = IsHeavy(exe), full = exe.Equals(fullScreen, StringComparison.OrdinalIgnoreCase);
        if (!heavy && !full) continue;
        if (c == null) c = new PipeClient(HostSettings.HWCTL_PIPE, NightTimeoutMs);
        Dictionary<string, object> data;
        string[] rule;
        string r = nightDayRules.TryGetValue(exe, out rule)
            ? c.Request("aff set " + rule[0] + " " + rule[1].Replace(';', ','), out data)
            : c.Request("aff clear " + name, out data);
        string e = c.Request("eco off " + name, out data);
        nightPinned.Remove(exe);
        nightLeft.Add(exe);
        unpinned++;
        Log("night: " + exe + " " + (heavy ? "turned heavy" : "went full screen") + ", day rule back (" + r + "), efficiency mode off (" + e + ")");
      }
    } finally {
      if (own && c != null) c.Dispose();
    }
    if (pinned > 0 || unpinned > 0) NightSave();
    if (first)
      Log("night: " + pinned + " exes on CPUs " + NightCores + " in efficiency mode, " + changed + " running processes changed" +
          (left.Count > 0 ? ", left alone: " + string.Join(", ", left) : "") +
          (skipped > 0 ? ", " + skipped + " names with spaces or no .exe skipped" : "") +
          (refused > 0 ? ", " + refused + " refused, first " + firstRefusal : ""));
    else if (pinned > 0)
      Log("night: " + pinned + " more exes pinned (" + changed + " processes)" + (refused > 0 ? ", " + refused + " refused, first " + firstRefusal : ""));
  }

  static void NightRelease(string why) {
    Dictionary<string, object> data;
    string r = PipeRequest(HostSettings.HWCTL_PIPE, "lasso load day", NightTimeoutMs, out data);
    if (r != "ok" && r.IndexOf("no profile", StringComparison.OrdinalIgnoreCase) < 0) {
      Log("night: " + why + ": lasso load day -> " + r + ", retried in 10 s");
      return;
    }
    int moved = Changed(data);
    string e = PipeRequest(HostSettings.HWCTL_PIPE, "eco off all", NightTimeoutMs, out data);
    int ecoChanged = Changed(data);
    string plan = PipeRequest(HostSettings.HWCTL_PIPE, "power plan day", NightTimeoutMs, out data);
    nightOn = false;
    nightPinned.Clear();
    nightDayRules.Clear();
    nightLeft.Clear();
    File.Delete(NightMarker);
    Log("night: " + why + ": " + (r == "ok" ? "normal Process Lasso config back (lasso load day), " + moved + " processes moved" : "lasso load day -> " + r) +
        ", efficiency mode off (" + e + (e == "ok" ? ", " + ecoChanged + " processes" : "") + "), power plan day -> " + plan);
  }

  // night.on: "on <time>", then "pin <exe>\t<name>" and "day <exe>\t<rule>\t<cpus>" lines
  static void NightSave() {
    var sb = new StringBuilder();
    sb.Append("on ").Append(DateTime.Now.ToString("s", CultureInfo.InvariantCulture)).Append('\n');
    foreach (KeyValuePair<string, string> kv in nightPinned)
      if (kv.Value != null) sb.Append("pin ").Append(kv.Key).Append('\t').Append(kv.Value).Append('\n');
    foreach (KeyValuePair<string, string[]> kv in nightDayRules)
      sb.Append("day ").Append(kv.Key).Append('\t').Append(kv.Value[0]).Append('\t').Append(kv.Value[1]).Append('\n');
    File.WriteAllText(NightMarker, sb.ToString());
  }

  static void NightLoad() {
    nightOn = File.Exists(NightMarker);
    if (!nightOn) return;
    foreach (string line in File.ReadAllLines(NightMarker)) {
      string[] f = line.Length > 4 ? line.Substring(4).Split('\t') : new string[0];
      if (line.StartsWith("pin ") && f.Length == 2) nightPinned[f[0]] = f[1];
      else if (line.StartsWith("day ") && f.Length == 3) nightDayRules[f[0]] = new[] { f[1], f[2] };
    }
    Log("night: engaged before this start, " + nightPinned.Count + " exes pinned");
  }

  static bool AffName(string name) { return name.Length > 0 && name.IndexOfAny(new[] { ' ', '\t', ',', ';', '=', '(' }) < 0; }

  static int Changed(Dictionary<string, object> data) {
    object v;
    return data != null && data.TryGetValue("changed", out v) && v != null ? Convert.ToInt32(v, CultureInfo.InvariantCulture) : 0;
  }

  // Process Lasso's ProBalance exclusions, read only: PcHwCtl has no command
  // for them. Empty when the ini is unreadable.
  static string[] LassoExclusions() {
    try {
      string text = File.ReadAllText(HostSettings.LASSO_INI);   // UTF-16 with BOM, detected
      Match m = Regex.Match(text, @"^OocExclusions=(.*)$", RegexOptions.Multiline);
      if (!m.Success) return new string[0];
      return HostSettings.List(m.Groups[1].Value.TrimEnd('\r'));
    } catch (Exception) {
      return new string[0];
    }
  }

  static string RuleExe(string ruleName) {   // "svchost.exe (netprofm -p)" -> "svchost"
    string n = ruleName;
    int sp = n.IndexOf(" (", StringComparison.Ordinal);
    if (sp > 0) n = n.Substring(0, sp);
    if (n.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) n = n.Substring(0, n.Length - 4);
    return n.Trim();
  }
}
