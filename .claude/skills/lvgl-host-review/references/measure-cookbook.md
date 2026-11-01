# Measurement cookbook

Copy-paste answers to the questions that actually come up.

```python
import sys
sys.path.insert(0, "scripts")
from lvmeasure import (load, hexstr, color_bbox, dark_bbox, text_bands,
                       region_diff, ascii_preview, coverage, near)
```

## "Is the card in the right place?"

```python
im, px = load("home.png")
print(color_bbox(px, im.size, (0xFF, 0xFA, 0xF6), box=(0, 100, 300, 560)))
# -> (24, 136, 208, 384)   compare with design rect
```

**Always pass `box`.** `color_bbox` returns the union of every matching pixel;
a fill reused by the page background or by several cards yields the whole
screen `(0, 0, 1024, 600)`, which reads as a catastrophic layout failure but
only means "this colour is everywhere". Narrow the box until the answer is
specific.

Design rect `(24,136,208,384)` means x=24 y=136 w=208 h=384. A 1 px
difference is the border; more than 2 px is real.

## "Is the text on the right row?"

```python
print(text_bands(px, 24, 900, 56, 540))
# -> [(81, 101), (164, 177), ...] tops of each text line
```

Compare band **tops**, never the design SVG `y` (that is a baseline).

## "Which region is actually wrong?"

```python
d_im, d_px = load("design/09-settings.png")
h_im, h_px = load("host/settings.png")
for label, box in (("top",    (0, 0, 1024, 56)),
                   ("title",  (24, 56, 1000, 60)),
                   ("body",   (24, 116, 976, 400)),
                   ("bottom", (0, 536, 1024, 64))):
    x, y, w, h = box
    print(label, region_diff(d_px, h_px, (x, y, x + w, y + h)))
```

Large `strong` count in one region localises the problem. Similar counts
everywhere means global (font, scale, theme), not layout.

## "Did the widget render at all?"

```python
box = (936, 72, 1000, 136)
print("coverage", coverage(px, im.size, box, (0xEE, 0xEE, 0xEE)))
print(ascii_preview(px, im.size, box))
```

Coverage near 0 -> nothing drawn (hidden, zero size, wrong screen).
Coverage under ~10% with a blob in the centre -> `inner_align` clipping
(pitfall 4).

## "Is this colour the design colour?"

```python
print(hexstr(px[600, 300]))
# compare against the design fill, allow distance <= 8
```

If the distance is 80+, it is a real mismatch -- check whether an LVGL theme
style is winning (pitfall 8).

## "Where exactly is the ink?"

```python
print(dark_bbox(px, im.size, (24, 56, 1000, 140)))
```

Returns `(x, y, w, h)` of everything darker than the threshold -- the union of
all glyphs in the region. Good for "is the title inside the header".

## "Design and host are different sizes"

```python
from PIL import Image
d = Image.open("design.png"); h = Image.open("host.png")
print(d.size, h.size)
```

`lvmeasure` compares within the overlap, but a size mismatch usually means a
different display config or a scaled design export (2x). Check before
measuring anything else; a 2x design will make every number meaningless.

## Building the side-by-side yourself

```python
from PIL import Image
d = Image.open("design.png").convert("RGB")
h = Image.open("host.png").convert("RGB")
w, hh = d.size
out = Image.new("RGB", (w * 2 + 20, hh), "#dfe7e4")
out.paste(d, (0, 0))
out.paste(h, (w + 20, 0))
out.save("compare.png")
```

Crop to the region of interest when the page is large -- a 1024-wide pair is
already 2000 px and detail disappears on a scaled-down view.