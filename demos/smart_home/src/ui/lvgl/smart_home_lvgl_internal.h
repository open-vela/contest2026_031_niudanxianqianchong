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
#define SMART_HOME_TOPBAR_H 56
#define SMART_HOME_UI_DEFAULT_CLOCK "12:00"
#define SMART_HOME_UI_DEFAULT_WEATHER "晴天"
#define SMART_HOME_NAV_H 64
#define SMART_HOME_NAV_BOTTOM_PAD 4

/* V9 bottom navigation geometry (1024x600). */
#define SMART_HOME_NAV_COUNT 3
#define SMART_HOME_NAV_ITEM_W 216
#define SMART_HOME_NAV_ITEM_H 52
#define SMART_HOME_NAV_ITEM_Y 6
#define SMART_HOME_NAV_X0 156
#define SMART_HOME_NAV_STEP 248
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
    return smart_home_lvgl_compact() ? 100 : 180;
}

static inline int smart_home_lvgl_pad_x(void)
{
    return smart_home_lvgl_disp_w() >= 720 ? 28 : SMART_HOME_PAD_X;
}

enum {
    SMART_HOME_TAB_HOME = 0,
    SMART_HOME_TAB_CHAT,
    SMART_HOME_TAB_SETTINGS,
    SMART_HOME_TAB_COUNT,
};

lv_obj_t *smart_home_lvgl_build_nav_bar(lv_obj_t *screen,
                                        smart_home_lvgl_t *ui);

void smart_home_lvgl_build_home_screen_v9(smart_home_lvgl_t *ui);
int smart_home_lvgl_v9_visible_card_count(void);
void smart_home_lvgl_v9_refresh_views(smart_home_lvgl_t *ui);
int smart_home_lvgl_v9_select_room(int index);
void smart_home_lvgl_build_settings_screen_v9(smart_home_lvgl_t *ui);
void smart_home_lvgl_settings_v9_deinit(smart_home_lvgl_t *ui);

#ifdef CONFIG_SMART_HOME_KWS
void smart_home_lvgl_build_wake_settings(smart_home_lvgl_t *ui, lv_obj_t *panel);
void smart_home_lvgl_wake_settings_poll(smart_home_lvgl_t *ui);
void smart_home_lvgl_wake_settings_deinit(smart_home_lvgl_t *ui);
#endif

/* 语音服务独立面板（ASR/TTS 端点与独立密钥）。
 * 只在 CONFIG_SMART_HOME_VOICE_ASR 或 CONFIG_SMART_HOME_VOICE_TTS 打开时编译。 */
#if defined(CONFIG_SMART_HOME_VOICE_ASR) || defined(CONFIG_SMART_HOME_VOICE_TTS)
void smart_home_lvgl_build_voice_settings(smart_home_lvgl_t *ui,
                                          lv_obj_t *panel,
                                          lv_obj_t *keyboard);
int smart_home_lvgl_save_llm_key(smart_home_lvgl_t *ui, const char *backend,
                                 const char *key, lv_obj_t *status,
                                 lv_obj_t *input);
void smart_home_lvgl_voice_settings_poll(smart_home_lvgl_t *ui);
int smart_home_lvgl_voice_settings_busy(smart_home_lvgl_t *ui);
void smart_home_lvgl_voice_settings_deinit(smart_home_lvgl_t *ui);
#endif
int smart_home_lvgl_v9_settings_category_count(void);
int smart_home_lvgl_v9_settings_select_category(int index);
void smart_home_lvgl_build_screensaver_screen(smart_home_lvgl_t *ui);
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
void smart_home_lvgl_miloco_poll_set_enabled(smart_home_lvgl_t *ui, int enable);
/* 扫码绑定改为弹窗，叠在当前页面之上；close 幂等。 */
void smart_home_lvgl_open_miloco_bind(smart_home_lvgl_t *ui);
void smart_home_lvgl_miloco_bind_close(smart_home_lvgl_t *ui);
int smart_home_lvgl_miloco_bind_is_open(const smart_home_lvgl_t *ui);
#endif
void smart_home_lvgl_refresh_home(smart_home_lvgl_t *ui);
void smart_home_lvgl_set_camera_indicator(smart_home_lvgl_t *ui, int on);

void smart_home_lvgl_update_clock(smart_home_lvgl_t *ui);
void smart_home_lvgl_update_clock_at(smart_home_lvgl_t *ui, time_t utc);
void smart_home_lvgl_load_tab(smart_home_lvgl_t *ui, int tab);
void smart_home_lvgl_build_top_bar(lv_obj_t *screen, smart_home_lvgl_t *ui,
                                   const char *title);
void smart_home_lvgl_refresh_network_indicators(smart_home_lvgl_t *ui);

void smart_home_lvgl_build_chat_screen(smart_home_lvgl_t *ui);
int smart_home_lvgl_chat_restore(smart_home_lvgl_t *ui);
const char *smart_home_lvgl_tool_display_name(const char *name);
void smart_home_lvgl_chat_send_text(smart_home_lvgl_t *ui, const char *text);
int smart_home_lvgl_chat_clear(smart_home_lvgl_t *ui);
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

#ifdef CONFIG_SMART_HOME_REMINDERS
void smart_home_lvgl_reminder_init(smart_home_lvgl_t *ui);
void smart_home_lvgl_reminder_poll(smart_home_lvgl_t *ui);
void smart_home_lvgl_reminder_deinit(smart_home_lvgl_t *ui);
#endif
