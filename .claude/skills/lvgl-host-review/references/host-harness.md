# Minimal LVGL host harness

Everything the capture flow needs lives in the test binary. There is no
display, no driver, no board -- LVGL renders into a `uint16_t` array that you
read back as a PPM.

## 1. Framebuffer and flush

```c
#include <lvgl/lvgl.h>
#include <stdio.h>

static uint16_t pixels[WIDTH * HEIGHT];

/* DIRECT mode renders straight into `pixels`; the flush callback only has to
 * acknowledge. Without lv_display_flush_ready() LVGL waits forever for the
 * previous frame and the screenshot comes out empty or half drawn.
 */
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area;
    (void)data;
    lv_display_flush_ready(display);
}

static void display_init(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels),
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
}
```

Why `DIRECT` with a full-screen buffer: partial and double-buffer modes leave
the array holding only the last flushed strips, so a single read at the end
gives you a patchwork. With DIRECT the array is the frame.

## 2. Capture one screen

```c
static void capture_screen(lv_obj_t *screen, const char *env)
{
    const char *path = getenv(env);

    if (!path || !screen) return;               /* opt-in via env var */
    lv_screen_load(screen);
    lv_refr_now(NULL);                          /* force a full render */

    FILE *file = fopen(path, "wb");
    if (!file) return;
    fprintf(file, "P6\n%d %d\n255\n", WIDTH, HEIGHT);
    for (size_t i = 0; i < sizeof(pixels) / sizeof(pixels[0]); i++) {
        unsigned p = pixels[i];
        uint8_t rgb[3] = {
            ((p >> 11) & 31) * 255 / 31,        /* R: 5 bits */
            ((p >>  5) & 63) * 255 / 63,        /* G: 6 bits */
            ((p      ) & 31) * 255 / 31,        /* B: 5 bits */
        };
        fwrite(rgb, 1, 3, file);
    }
    fclose(file);
}
```

Two things that matter:

- `lv_refr_now(NULL)` -- without it, a screen that was just built may not have
  been rendered yet and you capture the previous page.
- The 5/6/5 expansion is why the PNG shows `#FFFAF6` where the design says
  `#F8F8F4`. That is expected, not a bug. See `pitfalls.md` #2.

## 3. Make capture opt-in

```c
#define CAPTURE(screen, env) capture_screen((screen), (env))
```

Gate every export behind an environment variable so the normal `ctest` run
does not write files:

```c
capture_screen(ui->screen_home, "APP_CAPTURE");
capture_screen(ui->screen_settings, "APP_SETTINGS_CAPTURE");
```

One binary, many pages -- the driver script sets all the variables and runs
the binary once.

## 4. Keep the test deterministic

Screenshots must not depend on the clock, network or real data:

- Fix the clock: call the page builder after `set_clock(fixed_time)`.
- Feed fixed fixtures (device lists, weather) from static arrays.
- Avoid animations: they capture at whatever frame happened to be current.

If the page reads live state, provide a stub that returns constants. A
screenshot that changes between runs is worse than no screenshot.

## 5. Wire it into CMake

```cmake
add_executable(panel_check test_panel.c ${SOURCES})
target_include_directories(panel_check PRIVATE ${LVGL_INCLUDE} ${APP_INCLUDE})
target_link_libraries(panel_check PRIVATE lvgl m pthread)
add_test(NAME panel_check COMMAND panel_check)
```

Then `host_capture.py --config` only needs the build directory, the target
name and the env-var map.

## 6. When the page lives in another translation unit

Some pages are built by `static` functions. Either:

- `#include "the_page.c"` in the test (what smart_home does -- fast, but the
  test must compile the page's dependencies too), or
- expose a non-static builder.

The `#include .c` trick is fine for review harnesses; it is not fine for
tests that need to mock the page's dependencies, because the include pulls in
the real ones.
