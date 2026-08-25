/**
 * smart_home LVGL UI style resources.
 */

#include "smart_home_lvgl_style.h"
#include "icons/smart_home_lvgl_icons.h"

#include <nuttx/config.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef CONFIG_SMART_HOME_DEMO_DATA_ROOT
#define CONFIG_SMART_HOME_DEMO_DATA_ROOT "/data"
#endif

#define SMART_HOME_FONT_ROOT CONFIG_SMART_HOME_DEMO_DATA_ROOT "/res/fonts"
#define SMART_HOME_FONT_NORMAL SMART_HOME_FONT_ROOT "/MiSans-Normal.ttf"

#ifndef CONFIG_SMART_HOME_DEMO_UI_LVGL_ICONS
#define CONFIG_SMART_HOME_DEMO_UI_LVGL_ICONS \
    CONFIG_SMART_HOME_DEMO_DATA_ROOT "/res/icons"
#endif

#define SMART_HOME_ICONS_ROOT CONFIG_SMART_HOME_DEMO_UI_LVGL_ICONS
#define SMART_HOME_ICONS_FALLBACK_ROOT \
    CONFIG_SMART_HOME_DEMO_DATA_ROOT "/res/res/icons"

/* LVGL filesystem drive letter prefix (matches CONFIG_LV_FS_POSIX_LETTER) */
#define SMART_HOME_LV_FS_PREFIX "A:"

static smart_home_lvgl_style_t g_style;

static lv_font_t *load_font(int size)
{
#ifdef CONFIG_LV_USE_FREETYPE
    return lv_freetype_font_create(SMART_HOME_FONT_NORMAL,
                                   LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                   size,
                                   LV_FREETYPE_FONT_STYLE_NORMAL);
#elif defined(CONFIG_LV_USE_TINY_TTF) && \
      defined(CONFIG_LV_TINY_TTF_FILE_SUPPORT)
    /* TinyTTF streams the subset font from LittleFS and avoids an external
     * FreeType package dependency in the P4X Route-A build. */
    return lv_tiny_ttf_create_file(SMART_HOME_FONT_NORMAL, size);
#else
    (void)size;
    return NULL;
#endif
}

int smart_home_lvgl_style_init(void)
{
    g_style.font_12 = load_font(12);
    g_style.font_14 = load_font(14);
    g_style.font_16 = load_font(16);
    g_style.font_20 = load_font(20);
    printf("[smart_home_lvgl] font path=%s font12=%p font14=%p font16=%p font20=%p\n",
           SMART_HOME_FONT_NORMAL,
           g_style.font_12,
           g_style.font_14,
           g_style.font_16,
           g_style.font_20);
    return 0;
}

void smart_home_lvgl_style_deinit(void)
{
#ifdef CONFIG_LV_USE_FREETYPE
    if (g_style.font_12) {
        lv_freetype_font_delete(g_style.font_12);
    }
    if (g_style.font_14) {
        lv_freetype_font_delete(g_style.font_14);
    }
    if (g_style.font_16) {
        lv_freetype_font_delete(g_style.font_16);
    }
    if (g_style.font_20) {
        lv_freetype_font_delete(g_style.font_20);
    }
#elif defined(CONFIG_LV_USE_TINY_TTF)
    if (g_style.font_12) {
        lv_tiny_ttf_destroy(g_style.font_12);
    }
    if (g_style.font_14) {
        lv_tiny_ttf_destroy(g_style.font_14);
    }
    if (g_style.font_16) {
        lv_tiny_ttf_destroy(g_style.font_16);
    }
    if (g_style.font_20) {
        lv_tiny_ttf_destroy(g_style.font_20);
    }
#endif
    g_style.font_12 = NULL;
    g_style.font_14 = NULL;
    g_style.font_16 = NULL;
    g_style.font_20 = NULL;
}

const lv_font_t *smart_home_lvgl_font(int size)
{
    if (size <= 12 && g_style.font_12) {
        return g_style.font_12;
    }
    if (size <= 14 && g_style.font_14) {
        return g_style.font_14;
    }
    if (size <= 16 && g_style.font_16) {
        return g_style.font_16;
    }
    if (g_style.font_20) {
        return g_style.font_20;
    }

    /* Fallback to built-in Montserrat when the external font is unavailable. */

    if (size <= 12) {
        return &lv_font_montserrat_12;
    }
    if (size <= 14) {
        return &lv_font_montserrat_14;
    }
    if (size <= 16) {
        return &lv_font_montserrat_16;
    }
    return &lv_font_montserrat_20;
}

static const lv_font_t *smart_home_lvgl_symbol_font(int size)
{
    if (size <= 12) {
        return &lv_font_montserrat_12;
    }
    if (size <= 14) {
        return &lv_font_montserrat_14;
    }
    if (size <= 16) {
        return &lv_font_montserrat_16;
    }
    return &lv_font_montserrat_20;
}

void smart_home_lvgl_set_bg(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

void smart_home_lvgl_card_style(lv_obj_t *obj)
{
    smart_home_lvgl_set_bg(obj, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
}

void smart_home_lvgl_soft_card_style(lv_obj_t *obj)
{
    smart_home_lvgl_set_bg(obj, SMART_HOME_UI_COLOR_SURFACE_SOFT);
    lv_obj_set_style_radius(obj, 6, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
}

lv_obj_t *smart_home_lvgl_label_create(lv_obj_t *parent,
                                       const char *text,
                                       lv_color_t color,
                                       int size)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text ? text : "");
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, smart_home_lvgl_font(size), 0);
    return label;
}

/* ── Icon helpers ─────────────────────────────────────── */

static void icon_src_delete_cb(lv_event_t *event)
{
    char *src = (char *)lv_event_get_user_data(event);

    free(src);
}

static int icon_is_lv_symbol(const char *icon)
{
    unsigned char first;

    if (!icon || !icon[0]) {
        return 0;
    }

    first = (unsigned char)icon[0];
    return first >= 0x80;
}

const lv_font_t *smart_home_lvgl_embedded_icon_font(const char *icon)
{
    if (!icon) {
        return NULL;
    }

    if (strcmp(icon, SMART_HOME_ICON_AC) == 0) {
        return &ac_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_BED) == 0) {
        return &bed_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_CHAT) == 0) {
        return &chat_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_COUCH) == 0) {
        return &couch_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_DEVICE) == 0) {
        return &device_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_DROPLET) == 0) {
        return &droplet_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_FAN) == 0) {
        return &fan_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_LIGHT) == 0) {
        return &light_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_SUN) == 0) {
        return &sun_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_TEMPERATURE) == 0) {
        return &temperature_20;
    }
    if (strcmp(icon, SMART_HOME_ICON_TOOL) == 0) {
        return &tool_20;
    }

    return NULL;
}

lv_obj_t *smart_home_lvgl_icon_create(lv_obj_t *parent,
                                      const char *filename,
                                      int width,
                                      int height)
{
    char posix_path[128];
    char lvgl_path[130];
    char fallback[128];
    lv_obj_t *img;
    char *src;

    if (!parent || !filename) {
        return NULL;
    }

    if (icon_is_lv_symbol(filename)) {
        int font_size = width > 0 ? width : 16;
        const lv_font_t *font = smart_home_lvgl_embedded_icon_font(filename);
        lv_obj_t *label = lv_label_create(parent);

        if (height > font_size) {
            font_size = height;
        }

        lv_label_set_text(label, filename);
        lv_obj_set_style_text_font(label,
                                   font ? font :
                                       smart_home_lvgl_symbol_font(font_size),
                                   0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        if (width > 0) {
            if (font && width < 26) {
                width = 26;
            }
            lv_obj_set_width(label, width);
        }
        return label;
    }

    /* Check file existence via POSIX path (access()) */

    snprintf(posix_path, sizeof(posix_path),
             "%s/%s.png", SMART_HOME_ICONS_ROOT, filename);
    if (access(posix_path, F_OK) != 0) {
        snprintf(fallback, sizeof(fallback),
                 "%s/%s.png", SMART_HOME_ICONS_FALLBACK_ROOT, filename);
        if (access(fallback, F_OK) == 0) {
            strncpy(posix_path, fallback, sizeof(posix_path) - 1);
            posix_path[sizeof(posix_path) - 1] = '\0';
        }
    }

    /* Build LVGL path with drive letter prefix */

    snprintf(lvgl_path, sizeof(lvgl_path),
             SMART_HOME_LV_FS_PREFIX "%s", posix_path);

    img = lv_image_create(parent);
    src = strdup(lvgl_path);
    if (src) {
        lv_image_set_src(img, src);
        lv_obj_add_event_cb(img, icon_src_delete_cb, LV_EVENT_DELETE, src);
    }
    if (width > 0) {
        lv_obj_set_size(img, width, height > 0 ? height : width);
    }
    return img;
}

lv_obj_t *smart_home_lvgl_icon_with_text(lv_obj_t *parent,
                                          const char *filename,
                                          int icon_w,
                                          int icon_h,
                                          const char *text,
                                          lv_color_t text_color,
                                          int text_size)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_t *icon;
    lv_obj_t *label;

    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    icon = smart_home_lvgl_icon_create(cont, filename, icon_w, icon_h);
    if (icon) {
        lv_obj_set_style_margin_bottom(icon, 4, 0);
        lv_obj_set_style_text_color(icon, text_color, 0);
    }

    label = smart_home_lvgl_label_create(cont, text, text_color, text_size);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    return cont;
}
