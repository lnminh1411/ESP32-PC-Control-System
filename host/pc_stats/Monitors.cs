// PC Monitor Control, user-session half: DDC/CI on every physical monitor
// (session 0 sees no monitors). VCP 0x10 is brightness, 0xD6 the power mode
// (1 on, 4 off: the monitor's own standby, so DDC can wake it again).
//
// Monitors are numbered from the left. Feed "mon" (@source mon helper:mon),
// sent every 5 s and right after each change:
//   mon.count            physical monitors found
//   mon.<n>.name         model from DDC, e.g. "DELL E1715S" (text)
//   mon.<n>.main         1 on the Windows main display
//   mon.<n>.bright       brightness % (-1: no DDC answer)
//   mon.<n>.power        1 on, 0 off/standby, -1 no DDC answer
// Commands from the service ("M <id> <command>", answered "R <id> ok|err <why>"):
//   bright <n|all> <0-100>
//   power <n|all> on|off
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Threading;

static partial class AgyQuota {
  [StructLayout(LayoutKind.Sequential)]
  struct MonRect { public int Left, Top, Right, Bottom; }

  [StructLayout(LayoutKind.Sequential)]
  struct MONITORINFO {
    public int cbSize;
    public MonRect rcMonitor;
    public MonRect rcWork;
    public uint dwFlags;
  }

  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct PHYSICAL_MONITOR {
    public IntPtr hPhysicalMonitor;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szPhysicalMonitorDescription;
  }

  delegate bool MonitorEnumProc(IntPtr hMonitor, IntPtr hdc, ref MonRect rect, IntPtr data);

  [DllImport("user32.dll")]
  static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumProc proc, IntPtr data);

  [DllImport("user32.dll")]
  static extern bool GetMonitorInfo(IntPtr hMonitor, ref MONITORINFO info);

  [DllImport("dxva2.dll", SetLastError = true)]
  static extern bool GetNumberOfPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, out uint count);

  [DllImport("dxva2.dll", SetLastError = true)]
  static extern bool GetPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, uint count, [Out] PHYSICAL_MONITOR[] monitors);

  [DllImport("dxva2.dll", SetLastError = true)]
  static extern bool DestroyPhysicalMonitors(uint count, PHYSICAL_MONITOR[] monitors);

  [DllImport("dxva2.dll", SetLastError = true)]
  static extern bool GetVCPFeatureAndVCPFeatureReply(IntPtr monitor, byte code, out uint type, out uint current, out uint maximum);

  [DllImport("dxva2.dll", SetLastError = true)]
  static extern bool SetVCPFeature(IntPtr monitor, byte code, uint value);

  const byte VCP_BRIGHTNESS = 0x10;
  const byte VCP_POWER = 0xD6;

  class Mon {
    public IntPtr Handle;
    public string Name;
    public int Left;
    public bool Main;
    public int Bright = -1, Power = -1;
    public uint BrightMax = 100;
  }

  static readonly object ddcLock = new object();   // one DDC conversation at a time (I2C is slow)
  static readonly Regex MonCmdRegex = new Regex(@"^(bright|power)\s+(all|\d{1,2})\s+(\S+)$", RegexOptions.IgnoreCase);

  // Every physical monitor, left to right. The handles stay valid until
  // FreeMonitors; the DDC values are read when read is set.
  static List<Mon> ScanMonitors(bool read, List<PHYSICAL_MONITOR[]> owned) {
    var list = new List<Mon>();
    MonitorEnumProc proc = delegate(IntPtr hMon, IntPtr hdc, ref MonRect rect, IntPtr data) {
      var info = new MONITORINFO { cbSize = Marshal.SizeOf(typeof(MONITORINFO)) };
      GetMonitorInfo(hMon, ref info);
      uint n;
      if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, out n) || n == 0) return true;
      var arr = new PHYSICAL_MONITOR[n];
      if (!GetPhysicalMonitorsFromHMONITOR(hMon, n, arr)) return true;
      owned.Add(arr);
      foreach (PHYSICAL_MONITOR pm in arr) {
        // "DELL E1715S(DisplayPort)" -> "DELL E1715S"
        string name = Regex.Replace(pm.szPhysicalMonitorDescription ?? "", @"\s*\([^)]*\)\s*$", "").Trim();
        list.Add(new Mon {
          Handle = pm.hPhysicalMonitor, Name = name.Length > 0 ? name : "Monitor",
          Left = info.rcMonitor.Left, Main = (info.dwFlags & 1) != 0
        });
      }
      return true;
    };
    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, proc, IntPtr.Zero);
    GC.KeepAlive(proc);
    list.Sort((a, b) => a.Left.CompareTo(b.Left));
    if (read) {
      foreach (Mon m in list) {
        uint type, cur, max;
        if (ReadVcp(m.Handle, VCP_BRIGHTNESS, out type, out cur, out max) && max > 0) {
          m.BrightMax = max;
          m.Bright = (int)Math.Round(cur * 100.0 / max);
        }
        if (ReadVcp(m.Handle, VCP_POWER, out type, out cur, out max)) m.Power = cur == 1 ? 1 : 0;
      }
    }
    return list;
  }

  // DDC/CI replies get lost now and then: one retry
  static bool ReadVcp(IntPtr h, byte code, out uint type, out uint cur, out uint max) {
    if (GetVCPFeatureAndVCPFeatureReply(h, code, out type, out cur, out max)) return true;
    Thread.Sleep(50);
    return GetVCPFeatureAndVCPFeatureReply(h, code, out type, out cur, out max);
  }

  static bool WriteVcp(IntPtr h, byte code, uint value) {
    if (SetVCPFeature(h, code, value)) return true;
    Thread.Sleep(50);
    return SetVCPFeature(h, code, value);
  }

  static void FreeMonitors(List<PHYSICAL_MONITOR[]> owned) {
    foreach (PHYSICAL_MONITOR[] arr in owned) DestroyPhysicalMonitors((uint)arr.Length, arr);
  }

  static void SendMonitorFeed() {
    var lines = new List<string>();
    lock (ddcLock) {
      var owned = new List<PHYSICAL_MONITOR[]>();
      try {
        List<Mon> mons = ScanMonitors(true, owned);
        lines.Add("mon.count=" + mons.Count);
        for (int i = 0; i < mons.Count; i++) {
          string k = "mon." + (i + 1) + ".";
          lines.Add(k + "name=" + mons[i].Name);
          lines.Add(k + "main=" + (mons[i].Main ? 1 : 0));
          lines.Add(k + "bright=" + mons[i].Bright);
          lines.Add(k + "power=" + mons[i].Power);
        }
      } finally {
        FreeMonitors(owned);
      }
    }
    SendFeed("mon", lines);
  }

  static void MonitorLoop() {
    while (true) {
      try { SendMonitorFeed(); } catch (Exception ex) { Log("mon: " + ex.Message); }
      Thread.Sleep(5000);
    }
  }

  // "bright 2 50", "power all off": "ok" or "err <why>"
  static string MonitorCommand(string command) {
    Match m = MonCmdRegex.Match(command.Trim());
    if (!m.Success) return "err usage: bright <n|all> <0-100> | power <n|all> on|off";
    string verb = m.Groups[1].Value.ToLowerInvariant(), target = m.Groups[2].Value.ToLowerInvariant(), arg = m.Groups[3].Value.ToLowerInvariant();
    int pct = 0;
    uint power = 0;
    if (verb == "bright") {
      if (!int.TryParse(arg, NumberStyles.Integer, CultureInfo.InvariantCulture, out pct) || pct < 0 || pct > 100) return "err brightness is 0-100";
    } else if (arg == "on") {
      power = 1;
    } else if (arg == "off") {
      power = 4;
    } else {
      return "err power is on or off";
    }
    lock (ddcLock) {
      var owned = new List<PHYSICAL_MONITOR[]>();
      try {
        List<Mon> mons = ScanMonitors(verb == "bright", owned);   // brightness needs each monitor's maximum
        if (mons.Count == 0) return "err no monitors";
        int first = 0, last = mons.Count - 1;
        if (target != "all") {
          int n = int.Parse(target, CultureInfo.InvariantCulture);
          if (n < 1 || n > mons.Count) return "err no monitor " + n + " (" + mons.Count + " found)";
          first = last = n - 1;
        }
        var failed = new List<string>();
        for (int i = first; i <= last; i++) {
          bool ok = verb == "bright"
              ? WriteVcp(mons[i].Handle, VCP_BRIGHTNESS, (uint)Math.Round(pct * mons[i].BrightMax / 100.0))
              : WriteVcp(mons[i].Handle, VCP_POWER, power);
          if (!ok) failed.Add((i + 1).ToString(CultureInfo.InvariantCulture));
        }
        if (failed.Count > 0) return "err no DDC answer from monitor " + string.Join(", ", failed);
        return "ok";
      } finally {
        FreeMonitors(owned);
      }
    }
  }

  // "M <id> <command>" from the service: runs off the pipe reader's thread
  static void HandleMonitorRequest(string rest) {
    int sp = rest.IndexOf(' ');
    if (sp <= 0) return;
    string id = rest.Substring(0, sp), command = rest.Substring(sp + 1);
    ThreadPool.QueueUserWorkItem(delegate(object o) {
      string reply;
      try { reply = MonitorCommand(command); } catch (Exception ex) { reply = "err " + ex.GetType().Name; }
      SendToService("R " + id + " " + reply + "\n");
      Log("mon: " + command + " -> " + reply);
      try { SendMonitorFeed(); } catch (Exception) { }   // the new state, not 5 s later
    });
  }

  internal static void RunMonitorTest(string command) {
    if (!string.IsNullOrEmpty(command)) Console.WriteLine(MonitorCommand(command));
    var owned = new List<PHYSICAL_MONITOR[]>();
    try {
      List<Mon> mons = ScanMonitors(true, owned);
      for (int i = 0; i < mons.Count; i++)
        Console.WriteLine((i + 1) + " " + mons[i].Name + (mons[i].Main ? " (main)" : "") + ": brightness " + mons[i].Bright + " %, power " + mons[i].Power);
    } finally {
      FreeMonitors(owned);
    }
  }
}
