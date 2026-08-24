/**
 * P4X P2 static Smart Home LVGL screen.
 *
 * The view intentionally owns no device model.  Values are fixed visual
 * fixtures so that this phase validates the LVGL-to-/dev/fb0 flush path
 * before introducing cAGENT, networking, persistent state, or touch input.
 */

#include "smart_home_lvgl_static.h"

#include <nuttx/config.h>

#include <lvgl/lvgl.h>

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/boardctl.h>

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

#define STATIC_HOME_LOOP_MAX_DELAY_MS 20u

static lv_obj_t *static_home_label(lv_obj_t *parent, const char *text,
                                   const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static void static_home_style_card(lv_obj_t *card, uint32_t background)
{
    lv_obj_set_style_bg_color(card, lv_color_hex(background), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0xe2e8f0), 0);
    lv_obj_set_style_shadow_width(card, 12, 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_10, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x0f172a), 0);
}

static void static_home_add_metric(lv_obj_t *parent, int x, int width,
                                   const char *symbol, const char *value,
                                   const char *name)
{
    lv_obj_t *metric = lv_obj_create(parent);
    lv_obj_t *label;

    lv_obj_remove_style_all(metric);
    lv_obj_set_pos(metric, x, 0);
    lv_obj_set_size(metric, width, 76);
    lv_obj_set_style_bg_color(metric, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_bg_opa(metric, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(metric, 14, 0);
    lv_obj_set_style_pad_left(metric, 14, 0);
    lv_obj_set_style_pad_top(metric, 10, 0);

    label = static_home_label(metric, symbol, &lv_font_montserrat_20,
                              0x2563eb);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    label = static_home_label(metric, value, &lv_font_montserrat_20,
                              0x0f172a);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, -12, 0);
    label = static_home_label(metric, name, &lv_font_montserrat_14,
                              0x64748b);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, -8);
}

static void static_home_add_device_card(lv_obj_t *parent, int x, int y,
                                        int width, int height,
                                        const char *symbol,
                                        const char *room,
                                        const char *name,
                                        const char *status,
                                        uint32_t accent,
                                        int enabled)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_t *label;

    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    static_home_style_card(card, enabled ? 0xffffff : 0xf8fafc);
    lv_obj_set_style_pad_all(card, 18, 0);

    label = static_home_label(card, symbol, &lv_font_montserrat_20, accent);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    label = static_home_label(card, room, &lv_font_montserrat_14, 0x64748b);
    lv_obj_align(label, LV_ALIGN_TOP_RIGHT, 0, 3);
    label = static_home_label(card, name, &lv_font_montserrat_20, 0x0f172a);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, -28);
    label = static_home_label(card, status, &lv_font_montserrat_14,
                              enabled ? accent : 0x64748b);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void static_home_build_screen(lv_display_t *disp)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *header;
    lv_obj_t *metrics;
    lv_obj_t *nav;
    lv_obj_t *label;
    int width = (int)lv_display_get_horizontal_resolution(disp);
    int height = (int)lv_display_get_vertical_resolution(disp);
    int margin = width >= 720 ? 36 : 16;
    int content_width = width - margin * 2;
    int gap = width >= 720 ? 18 : 10;
    int card_width = (content_width - gap) / 2;
    int card_height = height >= 500 ? 142 : 112;
    int metrics_y = 102;
    int cards_y = metrics_y + 96;

    lv_obj_set_style_bg_color(screen, lv_color_hex(0xf1f5f9), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    header = lv_obj_create(screen);
    lv_obj_remove_style_all(header);
    lv_obj_set_pos(header, margin, 22);
    lv_obj_set_size(header, content_width, 66);
    lv_obj_set_style_bg_color(header, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 18, 0);
    lv_obj_set_style_pad_left(header, 20, 0);
    lv_obj_set_style_pad_right(header, 20, 0);

    label = static_home_label(header, LV_SYMBOL_HOME "  Smart Home",
                              &lv_font_montserrat_20, 0x0f172a);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    label = static_home_label(header, LV_SYMBOL_OK "  LOCAL UI",
                              &lv_font_montserrat_14, 0x16a34a);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, 0, 0);

    metrics = lv_obj_create(screen);
    lv_obj_remove_style_all(metrics);
    lv_obj_set_pos(metrics, margin, metrics_y);
    lv_obj_set_size(metrics, content_width, 76);
    lv_obj_clear_flag(metrics, LV_OBJ_FLAG_SCROLLABLE);
    static_home_add_metric(metrics, 0, (content_width - gap * 2) / 3,
                           LV_SYMBOL_EYE_OPEN, "24 C", "Indoor temperature");
    static_home_add_metric(metrics, (content_width + gap) / 3,
                           (content_width - gap * 2) / 3,
                           LV_SYMBOL_REFRESH, "48%", "Humidity");
    static_home_add_metric(metrics, (content_width + gap) * 2 / 3,
                           (content_width - gap * 2) / 3,
                           LV_SYMBOL_POWER, "3 / 4", "Devices online");

    static_home_add_device_card(screen, margin, cards_y, card_width,
                                card_height, LV_SYMBOL_BULLET,
                                "Living room", "Ceiling light",
                                "ON  •  70%",
                                0xf59e0b, 1);
    static_home_add_device_card(screen, margin + card_width + gap, cards_y,
                                card_width, card_height, LV_SYMBOL_SETTINGS,
                                "Bedroom", "Air conditioner",
                                "COOL  •  24 C",
                                0x2563eb, 1);
    static_home_add_device_card(screen, margin, cards_y + card_height + gap,
                                card_width, card_height, LV_SYMBOL_REFRESH,
                                "Study", "Circulation fan", "OFF  •  Ready",
                                0x64748b, 0);
    static_home_add_device_card(screen, margin + card_width + gap,
                                cards_y + card_height + gap,
                                card_width, card_height, LV_SYMBOL_OK,
                                "Whole home", "Safety status", "All normal",
                                0x16a34a, 1);

    nav = lv_obj_create(screen);
    lv_obj_remove_style_all(nav);
    lv_obj_set_size(nav, content_width, 48);
    lv_obj_align(nav, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_bg_color(nav, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(nav, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(nav, 16, 0);
    label = static_home_label(nav, LV_SYMBOL_HOME "  Home",
                              &lv_font_montserrat_14,
                              0x2563eb);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 28, 0);
    label = static_home_label(nav, LV_SYMBOL_LIST "  Chat",
                              &lv_font_montserrat_14,
                              0x64748b);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    label = static_home_label(nav, LV_SYMBOL_SETTINGS "  Settings",
                              &lv_font_montserrat_14, 0x64748b);
    lv_obj_align(label, LV_ALIGN_RIGHT_MID, -28, 0);
}

int smart_home_lvgl_static_run(void)
{
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;

    if (lv_is_initialized()) {
        printf("[lvgl-static] LVGL is already initialized\n");
        return -1;
    }

#ifdef NEED_BOARDINIT
    (void)boardctl(BOARDIOC_INIT, 0);
#endif

    printf("[lvgl-static] lv_init\n");
    lv_init();
    lv_nuttx_dsc_init(&info);
    info.fb_path = CONFIG_SMART_HOME_DEMO_LVGL_FB_PATH;
    info.input_path = NULL;
    lv_nuttx_init(&info, &result);
    if (!result.disp) {
        printf("[lvgl-static] framebuffer init failed: %s\n", info.fb_path);
        lv_deinit();
        return -1;
    }

    printf("[lvgl-static] framebuffer=%s resolution=%dx%d\n",
           info.fb_path,
           (int)lv_display_get_horizontal_resolution(result.disp),
           (int)lv_display_get_vertical_resolution(result.disp));
    static_home_build_screen(result.disp);
    lv_refr_now(result.disp);
    printf("[lvgl-static] dashboard shown; entering timer loop\n");

    for (;;) {
        uint32_t delay = lv_timer_handler();

        if (delay == 0u || delay > STATIC_HOME_LOOP_MAX_DELAY_MS) {
            delay = STATIC_HOME_LOOP_MAX_DELAY_MS;
        }
        usleep(delay * 1000u);
    }
}
