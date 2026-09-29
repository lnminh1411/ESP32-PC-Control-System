# Part Fit Research — Pre-Arrival Dimension Check

Parts not in hand yet. Goal here: catch places where Gemini's CAD brief assumed a number that real-world parts don't reliably match, before any parametric script gets fixed dimensions baked in.

## Summary table

| Part | Brief assumed | Real-world typical | Verdict |
|---|---|---|---|
| ESP32-S3 SuperMini | 22.5 x 18 mm | **23.2 x 18.2 mm**, confirmed from the actual listing's own dimensioned product photo (matches the generic 23.5x18mm figure found from other sellers within margin of error) | Off by ~1mm on length. Use 23.2 x 18.2mm as pocket basis — this is the specific unit likely to be bought. |
| ST7789V 2.0" display module | Not dimensioned in brief (only bezel lip + magnet pockets specified) | **Calipered off the arrived part (2026-09-29): PCB 63 x 38mm, total depth 13mm glass-to-header-tip. LCD panel module (sits proud of the PCB, not bare glass) 50 x 36 x 4mm.** Landscape orientation — the original 39x58mm portrait placeholder was wrong on both axes. | **Resolved.** `part2_display_enclosure.scad` updated to these exact numbers. Panel sits offset toward one end (63-50=13mm slack width-wise vs. only 38-36=2mm height-wise — the header row eats the 13mm end); confirm that assumption once the header's physical edge is visible. Also caught a derived bug while updating: standoff height under the back cover needs to clear ~9mm of header-pin protrusion behind the PCB (13mm total − 4mm panel), not the 3mm originally guessed — fixed. |
| EL817C (DIP-4) | Not dimensioned (just "a slot") | Standard DIP-4 body **~6.5 x 4.0 x 3.0mm**, 2.54mm pin pitch, 7.62mm row spacing (0.3" — confirmed via creepage-distance spec). Candidate listing (Vanxy-branded) photo shows standard DIP-4 shape, no deviation. | Standard JEDEC package, low risk. Safe to use these numbers even pre-arrival. |
| MT3608 boost module | Not dimensioned (just "a mounting base") | **Two very different variants**: trimpot version 36 x 17 x 14mm (tall, has adjustment pot sticking up), micro-USB slim version 30 x 17 x 6mm | **Biggest risk in the whole BOM.** 14mm vs 6mm height is more than double — Chamber B's ceiling height depends entirely on which variant ships. Do not fix this dimension. |
| USB Type-E internal header / Type-C adapter | Front cutout 10.5 x 6.5mm for cable's "molded collar" | Bare USB-C plug shell: 8.4 x 2.56mm (fixed by USB-IF spec). Molded overmold/boot: unstandardized, commonly 10-13mm wide x 5-8mm tall depending on cable brand; USB-IF's *locking*-connector spec caps overmold at 12.35 x 6.5mm max but that's not binding on a normal cable | Brief's 10.5 x 6.5mm sits inside the typical range but not guaranteed for the specific 20cm Type-C cable on hand. Some cables run wider. |
| RF key fob (4-button, decased) | Not dimensioned | **No public spec exists.** These are generic OEM boards sold under dozens of brand names; frequency/chip is documented, PCB outline is not. | Highest uncertainty item. Cannot be researched further online — must be calipered on arrival. |

## What this changes about the CAD approach

The brief's `tol = 0.25mm` global clearance is meant for manufacturing slop on a *known* dimension. It does nothing for the SuperMini's 1mm error or the MT3608's 8mm height swing — those are wrong-assumption errors, not tolerance errors. Two different problems, two different fixes:

1. **Known-good dimensions** (EL817C DIP-4, USB-C bare plug shell, SuperMini footprint pin spacing, and now the SuperMini outline at 23.2 x 18.2mm from the actual candidate listing) — safe to hardcode as named constants now. Low regret if wrong.
2. **Unverified/high-variance dimensions** (display PCB outline, MT3608 body, fob PCB, cable overmold) — keep these as script-top parameters with generous default clearance (2-3mm, not 0.25mm) and a comment marking them `# VERIFY ON ARRIVAL`. Do not cut tight pockets against these until calipered.

### Specific per-part fixes to the original brief

- **Part 1 (ESP32 cradle):** bump pocket parameters from 22.5 x 18mm to 23.2 x 18.2mm + tol (matches the candidate listing's dimensioned photo).
- **Part 2 (display enclosure):** done — now built to the calipered 63 x 38 x 13mm PCB/depth and 50 x 36 x 4mm panel, landscape orientation, standoff height corrected to actually clear the header pins.
- **Part 3 (door remote + boost enclosure):** Chamber B (MT3608) needs two standoff height options in the script (6mm and 14mm variant), selected by a boolean parameter, rather than one fixed ceiling — or just build Chamber B's lid removable/adjustable until the actual module is in hand. Chamber A (fob PCB) should use adjustable card-edge rails (a slot with a range, not a snug pocket) since no dimension exists to design against.
- **USB port cutout:** widen from 10.5 x 6.5mm to 12 x 7mm as a safer default given overmold isn't standardized, or make the cutout a through-slot open on one side so an oversized cable boot can still seat even if the pocket depth guess is off.

## Recommended workflow once parts arrive

1. Caliper each part, fill in a small `measurements.json` (or just update the parameter block at the top of each script).
2. Re-run the parametric script — nothing else should need touching if the scripts stayed parametric per above.
3. Print a single test coupon per uncertain pocket (just the pocket geometry, not the full shell) before committing a full enclosure print.
