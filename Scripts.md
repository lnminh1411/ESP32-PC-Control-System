# Scripts Summary

### board_config.py
Shared configuration for the board build scripts including revisions, dates, net classes, and tool paths. Used across the KiCad automation chain. Located at `scripts/board_config.py`.

### make_blank_pcb.py
Creates a blank 2-layer KiCad board file with title block metadata. Used to start the board layout from scratch. Located at `scripts/make_blank_pcb.py`.

### sync_netlist_to_board.py
Imports atopile netlists directly into the KiCad PCB file via the pcbnew API. Assigns footprints and pad nets without requiring the GUI. Located at `scripts/sync_netlist_to_board.py`.

### layout_board.py
Places footprints based on measured geometry, creates board outlines, mounting holes, and silkscreen labels. Must be executed on a blank board. Located at `scripts/layout_board.py`.

### add_pours.py
Pours ground planes on bottom copper and sets antenna keepout zones on all copper layers. Ensures keepouts exist before routing. Located at `scripts/add_pours.py`.

### route_spi.py
Hand-routes critical nets including SPI display daisy chains, control lines, power rails, and optocoupler channels. Minimizes layer transitions and avoids T-junctions. Located at `scripts/route_spi.py`.

### route_board.py
Autoroutes remaining signals using Freerouting via Specctra DSN export and SES import. Serves as a fallback for routes not handled by `route_spi.py`. Located at `scripts/route_board.py`.

### chamfer_corners.py
Replaces 90-degree track bends with 45-degree diagonal chamfers while avoiding copper collisions. Improves signal integrity and manufacturing yield. Located at `scripts/chamfer_corners.py`.

### check_spi_topology.py
Verifies SPI display traces remain on top copper with daisy-chain ordering MCU -> display 1 -> 2 -> 3. Ensures no unwanted vias or T-junctions exist. Located at `scripts/check_spi_topology.py`.

### render_board.py
Generates 2D copper/silkscreen renders and slanted 3D perspective previews of the PCB layout. Outputs rendered images to `pcb/build/renders/`. Located at `scripts/render_board.py`.

### export_gerbers.py
Generates manufacturing Gerber files, Excellon drill files, and job definitions packaged in a ZIP archive. Saves output to the configured artifacts directory. Located at `scripts/export_gerbers.py`.

### make_fit_models.py
Generates VRML 3D fit-check models for the boost converter and microcontroller envelopes. Enables visual clearance checking in 3D renders. Located at `scripts/make_fit_models.py`.

### check_pins.py
Validates firmware GPIO definitions against the atopile hardware pin map. Helps catch pin assignment mismatches before flashing. Located at `scripts/check_pins.py`.

### gif_stream.py
Streams RGB565 GIF frames over USB-UART to the ESP32-S3 for storage in FAT and playback. Validates transmission integrity using per-frame checksums. Located at `scripts/gif_stream.py`.

### idf_flash.py
Builds and flashes `firmware` over the native USB port hands-off: stops the ESP32PcStats service (no UAC prompt when it is already stopped), does a 1200-baud touch on COM12, waits for the ROM downloader port and runs `idf.py flash`. Run it with the ESP-IDF Python (`C:\Espressif\python_env\idf5.5_py3.12_env\Scripts\python.exe scripts\idf_flash.py [--no-build]`). Located at `scripts/idf_flash.py`.

### ota_upload.py
Uploads `firmware/build/carrier.bin` to the board over Wi-Fi (HTTP POST /ota, password from settings.h). Located at `scripts/ota_upload.py`.

### make_vlw.py
Converts a TTF/OTF font into an anti-aliased LovyanGFX VLW font as a C array (`font_*.cpp` in firmware/main). Usage: `python scripts/make_vlw.py FONT.otf SIZE NAME OUT.cpp [--extra U+F2C9]`. Located at `scripts/make_vlw.py`.
### layout_upload.py
Uploads a dashboard layout JSON (format in docs/layouts.md) to the board without reflashing: serial in base64 chunks (`--port COM12`) or HTTP `PUT /layout` (`--host esp32-pc-control.local`, password from settings.h). Usage: `python scripts/layout_upload.py FILE.json --port COM12`; the board rejects invalid JSON and keeps the old layout. Located at `scripts/layout_upload.py`.

