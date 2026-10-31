# Dashboard Layout Engine & JSON Schema

The ESP32 PC Controller features a dynamic dashboard engine (`VIEW_DASH`, name `"dash"`). Instead of hardcoding UI screens in C++, panels display widget trees described in JSON. Layouts can be updated on the fly over USB Serial or Wi-Fi (HTTP PUT) and are saved to flash FAT filesystem (`/ffat/layout.json`) without reflashing firmware.

---

## Architecture

1. **Rendering & Diffing Pipeline**:
   - Drawn on **Core 1** into an `LGFX_Sprite` 240x320 RGB565 canvas.
   - Core 0 flush task performs dirty-band diffing against the sent frame and pushes RGB444 pixels over the 40 MHz SPI bus.
   - **Differential Widget Redraw**: When a view is entered or a layout reloaded, `full = true` clears the canvas to page `bg` and draws all static lines, labels, and widgets. On subsequent frames (`full = false`), each widget compares its current rendered state (formatted string, bar fill width, or sparkline sequence ID) against its cached state. Unchanged widgets cost zero draw cycles; changed widgets clear only their exact bounding box and redraw in place.

2. **Panel Mapping**:
   - Each panel configured in `VIEW_DASH` takes the $k$-th page from the `pages` array (where $k$ is the panel's 0-based rank among panels set to `VIEW_DASH`).
   - If $k$ exceeds the number of pages defined in the layout, the screen displays `"no page <n>"` centered.

3. **Storage & Capacity Limits**:
   - JSON source size: $\le 16\text{ KB}$.
   - Max pages: $\le 6$.
   - Max widgets per page: $\le 32$.
   - Max stat key length: $\le 23\text{ chars}$ (+ null terminator).
   - Max label text length: $\le 40\text{ chars}$ (+ null terminator).
   - Layout structs and sparkline ring buffers are allocated in **PSRAM** (`MALLOC_CAP_SPIRAM`), keeping internal SRAM free. Temporary `cJSON` structures are deleted immediately after parsing.

---

## JSON Format

A layout file consists of a single root object with a `pages` array:

```json
{
  "pages": [
    {
      "bg": "#000000",
      "widgets": [
        {"type":"label", "x":8, "y":6, "font":"head", "color":"#58A6FF", "text":"SYSTEM"},
        {"type":"line",  "x":8, "y":30, "w":224, "color":"#333333"},
        {"type":"value", "x":8, "y":50, "font":"big", "key":"cpu_temp", "fmt":"%.0f\u00b0C",
         "color":"#FFFFFF", "warn":75, "bad":90, "warn_color":"#FFA500", "bad_color":"#FF3030",
         "w":100, "align":"left"},
        {"type":"value", "x":120, "y":50, "font":"big", "key":"cpu_load", "fmt":"%.0f%%",
         "color":"#FFFFFF", "warn":80, "bad":95, "warn_color":"#FFA500", "bad_color":"#FF3030",
         "w":112, "align":"right"},
        {"type":"bar",   "x":8, "y":82, "w":224, "h":8, "key":"cpu_load", "min":0, "max":100,
         "color":"#3FB950", "bg":"#222222", "warn":80, "bad":95},
        {"type":"spark", "x":8, "y":94, "w":224, "h":30, "key":"cpu_load", "min":0, "max":100,
         "samples":60, "color":"#58A6FF"},
        {"type":"text",  "x":8, "y":282, "font":"small", "key":"claude.plan", "w":224, "color":"#58A6FF"}
      ]
    }
  ]
}
```

---

## Widget Specifications

### 1. `label`
Static text rendered once during full page draws.

| Field | Type | Description | Default |
|---|---|---|---|
| `type` | string | `"label"` (required) | — |
| `x`, `y` | integer | Coordinates in pixels | (required) |
| `text` | string | Text string ($\le 40$ chars) | (required) |
| `font` | string | `"small"`, `"body"`, `"head"`, `"big"` | `"small"` |
| `color` | string | Hex color `"#RRGGBB"` | `"#FFFFFF"` |
| `w` | integer | Optional width for alignment | `0` |
| `align` | string | `"left"`, `"right"`, `"center"` | `"left"` |

### 2. `value`
Dynamic numeric stat read from `pcStat(key)`. If value is missing or stale ($\ge 5\text{ s}$), displays `"--"`.

| Field | Type | Description | Default |
|---|---|---|---|
| `type` | string | `"value"` (required) | — |
| `x`, `y` | integer | Coordinates in pixels | (required) |
| `key` | string | Metric key name ($\le 23$ chars) | (required) |
| `font` | string | `"small"`, `"body"`, `"head"`, `"big"` | `"small"` |
| `fmt` | string | `printf` format for one double (e.g. `"%.0f\u00b0C"`) | whole: `"%.0f"`, float: `"%.1f"` |
| `scale` | number | Multiplier applied to stat before formatting | `1.0` |
| `unit` | string | Optional suffix string appended after formatted number | `""` |
| `w` | integer | Bounding box width for clearing and alignment | `TFT_W - x` |
| `align` | string | `"left"`, `"right"`, `"center"` | `"left"` |
| `color` | string | Default text color `"#RRGGBB"` | `"#FFFFFF"` |
| `warn` | number | Warning threshold | `NAN` (off) |
| `bad` | number | Error/critical threshold | `NAN` (off) |
| `warn_color` | string | Text color when threshold reached | `"#FFA500"` |
| `bad_color` | string | Text color when threshold reached | `"#FF3030"` |

*Note on thresholds*:
- If `bad > warn` (e.g. CPU temperature): `val >= bad` uses `bad_color`; `val >= warn` uses `warn_color`.
- If `bad < warn` (e.g. remaining battery or free space): `val <= bad` uses `bad_color`; `val <= warn` uses `warn_color`.

### 3. `text`
Dynamic string stat read from `pcText(key)`. Automatically truncated so its pixel width does not exceed `w`.

| Field | Type | Description | Default |
|---|---|---|---|
| `type` | string | `"text"` (required) | — |
| `x`, `y` | integer | Coordinates in pixels | (required) |
| `key` | string | String metric key ($\le 23$ chars) | (required) |
| `font` | string | `"small"`, `"body"`, `"head"`, `"big"` | `"small"` |
| `w` | integer | Maximum width in pixels to truncate to | `TFT_W - x` |
| `color` | string | Text color `"#RRGGBB"` | `"#FFFFFF"` |

### 4. `bar`
Progress bar gauge scaled between `min` and `max`.

| Field | Type | Description | Default |
|---|---|---|---|
| `type` | string | `"bar"` (required) | — |
| `x`, `y` | integer | Coordinates in pixels | (required) |
| `w`, `h` | integer | Bar dimensions in pixels | (required) |
| `key` | string | Metric key name | (required) |
| `min`, `max` | number | Gauge range | `0.0`, `100.0` |
| `scale` | number | Multiplier applied to stat | `1.0` |
| `color` | string | Fill color `"#RRGGBB"` | `"#3FB950"` (green) |
| `bg` | string | Background track color `"#RRGGBB"` | `"#222222"` |
| `warn`, `bad` | number | Optional threshold limits | `NAN` (off) |
| `warn_color` | string | Fill color when warning threshold exceeded | `"#FFA500"` |
| `bad_color` | string | Fill color when bad threshold exceeded | `"#FF3030"` |

### 5. `spark`
Rolling sparkline polyline. Appends a new sample to a ring buffer whenever `pcStatsSeq()` increments (new sample `@e` from PC).

| Field | Type | Description | Default |
|---|---|---|---|
| `type` | string | `"spark"` (required) | — |
| `x`, `y` | integer | Coordinates in pixels | (required) |
| `w`, `h` | integer | Sparkline dimensions in pixels | (required) |
| `key` | string | Metric key name | (required) |
| `min`, `max` | number | Vertical scale range | `0.0`, `100.0` |
| `samples` | integer | Number of historical samples in ring buffer ($2..240$) | `60` |
| `scale` | number | Multiplier applied to stat | `1.0` |
| `color` | string | Line color `"#RRGGBB"` | `"#58A6FF"` |
| `bg` | string | Optional background fill color | Page `bg` |

### 6. `line`
Static horizontal separator line.

| Field | Type | Description | Default |
|---|---|---|---|
| `type` | string | `"line"` (required) | — |
| `x`, `y` | integer | Coordinates in pixels | (required) |
| `w` | integer | Line length in pixels | `TFT_W - x` |
| `h` | integer | Line thickness in pixels | `1` |
| `color` | string | Line color `"#RRGGBB"` | `"#FFFFFF"` |

---

## Typography & Fonts

Anti-aliased Comic Shanns Mono Nerd Font VLW bitmaps are embedded in flash:

| Font Name | Size | Typical Role |
|---|---|---|
| `"small"` | 12 px | Labels, details, secondary metrics |
| `"body"` | 16 px | Standard metric values, medium labels |
| `"head"` | 20 px | Section titles, panel headers |
| `"big"` | 28 px | Hero metrics (temperature, primary load) |

---

## Available PC Stats Keys

Streamed by the host PC stats service over USB HID / CDC:

| Key | Type | Description | Unit |
|---|---|---|---|
| `uptime` | number | System uptime | seconds |
| `cpu_temp` | number | CPU package temperature | °C |
| `cpu_load` | number | CPU overall load | % |
| `cpu_clock` | number | CPU frequency | MHz |
| `cpu_fan` | number | CPU cooling fan speed | RPM |
| `sys_fan` | number | Chassis fan speed | RPM |
| `ram_used` | number | Physical RAM used | GB |
| `ram_load` | number | Physical RAM load | % |
| `ram_free` | number | Physical RAM free | GB |
| `gpu1_temp`, `gpu1_load` | number | GPU 1 temperature & load | °C, % |
| `gpu1_clock`, `gpu1_fan` | number | GPU 1 clock & fan speed | MHz, RPM |
| `gpu1_vram`, `gpu1_vramt` | number | GPU 1 VRAM used & total | MB |
| `gpu2_temp`, `gpu2_load` | number | GPU 2 temperature & load | °C, % |
| `gpu2_clock`, `gpu2_vram` | number | GPU 2 clock & VRAM used | MHz, MB |
| `igpu_load` | number | Integrated GPU load | % |
| `c_used`, `c_total` | number | C: drive disk space | GB |
| `cpu_power`, `gpu1_power`, `gpu2_power` | number | CPU package, RX 6600 and RTX 3050 power (Display 1's power card) | W |
| `net_down`, `net_up` | number | Network traffic (scale `0.0009765625` to KB/s) | bytes/s |
| `claude.s_pct` | number | Claude session quota consumed | % |
| `claude.w_pct` | number | Claude weekly quota consumed | % |
| `claude.s_reset_at` | text | Claude session reset time | string |
| `claude.w_reset_at` | text | Claude weekly reset time | string |
| `claude.plan` | text | Active AI plan / objective | string |
| `agy.active` | text | Active Antigravity agent name | string |
| `agy2.flash_pct` | number | Antigravity flash quota consumed | % |
| `agy2.flash_reset_at` | text | Antigravity flash quota reset time | string |
| `ai.phase_name`, `ai.guard_name` | text | Local agent phase and guard state | string |

---

## Uploading Layouts

### 1. Using the Upload Script (`scripts/layout_upload.py`)

The upload script validates JSON locally, verifies limits, and sends the layout to the board.

#### Upload over Serial (COM Port):
```bash
python scripts/layout_upload.py firmware/layouts/example.json --port COM12
```

#### Upload over Wi-Fi (HTTP PUT):
```bash
python scripts/layout_upload.py firmware/layouts/example.json --host esp32-pc-control.local
```
*(The `OTA_PASS` password is read automatically from `settings.h` or specified with `--password`)*.

---

### 2. Manual Serial Protocol (`layout begin / data / end`)

Layouts can be sent line-by-line over the UART/USB console:
1. `layout begin`: Allocates upload buffer in PSRAM.
2. `layout data <base64_chunk>`: Appends Base64 chunks ($\le 300\text{ chars}$ per line).
3. `layout end`: Decodes Base64, validates the JSON, writes atomically to `/ffat/layout.tmp`, renames to `/ffat/layout.json`, loads the new layout into PSRAM, marks all panels dirty (`viewsMarkDirty(-1)`), and logs `"layout: N pages loaded"`.

---

### 3. Console Commands

| Command | Description |
|---|---|
| `layout show` | Displays active layout source (`built-in default` or `/ffat/layout.json`) and per-page widget counts |
| `layout reload` | Reloads and recompiles `/ffat/layout.json` from flash |
| `layout default` | Deletes `/ffat/layout.json` and restores the built-in 3-page default layout |
| `view <1\|2\|3> dash` | Switches a panel to display the dashboard engine |
