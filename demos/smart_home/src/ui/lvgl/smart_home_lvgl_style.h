/**
 * smart_home LVGL UI style resources.
 */

#pragma once

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── V9 light tokens ─────────────────────────────────────── */
#define SMART_HOME_UI_COLOR_PRIMARY        lv_color_hex(0x91BCCB)
#define SMART_HOME_UI_COLOR_PRIMARY_DARK   lv_color_hex(0x314956)
#define SMART_HOME_UI_COLOR_SUCCESS        lv_color_hex(0x6D9681)
#define SMART_HOME_UI_COLOR_WARNING        lv_color_hex(0xD7B58F)
#define SMART_HOME_UI_COLOR_DANGER         lv_color_hex(0xB66F69)
#define SMART_HOME_UI_COLOR_BG             lv_color_hex(0xECEFEA)
#define SMART_HOME_UI_COLOR_SURFACE        lv_color_hex(0xF8F8F4)
#define SMART_HOME_UI_COLOR_SURFACE_SOFT   lv_color_hex(0xF1F3EF)
#define SMART_HOME_UI_COLOR_SURFACE_ON     lv_color_hex(0xE1EDF0)
#define SMART_HOME_UI_COLOR_TEXT_PRIMARY   lv_color_hex(0x314956)
#define SMART_HOME_UI_COLOR_TEXT_SECONDARY lv_color_hex(0x526775)
#define SMART_HOME_UI_COLOR_TEXT_MUTED     lv_color_hex(0x718797)
#define SMART_HOME_UI_COLOR_BORDER         lv_color_hex(0xD8E0DF)
#define SMART_HOME_UI_COLOR_DEVICE_ON_BORDER lv_color_hex(0xB7D2DA)
#define SMART_HOME_UI_COLOR_USER_BUBBLE      lv_color_hex(0xE1EDF0)
#define SMART_HOME_UI_COLOR_TOOL_CARD_BG     lv_color_hex(0xF4EFE6)
#define SMART_HOME_UI_COLOR_ERROR_BUBBLE_BG  lv_color_hex(0xF7E9E7)
#define SMART_HOME_UI_COLOR_NAV_ACTIVE_BG    lv_color_hex(0xE1EDF0)
#define SMART_HOME_UI_COLOR_BTN_SECONDARY    lv_color_hex(0x718797)

typedef struct {
    lv_font_t *font_11;
    lv_font_t *font_12;
    lv_font_t *font_14;
    lv_font_t *font_15;
    lv_font_t *font_16;
    lv_font_t *font_17;
    lv_font_t *font_18;
    lv_font_t *font_20;
    lv_font_t *font_24;
    lv_font_t *font_32;
    lv_font_t *font_50;
} smart_home_lvgl_style_t;

int smart_home_lvgl_style_init(void);
void smart_home_lvgl_style_deinit(void);

const lv_font_t *smart_home_lvgl_font(int size);

void smart_home_lvgl_set_bg(lv_obj_t *obj, lv_color_t color);
void smart_home_lvgl_card_style(lv_obj_t *obj);
void smart_home_lvgl_press_feedback(lv_obj_t *obj);
void smart_home_lvgl_soft_card_style(lv_obj_t *obj);
void smart_home_lvgl_style_keyboard(lv_obj_t *keyboard);

lv_obj_t *smart_home_lvgl_label_create(lv_obj_t *parent,
                                       const char *text,
                                       lv_color_t color,
                                       int size);

/* ── Icon helpers ─────────────────────────────────────── */

/** Create an icon widget from an LVGL symbol or a PNG filename. */
lv_obj_t *smart_home_lvgl_icon_create(lv_obj_t *parent,
                                      const char *filename,
                                      int width,
                                      int height);

/** Create an icon + text label combo (icon above text, for nav bar / cards). */
lv_obj_t *smart_home_lvgl_icon_with_text(lv_obj_t *parent,
                                          const char *filename,
                                          int icon_w,
                                          int icon_h,
                                          const char *text,
                                          lv_color_t text_color,
                                          int text_size);

#ifdef __cplusplus
}
#endif
