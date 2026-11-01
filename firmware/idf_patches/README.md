# ESP-IDF patches

Local fixes to ESP-IDF v5.5.6 (`C:\Espressif\frameworks\esp-idf-v5.5.6`), as
`git diff` from the ESP-IDF root. A reinstall or upgrade of ESP-IDF drops
them: apply them again, unless the new version already has the fix.

| Patch | Why |
|---|---|
| `spi-bus-lock-toctou-18527.patch` | Upstream fix a16c003 for [esp-idf#18527](https://github.com/espressif/esp-idf/issues/18527): the SPI master ISR read `lock->acquiring_dev` twice and could load through NULL when the other core cleared it in between. It crashed the board on 2026-11-01 (task main, `spi_bus_lock_bg_exit`). On master, not yet in v5.5.6. |
| `xtensa-zol-lcount-a00bb2d.patch` | Upstream a00bb2d1cb (master, not v5.5/v6.0): the interrupt and exception entry saves LCOUNT and zeroes it, so handler code that passes LEND does not jump back to LBEG of an interrupted zero-overhead loop. Hardening; no crash of ours traced to it. |
| `tinyusb-rx-pending-3937.patch` | Not ESP-IDF: TinyUSB [PR 3937](https://github.com/hathach/tinyusb/pull/3937) for [tinyusb#1292](https://github.com/hathach/tinyusb/issues/1292), applied in `firmware/vendor/esp_tinyusb/tinyusb/src` (`patch -p1`). An OUT endpoint stays held (RX_PENDING) until the class copied its buffer, so a reader on the other core can't re-arm a transfer into it. esp_tinyusb 3.0.0 lacks it; `main/idf_component.yml` points at the local copy (`override_path`). Drop the copy once a release has it. |
| `ldgen-dedupe-libs.patch` | `tools/ldgen/ldgen.py` drops duplicate library lines (MINIMAL_BUILD, 2026-10-27). |

```bash
git -C /c/Espressif/frameworks/esp-idf-v5.5.6 apply "E:/ESP32 PC Controller/firmware/idf_patches/spi-bus-lock-toctou-18527.patch"
```

`git -C <idf> apply --check -R <patch>` succeeds when a patch is already in.
