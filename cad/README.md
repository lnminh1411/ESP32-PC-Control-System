# CAD scripts

Three parametric OpenSCAD files, one per enclosure from the original brief.

## Render to .3mf

Requires the [OpenSCAD](https://openscad.org/) CLI (not installed on this machine — install it, then run these from this folder):

```bash
openscad -o part1_esp32_cradle.3mf part1_esp32_cradle.scad
openscad -o part2_display_enclosure.3mf part2_display_enclosure.scad
openscad -o part3_remote_boost_enclosure.3mf part3_remote_boost_enclosure.scad
```

Drop `-o ...3mf` and just run `openscad partN_....scad` to open the interactive preview instead.

## Before printing anything

Every file has a parameter block at the top. Lines commented `VERIFY ON ARRIVAL` are placeholders — not measured, because the part hasn't arrived. See [`../docs/part-fit-research.md`](../docs/part-fit-research.md) for what's confirmed vs. guessed and why.

Once parts are in hand: caliper them, update the flagged variables, re-run the render. Nothing else in these files should need touching — that's the point of keeping them parametric.

| File | Biggest open unknown |
|---|---|
| `part1_esp32_cradle.scad` | `esp32_h` (board + connector stack height) — SuperMini footprint itself (23.2x18.2mm) is confirmed |
| `part2_display_enclosure.scad` | **Fully measured, no open unknowns** — PCB 63x38x13mm, panel 50x36x4mm, all calipered off the real part. One assumption not yet confirmed: which edge the header sits on (inferred from the 13mm vs 2mm size-slack asymmetry, not directly measured). |
| `part3_remote_boost_enclosure.scad` | `fob_pcb_w/h` (no public spec exists) and `mt3608_use_tall_variant` (two variants, 8mm apart in height) |

None of these scripts have been rendered or test-printed — OpenSCAD isn't installed in this environment. Treat them as a structured first draft: sound parametrically, not yet visually verified.
