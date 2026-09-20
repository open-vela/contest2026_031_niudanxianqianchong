/**
 * smart_home LVGL UI internal declarations.
 */

#pragma once

#include "smart_home_lvgl.h"
#include "smart_home_lvgl_style.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SMART_HOME_SCR_W 1024
#define SMART_HOME_SCR_H 600
#define SMART_HOME_TOPBAR_H 64
#define SMART_HOME_NAV_H 64
#define SMART_HOME_NAV_BOTTOM_PAD 4
#define SMART_HOME_PAD_X 8
#define SMART_HOME_PAD_Y 6

static inline int smart_home_lvgl_disp_w(void)
{
    lv_display_t *disp = lv_display_get_default();
    int width = disp ? (int)lv_display_get_horizontal_resolution(disp) :
                       SMART_HOME_SCR_W;

    return width > 0 ? width : SMART_HOME_SCR_W;
}

static inline int smart_home_lvgl_disp_h(void)
{
    lv_display_t *disp = lv_display_get_default();
    int height = disp ? (int)lv_display_get_vertical_resolution(disp) :
                        SMART_HOME_SCR_H;

    return height > 0 ? height : SMART_HOME_SCR_H;
}

static inline int smart_home_lvgl_square_size(void)
{
    int width = smart_home_lvgl_disp_w();
    int height = smart_home_lvgl_disp_h();

    return width < height ? width : height;
}

static inline int smart_home_lvgl_compact(void)
{
    return smart_home_lvgl_disp_w() < 720 || smart_home_lvgl_disp_h() < 480;
}

static inline int smart_home_lvgl_content_w(void)
{
    int width = smart_home_lvgl_disp_w();
    int pad = width >= 720 ? 28 : SMART_HOME_PAD_X;

    return width - 2 * pad;
}

static inline int smart_home_lvgl_content_h(void)
{
    int height = smart_home_lvgl_disp_h();
    int pad = height >= 720 ? 28 : SMART_HOME_PAD_Y;

    return height - 2 * pad;
}

static inline int smart_home_lvgl_keyboard_h(void)
{
    return smart_home_lvgl_compact() ? 88 : 150;
}

static inline int smart_home_lvgl_pad_x(void)
{
    return smart_home_lvgl_disp_w() >= 720 ? 28 : SMART_HOME_PAD_X;
}

enum {
    SMART_HOME_TAB_HOME = 0,
    SMART_HOME_TAB_DEVICES,
    SMART_HOME_TAB_CHAT,
    SMART_HOME_TAB_SECURITY,
    SMART_HOME_TAB_MORE,
    SMART_HOME_TAB_COUNT,
};

/* Legacy names keep the existing device and settings modules buildable
 * while their product entry points are migrated to the five-page shell. */
#define SMART_HOME_TAB_PANEL    SMART_HOME_TAB_DEVICES
#define SMART_HOME_TAB_SETTINGS SMART_HOME_TAB_MORE

lv_obj_t *smart_home_lvgl_build_nav_bar(lv_obj_t *screen,
                                        smart_home_lvgl_t *ui);

void smart_home_lvgl_build_home_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_build_screensaver_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_build_security_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_security_camera_stop(smart_home_lvgl_t *ui);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
void smart_home_lvgl_miloco_poll_set_enabled(smart_home_lvgl_t *ui,
                                             int enable);
void smart_home_lvgl_miloco_sheet_refresh(smart_home_lvgl_t *ui);
void smart_home_lvgl_build_miloco_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_refresh_miloco_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_build_miloco_bind_screen(smart_home_lvgl_t *ui);

/* 页面公共 helper（pages.c 定义，多页复用）。 */
lv_obj_t *page_screen(smart_home_lvgl_t *ui);
lv_obj_t *page_card(lv_obj_t *screen, int x, int y, int w, int h);
void page_heading(lv_obj_t *screen, const char *title);
void page_icon_badge(lv_obj_t *card, const char *icon, lv_color_t bg);
#endif
void smart_home_lvgl_build_more_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_build_network_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_refresh_network_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_refresh_home(smart_home_lvgl_t *ui);
/* 顶栏摄像头图标跨屏同步（安防页开关/摄像头服务状态驱动）。 */
void smart_home_lvgl_set_camera_indicator(smart_home_lvgl_t *ui, int on);
void smart_home_lvgl_load_tab(smart_home_lvgl_t *ui, int tab);
void smart_home_lvgl_build_top_bar(lv_obj_t *screen, smart_home_lvgl_t *ui,
                                   const char *title);
void smart_home_lvgl_refresh_network_indicators(smart_home_lvgl_t *ui);

void smart_home_lvgl_build_panel_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_build_chat_screen(smart_home_lvgl_t *ui);
void smart_home_lvgl_build_settings_screen(smart_home_lvgl_t *ui);
/* 设置页分区定位（更多页卡片跳转）：展开对应卡并滚动到可见。 */
enum {
    SMART_HOME_SETTINGS_FOCUS_MODEL = 0,
    SMART_HOME_SETTINGS_FOCUS_TOOLS,
    SMART_HOME_SETTINGS_FOCUS_SYSTEM,
};
void smart_home_lvgl_settings_focus(smart_home_lvgl_t *ui, int section);
void smart_home_lvgl_refresh_tool_directory(smart_home_lvgl_t *ui);
void smart_home_lvgl_settings_deinit(void);

void smart_home_lvgl_chat_send_text(smart_home_lvgl_t *ui, const char *text);
#ifdef CONFIG_SMART_HOME_VOICE_ASR
/* PTT 语音输入：LVGL 线程调用；finish 在 ASR 结果分发后复位按钮。 */
void smart_home_lvgl_chat_asr_finish(smart_home_lvgl_t *ui);
void smart_home_lvgl_chat_asr_begin(smart_home_lvgl_t *ui);
int smart_home_lvgl_submit_asr_job(smart_home_lvgl_t *ui);
#endif
#ifdef CONFIG_SMART_HOME_KWS
/* 语音会话联动（KWS 唤醒 → 聊天/PTT → TTS 播报）。 */
void smart_home_lvgl_voice_session_start(smart_home_lvgl_t *ui);
void smart_home_voice_session_on_wake(smart_home_lvgl_t *ui, float score);
int smart_home_lvgl_post_kws_wake(smart_home_lvgl_t *ui, float score);
#endif
void smart_home_lvgl_append_msg_bubble(smart_home_lvgl_t *ui,
                                       const char *text,
                                       int is_user);
void smart_home_lvgl_append_tool_card(smart_home_lvgl_t *ui,
                                      const char *name,
                                      const char *call_id,
                                      int ok);
void smart_home_lvgl_append_error_bubble(smart_home_lvgl_t *ui,
                                         const char *msg);
void smart_home_lvgl_show_thinking(smart_home_lvgl_t *ui);
void smart_home_lvgl_finish_thinking(smart_home_lvgl_t *ui);
void smart_home_lvgl_trace_tool(smart_home_lvgl_t *ui,
                                const char *name,
                                const char *call_id,
                                int ok);

int smart_home_lvgl_submit_agent_job(smart_home_lvgl_t *ui, const char *text);

#ifdef __cplusplus
}
#endif
