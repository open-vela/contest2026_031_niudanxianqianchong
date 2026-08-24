# GT911 Probe

`gt911_probe [seconds]` reads the P4X GT911 touchscreen endpoint at
`/dev/input0` and prints raw touchscreen upper-half events.  The default
capture window is 30 seconds; the accepted range is 1 to 600 seconds.

This is the P3.1 hardware proof before attaching `/dev/input0` to the static
LVGL Smart Home page.  A successful run shows the GT911 product ID during
board bring-up, then one or more `DOWN`, `MOVE`, and `UP` samples with
coordinates in the 1024x600 panel space.
