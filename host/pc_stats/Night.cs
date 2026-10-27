// Night mode, service side. The board decides when night mode is on; this
// file does the PC parts that need the SYSTEM service.
//
// pc.disp_req (number) is 1 while some process holds a display-required power
// request (a video playing, a presentation), read from `powercfg /requests`
// every 10 s; pc.disp_req_by (text) names the first one. The board does not
// engage night mode on its own while it is 1.
//
// "!night 1" from the board swaps Process Lasso's prolasso.ini for a copy that
// pins every non-critical background process to E-cores 12-17, and pins the
// running ones right away. "!night 0" puts the original file back for the
// governor to reload, and gives every pinned process its old affinity back.
// The board repeats its state every 30 s; after 5 min with no word (board
// gone, service restarted mid-night) the original goes back by itself.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

partial class PcStats {
  const string LassoIni = @"C:\ProgramData\ProcessLasso\config\prolasso.ini";
  const string LassoBackup = LassoIni + ".esp32-night";   // the original while night mode runs
  const string NightCores = "12-17";                      // 6 of the 265K's E-cores (2-5, 10-17)
  const long NightMask = 0x3F000;                         // bits 12-17

  // Never pinned: the kernel and session processes, the desktop shell, audio,
  // this stack and Process Lasso itself. Process Lasso's own ProBalance
  // exclusions (OocExclusions) are added from the ini.
  static readonly HashSet<string> NightKeep = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
    "system", "idle", "registry", "secure system", "memory compression", "smss", "csrss", "wininit", "winlogon",
    "services", "lsass", "lsaiso", "fontdrvhost", "dwm", "audiodg", "explorer", "sihost", "ctfmon", "textinputhost",
    "searchhost", "startmenuexperiencehost", "shellexperiencehost", "logonui", "lockapp", "esp32-helper", "pchwctl",
    "localaihost", "llama-server", "processgovernor", "processlasso", "bitsumsessionagent", "msmpeng", "mpdefendercoreservice",
  };

  static readonly object nightLock = new object();
  static DateTime nightLastWordUtc = DateTime.UtcNow;
  static readonly Dictionary<int, long> nightPinned = new Dictionary<int, long>();   // pid -> affinity before
  static volatile int dispReq = -1;          // -1 unknown (powercfg needs admin)
  static volatile string dispReqBy = "";

  static void NightStart() {
    new Thread(NightLoop) { IsBackground = true, Name = "night" }.Start();
  }

  static void NightLoop() {
    while (true) {
      try { PollDisplayRequests(); } catch (Exception) { dispReq = -1; }
      lock (nightLock) {
        if (File.Exists(LassoBackup) && (DateTime.UtcNow - nightLastWordUtc).TotalMinutes >= 5) {
          Log("night: no word from the board for 5 min, Process Lasso config restored");
          NightLasso(false);
        }
      }
      Thread.Sleep(10000);
    }
  }

  static void HandleNightLine(string line) {
    Match m = Regex.Match(line.Trim(), @"^!night\s+([01])$");
    if (!m.Success) { Log("night: unrecognized line"); return; }
    lock (nightLock) {
      nightLastWordUtc = DateTime.UtcNow;
      bool on = m.Groups[1].Value == "1";
      if (on != File.Exists(LassoBackup)) NightLasso(on);
    }
  }

  // ---- display-required power requests ----

  static void PollDisplayRequests() {
    var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "powercfg.exe"), "/requests") {
      UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, CreateNoWindow = true,
    };
    string output;
    using (Process p = Process.Start(psi)) {
      output = p.StandardOutput.ReadToEnd();
      p.WaitForExit(5000);
      if (p.ExitCode != 0) { dispReq = -1; return; }
    }
    // "DISPLAY:" then "None." or "[PROCESS] \Device\...\brave.exe" + reason lines, until the next "XYZ:" header
    bool inDisplay = false;
    string by = null;
    foreach (string raw in output.Split('\n')) {
      string l = raw.Trim();
      if (Regex.IsMatch(l, @"^[A-Z]+:$")) { inDisplay = l == "DISPLAY:"; continue; }
      if (inDisplay && by == null && l.StartsWith("[")) {
        string what = l.Substring(l.IndexOf(']') + 1).Trim();
        by = Path.GetFileName(what);
        if (string.IsNullOrEmpty(by)) by = what;
      }
    }
    dispReqBy = by ?? "";
    dispReq = by != null ? 1 : 0;
  }

  // ---- Process Lasso ----

  static string IniValue(string text, string key) {
    Match m = Regex.Match(text, "^" + Regex.Escape(key) + "=(.*)$", RegexOptions.Multiline);
    return m.Success ? m.Groups[1].Value.TrimEnd('\r') : null;
  }

  // "2-5;10-17" -> bit mask
  static long CpuListMask(string list) {
    long mask = 0;
    foreach (string part in list.Split(';')) {
      string[] ab = part.Split('-');
      int a, b;
      if (!int.TryParse(ab[0], NumberStyles.Integer, CultureInfo.InvariantCulture, out a)) continue;
      if (ab.Length < 2 || !int.TryParse(ab[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out b)) b = a;
      for (int i = a; i <= b && i < 63; i++) mask |= 1L << i;
    }
    return mask;
  }

  // DefaultAffinitiesEx: "name,0,cpus,name,0,cpus,..." -> name (lower case, as written) -> cpus
  static List<string[]> ParseAffinities(string value) {
    var rules = new List<string[]>();
    if (string.IsNullOrEmpty(value)) return rules;
    string[] v = value.Split(',');
    for (int i = 0; i + 2 < v.Length; i += 3) rules.Add(new[] { v[i], v[i + 1], v[i + 2] });
    return rules;
  }

  static string RuleExe(string ruleName) {   // "svchost.exe (netprofm -p)" -> "svchost"
    string n = ruleName;
    int sp = n.IndexOf(" (", StringComparison.Ordinal);
    if (sp > 0) n = n.Substring(0, sp);
    if (n.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) n = n.Substring(0, n.Length - 4);
    return n.Trim();
  }

  static long AllCpus() { return Environment.ProcessorCount >= 63 ? -1L : (1L << Environment.ProcessorCount) - 1; }

  static void NightLasso(bool on) {
    if (on) NightEngage();
    else NightRelease();
  }

  static void NightEngage() {
    if (!File.Exists(LassoIni)) { Log("night: no " + LassoIni); return; }
    string text = File.ReadAllText(LassoIni);   // UTF-16 with BOM, detected
    var keep = new HashSet<string>(NightKeep, StringComparer.OrdinalIgnoreCase);
    foreach (string n in (IniValue(text, "OocExclusions") ?? "").Split(','))
      if (n.Trim().Length > 0) keep.Add(RuleExe(n.Trim()));

    // every running process outside the keep list, by name
    var names = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
    Process[] procs = Process.GetProcesses();
    foreach (Process p in procs)
      if (p.Id > 4 && !keep.Contains(p.ProcessName)) names.Add(p.ProcessName);

    // existing rules for those names move to 12-17 (others stay), then one
    // rule per remaining name
    List<string[]> rules = ParseAffinities(IniValue(text, "DefaultAffinitiesEx"));
    var ruled = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
    var sb = new StringBuilder();
    foreach (string[] r in rules) {
      string exe = RuleExe(r[0]);
      bool pin = names.Contains(exe) || (!keep.Contains(exe) && r[0].IndexOf(" (", StringComparison.Ordinal) < 0);
      if (sb.Length > 0) sb.Append(',');
      sb.Append(r[0]).Append(',').Append(r[1]).Append(',').Append(pin ? NightCores : r[2]);
      if (r[0].IndexOf(" (", StringComparison.Ordinal) < 0) ruled.Add(exe);
    }
    foreach (string n in names) {
      if (ruled.Contains(n)) continue;
      if (sb.Length > 0) sb.Append(',');
      sb.Append(n.ToLowerInvariant()).Append(".exe,0,").Append(NightCores);
    }
    string line = "DefaultAffinitiesEx=" + sb;
    string night = Regex.IsMatch(text, "^DefaultAffinitiesEx=.*$", RegexOptions.Multiline)
        ? Regex.Replace(text, "^DefaultAffinitiesEx=.*?(?=\r?$)", line.Replace("$", "$$"), RegexOptions.Multiline)
        : text;

    File.Copy(LassoIni, LassoBackup, true);
    File.WriteAllText(LassoIni, night, new UnicodeEncoding(false, true));

    // the running ones now, not at their next start
    int pinned = 0;
    nightPinned.Clear();
    foreach (Process p in procs) {
      if (p.Id <= 4 || keep.Contains(p.ProcessName)) continue;
      try {
        long old = (long)p.ProcessorAffinity;
        if (old == NightMask) continue;
        p.ProcessorAffinity = (IntPtr)NightMask;
        nightPinned[p.Id] = old;
        pinned++;
      } catch (Exception) { }   // protected or gone
    }
    Log("night: Process Lasso night config in (" + names.Count + " process names on E-cores " + NightCores +
        "), " + pinned + " running processes pinned");
  }

  static void NightRelease() {
    if (!File.Exists(LassoBackup)) return;
    string original = File.ReadAllText(LassoBackup);
    File.Copy(LassoBackup, LassoIni, true);
    File.Delete(LassoBackup);

    // pinned processes back to what they had; anything else still on exactly
    // 12-17 (started during the night) gets its original rule or every CPU
    var ruleMask = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
    foreach (string[] r in ParseAffinities(IniValue(original, "DefaultAffinitiesEx")))
      if (r[0].IndexOf(" (", StringComparison.Ordinal) < 0) ruleMask[RuleExe(r[0])] = CpuListMask(r[2]);
    int restored = 0;
    foreach (Process p in Process.GetProcesses()) {
      try {
        if ((long)p.ProcessorAffinity != NightMask) continue;
        long old;
        if (!nightPinned.TryGetValue(p.Id, out old) && !ruleMask.TryGetValue(p.ProcessName, out old)) old = AllCpus();
        if (old == NightMask) continue;
        p.ProcessorAffinity = (IntPtr)old;
        restored++;
      } catch (Exception) { }
    }
    nightPinned.Clear();
    Log("night: Process Lasso config restored, " + restored + " processes back to their CPUs");
  }
}
