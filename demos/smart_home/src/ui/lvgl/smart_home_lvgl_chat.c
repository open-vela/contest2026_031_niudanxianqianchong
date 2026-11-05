/**
 * smart_home LVGL chat screen: messages and optional operation summaries.
 */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"
#include "icons/smart_home_lvgl_png_icons.h"

#include "../../config/smart_home_config.h"
#ifdef CONFIG_SMART_HOME_VOICE_ASR
#include <pthread.h>
#include "../../smart_home_memory.h"
#include "../../voice/smart_home_asr.h"
#include "../../voice/smart_home_voice_capture.h"
#endif

#include <stdio.h>
#include <string.h>

#define SMART_HOME_CHAT_INPUT_H 56
#define SMART_HOME_CHAT_HEADER_H 124
#define SMART_HOME_CHAT_STATUS_H 22

static int chat_content_width(void)
{
    return smart_home_lvgl_disp_w() - 48;
}

static int chat_list_height(int keyboard_visible)
{
    int reserved = SMART_HOME_CHAT_HEADER_H + SMART_HOME_NAV_H +
                   16 + SMART_HOME_CHAT_INPUT_H + SMART_HOME_CHAT_STATUS_H + 14;
    int list_h;

    if (keyboard_visible) {
        reserved += smart_home_lvgl_keyboard_h() + 6;
    }

    list_h = smart_home_lvgl_disp_h() - reserved;
    return list_h > 48 ? list_h : 48;
}

static void layout_chat_controls(smart_home_lvgl_t *ui, int keyboard_visible)
{
    int content_w;
    int input_y;

    if (!ui) {
        return;
    }

    content_w = chat_content_width();
    input_y = -SMART_HOME_NAV_H - 16;

    if (ui->chat_list) {
        lv_obj_set_size(ui->chat_list,
                        content_w,
                        chat_list_height(keyboard_visible));
        lv_obj_align(ui->chat_list,
                     LV_ALIGN_TOP_MID,
                     0,
                     SMART_HOME_CHAT_HEADER_H);
    }

    if (ui->chat_keyboard) {
        lv_obj_set_size(ui->chat_keyboard,
                        content_w,
                        smart_home_lvgl_keyboard_h());
        lv_obj_align(ui->chat_keyboard,
                     LV_ALIGN_BOTTOM_MID,
                     0,
                     -SMART_HOME_NAV_H - SMART_HOME_NAV_BOTTOM_PAD - 8);
        if (keyboard_visible) {
            lv_obj_clear_flag(ui->chat_keyboard, LV_OBJ_FLAG_HIDDEN);
            input_y -= smart_home_lvgl_keyboard_h() + 6;
        } else {
            lv_obj_add_flag(ui->chat_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (ui->chat_input_bar) {
        lv_obj_set_size(ui->chat_input_bar, content_w, SMART_HOME_CHAT_INPUT_H);
        lv_obj_align(ui->chat_input_bar, LV_ALIGN_BOTTOM_MID, 0, input_y);
    }
    if (ui->chat_status && ui->chat_input_bar) {
        lv_obj_align_to(ui->chat_status, ui->chat_input_bar,
                         LV_ALIGN_OUT_TOP_LEFT, 0, -6);
    }
}

static void scroll_chat_to_bottom(smart_home_lvgl_t *ui)
{
    if (!ui || !ui->chat_list) {
        return;
    }

    lv_obj_update_layout(ui->chat_list);
    lv_obj_scroll_to_y(ui->chat_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static lv_obj_t *create_chat_row(smart_home_lvgl_t *ui, int align_right)
{
    lv_obj_t *row = lv_obj_create(ui->chat_list);

    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row,
                          align_right ? LV_FLEX_ALIGN_END :
                                        LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    return row;
}

static void submit_chat_input(smart_home_lvgl_t *ui)
{
    const char *text;
    char input[SMART_HOME_INPUT_SIZE];

    if (!ui || !ui->chat_input || ui->request_inflight) {
        return;
    }

    text = lv_textarea_get_text(ui->chat_input);
    if (!text || !text[0]) {
        layout_chat_controls(ui, 0);
        return;
    }

    strncpy(input, text, sizeof(input) - 1);
    input[sizeof(input) - 1] = '\0';
    lv_textarea_set_text(ui->chat_input, "");
    layout_chat_controls(ui, 0);
    smart_home_lvgl_chat_send_text(ui, input);
}

static void chat_input_event_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (!ui) {
        return;
    }

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        if (ui->chat_keyboard && ui->chat_input) {
            lv_keyboard_set_textarea(ui->chat_keyboard, ui->chat_input);
        }
        layout_chat_controls(ui, 1);
    } else if (code == LV_EVENT_READY) {
        submit_chat_input(ui);
    } else if (code == LV_EVENT_CANCEL || code == LV_EVENT_DEFOCUSED) {
        layout_chat_controls(ui, 0);
    }
}

static void chat_keyboard_event_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (!ui) {
        return;
    }

    if (code == LV_EVENT_READY) {
        submit_chat_input(ui);
    } else if (code == LV_EVENT_CANCEL) {
        layout_chat_controls(ui, 0);
    }
}

static void chat_send_event_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        submit_chat_input(ui);
    }
}

#ifdef CONFIG_SMART_HOME_VOICE_ASR
/* PTT 语音输入：点击开始录音（按钮转"停止"），再点结束并云端识别，
 * 结果由 agent.c 的 pending 队列回投 LVGL 线程后自动发送。 */
void smart_home_lvgl_chat_asr_finish(smart_home_lvgl_t *ui)
{
    if (!ui) {
        return;
    }

    ui->asr_active = 0;
    ui->asr_abort = 0;
    if (ui->chat_mic_btn) {
        smart_home_lvgl_set_bg(ui->chat_mic_btn, SMART_HOME_UI_COLOR_PRIMARY);
    }
    if (ui->chat_mic_label) {
        lv_image_set_src(ui->chat_mic_label,
            smart_home_lvgl_png_icon_get("asset:microphone", 32));
    }
}

void smart_home_lvgl_chat_asr_begin(smart_home_lvgl_t *ui)
{
    smart_home_asr_config_t config;
    int ret;

    if (!ui) {
        return;
    }

    if (smart_home_lvgl_voice_settings_busy(ui)) {
        if (ui->chat_status) {
            lv_label_set_text(ui->chat_status, "语音设置任务进行中，请稍后重试");
            lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    smart_home_asr_config_load(&config);
    if (!config.enabled) {
        if (ui->chat_status) {
            lv_label_set_text(ui->chat_status,
                              "语音输入已在设置中关闭。");
            lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    ret = smart_home_secrets_api_key_status(SMART_HOME_SECRET_ASR,
                                             config.backend_id);
    if (ret != AGENT_OK) {
        if (ui->chat_status) {
            lv_label_set_text(ui->chat_status, smart_home_secrets_error_text(ret));
            lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    ui->asr_active = 1;
    ui->asr_abort = 0;
    if (ui->chat_mic_btn) {
        smart_home_lvgl_set_bg(ui->chat_mic_btn, SMART_HOME_UI_COLOR_DANGER);
    }
    if (ui->chat_mic_label) {
        lv_image_set_src(ui->chat_mic_label,
            smart_home_lvgl_png_icon_get("asset:microphone-off", 32));
    }
    if (ui->chat_status) {
        lv_label_set_text(ui->chat_status, "正在录音，再点一次结束…");
        lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
    }

    ret = smart_home_lvgl_submit_asr_job(ui);
    if (ret != 0) {
        smart_home_lvgl_append_error_bubble(ui, "语音输入启动失败（内存不足）");
        smart_home_lvgl_chat_asr_finish(ui);
    }
}

static void chat_mic_event_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = (smart_home_lvgl_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !ui) {
        return;
    }

    if (ui->asr_active) {
        ui->asr_abort = 1;
        /* 录音循环要等下一个缓冲周期才退出，之后还有云端识别。
         * 立刻给出状态，否则第二次点击看起来"没有反应"。 */
        if (ui->chat_mic_btn) {
            smart_home_lvgl_set_bg(ui->chat_mic_btn,
                                   SMART_HOME_UI_COLOR_PRIMARY);
        }
        if (ui->chat_mic_label) {
            lv_image_set_src(ui->chat_mic_label,
                smart_home_lvgl_png_icon_get("asset:microphone", 32));
        }
        if (ui->chat_status) {
            lv_label_set_text(ui->chat_status, "正在识别…");
            lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    smart_home_lvgl_chat_asr_begin(ui);
}
#endif

static void chat_avatar(lv_obj_t *row, int is_user)
{
    lv_obj_t *avatar = lv_obj_create(row);
    lv_obj_t *glyph;
    lv_obj_remove_style_all(avatar);
    lv_obj_set_size(avatar, is_user ? 44 : 32, 44);
    lv_obj_set_style_radius(avatar, 22, 0);
    lv_obj_clear_flag(avatar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    if (is_user) smart_home_lvgl_set_bg(avatar, SMART_HOME_UI_COLOR_SURFACE);
    glyph = smart_home_lvgl_icon_create(avatar,
        is_user ? "asset:user" : "asset:setting-ai", 32, 32);
    if (glyph) lv_obj_center(glyph);
}

void smart_home_lvgl_append_msg_bubble(smart_home_lvgl_t *ui,
                                       const char *text,
                                       int is_user)
{
    lv_obj_t *row;
    lv_obj_t *bubble;
    lv_obj_t *label;
    int width = is_user ? 568 : 696;

    if (!ui || !ui->chat_list || !text) return;
    row = create_chat_row(ui, is_user);
    lv_obj_set_style_pad_column(row, 12, 0);
    if (is_user) lv_obj_set_style_pad_bottom(row, 14, 0);
    else chat_avatar(row, 0);
    bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, width);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(bubble, 60, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(bubble, 20, 0);
    lv_obj_set_style_radius(bubble, is_user ? 16 : 18, 0);
    smart_home_lvgl_set_bg(bubble, is_user ? SMART_HOME_UI_COLOR_USER_BUBBLE :
                                            SMART_HOME_UI_COLOR_SURFACE);
    if (!is_user) {
        lv_obj_set_style_border_width(bubble, 1, 0);
        lv_obj_set_style_border_color(bubble, SMART_HOME_UI_COLOR_BORDER, 0);
    }
    label = smart_home_lvgl_label_create(bubble, text,
        is_user ? SMART_HOME_UI_COLOR_TEXT_PRIMARY : SMART_HOME_UI_COLOR_TEXT_SECONDARY, 18);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width - 40);
    lv_obj_set_style_text_line_space(label, 6, 0);
    if (is_user) chat_avatar(row, 1);
    scroll_chat_to_bottom(ui);
}

static const char *tool_status_text(int ok)
{
    if (ok > 0) {
        return "完成";
    }
    if (ok == 0) {
        return "失败";
    }
    return ok == -2 ? "未确认" : "调用中";
}

/* Protocol names and call IDs remain internal to event matching. */
const char *smart_home_lvgl_tool_display_name(const char *name)
{
    static const struct {
        const char *name;
        const char *title;
    } titles[] = {
        {"get_home_status", "演示 · 查询设备状态"},
        {"get_device_inventory", "查看设备来源"},
        {"board_camera", "本机 · 摄像头操作"},
        {"camera", "本机 · 摄像头操作"},
        {"get_indoor_environment", "查看室内环境"},
        {"get_weather", "查询天气"},
        {"set_light", "演示 · 调整灯光"},
        {"set_ac", "演示 · 调整空调"},
#ifdef CONFIG_SMART_HOME_MILOCO_BRIDGE
        {"run_scene", "米家 · 提交场景"},
#else
        {"run_scene", "演示 · 切换场景"},
#endif
        {"set_timer", "设置提醒"},
        {"set_alarm", "设置闹钟"},
        {"list_timers", "查看提醒"},
        {"cancel_timer", "取消提醒"},
        {"read_skill", "查阅操作说明"},
        {"user_memory", "管理用户记忆"},
        {"miot_device_list", "米家 · 查询设备能力"},
        {"miot_device_control", "米家 · 提交设备控制"},
    };
    size_t i;

    for (i = 0; name && i < sizeof(titles) / sizeof(titles[0]); i++) {
        if (strcmp(name, titles[i].name) == 0) {
            return titles[i].title;
        }
    }
    if (name && !strncmp(name, "node_", 5)) return "Node · 节点操作";
    if (name && !strncmp(name, "mcp_", 4)) return "MCP · 服务调用";
    return "其他服务操作";
}

static lv_color_t tool_status_color(int ok)
{
    if (ok > 0) {
        return SMART_HOME_UI_COLOR_SUCCESS;
    }
    if (ok == 0) {
        return SMART_HOME_UI_COLOR_DANGER;
    }
    return SMART_HOME_UI_COLOR_WARNING;
}

static void refresh_trace_header(smart_home_lvgl_t *ui)
{
    int has_running = 0;
    int has_failed = 0;
    int has_unknown = 0;

    if (!ui || !ui->chat_trace_status) {
        return;
    }

    for (int i = 0; i < ui->chat_trace_tool_count; i++) {
        if (ui->chat_trace_tools[i].state == -1) {
            has_running = 1;
        } else if (ui->chat_trace_tools[i].state == 0) {
            has_failed = 1;
        } else if (ui->chat_trace_tools[i].state == -2) {
            has_unknown = 1;
        }
    }

    if (ui->chat_trace_title && ui->chat_trace_tool_count) {
        lv_label_set_text(ui->chat_trace_title,
            ui->chat_trace_tool_count == 1 ?
            smart_home_lvgl_tool_display_name(ui->chat_trace_tools[0].name) : "工具调用");
    }
    lv_obj_set_style_bg_color(ui->chat_trace_status,
        has_failed ? SMART_HOME_UI_COLOR_ERROR_BUBBLE_BG :
        has_running || has_unknown ? SMART_HOME_UI_COLOR_TOOL_CARD_BG :
                                     lv_color_hex(0xE4EEE7), 0);
    if (has_running) {
        lv_label_set_text(ui->chat_trace_status, "调用中");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_WARNING,
                                    0);
    } else if (has_failed) {
        lv_label_set_text(ui->chat_trace_status, "失败");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_DANGER,
                                    0);
    } else if (has_unknown) {
        lv_label_set_text(ui->chat_trace_status, "结果待确认");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_WARNING,
                                    0);
    } else {
        lv_label_set_text(ui->chat_trace_status, "完成");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_SUCCESS,
                                    0);
    }

    /* Force display redraw — lv_label_set_text may not dirty the parent
     * container in LVGL 9, so the screen stays stale until a touch event
     * triggers an implicit layout update.  Invalidating the trace card
     * parent ensures the change is visible immediately. */

    if (ui->chat_trace_status) {
        lv_obj_invalidate(ui->chat_trace_status);
    }
    if (ui->chat_trace_title) {
        lv_obj_invalidate(ui->chat_trace_title);
    }
    if (ui->chat_trace_body) {
        lv_obj_invalidate(ui->chat_trace_body);
    }
}

static void trace_header_event_cb(lv_event_t *event)
{
    lv_obj_t *body = (lv_obj_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !body) {
        return;
    }

    if (lv_obj_has_flag(body, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(body, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_update_layout(lv_obj_get_parent(body));
}

static void reset_active_trace_tools(smart_home_lvgl_t *ui)
{
    if (!ui) {
        return;
    }

    ui->chat_trace_tool_count = 0;
    for (int i = 0; i < SMART_HOME_LVGL_TRACE_TOOL_MAX; i++) {
        memset(&ui->chat_trace_tools[i], 0, sizeof(ui->chat_trace_tools[i]));
    }
}

static void ensure_trace_card(smart_home_lvgl_t *ui)
{
    lv_obj_t *row;
    lv_obj_t *card;
    lv_obj_t *header;
    lv_obj_t *body;
    int width;

    if (!ui || !ui->chat_list) {
        return;
    }

    /* Reuse the existing trace card across multiple ReAct iterations.
     * All tool calls from every iteration accumulate in the same card. */
    if (ui->chat_trace_body) {
        ui->chat_trace_active = 1;
        refresh_trace_header(ui);
        return;
    }

    ui->chat_trace_collapsed = 1;
    ui->chat_trace_active = 1;

    row = create_chat_row(ui, 0);
    lv_obj_set_style_pad_left(row, 44, 0);
    width = 696;
    card = lv_obj_create(row);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, width);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    smart_home_lvgl_card_style(card);
    smart_home_lvgl_set_bg(card, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 6, 0);

    smart_home_lvgl_icon_create(header, ICON_TOOL, 20, 20);
    ui->chat_trace_title = smart_home_lvgl_label_create(header,
                                                        "工具调用",
                                                        SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                                        15);
    lv_obj_set_flex_grow(ui->chat_trace_title, 1);
    ui->chat_trace_status = smart_home_lvgl_label_create(header,
                                                         "调用中",
                                                         SMART_HOME_UI_COLOR_WARNING,
                                                         14);
    lv_obj_set_size(ui->chat_trace_status, 112, 30);
    lv_obj_set_style_text_align(ui->chat_trace_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(ui->chat_trace_status, 6, 0);
    lv_obj_set_style_radius(ui->chat_trace_status, 15, 0);
    lv_obj_set_style_bg_opa(ui->chat_trace_status, LV_OPA_COVER, 0);

    body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 5, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_HIDDEN);

    smart_home_lvgl_press_feedback(header);
    lv_obj_add_event_cb(header, trace_header_event_cb, LV_EVENT_CLICKED, body);

    ui->chat_trace_row = row;
    ui->chat_trace_body = body;
    refresh_trace_header(ui);
    scroll_chat_to_bottom(ui);
}

void smart_home_lvgl_show_thinking(smart_home_lvgl_t *ui)
{
    if (ui && ui->chat_status) {
        lv_label_set_text(ui->chat_status, "正在思考…");
        lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
    }
}

void smart_home_lvgl_finish_thinking(smart_home_lvgl_t *ui)
{
    if (!ui) {
        return;
    }

    for (int i = 0; i < ui->chat_trace_tool_count; i++) {
        smart_home_lvgl_trace_tool_t *tool = &ui->chat_trace_tools[i];
        if (tool->state == -1) {
            /* A missing result is not evidence of a successful operation. */
            tool->state = -2;
            if (tool->status_label) {
                lv_label_set_text(tool->status_label, tool_status_text(-2));
                lv_obj_set_style_text_color(tool->status_label,
                                            tool_status_color(-2),
                                            0);
                lv_obj_invalidate(tool->status_label);
            }
        }
    }

    ui->chat_trace_active = 0;
    refresh_trace_header(ui);

    if (ui->chat_status) {
        lv_obj_add_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
    }

    scroll_chat_to_bottom(ui);

    /* Invalidate the operation summary so its final status is visible.
     * The caller (agent_done_async_cb) calls lv_refr_now() after
     * appending the response bubble. */
    if (ui->chat_trace_row) {
        lv_obj_invalidate(ui->chat_trace_row);
    }
}

void smart_home_lvgl_trace_tool(smart_home_lvgl_t *ui,
                                const char *name,
                                const char *call_id,
                                int ok)
{
    lv_obj_t *tool_row;
    lv_obj_t *label;
    smart_home_lvgl_trace_tool_t *tool;
    const char *id = call_id && call_id[0] ? call_id : "-";
    const char *tool_name = name && name[0] ? name : "tool";
    int slot = -1;

    if (!ui || !ui->chat_list) {
        return;
    }

    ensure_trace_card(ui);
    if (!ui->chat_trace_body) {
        return;
    }

    for (int i = 0; i < ui->chat_trace_tool_count; i++) {
        if (strcmp(ui->chat_trace_tools[i].id, id) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0 && ok >= 0) {
        for (int i = ui->chat_trace_tool_count - 1; i >= 0; i--) {
            if (ui->chat_trace_tools[i].state < 0 &&
                strcmp(ui->chat_trace_tools[i].name, tool_name) == 0) {
                slot = i;
                break;
            }
        }
    }

    if (slot >= 0) {
        tool = &ui->chat_trace_tools[slot];
        tool->state = ok;
        if (tool->status_label) {
            lv_label_set_text(tool->status_label, tool_status_text(ok));
            lv_obj_set_style_text_color(tool->status_label,
                                        tool_status_color(ok),
                                        0);
            lv_obj_invalidate(tool->status_label);
        }
        if (tool->row) {
            lv_obj_invalidate(tool->row);
        }
        refresh_trace_header(ui);
        scroll_chat_to_bottom(ui);
        return;
    }

    if (ui->chat_trace_tool_count >= SMART_HOME_LVGL_TRACE_TOOL_MAX) {
        scroll_chat_to_bottom(ui);
        return;
    }

    slot = ui->chat_trace_tool_count++;
    tool = &ui->chat_trace_tools[slot];
    strncpy(tool->id, id, sizeof(tool->id) - 1);
    strncpy(tool->name, tool_name, sizeof(tool->name) - 1);
    tool->state = ok;

    tool_row = lv_obj_create(ui->chat_trace_body);
    lv_obj_remove_style_all(tool_row);
    lv_obj_set_size(tool_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_clear_flag(tool_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(tool_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tool_row, 0, 0);
    lv_obj_set_style_pad_all(tool_row, 0, 0);
    lv_obj_set_flex_flow(tool_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tool_row,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tool_row, 6, 0);
    tool->row = tool_row;

    smart_home_lvgl_icon_create(tool_row, ICON_TOOL, 14, 14);
    label = smart_home_lvgl_label_create(tool_row,
                                         smart_home_lvgl_tool_display_name(tool_name),
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, smart_home_lvgl_content_w() * 48 / 100);
    lv_obj_set_flex_grow(label, 1);

    tool->status_label = smart_home_lvgl_label_create(tool_row,
                                                      tool_status_text(ok),
                                                      tool_status_color(ok),
                                                      12);
    refresh_trace_header(ui);
    scroll_chat_to_bottom(ui);
}

void smart_home_lvgl_append_tool_card(smart_home_lvgl_t *ui,
                                      const char *name,
                                      const char *call_id,
                                      int ok)
{
    smart_home_lvgl_trace_tool(ui, name, call_id, ok);
}

void smart_home_lvgl_append_error_bubble(smart_home_lvgl_t *ui,
                                         const char *msg)
{
    lv_obj_t *row;
    lv_obj_t *bubble;
    lv_obj_t *label;
    char text[256];
    int width;

    if (!ui || !ui->chat_list) {
        return;
    }

    row = create_chat_row(ui, 0);
    width = smart_home_lvgl_content_w() * 78 / 100;
    bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, width);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    smart_home_lvgl_card_style(bubble);
    smart_home_lvgl_set_bg(bubble, SMART_HOME_UI_COLOR_ERROR_BUBBLE_BG);
    lv_obj_set_style_border_side(bubble, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(bubble, 3, 0);
    lv_obj_set_style_border_color(bubble, SMART_HOME_UI_COLOR_WARNING, 0);

    snprintf(text, sizeof(text), "%s",
             msg && msg[0] ? msg : "暂时无法处理，请稍后重试。");
    label = smart_home_lvgl_label_create(bubble,
                                         text,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width - 20);
    scroll_chat_to_bottom(ui);
}

/* ── Submit a message from the UI or a native input source ───── */

void smart_home_lvgl_chat_send_text(smart_home_lvgl_t *ui, const char *text)
{
    if (!ui || !text || !text[0]) {
        return;
    }
    /* Preserve this turn's operation summary while a request is running. */
    if (ui->request_inflight) {
        smart_home_lvgl_append_error_bubble(ui,
                                           "正在处理上一条消息，请稍候。");
        return;
    }

#ifdef CONFIG_SMART_HOME_DEMO_DEBUG_LOG
    printf("[chat] user: %.200s\n", text);
#endif

    /* Clear trace card state so a new card is created for this turn.
     * The old card stays in the chat history as a record of previous
     * tool calls — only the pointers are reset. */
    ui->chat_trace_body = NULL;
    ui->chat_trace_row = NULL;
    ui->chat_trace_title = NULL;
    ui->chat_trace_status = NULL;
    ui->chat_trace_active = 0;
    ui->chat_trace_collapsed = 0;
    reset_active_trace_tools(ui);

    /* Show user bubble */
    smart_home_lvgl_append_msg_bubble(ui, text, 1);

    /* Submit to worker thread */
    if (smart_home_lvgl_submit_agent_job(ui, text) != AGENT_OK) {
        return;
    }
    if (ui->chat_send_btn) {
        lv_obj_add_state(ui->chat_send_btn, LV_STATE_DISABLED);
    }

    /* Update status hint */
    if (ui->chat_status) {
        lv_label_set_text(ui->chat_status,
                          "正在思考…");
        lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ── build ─────────────────────────────────────────────────── */

int smart_home_lvgl_chat_clear(smart_home_lvgl_t *ui)
{
    int ret;
    if (!ui || !ui->app) return AGENT_ERROR_INVALID;
    if (ui->request_inflight) return AGENT_ERROR_BUSY;
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    if (ui->asr_active || ui->asr_worker_active) return AGENT_ERROR_BUSY;
#endif
    ret = smart_home_agent_app_clear_session(ui->app);
    if (ret != AGENT_OK) return ret;
    if (ui->chat_list) lv_obj_clean(ui->chat_list);
    ui->chat_trace_body = NULL;
    ui->chat_trace_row = NULL;
    ui->chat_trace_title = NULL;
    ui->chat_trace_status = NULL;
    ui->chat_trace_active = 0;
    ui->chat_trace_collapsed = 0;
    ui->chat_msg_count = 0;
    reset_active_trace_tools(ui);
    if (ui->chat_input) lv_textarea_set_text(ui->chat_input, "");
    layout_chat_controls(ui, 0);
    if (ui->chat_status) {
        lv_label_set_text(ui->chat_status, "已开始新对话");
        lv_obj_clear_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
    }
    return AGENT_OK;
}

static void chat_new_event_cb(lv_event_t *event)
{
    smart_home_lvgl_t *ui = lv_event_get_user_data(event);
    int ret = smart_home_lvgl_chat_clear(ui);
    if (ret == AGENT_ERROR_BUSY)
        smart_home_lvgl_append_error_bubble(ui, "请等待当前回复或录音结束后再开始新对话。");
    else if (ret != AGENT_OK)
        smart_home_lvgl_append_error_bubble(ui, "无法清空会话，请重试。");
}

void smart_home_lvgl_build_chat_screen(smart_home_lvgl_t *ui)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_t *input_bar;
    lv_obj_t *label;
    lv_obj_t *button;
    lv_obj_t *mic;
    int content_w = chat_content_width();

    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    lv_obj_set_style_text_font(screen, smart_home_lvgl_font(16), 0);
    ui->screen_chat = screen;
    smart_home_lvgl_build_top_bar(screen, ui, "聊天");
    label = smart_home_lvgl_label_create(screen, "智能管家",
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY, 24);
    lv_obj_set_pos(label, 24, 76);

    /* Preserve session clearing without taking space from the input row. */
    button = lv_button_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 96, 36);
    lv_obj_set_pos(button, smart_home_lvgl_disp_w() - 120, 76);
    lv_obj_set_style_radius(button, 10, 0);
    smart_home_lvgl_set_bg(button, SMART_HOME_UI_COLOR_SURFACE);
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, chat_new_event_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(button, "新对话",
                                         SMART_HOME_UI_COLOR_TEXT_SECONDARY, 14);
    lv_obj_center(label);

    ui->chat_list = lv_obj_create(screen);
    lv_obj_remove_style_all(ui->chat_list);
    lv_obj_set_size(ui->chat_list, content_w, chat_list_height(0));
    lv_obj_set_flex_flow(ui->chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ui->chat_list, 0, 0);
    lv_obj_set_style_pad_row(ui->chat_list, 12, 0);
    smart_home_lvgl_set_bg(ui->chat_list, SMART_HOME_UI_COLOR_BG);
    lv_obj_set_scrollbar_mode(ui->chat_list, LV_SCROLLBAR_MODE_AUTO);

    input_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(input_bar);
    lv_obj_set_size(input_bar, content_w, SMART_HOME_CHAT_INPUT_H);
    smart_home_lvgl_set_bg(input_bar, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_set_style_border_color(input_bar, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(input_bar, 1, 0);
    lv_obj_set_style_radius(input_bar, 18, 0);
    lv_obj_clear_flag(input_bar, LV_OBJ_FLAG_SCROLLABLE);
    ui->chat_input_bar = input_bar;

    button = lv_button_create(input_bar);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, 48, 48);
    lv_obj_align(button, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(button, 12, 0);
    mic = smart_home_lvgl_icon_create(button, "asset:microphone", 32, 32);
    if (mic) lv_obj_center(mic);
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    ui->chat_mic_btn = button;
    ui->chat_mic_label = mic;
    smart_home_lvgl_press_feedback(button);
    lv_obj_add_event_cb(button, chat_mic_event_cb, LV_EVENT_CLICKED, ui);
#else
    if (mic) lv_image_set_src(mic,
        smart_home_lvgl_png_icon_get("asset:microphone-off", 32));
    lv_obj_add_state(button, LV_STATE_DISABLED);
#endif

    ui->chat_input = lv_textarea_create(input_bar);
    lv_obj_set_size(ui->chat_input, content_w - 272, 48);
    lv_textarea_set_one_line(ui->chat_input, true);
    lv_textarea_set_placeholder_text(ui->chat_input, "说说你的想法，或输入指令…");
    smart_home_lvgl_style_input(ui->chat_input, 17);
    lv_obj_align(ui->chat_input, LV_ALIGN_LEFT_MID, 64, 0);
    /* 单行文本与占位文字按当前 MiSans 行高垂直居中。 */
    lv_obj_set_style_pad_ver(ui->chat_input,
        (48 - lv_font_get_line_height(smart_home_lvgl_font(17)) - 2) / 2, 0);
    lv_obj_add_event_cb(ui->chat_input, chat_input_event_cb, LV_EVENT_ALL, ui);

    ui->chat_send_btn = lv_button_create(input_bar);
    lv_obj_remove_style_all(ui->chat_send_btn);
    lv_obj_set_size(ui->chat_send_btn, 168, 40);
    lv_obj_align(ui->chat_send_btn, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_set_ext_click_area(ui->chat_send_btn, 4);
    smart_home_lvgl_set_bg(ui->chat_send_btn, SMART_HOME_UI_COLOR_PRIMARY_DARK);
    lv_obj_set_style_radius(ui->chat_send_btn, 12, 0);
    smart_home_lvgl_press_feedback(ui->chat_send_btn);
    lv_obj_add_event_cb(ui->chat_send_btn, chat_send_event_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(ui->chat_send_btn, "发送",
                                         SMART_HOME_UI_COLOR_SURFACE, 18);
    lv_obj_center(label);

    ui->chat_keyboard = lv_keyboard_create(screen);
    smart_home_lvgl_style_keyboard(ui->chat_keyboard);
    lv_keyboard_set_textarea(ui->chat_keyboard, ui->chat_input);
    lv_obj_add_event_cb(ui->chat_keyboard, chat_keyboard_event_cb, LV_EVENT_ALL, ui);
    lv_obj_add_flag(ui->chat_keyboard, LV_OBJ_FLAG_HIDDEN);
    ui->chat_status = smart_home_lvgl_label_create(screen, "",
        SMART_HOME_UI_COLOR_TEXT_SECONDARY, 12);
    lv_obj_add_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);
    smart_home_lvgl_build_nav_bar(screen, ui);
    smart_home_lvgl_update_clock(ui);
    layout_chat_controls(ui, 0);
}
