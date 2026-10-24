// ESP32 PC Stats: Windows service that reads configured hardware sensors
// through LibreHardwareMonitor (lhwm-wrapper.dll, OpenRGB build) and streams
// them to the carrier board over a vendor HID interface (report ID 6) with
// native USB CDC fallback (Espressif VID 303A).
//
// Sensor mapping is defined in stats.ini (in the executable's directory):
//   key = <hardware> | <sensor type> | <sensor name> [| first|avg|sum|max|min]
//   key = @uptime
//   key = @drive <letter>: used|free|total|load
//   key = @cpu_mhz                       Task Manager's CPU speed (no LHWM cost)
//   key = @counter <category> | <counter> | <instance> [| scale]   any Windows perf counter
//   @every <hardware> <ms>               update that hardware at most every ms
//   @source <name> <url> <ms>            poll HTTP key=value stream on background thread
//
// Wire format, ASCII lines (firmware side: firmware/carrier_main/pcstats.h):
//   @s key=value key=value ...   (split into lines <= 300 chars)
//   @t key text                  (text stats, up to 80 chars)
//   @e <seq>                     end of one sample (every @interval ms, default 500)
//
// Usage: esp32-pcstats.exe            run as service (installed by install.ps1)
//        esp32-pcstats.exe --console  run in console, print stats each tick
//        esp32-pcstats.exe --list     print every sensor once and exit
//        esp32-pcstats.exe --bench    time one Update() of each hardware
//        --transport auto|hid|serial  transport mode (default: auto)
//        --port COMn                  skip VID auto-detect, force serial COMn
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Management;
using System.Net;
using System.Runtime.InteropServices;
using System.ServiceProcess;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Web.Script.Serialization;
using LibreHardwareMonitor.Hardware;
using LHWMWrapperNameSpace;
using Microsoft.Win32.SafeHandles;

class PcStats : ServiceBase {
  const string Vid = "VID_303A";
  const int MaxLineLen = 300;
  static int intervalMs = 500;             // "@interval <ms>" in stats.ini overrides

  [DllImport("kernel32.dll")]
  static extern ulong GetTickCount64();

  const uint DIGCF_PRESENT = 0x00000002;
  const uint DIGCF_DEVICEINTERFACE = 0x00000010;

  const uint GENERIC_READ = 0x80000000;
  const uint GENERIC_WRITE = 0x40000000;
  const uint FILE_SHARE_READ = 0x00000001;
  const uint FILE_SHARE_WRITE = 0x00000002;
  const uint OPEN_EXISTING = 3;
  const uint FILE_FLAG_OVERLAPPED = 0x40000000;

  const uint WAIT_OBJECT_0 = 0x00000000;
  const int ERROR_IO_PENDING = 997;

  [StructLayout(LayoutKind.Sequential)]
  struct SP_DEVICE_INTERFACE_DATA {
    public uint cbSize;
    public Guid InterfaceClassGuid;
    public uint Flags;
    public IntPtr Reserved;
  }

  [StructLayout(LayoutKind.Sequential)]
  struct HIDP_CAPS {
    public ushort Usage;
    public ushort UsagePage;
    public ushort InputReportByteLength;
    public ushort OutputReportByteLength;
    public ushort FeatureReportByteLength;
    public ushort Reserved0, Reserved1, Reserved2, Reserved3, Reserved4;
    public ushort Reserved5, Reserved6, Reserved7, Reserved8, Reserved9;
    public ushort Reserved10, Reserved11, Reserved12, Reserved13, Reserved14;
    public ushort Reserved15, Reserved16;
    public ushort NumberLinkCollectionNodes;
    public ushort NumberInputButtonCaps;
    public ushort NumberInputValueCaps;
    public ushort NumberInputDataIndices;
    public ushort NumberOutputButtonCaps;
    public ushort NumberOutputValueCaps;
    public ushort NumberOutputDataIndices;
    public ushort NumberFeatureButtonCaps;
    public ushort NumberFeatureValueCaps;
    public ushort NumberFeatureDataIndices;
  }

  [StructLayout(LayoutKind.Sequential)]
  struct OVERLAPPED {
    public IntPtr Internal;
    public IntPtr InternalHigh;
    public int Offset;
    public int OffsetHigh;
    public IntPtr hEvent;
  }

  [DllImport("hid.dll", SetLastError = true)]
  static extern void HidD_GetHidGuid(out Guid hidGuid);

  [DllImport("setupapi.dll", SetLastError = true)]
  static extern IntPtr SetupDiGetClassDevs(ref Guid classGuid, IntPtr enumerator, IntPtr hwndParent, uint flags);

  [DllImport("setupapi.dll", SetLastError = true)]
  static extern bool SetupDiDestroyDeviceInfoList(IntPtr deviceInfoSet);

  [DllImport("setupapi.dll", SetLastError = true)]
  static extern bool SetupDiEnumDeviceInterfaces(IntPtr deviceInfoSet, IntPtr deviceInfoData, ref Guid interfaceClassGuid, uint memberIndex, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData);

  [DllImport("setupapi.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr deviceInfoSet, ref SP_DEVICE_INTERFACE_DATA deviceInterfaceData, IntPtr deviceInterfaceDetailData, uint deviceInterfaceDetailDataSize, out uint requiredSize, IntPtr deviceInfoData);

  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern SafeFileHandle CreateFile(string lpFileName, uint dwDesiredAccess, uint dwShareMode, IntPtr lpSecurityAttributes, uint dwCreationDisposition, uint dwFlagsAndAttributes, IntPtr hTemplateFile);

  [DllImport("hid.dll", SetLastError = true)]
  static extern bool HidD_GetPreparsedData(SafeFileHandle hidDeviceObject, out IntPtr preparsedData);

  [DllImport("hid.dll", SetLastError = true)]
  static extern bool HidD_FreePreparsedData(IntPtr preparsedData);

  [DllImport("hid.dll", SetLastError = true)]
  static extern int HidP_GetCaps(IntPtr preparsedData, out HIDP_CAPS capabilities);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool WriteFile(SafeFileHandle hFile, IntPtr lpBuffer, uint nNumberOfBytesToWrite, IntPtr lpNumberOfBytesWritten, IntPtr lpOverlapped);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern IntPtr CreateEvent(IntPtr lpEventAttributes, bool bManualReset, bool bInitialState, string lpName);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool ResetEvent(IntPtr hEvent);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool GetOverlappedResult(SafeFileHandle hFile, IntPtr lpOverlapped, out uint lpNumberOfBytesTransferred, bool bWait);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool CancelIo(SafeFileHandle hFile);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool CloseHandle(IntPtr hObject);

  interface ILink : IDisposable {
    void Write(string text);
    void Close();
  }

  class SerialLink : ILink {
    SerialPort port;

    public SerialLink(string name) {
      port = OpenPort(name);
    }

    public void Write(string text) {
      if (port.BytesToRead > 0) port.ReadExisting();
      port.Write(text);
    }

    public void Close() {
      if (port != null) {
        try { port.Close(); } catch (Exception) { }
        port = null;
      }
    }

    public void Dispose() {
      Close();
    }
  }

  class HidLink : ILink {
    SafeFileHandle handle;
    IntPtr hEvent;
    // The kernel uses the report buffer and the OVERLAPPED after WriteFile
    // returns (I/O pending): both live in unmanaged memory so the GC can
    // never move them mid-write.
    IntPtr report = Marshal.AllocHGlobal(64);
    IntPtr ov = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));

    public HidLink(SafeFileHandle h) {
      handle = h;
      hEvent = CreateEvent(IntPtr.Zero, true, false, null);
    }

    public void Write(string text) {
      byte[] bytes = Encoding.ASCII.GetBytes(text);
      int offset = 0;
      while (offset < bytes.Length) {
        int count = Math.Min(63, bytes.Length - offset);
        byte[] buf = new byte[64];
        buf[0] = 0x06;   // HID_REPORT_ID_VENDOR
        Array.Copy(bytes, offset, buf, 1, count);
        Marshal.Copy(buf, 0, report, 64);
        SendReport();
        offset += count;
      }
    }

    void SendReport() {
      ResetEvent(hEvent);
      OVERLAPPED o = new OVERLAPPED();
      o.hEvent = hEvent;
      Marshal.StructureToPtr(o, ov, false);
      bool ok = WriteFile(handle, report, 64, IntPtr.Zero, ov);
      if (!ok) {
        int err = Marshal.GetLastWin32Error();
        if (err == ERROR_IO_PENDING) {
          uint waitRes = WaitForSingleObject(hEvent, 1000);
          uint transferred;
          if (waitRes == WAIT_OBJECT_0) {
            if (!GetOverlappedResult(handle, ov, out transferred, false)) {
              throw new IOException("HID GetOverlappedResult failed: " + Marshal.GetLastWin32Error());
            }
          } else {
            CancelIo(handle);
            GetOverlappedResult(handle, ov, out transferred, true);   // let the cancel land before reuse
            throw new IOException("HID write timeout (" + waitRes + ")");
          }
        } else {
          throw new IOException("HID WriteFile failed: " + err);
        }
      }
    }

    public void Close() {
      if (handle != null && !handle.IsClosed) {
        try { CancelIo(handle); } catch (Exception) { }
        try { handle.Close(); } catch (Exception) { }
      }
      if (hEvent != IntPtr.Zero) {
        try { CloseHandle(hEvent); } catch (Exception) { }
        hEvent = IntPtr.Zero;
      }
      if (report != IntPtr.Zero) { Marshal.FreeHGlobal(report); report = IntPtr.Zero; }
      if (ov != IntPtr.Zero) { Marshal.FreeHGlobal(ov); ov = IntPtr.Zero; }
    }

    public void Dispose() {
      Close();
    }
  }

  static string transportMode = "auto";
  static bool consoleMode;
  static string fixedPort;
  static string logPath;

  Thread worker;
  volatile bool stop;

  enum StatKind { Sensor, Uptime, Drive, Counter, CpuMhz }

  class StatRule {
    public string Key;
    public StatKind Kind;
    // Sensor rule
    public string[] HwAlternatives;
    public string SensorType;
    public string SensorName;
    public string Reduce;
    public List<ISensor> ResolvedSensors = new List<ISensor>();
    // Drive rule
    public string DriveLetter;
    public string DriveMetric;
    // Windows performance counter rule (@counter, @cpu_mhz)
    public System.Diagnostics.PerformanceCounter Counter;
    public float Scale = 1;
  }

  class HttpSource {
    public string Name;
    public string Url;
    public int IntervalMs;
    public Thread Worker;
    public volatile bool Stop;
    public readonly object Lock = new object();
    public Dictionary<string, float> Numbers = new Dictionary<string, float>();
    public Dictionary<string, string> Texts = new Dictionary<string, string>();
    public DateTime LastSuccessUtc = DateTime.MinValue;
    WebRequest currentRequest;

    public void Start() {
      Worker = new Thread(Run) { IsBackground = true, Name = "source_" + Name };
      Worker.Start();
    }

    public void StopWorker() {
      Stop = true;
      lock (Lock) {
        if (currentRequest != null) {
          try { currentRequest.Abort(); } catch (Exception) { }
        }
      }
      if (Worker != null) Worker.Join(1000);
    }

    void Run() {
      while (!Stop) {
        DateTime t0 = DateTime.UtcNow;
        try {
          WebRequest req = WebRequest.Create(Url);
          HttpWebRequest httpReq = req as HttpWebRequest;
          if (httpReq != null) {
            httpReq.Timeout = 5000;
            httpReq.ReadWriteTimeout = 5000;
            httpReq.Proxy = null;
          }
          lock (Lock) { currentRequest = req; }

          using (WebResponse resp = req.GetResponse())
          using (Stream stream = resp.GetResponseStream())
          using (StreamReader reader = new StreamReader(stream, Encoding.UTF8)) {
            var newNumbers = new Dictionary<string, float>();
            var newTexts = new Dictionary<string, string>();
            string line;
            while ((line = reader.ReadLine()) != null) {
              line = line.Trim();
              if (line.Length == 0) continue;
              int eq = line.IndexOf('=');
              if (eq < 0) continue;
              string k = line.Substring(0, eq).Trim();
              string v = line.Substring(eq + 1).Trim();

              // Valid key: [a-z0-9_.], max 23 chars; skip others.
              if (k.Length == 0 || k.Length > 23 || !Regex.IsMatch(k, @"^[a-z0-9_.]+$", RegexOptions.IgnoreCase))
                continue;

              float num;
              if (float.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out num) && !float.IsNaN(num) && !float.IsInfinity(num)) {
                newNumbers[k] = num;
              } else {
                string text = CleanText(v);
                if (text.Length > 80) text = text.Substring(0, 80);
                newTexts[k] = text;
              }
            }

            lock (Lock) {
              Numbers = newNumbers;
              Texts = newTexts;
              LastSuccessUtc = DateTime.UtcNow;
            }
          }
        } catch (Exception) {
          // Hub might be sleeping or unreachable, keep going
        } finally {
          lock (Lock) { currentRequest = null; }
        }

        int elapsed = (int)(DateTime.UtcNow - t0).TotalMilliseconds;
        int wait = IntervalMs - elapsed;
        if (wait < 50) wait = 50;
        int slept = 0;
        while (!Stop && slept < wait) {
          Thread.Sleep(Math.Min(100, wait - slept));
          slept += 100;
        }
      }
    }

    static string CleanText(string s) {
      if (s == null) return "";
      return s.Replace('\r', ' ').Replace('\n', ' ').Replace('\t', ' ').Trim();
    }
  }

  static void StopSources(List<HttpSource> sources) {
    if (sources == null) return;
    for (int i = 0; i < sources.Count; i++) sources[i].StopWorker();
  }

  static void StartSources(List<HttpSource> sources) {
    if (sources == null) return;
    for (int i = 0; i < sources.Count; i++) sources[i].Start();
  }

  // Claude usage logic ported from aiusage (MIT, Ahsan Habib Akik)
  // https://github.com/ahsanhabibakik/aiusage

  // Rough per-model $/token rates for estimating local spend from session logs.
  //
  // Best-effort only -- Anthropic/OpenAI can change list prices at any time.
  // Rates are USD per token (list price, not cache-adjusted beyond the simple
  // read/write split below). Unknown models fall back to DEFAULT.
  struct ClaudeRate {
    public double Input;
    public double Output;
    public double CacheWrite;
    public double CacheRead;
    public ClaudeRate(double inp, double outp, double cw, double cr) {
      Input = inp; Output = outp; CacheWrite = cw; CacheRead = cr;
    }
  }

  static readonly ClaudeRate DefaultClaudeRate = new ClaudeRate(3.00, 15.00, 3.75, 0.30); // sonnet
  static readonly KeyValuePair<string, ClaudeRate>[] ClaudeRates = new KeyValuePair<string, ClaudeRate>[] {
    new KeyValuePair<string, ClaudeRate>("opus", new ClaudeRate(15.00, 75.00, 18.75, 1.50)),
    new KeyValuePair<string, ClaudeRate>("sonnet", new ClaudeRate(3.00, 15.00, 3.75, 0.30)),
    new KeyValuePair<string, ClaudeRate>("haiku", new ClaudeRate(0.80, 4.00, 1.00, 0.08)),
    new KeyValuePair<string, ClaudeRate>("fable", new ClaudeRate(3.00, 15.00, 3.75, 0.30))
  };

  static ClaudeRate RateForModel(string model) {
    if (model != null) {
      string m = model.ToLowerInvariant();
      for (int i = 0; i < ClaudeRates.Length; i++) {
        if (m.IndexOf(ClaudeRates[i].Key, StringComparison.Ordinal) >= 0) {
          return ClaudeRates[i].Value;
        }
      }
    }
    return DefaultClaudeRate;
  }

  static double EstimateCostUsd(string model, long inputTokens, long outputTokens, long cacheWrite, long cacheRead) {
    ClaudeRate rate = RateForModel(model);
    double cost = (inputTokens * rate.Input
                 + outputTokens * rate.Output
                 + cacheWrite * rate.CacheWrite
                 + cacheRead * rate.CacheRead) / 1000000.0;
    return cost;
  }

  class ClaudeLiveWindow {
    public float Utilization;
    public string ResetsAt;
    public DateTime ResetsAtUtc;
  }

  class ClaudeModelLimit {
    public string Kind;
    public string ModelDisplayName;
    public float Percent;
    public string ResetsAt;
    public DateTime ResetsAtUtc;
  }

  class ClaudeExtraUsage {
    public bool IsEnabled;
    public float Utilization;
  }

  class ClaudeLiveSnapshot {
    public ClaudeLiveWindow FiveHour;
    public ClaudeLiveWindow SevenDay;
    public List<ClaudeModelLimit> Limits = new List<ClaudeModelLimit>();
    public ClaudeExtraUsage ExtraUsage;
  }

  class ClaudePace {
    public float ProjectedUsedPct;
    public string Verdict; // ok, tight, over
  }

  static ClaudePace ComputePace(float usedPct, DateTime resetUtc, double windowMs, DateTime nowUtc) {
    double remainingMs = (resetUtc - nowUtc).TotalMilliseconds;
    double elapsedMs = windowMs - remainingMs;
    if (elapsedMs < windowMs * 0.05 || elapsedMs <= 0) {
      return null;
    }
    double projected = (usedPct / elapsedMs) * windowMs;
    float projVal = (float)Math.Round(Math.Min(projected, 999.0));
    string verdict;
    if (projected > 100.0) verdict = "over";
    else if (projected > 90.0) verdict = "tight";
    else verdict = "ok";
    return new ClaudePace { ProjectedUsedPct = projVal, Verdict = verdict };
  }

  class ClaudeLogEntry {
    public DateTime UtcTime;
    public long Tokens;
    public double Cost;
  }

  class ClaudeFileCache {
    public string Path;
    public long BytesParsed;
    public DateTime LastModifiedUtc;
    public Dictionary<string, ClaudeLogEntry> EntriesById = new Dictionary<string, ClaudeLogEntry>();
    public List<ClaudeLogEntry> NonIdEntries = new List<ClaudeLogEntry>();
  }

  class ClaudeSource {
    public string ClaudeDir;
    public int LiveMs;

    Thread liveWorker;
    Thread scanWorker;
    volatile bool stop;
    readonly object syncLock = new object();
    HttpWebRequest currentLiveReq;

    ClaudeLiveSnapshot lastGoodData;
    DateTime lastGoodFetchUtc = DateTime.MinValue;
    bool hasLastGood;
    string currentError = ""; // "" when fine
    string currentPlan = "";

    bool firstScanFinished;
    Dictionary<string, ClaudeFileCache> fileCache = new Dictionary<string, ClaudeFileCache>(StringComparer.OrdinalIgnoreCase);
    List<ClaudeLogEntry> flatEntries = new List<ClaudeLogEntry>();

    const double SessionWindowMs = 5.0 * 3600.0 * 1000.0;
    const double WeeklyWindowMs = 7.0 * 24.0 * 3600.0 * 1000.0;

    public ClaudeSource(string dir, int liveMs) {
      ClaudeDir = dir;
      LiveMs = liveMs > 0 ? liveMs : 120000;
    }

    public void Start() {
      liveWorker = new Thread(RunLive) { IsBackground = true, Name = "claude_live" };
      scanWorker = new Thread(RunScan) { IsBackground = true, Name = "claude_scan", Priority = ThreadPriority.BelowNormal };
      liveWorker.Start();
      scanWorker.Start();
    }

    public void Stop() {
      stop = true;
      lock (syncLock) {
        if (currentLiveReq != null) {
          try { currentLiveReq.Abort(); } catch (Exception) { }
        }
      }
      if (liveWorker != null) liveWorker.Join(1000);
      if (scanWorker != null) scanWorker.Join(1000);
    }

    void RunLive() {
      while (!stop) {
        int sleepMs = LiveMs;
        try {
          sleepMs = FetchLiveOnce();
        } catch (Exception) {
          lock (syncLock) {
            currentError = "request_failed";
          }
        }

        int slept = 0;
        while (!stop && slept < sleepMs) {
          int chunk = Math.Min(100, sleepMs - slept);
          Thread.Sleep(chunk);
          slept += chunk;
        }
      }
    }

    public int FetchLiveOnce() {
      string credPath = Path.Combine(ClaudeDir, ".credentials.json");
      if (!File.Exists(credPath)) {
        lock (syncLock) {
          currentError = "no_credentials";
        }
        return LiveMs;
      }

      string token = null;
      long expiresAt = 0;
      string planText = "";
      try {
        string json;
        using (var fs = new FileStream(credPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        using (var reader = new StreamReader(fs, Encoding.UTF8)) {
          json = reader.ReadToEnd();
        }
        var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
        var dict = serializer.Deserialize<Dictionary<string, object>>(json);
        if (dict != null && dict.ContainsKey("claudeAiOauth") && dict["claudeAiOauth"] is Dictionary<string, object>) {
          var oauth = (Dictionary<string, object>)dict["claudeAiOauth"];
          if (oauth.ContainsKey("accessToken") && oauth["accessToken"] != null) {
            token = oauth["accessToken"].ToString();
          }
          if (oauth.ContainsKey("expiresAt") && oauth["expiresAt"] != null) {
            expiresAt = ConvertToLong(oauth["expiresAt"]);
          }
          planText = ExtractPlanName(oauth);
        }
      } catch (Exception) {
        lock (syncLock) {
          currentError = "no_credentials";
        }
        return LiveMs;
      }

      if (string.IsNullOrEmpty(token)) {
        lock (syncLock) {
          currentError = "no_credentials";
        }
        return LiveMs;
      }

      long nowEpochMs = (long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalMilliseconds;
      if (expiresAt > 0 && expiresAt <= nowEpochMs) {
        lock (syncLock) {
          currentError = "token_expired";
          if (!string.IsNullOrEmpty(planText)) currentPlan = planText;
        }
        return LiveMs;
      }

      HttpWebRequest req = (HttpWebRequest)WebRequest.Create("https://api.anthropic.com/api/oauth/usage");
      req.Method = "GET";
      req.Timeout = 10000;
      req.ReadWriteTimeout = 10000;
      req.ContentType = "application/json";
      req.Headers["Authorization"] = "Bearer " + token;
      req.Headers["anthropic-beta"] = "oauth-2025-04-20";

      lock (syncLock) { currentLiveReq = req; }

      try {
        using (HttpWebResponse resp = (HttpWebResponse)req.GetResponse())
        using (Stream s = resp.GetResponseStream())
        using (StreamReader reader = new StreamReader(s, Encoding.UTF8)) {
          string respJson = reader.ReadToEnd();
          var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
          var respDict = serializer.Deserialize<Dictionary<string, object>>(respJson);
          if (respDict != null) {
            ClaudeLiveSnapshot snap = ParseLiveSnapshot(respDict);
            lock (syncLock) {
              lastGoodData = snap;
              lastGoodFetchUtc = DateTime.UtcNow;
              hasLastGood = true;
              currentError = "";
              if (!string.IsNullOrEmpty(planText)) currentPlan = planText;
            }
          }
        }
        return LiveMs;
      } catch (WebException we) {
        string err = "request_failed";
        int status = 0;
        if (we.Response is HttpWebResponse) {
          status = (int)((HttpWebResponse)we.Response).StatusCode;
          if (status == 401 || status == 403) err = "not_logged_in";
          else if (status == 429) err = "rate_limited";
          else err = "request_failed";
        }
        lock (syncLock) {
          currentError = err;
          if (!string.IsNullOrEmpty(planText)) currentPlan = planText;
        }
        if (status == 429) {
          return 2 * LiveMs;
        }
        return LiveMs;
      } catch (Exception) {
        lock (syncLock) {
          currentError = "request_failed";
          if (!string.IsNullOrEmpty(planText)) currentPlan = planText;
        }
        return LiveMs;
      } finally {
        lock (syncLock) { currentLiveReq = null; }
      }
    }

    static string ExtractPlanName(Dictionary<string, object> oauth) {
      if (oauth == null) return "";
      string tier = null;
      if (oauth.ContainsKey("rateLimitTier") && oauth["rateLimitTier"] != null) {
        tier = oauth["rateLimitTier"].ToString();
      }
      string sub = null;
      if (oauth.ContainsKey("subscriptionType") && oauth["subscriptionType"] != null) {
        sub = oauth["subscriptionType"].ToString();
      }
      string raw = tier;
      if (string.IsNullOrEmpty(raw) || raw == "default_claude_ai") {
        if (!string.IsNullOrEmpty(sub)) raw = sub;
      }
      if (string.IsNullOrEmpty(raw)) return "";
      string cleaned = raw.Replace("default_", "").Replace("_", " ").Trim();
      if (cleaned.Length == 0) return "";
      return CultureInfo.InvariantCulture.TextInfo.ToTitleCase(cleaned).Replace("X", "x");
    }

    static ClaudeLiveSnapshot ParseLiveSnapshot(Dictionary<string, object> root) {
      var snap = new ClaudeLiveSnapshot();

      if (root.ContainsKey("five_hour") && root["five_hour"] is Dictionary<string, object>) {
        var fh = (Dictionary<string, object>)root["five_hour"];
        snap.FiveHour = ParseWindow(fh);
      }

      if (root.ContainsKey("seven_day") && root["seven_day"] is Dictionary<string, object>) {
        var sd = (Dictionary<string, object>)root["seven_day"];
        snap.SevenDay = ParseWindow(sd);
      }

      if (root.ContainsKey("limits") && root["limits"] is System.Collections.IEnumerable) {
        foreach (object item in (System.Collections.IEnumerable)root["limits"]) {
          if (item is Dictionary<string, object>) {
            var ld = (Dictionary<string, object>)item;
            string kind = ld.ContainsKey("kind") && ld["kind"] != null ? ld["kind"].ToString() : "";
            if (kind == "weekly_scoped") {
              string modelName = null;
              if (ld.ContainsKey("scope") && ld["scope"] is Dictionary<string, object>) {
                var scope = (Dictionary<string, object>)ld["scope"];
                if (scope.ContainsKey("model") && scope["model"] is Dictionary<string, object>) {
                  var model = (Dictionary<string, object>)scope["model"];
                  if (model.ContainsKey("display_name") && model["display_name"] != null) {
                    modelName = model["display_name"].ToString();
                  }
                }
              }
              if (!string.IsNullOrEmpty(modelName)) {
                var lim = new ClaudeModelLimit();
                lim.Kind = kind;
                lim.ModelDisplayName = modelName;
                lim.Percent = ConvertToFloat(ld.ContainsKey("percent") ? ld["percent"] : null);
                if (ld.ContainsKey("resets_at") && ld["resets_at"] != null) {
                  lim.ResetsAt = ld["resets_at"].ToString();
                  DateTime dt;
                  if (DateTime.TryParse(lim.ResetsAt, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out dt)) {
                    lim.ResetsAtUtc = dt;
                  }
                }
                snap.Limits.Add(lim);
              }
            }
          }
        }
      }

      if (root.ContainsKey("extra_usage") && root["extra_usage"] is Dictionary<string, object>) {
        var eu = (Dictionary<string, object>)root["extra_usage"];
        snap.ExtraUsage = new ClaudeExtraUsage();
        if (eu.ContainsKey("is_enabled") && eu["is_enabled"] is bool) {
          snap.ExtraUsage.IsEnabled = (bool)eu["is_enabled"];
        }
        snap.ExtraUsage.Utilization = ConvertToFloat(eu.ContainsKey("utilization") ? eu["utilization"] : null);
      }

      return snap;
    }

    static ClaudeLiveWindow ParseWindow(Dictionary<string, object> dict) {
      var w = new ClaudeLiveWindow();
      w.Utilization = ConvertToFloat(dict.ContainsKey("utilization") ? dict["utilization"] : null);
      if (dict.ContainsKey("resets_at") && dict["resets_at"] != null) {
        w.ResetsAt = dict["resets_at"].ToString();
        DateTime dt;
        if (DateTime.TryParse(w.ResetsAt, CultureInfo.InvariantCulture, DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out dt)) {
          w.ResetsAtUtc = dt;
        }
      }
      return w;
    }

    static long ConvertToLong(object val) {
      if (val == null) return 0;
      if (val is int) return (long)(int)val;
      if (val is long) return (long)val;
      if (val is double) return (long)(double)val;
      if (val is decimal) return (long)(decimal)val;
      long res;
      if (long.TryParse(val.ToString(), out res)) return res;
      return 0;
    }

    static float ConvertToFloat(object val) {
      if (val == null) return 0f;
      if (val is float) return (float)val;
      if (val is double) return (float)(double)val;
      if (val is int) return (float)(int)val;
      if (val is long) return (float)(long)val;
      if (val is decimal) return (float)(decimal)val;
      float res;
      if (float.TryParse(val.ToString(), NumberStyles.Float, CultureInfo.InvariantCulture, out res)) return res;
      return 0f;
    }

    void RunScan() {
      while (!stop) {
        try {
          ScanLocalLogsOnce();
        } catch (Exception) { }

        int slept = 0;
        while (!stop && slept < 60000) {
          int chunk = Math.Min(500, 60000 - slept);
          Thread.Sleep(chunk);
          slept += chunk;
        }
      }
    }

    public void ScanLocalLogsOnce() {
      string projDir = Path.Combine(ClaudeDir, "projects");
      if (!Directory.Exists(projDir)) {
        lock (syncLock) {
          firstScanFinished = true;
          flatEntries = new List<ClaudeLogEntry>();
        }
        return;
      }

      DateTime nowUtc = DateTime.UtcNow;
      DateTime cutoff31d = nowUtc.AddDays(-31);

      string[] files = null;
      try {
        files = Directory.GetFiles(projDir, "*.jsonl", SearchOption.AllDirectories);
      } catch (Exception) {
        lock (syncLock) { firstScanFinished = true; }
        return;
      }

      var currentFiles = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
      var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };

      for (int i = 0; i < files.Length; i++) {
        if (stop) return;
        string path = files[i];
        FileInfo fi = null;
        try {
          fi = new FileInfo(path);
          if (!fi.Exists || fi.LastWriteTimeUtc < cutoff31d) continue;
        } catch (Exception) {
          continue;
        }

        currentFiles.Add(path);

        ClaudeFileCache cache;
        if (!fileCache.TryGetValue(path, out cache)) {
          cache = new ClaudeFileCache { Path = path };
          fileCache[path] = cache;
        }

        if (fi.Length < cache.BytesParsed) {
          cache.BytesParsed = 0;
          cache.EntriesById.Clear();
          cache.NonIdEntries.Clear();
        }

        if (fi.Length == cache.BytesParsed && fi.LastWriteTimeUtc == cache.LastModifiedUtc) {
          continue;
        }

        long bytesToRead = fi.Length - cache.BytesParsed;
        if (bytesToRead <= 0) continue;

        try {
          using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
            if (cache.BytesParsed > 0) {
              fs.Seek(cache.BytesParsed, SeekOrigin.Begin);
            }
            byte[] buffer = new byte[bytesToRead];
            int bytesRead = 0;
            while (bytesRead < buffer.Length) {
              int r = fs.Read(buffer, bytesRead, buffer.Length - bytesRead);
              if (r <= 0) break;
              bytesRead += r;
            }

            int lastNl = -1;
            for (int b = bytesRead - 1; b >= 0; b--) {
              if (buffer[b] == 10) {
                lastNl = b;
                break;
              }
            }

            if (lastNl < 0) {
              continue;
            }

            string chunk = Encoding.UTF8.GetString(buffer, 0, lastNl + 1);
            cache.BytesParsed += (lastNl + 1);
            cache.LastModifiedUtc = fi.LastWriteTimeUtc;

            ParseLinesIntoCache(chunk, cache, cutoff31d, serializer);
          }
        } catch (Exception) { }
      }

      var removePaths = new List<string>();
      foreach (var kv in fileCache) {
        if (!currentFiles.Contains(kv.Key)) {
          removePaths.Add(kv.Key);
        }
      }
      for (int i = 0; i < removePaths.Count; i++) {
        fileCache.Remove(removePaths[i]);
      }

      var newFlat = new List<ClaudeLogEntry>();
      var dedupById = new Dictionary<string, ClaudeLogEntry>();
      var nonIdList = new List<ClaudeLogEntry>();

      foreach (var kv in fileCache) {
        ClaudeFileCache fc = kv.Value;

        var removeIds = new List<string>();
        foreach (var idKv in fc.EntriesById) {
          if (idKv.Value.UtcTime < cutoff31d) removeIds.Add(idKv.Key);
          else {
            ClaudeLogEntry existing;
            if (!dedupById.TryGetValue(idKv.Key, out existing) || idKv.Value.UtcTime >= existing.UtcTime) {
              dedupById[idKv.Key] = idKv.Value;
            }
          }
        }
        for (int r = 0; r < removeIds.Count; r++) fc.EntriesById.Remove(removeIds[r]);

        for (int n = fc.NonIdEntries.Count - 1; n >= 0; n--) {
          if (fc.NonIdEntries[n].UtcTime < cutoff31d) {
            fc.NonIdEntries.RemoveAt(n);
          } else {
            nonIdList.Add(fc.NonIdEntries[n]);
          }
        }
      }

      foreach (var e in dedupById.Values) newFlat.Add(e);
      newFlat.AddRange(nonIdList);

      lock (syncLock) {
        flatEntries = newFlat;
        firstScanFinished = true;
      }
    }

    static void ParseLinesIntoCache(string text, ClaudeFileCache cache, DateTime cutoff, JavaScriptSerializer serializer) {
      int start = 0;
      while (start < text.Length) {
        int nl = text.IndexOf('\n', start);
        int len = (nl >= 0 ? nl : text.Length) - start;
        if (len > 0) {
          string line = text.Substring(start, len).Trim();
          if (line.Length > 0 &&
              line.IndexOf("\"assistant\"", StringComparison.Ordinal) >= 0 &&
              line.IndexOf("\"usage\"", StringComparison.Ordinal) >= 0) {
            try {
              var root = serializer.Deserialize<Dictionary<string, object>>(line);
              if (root != null &&
                  root.ContainsKey("type") && "assistant".Equals(root["type"]) &&
                  root.ContainsKey("timestamp") && root["timestamp"] != null &&
                  root.ContainsKey("message") && root["message"] is Dictionary<string, object>) {

                DateTime ts;
                if (DateTime.TryParse(root["timestamp"].ToString(), CultureInfo.InvariantCulture,
                                      DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out ts)) {
                  if (ts >= cutoff) {
                    var msg = (Dictionary<string, object>)root["message"];
                    if (msg.ContainsKey("usage") && msg["usage"] is Dictionary<string, object>) {
                      var usage = (Dictionary<string, object>)msg["usage"];
                      string model = msg.ContainsKey("model") && msg["model"] != null ? msg["model"].ToString() : "";
                      string msgId = msg.ContainsKey("id") && msg["id"] != null ? msg["id"].ToString() : "";

                      long inputTokens = ConvertToLong(usage.ContainsKey("input_tokens") ? usage["input_tokens"] : null);
                      long outputTokens = ConvertToLong(usage.ContainsKey("output_tokens") ? usage["output_tokens"] : null);
                      long cacheWrite = ConvertToLong(usage.ContainsKey("cache_creation_input_tokens") ? usage["cache_creation_input_tokens"] : null);
                      long cacheRead = ConvertToLong(usage.ContainsKey("cache_read_input_tokens") ? usage["cache_read_input_tokens"] : null);

                      long tokens = inputTokens + outputTokens + cacheWrite + cacheRead;
                      double cost = EstimateCostUsd(model, inputTokens, outputTokens, cacheWrite, cacheRead);

                      var entry = new ClaudeLogEntry { UtcTime = ts, Tokens = tokens, Cost = cost };
                      if (!string.IsNullOrEmpty(msgId)) {
                        cache.EntriesById[msgId] = entry;
                      } else {
                        cache.NonIdEntries.Add(entry);
                      }
                    }
                  }
                }
              }
            } catch (Exception) { }
          }
        }
        if (nl < 0) break;
        start = nl + 1;
      }
    }

    public void GetSnapshot(List<string> tokens, List<KeyValuePair<string, string>> texts) {
      ClaudeLiveSnapshot live = null;
      DateTime lastGoodFetch = DateTime.MinValue;
      bool isGood = false;
      string err = "";
      string plan = "";
      bool scanDone = false;
      List<ClaudeLogEntry> entries = null;

      lock (syncLock) {
        live = lastGoodData;
        lastGoodFetch = lastGoodFetchUtc;
        isGood = hasLastGood;
        err = currentError ?? "";
        plan = currentPlan ?? "";
        scanDone = firstScanFinished;
        entries = flatEntries;
      }

      DateTime nowUtc = DateTime.UtcNow;
      bool fresh = (lastGoodFetch != DateTime.MinValue && (nowUtc - lastGoodFetch).TotalMilliseconds <= (3 * LiveMs));
      bool stale = (!fresh && isGood);

      tokens.Add("claude.ok=" + (fresh ? "1" : "0"));
      tokens.Add("claude.stale=" + (stale ? "1" : "0"));

      if (isGood && live != null) {
        if (live.FiveHour != null) {
          tokens.Add("claude.s_pct=" + Fmt(live.FiveHour.Utilization));
          if (live.FiveHour.ResetsAtUtc != DateTime.MinValue) {
            double remSec = Math.Max(0.0, (live.FiveHour.ResetsAtUtc - nowUtc).TotalSeconds);
            tokens.Add("claude.s_reset_s=" + Fmt((float)remSec));
            texts.Add(new KeyValuePair<string, string>("claude.s_reset_at", live.FiveHour.ResetsAtUtc.ToLocalTime().ToString("HH:mm")));

            ClaudePace pace = ComputePace(live.FiveHour.Utilization, live.FiveHour.ResetsAtUtc, SessionWindowMs, nowUtc);
            if (pace != null) {
              tokens.Add("claude.s_pace=" + Fmt(pace.ProjectedUsedPct));
              texts.Add(new KeyValuePair<string, string>("claude.s_pace_v", pace.Verdict));
            }
          }
        }

        if (live.SevenDay != null) {
          tokens.Add("claude.w_pct=" + Fmt(live.SevenDay.Utilization));
          if (live.SevenDay.ResetsAtUtc != DateTime.MinValue) {
            double remSec = Math.Max(0.0, (live.SevenDay.ResetsAtUtc - nowUtc).TotalSeconds);
            tokens.Add("claude.w_reset_s=" + Fmt((float)remSec));
            texts.Add(new KeyValuePair<string, string>("claude.w_reset_at", live.SevenDay.ResetsAtUtc.ToLocalTime().ToString("ddd dd MMM HH:mm", CultureInfo.InvariantCulture)));

            ClaudePace pace = ComputePace(live.SevenDay.Utilization, live.SevenDay.ResetsAtUtc, WeeklyWindowMs, nowUtc);
            if (pace != null) {
              tokens.Add("claude.w_pace=" + Fmt(pace.ProjectedUsedPct));
              texts.Add(new KeyValuePair<string, string>("claude.w_pace_v", pace.Verdict));
            }
          }
        }

        if (live.Limits != null) {
          for (int i = 0; i < live.Limits.Count; i++) {
            ClaudeModelLimit lim = live.Limits[i];
            if (lim.Kind == "weekly_scoped" && !string.IsNullOrEmpty(lim.ModelDisplayName)) {
              string clean = Regex.Replace(lim.ModelDisplayName.ToLowerInvariant(), "[^a-z0-9]", "");
              int maxModelLen = 23 - "claude.w_".Length - "_pct".Length;
              if (clean.Length > maxModelLen) clean = clean.Substring(0, maxModelLen);
              tokens.Add("claude.w_" + clean + "_pct=" + Fmt(lim.Percent));
            }
          }
        }

        if (live.ExtraUsage != null && live.ExtraUsage.IsEnabled) {
          tokens.Add("claude.x_pct=" + Fmt(live.ExtraUsage.Utilization));
        }
      }

      if (scanDone && entries != null) {
        DateTime t5h = nowUtc.AddHours(-5);
        DateTime tMidnight = DateTime.Today.ToUniversalTime();
        DateTime t7d = nowUtc.AddDays(-7);
        DateTime t30d = nowUtc.AddDays(-30);

        long tok5h = 0;
        long tokToday = 0;
        long tok7d = 0;
        long tok30d = 0;
        double usdToday = 0.0;
        double usd30d = 0.0;

        for (int i = 0; i < entries.Count; i++) {
          ClaudeLogEntry e = entries[i];
          if (e.UtcTime >= t5h) tok5h += e.Tokens;
          if (e.UtcTime >= tMidnight) {
            tokToday += e.Tokens;
            usdToday += e.Cost;
          }
          if (e.UtcTime >= t7d) tok7d += e.Tokens;
          if (e.UtcTime >= t30d) {
            tok30d += e.Tokens;
            usd30d += e.Cost;
          }
        }

        tokens.Add("claude.tok_5h=" + Fmt((float)tok5h));
        tokens.Add("claude.tok_today=" + Fmt((float)tokToday));
        tokens.Add("claude.tok_7d=" + Fmt((float)tok7d));
        tokens.Add("claude.tok_30d=" + Fmt((float)tok30d));
        tokens.Add("claude.usd_today=" + Fmt((float)usdToday));
        tokens.Add("claude.usd_30d=" + Fmt((float)usd30d));
      }

      if (!string.IsNullOrEmpty(plan)) {
        texts.Add(new KeyValuePair<string, string>("claude.plan", plan));
      }
      texts.Add(new KeyValuePair<string, string>("claude.err", err));
    }
  }

  static void ClaudeTest(string dir) {
    try {
      ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072;
    } catch (Exception) { }
    var src = new ClaudeSource(dir, 120000);
    src.FetchLiveOnce();
    src.ScanLocalLogsOnce();
    var tokens = new List<string>();
    var texts = new List<KeyValuePair<string, string>>();
    src.GetSnapshot(tokens, texts);
    if (tokens.Count > 0) {
      Console.WriteLine("@s " + string.Join(" ", tokens.ToArray()));
    }
    for (int i = 0; i < texts.Count; i++) {
      Console.WriteLine("@t " + texts[i].Key + " " + texts[i].Value);
    }
  }

  // "@every <hardware> <ms>": update matching hardware at most that often
  // (e.g. the CPU, whose update costs ~320 ms; temperatures move slowly).
  static List<KeyValuePair<string, int>> everyRules = new List<KeyValuePair<string, int>>();
  static float cpuBaseMhz;

  static System.Diagnostics.PerformanceCounter OpenCounter(string cat, string name, string inst) {
    try {
      var c = new System.Diagnostics.PerformanceCounter(cat, name, inst, true);
      c.NextValue();   // the first sample of a rate counter is always 0
      return c;
    } catch (Exception e) {
      Log("counter " + cat + "/" + name + "(" + inst + "): " + e.Message);
      return null;
    }
  }

  static void Main(string[] args) {
    try { ServicePointManager.SecurityProtocol |= (SecurityProtocolType)3072; } catch (Exception) { }
    for (int i = 0; i < args.Length; i++) {
      if (args[i] == "--console") consoleMode = true;
      else if (args[i] == "--port" && i + 1 < args.Length) fixedPort = args[++i];
      else if (args[i] == "--transport" && i + 1 < args.Length) transportMode = args[++i].ToLowerInvariant();
      else if (args[i] == "--list") { List(); return; }
      else if (args[i] == "--bench") { Bench(); return; }
      else if (args[i] == "--claude" && i + 1 < args.Length) { ClaudeTest(args[++i]); return; }
    }
    string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "ESP32PcStats");
    Directory.CreateDirectory(dir);
    logPath = Path.Combine(dir, "log.txt");
    if (consoleMode) {
      var s = new PcStats();
      Console.CancelKeyPress += delegate(object o, ConsoleCancelEventArgs e) { e.Cancel = true; s.stop = true; };
      s.Run();
    } else {
      ServiceBase.Run(new PcStats());
    }
  }

  public PcStats() { ServiceName = "ESP32PcStats"; CanStop = true; CanShutdown = true; }

  protected override void OnStart(string[] args) {
    for (int i = 0; i < args.Length; i++) {
      if (args[i] == "--port" && i + 1 < args.Length) fixedPort = args[++i];
      else if (args[i] == "--transport" && i + 1 < args.Length) transportMode = args[++i].ToLowerInvariant();
    }
    worker = new Thread(Run) { IsBackground = true, Name = "pcstats" };
    worker.Start();
  }

  protected override void OnStop() { stop = true; if (worker != null) worker.Join(5000); }
  protected override void OnShutdown() { OnStop(); }

  static void Log(string msg) {
    string line = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss ") + msg;
    if (consoleMode) { Console.WriteLine(line); return; }
    try {
      var fi = new FileInfo(logPath);
      if (fi.Exists && fi.Length > 4 << 20) {   // rotate: log.txt + log.old.txt stay <= 8 MB together
        string old = Path.ChangeExtension(logPath, ".old.txt");
        if (File.Exists(old)) File.Delete(old);
        File.Move(logPath, old);
      }
      File.AppendAllText(logPath, line + Environment.NewLine);
    } catch (IOException) { }
  }

  static Computer OpenComputer() {
    var c = new Computer {
      IsCpuEnabled = true, IsGpuEnabled = true, IsMemoryEnabled = true, IsMotherboardEnabled = true,
      IsStorageEnabled = true, IsNetworkEnabled = true, IsControllerEnabled = true,
      IsPsuEnabled = true, IsBatteryEnabled = true
    };
    c.Open();
    return c;
  }

  static void CollectHardware(IHardware hw, List<IHardware> list) {
    list.Add(hw);
    foreach (IHardware sub in hw.SubHardware) CollectHardware(sub, list);
  }

  static List<IHardware> AllHardware(Computer c) {
    var list = new List<IHardware>();
    foreach (IHardware hw in c.Hardware) CollectHardware(hw, list);
    return list;
  }

  static List<ISensor> Sensors(Computer c) {
    var list = new List<ISensor>();
    foreach (IHardware hw in AllHardware(c)) {
      list.AddRange(hw.Sensors);
    }
    return list;
  }

  static string Clean(string s) { return (s ?? "").Replace('\t', ' ').Replace('\n', ' ').Replace('\r', ' ').Trim(); }

  static void List() {
    var c = OpenComputer();
    c.Accept(new UpdateVisitor());
    foreach (ISensor s in Sensors(c)) {
      string hwCol = s.Hardware.HardwareType.ToString() + "/" + Clean(s.Hardware.Name);
      Console.WriteLine("{0,-36} {1,-12} {2,-30} {3}",
                        hwCol, s.SensorType, Clean(s.Name),
                        s.Value.HasValue ? s.Value.Value.ToString("0.###", CultureInfo.InvariantCulture) : "-");
    }
    c.Close();
  }

  // ms per Update() of each hardware, to see what limits the interval
  static void Bench() {
    var c = OpenComputer();
    c.Accept(new UpdateVisitor());
    foreach (IHardware hw in AllHardware(c)) {
      var t = System.Diagnostics.Stopwatch.StartNew();
      for (int i = 0; i < 5; i++) hw.Update();
      Console.WriteLine("{0,8:0.0} ms  {1}/{2}", t.Elapsed.TotalMilliseconds / 5, hw.HardwareType, Clean(hw.Name));
    }
    c.Close();
  }

  static string FindHid(out SafeFileHandle matchedHandle) {
    matchedHandle = null;
    Guid hidGuid;
    HidD_GetHidGuid(out hidGuid);
    IntPtr hDevInfo = SetupDiGetClassDevs(ref hidGuid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == IntPtr.Zero || hDevInfo.ToInt64() == -1) return null;

    try {
      SP_DEVICE_INTERFACE_DATA ifData = new SP_DEVICE_INTERFACE_DATA();
      ifData.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));

      for (uint i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, IntPtr.Zero, ref hidGuid, i, ref ifData); i++) {
        uint reqSize = 0;
        SetupDiGetDeviceInterfaceDetail(hDevInfo, ref ifData, IntPtr.Zero, 0, out reqSize, IntPtr.Zero);
        if (reqSize == 0) continue;

        IntPtr pDetail = Marshal.AllocHGlobal((int)reqSize);
        try {
          Marshal.WriteInt32(pDetail, IntPtr.Size == 8 ? 8 : 6);
          if (SetupDiGetDeviceInterfaceDetail(hDevInfo, ref ifData, pDetail, reqSize, out reqSize, IntPtr.Zero)) {
            string path = Marshal.PtrToStringUni(new IntPtr(pDetail.ToInt64() + 4));
            if (path != null && path.IndexOf("vid_303a", StringComparison.OrdinalIgnoreCase) >= 0) {
              SafeFileHandle hFile = CreateFile(
                  path,
                  GENERIC_READ | GENERIC_WRITE,
                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                  IntPtr.Zero,
                  OPEN_EXISTING,
                  FILE_FLAG_OVERLAPPED,
                  IntPtr.Zero);

              if (hFile != null && !hFile.IsInvalid) {
                bool matched = false;
                IntPtr pData = IntPtr.Zero;
                if (HidD_GetPreparsedData(hFile, out pData)) {
                  try {
                    HIDP_CAPS caps;
                    if (HidP_GetCaps(pData, out caps) >= 0) {
                      if (caps.UsagePage == 0xFF00 && caps.Usage == 0x01 && caps.OutputReportByteLength == 64) {
                        matched = true;
                      }
                    }
                  } finally {
                    HidD_FreePreparsedData(pData);
                  }
                }
                if (matched) {
                  matchedHandle = hFile;
                  return path;
                }
                hFile.Close();
              }
            }
          }
        } finally {
          Marshal.FreeHGlobal(pDetail);
        }
      }
    } finally {
      SetupDiDestroyDeviceInfoList(hDevInfo);
    }
    return null;
  }

  static string FindPort() {
    if (fixedPort != null) return fixedPort;
    using (var q = new ManagementObjectSearcher("SELECT Name, PNPDeviceID FROM Win32_PnPEntity WHERE PNPDeviceID LIKE 'USB\\\\" + Vid + "%'")) {
      foreach (ManagementObject o in q.Get()) {
        var m = Regex.Match((string)o["Name"] ?? "", @"\((COM\d+)\)");
        if (m.Success) return m.Groups[1].Value;
      }
    }
    return null;
  }

  static SerialPort OpenPort(string name) {
    // DTR on: the firmware only writes its console to a host that set DTR,
    // and we drain that output so its USB buffer never fills.
    var p = new SerialPort(name, 115200) { DtrEnable = true, RtsEnable = true, WriteTimeout = 1000, ReadTimeout = 1, NewLine = "\n" };
    p.Open();
    return p;
  }

  static string Fmt(float? v) {
    if (!v.HasValue || float.IsNaN(v.Value) || float.IsInfinity(v.Value)) return "";
    return v.Value.ToString("G7", CultureInfo.InvariantCulture);
  }

  static bool WildcardMatch(string pattern, string input) {
    if (pattern == null || input == null) return false;
    if (pattern == "*") return true;
    string regexPattern = "^" + Regex.Escape(pattern).Replace("\\*", ".*") + "$";
    return Regex.IsMatch(input, regexPattern, RegexOptions.IgnoreCase);
  }

  static List<StatRule> LoadRules(string path, List<HttpSource> sources, ref ClaudeSource claudeSource) {
    var list = new List<StatRule>();
    everyRules.Clear();
    if (!File.Exists(path)) return list;
    string[] lines;
    try {
      lines = File.ReadAllLines(path);
    } catch (Exception e) {
      Log("failed to read stats.ini: " + e.Message);
      return list;
    }
    for (int i = 0; i < lines.Length; i++) {
      string raw = lines[i];
      // '#' starts a comment only at line start or after whitespace and before
      // whitespace/end: sensor names like "Fan #1" or "*Core #*" keep theirs.
      raw = Regex.Replace(raw, @"(^|\s)#(\s.*|$)", "");
      raw = raw.Trim();
      if (raw.Length == 0) continue;

      if (raw.StartsWith("@interval", StringComparison.OrdinalIgnoreCase)) {
        int ms;
        if (int.TryParse(raw.Substring(9).Trim(), out ms) && ms >= 200 && ms <= 10000) intervalMs = ms;
        else Log("stats.ini line " + (i + 1) + ": @interval wants 200-10000 ms");
        continue;
      }

      if (raw.StartsWith("@every", StringComparison.OrdinalIgnoreCase)) {
        string[] ep = raw.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        int ms;
        if (ep.Length == 3 && int.TryParse(ep[2], out ms) && ms > 0) everyRules.Add(new KeyValuePair<string, int>(ep[1], ms));
        else Log("stats.ini line " + (i + 1) + ": @every wants <hardware> <ms>");
        continue;
      }

      if (raw.StartsWith("@source", StringComparison.OrdinalIgnoreCase)) {
        string[] sp = raw.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        int ms;
        if (sp.Length >= 4 && int.TryParse(sp[3], out ms) && ms > 0) {
          var src = new HttpSource();
          src.Name = sp[1].Trim();
          src.Url = sp[2].Trim();
          src.IntervalMs = ms;
          sources.Add(src);
        } else {
          Log("stats.ini line " + (i + 1) + ": @source wants <name> <url> <ms>");
        }
        continue;
      }

      if (raw.StartsWith("@claude", StringComparison.OrdinalIgnoreCase)) {
        string rest = raw.Substring(7).Trim();
        int lastSpace = rest.LastIndexOfAny(new char[] { ' ', '\t' });
        int ms;
        if (lastSpace > 0 && int.TryParse(rest.Substring(lastSpace + 1).Trim(), out ms) && ms > 0) {
          string dir = rest.Substring(0, lastSpace).Trim();
          claudeSource = new ClaudeSource(dir, ms);
        } else {
          Log("stats.ini line " + (i + 1) + ": @claude wants <claude_dir> <live_ms>");
        }
        continue;
      }

      int eqIdx = raw.IndexOf('=');
      if (eqIdx < 0) {
        Log("stats.ini line " + (i + 1) + ": missing '=' (" + raw + ")");
        continue;
      }
      string key = raw.Substring(0, eqIdx).Trim();
      string valPart = raw.Substring(eqIdx + 1).Trim();

      if (!Regex.IsMatch(key, "^[a-z0-9_.]{1,23}$", RegexOptions.IgnoreCase)) {
        Log("stats.ini line " + (i + 1) + ": invalid key '" + key + "' (must be [a-z0-9_.] max 23 chars)");
        continue;
      }

      if (string.Equals(valPart, "@uptime", StringComparison.OrdinalIgnoreCase)) {
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.Uptime;
        list.Add(r);
      } else if (string.Equals(valPart, "@cpu_mhz", StringComparison.OrdinalIgnoreCase)) {
        // Task Manager's "Speed": base clock x % Processor Performance. Free,
        // unlike LibreHardwareMonitor's per-core clocks.
        if (cpuBaseMhz == 0) {
          using (var q = new ManagementObjectSearcher("SELECT MaxClockSpeed FROM Win32_Processor"))
            foreach (ManagementObject o in q.Get()) { cpuBaseMhz = Convert.ToSingle(o["MaxClockSpeed"]); break; }
        }
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.CpuMhz;
        r.Counter = OpenCounter("Processor Information", "% Processor Performance", "_Total");
        r.Scale = cpuBaseMhz / 100;
        list.Add(r);
      } else if (valPart.StartsWith("@counter", StringComparison.OrdinalIgnoreCase)) {
        // @counter <category> | <counter> | <instance> [| scale]
        string[] cp = valPart.Substring(8).Split('|');
        float scale = 1;
        if (cp.Length < 3 || cp.Length > 4 ||
            (cp.Length == 4 && !float.TryParse(cp[3].Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out scale))) {
          Log("stats.ini line " + (i + 1) + ": @counter wants <category> | <counter> | <instance> [| scale]");
          continue;
        }
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.Counter;
        r.Counter = OpenCounter(cp[0].Trim(), cp[1].Trim(), cp[2].Trim());
        r.Scale = scale;
        list.Add(r);
      } else if (valPart.StartsWith("@drive", StringComparison.OrdinalIgnoreCase)) {
        string[] parts = valPart.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        if (parts.Length < 3) {
          Log("stats.ini line " + (i + 1) + ": invalid @drive syntax: " + valPart);
          continue;
        }
        string metric = parts[2].ToLowerInvariant();
        if (metric != "used" && metric != "free" && metric != "total" && metric != "load") {
          Log("stats.ini line " + (i + 1) + ": invalid drive metric '" + parts[2] + "'");
          continue;
        }
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.Drive;
        r.DriveLetter = parts[1];
        r.DriveMetric = metric;
        list.Add(r);
      } else {
        string[] parts = valPart.Split('|');
        if (parts.Length < 3 || parts.Length > 4) {
          Log("stats.ini line " + (i + 1) + ": invalid sensor rule (expected 3-4 '|' parts): " + valPart);
          continue;
        }
        string hwPart = parts[0].Trim();
        string typePart = parts[1].Trim();
        string namePart = parts[2].Trim();
        string reducePart = parts.Length == 4 ? parts[3].Trim().ToLowerInvariant() : "first";

        if (reducePart != "first" && reducePart != "avg" && reducePart != "sum" && reducePart != "max" && reducePart != "min") {
          Log("stats.ini line " + (i + 1) + ": invalid reduce '" + reducePart + "' (defaulting to first)");
          reducePart = "first";
        }

        string[] hwAlts = hwPart.Split(new char[] { ',' }, StringSplitOptions.RemoveEmptyEntries);
        for (int k = 0; k < hwAlts.Length; k++) hwAlts[k] = hwAlts[k].Trim();

        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.Sensor;
        r.HwAlternatives = hwAlts;
        r.SensorType = typePart;
        r.SensorName = namePart;
        r.Reduce = reducePart;
        list.Add(r);
      }
    }
    return list;
  }

  static void ResolveSensors(List<StatRule> rules, Computer computer) {
    if (rules == null || computer == null) return;
    List<ISensor> allSensors = Sensors(computer);
    for (int rIdx = 0; rIdx < rules.Count; rIdx++) {
      StatRule r = rules[rIdx];
      if (r.Kind != StatKind.Sensor) continue;
      r.ResolvedSensors.Clear();
      for (int i = 0; i < allSensors.Count; i++) {
        ISensor s = allSensors[i];
        bool hwMatch = false;
        string hwType = s.Hardware.HardwareType.ToString();
        string hwName = s.Hardware.Name;
        for (int j = 0; j < r.HwAlternatives.Length; j++) {
          string alt = r.HwAlternatives[j];
          if (WildcardMatch(alt, hwType) || WildcardMatch(alt, hwName)) {
            hwMatch = true;
            break;
          }
        }
        if (!hwMatch) continue;

        if (!string.Equals(s.SensorType.ToString(), r.SensorType, StringComparison.OrdinalIgnoreCase)) continue;
        if (!WildcardMatch(r.SensorName, s.Name)) continue;

        r.ResolvedSensors.Add(s);
      }
    }
  }

  static List<IHardware> HotHardware(List<StatRule> rules) {
    var list = new List<IHardware>();
    for (int i = 0; i < rules.Count; i++)
      for (int k = 0; k < rules[i].ResolvedSensors.Count; k++) {
        IHardware hw = rules[i].ResolvedSensors[k].Hardware;
        if (!list.Contains(hw)) list.Add(hw);
      }
    return list;
  }

  // The CPU update alone takes ~320 ms (it samples clocks on every core, mostly
  // sleeping) and the NVIDIA one ~80 ms: run them side by side so a tick costs
  // the slowest one, not the sum.
  static void UpdateParallel(List<IHardware> hw) {
    using (var done = new CountdownEvent(hw.Count)) {
      for (int i = 0; i < hw.Count; i++) {
        IHardware h = hw[i];
        ThreadPool.QueueUserWorkItem(delegate(object o) {
          try { h.Update(); } catch (Exception) { } finally { done.Signal(); }
        });
      }
      done.Wait();
    }
  }

  static HashSet<IHardware> updating = new HashSet<IHardware>();

  static bool UpdateInBackground(IHardware hw) {
    lock (updating) { if (!updating.Add(hw)) return false; }
    ThreadPool.QueueUserWorkItem(delegate(object o) {
      try { hw.Update(); } catch (Exception) { } finally { lock (updating) updating.Remove(hw); }
    });
    return true;
  }

  static int EveryMs(IHardware hw) {
    for (int i = 0; i < everyRules.Count; i++)
      if (WildcardMatch(everyRules[i].Key, hw.HardwareType.ToString()) || WildcardMatch(everyRules[i].Key, hw.Name))
        return everyRules[i].Value;
    return 0;
  }

  static bool Unresolved(List<StatRule> rules) {
    for (int i = 0; i < rules.Count; i++)
      if (rules[i].Kind == StatKind.Sensor && rules[i].ResolvedSensors.Count == 0) return true;
    return false;
  }

  static float? EvalStat(StatRule r) {
    if (r.Kind == StatKind.Uptime) {
      return (float)(GetTickCount64() / 1000.0);
    }
    if (r.Kind == StatKind.Counter || r.Kind == StatKind.CpuMhz) {
      if (r.Counter == null) return null;
      try { return r.Counter.NextValue() * r.Scale; } catch (Exception) { return null; }
    }
    if (r.Kind == StatKind.Drive) {
      try {
        DriveInfo d = new DriveInfo(r.DriveLetter);
        if (d.IsReady) {
          double total = d.TotalSize;
          double free = d.TotalFreeSpace;
          double used = total - free;
          if (r.DriveMetric == "total") return (float)(total / (1024.0 * 1024.0 * 1024.0));
          if (r.DriveMetric == "free") return (float)(free / (1024.0 * 1024.0 * 1024.0));
          if (r.DriveMetric == "used") return (float)(used / (1024.0 * 1024.0 * 1024.0));
          if (r.DriveMetric == "load") return total > 0 ? (float)(used / total * 100.0) : 0f;
        }
      } catch (Exception) { }
      return null;
    }
    // Sensor evaluation
    var vals = new List<float>();
    for (int i = 0; i < r.ResolvedSensors.Count; i++) {
      ISensor s = r.ResolvedSensors[i];
      if (s.Value.HasValue && !float.IsNaN(s.Value.Value) && !float.IsInfinity(s.Value.Value)) {
        vals.Add(s.Value.Value);
        if (r.Reduce == "first") break;
      }
    }
    if (vals.Count == 0) return null;
    if (r.Reduce == "first") return vals[0];
    if (r.Reduce == "sum") {
      double sum = 0;
      for (int i = 0; i < vals.Count; i++) sum += vals[i];
      return (float)sum;
    }
    if (r.Reduce == "avg") {
      double sum = 0;
      for (int i = 0; i < vals.Count; i++) sum += vals[i];
      return (float)(sum / vals.Count);
    }
    if (r.Reduce == "max") {
      float max = vals[0];
      for (int i = 1; i < vals.Count; i++) if (vals[i] > max) max = vals[i];
      return max;
    }
    if (r.Reduce == "min") {
      float min = vals[0];
      for (int i = 1; i < vals.Count; i++) if (vals[i] < min) min = vals[i];
      return min;
    }
    return null;
  }

  void Run() {
    Log("start");
    Computer computer = null;
    ILink link = null;
    List<StatRule> rules = null;
    List<HttpSource> sources = null;
    ClaudeSource claudeSource = null;
    bool hardwareChanged = false;
    bool iniMissingLogged = false;
    DateTime lastIniWriteTime = DateTime.MinValue;
    DateTime nextScan = DateTime.MinValue, nextResolve = DateTime.MinValue;
    List<IHardware> hot = null;              // hardware the rules read, updated every tick
    var lastUpdate = new Dictionary<IHardware, DateTime>();
    uint seq = 0;
    string exeDir = AppDomain.CurrentDomain.BaseDirectory;
    string iniPath = Path.Combine(exeDir, "stats.ini");

    try {
      computer = OpenComputer();
      computer.HardwareAdded += delegate(IHardware h) { hardwareChanged = true; };
      computer.HardwareRemoved += delegate(IHardware h) { hardwareChanged = true; };
      var visitor = new UpdateVisitor();

      while (!stop) {
        DateTime t0 = DateTime.UtcNow;

        // Check stats.ini changes
        if (!File.Exists(iniPath)) {
          if (!iniMissingLogged) {
            Log("stats.ini not found in " + exeDir);
            iniMissingLogged = true;
          }
          StopSources(sources);
          sources = null;
          if (claudeSource != null) {
            claudeSource.Stop();
            claudeSource = null;
          }
          rules = null;
        } else {
          iniMissingLogged = false;
          DateTime writeTime = File.GetLastWriteTimeUtc(iniPath);
          if (rules == null || writeTime != lastIniWriteTime) {
            lastIniWriteTime = writeTime;
            StopSources(sources);
            sources = new List<HttpSource>();
            if (claudeSource != null) {
              claudeSource.Stop();
              claudeSource = null;
            }
            rules = LoadRules(iniPath, sources, ref claudeSource);
            StartSources(sources);
            if (claudeSource != null) claudeSource.Start();
            nextResolve = DateTime.MinValue;
            hot = null;
            Log("loaded " + rules.Count + " stats, " + sources.Count + " sources" + (claudeSource != null ? ", claude enabled" : "") + " from stats.ini");
          }
        }

        // Port reconnect loop
        if (link == null && t0 >= nextScan) {
          if (fixedPort != null || transportMode == "serial") {
            string name = FindPort();
            if (name != null) {
              try {
                link = new SerialLink(name);
                Log("connected serial " + name);
              } catch (Exception e) {
                Log("open " + name + ": " + e.Message);
              }
            }
          } else if (transportMode == "hid") {
            SafeFileHandle hHid;
            string hidPath = FindHid(out hHid);
            if (hidPath != null && hHid != null) {
              try {
                link = new HidLink(hHid);
                Log("connected hid " + hidPath);
              } catch (Exception e) {
                Log("open hid " + hidPath + ": " + e.Message);
                if (hHid != null && !hHid.IsClosed) hHid.Close();
              }
            }
          } else { // auto: HID first, serial fallback
            SafeFileHandle hHid;
            string hidPath = FindHid(out hHid);
            if (hidPath != null && hHid != null) {
              try {
                link = new HidLink(hHid);
                Log("connected hid " + hidPath);
              } catch (Exception e) {
                Log("open hid " + hidPath + ": " + e.Message);
                if (hHid != null && !hHid.IsClosed) hHid.Close();
              }
            }
            if (link == null) {
              string name = FindPort();
              if (name != null) {
                try {
                  link = new SerialLink(name);
                  Log("connected serial " + name);
                } catch (Exception e) {
                  Log("open " + name + ": " + e.Message);
                }
              }
            }
          }
          nextScan = t0.AddSeconds(3);
        }

        DateTime u0 = DateTime.UtcNow;
        // Resolve after a FULL update: SuperIO fans and some GPU sensors only
        // exist once a reading was seen. A full update costs ~0.5 s (SMART on
        // every drive, every NIC), so it only runs on (re)load, hardware
        // change, or every 30 s while some rule still matches nothing.
        if (rules != null && (hardwareChanged || (t0 >= nextResolve && (hot == null || Unresolved(rules))))) {
          hardwareChanged = false;
          for (;;) { lock (updating) { if (updating.Count == 0) break; } Thread.Sleep(10); }   // no overlap with a background update
          computer.Accept(visitor);
          ResolveSensors(rules, computer);
          hot = HotHardware(rules);
          nextResolve = t0.AddSeconds(30);
        } else if (hot != null) {
          // Only what the rules read. @every hardware (the slow CPU update)
          // refreshes in the background so no tick waits for it; the tick
          // sends its last values.
          var due = new List<IHardware>();
          for (int i = 0; i < hot.Count; i++) {
            int every = EveryMs(hot[i]);
            if (every == 0) { due.Add(hot[i]); continue; }
            DateTime last;
            if (lastUpdate.TryGetValue(hot[i], out last) && (t0 - last).TotalMilliseconds < every - intervalMs / 2) continue;
            if (!UpdateInBackground(hot[i])) continue;   // previous one still running
            lastUpdate[hot[i]] = t0;
          }
          if (due.Count > 0) UpdateParallel(due);
        }
        int updateMs = (int)(DateTime.UtcNow - u0).TotalMilliseconds;
        if (updateMs > intervalMs * 8 / 10 && seq % 60 == 0)
          Log("hardware update takes " + updateMs + " ms of a " + intervalMs + " ms interval");

        // Evaluate stats
        var tokens = new List<string>();
        if (rules != null) {
          for (int i = 0; i < rules.Count; i++) {
            StatRule r = rules[i];
            float? val = EvalStat(r);
            tokens.Add(r.Key + "=" + Fmt(val));
          }
        }

        // Evaluate sources
        var texts = new List<KeyValuePair<string, string>>();
        if (sources != null) {
          for (int i = 0; i < sources.Count; i++) {
            HttpSource src = sources[i];
            bool ok = false;
            Dictionary<string, float> numCopy = null;
            Dictionary<string, string> textCopy = null;

            lock (src.Lock) {
              if (src.LastSuccessUtc != DateTime.MinValue &&
                  (DateTime.UtcNow - src.LastSuccessUtc).TotalMilliseconds <= (3 * src.IntervalMs)) {
                ok = true;
                numCopy = new Dictionary<string, float>(src.Numbers);
                textCopy = new Dictionary<string, string>(src.Texts);
              }
            }

            tokens.Add(src.Name + ".ok=" + (ok ? "1" : "0"));
            if (ok) {
              foreach (KeyValuePair<string, float> kv in numCopy) {
                tokens.Add(kv.Key + "=" + Fmt(kv.Value));
              }
              foreach (KeyValuePair<string, string> kv in textCopy) {
                texts.Add(kv);
              }
            }
          }
        }

        // Evaluate Claude source
        if (claudeSource != null) {
          claudeSource.GetSnapshot(tokens, texts);
        }

        // Console mode: print full @s line and @t lines each tick
        if (consoleMode) {
          string fullLine = "@s" + (tokens.Count > 0 ? " " + string.Join(" ", tokens.ToArray()) : "");
          Console.WriteLine("[" + updateMs + " ms] " + fullLine);
          for (int i = 0; i < texts.Count; i++) {
            Console.WriteLine("  @t " + texts[i].Key + " " + texts[i].Value);
          }
        }

        // Send to link if connected
        if (link != null) {
          try {
            var sb = new StringBuilder();
            if (tokens.Count > 0) {
              var current = new StringBuilder("@s");
              for (int i = 0; i < tokens.Count; i++) {
                string tok = tokens[i];
                if (current.Length + 1 + tok.Length > MaxLineLen) {
                  sb.Append(current.ToString()).Append('\n');
                  current = new StringBuilder("@s");
                }
                current.Append(' ').Append(tok);
              }
              if (current.Length > 2) {
                sb.Append(current.ToString()).Append('\n');
              }
            }
            for (int i = 0; i < texts.Count; i++) {
              sb.Append("@t ").Append(texts[i].Key).Append(' ').Append(texts[i].Value).Append('\n');
            }
            sb.Append("@e ").Append(seq++).Append('\n');
            link.Write(sb.ToString());
          } catch (Exception e) {
            Log("port lost: " + e.Message);
            try { link.Close(); } catch (Exception) { }
            link = null;
          }
        }

        int wait = intervalMs - (int)(DateTime.UtcNow - t0).TotalMilliseconds;
        if (wait > 0) Thread.Sleep(wait);
      }
    } catch (Exception e) {
      Log("fatal: " + e);
      throw;
    } finally {
      StopSources(sources);
      if (claudeSource != null) {
        claudeSource.Stop();
        claudeSource = null;
      }
      if (link != null) try { link.Close(); } catch (Exception) { }
      if (computer != null) computer.Close();
      Log("stop");
    }
  }
}
