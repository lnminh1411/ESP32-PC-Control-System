// Music-reactive border: captures what the PC plays (WASAPI loopback on the
// default render device, hand-written COM interop, no NuGet), turns it into
// 6 bands 30 times a second and hands each frame to the service as a pipe
// line "V <seq><6 lengths><6 volumes>\n" (2 + 12 + 12 lowercase hex digits).
// The service forwards it to the board as HID report 7.
//
// Bands: 20-60, 60-250, 250-1000, 1000-4000, 4000-8000, 8000-16000 Hz, each
// weighed on one shared scale: band power per octave (pink noise reads
// level; music, falling faster than pink, leans to the low bands).
// Length = the band's amplitude times a mild bass-first weight, relative to
// the loudest band, so a faint band takes little of the ring. Volume = the
// band's level against the song's own loudness (songRef: the loudest band
// level heard, falling 1 dB/s), over a 24 dB range; Windows' volume plays no
// part. Both enveloped (instant attack, release 150/110/80 ms).
// Silence (under -60 dBFS for 300 ms, or no packets for 200 ms) sends one
// all-zero frame and then nothing until the sound passes -50 dBFS.
//
// Runs in the user-session half (AgyQuota.cs) on thread esp32_audio, pinned
// to the E-cores (CpuSets.cs). RunTest runs the same capture and DSP alone
// from the console, printing levels instead of sending them.

using System;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Threading;

static class Audio {
  // ---- COM interop: vtables declared in order, only up to the last method used ----
  [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
  internal class MMDeviceEnumeratorCom { }

  [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  internal interface IMMDeviceEnumerator {
    [PreserveSig] int EnumAudioEndpoints(int dataFlow, int stateMask, out IntPtr devices);
    [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IntPtr endpoint);
  }

  [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  internal interface IMMDevice {
    [PreserveSig] int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    [PreserveSig] int OpenPropertyStore(int access, out IntPtr properties);
    [PreserveSig] int GetId(out IntPtr id);
  }

  [ComImport, Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  internal interface IAudioClient {
    [PreserveSig] int Initialize(int shareMode, int streamFlags, long bufferDuration, long periodicity, IntPtr format, IntPtr sessionGuid);
    [PreserveSig] int GetBufferSize(out uint frames);
    [PreserveSig] int GetStreamLatency(out long latency);
    [PreserveSig] int GetCurrentPadding(out uint frames);
    [PreserveSig] int IsFormatSupported(int shareMode, IntPtr format, out IntPtr closest);
    [PreserveSig] int GetMixFormat(out IntPtr format);
    [PreserveSig] int GetDevicePeriod(out long defaultPeriod, out long minimumPeriod);
    [PreserveSig] int Start();
    [PreserveSig] int Stop();
    [PreserveSig] int Reset();
    [PreserveSig] int SetEventHandle(IntPtr eventHandle);
    [PreserveSig] int GetService(ref Guid iid, [MarshalAs(UnmanagedType.IUnknown)] out object service);
  }

  [ComImport, Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
  internal interface IAudioCaptureClient {
    [PreserveSig] int GetBuffer(out IntPtr data, out uint frames, out uint flags, out ulong devicePosition, out ulong qpcPosition);
    [PreserveSig] int ReleaseBuffer(uint frames);
    [PreserveSig] int GetNextPacketSize(out uint frames);
  }

  [DllImport("avrt.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  static extern IntPtr AvSetMmThreadCharacteristicsW(string taskName, ref uint taskIndex);

  const int eRender = 0, eConsole = 0;
  const int CLSCTX_ALL = 0x17;
  const int AUDCLNT_SHAREMODE_SHARED = 0;
  const int AUDCLNT_STREAMFLAGS_LOOPBACK = 0x00020000;
  const uint AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY = 0x1, AUDCLNT_BUFFERFLAGS_SILENT = 0x2;
  const int AUDCLNT_S_BUFFER_EMPTY = 0x08890001;
  const int AUDCLNT_E_DEVICE_INVALIDATED = unchecked((int)0x88890004);
  const int E_NOTFOUND = unchecked((int)0x80070490);
  static readonly Guid IID_IAudioClient = new Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2");
  static readonly Guid IID_IAudioCaptureClient = new Guid("C8ADBD64-E71E-48A0-A4DE-185C395CD317");
  static readonly Guid SubtypeFloat = new Guid("00000003-0000-0010-8000-00aa00389b71");
  static readonly Guid SubtypePcm = new Guid("00000001-0000-0010-8000-00aa00389b71");

  static readonly double[] BandLo = { 20, 60, 250, 1000, 4000, 8000 };
  static readonly double[] BandHi = { 60, 250, 1000, 4000, 8000, 16000 };
  // length weight: lower bands count a little more (user, 2026-11-16)
  static readonly double[] LenWeight = { 1.25, 1.15, 1.05, 1.0, 0.95, 0.9 };
  // band volume 0..1 = band dB over the VolRangeDb under the song reference
  // (FFT units at N 2048: a full-scale sine is about +56 dB). The reference
  // jumps to the loudest band, falls RefFallDb a second, never under
  // RefFloorDb (so near-silence is not blown up to full brightness).
  const double VolRangeDb = 24, RefFallDb = 1, RefFloorDb = 10;
  // a band this far under the song reference gets no length
  const double LenGateDb = 48;
  static readonly double[] ReleaseTau = { 0.150, 0.150, 0.110, 0.110, 0.080, 0.080 };

  static int started;
  static volatile bool testMode;   // --audio-test: notes go to the console, not the log

  static void Note(string message) {
    if (testMode) { try { Console.WriteLine("audio: " + message); } catch { } }
    else AgyQuota.LogLine("audio: " + message);
  }

  static void Check(int hr, string what) {
    if (hr < 0) throw new COMException(what + " failed", hr);
  }

  static void Release(object o) {
    if (o == null) return;
    try { Marshal.ReleaseComObject(o); } catch { }
  }

  // E-cores and the MMCSS "Audio" class: steady, never on a game's P-core
  static void PrepareThread() {
    bool pinned = false;
    try { pinned = CpuSets.PinCurrentThreadToEcores(); } catch { }
    bool mmcss = false;
    try { uint idx = 0; mmcss = AvSetMmThreadCharacteristicsW("Audio", ref idx) != IntPtr.Zero; } catch { }
    Note((pinned ? "pinned to " : "not pinned, ") + CpuSets.Describe() + (mmcss ? ", mmcss Audio" : ", no mmcss"));
  }

  internal static void Start(Action<string> sendLine) {
    if (Interlocked.Exchange(ref started, 1) != 0) return;
    Thread t = new Thread(delegate() {
      PrepareThread();
      Engine e = new Engine(sendLine);
      while (true) {
        try { e.Run(() => true); }
        catch (Exception ex) {   // Run catches its own; this is the last net, the half must not die
          try { e.Fail(ex); e.Close(); } catch { }
          Thread.Sleep(1000);
        }
      }
    }) { IsBackground = true, Name = "esp32_audio", Priority = ThreadPriority.AboveNormal };
    t.SetApartmentState(ApartmentState.MTA);
    t.Start();
  }

  internal static void RunTest(int seconds) {
    AgyQuota.UseParentConsole();
    testMode = true;
    if (seconds < 1) seconds = 10;
    Engine e = null;
    long t0 = 0, t1 = 0;
    Thread t = new Thread(delegate() {
      PrepareThread();
      e = new Engine(null);
      e.report = PrintLevels;
      t0 = Environment.TickCount64;
      long end = t0 + seconds * 1000L;
      try { e.Run(() => Environment.TickCount64 < end); }
      catch (Exception ex) { Note(ex.GetType().Name + ": " + ex.Message); }
      t1 = Environment.TickCount64;
    }) { Name = "esp32_audio_test", Priority = ThreadPriority.AboveNormal };
    t.SetApartmentState(ApartmentState.MTA);
    t.Start();
    t.Join();

    double secs = Math.Max(0.001, (t1 - t0) / 1000.0);
    var inv = CultureInfo.InvariantCulture;
    Console.WriteLine("frames " + e.frames + " in " + secs.ToString("0.0", inv) + " s = " +
        (e.frames / secs).ToString("0.0", inv) + " frames/s");
    Console.WriteLine("silence enter " + e.silenceEnters + ", exit " + e.silenceExits);
    Console.WriteLine("capture " + e.formatText + (e.fftSize > 0 ? ", fft " + e.fftSize + ", hop " + e.hopSize : ""));
    Console.WriteLine(CpuSets.Describe());
  }

  static void PrintLevels(Engine e) {
    var sb = new System.Text.StringBuilder();
    var inv = CultureInfo.InvariantCulture;
    sb.Append("len");
    for (int b = 0; b < 6; b++) sb.Append(' ').Append(e.lengths[b].ToString("D3", inv));
    sb.Append(" | vol");
    for (int b = 0; b < 6; b++) sb.Append(' ').Append(e.vols[b].ToString("D3", inv));
    sb.Append(" | dB");
    for (int b = 0; b < 6; b++) sb.Append(' ').Append(e.bandDb[b].ToString("0", inv));
    sb.Append(" | ref ").Append(e.songRef.ToString("0.0", inv));
    sb.Append(" silent=").Append(e.silent ? 1 : 0);
    sb.Append(" rms=").Append(e.rmsDb.ToString("0.0", CultureInfo.InvariantCulture)).Append(" dBFS");
    try { Console.WriteLine(sb.ToString()); } catch { }
  }

  sealed class Engine {
    readonly Action<string> send;   // null in the test: frames are only counted
    internal Action<Engine> report;  // test: called every 200 ms

    // ---- capture ----
    IMMDeviceEnumerator enumerator;
    IMMDevice device;
    IAudioClient client;
    IAudioCaptureClient capture;
    string deviceId;
    bool noDeviceLogged;
    long retryAtMs;
    int rate, channels, bits, blockAlign;
    bool isFloat;
    internal string formatText = "none";
    float[] fbuf = new float[0];
    short[] sbuf = new short[0];
    int[] ibuf = new int[0];
    byte[] bbuf = new byte[0];

    // ---- DSP ----
    int dspRate;
    internal int fftSize, hopSize;
    int n, half, hop;
    double dt;
    float[] ring;
    int ringPos, fresh;
    double sumSq;
    double[] window, cosT, sinT, re, im;
    int[] rev;
    readonly int[] kLo = new int[6], kHi = new int[6];
    readonly double[] envLen = new double[6], envVol = new double[6], decay = new double[6], tiltDb = new double[6];
    internal readonly double[] bandDb = new double[6];   // per octave, N-normalised
    internal readonly byte[] lengths = new byte[6], vols = new byte[6];
    internal double songRef;
    bool reseed;
    internal bool silent;
    double quietSec;
    internal double rmsDb = -120;
    long lastPacketMs, lastFeedMs;

    // ---- output ----
    byte seq;
    internal long frames, silenceEnters, silenceExits;
    readonly char[] line = new char[29];
    const string Hex = "0123456789abcdef";

    // rate-limited error log: the same failure once a minute, with a repeat count
    string lastErr;
    long lastErrMs;
    int errRepeats;

    internal Engine(Action<string> send) { this.send = send; }

    internal void Run(Func<bool> keepGoing) {
      long nextCheckMs = 0, nextReportMs = 0;
      while (keepGoing()) {
        long now = Environment.TickCount64;
        try {
          if (client == null && now >= retryAtMs) {
            if (Open()) nextCheckMs = now + 2000;
            else { retryAtMs = now + 2000; GoQuiet(); }
          }
          if (client != null) {
            Pump(now);
            if (now >= nextCheckMs) {
              nextCheckMs = now + 2000;
              if (DefaultChanged()) { Note("default device changed, reopening"); Close(); }
            }
          }
        } catch (Exception ex) {
          Fail(ex);
          Close();
          retryAtMs = now + 1000;
          GoQuiet();
        }
        if (report != null && now >= nextReportMs) { nextReportMs = now + 200; report(this); }
        Thread.Sleep(10);
      }
      Close();
    }

    internal void Fail(Exception ex) {
      int hr = ex is COMException ? ex.HResult : 0;
      string msg = hr == AUDCLNT_E_DEVICE_INVALIDATED ? "device invalidated, reopening"
          : ex.GetType().Name + (hr != 0 ? " 0x" + hr.ToString("x8") : "") + ": " + ex.Message;
      if (hr != AUDCLNT_E_DEVICE_INVALIDATED) { Release(enumerator); enumerator = null; }   // start over clean
      long now = Environment.TickCount64;
      if (msg == lastErr && now - lastErrMs < 60000) { errRepeats++; return; }
      Note(errRepeats > 0 ? msg + " (previous error repeated " + errRepeats + "x)" : msg);
      lastErr = msg;
      lastErrMs = now;
      errRepeats = 0;
    }

    bool Open() {
      if (enumerator == null) enumerator = (IMMDeviceEnumerator)new MMDeviceEnumeratorCom();
      int hr;
      IMMDevice dev = DefaultDevice(out hr);
      if (hr == E_NOTFOUND || (hr >= 0 && dev == null)) {
        if (!noDeviceLogged) { noDeviceLogged = true; Note("no render device, retrying every 2 s"); }
        return false;
      }
      Check(hr, "GetDefaultAudioEndpoint");
      noDeviceLogged = false;
      device = dev;
      deviceId = GetId(dev);

      Guid iid = IID_IAudioClient;
      object o;
      Check(device.Activate(ref iid, CLSCTX_ALL, IntPtr.Zero, out o), "Activate");
      client = (IAudioClient)o;

      IntPtr fmt;
      Check(client.GetMixFormat(out fmt), "GetMixFormat");
      try {
        ParseFormat(fmt);
        Check(client.Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0, fmt, IntPtr.Zero), "Initialize");
      } finally {
        Marshal.FreeCoTaskMem(fmt);
      }

      Guid iidCap = IID_IAudioCaptureClient;
      Check(client.GetService(ref iidCap, out o), "GetService");
      capture = (IAudioCaptureClient)o;
      Check(client.Start(), "Start");

      SetupDsp(rate);
      ResetRing();
      lastPacketMs = lastFeedMs = Environment.TickCount64;
      Note("capturing " + formatText);
      return true;
    }

    internal void Close() {
      if (client != null) { try { client.Stop(); } catch { } }
      Release(capture); capture = null;
      Release(client); client = null;
      Release(device); device = null;
      deviceId = null;
    }

    static string GetId(IMMDevice dev) {
      IntPtr s;
      Check(dev.GetId(out s), "GetId");
      try { return Marshal.PtrToStringUni(s); }
      finally { Marshal.FreeCoTaskMem(s); }
    }

    // The default render endpoint as a raw pointer, then the interface: a
    // failed QueryInterface says which IID and HRESULT instead of a bare cast error
    IMMDevice DefaultDevice(out int hr) {
      IntPtr p;
      hr = enumerator.GetDefaultAudioEndpoint(eRender, eConsole, out p);
      if (hr < 0 || p == IntPtr.Zero) return null;
      try {
        Guid iid = typeof(IMMDevice).GUID;
        IntPtr q;
        int qhr = Marshal.QueryInterface(p, ref iid, out q);
        if (qhr < 0) throw new COMException("IMMDevice QueryInterface " + iid + " failed", qhr);
        Marshal.Release(q);
        return (IMMDevice)Marshal.GetObjectForIUnknown(p);
      } finally {
        Marshal.Release(p);
      }
    }

    bool DefaultChanged() {
      int hr;
      IMMDevice dev = DefaultDevice(out hr);
      if (hr < 0 || dev == null) return true;   // gone: Open logs "no render device"
      try { return GetId(dev) != deviceId; }
      finally { Release(dev); }
    }

    // WAVEFORMATEX(+EXTENSIBLE): tag @0, channels @2, rate @4, blockAlign @12,
    // bits @14, cbSize @16, SubFormat GUID @24
    void ParseFormat(IntPtr p) {
      int tag = (ushort)Marshal.ReadInt16(p, 0);
      int ch = (ushort)Marshal.ReadInt16(p, 2);
      int sr = Marshal.ReadInt32(p, 4);
      int align = (ushort)Marshal.ReadInt16(p, 12);
      int bps = (ushort)Marshal.ReadInt16(p, 14);
      int cb = (ushort)Marshal.ReadInt16(p, 16);
      Guid sub = Guid.Empty;
      if (tag == 0xFFFE && cb >= 22) {
        byte[] g = new byte[16];
        Marshal.Copy(IntPtr.Add(p, 24), g, 0, 16);
        sub = new Guid(g);
      }
      bool flt = tag == 3 || sub == SubtypeFloat;
      bool pcm = tag == 1 || sub == SubtypePcm;
      bool ok = ch >= 1 && sr >= 8000 && align == ch * bps / 8 &&
          ((flt && bps == 32) || (pcm && (bps == 16 || bps == 24 || bps == 32)));
      if (!ok) throw new NotSupportedException("mix format tag 0x" + tag.ToString("x") + " sub " + sub + " " + bps + " bit " + ch + " ch");
      rate = sr; channels = ch; bits = bps; blockAlign = align; isFloat = flt;
      formatText = sr + " Hz, " + ch + " ch, " + (flt ? "float" : "pcm") + bps;
    }

    void Pump(long now) {
      uint packet;
      Check(capture.GetNextPacketSize(out packet), "GetNextPacketSize");
      bool got = false;
      while (packet > 0) {
        IntPtr data;
        uint count, flags;
        ulong devPos, qpcPos;
        int hr = capture.GetBuffer(out data, out count, out flags, out devPos, out qpcPos);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
        Check(hr, "GetBuffer");
        if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) ResetRing();
        if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == IntPtr.Zero) FeedZeros((int)count);
        else Feed(data, (int)count);
        Check(capture.ReleaseBuffer(count), "ReleaseBuffer");
        got = true;
        Check(capture.GetNextPacketSize(out packet), "GetNextPacketSize");
      }

      if (got) { lastPacketMs = lastFeedMs = now; return; }
      if (silent) return;
      // loopback delivers nothing while nothing plays: our own clock takes over
      if (now - lastPacketMs >= 200) EnterSilence();
      else if (now - lastFeedMs >= 33) { lastFeedMs += 33; FeedZeros(hop); }
    }

    // GoQuiet: no device or a failure; the border must not freeze lit
    void GoQuiet() {
      if (!silent) EnterSilence();
    }

    void Feed(IntPtr data, int count) {
      int samples = count * channels;
      float inv = 1f / channels;
      if (isFloat) {
        if (fbuf.Length < samples) fbuf = new float[samples];
        Marshal.Copy(data, fbuf, 0, samples);
        for (int f = 0, i = 0; f < count; f++) {
          float sum = 0;
          for (int c = 0; c < channels; c++) sum += fbuf[i++];
          Push(sum * inv);
        }
      } else if (bits == 16) {
        if (sbuf.Length < samples) sbuf = new short[samples];
        Marshal.Copy(data, sbuf, 0, samples);
        for (int f = 0, i = 0; f < count; f++) {
          float sum = 0;
          for (int c = 0; c < channels; c++) sum += sbuf[i++] / 32768f;
          Push(sum * inv);
        }
      } else if (bits == 32) {
        if (ibuf.Length < samples) ibuf = new int[samples];
        Marshal.Copy(data, ibuf, 0, samples);
        for (int f = 0, i = 0; f < count; f++) {
          float sum = 0;
          for (int c = 0; c < channels; c++) sum += (float)(ibuf[i++] / 2147483648.0);
          Push(sum * inv);
        }
      } else {   // 24-bit packed
        int bytes = samples * 3;
        if (bbuf.Length < bytes) bbuf = new byte[bytes];
        Marshal.Copy(data, bbuf, 0, bytes);
        for (int f = 0, o = 0; f < count; f++) {
          float sum = 0;
          for (int c = 0; c < channels; c++, o += 3) {
            int v = (bbuf[o] << 8 | bbuf[o + 1] << 16 | bbuf[o + 2] << 24) >> 8;
            sum += v / 8388608f;
          }
          Push(sum * inv);
        }
      }
    }

    void FeedZeros(int count) {
      for (int i = 0; i < count; i++) Push(0f);
    }

    void Push(float x) {
      ring[ringPos] = x;
      if (++ringPos == n) ringPos = 0;
      sumSq += x * x;
      if (++fresh >= hop) Hop();
    }

    void ResetRing() {
      if (ring != null) Array.Clear(ring, 0, ring.Length);
      ringPos = 0;
      fresh = 0;
      sumSq = 0;
    }

    // ~23 Hz bins or finer: 2048 at 44.1/48 kHz, 4096 at 96 kHz
    void SetupDsp(int sampleRate) {
      if (sampleRate == dspRate) return;
      dspRate = sampleRate;
      n = 2048;
      while ((double)sampleRate / n > 24.0 && n < 16384) n <<= 1;
      half = n / 2;
      hop = Math.Max(1, (int)Math.Round(sampleRate / 30.0));
      dt = (double)hop / sampleRate;
      fftSize = n;
      hopSize = hop;

      ring = new float[n];
      re = new double[n];
      im = new double[n];
      window = new double[n];
      for (int i = 0; i < n; i++) window[i] = 0.5 - 0.5 * Math.Cos(2 * Math.PI * i / n);
      cosT = new double[half];
      sinT = new double[half];
      for (int k = 0; k < half; k++) {
        cosT[k] = Math.Cos(2 * Math.PI * k / n);
        sinT[k] = -Math.Sin(2 * Math.PI * k / n);
      }
      int log2 = 0;
      while ((1 << log2) < n) log2++;
      rev = new int[n];
      for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < log2; b++) if ((i & (1 << b)) != 0) r |= 1 << (log2 - 1 - b);
        rev[i] = r;
      }
      for (int b = 0; b < 6; b++) {
        kLo[b] = Math.Max(1, (int)Math.Ceiling(BandLo[b] * n / sampleRate));
        kHi[b] = Math.Min(half, (int)Math.Ceiling(BandHi[b] * n / sampleRate));
        decay[b] = Math.Exp(-dt / ReleaseTau[b]);
        tiltDb[b] = -10 * Math.Log10(Math.Log(BandHi[b] / BandLo[b], 2));   // power per octave
        envLen[b] = envVol[b] = 0;
      }
      songRef = RefFloorDb;
      silent = true;   // nothing goes out until the sound passes -50 dBFS
      quietSec = 0;
    }

    void Hop() {
      double ms = sumSq / fresh;
      fresh = 0;
      sumSq = 0;
      rmsDb = 10 * Math.Log10(ms + 1e-20);
      if (silent) {
        if (rmsDb <= -50) return;
        silent = false;
        silenceExits++;
        quietSec = 0;
      } else {
        quietSec = rmsDb < -60 ? quietSec + dt : 0;
        if (quietSec >= 0.3) { EnterSilence(); return; }
      }
      Analyse();
      Emit();
    }

    void EnterSilence() {
      silent = true;
      silenceEnters++;
      quietSec = 0;
      for (int b = 0; b < 6; b++) { envLen[b] = envVol[b] = 0; lengths[b] = vols[b] = 0; }
      reseed = true;   // the next sound may be another song: judge it by itself
      Emit();
    }

    static byte ToByte(double v) {
      int x = (int)Math.Round(v * 255);
      return (byte)(x > 255 ? 255 : x < 0 ? 0 : x);
    }

    void Analyse() {
      int idx = ringPos;   // oldest sample
      for (int i = 0; i < n; i++) {
        re[i] = ring[idx] * window[i];
        im[i] = 0;
        if (++idx == n) idx = 0;
      }
      Fft();

      // one scale for all bands: power per octave at N 2048 whatever the FFT size
      double norm = 20 * Math.Log10(n / 2048.0);
      double[] amp = new double[6];
      double maxAmp = 0, loudest = -1e9;
      for (int b = 0; b < 6; b++) {
        double p = 0;
        for (int k = kLo[b]; k < kHi[b]; k++) p += re[k] * re[k] + im[k] * im[k];
        double db = 10 * Math.Log10(p + 1e-12) - norm + tiltDb[b];
        bandDb[b] = db;
        if (db > loudest) loudest = db;
      }
      // the song's loudness: frozen while silent (Analyse does not run),
      // started over from the first frame after silence
      if (reseed) { songRef = loudest; reseed = false; }
      else songRef -= RefFallDb * dt;
      if (loudest > songRef) songRef = loudest;
      if (songRef < RefFloorDb) songRef = RefFloorDb;
      // a band far under the song has no length: the zero window before
      // silence is detected (all bands at the epsilon) must not split the ring evenly
      for (int b = 0; b < 6; b++) {
        amp[b] = bandDb[b] < songRef - LenGateDb ? 0 : Math.Pow(10, bandDb[b] / 20) * LenWeight[b];
        if (amp[b] > maxAmp) maxAmp = amp[b];
      }
      for (int b = 0; b < 6; b++) {
        double len = maxAmp > 0 ? amp[b] / maxAmp : 0;
        double vol = (bandDb[b] - songRef + VolRangeDb) / VolRangeDb;
        if (vol < 0) vol = 0; else if (vol > 1) vol = 1;
        double e = envLen[b] * decay[b];
        envLen[b] = len > e ? len : e;
        e = envVol[b] * decay[b];
        envVol[b] = vol > e ? vol : e;
        lengths[b] = ToByte(envLen[b]);
        vols[b] = ToByte(envVol[b]);
      }
    }

    // in-place iterative radix-2 FFT over re/im
    void Fft() {
      for (int i = 0; i < n; i++) {
        int j = rev[i];
        if (j > i) {
          double t = re[i]; re[i] = re[j]; re[j] = t;
          t = im[i]; im[i] = im[j]; im[j] = t;
        }
      }
      for (int size = 2; size <= n; size <<= 1) {
        int hs = size >> 1, step = n / size;
        for (int i = 0; i < n; i += size) {
          for (int j = 0, k = 0; j < hs; j++, k += step) {
            int a = i + j, b = a + hs;
            double tr = re[b] * cosT[k] - im[b] * sinT[k];
            double ti = re[b] * sinT[k] + im[b] * cosT[k];
            re[b] = re[a] - tr; im[b] = im[a] - ti;
            re[a] += tr; im[a] += ti;
          }
        }
      }
    }

    void Emit() {
      line[0] = 'V';
      line[1] = ' ';
      line[2] = Hex[seq >> 4];
      line[3] = Hex[seq & 15];
      for (int b = 0; b < 6; b++) {
        line[4 + b * 2] = Hex[lengths[b] >> 4];
        line[5 + b * 2] = Hex[lengths[b] & 15];
        line[16 + b * 2] = Hex[vols[b] >> 4];
        line[17 + b * 2] = Hex[vols[b] & 15];
      }
      line[28] = '\n';
      seq++;
      frames++;
      if (send != null) send(new string(line));
    }
  }
}
