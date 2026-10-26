// AgyUsage: reads the Gemini quota (weekly and 5-hour) the way a person does,
// by running `agy`, typing /usage and reading the screen. Google's quota APIs
// reachable with agy's login only report the Claude/GPT group and one Flash
// value, while agy's own /usage panel shows both Gemini windows.
//
// agy runs in a pseudo console (ConPTY) in its own empty workspace folder;
// the /usage and /exit lines it appends to its prompt history for that
// folder are removed again afterwards.
//
// Target: .NET 10 (ESP32-Helper.csproj).
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.Win32.SafeHandles;

static class AgyUsage {
  public class Window {
    public double Pct = double.NaN;   // remaining %
    public DateTime? ResetUtc;
  }

  [StructLayout(LayoutKind.Sequential)]
  struct COORD { public short X, Y; }

  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct STARTUPINFO {
    public int cb;
    public string lpReserved, lpDesktop, lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2;
    public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }

  [StructLayout(LayoutKind.Sequential)]
  struct STARTUPINFOEX {
    public STARTUPINFO StartupInfo;
    public IntPtr lpAttributeList;
  }

  [StructLayout(LayoutKind.Sequential)]
  struct PROCESS_INFORMATION {
    public IntPtr hProcess, hThread;
    public int dwProcessId, dwThreadId;
  }

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern int CreatePseudoConsole(COORD size, SafeFileHandle hInput, SafeFileHandle hOutput, uint dwFlags, out IntPtr phPC);

  [DllImport("kernel32.dll")]
  static extern void ClosePseudoConsole(IntPtr hPC);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool CreatePipe(out SafeFileHandle hReadPipe, out SafeFileHandle hWritePipe, IntPtr lpPipeAttributes, int nSize);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool InitializeProcThreadAttributeList(IntPtr lpAttributeList, int dwAttributeCount, int dwFlags, ref IntPtr lpSize);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool UpdateProcThreadAttribute(IntPtr lpAttributeList, uint dwFlags, IntPtr attribute, IntPtr lpValue,
      IntPtr cbSize, IntPtr lpPreviousValue, IntPtr lpReturnSize);

  [DllImport("kernel32.dll")]
  static extern void DeleteProcThreadAttributeList(IntPtr lpAttributeList);

  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool CreateProcessW(string lpApplicationName, StringBuilder lpCommandLine, IntPtr lpProcessAttributes,
      IntPtr lpThreadAttributes, bool bInheritHandles, uint dwCreationFlags, IntPtr lpEnvironment, string lpCurrentDirectory,
      ref STARTUPINFOEX lpStartupInfo, out PROCESS_INFORMATION lpProcessInformation);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool TerminateProcess(IntPtr hProcess, uint uExitCode);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool CloseHandle(IntPtr hObject);

  const uint EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
  const int STARTF_USESTDHANDLES = 0x00000100;
  static readonly IntPtr PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = (IntPtr)0x00020016;

  static readonly Regex Ansi = new Regex(@"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*\x07|\x1b[()][0-9A-B]|\x1b[=>]");

  public static string Workspace {
    get { return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"ESP32PcStats\agy-usage"); }
  }

  // Runs agy /usage; false when agy is missing, signed out or the panel never showed.
  public static bool Read(out Window week, out Window five) {
    week = new Window();
    five = new Window();
    string exe = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), @"agy\bin\agy.exe");
    if (!File.Exists(exe)) return false;
    Directory.CreateDirectory(Workspace);

    string text;
    try {
      text = RunUsage(exe);
    } finally {
      try { DropHistory(); } catch (Exception) { }
    }
    if (text == null) return false;

    int g = text.IndexOf("GEMINI MODELS", StringComparison.Ordinal);
    if (g < 0) return false;
    int end = text.IndexOf("MODELS", g + 13, StringComparison.Ordinal);
    string grp = end > 0 ? text.Substring(g, end - g) : text.Substring(g);
    ParseWindow(grp, "Weekly Limit Remaining", week);
    ParseWindow(grp, "Five Hour Limit Remaining", five);
    return !double.IsNaN(week.Pct) || !double.IsNaN(five.Pct);
  }

  // "Weekly Limit Remaining 91.15% Refreshes in 146h 35m" or "... 100.00% Quota available"
  static void ParseWindow(string grp, string label, Window w) {
    Match m = Regex.Match(grp, Regex.Escape(label) + @"\s+([0-9.]+)%\s+(?:Refreshes in\s+(?:(\d+)d\s*)?(?:(\d+)h\s*)?(?:(\d+)m)?)?");
    if (!m.Success) return;
    w.Pct = double.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture);
    if (m.Groups[2].Success || m.Groups[3].Success || m.Groups[4].Success) {
      double mins = (m.Groups[2].Success ? int.Parse(m.Groups[2].Value) * 1440 : 0) +
                    (m.Groups[3].Success ? int.Parse(m.Groups[3].Value) * 60 : 0) +
                    (m.Groups[4].Success ? int.Parse(m.Groups[4].Value) : 0);
      w.ResetUtc = DateTime.UtcNow.AddMinutes(mins);
    }
  }

  // Screen text after /usage with escape codes, bars and box lines flattened to spaces.
  static string RunUsage(string exe) {
    SafeFileHandle inRead, inWrite, outRead, outWrite;
    if (!CreatePipe(out inRead, out inWrite, IntPtr.Zero, 0)) return null;
    if (!CreatePipe(out outRead, out outWrite, IntPtr.Zero, 0)) return null;
    IntPtr hpc;
    if (CreatePseudoConsole(new COORD { X = 120, Y = 60 }, inRead, outWrite, 0, out hpc) != 0) return null;
    inRead.Dispose();
    outWrite.Dispose();

    IntPtr attrSize = IntPtr.Zero;
    InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref attrSize);
    IntPtr attrs = Marshal.AllocHGlobal(attrSize);
    PROCESS_INFORMATION pi = new PROCESS_INFORMATION();
    bool started = false;
    StringBuilder seen = new StringBuilder();
    object seenLock = new object();
    FileStream outStream = new FileStream(outRead, FileAccess.Read);
    try {
      if (!InitializeProcThreadAttributeList(attrs, 1, 0, ref attrSize)) return null;
      if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc, (IntPtr)IntPtr.Size, IntPtr.Zero, IntPtr.Zero)) return null;
      STARTUPINFOEX si = new STARTUPINFOEX();
      si.StartupInfo.cb = Marshal.SizeOf(typeof(STARTUPINFOEX));
      si.lpAttributeList = attrs;
      si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;   // null std handles: never the helper's own console
      started = CreateProcessW(null, new StringBuilder("\"" + exe + "\""), IntPtr.Zero, IntPtr.Zero, false,
          EXTENDED_STARTUPINFO_PRESENT, IntPtr.Zero, Workspace, ref si, out pi);
      if (!started) return null;

      Thread reader = new Thread(() => {
        byte[] buf = new byte[16384];
        Decoder dec = Encoding.UTF8.GetDecoder();
        char[] chars = new char[16384];
        try {
          int n;
          while ((n = outStream.Read(buf, 0, buf.Length)) > 0) {
            int c = dec.GetChars(buf, 0, n, chars, 0);
            lock (seenLock) seen.Append(chars, 0, c);
          }
        } catch (Exception) {
          // pipe closed with the pseudo console
        }
      }) { IsBackground = true };
      reader.Start();

      using (FileStream input = new FileStream(inWrite, FileAccess.Write)) {
        // banner first, then a quiet moment so the prompt takes keys
        if (!WaitFor(seen, seenLock, 0, "Antigravity CLI", 20000)) return null;
        WaitQuiet(seen, seenLock, 700, 5000);
        int mark;
        lock (seenLock) mark = seen.Length;
        Send(input, "/usage\r");
        bool ok = WaitFor(seen, seenLock, mark, "CLAUDE AND GPT MODELS", 15000) || WaitFor(seen, seenLock, mark, "Five Hour", 1000);
        Thread.Sleep(300);
        string raw;
        lock (seenLock) raw = seen.ToString(mark, seen.Length - mark);

        Send(input, "\x1b");
        Thread.Sleep(300);
        Send(input, "/exit\r");
        if (WaitForSingleObject(pi.hProcess, 5000) != 0) TerminateProcess(pi.hProcess, 1);
        if (!ok) return null;

        string t = Ansi.Replace(raw, " ");
        t = Regex.Replace(t, "[▀-▟─-╿\\[\\]]", " ");
        return Regex.Replace(t, @"\s+", " ");
      }
    } finally {
      if (started) {
        if (WaitForSingleObject(pi.hProcess, 0) != 0) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
      }
      ClosePseudoConsole(hpc);
      outStream.Dispose();
      DeleteProcThreadAttributeList(attrs);
      Marshal.FreeHGlobal(attrs);
    }
  }

  static void Send(FileStream input, string s) {
    byte[] b = Encoding.UTF8.GetBytes(s);
    input.Write(b, 0, b.Length);
    input.Flush();
  }

  static bool WaitFor(StringBuilder seen, object seenLock, int from, string marker, int timeoutMs) {
    DateTime until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
    while (DateTime.UtcNow < until) {
      lock (seenLock) {
        if (seen.Length > from && Ansi.Replace(seen.ToString(from, seen.Length - from), "").IndexOf(marker, StringComparison.Ordinal) >= 0) return true;
      }
      Thread.Sleep(100);
    }
    return false;
  }

  static void WaitQuiet(StringBuilder seen, object seenLock, int quietMs, int timeoutMs) {
    DateTime until = DateTime.UtcNow.AddMilliseconds(timeoutMs);
    int last = -1;
    DateTime lastChange = DateTime.UtcNow;
    while (DateTime.UtcNow < until) {
      int len;
      lock (seenLock) len = seen.Length;
      if (len != last) { last = len; lastChange = DateTime.UtcNow; }
      else if ((DateTime.UtcNow - lastChange).TotalMilliseconds >= quietMs) return;
      Thread.Sleep(50);
    }
  }

  // agy keeps typed commands in history.jsonl with the workspace they came
  // from; only lines from our own workspace folder are removed.
  static void DropHistory() {
    string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), @".gemini\antigravity-cli\history.jsonl");
    if (!File.Exists(path)) return;
    string ws = "\"workspace\":" + Json.Write(Workspace);
    string[] lines = File.ReadAllLines(path, new UTF8Encoding(false));
    List<string> keep = new List<string>(lines.Length);
    foreach (string l in lines) if (l.IndexOf(ws, StringComparison.Ordinal) < 0) keep.Add(l);
    if (keep.Count == lines.Length) return;
    string tmp = path + ".esp32tmp";
    File.WriteAllLines(tmp, keep.ToArray(), new UTF8Encoding(false));
    File.Replace(tmp, path, null);
  }
}
