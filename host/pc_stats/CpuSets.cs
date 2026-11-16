// Efficiency-core CPU sets on a hybrid CPU (Intel 12th gen and later). Steady
// background work (the audio DSP thread) is pinned there so it never takes a
// P-core from a game. Same SYSTEM_CPU_SET_INFORMATION walk as
// PcStats.InitEcoreCounters; the list is read once and cached.

using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

static class CpuSets {
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool GetSystemCpuSetInformation(IntPtr information, uint bufferLength, out uint returnedLength, IntPtr process, uint flags);

  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool SetThreadSelectedCpuSets(IntPtr thread, uint[] cpuSetIds, uint cpuSetIdCount);

  [DllImport("kernel32.dll")]
  static extern IntPtr GetCurrentThread();

  static readonly object gate = new object();
  static bool loaded;
  static uint[] ecoreIds;      // null: not hybrid, or the query failed
  static string ecoreLps = "";
  static string failure;       // why ecoreIds is null when it is not "no hybrid cpu"

  static void Load() {
    lock (gate) {
      if (loaded) return;
      loaded = true;
      var ids = new List<uint>();
      var lps = new List<int>();
      var effs = new List<byte>();
      try {
        uint len = 0;
        GetSystemCpuSetInformation(IntPtr.Zero, 0, out len, IntPtr.Zero, 0);
        if (len == 0) { failure = "cpu set query failed"; return; }
        IntPtr buf = Marshal.AllocHGlobal((int)len);
        try {
          if (!GetSystemCpuSetInformation(buf, len, out len, IntPtr.Zero, 0)) {
            failure = "cpu set query failed (" + Marshal.GetLastWin32Error() + ")";
            return;
          }
          int offset = 0;
          while ((long)offset + 20 <= len) {
            int size = Marshal.ReadInt32(buf, offset);
            int type = Marshal.ReadInt32(buf, offset + 4);
            if (type == 0) {   // CpuSetInformation
              ids.Add((uint)Marshal.ReadInt32(buf, offset + 8));
              lps.Add(Marshal.ReadByte(buf, offset + 14));
              effs.Add(Marshal.ReadByte(buf, offset + 18));
            }
            if (size <= 0) break;
            offset += size;
          }
        } finally {
          Marshal.FreeHGlobal(buf);
        }
      } catch (Exception ex) {
        failure = "cpu set query failed (" + ex.GetType().Name + ")";
        return;
      }

      byte minEff = 255, maxEff = 0;
      for (int i = 0; i < effs.Count; i++) {
        if (effs[i] < minEff) minEff = effs[i];
        if (effs[i] > maxEff) maxEff = effs[i];
      }
      if (ids.Count == 0 || minEff == maxEff) return;   // one core type: nothing to pick

      var pick = new List<uint>();
      var pickLps = new List<int>();
      for (int i = 0; i < ids.Count; i++) {
        if (effs[i] != minEff) continue;
        pick.Add(ids[i]);
        pickLps.Add(lps[i]);
      }
      ecoreIds = pick.ToArray();
      ecoreLps = Ranges(pickLps);
    }
  }

  // 2,3,4,5,10,11 -> "2-5,10-11"
  static string Ranges(List<int> values) {
    values.Sort();
    var sb = new StringBuilder();
    int i = 0;
    while (i < values.Count) {
      int j = i;
      while (j + 1 < values.Count && values[j + 1] == values[j] + 1) j++;
      if (sb.Length > 0) sb.Append(',');
      sb.Append(values[i]);
      if (j > i) sb.Append('-').Append(values[j]);
      i = j + 1;
    }
    return sb.ToString();
  }

  internal static bool PinCurrentThreadToEcores() {
    Load();
    uint[] ids = ecoreIds;
    if (ids == null || ids.Length == 0) return false;
    try { return SetThreadSelectedCpuSets(GetCurrentThread(), ids, (uint)ids.Length); }
    catch { return false; }
  }

  internal static string Describe() {
    Load();
    if (ecoreIds != null) return "e-cores: " + ecoreIds.Length + " cpu sets (lp " + ecoreLps + ")";
    return failure ?? "no hybrid cpu";
  }
}
