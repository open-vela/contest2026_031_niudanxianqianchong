/**
 * smart_home LVGL UI style resources.
 */

#include "smart_home_lvgl_style.h"
#include "icons/smart_home_lvgl_png_icons.h"
#include "smart_home_memory.h"

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
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

/* TinyTTF's file backend performs many small seeks while locating and
 * rasterising CJK glyphs.  On LittleFS this makes a complete MiSans font
 * impractical at startup.  Keep one immutable copy in the ESP32-P4 user
 * heap (configured as PSRAM) and let every point size share that buffer. */
static void *g_font_data;
static size_t g_font_data_size;

static int smart_home_lvgl_load_font_data(void)
{
    struct stat st;
    ssize_t ret;
    size_t offset = 0;
    int fd;

    if (g_font_data) {
        return 0;
    }

    printf("[smart_home_lvgl] font preload begin path=%s\n",
           SMART_HOME_FONT_NORMAL);

    fd = open(SMART_HOME_FONT_NORMAL, O_RDONLY);
    if (fd < 0) {
        printf("[smart_home_lvgl] font preload open failed errno=%d\n", errno);
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        printf("[smart_home_lvgl] font preload stat failed errno=%d\n", errno);
        close(fd);
        return -1;
    }

    if (st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX) {
        printf("[smart_home_lvgl] font preload invalid size=%ld\n",
               (long)st.st_size);
        close(fd);
        return -1;
    }

    g_font_data_size = (size_t)st.st_size;
    g_font_data = smart_home_bulk_alloc(g_font_data_size);
    if (!g_font_data) {
        printf("[smart_home_lvgl] font preload alloc failed size=%zu\n",
               g_font_data_size);
        g_font_data_size = 0;
        close(fd);
        return -1;
    }

    while (offset < g_font_data_size) {
        ret = read(fd, (char *)g_font_data + offset,
                   g_font_data_size - offset);
        if (ret > 0) {
            offset += (size_t)ret;
            continue;
        }

        if (ret < 0 && errno == EINTR) {
            continue;
        }

        printf("[smart_home_lvgl] font preload read failed read=%zd "
               "expected=%zu errno=%d\n",
               ret, g_font_data_size, errno);
        smart_home_bulk_free(g_font_data);
        g_font_data = NULL;
        g_font_data_size = 0;
        close(fd);
        return -1;
    }

    close(fd);

    /* With CONFIG_ESPRESSIF_SPIRAM_USER_HEAP, ESP32-P4's user heap is the
     * external PSRAM region.  Keep this explicit in the boot log so the
     * resource image and runtime allocation can be checked together. */
#if defined(CONFIG_ARCH_CHIP_ESP32P4) && \
    defined(CONFIG_ESPRESSIF_SPIRAM_USER_HEAP)
    printf("[smart_home_lvgl] font preload done size=%zu buffer=%p "
           "region=PSRAM(user-heap)\n",
           g_font_data_size, g_font_data);
#else
    printf("[smart_home_lvgl] font preload done size=%zu buffer=%p "
           "region=bulk-heap\n",
           g_font_data_size, g_font_data);
#endif
    return 0;
}

static lv_font_t *load_font(int size)
{
#ifdef CONFIG_LV_USE_FREETYPE
    return lv_freetype_font_create(SMART_HOME_FONT_NORMAL,
                                   LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                   size,
                                   LV_FREETYPE_FONT_STYLE_NORMAL);
#elif defined(CONFIG_LV_USE_TINY_TTF)
    if (!g_font_data) {
        return NULL;
    }

    return lv_tiny_ttf_create_data(g_font_data, g_font_data_size, size);
#else
    (void)size;
    return NULL;
#endif
}

int smart_home_lvgl_style_init(void)
{
#if defined(CONFIG_LV_USE_TINY_TTF)
    if (smart_home_lvgl_load_font_data() < 0) {
        printf("[smart_home_lvgl] external font disabled; using Montserrat fallback\n");
    }
#endif

    /* Sizes 12 and 20 are intentionally not loaded: the upscaled mapping
     * below renders every request at one bucket larger, so those instances
     * would never be referenced. */
    g_style.font_14 = load_font(14);
    g_style.font_16 = load_font(16);
    g_style.font_18 = load_font(18);
    g_style.font_24 = load_font(24);
    g_style.font_32 = load_font(32);
    printf("[smart_home_lvgl] font instances source=%s size=%zu "
           "font14=%p font16=%p font18=%p font24=%p font32=%p\n",
           g_font_data ? "PSRAM-data" : "builtin-fallback",
           g_font_data_size,
           g_style.font_14,
           g_style.font_16,
           g_style.font_18,
           g_style.font_24,
           g_style.font_32);
    return 0;
}

void smart_home_lvgl_style_deinit(void)
{
#ifdef CONFIG_LV_USE_FREETYPE
    if (g_style.font_14) {
        lv_freetype_font_delete(g_style.font_14);
    }
    if (g_style.font_16) {
        lv_freetype_font_delete(g_style.font_16);
    }
    if (g_style.font_18) {
        lv_freetype_font_delete(g_style.font_18);
    }
    if (g_style.font_24) {
        lv_freetype_font_delete(g_style.font_24);
    }
    if (g_style.font_32) {
        lv_freetype_font_delete(g_style.font_32);
    }
#elif defined(CONFIG_LV_USE_TINY_TTF)
    if (g_style.font_14) {
        lv_tiny_ttf_destroy(g_style.font_14);
    }
    if (g_style.font_16) {
        lv_tiny_ttf_destroy(g_style.font_16);
    }
    if (g_style.font_18) {
        lv_tiny_ttf_destroy(g_style.font_18);
    }
    if (g_style.font_24) {
        lv_tiny_ttf_destroy(g_style.font_24);
    }
    if (g_style.font_32) {
        lv_tiny_ttf_destroy(g_style.font_32);
    }
#endif
    g_style.font_14 = NULL;
    g_style.font_16 = NULL;
    g_style.font_18 = NULL;
    g_style.font_24 = NULL;
    g_style.font_32 = NULL;

    if (g_font_data) {
        smart_home_bulk_free(g_font_data);
        g_font_data = NULL;
        g_font_data_size = 0;
    }
}

const lv_font_t *smart_home_lvgl_font(int size)
{
    /* Requested sizes map one bucket up so the whole UI renders larger:
     * 12/13 -> 14, 14 -> 16, 16/18 -> 18, 20/22 -> 24, 28+ -> 32. */
    if (size <= 13 && g_style.font_14) {
        return g_style.font_14;
    }
    if (size <= 15 && g_style.font_16) {
        return g_style.font_16;
    }
    if (size <= 18 && g_style.font_18) {
        return g_style.font_18;
    }
    if (size <= 22 && g_style.font_24) {
        return g_style.font_24;
    }
    if (g_style.font_32) {
        return g_style.font_32;
    }

    /* Fallback to built-in Montserrat when the external font is unavailable. */

    if (size <= 13) {
        return &lv_font_montserrat_14;
    }
    if (size <= 15) {
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

/* 复用当前渲染器的着色能力，不缩放控件，也不增加过渡动画。 */
void smart_home_lvgl_press_feedback(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_recolor(obj, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_recolor_opa(obj, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_recolor_opa(obj, LV_OPA_TRANSP,
        LV_STATE_DISABLED | LV_STATE_PRESSED);
    lv_obj_set_style_opa(obj, LV_OPA_50, LV_STATE_DISABLED);
}

void smart_home_lvgl_card_style(lv_obj_t *obj)
{
    smart_home_lvgl_set_bg(obj, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_style_radius(obj, 17, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_pad_all(obj, 15, 0);
}

void smart_home_lvgl_soft_card_style(lv_obj_t *obj)
{
    smart_home_lvgl_set_bg(obj, SMART_HOME_UI_COLOR_SURFACE_SOFT);
    lv_obj_set_style_radius(obj, 11, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
}

void smart_home_lvgl_style_keyboard(lv_obj_t *keyboard)
{
    if (!keyboard) {
        return;
    }

    /* Key labels are ASCII plus LVGL's built-in symbol glyphs (backspace,
     * enter, shift), which the CJK subset font lacks; use the built-in
     * Montserrat instance when the config provides it. */
#ifdef CONFIG_LV_FONT_MONTSERRAT_20
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_20, 0);
#endif
    lv_obj_set_style_pad_all(keyboard, 2, 0);
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

static int icon_is_product_asset(const char *icon)
{
    return icon && strncmp(icon, "asset:", strlen("asset:")) == 0;
}

const lv_font_t *smart_home_lvgl_embedded_icon_font(const char *icon)
{
    /* Product UI uses the PNG-derived image library. Keep this compatibility
     * hook for callers that pass LVGL built-in symbols. */
    (void)icon;
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

    if (icon_is_product_asset(filename)) {
        const lv_image_dsc_t *asset = smart_home_lvgl_png_icon_get(
            filename, width > height ? width : height);

        if (!asset) {
            return NULL;
        }
        img = lv_image_create(parent);
        lv_image_set_src(img, asset);
        if (width > 0) {
            int draw_w = width;
            int draw_h = height > 0 ? height : width;

            /* A8 bitmaps draw at their native size without scaling; when
             * no small variant was generated, clamp the widget to the
             * native bitmap size so the glyph is shown in full instead
             * of being clipped to the smaller request. */
            if (asset->header.w > draw_w) {
                draw_w = asset->header.w;
            }
            if (asset->header.h > draw_h) {
                draw_h = asset->header.h;
            }
            lv_obj_set_size(img, draw_w, draw_h);
        }
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        if (asset->header.cf == LV_COLOR_FORMAT_A8) {
            lv_obj_set_style_image_recolor(img, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
        } else {
            /* Preserve the brand PNG's original colors. */
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_TRANSP, 0);
        }
        return img;
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
        lv_obj_set_style_image_recolor(icon, text_color, 0);
        lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    }

    label = smart_home_lvgl_label_create(cont, text, text_color, text_size);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

    return cont;
}
