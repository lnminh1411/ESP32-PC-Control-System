// ============================================================
// Part 3 — Rolling Door Remote + Boost Converter Enclosure
// ============================================================
// Single base shell, three zones along X:
//   Chamber A — card-edge slide rails for the decased key-fob PCB, plus a
//               tray of 4x EL817C friction frames (wired across the fob's
//               UP/DOWN/STOP/LOCK button pads)
//   Divider   — 1.5mm solid wall, isolates the RF trace antenna in Chamber A
//               from the boost converter's switching-inductor noise
//   Chamber B — standoff posts for the MT3608 boost module
//
// Render:   openscad -o part3_remote_boost_enclosure.3mf part3_remote_boost_enclosure.scad
//
// The fob PCB has NO public dimension anywhere (generic OEM board, sold under
// many brand names) — Chamber A uses adjustable slide rails rather than a
// snug pocket so it isn't hostage to a guessed number. The MT3608 has two
// common variants 8mm apart in height; mt3608_use_tall_variant picks which
// one this run is built for.

$fn = 48;

tol = 0.25;

// ---- decased 4-button RF key fob PCB ----
// VERIFY ON ARRIVAL — no manufacturer spec exists for this part; these are
// rough placeholders sized for a typical small remote. Chamber A is designed
// as adjustable rails (see rail_gap below) specifically so this guess matters less.
fob_pcb_w = 40;
fob_pcb_h = 70;
fob_pcb_t = 1.6;
rail_gap  = 4;              // extra width margin the rails allow beyond fob_pcb_w — widen if real board is bigger

// ---- MT3608 boost converter module ----
// Two variants seen in the field, 8mm apart in height. Pick one; re-run the
// script if the wrong variant shows up.
mt3608_use_tall_variant = true;   // true = 36x17x14mm (trimpot module), false = 30x17x6mm (slim USB-input module)
mt3608_l = mt3608_use_tall_variant ? 36 : 30;
mt3608_w = 17;
mt3608_h = mt3608_use_tall_variant ? 14 : 6;

// ---- EL817C DIP-4 (standard JEDEC package — confirmed low-risk) ----
el817_l = 6.5;
el817_w = 4.0;
el817_h = 3.0;
el817_count = 4;             // UP / DOWN / STOP / LOCK

wall      = 2;
divider   = 1.5;             // RF / thermal isolation wall, per brief
bay_gap   = 1;
frame_wt  = 0.8;

// ---- Chamber A: fob rails ----
rail_w    = 2;                                  // width of each guide rail
rail_gap_total = fob_pcb_w + tol + rail_gap;     // channel width the rails define
chA_slot_l = fob_pcb_h + tol + 4;                // room to slide the board in lengthwise
chA_w      = rail_gap_total + 2*rail_w;

// ---- optocoupler tray (4x EL817C, in a row, same chamber as the fob) ----
// each frame's footprint is el817_l (X) by el817_w (Y); laid out in a row along X
opto_tray_l = el817_count * (el817_l + tol + bay_gap) + bay_gap;
opto_tray_w = el817_w + tol + 2*bay_gap;

chA_total_l = chA_slot_l + opto_tray_l;

// ---- Chamber B: MT3608 mount ----
post_w = 4;
chB_l  = mt3608_l + 2*tol + 2*wall;
chB_w  = mt3608_w + 2*tol + 2*wall;

shell_l = wall + chA_total_l + divider + chB_l + wall;
shell_w = max(chA_w, chB_w) + 2*wall;
cavity_h = max(fob_pcb_t + 6, mt3608_h + 1) + wall;   // tallest internal requirement plus floor

// ============================================================
module component_frame(l, w, h, wt = frame_wt) {
    difference() {
        cube([l, w, h]);
        translate([wt, wt, -1]) cube([l - 2*wt, w - 2*wt, h + 2]);
    }
}

module fob_rails(x0, y0) {
    // two facing L-rails the fob PCB's long edges slide into, like a card-edge guide
    translate([x0, y0, wall])
        cube([chA_slot_l, rail_w, fob_pcb_t + tol + 2]);
    translate([x0, y0 + rail_w + rail_gap_total, wall])
        cube([chA_slot_l, rail_w, fob_pcb_t + tol + 2]);
}

module opto_tray(x0, y0) {
    for (i = [0 : el817_count - 1])
        translate([x0 + bay_gap + i * (el817_l + tol + bay_gap), y0, wall])
            component_frame(el817_l + tol, el817_w + tol, el817_h + tol);
}

module base_shell() {
    difference() {
        cube([shell_l, shell_w, cavity_h + wall]);

        // Chamber A open cavity (rails + optocoupler tray both live in this open pocket)
        translate([wall, wall, wall])
            cube([chA_total_l, shell_w - 2*wall, cavity_h + 1]);

        // Chamber B open cavity
        translate([wall + chA_total_l + divider, wall, wall])
            cube([chB_l - 2*wall, shell_w - 2*wall, cavity_h + 1]);

        // wire pass-through low in the divider (button leads -> optocouplers already
        // inside chamber A; this notch is for the 12V feed from Chamber B to the fob pads)
        translate([wall + chA_total_l, wall + (shell_w - 2*wall)/2 - 2, wall])
            cube([divider, 4, 3]);
    }

    // fob card-edge rails, centered in chamber A's first section
    fob_rails(wall + 2, wall + (shell_w - 2*wall - rail_gap_total)/2 - rail_w);

    // optocoupler tray, right after the fob slot
    opto_tray(wall + chA_slot_l, wall + (shell_w - 2*wall - opto_tray_w)/2 + bay_gap);

    // MT3608 standoff posts (4 corners), height sized to the selected variant's
    // clearance — module rests on top, secured by its own mounting holes / zip tie
    for (pos = [[0,0], [mt3608_l + 2*tol - post_w, 0],
                [0, mt3608_w + 2*tol - post_w], [mt3608_l + 2*tol - post_w, mt3608_w + 2*tol - post_w]])
        translate([wall + chA_total_l + divider + wall + pos[0],
                    wall + (shell_w - 2*wall - (mt3608_w + 2*tol))/2 + pos[1],
                    wall])
            cube([post_w, post_w, 3]);
}

base_shell();
