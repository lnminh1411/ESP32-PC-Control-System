// ============================================================
// Part 1 — ESP32-S3 SuperMini Cradle & Reset Module
// ============================================================
// Two-piece enclosure (base tray + friction-fit lid):
//   - pocket for the SuperMini board
//   - front port cutout for the 20cm Type-C cable's molded collar
//   - internal friction-fit bay for 1x EL817C (DIP-4) + 1x 220R resistor
//   - rear notch to route the 2-pin DuPont wire to motherboard RST header #18
//
// Render:   openscad -o part1_esp32_cradle.3mf part1_esp32_cradle.scad
// Preview:  openscad part1_esp32_cradle.scad
//
// Numbers marked VERIFY ON ARRIVAL are placeholders — the part hasn't been
// calipered yet. Everything downstream is derived from these variables, so
// updating them here is the only edit needed once the real part is in hand.

$fn = 48;

// ---- global clearance ----
tol = 0.25;                    // fit clearance for confirmed dimensions

// ---- ESP32-S3 SuperMini ----
// Confirmed from the candidate listing's dimensioned product photo.
esp32_l = 23.2;
esp32_w = 18.2;
esp32_h = 6;                   // VERIFY ON ARRIVAL — board + USB-C connector + underside stack height

// ---- USB-C cable strain-relief cutout (front face) ----
// Bare plug shell is a fixed 8.4 x 2.56mm (USB-IF spec), but the molded
// overmold/boot around it is NOT standardized. Widened from the original
// 10.5 x 6.5mm brief figure to a safer default.
usb_cutout_w = 12;             // VERIFY ON ARRIVAL — cable overmold width
usb_z_margin = 0.5;            // cutout starts/ends this far from floor/ceiling

// ---- EL817C DIP-4 (standard JEDEC package — confirmed low-risk) ----
el817_l = 6.5;
el817_w = 4.0;
el817_h = 3.0;

// ---- 220 ohm resistor (generic 1/4W axial, through-hole) ----
resistor_l = 7;
resistor_d = 3;

// ---- shell construction ----
wall     = 2;                  // wall / floor thickness
divider  = 1.5;                // wall between ESP32 pocket and component bay
bay_gap  = 1;                  // clearance around each component inside the bay
frame_wt = 0.8;                // wall thickness of each friction-fit component frame

pocket_l = esp32_l + tol;
pocket_w = esp32_w + tol;

bay_l = bay_gap + (el817_l + tol) + bay_gap + (resistor_l + tol) + bay_gap;

cavity_h  = esp32_h + 1;       // headroom above floor for board + wires
shell_h   = wall + cavity_h;   // total side-wall height (open top, lid closes it)
shell_w   = pocket_w + 2*wall;
shell_l   = wall + pocket_l + divider + bay_l + wall;

// ============================================================
module component_frame(l, w, h, wt = frame_wt) {
    // open-top/open-bottom friction ring — component drops in from the top
    // and rests on the tray floor beneath it.
    difference() {
        cube([l, w, h]);
        translate([wt, wt, -1]) cube([l - 2*wt, w - 2*wt, h + 2]);
    }
}

module base_shell() {
    difference() {
        cube([shell_l, shell_w, shell_h]);

        // ESP32 pocket — open through the top
        translate([wall, wall, wall])
            cube([pocket_l, pocket_w, cavity_h + 1]);

        // component bay — open through the top
        translate([wall + pocket_l + divider, wall, wall])
            cube([bay_l, shell_w - 2*wall, cavity_h + 1]);

        // USB-C port cutout, front face (X=0), centered on Y
        translate([-1, (shell_w - usb_cutout_w) / 2, wall + usb_z_margin])
            cube([wall + 2, usb_cutout_w, cavity_h - 2*usb_z_margin]);

        // DuPont wire routing notch, rear face, near the floor
        translate([shell_l - wall - 1, wall + (shell_w - 2*wall)/2 - 2, wall - 0.5])
            cube([wall + 2, 4, 3]);

        // low pass-through gap in the divider (ESP32 GPIO leads -> opto input)
        translate([wall + pocket_l, wall + (shell_w - 2*wall)/2 - 1.5, wall])
            cube([divider, 3, 2.5]);
    }

    // EL817C friction-fit frame
    translate([wall + pocket_l + divider + bay_gap,
               wall + (shell_w - 2*wall - (el817_w + tol)) / 2,
               wall])
        component_frame(el817_l + tol, el817_w + tol, el817_h + tol);

    // resistor friction-fit frame
    translate([wall + pocket_l + divider + bay_gap + (el817_l + tol) + bay_gap,
               wall + (shell_w - 2*wall - (resistor_d + tol)) / 2,
               wall])
        component_frame(resistor_l + tol, resistor_d + tol, resistor_d + tol);
}

module lid() {
    lip = 1;
    lip_h = 3;
    // top plate — overhangs the shell like a cap
    translate([-lip, -lip, lip_h])
        cube([shell_l + 2*lip, shell_w + 2*lip, wall]);
    // friction lip that presses into the shell's open top
    translate([lip, lip, 0])
        cube([shell_l - 2*lip, shell_w - 2*lip, lip_h]);
}

// ---- layout for printing: base + lid side by side ----
base_shell();
translate([0, shell_w + 10, 0]) lid();
