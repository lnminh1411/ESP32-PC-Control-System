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
