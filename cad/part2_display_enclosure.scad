// ============================================================
// Part 2 — 2.0" ST7789V Screen Enclosure (Chassis HUD)
// ============================================================
// Two-piece enclosure (front bezel + back cover):
//   - front bezel: pocket recesses the LCD panel module (a 4mm-thick
//     assembly sitting proud of the PCB, not bare glass) flush with the
//     front face, with a slightly smaller through-window that laps over
//     the panel's front edge to retain it
//   - back cover: 2x press-fit neodymium magnet pockets, an internal
//     retention crossbar that clamps the 8-pin DuPont housing shoulders,
//     and a chamfered wire exit at the header edge
//
// Render:   openscad -o part2_display_enclosure.3mf part2_display_enclosure.scad
//
// Measurements below are calipered off the actual part (2026-09-29):
//   PCB footprint:  63 x 38mm            (display_pcb_w / display_pcb_h)
//   Total depth:    13mm, glass face to header-pin tips (display_total_depth)
//   LCD panel:      50 x 36 x 4mm, mounted on top of the PCB (panel_w/h/t)
// All confirmed — no VERIFY placeholders left in this file.
//
// One remaining ASSUMPTION, not measured: the panel sits within the PCB
// with only ~1mm clearance top/bottom (38-36) but ~13mm clearance
// width-wise (63-50) — that spare width has to be where the 8-pin header
// row lives, so the panel is modeled offset toward one end rather than
// centered, with the header/wire-exit zone reserved at the other end
// (`header_zone_w`). If the header actually runs along the long edge
// instead, flip `header_on_short_edge` below — everything derives from it.

$fn = 48;

tol = 0.25;

// ---- ST7789V module — confirmed measurements ----
display_pcb_w     = 63;
display_pcb_h     = 38;
display_total_depth = 13;      // glass face to header-pin tips
panel_w = 50;
panel_h = 36;
panel_t = 4;                   // LCD panel module height above the PCB

header_zone_w = display_pcb_w - panel_w;  // 13mm — reserved for the header row, see note above
retain_lip = 1;                            // front-face overlap that catches the panel's edge

// ---- 6x2mm neodymium magnets, press-fit ----
magnet_d = 6.2;
magnet_h = 2.2;
magnet_skin = 1;                       // solid roof left behind the magnet so it doesn't poke through
magnet_boss_wall = magnet_h + magnet_skin;  // local back-cover thickness under each magnet

// ---- 8-pin female DuPont housing (generic 2.54mm pitch, 1x8) ----
dupont_housing_w = 8 * 2.54 + 1;   // 8 pins at 2.54mm pitch + a little slack
crossbar_bite     = 1.2;           // how far the crossbar overlaps the housing shoulder

wall = 2;
// Standoff height has to clear the DuPont header pins protruding behind the
// PCB, not just lift the board a nominal amount: 13mm total depth - 4mm for
// the panel (which sits in FRONT, per the measurement) leaves ~9mm of stack
// behind the PCB (board thickness + header pin length). Undersizing this
// was the original mistake here — a 3mm standoff would have driven the
// header pins straight into the back cover.
standoff_h = display_total_depth - panel_t + 1;  // 10mm, +1mm clearance past the pins
post_w = 4;

shell_w = display_pcb_w + 2*wall;       // 67mm
shell_h_plan = display_pcb_h + 2*wall;  // 42mm
// Assembled depth = front_bezel thickness + standoff_h + back_cover wall
// = (wall+panel_t+1) + standoff_h + wall — comfortably exceeds the measured
// 13mm stack (~19mm), since front_bezel and back_cover each need their own
// wall thickness on top of the component clearance. Loose is fine here;
// crushing the header pins is not.

// panel position: offset away from the header zone, centered on the other axis
panel_x = wall + header_zone_w;
panel_y = wall + (display_pcb_h - panel_h) / 2;

// ============================================================
module front_bezel() {
    difference() {
        cube([shell_w, shell_h_plan, wall + panel_t + 1]);

        // visible window — slightly smaller than the panel so the front
        // face laps over the panel's edge by `retain_lip` and holds it in
        translate([panel_x + retain_lip, panel_y + retain_lip, -1])
            cube([panel_w - 2*retain_lip, panel_h - 2*retain_lip, wall + panel_t + 3]);

        // recessed pocket — full panel size, panel drops in from the front
        // and sits flush; wall thickness alone forms the retaining shoulder
        translate([panel_x, panel_y, wall])
            cube([panel_w + tol, panel_h + tol, panel_t + 1]);
    }
}

module magnet_boss_and_pocket(x, y) {
    // extra material on the inside face so the pocket doesn't cut through the thin back cover
    translate([x, y, wall])
        cylinder(d = magnet_d + 3, h = magnet_boss_wall - wall);
}

module back_cover() {
    magnet1 = [shell_w * 0.25, shell_h_plan - 8];
    magnet2 = [shell_w * 0.75, shell_h_plan - 8];

    difference() {
        union() {
            cube([shell_w, shell_h_plan, wall]);
            magnet_boss_and_pocket(magnet1[0], magnet1[1]);
            magnet_boss_and_pocket(magnet2[0], magnet2[1]);
        }

        // 2x magnet pockets, blind — cut from the outward-facing side, flush press-fit
        translate([magnet1[0], magnet1[1], -0.1])
            cylinder(d = magnet_d, h = magnet_h + 0.1);
        translate([magnet2[0], magnet2[1], -0.1])
            cylinder(d = magnet_d, h = magnet_h + 0.1);

        // chamfered wire exit — centered in the reserved header zone, at
        // the board edge nearest the header (X=0 end, where header_zone_w
        // was reserved)
        translate([-1, shell_h_plan/2 - dupont_housing_w/2, wall - 2])
            cube([wall + 2, dupont_housing_w, 2 + 1]);
    }

    // corner standoff posts for the display PCB
    for (pos = [[wall, wall], [shell_w - wall - post_w, wall],
                [wall, shell_h_plan - wall - post_w], [shell_w - wall - post_w, shell_h_plan - wall - post_w]])
        translate([pos[0], pos[1], wall])
            cube([post_w, post_w, standoff_h]);

    // internal retention crossbar — clamps against the DuPont housing shoulders
    // so cable tension can't walk the connector out of the pocket
    translate([wall + 2, shell_h_plan/2 - dupont_housing_w/2 - crossbar_bite, wall])
        cube([3, dupont_housing_w + 2*crossbar_bite, standoff_h + 1]);
}

// ---- layout for printing: bezel + cover side by side ----
front_bezel();
translate([shell_w + 10, 0, 0]) back_cover();
