// Music-reactive ring, service side. The user half (Audio.cs) captures system
// audio and sends 6 band lengths and 6 band volumes 30 times a second as
// "V <seq><len0..len5><vol0..vol5>" pipe lines; the audio_tx thread forwards
// the latest one to the board as HID output report 7: [0]=7 [1]='V' [2]=seq
// [3..8]=lengths [9..14]=volumes, 64 bytes.
//
// The main tick writes up to 48 KB per tick, 0.7 s on HID: a frame queued
// behind it would arrive long stale. So frames never ride the main link's
// line stream:
//   WinUSB main link: a separate handle on the board's HID interface (the
//     same USB device, matched by its composite parent), report 7 alone.
//   HID main link: report 7 through HidLink, which locks per 64-byte report,
//     so a frame slips in between two chunks of the tick's write.
//   serial or no link: dropped.
// No @auth on the HID side channel: the board would answer on the vendor pipe
// and LinkAuthCheck would take it for the main link's answer. Frames go out
// only while the main link is trusted.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Microsoft.Win32.SafeHandles;

partial class PcStats {
  // the main link once its challenge is under way (Run()); null while none
  static volatile ILink currentLink;

  static readonly object visLock = new object();
  static readonly AutoResetEvent visEvent = new AutoResetEvent(false);
  static readonly byte[] visLevels = new byte[12];   // 6 lengths, 6 volumes
  static byte visSeq;
  static long visTicks;              // Stopwatch timestamp of the latest frame
  static bool visFresh;              // latest frame not yet taken by audio_tx
  static bool visBadLogged;
  static Thread visThread;

  [DllImport("winmm.dll")]
  static extern uint timeBeginPeriod(uint period);

  [DllImport("winmm.dll")]
  static extern uint timeEndPeriod(uint period);

  [StructLayout(LayoutKind.Sequential)]
  struct DEVPROPKEY {
    public Guid fmtid;
    public uint pid;
  }

  [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, EntryPoint = "CM_Get_Device_Interface_PropertyW")]
  static extern int CM_Get_Device_Interface_Property(string deviceInterface, ref DEVPROPKEY key, out uint propertyType,
      [Out] byte[] buffer, ref uint bufferSize, uint flags);

  [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, EntryPoint = "CM_Locate_DevNodeW")]
  static extern int CM_Locate_DevNode(out uint devInst, string deviceId, uint flags);

  [DllImport("cfgmgr32.dll")]
  static extern int CM_Get_Parent(out uint parent, uint devInst, uint flags);

  [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, EntryPoint = "CM_Get_Device_IDW")]
  static extern int CM_Get_Device_ID(uint devInst, [Out] char[] buffer, int bufferLen, uint flags);

  // "V " + seq + 6 lengths + 6 volumes, 2 lowercase hex digits each (ReadUserHalf)
  static void VisLine(string line) {
    byte[] v = new byte[13];
    bool ok = line.Length == 28;
    for (int i = 0; ok && i < 13; i++) {
      int hi = HexDigit(line[2 + 2 * i]), lo = HexDigit(line[3 + 2 * i]);
      if (hi < 0 || lo < 0) ok = false;
      else v[i] = (byte)((hi << 4) | lo);
    }
    if (!ok) {
      if (!visBadLogged) { visBadLogged = true; Log("audio: bad V line from the user half (" + line.Length + " chars)"); }
      return;
    }
    lock (visLock) {
      visSeq = v[0];
      Array.Copy(v, 1, visLevels, 0, 12);
      visTicks = Stopwatch.GetTimestamp();
      visFresh = true;
    }
    visEvent.Set();
  }

  static int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }

  static void StartVisTx() {
    if (visThread != null) return;
    visThread = new Thread(VisTxLoop) { IsBackground = true, Name = "audio_tx", Priority = ThreadPriority.AboveNormal };
    visThread.Start();
  }

  static void VisTxLoop() {
    bool pinned = false;
    try { pinned = CpuSets.PinCurrentThreadToEcores(); } catch (Exception ex) { Log("audio_tx: pin failed: " + ex.Message); }
    try { Log("audio_tx: " + (pinned ? "pinned, " : "not pinned, ") + CpuSets.Describe()); } catch (Exception) { }

    long staleTicks = Stopwatch.Frequency / 10;   // 100 ms
    double tickMs = 1000.0 / Stopwatch.Frequency;
    byte[] rep = new byte[64];
    rep[0] = 0x07;
    rep[1] = 0x56;   // 'V'
    HidSide side = null;
    ILink sideFor = null;        // the main link the side channel and its log-once flags belong to
    long nextOpen = 0;
    bool pairLogged = false, errLogged = false;
    // counters for the 60 s summary, reset after each
    int nIn = 0, nSent = 0, nStale = 0, nNoLink = 0, nTimeout = 0, nErr = 0;
    double maxMs = 0;
    long periodStart = 0;

    while (true) {
      bool got = visEvent.WaitOne(1000);
      ILink link = currentLink;
      if (link != sideFor) {
        if (side != null) { side.Dispose(); side = null; }
        sideFor = link;
        nextOpen = 0;
        pairLogged = false;
        errLogged = false;
      }
      long now = Stopwatch.GetTimestamp();
      if (nIn > 0 && now - periodStart >= 60 * Stopwatch.Frequency) {
        Log("audio_tx: " + ((now - periodStart) / Stopwatch.Frequency) + " s: in " + nIn + ", sent " + nSent +
            " (" + ((link is HidLink) ? "hid link" : (link is WinUsbLink) ? "hid side" : "no link") + "), stale " + nStale +
            ", no link " + nNoLink + ", timeouts " + nTimeout + ", errors " + nErr +
            ", max write " + maxMs.ToString("0.0", CultureInfo.InvariantCulture) + " ms");
        nIn = nSent = nStale = nNoLink = nTimeout = nErr = 0;
        maxMs = 0;
      }
      if (!got) continue;

      byte seq;
      long ticks;
      lock (visLock) {
        if (!visFresh) continue;
        visFresh = false;
        seq = visSeq;
        ticks = visTicks;
        Array.Copy(visLevels, 0, rep, 3, 12);
      }
      if (nIn++ == 0) periodStart = now;
      if (now - ticks > staleTicks) { nStale++; continue; }
      if (link == null || !LinkTrusted() || !(link is HidLink || link is WinUsbLink)) { nNoLink++; continue; }
      rep[2] = seq;

      int r, err = 0;
      long w0 = Stopwatch.GetTimestamp();
      HidLink hid = link as HidLink;
      if (hid != null) {
        r = hid.SendRaw(rep, 50);
      } else {
        if (side == null) {
          if (now < nextOpen) { nNoLink++; continue; }
          nextOpen = now + 3 * Stopwatch.Frequency;
          side = OpenVisSide(((WinUsbLink)link).DevicePath, ref pairLogged);
          if (side == null) { nNoLink++; continue; }
          w0 = Stopwatch.GetTimestamp();
        }
        r = side.Send(rep, 50, out err);
      }
      double ms = (Stopwatch.GetTimestamp() - w0) * tickMs;
      if (ms > maxMs) maxMs = ms;
      if (r == 0) nSent++;
      else if (r == 1) nTimeout++;
      else {
        nErr++;
        if (!errLogged) { errLogged = true; Log("audio_tx: write failed" + (err != 0 ? " (" + err + ")" : "") + (side != null ? ", side channel closed" : "")); }
        if (side != null) { side.Dispose(); side = null; }   // reopened after nextOpen, 3 s apart
      }
    }
  }

  // The board's HID interface on the same USB device as the WinUSB link; null
  // (logged once per main link) when there is none
  static HidSide OpenVisSide(string winUsbPath, ref bool logged) {
    string parent = winUsbPath == null ? null : UsbParentId(winUsbPath);
    if (parent == null) {
      if (!logged) { logged = true; Log("audio_tx: no USB device found for the winusb link, frames dropped"); }
      return null;
    }
    SafeFileHandle h;
    string path = FindHid(out h, parent);
    if (path == null || h == null) {
      if (!logged) { logged = true; Log("audio_tx: no board HID interface on " + parent + ", frames dropped"); }
      return null;
    }
    Log("audio_tx: hid side channel " + path);
    return new HidSide(h, path);
  }

  // Instance id of the USB device an interface belongs to: walks up from the
  // interface's device to the first USB\VID_303A&PID_... node without &MI_
  // (the composite parent; the device itself when not composite)
  static string UsbParentId(string interfacePath) {
    try {
      var key = new DEVPROPKEY { fmtid = new Guid("78c34fc8-104a-4aca-9ea4-524d52996e57"), pid = 256 };   // DEVPKEY_Device_InstanceId
      byte[] buf = new byte[1024];
      uint size = (uint)buf.Length, type;
      if (CM_Get_Device_Interface_Property(interfacePath, ref key, out type, buf, ref size, 0) != 0) return null;
      string id = Encoding.Unicode.GetString(buf, 0, (int)Math.Min(size, (uint)buf.Length)).TrimEnd('\0');
      uint node;
      if (CM_Locate_DevNode(out node, id, 0) != 0) return null;
      char[] idBuf = new char[512];
      for (int depth = 0; depth < 8; depth++) {
        Array.Clear(idBuf, 0, idBuf.Length);
        if (CM_Get_Device_ID(node, idBuf, idBuf.Length, 0) != 0) return null;
        string cur = new string(idBuf).TrimEnd('\0');
        if (cur.StartsWith(@"USB\VID_303A&PID_", StringComparison.OrdinalIgnoreCase) &&
            cur.IndexOf("&MI_", StringComparison.OrdinalIgnoreCase) < 0) return cur;
        uint parent;
        if (CM_Get_Parent(out parent, node, 0) != 0) return null;
        node = parent;
      }
    } catch (Exception) { }
    return null;
  }

  // Write-only HID handle for report 7: no reader, one report in flight
  class HidSide : IDisposable {
    SafeFileHandle handle;
    public readonly string DevicePath;
    // unmanaged: the kernel holds both until the write completes or the cancel lands
    IntPtr report = Marshal.AllocHGlobal(64);
    IntPtr ov = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));
    IntPtr ev = CreateEvent(IntPtr.Zero, true, false, null);

    public HidSide(SafeFileHandle h, string path) {
      handle = h;
      DevicePath = path;
    }

    // 0 sent, 1 timed out (cancelled), 2 failed (err = Win32 error)
    public int Send(byte[] buf, uint timeoutMs, out int err) {
      err = 0;
      if (handle == null || handle.IsClosed || report == IntPtr.Zero) return 2;
      Marshal.Copy(buf, 0, report, 64);
      ResetEvent(ev);
      OVERLAPPED o = new OVERLAPPED();
      o.hEvent = ev;
      Marshal.StructureToPtr(o, ov, false);
      if (WriteFile(handle, report, 64, IntPtr.Zero, ov)) return 0;
      err = Marshal.GetLastWin32Error();
      if (err != ERROR_IO_PENDING) return 2;
      err = 0;
      uint transferred;
      if (WaitForSingleObject(ev, timeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(handle, ov);
        // wait for the cancel to land before the buffer is reused; it may have completed after all
        return GetOverlappedResult(handle, ov, out transferred, true) ? 0 : 1;
      }
      if (!GetOverlappedResult(handle, ov, out transferred, false)) { err = Marshal.GetLastWin32Error(); return 2; }
      return 0;
    }

    public void Dispose() {
      if (handle != null && !handle.IsClosed) {
        try { CancelIoEx(handle, IntPtr.Zero); } catch (Exception) { }
        try { handle.Close(); } catch (Exception) { }
      }
      if (ev != IntPtr.Zero) { CloseHandle(ev); ev = IntPtr.Zero; }
      if (report != IntPtr.Zero) { Marshal.FreeHGlobal(report); report = IntPtr.Zero; }
      if (ov != IntPtr.Zero) { Marshal.FreeHGlobal(ov); ov = IntPtr.Zero; }
    }
  }

  // --vis-test: Audio.cs runs capture + DSP here (no service) and prints levels
  static void VisTest(int seconds) {
    consoleMode = true;   // Log() to the console
    if (Thread.CurrentThread.GetApartmentState() == ApartmentState.MTA) { Audio.RunTest(seconds); return; }
    Thread t = new Thread(delegate() { Audio.RunTest(seconds); }) { Name = "vis_test" };
    t.SetApartmentState(ApartmentState.MTA);
    t.Start();
    t.Join();
  }

  // --vis-bench: report 7 at exactly 30 Hz on the board's HID interface (opened
  // shared, works while the service runs), a sweeping pattern, timing stats
  static void VisBench(int seconds) {
    consoleMode = true;
    SafeFileHandle h;
    string path = FindHid(out h);
    if (path == null || h == null) { Console.WriteLine("vis-bench: no board HID interface (VID 303A, page FF00, 64-byte output)"); return; }
    Console.WriteLine("vis-bench: " + path);
    bool pinned = CpuSets.PinCurrentThreadToEcores();
    Console.WriteLine("vis-bench: " + (pinned ? "pinned, " : "not pinned, ") + CpuSets.Describe());
    Thread.CurrentThread.Priority = ThreadPriority.AboveNormal;

    const double periodMs = 1000.0 / 30;
    int total = seconds * 30;
    double tickMs = 1000.0 / Stopwatch.Frequency;
    var writeMs = new List<double>(total);
    var lateMs = new List<double>(total);
    int sent = 0, timeouts = 0, fails = 0, late5 = 0, failRun = 0, lastErr = 0;
    double winWriteSum = 0, winWriteMax = 0, winLateMax = 0;
    int winSent = 0, nextReportS = 5;
    byte[] rep = new byte[64];
    rep[0] = 0x07;
    rep[1] = 0x56;   // 'V'
    double elapsedMs = 0;
    timeBeginPeriod(1);
    try {
      using (var side = new HidSide(h, path)) {
        long start = Stopwatch.GetTimestamp();
        for (int i = 0; i < total; i++) {
          double due = i * periodMs;
          // sleep to ~2 ms before the slot, then spin: Sleep alone is +-1 ms even at 1 ms timer resolution
          while (true) {
            double left = due - (Stopwatch.GetTimestamp() - start) * tickMs;
            if (left <= 0) break;
            if (left > 3) Thread.Sleep((int)(left - 2));
            else Thread.SpinWait(20);
          }
          double t = (Stopwatch.GetTimestamp() - start) * tickMs;
          double late = t - due;
          lateMs.Add(late);
          if (late > 5) late5++;
          if (late > winLateMax) winLateMax = late;

          double ts = t / 1000.0;
          rep[2] = (byte)i;
          for (int b = 0; b < 6; b++) {
            rep[3 + b] = (byte)Math.Round(128 + 127 * Math.Sin(2 * Math.PI * (ts * 0.5 + b / 6.0)));
            rep[9 + b] = (byte)Math.Round(128 + 127 * Math.Sin(2 * Math.PI * (ts * 0.3 + b / 6.0)));
          }
          int err;
          long w0 = Stopwatch.GetTimestamp();
          int r = side.Send(rep, 50, out err);
          double wms = (Stopwatch.GetTimestamp() - w0) * tickMs;
          if (r == 0) {
            sent++; winSent++; failRun = 0;
            writeMs.Add(wms);
            winWriteSum += wms;
            if (wms > winWriteMax) winWriteMax = wms;
          } else {
            if (r == 1) timeouts++; else { fails++; lastErr = err; }
            if (++failRun >= 30) { Console.WriteLine("vis-bench: 30 failures in a row (last error " + lastErr + "), stopping"); break; }
          }

          if (t >= nextReportS * 1000.0) {
            Console.WriteLine(string.Format(CultureInfo.InvariantCulture,
                "{0,4} s: sent {1}, timeouts {2}, failures {3}, write avg {4:0.00} max {5:0.00} ms, late max {6:0.00} ms",
                nextReportS, sent, timeouts, fails, winSent > 0 ? winWriteSum / winSent : 0, winWriteMax, winLateMax));
            nextReportS += 5;
            winSent = 0;
            winWriteSum = winWriteMax = winLateMax = 0;
          }
        }
        elapsedMs = (Stopwatch.GetTimestamp() - start) * tickMs;
      }
    } finally {
      timeEndPeriod(1);
    }

    Console.WriteLine("vis-bench: " + (sent + timeouts + fails) + " frames in " + (elapsedMs / 1000).ToString("0.00", CultureInfo.InvariantCulture) + " s");
    Console.WriteLine("  sent " + sent + ", timeouts " + timeouts + ", failures " + fails + (fails > 0 ? " (last error " + lastErr + ")" : ""));
    Console.WriteLine("  write ms: " + TimingStats(writeMs));
    Console.WriteLine("  lateness ms: " + TimingStats(lateMs) + ", late > 5 ms: " + late5);
    Console.WriteLine("  effective rate " + (elapsedMs > 0 ? sent * 1000.0 / elapsedMs : 0).ToString("0.00", CultureInfo.InvariantCulture) + " frames/s");
  }

  // avg / p50 / p99 / max
  static string TimingStats(List<double> v) {
    if (v.Count == 0) return "none";
    var s = new List<double>(v);
    s.Sort();
    double sum = 0;
    foreach (double d in s) sum += d;
    return string.Format(CultureInfo.InvariantCulture, "avg {0:0.00} p50 {1:0.00} p99 {2:0.00} max {3:0.00}",
        sum / s.Count, s[(int)(0.50 * (s.Count - 1))], s[(int)(0.99 * (s.Count - 1))], s[s.Count - 1]);
  }

  // optional seconds after a test flag: "--vis-bench 120"
  static int ArgSeconds(string[] args, int i, int def) {
    int n;
    if (i + 1 < args.Length && int.TryParse(args[i + 1], NumberStyles.None, CultureInfo.InvariantCulture, out n) && n > 0 && n <= 86400) return n;
    return def;
  }
}
