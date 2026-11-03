---
name: lvgl-host-review
description: Renders real LVGL pages on the host (no board), exports PNG screenshots, and quantifies pixel differences against design references. Use when verifying LVGL UI against a design spec, checking a layout change, comparing before/after, or debugging why a widget renders wrong.
---

# LVGL Host Render & Design Review

Run the real LVGL code on a PC, capture the framebuffer, and compare it to a
design reference with measured numbers instead of impressions.

## When to use this

- "does this screen match the design", "对比设计稿", "截图看看"
- a widget renders wrong: clipped, invisible, misaligned, wrong colour
- before/after comparison of a UI change
- you need evidence (numbers + images) rather than a description

Do **not** use it to validate behaviour (touch, state machines, network) --
those need `ctest` assertions, not screenshots.

## Quick start

1. Write a page recipe (paths are project-specific, the script is not):

```json
{
  "target": "panel_check",
  "build": {
    "source": "demos/app/tests/lvgl",
    "openvela_root": "/home/user/openvela"
  },
  "pages": [
    { "name": "home",     "env": "APP_CAPTURE" },
    { "name": "settings", "env": "APP_SETTINGS_CAPTURE" }
  ]
}
```

2. Capture:

```bash
python3 scripts/host_capture.py --config pages.json --out /tmp/shots --clean
```

3. Compare against the design PNGs:

```bash
python3 scripts/design_compare.py \
    --host /tmp/shots \
    --design out/design/designs/png \
    --out /tmp/review \
    --pairs pairs.json
```

Both commands print their full argv first, so a path mistake is visible
immediately instead of surfacing as an empty screenshot directory.

`build.source` may be relative, but it resolves against the **config file's
directory**, not your current working directory -- and it must point at the
LVGL test project directory (the one containing `CMakeLists.txt`), not the
skill directory. A config sitting one level deeper needs one more `../`; this
is the single most common setup error.

## This repo already ships a recipe

For the smart_home V9 screens in this contest repo, two files cover it:

```bash
cd .claude/skills/lvgl-host-review
python3 scripts/host_capture.py --config pages.smart_home.json \
    --out /tmp/shots --build-dir /tmp/b
python3 scripts/design_compare.py --host /tmp/shots \
    --design /home/arongw/openvela/out/smart-home-ui-design-v9-card-layout-20261030/designs/png \
    --out /tmp/review --pairs pairs.smart_home.json
```

`pages.smart_home.json` covers home/chat/settings plus all six settings
categories (nine captures, one binary run). `pairs.smart_home.json` maps each
capture to its design page id.

Outputs `compare-<page>.png` (design | host, 1:1) and a region diff table.

## The order that works: look, measure, then count

1. **Look** at the side-by-side first. Most real bugs are obvious at a
   glance: a missing chip, a glyph in the wrong place, text overlapping.
2. **Measure** the thing you think is wrong with `lvmeasure`. Quote the
   numbers (design x/y vs host x/y) -- never "looks off by a bit".
3. **Count** region diff only to confirm or rank, never as the first step.
   A 10% diff in a text region is usually font metrics, not layout.

Diff counts measure *area*, not *displacement*. A solid card shifted 6 px
changes only its two edge strips (~0.3% of a 200x200 region) because the
interior keeps the same colour. Low diff does not mean "aligned" -- it means
"little ink changed". Use the side-by-side or coordinates to judge alignment,
and diff to find *what* changed.

Skipping step 1 and starting from diff percentages leads to chasing
antialiasing. Skipping step 2 leads to vague reports nobody can act on.

## The five mistakes that produce wrong conclusions

All five have actually happened; read them before trusting a measurement.

### 1. SVG `y` is a baseline, LVGL `set_pos` is a top-left corner

A design tool writes `y=100` meaning the text **baseline**. `lv_obj_set_pos(
label, 24, 100)` puts the widget **top** at 100. Comparing them directly
makes a correct layout look 20-30 px too high. It cost a whole review round
once, with a confident but wrong "title sits 30 px too high" conclusion.

Compare rendered pixels instead:

```python
from lvmeasure import load, text_bands
im, px = load("home.png")
print(text_bands(px, 24, 900, 56, 540))   # [(283, 295), (318, 333), ...]
```

### 2. RGB565 quantises colour

`#F8F8F4` renders as `#FFFAF6`; `#ECEFEA` renders as `#EEEEEE`. Exact RGB
equality always fails. `lvmeasure` defaults to a tolerance of 8 and the
distance function is the mean per-channel delta.

When a colour *is* genuinely wrong it is far outside the noise floor: the
dropdown selection once used LVGL theme blue `#2095F6` where the design said
`#E1EDF0` -- a distance over 90, unmistakable.

### 3. A test double must be structurally identical to the real code

A stubbed `qr_encode()` filled a 21x21 matrix at stride 21 while the real
encoder used stride 97 and the UI read at stride 97. The QR code rendered as
five rows of garbage and the screenshot looked like a clipping bug.

Rule: if the double substitutes a buffer producer, it must honour the same
stride, size and sentinel rules. Silently different shapes are worse than no
double at all.

### 4. `lv_image` clips when scaled unless told otherwise

Default `inner_align` is `_LV_IMAGE_ALIGN_AUTO_TRANSFORM`, which draws the
bitmap at its native size, centred, and clips the overflow. Enlarging a 32 px
icon into a 96 px widget shows only the middle 16 px. Fix:

```c
lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
lv_obj_set_size(icon, size, size);
```

Symptom to watch for: the widget geometry is right (`lv_obj_get_coords`
matches), the source data is right, but ink coverage in the region is tiny.

### 5. `cmake --build --target X` leaves every other binary stale

Building one target then running the whole `ctest` suite reports dozens of
failures that have nothing to do with your change. Always full-build first.
`host_capture.py` does this by default and only falls back to per-target
builds with `--allow-partial`, which exists because a baseline can be
uncompilable and you still need screenshots.

## Working with an uncompilable baseline

When the tree you inherited does not build:

1. Build the baseline on its own and record the error.
2. Build only the targets you need (`--allow-partial`).
3. State clearly in the report which failures are pre-existing.

Never let a pre-existing break be reported as a regression, and never "fix"
unrelated code to make the build green -- that hides the real state.

## Reporting

Every review should carry:

| Item | Why |
| --- | --- |
| side-by-side image path | lets the reader judge without rerunning |
| measured coordinates (design vs host, per element) | actionable |
| region diff table | ranks what is left |
| explicitly listed intentional differences | stops future "why is this off" |
| known blockers (pre-existing) | separates your change from the baseline |

Say "differs by N px at (x,y)" not "slightly off". Say "intentional: the
design shows a placeholder QR" not "the QR differs".

## Verifying the skill itself

```bash
python3 scripts/selftest.py      # synthesises images, no project needed
```

Checks the measurement primitives and the CLI error paths. Run it after
changing any script: a broken measurement tool silently produces confident
wrong answers, which is the worst failure mode this skill has.

## References

- `references/host-harness.md` -- minimal harness to add to any test
- `references/pitfalls.md` -- nine real mistakes in detail, with fixes
- `references/measure-cookbook.md` -- copy-paste snippets per question type
