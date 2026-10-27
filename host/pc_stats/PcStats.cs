// ESP32-Helper: Windows service that collects configured PC stats (hardware
// sensors from PcHwCtl's sensor feed, Feed.cs) and streams
// them to the carrier board over its WinUSB bulk interface, else a vendor
// HID interface (report ID 6), else native USB CDC (Espressif VID 303A).
//
// The same exe is also the user-session half (AgyQuota.cs): the service starts
// it with --user in the signed-in user's session and talks to it over two
// anonymous pipes (see "User-session half" below).
//
// Sensor mapping is defined in stats.ini (in the executable's directory):
//   key = @feed [<feed key>]             PcHwCtl sensor feed (Feed.cs), default the same key
//   key = @ram load|used|free            physical memory (% / GB)
//   key = @net down|up <adapter,...>     bytes/s summed over adapters (wildcards)
//   key = @uptime
//   key = @drive <letter>: used|free|total|load
//   key = @cpu_mhz                       Task Manager's CPU speed
//   key = @counter <category> | <counter> | <instance> [| scale]   any Windows perf counter
//   @source <name> <url> <ms>            poll HTTP key=value stream on background thread
//
// Wire format, ASCII lines (firmware side: firmware/carrier_main/pcstats.h):
//   @s key=value key=value ...   (split into lines <= 300 chars)
//   @t key text                  (text stats, UTF-8, up to 120 bytes)
//   @e <seq>                     end of one sample (every @interval ms, default 500)
//
// Usage: ESP32-Helper.exe             run as service (installed by install.ps1)
//        ESP32-Helper.exe --console   run in console, print stats each tick; lines
//                                     typed there starting with ! act as board lines
//        ESP32-Helper.exe --list      print one PcHwCtl sensor feed snapshot and exit
//        ESP32-Helper.exe --agy-once  poll Antigravity quota once, print it
//        ESP32-Helper.exe --media     sample media sessions for 5 s
//        --transport auto|winusb|hid|serial  transport mode (default: auto)
//        --port COMn                  skip VID auto-detect, force serial COMn
//        --no-user                    with --console: don't start the user half
using System;
using System.Collections.Generic;
using System.Diagnostics.Eventing.Reader;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.IO.Pipes;
using System.IO.Ports;
using System.Management;
using System.Net;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;
using System.ServiceProcess;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using Microsoft.Win32;
using Microsoft.Win32.SafeHandles;

partial class PcStats : ServiceBase {
  static readonly Encoding Utf8NoBom = new UTF8Encoding(false);

  static string TruncateUtf8(string s, int maxBytes) {
    if (string.IsNullOrEmpty(s) || maxBytes <= 0) return "";
    int byteCount = 0;
    int charIndex = 0;
    while (charIndex < s.Length) {
      int charStep = (char.IsHighSurrogate(s[charIndex]) && charIndex + 1 < s.Length && char.IsLowSurrogate(s[charIndex + 1])) ? 2 : 1;
      int bytesForChar = Utf8NoBom.GetByteCount(s.Substring(charIndex, charStep));
      if (byteCount + bytesForChar > maxBytes) break;
      byteCount += bytesForChar;
      charIndex += charStep;
    }
    return s.Substring(0, charIndex);
  }

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

  // WinUSB: the board's vendor bulk interface, bound by its MS OS 2.0
  // descriptors and published under this GUID (firmware/main/usb.cpp)
  static readonly Guid WinUsbInterfaceGuid = new Guid("39531AD1-55E5-4933-869D-123B4E7D172C");
  const byte WinUsbPipeOut = 0x04;
  const byte WinUsbPipeIn = 0x84;
  const uint PIPE_TRANSFER_TIMEOUT = 0x03;
  const uint FILE_ATTRIBUTE_NORMAL = 0x80;

  [DllImport("winusb.dll", SetLastError = true)]
  static extern bool WinUsb_Initialize(SafeFileHandle deviceHandle, out IntPtr interfaceHandle);

  [DllImport("winusb.dll", SetLastError = true)]
  static extern bool WinUsb_Free(IntPtr interfaceHandle);

  [DllImport("winusb.dll", SetLastError = true)]
  static extern bool WinUsb_WritePipe(IntPtr interfaceHandle, byte pipeId, byte[] buffer, uint length, out uint transferred, IntPtr overlapped);

  [DllImport("winusb.dll", SetLastError = true)]
  static extern bool WinUsb_ReadPipe(IntPtr interfaceHandle, byte pipeId, byte[] buffer, uint length, out uint transferred, IntPtr overlapped);

  [DllImport("winusb.dll", SetLastError = true)]
  static extern bool WinUsb_SetPipePolicy(IntPtr interfaceHandle, byte pipeId, uint policyType, uint valueLength, ref uint value);

  [DllImport("winusb.dll", SetLastError = true)]
  static extern bool WinUsb_FlushPipe(IntPtr interfaceHandle, byte pipeId);

  [DllImport("winusb.dll", SetLastError = true)]
  static extern bool WinUsb_AbortPipe(IntPtr interfaceHandle, byte pipeId);

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

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool ReadFile(SafeFileHandle hFile, IntPtr lpBuffer, uint nNumberOfBytesToRead, IntPtr lpNumberOfBytesRead, IntPtr lpOverlapped);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool CancelIoEx(SafeFileHandle hFile, IntPtr lpOverlapped);

  [DllImport("powrprof.dll", SetLastError = true)]
  static extern bool SetSuspendState(bool hibernate, bool forceCritical, bool disableWakeEvent);

  // lock: the service runs in session 0, LockWorkStation has to run in the user's
  [DllImport("kernel32.dll")]
  static extern uint WTSGetActiveConsoleSessionId();

  [DllImport("wtsapi32.dll", SetLastError = true)]
  static extern bool WTSQueryUserToken(uint sessionId, out IntPtr token);

  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct STARTUPINFO {
    public int cb;
    public string lpReserved, lpDesktop, lpTitle;
    public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    public short wShowWindow, cbReserved2;
    public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
  }

  [StructLayout(LayoutKind.Sequential)]
  struct PROCESS_INFORMATION {
    public IntPtr hProcess, hThread;
    public int dwProcessId, dwThreadId;
  }

  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool CreateProcessAsUser(IntPtr hToken, string app, string cmdLine, IntPtr procAttr, IntPtr threadAttr,
      bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

  [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool CreateProcess(string app, string cmdLine, IntPtr procAttr, IntPtr threadAttr,
      bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);

  [DllImport("userenv.dll", SetLastError = true)]
  static extern bool CreateEnvironmentBlock(out IntPtr env, IntPtr token, bool inherit);

  [DllImport("userenv.dll", SetLastError = true)]
  static extern bool DestroyEnvironmentBlock(IntPtr env);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool TerminateProcess(IntPtr process, uint exitCode);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

  [DllImport("powrprof.dll", SetLastError = true)]
  static extern bool GetPwrCapabilities(IntPtr lpSystemPowerCapabilities);

  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern uint InitiateShutdown(string lpMachineName, string lpMessage, uint dwGracePeriod, uint dwShutdownFlags, uint dwReason);

  [DllImport("kernel32.dll")]
  static extern IntPtr GetCurrentProcess();

  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool OpenProcessToken(IntPtr ProcessHandle, uint DesiredAccess, out IntPtr TokenHandle);

  [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool LookupPrivilegeValue(string lpSystemName, string lpName, out LUID lpLuid);

  [DllImport("advapi32.dll", SetLastError = true)]
  static extern bool AdjustTokenPrivileges(IntPtr TokenHandle, bool DisableAllPrivileges, ref TOKEN_PRIVILEGES NewState, uint BufferLength, IntPtr PreviousState, IntPtr ReturnLength);

  [StructLayout(LayoutKind.Sequential)]
  struct LUID {
    public uint LowPart;
    public int HighPart;
  }

  [StructLayout(LayoutKind.Sequential, Pack = 1)]
  struct TOKEN_PRIVILEGES {
    public uint PrivilegeCount;
    public LUID Luid;
    public uint Attributes;
  }

  interface ILink : IDisposable {
    void Write(string text);
    void Poll();
    void Close();
    bool IsFaulted { get; }
  }

  class SerialLink : ILink {
    SerialPort port;
    StringBuilder serialBuf = new StringBuilder();
    bool serialDropping;

    public bool IsFaulted {
      get { return port == null || !port.IsOpen; }
    }

    public SerialLink(string name) {
      port = OpenPort(name);
    }

    public void Poll() {
      if (port != null && port.IsOpen) {
        try {
          if (port.BytesToRead > 0) {
            string text = port.ReadExisting();
            FeedSerial(text);
          }
        } catch (Exception) { }
      }
    }

    void FeedSerial(string s) {
      if (s == null) return;
      for (int i = 0; i < s.Length; i++) {
        char c = s[i];
        if (c == 0) continue;
        if (c == '\n') {
          if (!serialDropping) {
            string line = serialBuf.ToString().TrimEnd('\r');
            if (line.Length > 0 && line.StartsWith("!")) {
              HandleBoardLine(line);
            }
          }
          serialDropping = false;
          serialBuf.Length = 0;
        } else {
          if (!serialDropping) {
            serialBuf.Append(c);
            if (serialBuf.Length > 256) {
              serialDropping = true;
              serialBuf.Length = 0;
            }
          }
        }
      }
    }

    public void Write(string text) {
      Poll();
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
    IntPtr hReadEvent;
    Thread readerThread;
    volatile bool readerStop;
    volatile bool isFaulted;

    // The kernel uses the report buffer and the OVERLAPPED after WriteFile
    // returns (I/O pending): both live in unmanaged memory so the GC can
    // never move them mid-write.
    IntPtr report = Marshal.AllocHGlobal(64);
    IntPtr ov = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));

    IntPtr readReport = Marshal.AllocHGlobal(64);
    IntPtr readOv = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(OVERLAPPED)));

    public bool IsFaulted {
      get { return isFaulted; }
    }

    public HidLink(SafeFileHandle h) {
      handle = h;
      hEvent = CreateEvent(IntPtr.Zero, true, false, null);
      hReadEvent = CreateEvent(IntPtr.Zero, true, false, null);
      readerThread = new Thread(ReadLoop) { IsBackground = true, Name = "hid_reader" };
      readerThread.Start();
    }

    void ReadLoop() {
      var lineBuf = new StringBuilder();
      bool dropping = false;
      byte[] rawReport = new byte[64];

      while (!readerStop) {
        ResetEvent(hReadEvent);
        OVERLAPPED o = new OVERLAPPED();
        o.hEvent = hReadEvent;
        Marshal.StructureToPtr(o, readOv, false);

        bool ok = ReadFile(handle, readReport, 64, IntPtr.Zero, readOv);
        if (!ok) {
          int err = Marshal.GetLastWin32Error();
          if (err == ERROR_IO_PENDING) {
            while (!readerStop) {
              uint waitRes = WaitForSingleObject(hReadEvent, 500);
              if (waitRes == WAIT_OBJECT_0) break;
            }
            if (readerStop) {
              try { CancelIo(handle); } catch (Exception) { }
              uint dummy;
              try { GetOverlappedResult(handle, readOv, out dummy, true); } catch (Exception) { }
              break;
            }
            uint transferred;
            if (!GetOverlappedResult(handle, readOv, out transferred, false)) {
              int getErr = Marshal.GetLastWin32Error();
              if (readerStop || getErr == 995 /* ERROR_OPERATION_ABORTED */) break;
              Log("HID ReadFile GetOverlappedResult failed: " + getErr);
              isFaulted = true;
              break;
            }
          } else {
            if (readerStop || err == 995 /* ERROR_OPERATION_ABORTED */) break;
            Log("HID ReadFile failed: " + err);
            isFaulted = true;
            break;
          }
        }

        Marshal.Copy(readReport, rawReport, 0, 64);
        if (rawReport[0] == 0x06) {
          for (int i = 1; i < 64; i++) {
            byte b = rawReport[i];
            if (b == 0) continue;
            char c = (char)b;
            if (c == '\n') {
              if (!dropping) {
                string line = lineBuf.ToString().TrimEnd('\r');
                if (line.Length > 0) {
                  HandleBoardLine(line);
                }
              }
              dropping = false;
              lineBuf.Length = 0;
            } else {
              if (!dropping) {
                lineBuf.Append(c);
                if (lineBuf.Length > 256) {
                  dropping = true;
                  lineBuf.Length = 0;
                }
              }
            }
          }
        }
      }
    }

    public void Poll() { }

    public void Write(string text) {
      if (isFaulted) throw new IOException("HID link is faulted");
      byte[] bytes = Utf8NoBom.GetBytes(text);
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
      readerStop = true;
      if (handle != null && !handle.IsClosed && !handle.IsInvalid) {
        try { CancelIoEx(handle, readOv); } catch (Exception) { }
      }
      if (readerThread != null) {
        try { readerThread.Join(1000); } catch (Exception) { }
        readerThread = null;
      }
      if (handle != null && !handle.IsClosed) {
        try { CancelIo(handle); } catch (Exception) { }
        try { handle.Close(); } catch (Exception) { }
      }
      if (hEvent != IntPtr.Zero) {
        try { CloseHandle(hEvent); } catch (Exception) { }
        hEvent = IntPtr.Zero;
      }
      if (hReadEvent != IntPtr.Zero) {
        try { CloseHandle(hReadEvent); } catch (Exception) { }
        hReadEvent = IntPtr.Zero;
      }
      if (report != IntPtr.Zero) { Marshal.FreeHGlobal(report); report = IntPtr.Zero; }
      if (ov != IntPtr.Zero) { Marshal.FreeHGlobal(ov); ov = IntPtr.Zero; }
      if (readReport != IntPtr.Zero) { Marshal.FreeHGlobal(readReport); readReport = IntPtr.Zero; }
      if (readOv != IntPtr.Zero) { Marshal.FreeHGlobal(readOv); readOv = IntPtr.Zero; }
    }

    public void Dispose() {
      Close();
    }
  }

  // Bulk pipe: ~1 MB/s against HID's 63 bytes per 1 ms frame. Same text
  // lines as the other links. Synchronous WinUSB calls: the reader blocks in
  // ReadPipe until Close aborts it.
  class WinUsbLink : ILink {
    SafeFileHandle device;
    IntPtr usb;
    Thread readerThread;
    volatile bool readerStop;
    volatile bool isFaulted;
    readonly DateTime openedUtc = DateTime.UtcNow;

    public bool IsFaulted {
      get { return isFaulted; }
    }

    public WinUsbLink(SafeFileHandle h) {
      device = h;
      if (!WinUsb_Initialize(device, out usb)) {
        throw new IOException("WinUsb_Initialize failed: " + Marshal.GetLastWin32Error());
      }
      // the board makes the PC wait while its buffers are full; 3 s means it hung
      uint timeout = 3000;
      WinUsb_SetPipePolicy(usb, WinUsbPipeOut, PIPE_TRANSFER_TIMEOUT, 4, ref timeout);
      WinUsb_FlushPipe(usb, WinUsbPipeIn);
      readerThread = new Thread(ReadLoop) { IsBackground = true, Name = "winusb_reader" };
      readerThread.Start();
    }

    void ReadLoop() {
      var lineBuf = new StringBuilder();
      bool dropping = false;
      byte[] buf = new byte[512];
      while (!readerStop) {
        uint n;
        if (!WinUsb_ReadPipe(usb, WinUsbPipeIn, buf, (uint)buf.Length, out n, IntPtr.Zero)) {
          int err = Marshal.GetLastWin32Error();
          if (!readerStop && err != 995 /* ERROR_OPERATION_ABORTED */) {
            Log("WinUSB read failed: " + err);
            isFaulted = true;
          }
          break;
        }
        // a line still in the board's FIFO from before this link opened may
        // be an old request (a power action): drop the first 500 ms
        bool stale = (DateTime.UtcNow - openedUtc).TotalMilliseconds < 500;
        for (int i = 0; i < n; i++) {
          char c = (char)buf[i];
          if (c == '\0') continue;
          if (c == '\n') {
            if (!dropping && !stale) {
              string line = lineBuf.ToString().TrimEnd('\r');
              if (line.Length > 0) HandleBoardLine(line);
            }
            dropping = false;
            lineBuf.Length = 0;
          } else if (!dropping) {
            lineBuf.Append(c);
            if (lineBuf.Length > 256) {
              dropping = true;
              lineBuf.Length = 0;
            }
          }
        }
      }
    }

    public void Poll() { }

    public void Write(string text) {
      if (isFaulted) throw new IOException("WinUSB link is faulted");
      byte[] bytes = Utf8NoBom.GetBytes(text);
      uint sent;
      if (!WinUsb_WritePipe(usb, WinUsbPipeOut, bytes, (uint)bytes.Length, out sent, IntPtr.Zero) || sent != bytes.Length) {
        int err = Marshal.GetLastWin32Error();
        isFaulted = true;
        throw new IOException("WinUSB write failed: " + err + " (" + sent + " of " + bytes.Length + " bytes)");
      }
    }

    public void Close() {
      readerStop = true;
      if (usb != IntPtr.Zero) {
        try { WinUsb_AbortPipe(usb, WinUsbPipeIn); } catch (Exception) { }
      }
      if (readerThread != null) {
        try { readerThread.Join(1000); } catch (Exception) { }
        readerThread = null;
      }
      if (usb != IntPtr.Zero) {
        try { WinUsb_Free(usb); } catch (Exception) { }
        usb = IntPtr.Zero;
      }
      if (device != null && !device.IsClosed) {
        try { device.Close(); } catch (Exception) { }
      }
    }

    public void Dispose() {
      Close();
    }
  }

  // Power requests and Wake-on-LAN target
  static readonly object powerLock = new object();
  static readonly Regex PowerCmdRegex = new Regex(@"^!power\s+([a-zA-Z]+)\s+([0-9a-fA-F]{1,8})$");
  static readonly Dictionary<string, DateTime> recentNonces = new Dictionary<string, DateTime>(StringComparer.OrdinalIgnoreCase);
  static string currentAckValue;
  static DateTime ackExpiresUtc = DateTime.MinValue;
  static string scheduledAction;
  static DateTime scheduledActionTimeUtc = DateTime.MinValue;

  static string currentMac = "";
  static string currentMacIf = "";
  static DateTime nextMacRefreshUtc = DateTime.MinValue;

  static void HandleBoardLine(string line) {
    if (string.IsNullOrEmpty(line)) return;
    line = line.Trim();
    if (line.StartsWith("!art", StringComparison.OrdinalIgnoreCase)) {
      HandleArtLine(line);
    } else if (line.StartsWith("!txt", StringComparison.OrdinalIgnoreCase)) {
      HandleTxtLine(line);
    } else if (line.StartsWith("!power", StringComparison.OrdinalIgnoreCase)) {
      HandlePowerLine(line);
    } else if (line.StartsWith("!shot", StringComparison.OrdinalIgnoreCase)) {
      HandleShotLine(line);
    } else if (line.StartsWith("!hw", StringComparison.OrdinalIgnoreCase)) {
      HandleHwLine(line);
    } else if (line.StartsWith("!mon", StringComparison.OrdinalIgnoreCase)) {
      HandleMonLine(line);
    } else if (line.StartsWith("!ai", StringComparison.OrdinalIgnoreCase)) {
      HandleAiLine(line);
    } else if (line.StartsWith("!night", StringComparison.OrdinalIgnoreCase)) {
      HandleNightLine(line);
    } else {
      Log("board: unrecognized line '" + line + "'");
    }
  }

  static readonly Regex ShotCmdRegex = new Regex(@"^!shot\s+([0-9a-fA-F]{1,16})\s+(\S+)$");

  // The board asked for a screenshot and handed us the webhook from its
  // settings.h. The service runs as SYSTEM in session 0 and cannot see the
  // user's desktop, so we pass the request (nonce + webhook) to the user half
  // over its pipe; it captures, posts to the webhook, and auto-deletes after
  // 5 min. If nobody is signed in there is nothing to capture, so just log and
  // move on (never throw).
  static void HandleShotLine(string line) {
    if (line == null) return;
    Match m = ShotCmdRegex.Match(line.Trim());
    if (!m.Success) { Log("shot: unrecognized command"); return; }
    string nonce = m.Groups[1].Value.ToLowerInvariant();
    string webhook = m.Groups[2].Value;

    if (SendToUserHalf("S " + nonce + " " + webhook)) {
      Log("shot: request from board handed to the user half, nonce " + nonce);
    } else {
      Log("shot: user half not running (nobody signed in?), screenshot skipped");
    }
  }

  // ---- PC hardware control (PcHwCtl) ----
  // The board's `!hw <id> <command>` goes to PcHwCtl's named pipe (the fan and
  // RGB service in E:\PC Hardware Controller, docs/ipc.md). Only rgb and fan
  // commands and ping pass. The answer goes back as the text stat
  // hw.reply = "<id> ok" or "<id> err <message>", repeated each tick for 30 s.
  static readonly Regex HwCmdRegex = new Regex(@"^!hw\s+([0-9a-fA-F]{1,8})\s+(.+)$");
  static readonly object hwLock = new object();
  static string hwReply;
  static DateTime hwReplyExpiresUtc;
  static int hwBusy;   // one pipe request at a time (Interlocked)

  static void HandleHwLine(string line) {
    Match m = HwCmdRegex.Match(line.Trim());
    if (!m.Success) { Log("hw: unrecognized command"); return; }
    string id = m.Groups[1].Value.ToLowerInvariant();
    string command = m.Groups[2].Value.Trim();
    string lower = command.ToLowerInvariant();
    // fan set/curve/release/zerorpm only: PcHwCtl keeps each fan's floorPct and
    // caps a fan override at its ipc.maxTtlS, so no speed from here sticks
    // forever (zerorpm is a setting PcHwCtl keeps, like the GPU driver's own)
    bool fan = lower.StartsWith("fan set ") || lower.StartsWith("fan curve ") || lower.StartsWith("fan release ") ||
               lower.StartsWith("fan zerorpm ");
    if (lower != "ping" && !lower.StartsWith("rgb ") && !fan) {
      SetHwReply(id, "err only rgb and fan commands are allowed");
      Log("hw: #" + id + " refused (not an rgb or fan command)");
      return;
    }
    if (Interlocked.CompareExchange(ref hwBusy, 1, 0) != 0) {
      SetHwReply(id, "err busy");
      return;
    }
    ThreadPool.QueueUserWorkItem(delegate(object o) {
      try {
        string reply = SendToPcHwCtl(command);
        SetHwReply(id, reply);
        Log("hw: #" + id + " " + command + " -> " + reply);
      } finally {
        Interlocked.Exchange(ref hwBusy, 0);
      }
    });
  }

  static void SetHwReply(string id, string reply) {
    lock (hwLock) {
      hwReply = id + " " + reply;
      hwReplyExpiresUtc = DateTime.UtcNow.AddSeconds(30);
    }
    sendNow.Set();   // answer on the next tick, not the one after
  }

  static string SendToPcHwCtl(string command) {
    Dictionary<string, object> data;
    return PipeRequest(HostSettings.HWCTL_PIPE, command, HostSettings.HWCTL_TIMEOUT_MS, out data);
  }

  // PcHwCtl's state file (@source hw) carries what the helper used to read
  // itself, under hw.: the monitors (hw.mon.*) and the power requests
  // (hw.disp_req, hw.sys_req, hw.exec_req, each with _by). They go to the
  // board as mon.* and pc.*_req*, which its Discord panel and night mode read.
  static string BoardKey(string source, string key) {
    if (source != "hw") return key;
    if (key.StartsWith("hw.mon.", StringComparison.Ordinal)) return key.Substring(3);
    if (Regex.IsMatch(key, @"^hw\.(disp|sys|exec)_req")) return "pc" + key.Substring(2);
    return key;
  }

  // One request, one JSON line back: {"ok":true,"data":...} or {"ok":false,"error":"..."}.
  // Returns "ok" (data set) or "err <why>".
  static string PipeRequest(string pipeName, string command, int readTimeoutMs, out Dictionary<string, object> data) {
    data = null;
    try {
      using (var c = new PipeClient(pipeName, readTimeoutMs)) return c.Request(command, out data);
    } catch (System.TimeoutException) {
      return "err " + pipeName + " not running";
    } catch (Exception ex) {
      return "err " + ex.GetType().Name;
    }
  }

  // Several requests on one connection (night mode sends hundreds). The
  // constructor throws TimeoutException when nobody serves the pipe.
  class PipeClient : IDisposable {
    readonly string name;
    readonly int timeoutMs;
    readonly NamedPipeClientStream pipe;
    readonly StreamWriter w;
    readonly StreamReader r;
    bool broken;   // an answer timed out: the next line could be its late answer

    public PipeClient(string pipeName, int readTimeoutMs) {
      name = pipeName;
      timeoutMs = readTimeoutMs;
      pipe = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
      try {
        pipe.Connect(1000);
      } catch (Exception) {
        pipe.Dispose();
        throw;
      }
      w = new StreamWriter(pipe, Utf8NoBom) { AutoFlush = true, NewLine = "\n" };
      r = new StreamReader(pipe, Utf8NoBom);
    }

    public string Request(string command, out Dictionary<string, object> data) {
      data = null;
      if (broken) return "err " + name + " did not answer";
      try {
        w.WriteLine(command);
        var read = r.ReadLineAsync();
        if (!read.Wait(timeoutMs)) { broken = true; return "err " + name + " did not answer"; }
        string json = read.Result;
        if (json == null) { broken = true; return "err " + name + " closed the pipe"; }
        Dictionary<string, object> o = Json.ParseObject(json);
        object ok, err, d;
        if (o != null && o.TryGetValue("ok", out ok) && ok is bool && (bool)ok) {
          if (o.TryGetValue("data", out d)) data = d as Dictionary<string, object>;
          return "ok";
        }
        if (o != null && o.TryGetValue("error", out err) && err != null) return "err " + Convert.ToString(err, CultureInfo.InvariantCulture);
        return "err unexpected answer";
      } catch (Exception ex) {
        broken = true;
        return "err " + ex.GetType().Name;
      }
    }

    public void Dispose() { pipe.Dispose(); }
  }

  // ---- Local AI stack control (LocalAIHost) ----
  // The board's `!ai <id> <command>` goes to the LocalAIHost service's pipe
  // (E:\Local LLM\Service, Project_Note.md): model load/unload/ctx/idle,
  // restarts of the stack's children. The answer comes back as the text stat
  // aictl.reply = "<id> ok[ <info>]" or "<id> err <message>", 30 s like hw.reply
  // (ai.* belongs to the status hub's @source ai). `status` is squeezed into
  // key=value words for the Discord tab.
  static readonly Regex AiCmdRegex = new Regex(@"^!ai\s+([0-9a-fA-F]{1,8})\s+(.+)$");
  static readonly string[] AiAllowed = { "ping", "status", "model", "restart", "stop", "start", "gateway", "guard" };
  static readonly object aiLock = new object();
  static string aiReply;
  static DateTime aiReplyExpiresUtc;
  static readonly object aiPipeLock = new object();   // one LocalAIHost request at a time
  static int aiQueued;

  static void HandleAiLine(string line) {
    Match m = AiCmdRegex.Match(line.Trim());
    if (!m.Success) { Log("ai: unrecognized command"); return; }
    string id = m.Groups[1].Value.ToLowerInvariant();
    string command = m.Groups[2].Value.Trim();
    string verb = command.Split(' ')[0].ToLowerInvariant();
    if (Array.IndexOf(AiAllowed, verb) < 0) {
      SetAiReply(id, "err not allowed: " + verb);
      Log("ai: #" + id + " refused (" + verb + ")");
      return;
    }
    // queued, not refused: the Discord tab asks for status right after each
    // action, and the console or a second panel may ask at the same time
    if (Interlocked.Increment(ref aiQueued) > 4) {
      Interlocked.Decrement(ref aiQueued);
      SetAiReply(id, "err busy");
      return;
    }
    ThreadPool.QueueUserWorkItem(delegate(object o) {
      try {
        lock (aiPipeLock) {
          Dictionary<string, object> data;
          // 20 s: a forced unload can take ~8 s, status ~2 s with the model asleep
          string reply = PipeRequest(HostSettings.LOCALAI_PIPE, command, HostSettings.LOCALAI_TIMEOUT_MS, out data);
          if (reply == "ok") {
            if (verb == "status") reply = "ok " + AiStatusLine(data);
            else if (data != null && data.ContainsKey("started")) reply = "ok started";
          }
          SetAiReply(id, reply);
          if (verb != "status") Log("ai: #" + id + " " + command + " -> " + reply);
        }
      } finally {
        Interlocked.Decrement(ref aiQueued);
      }
    });
  }

  static void SetAiReply(string id, string reply) {
    lock (aiLock) {
      aiReply = TruncateUtf8(id + " " + reply, 120);
      aiReplyExpiresUtc = DateTime.UtcNow.AddSeconds(30);
    }
    sendNow.Set();
  }

  // "model=asleep ctx=- want=128k idle=20 keep=on busy=0 down=none last=load:ok"
  static string AiStatusLine(Dictionary<string, object> d) {
    if (d == null) return "";
    var sb = new StringBuilder();
    Dictionary<string, object> model = GetDict(d, "model");
    sb.Append("model=").Append(Word(model, "state"));
    sb.Append(" ctx=").Append(CtxWord(model, "ctx"));
    sb.Append(" want=").Append(CtxWord(model, "desired_ctx"));
    object idle = model != null && model.ContainsKey("idle_minutes") ? model["idle_minutes"] : null;
    sb.Append(" idle=").Append(idle == null ? "-" : Convert.ToDouble(idle, CultureInfo.InvariantCulture).ToString("0", CultureInfo.InvariantCulture));
    object keep = model != null && model.ContainsKey("keep_awake") ? model["keep_awake"] : null;
    sb.Append(" keep=").Append(keep is bool ? ((bool)keep ? "on" : "off") : "-");
    object busy = d.ContainsKey("busy") ? d["busy"] : null;
    sb.Append(" busy=").Append(busy is bool && (bool)busy ? "1" : "0");
    var down = new List<string>();
    Dictionary<string, object> children = GetDict(d, "children");
    if (children != null) {
      foreach (KeyValuePair<string, object> kv in children) {
        string st = Word(kv.Value as Dictionary<string, object>, "state");
        if (st != "running" && st != "external") down.Add(kv.Key + ":" + st);
      }
    }
    sb.Append(" down=").Append(down.Count == 0 ? "none" : string.Join(",", down));
    Dictionary<string, object> last = GetDict(d, "last");
    sb.Append(" last=").Append(last == null ? "-" : Word(last, "action").Replace(' ', '_') + ":" + Word(last, "result").Replace(' ', '_'));
    return sb.ToString();
  }

  static Dictionary<string, object> GetDict(Dictionary<string, object> d, string key) {
    object v;
    return d != null && d.TryGetValue(key, out v) ? v as Dictionary<string, object> : null;
  }

  static string Word(Dictionary<string, object> d, string key) {
    object v;
    if (d == null || !d.TryGetValue(key, out v) || v == null) return "-";
    string s = Convert.ToString(v, CultureInfo.InvariantCulture);
    return s.Length > 24 ? s.Substring(0, 24) : s;
  }

  static string CtxWord(Dictionary<string, object> d, string key) {
    object v;
    if (d == null || !d.TryGetValue(key, out v) || v == null) return "-";
    double n = Convert.ToDouble(v, CultureInfo.InvariantCulture);
    return (n / 1024).ToString("0", CultureInfo.InvariantCulture) + "k";
  }

  // ---- PC Monitor Control (PcHwCtl displays) ----
  // The board's `!mon <id> <command>` (bright <n|all> <pct>, power <n|all>
  // on|off) becomes PcHwCtl's `display ...` commands (docs/ipc.md#displays);
  // its user-session agent talks DDC/CI. "power all off" also sends `display
  // sleep` (Windows' display off, for monitors without a power method), "power
  // all on" a `display wake` first. The answer comes back as the text stat
  // monctl.reply = "<id> ok" or "<id> err <message>", 30 s like hw.reply. The
  // monitors' state is PcHwCtl's hw.mon.* (state.txt), sent as mon.*.
  static readonly Regex MonCmdRegex = new Regex(@"^!mon\s+([0-9a-fA-F]{1,8})\s+(bright|power)\s+(all|\d{1,2})\s+(\S+)$", RegexOptions.IgnoreCase);
  const int DisplayTimeoutMs = 10000;   // DDC/CI is slow: one write per monitor, with a retry
  static readonly object monLock = new object();
  static readonly object monPipeLock = new object();   // one display command at a time
  static string monReply;
  static DateTime monReplyExpiresUtc;

  static void HandleMonLine(string line) {
    Match m = MonCmdRegex.Match(line.Trim());
    if (!m.Success) { Log("mon: unrecognized command"); return; }
    string id = m.Groups[1].Value.ToLowerInvariant();
    string verb = m.Groups[2].Value.ToLowerInvariant(), target = m.Groups[3].Value.ToLowerInvariant(), arg = m.Groups[4].Value.ToLowerInvariant();
    var commands = new List<string>();
    if (verb == "bright") {
      commands.Add("display bright " + target + " " + arg);
    } else if (arg == "off") {
      commands.Add("display power " + target + " off");
      if (target == "all") commands.Add("display sleep");
    } else if (arg == "on") {
      if (target == "all") commands.Add("display wake");
      commands.Add("display power " + target + " on");
    } else {
      SetMonReply(id, "err power is on or off");
      return;
    }
    ThreadPool.QueueUserWorkItem(delegate(object o) {
      lock (monPipeLock) {
        string reply = "ok";
        foreach (string c in commands) {
          Dictionary<string, object> data;
          string r = PipeRequest(HostSettings.HWCTL_PIPE, c, DisplayTimeoutMs, out data);
          if (r != "ok") { reply = r; break; }
        }
        SetMonReply(id, reply);
        Log("mon: #" + id + " " + string.Join(" + ", commands) + " -> " + reply);
      }
    });
  }

  static void SetMonReply(string id, string reply) {
    lock (monLock) {
      monReply = TruncateUtf8(id + " " + reply, 120);
      monReplyExpiresUtc = DateTime.UtcNow.AddSeconds(30);
    }
    sendNow.Set();
  }

  // ---- User-session half ----
  // Session 0 cannot see the user's desktop or media sessions, so the service
  // starts this exe again with --user in the active console session (the
  // signed-in user's token, so it runs with that user's normal rights) and
  // keeps one running there. --console mode starts it as the current user.
  // Two anonymous pipes, UTF-8 lines. User half to service:
  //   F <feed> <n>   then n key=value lines (feeds: agy, media, idle; @source helper:<feed>)
  //   A <base64>     cover art, 144x144 RGB565 big-endian (41,472 bytes)
  //   L <text>       log line, written to this log as "user: <text>"
  // Service to user half:
  //   S <nonce> <webhook>   take a screenshot
  // The user half exits as soon as its pipes break, so it never outlives us.
  class HelperFeed {
    public string[] Lines;
    public DateTime ReceivedUtc;
  }

  static readonly object helperLock = new object();
  static readonly Dictionary<string, HelperFeed> helperFeeds = new Dictionary<string, HelperFeed>(StringComparer.OrdinalIgnoreCase);
  static byte[] helperArt;           // latest cover art from the user half
  static StreamWriter helperOut;     // pipe to the user half, null while none is connected
  static Thread helperThread;
  static volatile bool helperStop;
  static bool noUserHalf;            // --no-user

  static bool SendToUserHalf(string line) {
    lock (helperLock) {
      if (helperOut == null) return false;
      try { helperOut.Write(line + "\n"); return true; }
      catch (Exception) { helperOut = null; return false; }
    }
  }

  // Feed lines for HttpSource ("helper:<feed>"); null while no user half is connected
  static string[] GetHelperFeed(string feed) {
    lock (helperLock) {
      HelperFeed f;
      if (helperOut == null || !helperFeeds.TryGetValue(feed, out f)) return null;
      return f.Lines;
    }
  }

  // stats.ini files from before the merge still point at the files the old
  // esp32-agyquota.exe wrote; those now arrive over the pipe
  static string MapLegacySourceUrl(string url) {
    string u = url.Replace('\\', '/').ToLowerInvariant();
    if (!u.StartsWith("file:")) return url;
    if (u.EndsWith("/esp32pcstats/agy_quota.txt")) return "helper:agy";
    if (u.EndsWith("/esp32pcstats/media.txt")) return "helper:media";
    return url;
  }

  static void StartUserHalf() {
    if (noUserHalf || helperThread != null) return;
    helperStop = false;
    helperThread = new Thread(SuperviseUserHalf) { IsBackground = true, Name = "user_half" };
    helperThread.Start();
  }

  static void StopUserHalf() {
    helperStop = true;
    if (helperThread != null) helperThread.Join(3000);
    helperThread = null;
  }

  // Keeps one user half running in the active console session: starts it when
  // someone signs in, restarts it if it dies, moves it on a user switch.
  static void SuperviseUserHalf() {
    IntPtr proc = IntPtr.Zero;
    uint procSession = 0;
    DateTime startedUtc = DateTime.MinValue, nextTryUtc = DateTime.MinValue;
    string lastWait = null;
    while (!helperStop) {
      uint session = consoleMode ? 0 : WTSGetActiveConsoleSessionId();
      if (proc != IntPtr.Zero) {
        bool alive = WaitForSingleObject(proc, 0) == 0x102;   // WAIT_TIMEOUT
        if (alive && !consoleMode && session != procSession) {
          Log("user half: console session " + procSession + " -> " + session + ", moving");
          TerminateProcess(proc, 0);
          WaitForSingleObject(proc, 2000);
          alive = false;
        }
        if (!alive) {
          uint code;
          GetExitCodeProcess(proc, out code);
          CloseHandle(proc);
          proc = IntPtr.Zero;
          lock (helperLock) { helperOut = null; helperFeeds.Clear(); helperArt = null; }
          // a quick exit is usually another copy holding the per-session mutex
          bool quick = (DateTime.UtcNow - startedUtc).TotalSeconds < 15;
          Log("user half: exited, code " + code + (quick ? ", retry in 30 s" : ""));
          nextTryUtc = DateTime.UtcNow.AddSeconds(quick ? 30 : 2);
        }
      }
      if (proc == IntPtr.Zero && DateTime.UtcNow >= nextTryUtc) {
        string why;
        proc = SpawnUserHalf(session, out why);
        if (proc != IntPtr.Zero) {
          procSession = session;
          startedUtc = DateTime.UtcNow;
          lastWait = null;
          Log("user half: started in session " + session);
        } else {
          if (why != lastWait) Log("user half: waiting, " + why);
          lastWait = why;
          nextTryUtc = DateTime.UtcNow.AddSeconds(5);
        }
      }
      for (int i = 0; i < 20 && !helperStop; i++) Thread.Sleep(100);
    }
    lock (helperLock) {
      if (helperOut != null) { try { helperOut.Dispose(); } catch (Exception) { } }
      helperOut = null;
    }
    if (proc != IntPtr.Zero) {
      if (WaitForSingleObject(proc, 1000) != 0) TerminateProcess(proc, 0);   // it exits on its own when the pipe closes
      CloseHandle(proc);
    }
  }

  static IntPtr SpawnUserHalf(uint session, out string why) {
    why = null;
    IntPtr token = IntPtr.Zero, env = IntPtr.Zero;
    if (!consoleMode && (session == 0xFFFFFFFF || !WTSQueryUserToken(session, out token))) {
      why = "nobody signed in at the console";
      return IntPtr.Zero;
    }
    var toChild = new AnonymousPipeServerStream(PipeDirection.Out, HandleInheritability.Inheritable);
    var fromChild = new AnonymousPipeServerStream(PipeDirection.In, HandleInheritability.Inheritable);
    try {
      string exe = Environment.ProcessPath;   // single-file: Assembly.Location is empty
      string cmd = "\"" + exe + "\" --user " + toChild.GetClientHandleAsString() + " " + fromChild.GetClientHandleAsString();
      STARTUPINFO si = new STARTUPINFO();
      si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
      PROCESS_INFORMATION pi;
      bool ok;
      const uint CREATE_NO_WINDOW = 0x08000000, CREATE_UNICODE_ENVIRONMENT = 0x00000400;
      if (consoleMode) {
        ok = CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, true, CREATE_NO_WINDOW, IntPtr.Zero,
            Path.GetDirectoryName(exe), ref si, out pi);
      } else {
        si.lpDesktop = @"winsta0\default";
        if (!CreateEnvironmentBlock(out env, token, false)) env = IntPtr.Zero;   // USERPROFILE etc. of that user
        ok = CreateProcessAsUser(token, exe, cmd, IntPtr.Zero, IntPtr.Zero, true, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            env, Path.GetDirectoryName(exe), ref si, out pi);
      }
      if (!ok) {
        why = "CreateProcess failed (" + Marshal.GetLastWin32Error() + ")";
        toChild.Dispose();
        fromChild.Dispose();
        return IntPtr.Zero;
      }
      CloseHandle(pi.hThread);
      toChild.DisposeLocalCopyOfClientHandle();
      fromChild.DisposeLocalCopyOfClientHandle();
      StreamWriter w = new StreamWriter(toChild, Utf8NoBom) { AutoFlush = true };
      lock (helperLock) { helperOut = w; }
      new Thread(delegate() { ReadUserHalf(fromChild, w); }) { IsBackground = true, Name = "user_half_reader" }.Start();
      return pi.hProcess;
    } finally {
      if (env != IntPtr.Zero) DestroyEnvironmentBlock(env);
      if (token != IntPtr.Zero) CloseHandle(token);
    }
  }

  static void ReadUserHalf(Stream fromChild, StreamWriter toChild) {
    try {
      using (var r = new StreamReader(fromChild, Utf8NoBom)) {
        string line;
        while ((line = r.ReadLine()) != null) {
          if (line.StartsWith("F ")) {
            string[] p = line.Split(' ');
            int n;
            if (p.Length != 3 || !int.TryParse(p[2], out n) || n < 0 || n > 1000) { Log("user half: bad feed header"); break; }
            string[] lines = new string[n];
            for (int i = 0; i < n; i++) {
              lines[i] = r.ReadLine();
              if (lines[i] == null) return;
            }
            lock (helperLock) { helperFeeds[p[1]] = new HelperFeed { Lines = lines, ReceivedUtc = DateTime.UtcNow }; }
          } else if (line.StartsWith("A ")) {
            byte[] art = Convert.FromBase64String(line.Substring(2));
            if (art.Length == 41472) lock (helperLock) { helperArt = art; }
          } else if (line.StartsWith("L ")) {
            Log("user: " + line.Substring(2));
          }
        }
      }
    } catch (Exception ex) {
      Log("user half: pipe read: " + ex.Message);
    } finally {
      lock (helperLock) { if (helperOut == toChild) helperOut = null; }
      try { toChild.Dispose(); } catch (Exception) { }
    }
  }

  static void HandleArtLine(string line) {
    Match m = Regex.Match(line, @"^!art\s+([0-9a-zA-Z]+)$");
    if (!m.Success) {
      Log("art: unrecognized command '" + line + "'");
      return;
    }
    string id = m.Groups[1].Value;
    Log("art: resend requested for id " + id);
    TriggerArtResend(id);
  }

  static void HandlePowerLine(string line) {
    if (line == null) return;
    line = line.Trim();
    Match m = PowerCmdRegex.Match(line);
    if (!m.Success) {
      Log("power: unrecognized command '" + line + "'");
      return;
    }
    string action = m.Groups[1].Value.ToLowerInvariant();
    string nonce = m.Groups[2].Value.ToLowerInvariant();

    if (action != "sleep" && action != "hibernate" && action != "restart" && action != "shutdown" && action != "lock") {
      Log("power: unrecognized action '" + action + "' in command '" + line + "'");
      return;
    }

    lock (powerLock) {
      DateTime now = DateTime.UtcNow;

      var oldNonces = new List<string>();
      foreach (KeyValuePair<string, DateTime> kv in recentNonces) {
        if ((now - kv.Value).TotalSeconds >= 60.0) {
          oldNonces.Add(kv.Key);
        }
      }
      for (int i = 0; i < oldNonces.Count; i++) {
        recentNonces.Remove(oldNonces[i]);
      }

      if (recentNonces.ContainsKey(nonce)) {
        // Duplicate nonce within 60 s: ignore
        return;
      }
      recentNonces[nonce] = now;

      if (action == "hibernate") {
        if (!IsHibernateSupported()) {
          currentAckValue = "error " + nonce + " hibernate disabled";
          ackExpiresUtc = now.AddSeconds(10);
          scheduledAction = null;
          scheduledActionTimeUtc = DateTime.MinValue;
          Log("power: hibernate requested but hibernation is disabled");
          return;
        }
      }

      currentAckValue = action + " " + nonce;
      ackExpiresUtc = now.AddSeconds(10);
      scheduledAction = action;
      scheduledActionTimeUtc = DateTime.MinValue; // will be scheduled ~1.5s after first sent
      Log("power: " + action + " requested by the board");
    }
  }

  static bool IsHibernateSupported() {
    IntPtr buf = Marshal.AllocHGlobal(128);
    try {
      for (int i = 0; i < 128; i++) Marshal.WriteByte(buf, i, 0);
      if (!GetPwrCapabilities(buf)) {
        Log("GetPwrCapabilities failed: " + Marshal.GetLastWin32Error());
        return false;
      }
      // HiberFilePresent is at offset 8 (preceded by 8 1-byte BOOLEAN flags)
      return Marshal.ReadByte(buf, 8) != 0;
    } finally {
      Marshal.FreeHGlobal(buf);
    }
  }

  static void EnableShutdownPrivilege() {
    IntPtr hToken;
    if (!OpenProcessToken(GetCurrentProcess(), 0x0020 | 0x0008, out hToken)) {
      Log("OpenProcessToken failed: " + Marshal.GetLastWin32Error());
      return;
    }
    try {
      LUID luid;
      if (!LookupPrivilegeValue(null, "SeShutdownPrivilege", out luid)) {
        Log("LookupPrivilegeValue failed: " + Marshal.GetLastWin32Error());
        return;
      }
      TOKEN_PRIVILEGES tp = new TOKEN_PRIVILEGES();
      tp.PrivilegeCount = 1;
      tp.Luid = luid;
      tp.Attributes = 0x00000002; // SE_PRIVILEGE_ENABLED
      if (!AdjustTokenPrivileges(hToken, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero)) {
        Log("AdjustTokenPrivileges failed: " + Marshal.GetLastWin32Error());
      }
    } finally {
      CloseHandle(hToken);
    }
  }

  // Lock the console session: rundll32 user32.dll,LockWorkStation started in
  // that session with the user's token (SYSTEM may ask for it)
  static void LockConsoleSession() {
    uint session = WTSGetActiveConsoleSessionId();
    IntPtr token;
    if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, out token)) {
      Log("power: lock: no user signed in at the console (" + Marshal.GetLastWin32Error() + ")");
      return;
    }
    try {
      STARTUPINFO si = new STARTUPINFO();
      si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
      si.lpDesktop = @"winsta0\default";
      PROCESS_INFORMATION pi;
      string exe = Path.Combine(Environment.SystemDirectory, "rundll32.exe");
      if (!CreateProcessAsUser(token, exe, "\"" + exe + "\" user32.dll,LockWorkStation", IntPtr.Zero, IntPtr.Zero,
              false, 0x08000000, IntPtr.Zero, null, ref si, out pi)) {   // CREATE_NO_WINDOW
        Log("power: lock: CreateProcessAsUser failed: " + Marshal.GetLastWin32Error());
        return;
      }
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    } finally {
      CloseHandle(token);
    }
  }

  static void ExecutePowerAction(string action) {
    try {
      if (action == "lock") {
        LockConsoleSession();
      } else if (action == "sleep") {
        if (!SetSuspendState(false, false, false)) {
          Log("power: SetSuspendState(sleep) failed: " + Marshal.GetLastWin32Error());
        }
      } else if (action == "hibernate") {
        if (!SetSuspendState(true, false, false)) {
          Log("power: SetSuspendState(hibernate) failed: " + Marshal.GetLastWin32Error());
        }
      } else if (action == "restart") {
        EnableShutdownPrivilege();
        uint flags = 0x00000004 | 0x00000001; // SHUTDOWN_RESTART | SHUTDOWN_FORCE_OTHERS
        uint reason = 0x80000000;              // SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED
        uint res = InitiateShutdown(null, null, 0, flags, reason);
        if (res != 0) {
          Log("power: InitiateShutdown(restart) failed: " + res);
        }
      } else if (action == "shutdown") {
        EnableShutdownPrivilege();
        uint flags = 0x00000008 | 0x00000001; // SHUTDOWN_POWEROFF | SHUTDOWN_FORCE_OTHERS
        uint reason = 0x80000000;              // SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED
        uint res = InitiateShutdown(null, null, 0, flags, reason);
        if (res != 0) {
          Log("power: InitiateShutdown(shutdown) failed: " + res);
        }
      }
    } catch (Exception ex) {
      Log("power: execution exception (" + action + "): " + ex.Message);
    }
  }

  static readonly string[] NicSkip = HostSettings.List(HostSettings.NIC_SKIP);   // virtual / VPN adapters

  static bool IsVirtualAdapter(string desc) {
    if (string.IsNullOrEmpty(desc)) return false;
    string d = desc.ToLowerInvariant();
    foreach (string skip in NicSkip)
      if (d.IndexOf(skip, StringComparison.OrdinalIgnoreCase) >= 0) return true;
    return false;
  }

  static bool IsEthernetType(NetworkInterfaceType type) {
    return type == NetworkInterfaceType.Ethernet ||
           (int)type == 117 || // GigabitEthernet
           (int)type == 62 ||  // FastEthernetT
           (int)type == 69;   // FastEthernetFx
  }

  static bool HasIpv4Gateway(NetworkInterface nic) {
    try {
      IPInterfaceProperties ip = nic.GetIPProperties();
      if (ip == null || ip.GatewayAddresses == null) return false;
      foreach (GatewayIPAddressInformation gw in ip.GatewayAddresses) {
        if (gw.Address != null && gw.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork) {
          if (!gw.Address.Equals(IPAddress.Any) && !gw.Address.Equals(IPAddress.None)) {
            return true;
          }
        }
      }
    } catch (Exception) { }
    return false;
  }

  static string FormatMac(byte[] bytes) {
    if (bytes == null || bytes.Length != 6) return null;
    bool allZero = true;
    for (int i = 0; i < bytes.Length; i++) {
      if (bytes[i] != 0) { allZero = false; break; }
    }
    if (allZero) return null;
    return string.Format("{0:X2}:{1:X2}:{2:X2}:{3:X2}:{4:X2}:{5:X2}",
                         bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
  }

  static void RefreshWolTarget() {
    try {
      NetworkInterface[] nics = NetworkInterface.GetAllNetworkInterfaces();
      var candidates = new List<NetworkInterface>();

      for (int i = 0; i < nics.Length; i++) {
        NetworkInterface nic = nics[i];
        if (IsVirtualAdapter(nic.Description)) continue;
        byte[] macBytes = nic.GetPhysicalAddress().GetAddressBytes();
        if (macBytes == null || macBytes.Length != 6) continue;
        candidates.Add(nic);
      }

      NetworkInterface chosen = null;

      // 1. Operational Ethernet adapter with an IPv4 gateway
      for (int i = 0; i < candidates.Count; i++) {
        NetworkInterface nic = candidates[i];
        if (nic.OperationalStatus == OperationalStatus.Up &&
            IsEthernetType(nic.NetworkInterfaceType) &&
            HasIpv4Gateway(nic)) {
          chosen = nic;
          break;
        }
      }

      // 2. Else any Ethernet (prefer Up)
      if (chosen == null) {
        for (int i = 0; i < candidates.Count; i++) {
          NetworkInterface nic = candidates[i];
          if (nic.OperationalStatus == OperationalStatus.Up && IsEthernetType(nic.NetworkInterfaceType)) {
            chosen = nic;
            break;
          }
        }
      }
      if (chosen == null) {
        for (int i = 0; i < candidates.Count; i++) {
          NetworkInterface nic = candidates[i];
          if (IsEthernetType(nic.NetworkInterfaceType)) {
            chosen = nic;
            break;
          }
        }
      }

      // 3. Else any up adapter with a gateway
      if (chosen == null) {
        for (int i = 0; i < candidates.Count; i++) {
          NetworkInterface nic = candidates[i];
          if (nic.OperationalStatus == OperationalStatus.Up && HasIpv4Gateway(nic)) {
            chosen = nic;
            break;
          }
        }
      }

      if (chosen != null) {
        string mac = FormatMac(chosen.GetPhysicalAddress().GetAddressBytes());
        if (mac != null) {
          currentMac = mac;
          string ifName = Clean(chosen.Name);
          if (ifName.Length > 80) ifName = ifName.Substring(0, 80);
          currentMacIf = ifName;
          return;
        }
      }
    } catch (Exception e) {
      Log("RefreshWolTarget error: " + e.Message);
    }
  }

  static string transportMode = "auto";
  static bool consoleMode;
  static string fixedPort;
  static string logPath;

  // Shutdown / Restart / Update / Sleep detection
  static readonly HashSet<string> UpdateProcesses = new HashSet<string>(StringComparer.OrdinalIgnoreCase) {
    "MoUsoCoreWorker.exe",
    "TrustedInstaller.exe",
    "usoclient.exe",
    "wuauclt.exe",
    "MusNotification.exe",
    "MusNotifyIcon.exe",
    "UsoClient.exe"
  };

  static readonly object goingLock = new object();
  static string goingKind = "none";
  static string goingBy = "-";
  static string goingWhy = "-";
  static long goingId = 0;
  static DateTime goingSetUtc = DateTime.MinValue;
  static DateTime last1074TimeUtc = DateTime.MinValue;
  static int fallbackCounter = 0;

  static EventLogWatcher goingWatcher = null;
  static readonly AutoResetEvent sendNow = new AutoResetEvent(false);
  static readonly AutoResetEvent sendCompleted = new AutoResetEvent(false);

  static string lastDownKind = "unknown";
  static string lastDownAt = "-";

  static bool updatePending = false;
  static DateTime nextUpdatePendingCheckUtc = DateTime.MinValue;
  static bool updateRunning = false;
  static DateTime nextUpdateRunningCheckUtc = DateTime.MinValue;

  static string ExtractProcessName(string process) {
    if (string.IsNullOrEmpty(process)) return "";
    string p = process.Trim();
    if (p.EndsWith(")")) {
      int lastSlash = Math.Max(p.LastIndexOf('\\'), p.LastIndexOf('/'));
      int lastOpen = p.LastIndexOf(" (");
      if (lastOpen > lastSlash) {
        p = p.Substring(0, lastOpen).Trim();
      }
    }
    int slash = Math.Max(p.LastIndexOf('\\'), p.LastIndexOf('/'));
    if (slash >= 0) {
      p = p.Substring(slash + 1).Trim();
    }
    return p;
  }

  public static string ClassifyShutdown(string process, string type, string codeHex, bool updatePending) {
    bool isRestart = !string.IsNullOrEmpty(type) &&
                     type.IndexOf("restart", StringComparison.OrdinalIgnoreCase) >= 0;

    bool isUpdate = updatePending;

    if (!isUpdate) {
      string procName = ExtractProcessName(process);
      if (UpdateProcesses.Contains(procName)) {
        isUpdate = true;
      }
    }

    if (!isUpdate && !string.IsNullOrEmpty(codeHex)) {
      string s = codeHex.Trim();
      if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) {
        s = s.Substring(2);
      }
      uint code;
      if (uint.TryParse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out code)) {
        uint major = (code >> 16) & 0xFF;
        uint minor = code & 0xFFFF;
        if (major == 2 && (minor == 0x3 || minor == 0x10 || minor == 0x11 || minor == 0x12)) {
          isUpdate = true;
        }
      }
    }

    if (isUpdate) {
      return isRestart ? "update_restart" : "update_shutdown";
    }
    return isRestart ? "restart" : "shutdown";
  }

  static void ClearGoing() {
    goingKind = "none";
    goingBy = "-";
    goingWhy = "-";
    goingId = 0;
    goingSetUtc = DateTime.MinValue;
  }

  static bool CheckUpdatePending() {
    try {
      using (var hklm = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry64)) {
        if (hklm != null) {
          using (var k1 = hklm.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired")) {
            if (k1 != null) return true;
          }
          using (var k2 = hklm.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending")) {
            if (k2 != null) return true;
          }
        }
      }
    } catch (Exception ex) {
      Log("going: CheckUpdatePending failed: " + ex.Message);
    }
    return false;
  }

  // Windows is installing updates with nobody signed in: "Updates are
  // underway" after an update restart. Servicing workers alone are not enough:
  // Windows also services in the background while someone works (explorer
  // running) and right after an ordinary boot, at the sign-in screen. So it
  // only counts when the PC went down for an update (the "Working on updates"
  // phase before that restart is announced by event 1074 already).
  static bool CheckUpdateRunning() {
    try {
      if (!lastDownKind.StartsWith("update", StringComparison.Ordinal)) return false;
      if (ProcessRunning("explorer")) return false;
      foreach (string name in new string[] { "TiWorker", "poqexec", "SetupHost" })
        if (ProcessRunning(name)) return true;
    } catch (Exception ex) {
      Log("going: CheckUpdateRunning failed: " + ex.Message);
    }
    return false;
  }

  static bool ProcessRunning(string name) {
    System.Diagnostics.Process[] ps = System.Diagnostics.Process.GetProcessesByName(name);
    foreach (System.Diagnostics.Process p in ps) p.Dispose();
    return ps.Length > 0;
  }

  static bool GetUpdatePendingCached() {
    DateTime now = DateTime.UtcNow;
    if (now >= nextUpdatePendingCheckUtc) {
      updatePending = CheckUpdatePending();
      nextUpdatePendingCheckUtc = now.AddSeconds(30);
    }
    return updatePending;
  }

  static void OnGoingEventRecordWritten(object sender, EventRecordWrittenEventArgs e) {
    if (e == null) return;
    if (e.EventException != null) {
      Log("going: watcher exception: " + e.EventException.Message);
      return;
    }
    if (e.EventRecord == null) return;

    try {
      using (EventRecord rec = e.EventRecord) {
        long recId = rec.RecordId.HasValue ? rec.RecordId.Value : 0;
        string rawProcess = rec.Properties.Count > 0 && rec.Properties[0].Value != null ? rec.Properties[0].Value.ToString() : "";
        string rawReason = rec.Properties.Count > 2 && rec.Properties[2].Value != null ? rec.Properties[2].Value.ToString() : "";
        string rawCode = rec.Properties.Count > 3 && rec.Properties[3].Value != null ? rec.Properties[3].Value.ToString() : "";
        string rawType = rec.Properties.Count > 4 && rec.Properties[4].Value != null ? rec.Properties[4].Value.ToString() : "";

        bool updPending = GetUpdatePendingCached();
        string kind = ClassifyShutdown(rawProcess, rawType, rawCode, updPending);
        string procName = ExtractProcessName(rawProcess);
        if (string.IsNullOrEmpty(procName)) procName = "-";

        string reason = Clean(rawReason);
        if (reason.Length > 60) reason = reason.Substring(0, 60);
        if (string.IsNullOrEmpty(reason)) reason = "-";

        lock (goingLock) {
          goingKind = kind;
          goingBy = procName;
          goingWhy = reason;
          goingId = recId;
          goingSetUtc = DateTime.UtcNow;
          last1074TimeUtc = DateTime.UtcNow;
        }

        Log("going: " + kind + " by " + procName + " (" + reason + ") id=" + recId);
        sendNow.Set();
      }
    } catch (Exception ex) {
      Log("going: OnEventRecordWritten exception: " + ex.Message);
    }
  }

  static void ComputeLastShutdown() {
    lastDownKind = "unknown";
    lastDownAt = "-";
    try {
      ulong uptimeMs = GetTickCount64();
      DateTime bootTime = DateTime.Now.AddMilliseconds(-(double)uptimeMs);

      string queryStr = "*[System[(EventID=1074 or EventID=41 or EventID=6008)]]";
      var query = new EventLogQuery("System", PathType.LogName, queryStr);
      query.ReverseDirection = true;

      using (var reader = new EventLogReader(query)) {
        for (EventRecord rec = reader.ReadEvent(); rec != null; rec = reader.ReadEvent()) {
          using (rec) {
            if (!rec.TimeCreated.HasValue) continue;
            DateTime t = rec.TimeCreated.Value.ToLocalTime();
            bool match = (t < bootTime) || ((rec.Id == 41 || rec.Id == 6008) && t <= bootTime.AddMinutes(5));
            if (!match) continue;

            if (rec.Id == 41 || rec.Id == 6008) {
              lastDownKind = "unexpected";
              lastDownAt = t.ToString("ddd HH:mm", CultureInfo.InvariantCulture);
            } else if (rec.Id == 1074) {
              string rawProcess = rec.Properties.Count > 0 && rec.Properties[0].Value != null ? rec.Properties[0].Value.ToString() : "";
              string rawCode = rec.Properties.Count > 3 && rec.Properties[3].Value != null ? rec.Properties[3].Value.ToString() : "";
              string rawType = rec.Properties.Count > 4 && rec.Properties[4].Value != null ? rec.Properties[4].Value.ToString() : "";

              lastDownKind = ClassifyShutdown(rawProcess, rawType, rawCode, false);
              lastDownAt = t.ToString("ddd HH:mm", CultureInfo.InvariantCulture);
            }
            return;
          }
        }
      }
    } catch (Exception ex) {
      Log("going: ComputeLastShutdown failed: " + ex.Message);
    }
  }

  static void PrintGoingTest() {
    Console.WriteLine("update pending (reboot needed): " + (CheckUpdatePending() ? "yes" : "no"));
    Console.WriteLine("update running (installing, nobody signed in): " + (CheckUpdateRunning() ? "yes" : "no"));
    try {
      string queryStr = "*[System[(EventID=1074)]]";
      var query = new EventLogQuery("System", PathType.LogName, queryStr);
      query.ReverseDirection = true;

      int count = 0;
      using (var reader = new EventLogReader(query)) {
        for (EventRecord rec = reader.ReadEvent(); rec != null && count < 15; rec = reader.ReadEvent()) {
          using (rec) {
            DateTime t = rec.TimeCreated.HasValue ? rec.TimeCreated.Value.ToLocalTime() : DateTime.MinValue;
            string rawProcess = rec.Properties.Count > 0 && rec.Properties[0].Value != null ? rec.Properties[0].Value.ToString() : "";
            string rawReason = rec.Properties.Count > 2 && rec.Properties[2].Value != null ? rec.Properties[2].Value.ToString() : "";
            string rawCode = rec.Properties.Count > 3 && rec.Properties[3].Value != null ? rec.Properties[3].Value.ToString() : "";
            string rawType = rec.Properties.Count > 4 && rec.Properties[4].Value != null ? rec.Properties[4].Value.ToString() : "";

            string proc = ExtractProcessName(rawProcess);
            if (string.IsNullOrEmpty(proc)) proc = "-";
            string kind = ClassifyShutdown(proc, rawType, rawCode, false);
            string timeStr = t.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture);

            Console.WriteLine("{0} | {1} | {2} | {3} | {4} -> {5}",
              timeStr, proc, rawType, rawCode, Clean(rawReason), kind);
            count++;
          }
        }
      }
    } catch (Exception ex) {
      Console.WriteLine("Error reading 1074 events: " + ex.Message);
    }

    ComputeLastShutdown();
    bool updPending = CheckUpdatePending();

    Console.WriteLine("pc.last_down: " + lastDownKind);
    Console.WriteLine("pc.last_down_at: " + lastDownAt);
    Console.WriteLine("pc.update_pending: " + (updPending ? "1" : "0"));
  }

  Thread worker;
  volatile bool stop;

  enum StatKind { Feed, Ram, Net, Uptime, Drive, Counter, CpuMhz, EcoreLoad, ProcMb }

  class StatRule {
    public string Key;
    public StatKind Kind;
    // PcHwCtl feed key (@feed)
    public string FeedKey;
    // @ram load|used|free
    public string RamMetric;
    // @net down|up <adapters>, with the byte count at the last tick
    public string NetMetric;
    public string[] NetAdapters;
    public long NetLastBytes;
    public DateTime NetLastUtc = DateTime.MinValue;
    // Drive rule
    public string DriveLetter;
    public string DriveMetric;
    // Windows performance counter rule (@counter, @cpu_mhz)
    public System.Diagnostics.PerformanceCounter Counter;
    public float Scale = 1;
    // E-core load rule (@ecore_load)
    public List<System.Diagnostics.PerformanceCounter> EcoreCounters;
    // Process working set rule (@proc_mb)
    public string ProcessName;
  }

  class HttpSource {
    public string Name;
    public string Url;
    public int IntervalMs;
    public Thread Worker;
    public volatile bool Stop;
    public readonly object Lock = new object();
    public Dictionary<string, double> Numbers = new Dictionary<string, double>();
    public Dictionary<string, string> Texts = new Dictionary<string, string>();
    public DateTime LastSuccessUtc = DateTime.MinValue;
    CancellationTokenSource currentCancel;

    public void Start() {
      Worker = new Thread(Run) { IsBackground = true, Name = "source_" + Name };
      Worker.Start();
    }

    public void StopWorker() {
      Stop = true;
      lock (Lock) {
        if (currentCancel != null) {
          try { currentCancel.Cancel(); } catch (Exception) { }
        }
      }
      if (Worker != null) Worker.Join(1000);
    }

    void Run() {
      while (!Stop) {
        DateTime t0 = DateTime.UtcNow;
        if (Url.StartsWith("helper:", StringComparison.OrdinalIgnoreCase)) {
          // fed by the user half over its pipe; fresh as long as it is connected
          string[] feed = GetHelperFeed(Url.Substring(7));
          if (feed != null) Apply(new StringReader(string.Join("\n", feed)));
        } else try {
          if (new Uri(Url).IsFile) {
            string text;
            using (var fs = new FileStream(new Uri(Url).LocalPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            using (var reader = new StreamReader(fs, Encoding.UTF8)) {
              text = reader.ReadToEnd();
            }
            Apply(new StringReader(text));
          } else {
            CancellationTokenSource cts = new CancellationTokenSource();
            lock (Lock) { currentCancel = cts; }
            string body = Http.Get(Url, null, 5000, cts.Token);
            Apply(new StringReader(body));
          }
        } catch (Exception) {
          // Hub might be sleeping or unreachable, keep going
        } finally {
          lock (Lock) { currentCancel = null; }
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

    void Apply(TextReader reader) {
      var newNumbers = new Dictionary<string, double>();
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

        double num;
        if (double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out num) && !double.IsNaN(num) && !double.IsInfinity(num)) {
          newNumbers[k] = num;
        } else {
          string text = CleanText(v);
          text = TruncateUtf8(text, 120);
          newTexts[k] = text;
        }
      }

      lock (Lock) {
        Numbers = newNumbers;
        Texts = newTexts;
        LastSuccessUtc = DateTime.UtcNow;
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
    CancellationTokenSource currentLiveCancel;

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
        if (currentLiveCancel != null) {
          try { currentLiveCancel.Cancel(); } catch (Exception) { }
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
        var dict = Json.ParseObject(json);
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

      CancellationTokenSource cts = new CancellationTokenSource();
      lock (syncLock) { currentLiveCancel = cts; }

      try {
        Dictionary<string, string> headers = new Dictionary<string, string>();
        headers["Authorization"] = "Bearer " + token;
        headers["anthropic-beta"] = "oauth-2025-04-20";
        string respJson = Http.Get("https://api.anthropic.com/api/oauth/usage", headers, 10000, cts.Token);
        var respDict = Json.ParseObject(respJson);
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
        return LiveMs;
      } catch (HttpFailure ex) {
        string err = "request_failed";
        int status = ex.Status;
        if (status == 401 || status == 403) err = "not_logged_in";
        else if (status == 429) err = "rate_limited";
        else err = "request_failed";
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
        lock (syncLock) { currentLiveCancel = null; }
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

            ParseLinesIntoCache(chunk, cache, cutoff31d);
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

    static void ParseLinesIntoCache(string text, ClaudeFileCache cache, DateTime cutoff) {
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
              var root = Json.ParseObject(line);
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
            tokens.Add("claude.s_reset_s=" + Fmt((double)remSec));
            tokens.Add("claude.s_reset_ts=" + Fmt(Math.Floor((live.FiveHour.ResetsAtUtc - UnixEpochUtc).TotalSeconds)));
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
            tokens.Add("claude.w_reset_s=" + Fmt((double)remSec));
            tokens.Add("claude.w_reset_ts=" + Fmt(Math.Floor((live.SevenDay.ResetsAtUtc - UnixEpochUtc).TotalSeconds)));
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

        tokens.Add("claude.tok_5h=" + Fmt((double)tok5h));
        tokens.Add("claude.tok_today=" + Fmt((double)tokToday));
        tokens.Add("claude.tok_7d=" + Fmt((double)tok7d));
        tokens.Add("claude.tok_30d=" + Fmt((double)tok30d));
        tokens.Add("claude.usd_today=" + Fmt((double)usdToday));
        tokens.Add("claude.usd_30d=" + Fmt((double)usd30d));
      }

      if (!string.IsNullOrEmpty(plan)) {
        texts.Add(new KeyValuePair<string, string>("claude.plan", plan));
      }
      texts.Add(new KeyValuePair<string, string>("claude.err", err));
    }
  }

  static void ClaudeTest(string dir) {
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
    // the user-session half and its test modes (AgyQuota.cs) want an STA thread
    if (args.Length == 3 && args[0] == "--user") { RunSta(delegate() { AgyQuota.RunUser(args[1], args[2]); }); return; }
    if (args.Length == 1 && args[0] == "--agy-once") { RunSta(AgyQuota.RunOnceTest); return; }
    if (args.Length == 1 && args[0] == "--media") { RunSta(AgyQuota.RunMediaTest); return; }
    if (args.Length == 1 && args[0] == "--agy-usage") {
      AgyUsage.Window w, f;
      bool ok = AgyUsage.Read(out w, out f);
      Console.WriteLine(AgyUsage.LastText ?? "(no panel text)");
      Console.WriteLine("parsed: " + ok + ", weekly " + w.Pct + "% reset " + w.ResetUtc + ", 5h " + f.Pct + "% reset " + f.ResetUtc);
      return;
    }
    for (int i = 0; i < args.Length; i++) {
      if (args[i] == "--console") consoleMode = true;
      else if (args[i] == "--no-user") noUserHalf = true;
      else if (args[i] == "--port" && i + 1 < args.Length) fixedPort = args[++i];
      else if (args[i] == "--transport" && i + 1 < args.Length) transportMode = args[++i].ToLowerInvariant();
      else if (args[i] == "--list") { ListFeed(); return; }
      else if (args[i] == "--claude" && i + 1 < args.Length) { ClaudeTest(args[++i]); return; }
      else if (args[i] == "--going") { PrintGoingTest(); return; }
      else if (args[i] == "--render-text" && i + 3 < args.Length) {
        int px = int.Parse(args[++i]);
        bool bold = (args[++i] == "1");
        string outPath = args[++i];
        var textParts = new List<string>();
        for (int j = i + 1; j < args.Length; j++) textParts.Add(args[j]);
        string text = string.Join(" ", textParts.ToArray());
        RenderTextTest(px, bold, outPath, text);
        return;
      }
    }
    string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "ESP32PcStats");
    Directory.CreateDirectory(dir);
    logPath = Path.Combine(dir, "log.txt");
    if (consoleMode) {
      var s = new PcStats();
      Console.CancelKeyPress += delegate(object o, ConsoleCancelEventArgs e) { e.Cancel = true; s.stop = true; };
      // board lines typed here ("!mon 1 bright all 50") are handled as if the board sent them
      new Thread(delegate() {
        string l;
        while ((l = Console.ReadLine()) != null) if (l.StartsWith("!")) HandleBoardLine(l);
      }) { IsBackground = true, Name = "stdin" }.Start();
      StartUserHalf();
      s.Run();
      StopUserHalf();
    } else {
      ServiceBase.Run(new PcStats());
    }
  }

  static void RunSta(ThreadStart body) {
    Thread t = new Thread(body) { Name = "user_main" };
    t.SetApartmentState(ApartmentState.STA);
    t.Start();
    t.Join();
  }

  public PcStats() { ServiceName = "ESP32PcStats"; CanStop = true; CanShutdown = true; CanHandlePowerEvent = true; }

  protected override void OnStart(string[] args) {
    for (int i = 0; i < args.Length; i++) {
      if (args[i] == "--port" && i + 1 < args.Length) fixedPort = args[++i];
      else if (args[i] == "--transport" && i + 1 < args.Length) transportMode = args[++i].ToLowerInvariant();
    }
    worker = new Thread(Run) { IsBackground = true, Name = "pcstats" };
    worker.Start();
    StartUserHalf();
  }

  protected override void OnStop() { stop = true; sendNow.Set(); StopUserHalf(); if (worker != null) worker.Join(5000); }
  protected override void OnShutdown() {
    bool hasRecent1074 = false;
    lock (goingLock) {
      if (last1074TimeUtc != DateTime.MinValue && (DateTime.UtcNow - last1074TimeUtc).TotalSeconds < 60.0) {
        hasRecent1074 = true;
      }
    }
    if (!hasRecent1074) {
      long id = 0x40000000L + (long)Interlocked.Increment(ref fallbackCounter);
      lock (goingLock) {
        goingKind = "shutdown";
        goingBy = "-";
        goingWhy = "-";
        goingId = id;
        goingSetUtc = DateTime.UtcNow;
      }
      Log("going: shutdown (fallback OnShutdown) id=" + id);
      sendCompleted.Reset();
      sendNow.Set();
      sendCompleted.WaitOne(1000);
    }
    OnStop();
  }

  protected override bool OnPowerEvent(PowerBroadcastStatus powerStatus) {
    if (powerStatus == PowerBroadcastStatus.Suspend) {
      long id = 0x40000000L + (long)Interlocked.Increment(ref fallbackCounter);
      lock (goingLock) {
        goingKind = "sleep";
        goingBy = "-";
        goingWhy = "sleep";
        goingId = id;
        goingSetUtc = DateTime.UtcNow;
      }
      Log("going: sleep (OnPowerEvent Suspend) id=" + id);
      sendCompleted.Reset();
      sendNow.Set();
      sendCompleted.WaitOne(500);
      return true;
    }
    if (powerStatus == PowerBroadcastStatus.ResumeSuspend || powerStatus == PowerBroadcastStatus.ResumeAutomatic) {
      lock (goingLock) {
        ClearGoing();
      }
      Log("going: cleared on resume (" + powerStatus + ")");
      return true;
    }
    return true;
  }

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

  static string Clean(string s) { return (s ?? "").Replace('\t', ' ').Replace('\n', ' ').Replace('\r', ' ').Trim(); }

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

  static string FindWinUsb(out SafeFileHandle matchedHandle) {
    matchedHandle = null;
    Guid guid = WinUsbInterfaceGuid;
    IntPtr hDevInfo = SetupDiGetClassDevs(ref guid, IntPtr.Zero, IntPtr.Zero, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == IntPtr.Zero || hDevInfo.ToInt64() == -1) return null;

    try {
      SP_DEVICE_INTERFACE_DATA ifData = new SP_DEVICE_INTERFACE_DATA();
      ifData.cbSize = (uint)Marshal.SizeOf(typeof(SP_DEVICE_INTERFACE_DATA));

      for (uint i = 0; SetupDiEnumDeviceInterfaces(hDevInfo, IntPtr.Zero, ref guid, i, ref ifData); i++) {
        uint reqSize = 0;
        SetupDiGetDeviceInterfaceDetail(hDevInfo, ref ifData, IntPtr.Zero, 0, out reqSize, IntPtr.Zero);
        if (reqSize == 0) continue;

        IntPtr pDetail = Marshal.AllocHGlobal((int)reqSize);
        try {
          Marshal.WriteInt32(pDetail, IntPtr.Size == 8 ? 8 : 6);
          if (!SetupDiGetDeviceInterfaceDetail(hDevInfo, ref ifData, pDetail, reqSize, out reqSize, IntPtr.Zero)) continue;
          string path = Marshal.PtrToStringUni(new IntPtr(pDetail.ToInt64() + 4));
          if (path == null || path.IndexOf("vid_303a", StringComparison.OrdinalIgnoreCase) < 0) continue;
          SafeFileHandle hFile = CreateFile(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            IntPtr.Zero, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, IntPtr.Zero);
          if (hFile != null && !hFile.IsInvalid) {
            matchedHandle = hFile;
            return path;
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

  static ILink TryWinUsb() {
    SafeFileHandle h;
    string path = FindWinUsb(out h);
    if (path == null || h == null) return null;
    try {
      ILink l = new WinUsbLink(h);
      Log("connected winusb " + path);
      return l;
    } catch (Exception e) {
      Log("open winusb " + path + ": " + e.Message);
      if (!h.IsClosed) h.Close();
      return null;
    }
  }

  static ILink TryHid() {
    SafeFileHandle hHid;
    string hidPath = FindHid(out hHid);
    if (hidPath == null || hHid == null) return null;
    try {
      ILink l = new HidLink(hHid);
      Log("connected hid " + hidPath);
      return l;
    } catch (Exception e) {
      Log("open hid " + hidPath + ": " + e.Message);
      if (!hHid.IsClosed) hHid.Close();
      return null;
    }
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

  static readonly DateTime UnixEpochUtc = new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc);

  // Whole numbers go out exact up to 2^53 (uptime, token counters); the
  // rest with 7 significant digits.
  static string Fmt(double? v) {
    if (!v.HasValue || double.IsNaN(v.Value) || double.IsInfinity(v.Value)) return "";
    double d = v.Value;
    if (d == Math.Floor(d) && Math.Abs(d) <= 9007199254740992.0) return d.ToString("0", CultureInfo.InvariantCulture);
    return d.ToString("G7", CultureInfo.InvariantCulture);
  }

  static bool WildcardMatch(string pattern, string input) {
    if (pattern == null || input == null) return false;
    if (pattern == "*") return true;
    string regexPattern = "^" + Regex.Escape(pattern).Replace("\\*", ".*") + "$";
    return Regex.IsMatch(input, regexPattern, RegexOptions.IgnoreCase);
  }

  // Cover art transfer
  class ArtTransfer {
    public string Id;
    public string CrcHex;
    public string Base64;
    public int CharsSent;
    public bool SentBegin;
    public bool Done;
  }

  static readonly object artLock = new object();
  static ArtTransfer activeArtTransfer = null;
  static string lastReceivedArtId = null;
  static string requestedResendArtId = null;
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

  static void TriggerArtResend(string id) {
    lock (artLock) {
      requestedResendArtId = id;
    }
  }

  static void CheckArtUpdate(string currentArtId) {
    lock (artLock) {
      bool trigger = false;
      string targetId = null;

      if (!string.IsNullOrEmpty(requestedResendArtId)) {
        trigger = true;
        targetId = requestedResendArtId;
        requestedResendArtId = null;
      } else if (!string.IsNullOrEmpty(currentArtId) && currentArtId != "0" && currentArtId != lastReceivedArtId) {
        trigger = true;
        targetId = currentArtId;
      }

      if (!string.IsNullOrEmpty(currentArtId)) {
        lastReceivedArtId = currentArtId;
      }

      if (trigger && !string.IsNullOrEmpty(targetId) && targetId != "0") {
        byte[] bytes;
        lock (helperLock) { bytes = helperArt; }   // latest cover from the user half
        if (bytes != null) {
          uint crc = ComputeCrc32(bytes);
          string crcDec = crc.ToString(CultureInfo.InvariantCulture);
          if (crcDec == targetId || crc.ToString("x8").Equals(targetId, StringComparison.OrdinalIgnoreCase)) {
            // the panels are 12-bit: send RGB444 (2 px in 3 bytes), 25% less to move
            byte[] packed = new byte[144 * 144 * 3 / 2];
            for (int px = 0, o = 0; px < 144 * 144; px += 2, o += 3) {
              int a = (bytes[2 * px] << 8) | bytes[2 * px + 1], b = (bytes[2 * px + 2] << 8) | bytes[2 * px + 3];
              packed[o] = (byte)(((a >> 12) << 4) | ((a >> 7) & 15));
              packed[o + 1] = (byte)((((a >> 1) & 15) << 4) | (b >> 12));
              packed[o + 2] = (byte)((((b >> 7) & 15) << 4) | ((b >> 1) & 15));
            }
            activeArtTransfer = new ArtTransfer {
              Id = targetId,
              CrcHex = ComputeCrc32(packed).ToString("x8"),
              Base64 = Convert.ToBase64String(packed),
              CharsSent = 0,
              SentBegin = false,
              Done = false
            };
          }
        }
      }
    }
  }


  // --- Non-ASCII Text Rendering & Streaming Support ---
  static readonly object lastSentTextsLock = new object();
  static readonly Dictionary<string, string> lastSentTexts = new Dictionary<string, string>();

  const uint FR_PRIVATE = 0x10;
  const uint DEFAULT_CHARSET = 1;
  const uint OUT_DEFAULT_PRECIS = 0;
  const uint CLIP_DEFAULT_PRECIS = 0;
  const uint ANTIALIASED_QUALITY = 4;
  const uint DEFAULT_PITCH = 0;

  const int DT_CALCRECT = 0x00000400;
  const int DT_SINGLELINE = 0x00000020;
  const int DT_NOPREFIX = 0x00000800;
  const int DT_NOCLIP = 0x00000100;
  const int TRANSPARENT = 1;
  const uint DIB_RGB_COLORS = 0;

  [StructLayout(LayoutKind.Sequential)]
  struct RECT { public int Left, Top, Right, Bottom; }

  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  struct TEXTMETRICW {
    public int tmHeight, tmAscent, tmDescent, tmInternalLeading, tmExternalLeading;
    public int tmAveCharWidth, tmMaxCharWidth, tmWeight, tmOverhang, tmDigitizedAspectX, tmDigitizedAspectY;
    public char tmFirstChar, tmLastChar, tmDefaultChar, tmBreakChar;
    public byte tmItalic, tmUnderlined, tmStruckOut, tmPitchAndFamily, tmCharSet;
  }

  [StructLayout(LayoutKind.Sequential)]
  struct BITMAPINFOHEADER {
    public uint biSize;
    public int biWidth, biHeight;
    public ushort biPlanes, biBitCount;
    public uint biCompression, biSizeImage;
    public int biXPelsPerMeter, biYPelsPerMeter;
    public uint biClrUsed, biClrImportant;
  }

  [StructLayout(LayoutKind.Sequential)]
  struct BITMAPINFO { public BITMAPINFOHEADER bmiHeader; }

  [DllImport("gdi32.dll", SetLastError = true, CharSet = CharSet.Auto)]
  static extern int AddFontResourceEx(string lpszFilename, uint fl, IntPtr pdv);

  [DllImport("gdi32.dll", SetLastError = true)]
  static extern IntPtr CreateCompatibleDC(IntPtr hdc);

  [DllImport("gdi32.dll", SetLastError = true)]
  static extern bool DeleteDC(IntPtr hdc);

  [DllImport("gdi32.dll", SetLastError = true)]
  static extern IntPtr SelectObject(IntPtr hdc, IntPtr hgdiobj);

  [DllImport("gdi32.dll", SetLastError = true)]
  static extern bool DeleteObject(IntPtr hObject);

  [DllImport("gdi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern IntPtr CreateFontW(
    int nHeight, int nWidth, int nEscapement, int nOrientation, int fnWeight,
    uint fdwItalic, uint fdwUnderline, uint fdwStrikeOut, uint fdwCharSet,
    uint fdwOutputPrecision, uint fdwClipPrecision, uint fdwQuality,
    uint fdwPitchAndFamily, string lpszFace);

  [DllImport("gdi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern bool GetTextMetricsW(IntPtr hdc, out TEXTMETRICW lptm);

  [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern int DrawTextW(IntPtr hdc, string lpchText, int nCount, ref RECT lpRect, uint uFormat);

  [DllImport("gdi32.dll", SetLastError = true)]
  static extern uint SetTextColor(IntPtr hdc, uint crColor);

  [DllImport("gdi32.dll", SetLastError = true)]
  static extern int SetBkMode(IntPtr hdc, int iBkMode);

  [DllImport("gdi32.dll", SetLastError = true)]
  static extern IntPtr CreateDIBSection(IntPtr hdc, [In] ref BITMAPINFO pbmi, uint iUsage, out IntPtr ppvBits, IntPtr hSection, uint dwOffset);

  static readonly object fontLock = new object();
  static bool fontsInitialized = false;
  static string fontRegularFamily = null;
  static string fontBoldFamily = null;
  static bool fontFallbackLogged = false;

  static void EnsureFontsLoaded() {
    if (fontsInitialized) return;
    lock (fontLock) {
      if (fontsInitialized) return;

      string regPath = null;
      string boldPath = null;

      string winFonts = @"C:\Windows\Fonts";
      string rWin = Path.Combine(winFonts, HostSettings.FONT_REGULAR);
      if (File.Exists(rWin)) regPath = rWin;
      string bWin = Path.Combine(winFonts, HostSettings.FONT_BOLD);
      if (File.Exists(bWin)) boldPath = bWin;

      if (regPath == null || boldPath == null) {
        if (Directory.Exists(@"C:\Users")) {
          try {
            string[] userDirs = Directory.GetDirectories(@"C:\Users");
            for (int i = 0; i < userDirs.Length; i++) {
              string uFonts = Path.Combine(userDirs[i], @"AppData\Local\Microsoft\Windows\Fonts");
              if (Directory.Exists(uFonts)) {
                if (regPath == null) {
                  string r = Path.Combine(uFonts, HostSettings.FONT_REGULAR);
                  if (File.Exists(r)) regPath = r;
                }
                if (boldPath == null) {
                  string b = Path.Combine(uFonts, HostSettings.FONT_BOLD);
                  if (File.Exists(b)) boldPath = b;
                }
                if (regPath != null && boldPath != null) break;
              }
            }
          } catch (Exception) { }
        }
      }

      if (regPath != null) {
        try {
          AddFontResourceEx(regPath, FR_PRIVATE, IntPtr.Zero);
          using (var pfc = new PrivateFontCollection()) {
            pfc.AddFontFile(regPath);
            if (pfc.Families.Length > 0) {
              fontRegularFamily = pfc.Families[0].Name;
            }
          }
        } catch (Exception ex) {
          Log("txt: failed loading regular font " + regPath + ": " + ex.Message);
        }
      }

      if (boldPath != null) {
        try {
          AddFontResourceEx(boldPath, FR_PRIVATE, IntPtr.Zero);
          using (var pfc = new PrivateFontCollection()) {
            pfc.AddFontFile(boldPath);
            if (pfc.Families.Length > 0) {
              fontBoldFamily = pfc.Families[0].Name;
            }
          }
        } catch (Exception ex) {
          Log("txt: failed loading bold font " + boldPath + ": " + ex.Message);
        }
      }

      if (fontRegularFamily == null && fontBoldFamily == null) {
        if (!fontFallbackLogged) {
          Log("txt: " + Path.GetFileNameWithoutExtension(HostSettings.FONT_REGULAR).Split('-')[0] + " font not found, using " + HostSettings.FONT_FALLBACK);
          fontFallbackLogged = true;
        }
      }

      fontsInitialized = true;
    }
  }

  class RenderedText {
    public int W;
    public int H;
    public int Ascent;
    public byte[] Packed;
  }

  static RenderedText RenderText(string text, int px, bool bold) {
    if (text == null) text = "";
    EnsureFontsLoaded();

    string family = bold ? (fontBoldFamily ?? fontRegularFamily) : (fontRegularFamily ?? fontBoldFamily);
    if (string.IsNullOrEmpty(family)) family = HostSettings.FONT_FALLBACK;

    IntPtr memDC = CreateCompatibleDC(IntPtr.Zero);
    if (memDC == IntPtr.Zero) return null;

    IntPtr hFont = CreateFontW(
      -px, 0, 0, 0, bold ? 700 : 400, 0, 0, 0,
      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, family);
    IntPtr oldFont = SelectObject(memDC, hFont);

    RECT rc = new RECT();
    DrawTextW(memDC, text, text.Length, ref rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int measuredWidth = rc.Right - rc.Left;
    if (measuredWidth < 0) measuredWidth = 0;
    int w = measuredWidth + 2;
    if (w > 1600) w = 1600;
    if (w < 1) w = 1;

    TEXTMETRICW tm;
    GetTextMetricsW(memDC, out tm);
    int h = tm.tmHeight;
    if (h < 1) h = 1;
    int ascent = tm.tmAscent;

    BITMAPINFO bmi = new BITMAPINFO();
    bmi.bmiHeader.biSize = (uint)Marshal.SizeOf(typeof(BITMAPINFOHEADER));
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = 0;

    IntPtr pBits;
    IntPtr hBmp = CreateDIBSection(memDC, ref bmi, DIB_RGB_COLORS, out pBits, IntPtr.Zero, 0);
    if (hBmp == IntPtr.Zero || pBits == IntPtr.Zero) {
      SelectObject(memDC, oldFont);
      DeleteObject(hFont);
      DeleteDC(memDC);
      return null;
    }

    IntPtr oldBmp = SelectObject(memDC, hBmp);

    SetTextColor(memDC, 0x00FFFFFF);
    SetBkMode(memDC, TRANSPARENT);
    RECT drawRc = new RECT();
    drawRc.Left = 1;
    drawRc.Top = 0;
    drawRc.Right = w;
    drawRc.Bottom = h;
    DrawTextW(memDC, text, text.Length, ref drawRc, DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);

    byte[] dibBytes = new byte[w * h * 4];
    Marshal.Copy(pBits, dibBytes, 0, dibBytes.Length);

    SelectObject(memDC, oldBmp);
    DeleteObject(hBmp);
    SelectObject(memDC, oldFont);
    DeleteObject(hFont);
    DeleteDC(memDC);

    int stride = (w + 1) / 2;
    byte[] packed = new byte[stride * h];
    for (int y = 0; y < h; y++) {
      int dibRowOffset = y * w * 4;
      int packedRowOffset = y * stride;
      for (int x = 0; x < w; x++) {
        int pxOffset = dibRowOffset + (x * 4);
        byte b = dibBytes[pxOffset];
        byte g = dibBytes[pxOffset + 1];
        byte r = dibBytes[pxOffset + 2];
        int maxVal = Math.Max(r, Math.Max(g, b));
        byte alpha4 = (byte)(maxVal >> 4);

        int byteIdx = packedRowOffset + (x >> 1);
        if ((x & 1) == 0) {
          packed[byteIdx] |= (byte)(alpha4 << 4);
        } else {
          packed[byteIdx] |= (byte)(alpha4 & 0x0F);
        }
      }
    }

    RenderedText res = new RenderedText();
    res.W = w;
    res.H = h;
    res.Ascent = ascent;
    res.Packed = packed;
    return res;
  }

  static void RenderTextTest(int px, bool bold, string outPath, string text) {
    RenderedText res = RenderText(text, px, bold);
    if (res == null) {
      Console.WriteLine("Render failed");
      return;
    }

    int stride = (res.W + 1) / 2;
    using (var bmp = new Bitmap(res.W * 4, res.H * 4, PixelFormat.Format32bppArgb)) {
      for (int y = 0; y < res.H; y++) {
        for (int x = 0; x < res.W; x++) {
          int byteIdx = y * stride + (x >> 1);
          int a4 = ((x & 1) == 0) ? (res.Packed[byteIdx] >> 4) : (res.Packed[byteIdx] & 0x0F);
          int gray = (a4 << 4) | a4;
          Color c = Color.FromArgb(gray, gray, gray);
          for (int dy = 0; dy < 4; dy++) {
            for (int dx = 0; dx < 4; dx++) {
              bmp.SetPixel(x * 4 + dx, y * 4 + dy, c);
            }
          }
        }
      }
      string dir = Path.GetDirectoryName(outPath);
      if (!string.IsNullOrEmpty(dir) && !Directory.Exists(dir)) {
        Directory.CreateDirectory(dir);
      }
      bmp.Save(outPath, ImageFormat.Png);
    }

    Console.WriteLine("{0} {1} {2} {3}", res.W, res.H, res.Ascent, res.Packed.Length);
  }

  class TextTransfer {
    public string Id;
    public int Px;
    public bool Bold;
    public int W;
    public int H;
    public int Ascent;
    public string Base64;
    public string CrcHex;
    public int CharsSent;
    public bool SentBegin;
    public bool Done;
  }

  static readonly object txtLock = new object();
  static readonly Queue<TextTransfer> pendingTextTransfers = new Queue<TextTransfer>();
  static TextTransfer activeTextTransfer = null;
  static readonly Dictionary<string, DateTime> recentServedTxt = new Dictionary<string, DateTime>();

  static void HandleTxtLine(string line) {
    // "!txt <crc32> <px> <bold> <key> [base64 text]": the text comes along
    // when the board has it from elsewhere (e.g. the weather place)
    Match m = Regex.Match(line, @"^!txt\s+(\d+)\s+(\d+)\s+([01])\s+(\S+)(?:\s+([A-Za-z0-9+/=]+))?$");
    if (!m.Success) {
      Log("txt: unrecognized command '" + line + "'");
      return;
    }

    uint id;
    if (!uint.TryParse(m.Groups[1].Value, NumberStyles.None, CultureInfo.InvariantCulture, out id)) {
      Log("txt: rejected id=" + m.Groups[1].Value + " (invalid id)");
      return;
    }

    int px;
    if (!int.TryParse(m.Groups[2].Value, NumberStyles.None, CultureInfo.InvariantCulture, out px) || px < 8 || px > 64) {
      Log("txt: rejected id=" + id + " px=" + m.Groups[2].Value + " (px out of range 8..64)");
      return;
    }

    int boldVal = int.Parse(m.Groups[3].Value);
    bool bold = (boldVal == 1);
    string key = m.Groups[4].Value;

    string given = null;
    if (m.Groups[5].Success) {
      try {
        given = Utf8NoBom.GetString(Convert.FromBase64String(m.Groups[5].Value));
      } catch (FormatException) {
        Log("txt: rejected key=" + key + " id=" + id + " (bad base64 text)");
        return;
      }
    }

    ProcessTxtRequest(id, px, bold, key, given);
  }

  static void ProcessTxtRequest(uint id, int px, bool bold, string key, string given) {
    string textVal = given;
    if (textVal == null) {
      lock (lastSentTextsLock) {
        lastSentTexts.TryGetValue(key, out textVal);
      }
    }

    if (textVal == null) {
      Log(string.Format("txt: rejected key={0} id={1} (key not found)", key, id));
      return;
    }

    byte[] textUtf8 = Utf8NoBom.GetBytes(textVal);
    uint textCrc = ComputeCrc32(textUtf8);
    if (textCrc != id) {
      Log(string.Format("txt: rejected key={0} id={1} (crc mismatch: current={2})", key, id, textCrc));
      return;
    }

    DateTime now = DateTime.UtcNow;
    string reqSig = key + "|" + id + "|" + px + "|" + (bold ? "1" : "0");

    lock (txtLock) {
      var toRemove = new List<string>();
      foreach (KeyValuePair<string, DateTime> kv in recentServedTxt) {
        if ((now - kv.Value).TotalSeconds >= 10.0) {
          toRemove.Add(kv.Key);
        }
      }
      for (int i = 0; i < toRemove.Count; i++) {
        recentServedTxt.Remove(toRemove[i]);
      }

      DateTime servedTime;
      if (recentServedTxt.TryGetValue(reqSig, out servedTime) && (now - servedTime).TotalSeconds < 5.0) {
        Log(string.Format("txt: rejected key={0} id={1} (duplicate request within 5s)", key, id));
        return;
      }

      if (pendingTextTransfers.Count >= 4) {
        Log(string.Format("txt: rejected key={0} id={1} (queue full)", key, id));
        return;
      }

      recentServedTxt[reqSig] = now;
    }

    RenderedText rendered = RenderText(textVal, px, bold);
    if (rendered == null) {
      Log(string.Format("txt: rejected key={0} id={1} (render failed)", key, id));
      return;
    }

    TextTransfer transfer = new TextTransfer();
    transfer.Id = id.ToString(CultureInfo.InvariantCulture);
    transfer.Px = px;
    transfer.Bold = bold;
    transfer.W = rendered.W;
    transfer.H = rendered.H;
    transfer.Ascent = rendered.Ascent;
    transfer.Base64 = Convert.ToBase64String(rendered.Packed);
    transfer.CrcHex = ComputeCrc32(rendered.Packed).ToString("x8");
    transfer.CharsSent = 0;
    transfer.SentBegin = false;
    transfer.Done = false;

    lock (txtLock) {
      pendingTextTransfers.Enqueue(transfer);
    }

    Log(string.Format("txt: served key={0} id={1} {2}x{3}", key, id, rendered.W, rendered.H));
  }

  static int AppendTextChunk(StringBuilder sb, int budget) {
    if (budget <= 0) return 0;
    lock (txtLock) {
      if (activeTextTransfer == null || activeTextTransfer.Done) {
        if (pendingTextTransfers.Count > 0) {
          activeTextTransfer = pendingTextTransfers.Dequeue();
        } else {
          activeTextTransfer = null;
          return 0;
        }
      }

      if (!activeTextTransfer.SentBegin) {
        sb.Append("@x begin ")
          .Append(activeTextTransfer.Id).Append(' ')
          .Append(activeTextTransfer.Px).Append(' ')
          .Append(activeTextTransfer.Bold ? "1" : "0").Append(' ')
          .Append(activeTextTransfer.W).Append(' ')
          .Append(activeTextTransfer.H).Append(' ')
          .Append(activeTextTransfer.Ascent).Append('\n');
        activeTextTransfer.SentBegin = true;
      }

      int charsThisTick = 0;
      while (activeTextTransfer.CharsSent < activeTextTransfer.Base64.Length && charsThisTick < budget) {
        int take = Math.Min(160, Math.Min(activeTextTransfer.Base64.Length - activeTextTransfer.CharsSent, budget - charsThisTick));
        string chunk = activeTextTransfer.Base64.Substring(activeTextTransfer.CharsSent, take);
        sb.Append("@x d ").Append(chunk).Append('\n');
        activeTextTransfer.CharsSent += take;
        charsThisTick += take;
      }

      if (activeTextTransfer.CharsSent >= activeTextTransfer.Base64.Length) {
        sb.Append("@x end ").Append(activeTextTransfer.Id).Append(' ').Append(activeTextTransfer.CrcHex).Append('\n');
        activeTextTransfer.Done = true;
        activeTextTransfer = null;
      }

      return charsThisTick;
    }
  }

  static void AppendArtChunk(StringBuilder sb, int budget) {
    if (budget <= 0) return;
    lock (artLock) {
      if (activeArtTransfer == null || activeArtTransfer.Done) return;

      if (!activeArtTransfer.SentBegin) {
        sb.Append("@a begin ").Append(activeArtTransfer.Id).Append(" 144 144 444\n");
        activeArtTransfer.SentBegin = true;
      }

      int charsThisTick = 0;
      while (activeArtTransfer.CharsSent < activeArtTransfer.Base64.Length && charsThisTick < budget) {
        int take = Math.Min(160, Math.Min(activeArtTransfer.Base64.Length - activeArtTransfer.CharsSent, budget - charsThisTick));
        string chunk = activeArtTransfer.Base64.Substring(activeArtTransfer.CharsSent, take);
        sb.Append("@a d ").Append(chunk).Append('\n');
        activeArtTransfer.CharsSent += take;
        charsThisTick += take;
      }

      if (activeArtTransfer.CharsSent >= activeArtTransfer.Base64.Length) {
        sb.Append("@a end ").Append(activeArtTransfer.Id).Append(' ').Append(activeArtTransfer.CrcHex).Append('\n');
        activeArtTransfer.Done = true;
      }
    }
  }

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool GetSystemCpuSetInformation(
      IntPtr Information,
      uint BufferLength,
      out uint ReturnedLength,
      IntPtr Process,
      uint Flags);

  struct CpuSetEntry {
    public ushort Group;
    public byte LogicalIndex;
    public byte EfficiencyClass;
  }

  static List<System.Diagnostics.PerformanceCounter> InitEcoreCounters() {
    var list = new List<System.Diagnostics.PerformanceCounter>();
    var entries = new List<CpuSetEntry>();
    try {
      uint len = 0;
      GetSystemCpuSetInformation(IntPtr.Zero, 0, out len, IntPtr.Zero, 0);
      if (len > 0) {
        IntPtr buf = Marshal.AllocHGlobal((int)len);
        try {
          if (GetSystemCpuSetInformation(buf, len, out len, IntPtr.Zero, 0)) {
            int offset = 0;
            while (offset < len) {
              uint size = (uint)Marshal.ReadInt32(buf, offset);
              int type = Marshal.ReadInt32(buf, offset + 4);
              if (type == 0) { // CpuSetInformation
                ushort grp = (ushort)Marshal.ReadInt16(buf, offset + 12);
                byte logicalIdx = Marshal.ReadByte(buf, offset + 14);
                byte effClass = Marshal.ReadByte(buf, offset + 18);
                entries.Add(new CpuSetEntry { Group = grp, LogicalIndex = logicalIdx, EfficiencyClass = effClass });
              }
              if (size == 0) break;
              offset += (int)size;
            }
          }
        } finally {
          Marshal.FreeHGlobal(buf);
        }
      }
    } catch (Exception ex) {
      Log("GetSystemCpuSetInformation error: " + ex.Message);
    }

    byte minEff = 255;
    byte maxEff = 0;
    for (int i = 0; i < entries.Count; i++) {
      if (entries[i].EfficiencyClass < minEff) minEff = entries[i].EfficiencyClass;
      if (entries[i].EfficiencyClass > maxEff) maxEff = entries[i].EfficiencyClass;
    }

    if (entries.Count > 0 && minEff < maxEff) {
      for (int i = 0; i < entries.Count; i++) {
        if (entries[i].EfficiencyClass == minEff) {
          string inst = entries[i].Group + "," + entries[i].LogicalIndex;
          var c = OpenCounter("Processor Information", "% Processor Time", inst);
          if (c != null) list.Add(c);
        }
      }
    }

    if (list.Count == 0) {
      var c = OpenCounter("Processor Information", "% Processor Time", "_Total");
      if (c != null) list.Add(c);
    }

    return list;
  }

  static List<StatRule> LoadRules(string path, List<HttpSource> sources, ref ClaudeSource claudeSource) {
    var list = new List<StatRule>();
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
        Log("stats.ini line " + (i + 1) + ": @every is gone (PcHwCtl's config feed.every paces the sensors), ignored");
        continue;
      }

      if (raw.StartsWith("@source", StringComparison.OrdinalIgnoreCase)) {
        string[] sp = raw.Split(new char[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        int ms;
        if (sp.Length >= 4 && int.TryParse(sp[3], out ms) && ms > 0) {
          var src = new HttpSource();
          src.Name = sp[1].Trim();
          src.Url = MapLegacySourceUrl(sp[2].Trim());
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

      if (valPart.StartsWith("@feed", StringComparison.OrdinalIgnoreCase)) {
        string fk = valPart.Substring(5).Trim();
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.Feed;
        r.FeedKey = fk.Length > 0 ? fk : key;
        list.Add(r);
      } else if (valPart.StartsWith("@ram", StringComparison.OrdinalIgnoreCase)) {
        string metric = valPart.Substring(4).Trim().ToLowerInvariant();
        if (metric != "load" && metric != "used" && metric != "free") {
          Log("stats.ini line " + (i + 1) + ": @ram wants load|used|free");
          continue;
        }
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.Ram;
        r.RamMetric = metric;
        list.Add(r);
      } else if (valPart.StartsWith("@net", StringComparison.OrdinalIgnoreCase)) {
        string[] np = valPart.Substring(4).Trim().Split(new char[] { ' ', '\t' }, 2, StringSplitOptions.RemoveEmptyEntries);
        string metric = np.Length > 0 ? np[0].ToLowerInvariant() : "";
        if (np.Length < 2 || (metric != "down" && metric != "up")) {
          Log("stats.ini line " + (i + 1) + ": @net wants down|up <adapter,...>");
          continue;
        }
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.Net;
        r.NetMetric = metric;
        r.NetAdapters = np[1].Split(new char[] { ',' }, StringSplitOptions.RemoveEmptyEntries);
        for (int k = 0; k < r.NetAdapters.Length; k++) r.NetAdapters[k] = r.NetAdapters[k].Trim();
        list.Add(r);
      } else if (string.Equals(valPart, "@uptime", StringComparison.OrdinalIgnoreCase)) {
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.Uptime;
        list.Add(r);
      } else if (string.Equals(valPart, "@ecore_load", StringComparison.OrdinalIgnoreCase)) {
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.EcoreLoad;
        r.EcoreCounters = InitEcoreCounters();
        list.Add(r);
      } else if (valPart.StartsWith("@proc_mb", StringComparison.OrdinalIgnoreCase)) {
        string procName = valPart.Substring(8).Trim();
        if (procName.EndsWith(".exe", StringComparison.OrdinalIgnoreCase)) {
          procName = procName.Substring(0, procName.Length - 4);
        }
        var r = new StatRule();
        r.Key = key;
        r.Kind = StatKind.ProcMb;
        r.ProcessName = procName;
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
        // the old LibreHardwareMonitor rule (hardware | type | name): PcHwCtl reads the sensors now
        Log("stats.ini line " + (i + 1) + ": '" + key + "' is not a known rule (sensors are '@feed <key>' now, see --list)");
      }
    }
    return list;
  }

  static double? EvalStat(StatRule r) {
    if (r.Kind == StatKind.Feed) return FeedValue(r.FeedKey);
    if (r.Kind == StatKind.Ram) return RamStat(r.RamMetric);
    if (r.Kind == StatKind.Net) return NetStat(r);
    if (r.Kind == StatKind.Uptime) {
      return (double)(GetTickCount64() / 1000);   // whole seconds
    }
    if (r.Kind == StatKind.EcoreLoad) {
      if (r.EcoreCounters == null || r.EcoreCounters.Count == 0) return null;
      double sum = 0;
      int count = 0;
      for (int i = 0; i < r.EcoreCounters.Count; i++) {
        try {
          sum += r.EcoreCounters[i].NextValue();
          count++;
        } catch { }
      }
      return count > 0 ? (double?)(sum / count) : null;
    }
    if (r.Kind == StatKind.ProcMb) {
      if (string.IsNullOrEmpty(r.ProcessName)) return 0.0;
      try {
        System.Diagnostics.Process[] procs = System.Diagnostics.Process.GetProcessesByName(r.ProcessName);
        long totalBytes = 0;
        for (int j = 0; j < procs.Length; j++) {
          using (System.Diagnostics.Process p = procs[j]) {
            try {
              totalBytes += p.WorkingSet64;
            } catch { }
          }
        }
        return (double)(totalBytes / (1024.0 * 1024.0));
      } catch {
        return 0.0;
      }
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
    return null;
  }

  void Run() {
    Log("start");
    NightStart();
    FeedStart();
    ILink link = null;
    List<StatRule> rules = null;
    List<HttpSource> sources = null;
    ClaudeSource claudeSource = null;
    bool iniMissingLogged = false;
    DateTime lastIniWriteTime = DateTime.MinValue;
    DateTime nextScan = DateTime.MinValue;
    uint seq = 0;
    string exeDir = AppDomain.CurrentDomain.BaseDirectory;
    string iniPath = Path.Combine(exeDir, "stats.ini");

    try {
      ComputeLastShutdown();
      Log("last shutdown: " + lastDownKind + " at " + lastDownAt);
      updatePending = CheckUpdatePending();
      nextUpdatePendingCheckUtc = DateTime.UtcNow.AddSeconds(30);

      try {
        var query = new EventLogQuery("System", PathType.LogName, "*[System[(EventID=1074)]]");
        goingWatcher = new EventLogWatcher(query);
        goingWatcher.EventRecordWritten += OnGoingEventRecordWritten;
        goingWatcher.Enabled = true;
      } catch (Exception ex) {
        Log("going: EventLogWatcher failed: " + ex.Message);
        goingWatcher = null;
      }

      RefreshWolTarget();
      nextMacRefreshUtc = DateTime.UtcNow.AddSeconds(30);

      while (!stop) {
        DateTime t0 = DateTime.UtcNow;

        if (DateTime.UtcNow >= nextMacRefreshUtc) {
          RefreshWolTarget();
          nextMacRefreshUtc = DateTime.UtcNow.AddSeconds(30);
        }

        if (link != null) {
          link.Poll();
          if (link.IsFaulted) {
            Log("port faulted");
            try { link.Close(); } catch (Exception) { }
            link = null;
          }
        }

        if (DateTime.UtcNow >= nextUpdatePendingCheckUtc) {
          updatePending = CheckUpdatePending();
          nextUpdatePendingCheckUtc = DateTime.UtcNow.AddSeconds(30);
        }
        if (DateTime.UtcNow >= nextUpdateRunningCheckUtc) {
          bool running = CheckUpdateRunning();
          if (running != updateRunning) Log("going: update " + (running ? "running" : "no longer running"));
          updateRunning = running;
          nextUpdateRunningCheckUtc = DateTime.UtcNow.AddSeconds(5);
        }

        lock (goingLock) {
          if (goingKind != "none" && (DateTime.UtcNow - goingSetUtc).TotalSeconds >= 120.0) {
            ClearGoing();
            Log("going: cleared after 120 s timeout");
          }
        }

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
            link = TryHid();
          } else if (transportMode == "winusb") {
            link = TryWinUsb();
          } else { // auto: WinUSB, else HID, else serial
            link = TryWinUsb() ?? TryHid();
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

        // Evaluate stats
        DateTime u0 = DateTime.UtcNow;
        var tokens = new List<string>();
        if (rules != null) {
          for (int i = 0; i < rules.Count; i++) {
            StatRule r = rules[i];
            double? val = EvalStat(r);
            tokens.Add(r.Key + "=" + Fmt(val));
          }
        }

        int updateMs = (int)(DateTime.UtcNow - u0).TotalMilliseconds;

        // Evaluate sources
        var texts = new List<KeyValuePair<string, string>>();
        string currentArtId = null;
        if (sources != null) {
          for (int i = 0; i < sources.Count; i++) {
            HttpSource src = sources[i];
            bool ok = false;
            Dictionary<string, double> numCopy = null;
            Dictionary<string, string> textCopy = null;

            lock (src.Lock) {
              if (src.LastSuccessUtc != DateTime.MinValue &&
                  (DateTime.UtcNow - src.LastSuccessUtc).TotalMilliseconds <= (3 * src.IntervalMs)) {
                ok = true;
                numCopy = new Dictionary<string, double>(src.Numbers);
                textCopy = new Dictionary<string, string>(src.Texts);
              }
            }

            tokens.Add(src.Name + ".ok=" + (ok ? "1" : "0"));
            if (ok) {
              foreach (KeyValuePair<string, double> kv in numCopy) {
                if (string.Equals(kv.Key, src.Name + ".ok", StringComparison.OrdinalIgnoreCase)) continue;
                tokens.Add(BoardKey(src.Name, kv.Key) + "=" + Fmt(kv.Value));
              }
              foreach (KeyValuePair<string, string> kv in textCopy) {
                texts.Add(new KeyValuePair<string, string>(BoardKey(src.Name, kv.Key), kv.Value));
              }
              if (src.Name == "media") {
                double aidNum;
                if (numCopy.TryGetValue("media.art_id", out aidNum)) {
                  currentArtId = Fmt(aidNum);
                } else {
                  string aidStr;
                  if (textCopy.TryGetValue("media.art_id", out aidStr)) {
                    currentArtId = aidStr;
                  }
                }
              }
            }
          }
        }

        CheckArtUpdate(currentArtId);

        // Evaluate Claude source
        if (claudeSource != null) {
          claudeSource.GetSnapshot(tokens, texts);
        }

        // WoL target (pc.mac and pc.mac_if)
        if (!string.IsNullOrEmpty(currentMac)) {
          texts.Add(new KeyValuePair<string, string>("pc.mac", currentMac));
          texts.Add(new KeyValuePair<string, string>("pc.mac_if", currentMacIf));
        }

        // Answer to the board's last !hw request
        lock (hwLock) {
          if (hwReply != null) {
            if (DateTime.UtcNow < hwReplyExpiresUtc) texts.Add(new KeyValuePair<string, string>("hw.reply", hwReply));
            else hwReply = null;
          }
        }
        lock (aiLock) {
          if (aiReply != null) {
            if (DateTime.UtcNow < aiReplyExpiresUtc) texts.Add(new KeyValuePair<string, string>("aictl.reply", aiReply));
            else aiReply = null;
          }
        }
        lock (monLock) {
          if (monReply != null) {
            if (DateTime.UtcNow < monReplyExpiresUtc) texts.Add(new KeyValuePair<string, string>("monctl.reply", monReply));
            else monReply = null;
          }
        }
        // heavy exes for the board's night sleep / hibernate hold (Night.cs)
        int heavy = heavyCount;
        if (heavy >= 0) {
          tokens.Add("pc.heavy=" + heavy);
          texts.Add(new KeyValuePair<string, string>("pc.heavy_by", heavyBy));
        }

        // Power ack
        lock (powerLock) {
          if (currentAckValue != null) {
            if (DateTime.UtcNow < ackExpiresUtc) {
              texts.Add(new KeyValuePair<string, string>("power.ack", currentAckValue));
            } else {
              currentAckValue = null;
            }
          }
        }

        // Wire contract: pc.going*, pc.update_pending, pc.update, pc.last_down*
        string curGoingKind, curGoingBy, curGoingWhy;
        long curGoingId;
        lock (goingLock) {
          curGoingKind = goingKind;
          curGoingBy = goingBy;
          curGoingWhy = goingWhy;
          curGoingId = goingId;
        }

        tokens.Add("pc.going_id=" + curGoingId);
        tokens.Add("pc.update_pending=" + (updatePending ? "1" : "0"));
        tokens.Add("pc.update=" + (updateRunning ? "1" : "0"));   // installing right now
        // wall clock for the board (it keeps counting while the PC is off)
        tokens.Add("pc.epoch=" + ((long)(DateTime.UtcNow - new DateTime(1970, 1, 1, 0, 0, 0, DateTimeKind.Utc)).TotalSeconds).ToString(CultureInfo.InvariantCulture));
        tokens.Add("pc.tz_min=" + ((int)TimeZoneInfo.Local.GetUtcOffset(DateTime.Now).TotalMinutes).ToString(CultureInfo.InvariantCulture));

        texts.Add(new KeyValuePair<string, string>("pc.going", curGoingKind));
        texts.Add(new KeyValuePair<string, string>("pc.going_by", curGoingBy));
        texts.Add(new KeyValuePair<string, string>("pc.going_why", curGoingWhy));
        texts.Add(new KeyValuePair<string, string>("pc.last_down", lastDownKind));
        texts.Add(new KeyValuePair<string, string>("pc.last_down_at", lastDownAt));

        // Console mode: print full @s line and @t lines each tick
        if (consoleMode) {
          string fullLine = "@s" + (tokens.Count > 0 ? " " + string.Join(" ", tokens.ToArray()) : "");
          Console.WriteLine("[" + updateMs + " ms] " + fullLine);
          for (int i = 0; i < texts.Count; i++) {
            string val = TruncateUtf8(texts[i].Value, 120);
            Console.WriteLine("  @t " + texts[i].Key + " " + val);
          }
          if (link == null) {
            lock (powerLock) {
              if (scheduledAction != null && scheduledActionTimeUtc == DateTime.MinValue && currentAckValue != null) {
                scheduledActionTimeUtc = DateTime.UtcNow.AddMilliseconds(1500);
              }
            }
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
              string val = TruncateUtf8(texts[i].Value, 120);
              lock (lastSentTextsLock) {
                lastSentTexts[texts[i].Key] = val;
              }
              sb.Append("@t ").Append(texts[i].Key).Append(' ').Append(val).Append('\n');
            }
            sb.Append("@e ").Append(seq++).Append('\n');
            // a whole cover (41 KB of base64) fits one tick: ~0.7 s on the HID link
            int textChars = AppendTextChunk(sb, 12000);
            AppendArtChunk(sb, 48000 - textChars);
            link.Write(sb.ToString());
            lock (powerLock) {
              if (scheduledAction != null && scheduledActionTimeUtc == DateTime.MinValue && currentAckValue != null) {
                scheduledActionTimeUtc = DateTime.UtcNow.AddMilliseconds(1500);
              }
            }
          } catch (Exception e) {
            Log("port lost: " + e.Message);
            try { link.Close(); } catch (Exception) { }
            link = null;
          }
        }

        sendCompleted.Set();

        // Execute scheduled power action ~1.5s after ack was first sent
        string actionToExecute = null;
        lock (powerLock) {
          if (scheduledAction != null && scheduledActionTimeUtc != DateTime.MinValue && DateTime.UtcNow >= scheduledActionTimeUtc) {
            actionToExecute = scheduledAction;
            scheduledAction = null;
            scheduledActionTimeUtc = DateTime.MinValue;
          }
        }
        if (actionToExecute != null) {
          ExecutePowerAction(actionToExecute);
        }

        int wait = intervalMs - (int)(DateTime.UtcNow - t0).TotalMilliseconds;
        if (wait > 0) {
          sendNow.WaitOne(wait);
        } else {
          sendNow.WaitOne(0);
        }
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
      if (goingWatcher != null) {
        try {
          goingWatcher.Enabled = false;
          goingWatcher.Dispose();
        } catch (Exception) { }
        goingWatcher = null;
      }
      Log("stop");
    }
  }
}
