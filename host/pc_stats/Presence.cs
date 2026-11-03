// Is someone at the desk: the camera check is PcHwCtl's (`presence`,
// docs/ipc.md#presence there, about 3.5 s, frames never leave its agent), the
// rules are the board's (firmware/main/presence.cpp). The board says how often
// it wants a check, `!pres <seconds>` every 30 s, and `!pres now` for one
// straight away; `!pres 0`, or no word for PresWordS, stops them. One check at
// a time on this thread, so a slow one (the model compiling after an agent
// start) only stretches the gap.
//
// The board gets one text stat, so the fields of a check can't arrive apart:
// pc.present = "<n> <yes|no|unknown> <why> <conf %> <zone>". n counts the
// checks, so a new answer shows from the same answer again; why is PcHwCtl's,
// or "pipe" with PcHwCtl unreachable; zone is PcHwCtl's distance class for the
// person seen (desk | near | room), "-" when nobody, too dim or unknown. The
// board reads the first four alone too, so zone could be left off.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text.RegularExpressions;
using System.Threading;

partial class PcStats {
  const int PresTimeoutMs = 70000;   // a first check compiles the model
  const int PresWordS = 90;
  static readonly Regex PresCmdRegex = new Regex(@"^!pres\s+(now|\d{1,5})$", RegexOptions.IgnoreCase);
  static readonly object presLock = new object();
  static readonly AutoResetEvent presWake = new AutoResetEvent(false);
  static Thread presThread;
  static int presEveryS;
  static bool presNow;
  static DateTime presWordUtc, presLastUtc;
  static string presAnswer = "unknown", presWhy = "-", presZone = "-";
  static int presConf, presN;

  static void HandlePresLine(string line) {
    Match m = PresCmdRegex.Match(line.Trim());
    if (!m.Success) { Log("presence: unrecognized command"); return; }
    string arg = m.Groups[1].Value.ToLowerInvariant();
    lock (presLock) {
      presWordUtc = DateTime.UtcNow;
      if (arg == "now") presNow = true;
      else presEveryS = int.Parse(arg, CultureInfo.InvariantCulture);
      if (presThread == null) {
        presThread = new Thread(PresLoop) { IsBackground = true, Name = "presence" };
        presThread.Start();
      }
    }
    presWake.Set();
  }

  static void PresLoop() {
    string lastLogged = null;
    while (true) {
      presWake.WaitOne(1000);
      lock (presLock) {
        DateTime now = DateTime.UtcNow;
        bool word = (now - presWordUtc).TotalSeconds < PresWordS;
        bool due = presEveryS > 0 && (now - presLastUtc).TotalSeconds >= presEveryS;
        if (!word || !(presNow || due)) continue;
        presNow = false;
        presLastUtc = now;
      }
      Dictionary<string, object> data;
      string r = PipeRequest(HostSettings.HWCTL_PIPE, "presence", PresTimeoutMs, out data);
      string answer = "unknown", why = "pipe", zone = "-";
      int conf = 0;
      if (r == "ok" && data != null) {
        answer = PresField(data, "present", "unknown");
        why = PresField(data, "why", "-");
        zone = PresField(data, "zone", "-");
        if (zone != "desk" && zone != "near" && zone != "room") zone = "-";
        double c;
        if (double.TryParse(PresField(data, "conf", "0"), NumberStyles.Float, CultureInfo.InvariantCulture, out c))
          conf = (int)Math.Round(c * 100);
      }
      lock (presLock) {
        presAnswer = answer;
        presWhy = why;
        presConf = conf;
        presZone = zone;
        presN = presN % 999999 + 1;
      }
      sendNow.Set();
      string state = answer + " " + why + (zone == "-" ? "" : " " + zone);
      if (state != lastLogged) {
        Log("presence: " + state + " (" + conf + " %)" + (r == "ok" ? "" : ": " + r));
        lastLogged = state;
      }
    }
  }

  static string PresField(Dictionary<string, object> data, string key, string fallback) {
    object v;
    return data.TryGetValue(key, out v) && v != null ? Convert.ToString(v, CultureInfo.InvariantCulture) : fallback;
  }

  static void PresText(List<KeyValuePair<string, string>> texts) {
    lock (presLock) {
      if (presN == 0) return;
      texts.Add(new KeyValuePair<string, string>("pc.present", presN + " " + presAnswer + " " + presWhy + " " + presConf + " " + presZone));
    }
  }
}
