/** Product shells for Scenes, Security and More. */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"

#include "../../net/smart_home_network.h"
#include "../../smart_home_memory.h"
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
#include "../../miloco/smart_home_miloco.h"
#include "../../config/smart_home_secrets.h"
#endif

#include <errno.h>
#include <nuttx/sched.h>
#include <syslog.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

enum page_action_e {
    PAGE_ACTION_AGENT = 1,
    PAGE_ACTION_SETTINGS,
    PAGE_ACTION_NETWORK,
    PAGE_ACTION_MILOCO,
};

/* DHCP and DNS use substantially more stack than the 2 KiB pthread default.
 * Reserve this reusable stack from the bulk/PSRAM heap just as the Agent
 * worker does; the connection worker is always serialized. */
#define SMART_HOME_NETWORK_WORKER_STACK_SIZE 8192u

lv_obj_t *page_card(lv_obj_t *screen, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(screen);

    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    smart_home_lvgl_card_style(card);
    return card;
}

static void page_title(lv_obj_t *card, const char *title, const char *body)
{
    lv_obj_t *label = smart_home_lvgl_label_create(card, title,
                                                    SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                                    20);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 54);
    label = smart_home_lvgl_label_create(card, body,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

void page_heading(lv_obj_t *screen, const char *text)
{
    lv_obj_t *label = smart_home_lvgl_label_create(
        screen, text, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 28);

    lv_obj_align(label, LV_ALIGN_TOP_LEFT, smart_home_lvgl_pad_x(),
                 SMART_HOME_TOPBAR_H + 26);
}

void page_icon_badge(lv_obj_t *card, const char *icon,
                            lv_color_t background)
{
    lv_obj_t *badge = lv_obj_create(card);
    lv_obj_t *glyph;

    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 42, 42);
    lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_radius(badge, 12, 0);
    smart_home_lvgl_set_bg(badge, background);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    glyph = smart_home_lvgl_icon_create(badge, icon, 25, 25);
    if (glyph) {
        lv_obj_set_style_text_color(glyph, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_image_recolor(glyph, SMART_HOME_UI_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_image_recolor_opa(glyph, LV_OPA_COVER, 0);
        lv_obj_center(glyph);
    }
}

static lv_obj_t *page_outline_button(lv_obj_t *parent, const char *text,
                                     int width)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_t *label;

    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, width, 38);
    lv_obj_set_style_radius(button, 12, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, SMART_HOME_UI_COLOR_BORDER, 0);
    label = smart_home_lvgl_label_create(button, text,
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         13);
    lv_obj_center(label);
    return button;
}

static void page_click_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    int action = (int)(intptr_t)lv_obj_get_user_data(
        lv_event_get_current_target(event));

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui) {
        return;
    }
    if (action == PAGE_ACTION_AGENT) {
        smart_home_lvgl_load_tab(ui, SMART_HOME_TAB_CHAT);
    } else if (action == PAGE_ACTION_SETTINGS && ui->screen_settings) {
        lv_scr_load_anim(ui->screen_settings, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                         180, 0, false);
    } else if (action == PAGE_ACTION_NETWORK && ui->screen_network) {
        smart_home_lvgl_refresh_network_screen(ui);
        lv_scr_load_anim(ui->screen_network, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                         180, 0, false);
    } else if (action == PAGE_ACTION_MILOCO && ui->screen_miloco) {
        smart_home_lvgl_refresh_miloco_screen(ui);
        lv_scr_load_anim(ui->screen_miloco, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                         180, 0, false);
    }
}

#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
static void security_camera_refresh_ui(smart_home_lvgl_t *ui)
{
    struct smart_home_camera_status_s status;
    uint32_t sequence;
    int ret;
    char text[80];

    if (!ui || !ui->security_camera_status) {
        return;
    }

    ret = smart_home_camera_get_status(&status);
    if (ret < 0) {
        snprintf(text, sizeof(text), "摄像头状态读取失败 (%d)", ret);
        lv_label_set_text(ui->security_camera_status, text);
        return;
    }

    switch (status.state) {
    case SMART_HOME_CAMERA_STARTING:
        lv_label_set_text(ui->security_camera_status, "摄像头启动中…");
        smart_home_lvgl_set_camera_indicator(ui, 1);
        break;
    case SMART_HOME_CAMERA_RUNNING:
        lv_label_set_text(ui->security_camera_status, "● 摄像头在线");
        smart_home_lvgl_set_camera_indicator(ui, 1);
        if (ui->security_camera_preview && ui->security_camera_buffer &&
            status.preview_sequence != ui->security_camera_sequence &&
            smart_home_camera_copy_latest(ui->security_camera_buffer,
                                           SMART_HOME_CAMERA_PREVIEW_BYTES,
                                           &sequence) == OK) {
            ui->security_camera_sequence = sequence;
            lv_obj_invalidate(ui->security_camera_preview);
        }
        break;
    case SMART_HOME_CAMERA_ERROR:
        snprintf(text, sizeof(text), "摄像头不可用 (%d)", status.last_error);
        lv_label_set_text(ui->security_camera_status, text);
        if (ui->security_camera_switch) {
            lv_obj_remove_state(ui->security_camera_switch,
                                LV_STATE_CHECKED);
        }
        smart_home_lvgl_set_camera_indicator(ui, 0);
        /* A failed worker remains joinable until reaped. Reap it here so a
         * later user retry can allocate a fresh V4L2 ring and PSRAM buffers. */
        (void)smart_home_camera_stop();
        break;
    case SMART_HOME_CAMERA_OFF:
    default:
        lv_label_set_text(ui->security_camera_status, "摄像头已关闭");
        smart_home_lvgl_set_camera_indicator(ui, 0);
        break;
    }

    if (ui->security_camera_metrics) {
        snprintf(text, sizeof(text), "预览上限 15 FPS · 采集 %u.%02u FPS",
                 (unsigned)(status.capture_fps_x100 / 100),
                 (unsigned)(status.capture_fps_x100 % 100));
        lv_label_set_text(ui->security_camera_metrics, text);
    }
}

static int security_camera_set_enabled(smart_home_lvgl_t *ui, bool enabled)
{
    int ret;

    if (!ui) {
        return -EINVAL;
    }

    ret = enabled ? smart_home_camera_start() : smart_home_camera_stop();
    if (ret < 0 && enabled && ui->security_camera_switch) {
        lv_obj_remove_state(ui->security_camera_switch, LV_STATE_CHECKED);
    }

    security_camera_refresh_ui(ui);
    return ret;
}

static void security_camera_switch_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *sw = lv_event_get_current_target(event);

    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED || !ui || !sw) {
        return;
    }

    (void)security_camera_set_enabled(ui,
        lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void security_camera_button_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    bool enabled;

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui) {
        return;
    }

    enabled = !ui->security_camera_switch ||
              !lv_obj_has_state(ui->security_camera_switch, LV_STATE_CHECKED);
    if (ui->security_camera_switch) {
        if (enabled) {
            lv_obj_add_state(ui->security_camera_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(ui->security_camera_switch, LV_STATE_CHECKED);
        }
    }
    (void)security_camera_set_enabled(ui, enabled);
}

static void security_camera_timer_cb(lv_timer_t *timer)
{
    security_camera_refresh_ui((smart_home_lvgl_t *)timer->user_data);
}
#endif

void smart_home_lvgl_security_camera_stop(smart_home_lvgl_t *ui)
{
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    if (!ui) {
        return;
    }

    if (ui->security_camera_switch) {
        lv_obj_remove_state(ui->security_camera_switch, LV_STATE_CHECKED);
    }
    (void)smart_home_camera_stop();
    security_camera_refresh_ui(ui);
#else
    (void)ui;
#endif
}

static void page_action(lv_obj_t *card, smart_home_lvgl_t *ui,
                        enum page_action_e action)
{
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)action);
    lv_obj_add_event_cb(card, page_click_cb, LV_EVENT_CLICKED, ui);
}

lv_obj_t *page_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    smart_home_lvgl_build_top_bar(screen, ui, "OpenVela HOME");
    return screen;
}

void smart_home_lvgl_build_security_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *card;
    int x = smart_home_lvgl_pad_x();
    int y = SMART_HOME_TOPBAR_H + 78;
    int content_w = smart_home_lvgl_content_w();
    int alert_w = 250;
    int preview_w = content_w - alert_w - 14;
    /* Fill the preview card down to the navigation bar so the 512x300
     * feed becomes the dominant element of the security page. */
    int preview_h = smart_home_lvgl_disp_h() - y -
                    (SMART_HOME_NAV_H + SMART_HOME_NAV_BOTTOM_PAD) - 8;
    lv_obj_t *label;
    lv_obj_t *footer;
    lv_obj_t *button;

    if (!ui) return;
    screen = page_screen(ui);
    ui->screen_security = screen;
    page_heading(screen, "安防");
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    /* 摄像头开关置于右上角（原'已布防'徽标位置）：安防页最高频的
     * 操作应一步可达；布防状态不再是本页主叙事。 */
    ui->security_camera_switch = lv_switch_create(screen);
    lv_obj_set_size(ui->security_camera_switch, 46, 26);
    lv_obj_align(ui->security_camera_switch, LV_ALIGN_TOP_RIGHT, -x,
                 SMART_HOME_TOPBAR_H + 32);
    lv_obj_add_event_cb(ui->security_camera_switch, security_camera_switch_cb,
                        LV_EVENT_VALUE_CHANGED, ui);
#endif

    card = page_card(screen, x, y, preview_w, preview_h);
    smart_home_lvgl_set_bg(card, lv_color_hex(0x354846));
    lv_obj_set_style_border_color(card, lv_color_hex(0x415654), 0);
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    label = smart_home_lvgl_label_create(card, "客厅 · 本地实时预览",
                                         lv_color_hex(0xD9E4E0), 13);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    ui->security_camera_buffer =
        smart_home_bulk_alloc(SMART_HOME_CAMERA_PREVIEW_BYTES);
    if (ui->security_camera_buffer) {
        memset(ui->security_camera_buffer, 0,
               SMART_HOME_CAMERA_PREVIEW_BYTES);
        memset(&ui->security_camera_image, 0,
               sizeof(ui->security_camera_image));
        ui->security_camera_image.header.magic = LV_IMAGE_HEADER_MAGIC;
        ui->security_camera_image.header.cf = LV_COLOR_FORMAT_RGB565;
        ui->security_camera_image.header.w = SMART_HOME_CAMERA_PREVIEW_WIDTH;
        ui->security_camera_image.header.h = SMART_HOME_CAMERA_PREVIEW_HEIGHT;
        ui->security_camera_image.header.stride =
            SMART_HOME_CAMERA_PREVIEW_WIDTH * 2u;
        ui->security_camera_image.data_size = SMART_HOME_CAMERA_PREVIEW_BYTES;
        ui->security_camera_image.data = ui->security_camera_buffer;
        ui->security_camera_preview = lv_image_create(card);
        lv_image_set_src(ui->security_camera_preview,
                         &ui->security_camera_image);
        lv_obj_set_size(ui->security_camera_preview,
                        SMART_HOME_CAMERA_PREVIEW_WIDTH,
                        SMART_HOME_CAMERA_PREVIEW_HEIGHT);
        /* Center the feed in the area above the metrics footer. */
        lv_obj_align(ui->security_camera_preview, LV_ALIGN_CENTER, 0, -22);
        lv_obj_set_style_radius(ui->security_camera_preview, 12, 0);
        lv_obj_add_flag(ui->security_camera_preview,
                        LV_OBJ_FLAG_ADV_HITTEST);
    } else {
        label = smart_home_lvgl_label_create(card, "预览缓冲区分配失败",
                                             SMART_HOME_UI_COLOR_DANGER, 14);
        lv_obj_center(label);
    }
#else
    label = smart_home_lvgl_label_create(card, "本机构建未启用摄像头预览",
                                         lv_color_hex(0xD9E4E0), 13);
    lv_obj_center(label);
#endif

    footer = lv_obj_create(card);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, lv_pct(100), 48);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    smart_home_lvgl_set_bg(footer, SMART_HOME_UI_COLOR_SURFACE);
#ifdef CONFIG_SMART_HOME_CAMERA_PREVIEW
    ui->security_camera_metrics = smart_home_lvgl_label_create(
        footer, "预览上限 15 FPS · 等待开启", SMART_HOME_UI_COLOR_TEXT_MUTED, 11);
    lv_obj_align(ui->security_camera_metrics, LV_ALIGN_TOP_LEFT, 0, 2);
    ui->security_camera_status = smart_home_lvgl_label_create(
        footer, "摄像头已关闭", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_align(ui->security_camera_status, LV_ALIGN_BOTTOM_LEFT, 0, -2);
    button = page_outline_button(footer, "进入监控", 92);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(button, security_camera_button_cb,
                        LV_EVENT_CLICKED, ui);
    ui->security_camera_timer = lv_timer_create(security_camera_timer_cb,
                                                1000u / 15u, ui);
    security_camera_refresh_ui(ui);
#else
    label = smart_home_lvgl_label_create(footer, "摄像头预览未启用",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
#endif

    card = page_card(screen, x + preview_w + 14, y, alert_w, preview_h);
    label = smart_home_lvgl_label_create(card, "最近动态",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         14);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    label = smart_home_lvgl_label_create(card, "当前没有需要处理的提醒",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 44);
    label = smart_home_lvgl_label_create(card,
                                         "AI 事件、门窗异常和设备离线\n会在这里出现。",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                         14);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 88);
    label = smart_home_lvgl_label_create(card, "模拟一条 AI 提醒 ›",
                                         SMART_HOME_UI_COLOR_WARNING, 14);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    smart_home_lvgl_build_nav_bar(screen, ui);
}

void smart_home_lvgl_build_more_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *card;
    int x = smart_home_lvgl_pad_x();
    int y = SMART_HOME_TOPBAR_H + 78;
    int gap = 14;
    int w = (smart_home_lvgl_content_w() - gap * 3) / 4;
    int col = 0;

    if (!ui) return;
    screen = page_screen(ui);
    ui->screen_more = screen;
    page_heading(screen, "更多");
    /* 单行四卡（能耗中心/家庭成员已移除）：位置由 col 递增计算，
     * 避免硬编码列号导致米家卡漂到最右。 */
    card = page_card(screen, x + (w + gap) * col++, y, w, 140);
    page_icon_badge(card, ICON_NAV_CHAT, lv_color_hex(0xF1F4FF));
    page_title(card, "智能管家", "家庭问答与受控执行");
    page_action(card, ui, PAGE_ACTION_AGENT);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
    card = page_card(screen, x + (w + gap) * col++, y, w, 140);
    page_icon_badge(card, ICON_MIJIA, lv_color_hex(0xFFF8F4));
    page_title(card, "米家网关", "Miloco 服务器与设备");
    page_action(card, ui, PAGE_ACTION_MILOCO);
#endif
    card = page_card(screen, x + (w + gap) * col++, y, w, 140);
    page_icon_badge(card, ICON_STATUS_WIFI, lv_color_hex(0xEAF7F1));
    page_title(card, "网络设置", "连接家庭 Wi-Fi");
    page_action(card, ui, PAGE_ACTION_NETWORK);

    card = page_card(screen, x + (w + gap) * col++, y, w, 140);
    page_icon_badge(card, ICON_NAV_SETTINGS, lv_color_hex(0xEDF8F3));
    page_title(card, "系统设置", "网络、智能服务与系统状态");
    page_action(card, ui, PAGE_ACTION_SETTINGS);
    smart_home_lvgl_build_nav_bar(screen, ui);
}

typedef struct {
    smart_home_lvgl_t *ui;
    char ssid[33];
    char password[65];
} network_connect_job_t;

static void network_secure_clear(void *memory, size_t size)
{
    volatile unsigned char *p = memory;

    while (p && size-- > 0u) {
        *p++ = 0u;
    }
}

static void layout_network_keyboard(smart_home_lvgl_t *ui, int visible)
{
    if (!ui || !ui->network_keyboard) {
        return;
    }
    lv_obj_set_size(ui->network_keyboard, smart_home_lvgl_disp_w(),
                    smart_home_lvgl_keyboard_h());
    lv_obj_align(ui->network_keyboard, LV_ALIGN_BOTTOM_MID, 0,
                 -SMART_HOME_NAV_H - SMART_HOME_NAV_BOTTOM_PAD - 8);
    if (visible) {
        lv_obj_clear_flag(ui->network_keyboard, LV_OBJ_FLAG_HIDDEN);
        /* The navigation bar is created after this keyboard.  Raise the
         * keyboard while focused so it remains usable above that bar. */
        lv_obj_move_foreground(ui->network_keyboard);
    } else {
        lv_obj_add_flag(ui->network_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void network_keyboard_input_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_obj_t *target = lv_event_get_current_target(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (!ui || !ui->network_keyboard) {
        return;
    }
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(ui->network_keyboard, target);
        layout_network_keyboard(ui, 1);
    } else if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY ||
               code == LV_EVENT_DEFOCUSED) {
        layout_network_keyboard(ui, 0);
    }
}

static void network_keyboard_event_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (!ui) {
        return;
    }
    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        layout_network_keyboard(ui, 0);
    }
}

static void *network_connect_worker(void *argument)
{
    network_connect_job_t *job = argument;
    smart_home_network_status_t status;
    int ret;

    if (!job || !job->ui || !job->ui->app) {
        free(job);
        return NULL;
    }
    ret = smart_home_network_connect_credentials(&status, job->ssid,
                                                 job->password);
    pthread_mutex_lock(&job->ui->pending_mutex);
    job->ui->app->system_status.network_status = status;
    job->ui->network_result = ret;
    job->ui->network_result_ready = 1;
    pthread_mutex_unlock(&job->ui->pending_mutex);
    network_secure_clear(job->password, sizeof(job->password));
    network_secure_clear(job->ssid, sizeof(job->ssid));
    free(job);
    return NULL;
}

void smart_home_lvgl_refresh_network_screen(smart_home_lvgl_t *ui)
{
    smart_home_network_status_t status;
    int result_ready = 0;
    int result = 0;
    int worker_active = 0;
    char text[160];

    if (!ui || !ui->network_status_label || !ui->app) {
        return;
    }
    pthread_mutex_lock(&ui->pending_mutex);
    status = ui->app->system_status.network_status;
    if (ui->network_result_ready) {
        result_ready = 1;
        result = ui->network_result;
        ui->network_result_ready = 0;
    }
    worker_active = ui->network_worker_active;
    pthread_mutex_unlock(&ui->pending_mutex);

    if (result_ready) {
        pthread_join(ui->network_worker, NULL);
        pthread_mutex_lock(&ui->pending_mutex);
        ui->network_worker_active = 0;
        pthread_mutex_unlock(&ui->pending_mutex);
    }
    if (status.online) {
        snprintf(text, sizeof(text), "已连接互联网 · %s", status.ifname ?
                 status.ifname : "wlan0");
    } else if (status.ip_status == SMART_HOME_NETWORK_OK) {
        snprintf(text, sizeof(text), "已连接 Wi-Fi，互联网/DNS 暂不可用");
    } else if (worker_active) {
        snprintf(text, sizeof(text), "正在连接，请稍候…");
    } else if (status.ip_status == SMART_HOME_NETWORK_ERR_DHCP) {
        snprintf(text, sizeof(text), "Wi-Fi 已关联，但 DHCP 地址获取失败");
    } else if (result_ready) {
        snprintf(text, sizeof(text), "连接失败（%d），请检查密码或路由器", result);
    } else {
        snprintf(text, sizeof(text), "未连接 · 输入家庭 Wi-Fi 信息后连接");
    }
    lv_label_set_text(ui->network_status_label, text);
    smart_home_lvgl_refresh_network_indicators(ui);
}

static void network_status_timer_cb(lv_timer_t *timer)
{
    smart_home_lvgl_refresh_network_screen(timer ?
                                            lv_timer_get_user_data(timer) : NULL);
}

static void network_connect_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    network_connect_job_t *job;
    const char *ssid;
    const char *password;
    pthread_attr_t attr;
    int attr_ready = 0;
    int ret;

    if (!ui || !ui->network_ssid_input || !ui->network_password_input ||
        !ui->app || ui->network_worker_active) {
        return;
    }
    ssid = lv_textarea_get_text(ui->network_ssid_input);
    password = lv_textarea_get_text(ui->network_password_input);
    if (!ssid || !ssid[0] || strlen(ssid) > 32u || strlen(password) > 64u) {
        lv_label_set_text(ui->network_status_label,
                          "SSID 或密码长度不合法");
        return;
    }
    job = calloc(1u, sizeof(*job));
    if (!job) {
        lv_label_set_text(ui->network_status_label, "内存不足，无法开始连接");
        return;
    }
    job->ui = ui;
    memcpy(job->ssid, ssid, strlen(ssid) + 1u);
    memcpy(job->password, password, strlen(password) + 1u);
    pthread_mutex_lock(&ui->pending_mutex);
    ui->network_worker_active = 1;
    ui->network_result_ready = 0;
    pthread_mutex_unlock(&ui->pending_mutex);
    layout_network_keyboard(ui, 0);
    if (!ui->network_worker_stack_alloc) {
        ui->network_worker_stack_alloc = smart_home_bulk_alloc(
            SMART_HOME_NETWORK_WORKER_STACK_SIZE + STACK_ALIGNMENT - 1u);
        if (ui->network_worker_stack_alloc) {
            ui->network_worker_stack = (void *)STACK_ALIGN_UP(
                (uintptr_t)ui->network_worker_stack_alloc);
        }
    }
    if (!ui->network_worker_stack) {
        ret = -1;
    } else {
        ret = pthread_attr_init(&attr);
        if (ret == 0) {
            attr_ready = 1;
            ret = pthread_attr_setstack(&attr, ui->network_worker_stack,
                                        SMART_HOME_NETWORK_WORKER_STACK_SIZE);
        }
        if (ret == 0) {
            ret = pthread_create(&ui->network_worker, &attr,
                                 network_connect_worker, job);
        }
    }
    if (attr_ready) {
        pthread_attr_destroy(&attr);
    }
    if (ret != 0) {
        pthread_mutex_lock(&ui->pending_mutex);
        ui->network_worker_active = 0;
        pthread_mutex_unlock(&ui->pending_mutex);
        network_secure_clear(job, sizeof(*job));
        free(job);
        lv_label_set_text(ui->network_status_label, "无法创建网络连接任务");
        return;
    }
    smart_home_lvgl_refresh_network_screen(ui);
}

/* ── 网络设置页 ───────────────────────────────────────────── */

/* 演示环境 Wi-Fi 默认值：secrets 无已存凭据时预填输入框；已保存的
 * 凭据始终优先。 */
#define NETWORK_DEMO_DEFAULT_SSID     "123"
#define NETWORK_DEMO_DEFAULT_PASSWORD "888888888"

void smart_home_lvgl_build_network_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *button;
    int x = smart_home_lvgl_pad_x();
    int compact = smart_home_lvgl_compact();

    if (!ui) {
        return;
    }
    screen = page_screen(ui);
    ui->screen_network = screen;
    page_heading(screen, "网络设置");
    card = page_card(screen, x, SMART_HOME_TOPBAR_H + 78,
                     smart_home_lvgl_content_w(), compact ? 248 : 300);
    page_icon_badge(card, ICON_STATUS_WIFI, lv_color_hex(0xEAF7F1));
    label = smart_home_lvgl_label_create(card, "家庭 Wi-Fi",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 20);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 56, 4);
    ui->network_status_label = smart_home_lvgl_label_create(
        card, "", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_align(ui->network_status_label, LV_ALIGN_TOP_LEFT, 0, 54);

    /* 已保存凭据优先；否则预填演示默认值，连接只需一键。 */
    {
        char saved_ssid[33];
        char saved_password[65];

        saved_ssid[0] = '\0';
        saved_password[0] = '\0';
        if (ui->app &&
            smart_home_secrets_get_wifi_credentials(saved_ssid,
                                                    sizeof(saved_ssid),
                                                    saved_password,
                                                    sizeof(saved_password))
                != AGENT_OK) {
            snprintf(saved_ssid, sizeof(saved_ssid), "%s",
                     NETWORK_DEMO_DEFAULT_SSID);
            snprintf(saved_password, sizeof(saved_password), "%s",
                     NETWORK_DEMO_DEFAULT_PASSWORD);
        }
        ui->network_ssid_input = lv_textarea_create(card);
        lv_textarea_set_text(ui->network_ssid_input, saved_ssid);
    }
    lv_textarea_set_placeholder_text(ui->network_ssid_input, "Wi-Fi 名称（SSID）");
    lv_textarea_set_one_line(ui->network_ssid_input, true);
    lv_obj_set_style_text_font(ui->network_ssid_input,
                               smart_home_lvgl_font(14), 0);
    lv_obj_set_size(ui->network_ssid_input, lv_pct(88), 38);
    lv_obj_align(ui->network_ssid_input, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_add_event_cb(ui->network_ssid_input, network_keyboard_input_cb,
                        LV_EVENT_ALL, ui);

    {
        char saved_ssid[33];
        char saved_password[65];

        saved_ssid[0] = '\0';
        saved_password[0] = '\0';
        if (ui->app &&
            smart_home_secrets_get_wifi_credentials(saved_ssid,
                                                    sizeof(saved_ssid),
                                                    saved_password,
                                                    sizeof(saved_password))
                != AGENT_OK) {
            snprintf(saved_password, sizeof(saved_password), "%s",
                     NETWORK_DEMO_DEFAULT_PASSWORD);
        }
        ui->network_password_input = lv_textarea_create(card);
        lv_textarea_set_text(ui->network_password_input, saved_password);
    }
    lv_textarea_set_placeholder_text(ui->network_password_input, "密码（开放网络可留空）");
    lv_textarea_set_one_line(ui->network_password_input, true);
    lv_textarea_set_password_mode(ui->network_password_input, true);
    lv_obj_set_style_text_font(ui->network_password_input,
                               smart_home_lvgl_font(14), 0);
    lv_obj_set_size(ui->network_password_input, lv_pct(88), 38);
    lv_obj_align(ui->network_password_input, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_add_event_cb(ui->network_password_input, network_keyboard_input_cb,
                        LV_EVENT_ALL, ui);

    /* 连接按钮放卡片头部右侧：键盘弹出时覆盖卡片下半区（约 y>232），
     * 底部布局的按钮会被键盘遮住导致不可见不可点（米家页同款结论）。 */
    button = page_outline_button(card, "连接", 112);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_PRIMARY);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), lv_color_white(), 0);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(button, network_connect_cb, LV_EVENT_CLICKED, ui);

    ui->network_keyboard = lv_keyboard_create(screen);
    smart_home_lvgl_style_keyboard(ui->network_keyboard);
    lv_obj_add_event_cb(ui->network_keyboard, network_keyboard_event_cb,
                        LV_EVENT_ALL, ui);
    ui->network_status_timer = lv_timer_create(network_status_timer_cb, 250, ui);
    smart_home_lvgl_refresh_network_screen(ui);
    smart_home_lvgl_build_nav_bar(screen, ui);
    layout_network_keyboard(ui, 0);
}

#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
/* ── 米家网关设置页（与网络设置同款布局与键盘避让） ─────────── */

/* 演示环境默认值：secrets 未配置时预填，免触屏手输；已保存的配置
 * 始终优先于默认值。 */
#define MILOCO_DEMO_DEFAULT_HOST  "192.168.251.47"
#define MILOCO_DEMO_DEFAULT_TOKEN "p4x-miloco"

static void layout_miloco_keyboard(smart_home_lvgl_t *ui, int visible)
{
    if (!ui || !ui->miloco_keyboard) {
        return;
    }
    lv_obj_set_size(ui->miloco_keyboard, smart_home_lvgl_disp_w(),
                    smart_home_lvgl_keyboard_h());
    lv_obj_align(ui->miloco_keyboard, LV_ALIGN_BOTTOM_MID, 0,
                 -SMART_HOME_NAV_H - SMART_HOME_NAV_BOTTOM_PAD - 8);
    if (visible) {
        lv_obj_clear_flag(ui->miloco_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(ui->miloco_keyboard);
    } else {
        lv_obj_add_flag(ui->miloco_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void miloco_keyboard_input_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (!ui || !ui->miloco_keyboard) {
        return;
    }
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(ui->miloco_keyboard,
                                 lv_event_get_current_target(event));
        layout_miloco_keyboard(ui, 1);
    } else if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY ||
               code == LV_EVENT_DEFOCUSED) {
        layout_miloco_keyboard(ui, 0);
    }
}

static void miloco_keyboard_event_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (ui && (code == LV_EVENT_CANCEL || code == LV_EVENT_READY)) {
        layout_miloco_keyboard(ui, 0);
    }
}

static lv_obj_t *miloco_input_create(lv_obj_t *parent, smart_home_lvgl_t *ui,
                                     const char *placeholder,
                                     const char *value,
                                     uint32_t max_length, int password, int y)
{
    lv_obj_t *input = lv_textarea_create(parent);

    lv_textarea_set_placeholder_text(input, placeholder);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_text(input, value ? value : "");
    lv_textarea_set_max_length(input, max_length);
    if (password) {
        lv_textarea_set_password_mode(input, true);
    }
    lv_obj_set_style_text_font(input, smart_home_lvgl_font(14), 0);
    lv_obj_set_size(input, lv_pct(88), 38);
    lv_obj_align(input, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_add_event_cb(input, miloco_keyboard_input_cb, LV_EVENT_ALL, ui);
    return input;
}

static void copy_textarea_text(char *dst, size_t dst_size, lv_obj_t *textarea)
{
    const char *text;

    if (!dst || dst_size == 0u) {
        return;
    }
    dst[0] = '\0';
    if (!textarea) {
        return;
    }
    text = lv_textarea_get_text(textarea);
    if (text) {
        strncpy(dst, text, dst_size - 1u);
        dst[dst_size - 1u] = '\0';
    }
}

void smart_home_lvgl_refresh_miloco_screen(smart_home_lvgl_t *ui)
{
    char text_buffer[48];
    const char *text;
    lv_color_t color;

    if (!ui || !ui->miloco_status_label) {
        return;
    }
    if (ui->app && ui->app->miloco &&
        smart_home_miloco_reachable(ui->app->miloco)) {
        smart_home_miloco_device_t devices[SMART_HOME_MILOCO_MAX_DEVICES];
        size_t count = smart_home_miloco_list(ui->app->miloco, devices,
                                              SMART_HOME_MILOCO_MAX_DEVICES,
                                              NULL);

        snprintf(text_buffer, sizeof(text_buffer),
                 "网关在线 · %u 台设备", (unsigned)count);
        text = text_buffer;
        color = SMART_HOME_UI_COLOR_SUCCESS;
    } else if (ui->app && ui->app->miloco) {
        text = "已配置 · 等待连接…";
        color = SMART_HOME_UI_COLOR_WARNING;
    } else {
        text = "未配置";
        color = SMART_HOME_UI_COLOR_TEXT_MUTED;
    }
    lv_label_set_text(ui->miloco_status_label, text);
    lv_obj_set_style_text_color(ui->miloco_status_label, color, 0);
}

static void miloco_save_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    smart_home_miloco_config_t config;
    char port_text[8];
    long port_value;
    int ret;

    if (!ui || !ui->app || !ui->miloco_host_input) {
        return;
    }
    copy_textarea_text(config.host, sizeof(config.host),
                       ui->miloco_host_input);
    copy_textarea_text(port_text, sizeof(port_text),
                       ui->miloco_port_input);
    copy_textarea_text(config.token, sizeof(config.token),
                       ui->miloco_token_input);

    port_value = atol(port_text);
    if (port_value <= 0 || port_value > 65535) {
        port_value = SMART_HOME_MILOCO_DEFAULT_PORT;
    }
    config.port = (uint16_t)port_value;

    syslog(LOG_INFO, "[milo] save: begin\n");
    if (!smart_home_miloco_config_valid(&config)) {
        lv_label_set_text(ui->miloco_status_label, "地址不能为空");
        lv_obj_set_style_text_color(ui->miloco_status_label,
                                    SMART_HOME_UI_COLOR_DANGER, 0);
        return;
    }

    /* secrets 文件写入与配置应用全部由 worker 线程执行：LVGL/主线程
     * 上的 LittleFS 写入会挂死系统。首次配置先创建 worker。 */
    if (!ui->app->miloco) {
        ret = smart_home_miloco_start(&ui->app->miloco, &config);
        if (ret != AGENT_OK) {
            lv_label_set_text(ui->miloco_status_label, "网关启动失败");
            lv_obj_set_style_text_color(ui->miloco_status_label,
                                        SMART_HOME_UI_COLOR_DANGER, 0);
            return;
        }
    }
    ret = smart_home_miloco_request_save(ui->app->miloco, &config);
    syslog(LOG_INFO, "[milo] save: request ret=%d\n", ret);
    if (ret != AGENT_OK) {
        lv_label_set_text(ui->miloco_status_label, "保存请求失败");
        lv_obj_set_style_text_color(ui->miloco_status_label,
                                    SMART_HOME_UI_COLOR_DANGER, 0);
        return;
    }
    if (ret != AGENT_OK) {
        lv_label_set_text(ui->miloco_status_label, "网关启动失败");
        lv_obj_set_style_text_color(ui->miloco_status_label,
                                    SMART_HOME_UI_COLOR_DANGER, 0);
        return;
    }
    layout_miloco_keyboard(ui, 0);
    lv_label_set_text(ui->miloco_status_label, "已保存 · 正在连接…");
    lv_obj_set_style_text_color(ui->miloco_status_label,
                                SMART_HOME_UI_COLOR_WARNING, 0);
}

static void miloco_bind_entry_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui ||
        !ui->screen_miloco_bind) {
        return;
    }
    lv_scr_load_anim(ui->screen_miloco_bind, LV_SCR_LOAD_ANIM_MOVE_LEFT,
                     180, 0, false);
}

void smart_home_lvgl_build_miloco_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen;
    lv_obj_t *card;
    lv_obj_t *label;
    lv_obj_t *button;
    int x = smart_home_lvgl_pad_x();
    int y = SMART_HOME_TOPBAR_H + 78;
    char host[64];
    char token[64];
    uint16_t port = SMART_HOME_MILOCO_DEFAULT_PORT;
    char port_text[8];
    bool configured;

    if (!ui) {
        return;
    }
    screen = page_screen(ui);
    ui->screen_miloco = screen;
    page_heading(screen, "米家网关");

    card = page_card(screen, x, y, smart_home_lvgl_content_w(),
                     smart_home_lvgl_compact() ? 320 : 360);
    page_icon_badge(card, ICON_MIJIA, lv_color_hex(0xFFF8F4));

    host[0] = '\0';
    token[0] = '\0';
    configured = smart_home_secrets_get_miloco(host, sizeof(host), &port,
                                               token, sizeof(token))
                 == AGENT_OK;
    if (!configured) {
        snprintf(host, sizeof(host), "%s", MILOCO_DEMO_DEFAULT_HOST);
        snprintf(token, sizeof(token), "%s", MILOCO_DEMO_DEFAULT_TOKEN);
        port = SMART_HOME_MILOCO_DEFAULT_PORT;
    }
    snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);

    label = smart_home_lvgl_label_create(card, "Xiaomi Miloco 家庭服务器",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 18);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 56, 4);
    ui->miloco_status_label = smart_home_lvgl_label_create(
        card, "", SMART_HOME_UI_COLOR_TEXT_SECONDARY, 13);
    lv_obj_align(ui->miloco_status_label, LV_ALIGN_TOP_LEFT, 0, 54);

    ui->miloco_host_input = miloco_input_create(
        card, ui, "服务器地址（IP 或主机名）",
        host, sizeof(host) - 1u, 0, 88);
    ui->miloco_port_input = miloco_input_create(
        card, ui, "端口（默认 1810）",
        port_text, sizeof(port_text) - 1u, 0, 138);
    ui->miloco_token_input = miloco_input_create(
        card, ui, "服务 Token（未启用鉴权可留空）",
        token, sizeof(token) - 1u, 1, 188);

    /* 按钮放卡片头部右侧：键盘弹出时覆盖卡片下半区（约 y>232），
     * 底部布局的按钮会被键盘遮住导致不可见不可点。 */
    button = page_outline_button(card, "保存并连接", 132);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_PRIMARY);
    lv_obj_set_style_text_color(lv_obj_get_child(button, 0), lv_color_white(), 0);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(button, miloco_save_cb, LV_EVENT_CLICKED, ui);

    button = page_outline_button(card, "扫码绑定", 108);
    lv_obj_align(button, LV_ALIGN_TOP_RIGHT, -144, 0);
    lv_obj_add_event_cb(button, miloco_bind_entry_cb, LV_EVENT_CLICKED, ui);

    ui->miloco_keyboard = lv_keyboard_create(screen);
    smart_home_lvgl_style_keyboard(ui->miloco_keyboard);
    lv_obj_add_event_cb(ui->miloco_keyboard, miloco_keyboard_event_cb,
                        LV_EVENT_ALL, ui);
    smart_home_lvgl_refresh_miloco_screen(ui);
    smart_home_lvgl_build_nav_bar(screen, ui);
    layout_miloco_keyboard(ui, 0);
}
#endif

