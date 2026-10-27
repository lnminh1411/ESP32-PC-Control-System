// Input idle time, user-session half (session 0 sees no input).
//
// Feed "idle" (@source idle helper:idle), every second, for the board's night
// mode: pc.idle_s = seconds since the last keyboard/mouse input in this
// session (GetLastInputInfo); pc.in_px = mouse travel in px and pc.in_press =
// key and mouse button presses, both counting up (wrapping at 1,000,000), so
// the board can tell a real return from a bumped mouse. Gamepads count too
// (XInput, which Windows' idle time ignores): a button or trigger press is a
// press, a stick past its dead zone 20 px per 100 ms, and either resets
// pc.idle_s. pc.fg_full = 1 while the foreground app runs full screen (a game,
// a presentation; SHQueryUserNotificationState), pc.fg_exe names the
// foreground app; night mode leaves a full-screen app unpinned.
//
// Monitors (DDC/CI, display off) are PcHwCtl's now: `!mon` goes to its
// `display ...` commands (PcStats.cs) and its state comes back as mon.*.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;

static partial class AgyQuota {
  [StructLayout(LayoutKind.Sequential)]
  struct LASTINPUTINFO { public uint cbSize, dwTime; }

  [DllImport("user32.dll")]
  static extern bool GetLastInputInfo(ref LASTINPUTINFO info);

  [DllImport("user32.dll")]
  static extern short GetAsyncKeyState(int vKey);

  [DllImport("user32.dll")]
  static extern bool GetCursorPos(out MonPoint p);

  [StructLayout(LayoutKind.Sequential)]
  struct MonPoint { public int X, Y; }

  [StructLayout(LayoutKind.Sequential)]
  struct XINPUT_GAMEPAD { public ushort wButtons; public byte bLeftTrigger, bRightTrigger; public short sThumbLX, sThumbLY, sThumbRX, sThumbRY; }

  [StructLayout(LayoutKind.Sequential)]
  struct XINPUT_STATE { public uint dwPacketNumber; public XINPUT_GAMEPAD Gamepad; }

  [DllImport("xinput1_4.dll")]
  static extern uint XInputGetState(uint userIndex, out XINPUT_STATE state);

  [DllImport("shell32.dll")]
  static extern int SHQueryUserNotificationState(out int state);

  [DllImport("user32.dll")]
  static extern IntPtr GetForegroundWindow();

  [DllImport("user32.dll")]
  static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);

  static double inPx, inPress;   // IdleLoop's counters, wrap at 1e6
  static int lastPadTick;        // Environment.TickCount of the last gamepad input, 0 = none yet
  static readonly ushort[] padButtons = new ushort[4];
  static readonly bool[] padTrigger = new bool[4], padConnected = { true, true, true, true };

  // One XInput controller: presses, stick travel; true when it saw input.
  // A slot without a controller is slow to ask, so InputCountLoop asks it
  // only every 2 s.
  static bool PollPad(uint i) {
    XINPUT_STATE st;
    padConnected[i] = XInputGetState(i, out st) == 0;
    if (!padConnected[i]) return false;
    bool input = false;
    ushort pressed = (ushort)(st.Gamepad.wButtons & ~padButtons[i]);
    padButtons[i] = st.Gamepad.wButtons;
    for (int b = pressed; b != 0; b &= b - 1) { inPress = (inPress + 1) % 1000000.0; input = true; }
    bool trig = st.Gamepad.bLeftTrigger > 64 || st.Gamepad.bRightTrigger > 64;
    if (trig && !padTrigger[i]) { inPress = (inPress + 1) % 1000000.0; input = true; }
    padTrigger[i] = trig;
    // XINPUT_GAMEPAD_LEFT/RIGHT_THUMB_DEADZONE
    if (Math.Abs((int)st.Gamepad.sThumbLX) > 7849 || Math.Abs((int)st.Gamepad.sThumbLY) > 7849 ||
        Math.Abs((int)st.Gamepad.sThumbRX) > 8689 || Math.Abs((int)st.Gamepad.sThumbRY) > 8689) {
      inPx = (inPx + 20) % 1000000.0;
      input = true;
    }
    return input;
  }

  // Mouse travel and key/button presses, polled every 25 ms (no hooks: a
  // GetAsyncKeyState sweep is cheap and needs no message loop)
  static void InputCountLoop() {
    var down = new bool[256];
    MonPoint last;
    bool havePos = GetCursorPos(out last);
    int tick = 0;
    while (true) {
      MonPoint p;
      if (GetCursorPos(out p)) {
        if (havePos) {
          double dx = p.X - last.X, dy = p.Y - last.Y;
          inPx = (inPx + Math.Sqrt(dx * dx + dy * dy)) % 1000000.0;
        }
        last = p;
        havePos = true;
      }
      for (int vk = 1; vk < 255; vk++) {
        if (vk >= 0x10 && vk <= 0x12) continue;   // Shift/Ctrl/Alt also report as their left/right keys
        bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (d && !down[vk]) inPress = (inPress + 1) % 1000000.0;
        down[vk] = d;
      }
      if (++tick % 4 == 0) {
        bool slow = tick % 80 == 0;
        for (uint i = 0; i < 4; i++) {
          try {
            if ((padConnected[i] || slow) && PollPad(i)) lastPadTick = Environment.TickCount | 1;
          } catch (DllNotFoundException) { }
        }
      }
      Thread.Sleep(25);
    }
  }

  // Seconds since the last input in this session, for the board's night mode
  static void IdleLoop() {
    new Thread(InputCountLoop) { IsBackground = true, Name = "esp32_input" }.Start();
    while (true) {
      var lii = new LASTINPUTINFO { cbSize = (uint)Marshal.SizeOf(typeof(LASTINPUTINFO)) };
      if (GetLastInputInfo(ref lii)) {
        uint ms = (uint)Environment.TickCount - lii.dwTime;   // both wrap at 49.7 days together
        int pad = lastPadTick;
        if (pad != 0) ms = Math.Min(ms, (uint)(Environment.TickCount - pad));
        string fgExe;
        bool full = ForegroundFullScreen(out fgExe);
        SendFeed("idle", new List<string> {
          "pc.idle_s=" + (ms / 1000).ToString(CultureInfo.InvariantCulture),
          "pc.in_px=" + Math.Floor(inPx).ToString(CultureInfo.InvariantCulture),
          "pc.in_press=" + inPress.ToString(CultureInfo.InvariantCulture),
          "pc.fg_full=" + (full ? "1" : "0"),
          "pc.fg_exe=" + fgExe,
        });
      }
      Thread.Sleep(1000);
    }
  }

  // QUNS_BUSY (a full-screen app) or QUNS_RUNNING_D3D_FULL_SCREEN (a game)
  static bool ForegroundFullScreen(out string exe) {
    exe = "";
    try {
      uint pid;
      GetWindowThreadProcessId(GetForegroundWindow(), out pid);
      if (pid != 0) using (Process p = Process.GetProcessById((int)pid)) exe = p.ProcessName.ToLowerInvariant() + ".exe";
    } catch (Exception) { }
    int state;
    return SHQueryUserNotificationState(out state) == 0 && (state == 2 || state == 3);
  }
}
