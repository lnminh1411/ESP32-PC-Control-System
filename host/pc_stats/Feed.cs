// Hardware sensors from PcHwCtl's sensor feed (E:\PC Hardware Controller,
// docs/ipc.md#sensor-feed). PcHwCtl is the only LibreHardwareMonitor reader on
// this PC: the helper sends `subscribe sensors` on its pipe and keeps the
// newest pushed snapshot. Each one is a block of key=value lines (seq=, ts=,
// then every configured key; `key=` means no reading) ended by an empty line,
// about every 500 ms. stats.ini picks keys with `key = @feed [feed_key]`.
//
// No block for 3 s, or the pipe drops: every @feed key goes out empty (NaN on
// the board) and the reader reconnects, 1 s back-off doubling up to 30 s.
//
// The rest of what LibreHardwareMonitor used to give the helper costs nothing
// without it: @ram (GlobalMemoryStatusEx, as LHM's "Total Memory") and @net
// (adapter byte counters, as LHM's "Throughput").

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;
using System.Threading;

partial class PcStats {
  const int FeedStaleMs = 3000;
  static readonly object feedLock = new object();
  static Dictionary<string, double> feedValues = new Dictionary<string, double>(StringComparer.OrdinalIgnoreCase);
  static DateTime feedUtc = DateTime.MinValue;

  static void FeedStart() {
    new Thread(FeedLoop) { IsBackground = true, Name = "feed" }.Start();
  }

  // One feed key, null when it has no reading or the feed is stale
  static double? FeedValue(string key) {
    lock (feedLock) {
      if ((DateTime.UtcNow - feedUtc).TotalMilliseconds > FeedStaleMs) return null;
      double v;
      return feedValues.TryGetValue(key, out v) ? v : (double?)null;
    }
  }

  static void FeedLoop() {
    int backoffMs = 1000;
    string down = null;   // why the last session ended, logged once per outage
    while (true) {
      bool gotBlock = false;
      string why;
      try {
        why = FeedSession(ref gotBlock);
      } catch (Exception ex) {
        why = ex.GetType().Name + ": " + ex.Message;
      }
      if (gotBlock) { backoffMs = 1000; down = null; }
      if (why != down) { Log("feed: " + why + ", reconnecting"); down = why; }
      Thread.Sleep(backoffMs);
      backoffMs = Math.Min(backoffMs * 2, 30000);
    }
  }

  // Connects, subscribes and reads blocks until the pipe drops or goes quiet.
  // Returns why it stopped.
  static string FeedSession(ref bool gotBlock) {
    using (var pipe = new NamedPipeClientStream(".", HostSettings.HWCTL_PIPE, PipeDirection.InOut, PipeOptions.Asynchronous)) {
      try { pipe.Connect(1000); } catch (TimeoutException) { return HostSettings.HWCTL_PIPE + " not running"; }
      var w = new StreamWriter(pipe, Utf8NoBom) { AutoFlush = true, NewLine = "\n" };
      var r = new StreamReader(pipe, Utf8NoBom);
      w.WriteLine("subscribe sensors");
      string ack = ReadLineWithin(r, FeedStaleMs);
      if (ack == null) return HostSettings.HWCTL_PIPE + " closed the pipe";
      Dictionary<string, object> o = Json.ParseObject(ack);
      object ok, err;
      if (o == null || !o.TryGetValue("ok", out ok) || !(ok is bool) || !(bool)ok)
        return "subscribe refused: " + (o != null && o.TryGetValue("error", out err) ? Convert.ToString(err, CultureInfo.InvariantCulture) : ack);

      var block = new Dictionary<string, double>(StringComparer.OrdinalIgnoreCase);
      while (true) {
        string line = ReadLineWithin(r, FeedStaleMs);
        if (line == null) return HostSettings.HWCTL_PIPE + " closed the pipe";
        if (line.Length == 0) {
          lock (feedLock) { feedValues = block; feedUtc = DateTime.UtcNow; }
          if (!gotBlock) Log("feed: " + block.Count + " sensors from " + HostSettings.HWCTL_PIPE);
          gotBlock = true;
          block = new Dictionary<string, double>(StringComparer.OrdinalIgnoreCase);
          continue;
        }
        int eq = line.IndexOf('=');
        if (eq <= 0) continue;
        string k = line.Substring(0, eq).Trim(), v = line.Substring(eq + 1).Trim();
        double d;
        if (k != "seq" && k != "ts" && double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out d)) block[k] = d;
      }
    }
  }

  // null at end of stream; TimeoutException when nothing came within ms
  static string ReadLineWithin(StreamReader r, int ms) {
    var read = r.ReadLineAsync();
    if (!read.Wait(ms)) throw new TimeoutException("no block for " + ms / 1000 + " s");
    return read.Result;
  }

  // --list: one snapshot of the feed
  static void ListFeed() {
    Dictionary<string, object> data;
    string reply = PipeRequest(HostSettings.HWCTL_PIPE, "get sensors", HostSettings.HWCTL_TIMEOUT_MS, out data);
    if (reply != "ok" || data == null) { Console.WriteLine(reply); return; }
    var values = data.ContainsKey("values") ? data["values"] as Dictionary<string, object> : null;
    Console.WriteLine("PcHwCtl sensor feed, seq " + Convert.ToString(data.ContainsKey("seq") ? data["seq"] : "?", CultureInfo.InvariantCulture) +
                      " (keys come from PcHwCtl's config feed.keys):");
    if (values == null) return;
    foreach (KeyValuePair<string, object> kv in values)
      Console.WriteLine("  {0,-14} {1}", kv.Key, kv.Value == null ? "-" : Convert.ToString(kv.Value, CultureInfo.InvariantCulture));
  }

  // ---- @ram ----

  [StructLayout(LayoutKind.Sequential)]
  struct MEMORYSTATUSEX {
    public uint dwLength, dwMemoryLoad;
    public ulong ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile, ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
  }

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool GlobalMemoryStatusEx(ref MEMORYSTATUSEX status);

  // load = used %, used/free in GB
  static double? RamStat(string metric) {
    var s = new MEMORYSTATUSEX { dwLength = (uint)Marshal.SizeOf(typeof(MEMORYSTATUSEX)) };
    if (!GlobalMemoryStatusEx(ref s) || s.ullTotalPhys == 0) return null;
    const double GB = 1024.0 * 1024 * 1024;
    double used = s.ullTotalPhys - s.ullAvailPhys;
    if (metric == "load") return used * 100.0 / s.ullTotalPhys;
    if (metric == "used") return used / GB;
    return s.ullAvailPhys / GB;
  }

  // ---- @net ----

  // Bytes/s summed over the adapters named in r.NetAdapters (wildcards), from
  // the change in their byte counters since the last tick
  static double? NetStat(StatRule r) {
    long bytes = 0;
    bool any = false;
    foreach (NetworkInterface ni in NetworkInterface.GetAllNetworkInterfaces()) {
      bool match = false;
      foreach (string a in r.NetAdapters) if (WildcardMatch(a, ni.Name)) { match = true; break; }
      if (!match) continue;
      try {
        IPInterfaceStatistics st = ni.GetIPStatistics();
        bytes += r.NetMetric == "down" ? st.BytesReceived : st.BytesSent;
        any = true;
      } catch (NetworkInformationException) { }
    }
    DateTime now = DateTime.UtcNow;
    double? rate = null;
    if (any && r.NetLastUtc != DateTime.MinValue && bytes >= r.NetLastBytes) {
      double s = (now - r.NetLastUtc).TotalSeconds;
      if (s > 0) rate = (bytes - r.NetLastBytes) / s;
    }
    r.NetLastBytes = bytes;
    r.NetLastUtc = any ? now : DateTime.MinValue;
    return rate;
  }
}
