# Pitfalls, with real cases

Each entry: symptom, cause, fix. All of these produced a wrong conclusion at
least once before the cause was found.

## 1. "The layout sits 30 px too high" (it did not)

**Symptom.** Design spec says a title at `y=100`; the rendered title looked
far lower; the first measurement concluded "the whole page is shifted 30 px".

**Cause.** The SVG value was a **text baseline**. LVGL positions a widget by
its **top-left corner**. For a 24 px font the baseline sits roughly 20 px
below the top, so the two numbers are not comparable at all. A whole review
round was spent on a non-existent offset; re-measuring rendered pixels showed
the title was within 1 px of the design.

**Fix.** Never compare a design coordinate to a widget coordinate. Render,
then measure ink:

```python
print(text_bands(px, 24, 900, 56, 540))
# design reference measured the same way -> compare band tops
```

For a widget you control, `lv_obj_get_coords()` gives the truth; for text,
the ink band does.

## 2. Colour "mismatch" that is just RGB565

**Symptom.** Every card reported `#FFFAF6` against a design of `#F8F8F4`.
A naive check calls all of them wrong.

**Cause.** The panel is RGB565: 5 bits red, 6 green, 5 blue. `0xF8` (248)
becomes `0xFF` (255) after 5-bit quantisation and 8-bit expansion. The same
happens to `0xEC` -> `0xEE`.

**Fix.** Use a tolerance. `lvmeasure.DEFAULT_COLOR_TOL = 8` covers the noise;
genuine mistakes are an order of magnitude larger (LVGL theme blue `#2095F6`
vs design `#E1EDF0` = distance 90+).

## 3. A test double with a different memory layout

**Symptom.** A QR code rendered as five horizontal stripes inside a correct
white card. Looked exactly like a clipping bug.

**Cause.** The stub encoder filled a `21x21` matrix using stride 21. The real
encoder uses stride `SMART_HOME_MILOCO_QR_MAX_MODULES` (97), and the UI reads
with that stride. Rows 0-20 landed correctly; everything after read past the
end of the written area as zero.

**Fix.** Delete the stub and link the real implementation when the double
would have to mimic buffer geometry:

```cmake
add_executable(panel_check test.c ${UI}/widget.c ${SRC}/qr_encode.c
               ${SRC}/third_party/qrcodegen/qrcodegen.c)
```

Rule of thumb: stub *behaviour* (network, clocks, device state), link real
code when the thing under test is geometry or encoding.

## 4. Only the centre of an image renders

**Symptom.** A 96x96 icon widget reported correct coordinates, correct source
bitmap (96x96, 16384 bytes, correct header), yet only a ~16x16 patch of ink
appeared. Even the ASCII preview showed a blob in the middle.

**Cause.** `lv_image` defaults to `inner_align =
_LV_IMAGE_ALIGN_AUTO_TRANSFORM`. When the source and the widget differ, LVGL
draws the bitmap at its native size, centres it, and clips the rest.

**Fix.**

```c
lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
```

**Diagnostic.** Compare widget coords with ink coverage:

```python
from lvmeasure import coverage, ascii_preview
print(ascii_preview(px, im.size, (936, 72, 1000, 136)))
print(coverage(px, im.size, (936, 72, 1000, 136), (0xEE, 0xEE, 0xEE)))
```

Geometry right + coverage under ~5% + ink clustered in the centre = this bug.

## 5. Building one target then running the whole suite

**Symptom.** After `cmake --build DIR --target panel_check`, `ctest` reported
10 of 13 tests failing. Nothing about the change could plausibly break them.

**Cause.** `--target` builds *only* that target. Every other test binary was
stale or never built.

**Fix.** Always `cmake --build DIR` (no `--target`) before `ctest`.
`host_capture.py` builds everything first; `--allow-partial` exists only for
the case where the baseline itself is broken.

## 6. Blaming your change for a pre-existing break

**Symptom.** A branch failed to compile on symbols like
`SMART_HOME_BOARD_CAMERA_KEY`.

**Cause.** An earlier commit on the main branch referenced symbols that were
never committed. The baseline was already red.

**Fix.** Build the baseline alone before judging. Report pre-existing
breakage separately and never "fix" unrelated code to get a green build --
the next person needs to know the tree was already broken.

## 7. `pad_all` plus absolute coordinates

**Symptom.** Every child of a modal was offset by exactly the panel padding
(24 px), making the layout look uniformly wrong.

**Cause.** The panel was created with `lv_obj_set_style_pad_all(panel, 24, 0)`
and children positioned with coordinates meant to be absolute.

**Fix.** Pick one model: either no padding and absolute child coordinates
(`pad_all = 0`, subtract the panel origin yourself), or padding and
padding-relative coordinates. Mixing them offsets everything by the padding.

## 8. LVGL theme styles win over a single selector

**Symptom.** Setting the dropdown selection colour to a custom value had no
effect; the selection stayed LVGL theme blue.

**Cause.** The default theme registers styles on
`LV_PART_SELECTED | LV_STATE_CHECKED`, which is *more specific* than plain
`LV_PART_SELECTED`. Setting only the latter loses.

**Fix.** Override both, and kill the gradient explicitly:

```c
lv_obj_set_style_bg_color(list, colour, LV_PART_SELECTED);
lv_obj_set_style_bg_color(list, colour, LV_PART_SELECTED | LV_STATE_CHECKED);
lv_obj_set_style_bg_grad_dir(list, LV_GRAD_DIR_NONE, LV_PART_SELECTED);
lv_obj_set_style_bg_grad_dir(list, LV_GRAD_DIR_NONE,
                              LV_PART_SELECTED | LV_STATE_CHECKED);
```

**Diagnostic.** Sample the rendered pixel: theme blue is `#2095F6`, hard to
miss against a pale design.

## 9. A generated asset that silently changed format

**Symptom.** An icon generator run changed three unrelated 48 px assets from
A8 to ARGB8888 -- 4x the flash, invisible in the diff summary.

**Cause.** A "keep V9-sourced icons in colour" rule was written too broadly
and caught every size.

**Fix.** Scope format rules to the exact (icon, size) pair, then diff the
generated directory after every run:

```bash
git status --short path/to/generated/
```

Unexpected modifications to files you did not intend to touch are a signal,
not noise.