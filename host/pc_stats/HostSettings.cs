// The PC helper's settings: the HOST_* keys of firmware/main/settings.h
// (defaults in settings_or_example.h), as constants in Settings.g.cs, which
// build.cmd generates with gen_settings.ps1.
using System;

static partial class HostSettings {
  // "a, b,c" -> {"a", "b", "c"}; empty entries dropped
  public static string[] List(string commaList) {
    string[] parts = commaList.Split(',');
    var keep = new System.Collections.Generic.List<string>();
    foreach (string p in parts) {
      string t = p.Trim();
      if (t.Length > 0) keep.Add(t);
    }
    return keep.ToArray();
  }
}
