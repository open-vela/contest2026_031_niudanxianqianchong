/**
 * smart_home LVGL chat screen (read-only message flow).
 *
 * Input comes from NSH command line ("smart_home <text>").  This screen
 * displays the chat history with user bubbles, agent bubbles, tool-call
 * cards, and error bubbles.
 * bubbles, agent bubbles, tool-call cards, and error bubbles.
 */

#include "smart_home_lvgl_internal.h"
#include "images/smart_home_icons.h"

#include "../../config/smart_home_config.h"
#ifdef CONFIG_SMART_HOME_VOICE_ASR
#include <pthread.h>
#include "../../smart_home_memory.h"
#include "../../voice/smart_home_asr.h"
#include "../../voice/smart_home_voice_capture.h"
#endif

#include <stdio.h>
#include <string.h>

#define SMART_HOME_CHAT_INPUT_H 34
#define SMART_HOME_CHAT_HEADER_H (SMART_HOME_TOPBAR_H + 12)

static int chat_list_height(int keyboard_visible)
{
    int reserved = SMART_HOME_CHAT_HEADER_H + SMART_HOME_NAV_H +
                   SMART_HOME_NAV_BOTTOM_PAD + SMART_HOME_CHAT_INPUT_H + 16;
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

    content_w = smart_home_lvgl_content_w();
    input_y = -SMART_HOME_NAV_H - SMART_HOME_NAV_BOTTOM_PAD - 8;

    if (ui->chat_list) {
        lv_obj_set_size(ui->chat_list,
                        content_w,
                        chat_list_height(keyboard_visible));
        lv_obj_align(ui->chat_list,
                     LV_ALIGN_TOP_MID,
                     0,
                     SMART_HOME_TOPBAR_H + 12);
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

    if (!ui || !ui->chat_input) {
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
        lv_label_set_text(ui->chat_mic_label, "语音");
    }
}

void smart_home_lvgl_chat_asr_begin(smart_home_lvgl_t *ui)
{
    smart_home_asr_config_t config;
    int ret;

    if (!ui) {
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

    ui->asr_active = 1;
    ui->asr_abort = 0;
    if (ui->chat_mic_btn) {
        smart_home_lvgl_set_bg(ui->chat_mic_btn, SMART_HOME_UI_COLOR_DANGER);
    }
    if (ui->chat_mic_label) {
        lv_label_set_text(ui->chat_mic_label, "停止");
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
        return;
    }

    smart_home_lvgl_chat_asr_begin(ui);
}
#endif

void smart_home_lvgl_append_msg_bubble(smart_home_lvgl_t *ui,
                                       const char *text,
                                       int is_user)
{
    lv_obj_t *row;
    lv_obj_t *bubble;
    lv_obj_t *label;
    int width;

    if (!ui || !ui->chat_list || !text) {
        return;
    }

    row = create_chat_row(ui, is_user);
    width = smart_home_lvgl_content_w() * 74 / 100;
    bubble = lv_obj_create(row);
    lv_obj_remove_style_all(bubble);
    lv_obj_set_width(bubble, width);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(bubble, 10, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    smart_home_lvgl_set_bg(bubble,
                           is_user ? SMART_HOME_UI_COLOR_USER_BUBBLE :
                                     SMART_HOME_UI_COLOR_SURFACE);

    label = smart_home_lvgl_label_create(bubble,
                                         text,
                                         is_user ? SMART_HOME_UI_COLOR_PRIMARY :
                                                   SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width - 20);
    scroll_chat_to_bottom(ui);
}

static const char *tool_status_text(int ok)
{
    if (ok == -2) return "Unconfirmed";
    if (ok > 0) {
        return "OK";
    }
    if (ok == 0) {
        return "Failed";
    }
    return "Running";
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
    int has_unconfirmed = 0;

    if (!ui || !ui->chat_trace_status) {
        return;
    }

    for (int i = 0; i < ui->chat_trace_tool_count; i++) {
        if (ui->chat_trace_tools[i].state == -1) {
            has_running = 1;
        } else if (ui->chat_trace_tools[i].state == -2) {
            has_unconfirmed = 1;
        } else if (ui->chat_trace_tools[i].state == 0) {
            has_failed = 1;
        }
    }

    if (has_running) {
        lv_label_set_text(ui->chat_trace_status, "Running");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_WARNING,
                                    0);
    } else if (has_failed) {
        lv_label_set_text(ui->chat_trace_status, "Failed");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_DANGER,
                                    0);
    } else if (has_unconfirmed) {
        lv_label_set_text(ui->chat_trace_status, "Unconfirmed");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_WARNING, 0);
    } else if (ui->chat_trace_tool_count > 0) {
        lv_label_set_text(ui->chat_trace_status, "OK");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_SUCCESS,
                                    0);
    } else if (ui->chat_trace_active) {
        lv_label_set_text(ui->chat_trace_status, "Running");
        lv_obj_set_style_text_color(ui->chat_trace_status,
                                    SMART_HOME_UI_COLOR_WARNING,
                                    0);
    } else {
        if (ui->chat_trace_title) {
            lv_label_set_text(ui->chat_trace_title, "Thinking...done");
        }
        lv_label_set_text(ui->chat_trace_status, "Done");
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

    ui->chat_trace_collapsed = 0;
    ui->chat_trace_active = 1;

    row = create_chat_row(ui, 0);
    width = smart_home_lvgl_content_w() * 78 / 100;
    card = lv_obj_create(row);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, width);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    smart_home_lvgl_card_style(card);
    smart_home_lvgl_set_bg(card, SMART_HOME_UI_COLOR_TOOL_CARD_BG);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_border_color(card, SMART_HOME_UI_COLOR_WARNING, 0);
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

    smart_home_lvgl_icon_create(header, ICON_LOADING, 14, 14);
    ui->chat_trace_title = smart_home_lvgl_label_create(header,
                                                        "Thinking...",
                                                        SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                                        12);
    lv_obj_set_flex_grow(ui->chat_trace_title, 1);
    ui->chat_trace_status = smart_home_lvgl_label_create(header,
                                                         "Running",
                                                         SMART_HOME_UI_COLOR_WARNING,
                                                         12);

    body = lv_obj_create(card);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_set_style_pad_row(body, 5, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(header, trace_header_event_cb, LV_EVENT_CLICKED, body);

    ui->chat_trace_row = row;
    ui->chat_trace_body = body;
    refresh_trace_header(ui);
    scroll_chat_to_bottom(ui);
}

void smart_home_lvgl_show_thinking(smart_home_lvgl_t *ui)
{
    ensure_trace_card(ui);
}

void smart_home_lvgl_finish_thinking(smart_home_lvgl_t *ui)
{
    if (!ui) {
        return;
    }

    for (int i = 0; i < ui->chat_trace_tool_count; i++) {
        smart_home_lvgl_trace_tool_t *tool = &ui->chat_trace_tools[i];
        if (tool->state == -1) {
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
    ui->chat_trace_collapsed = 0;
    refresh_trace_header(ui);

    /* Unhide the trace card body so tool call details are visible */
    if (ui->chat_trace_body) {
        lv_obj_clear_flag(ui->chat_trace_body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(ui->chat_trace_body);
    }

    scroll_chat_to_bottom(ui);

    /* Invalidate the trace card so the "Done" status is visible.
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
    char text[128];
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
    snprintf(text,
             sizeof(text),
             "%s  %s",
             tool_name,
             id);
    label = smart_home_lvgl_label_create(tool_row,
                                         text,
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
    const char *status_text;
    const char *status_icon;
    lv_color_t status_color;
    lv_obj_t *row;
    lv_obj_t *card;
    lv_obj_t *header;
    lv_obj_t *icon;
    lv_obj_t *label;
    lv_obj_t *id_label;
    char text[256];
    int width;

    if (!ui || !ui->chat_list) {
        return;
    }

    if (ok > 0) {
        status_text = "OK";
        status_icon = ICON_STATUS_OK;
        status_color = SMART_HOME_UI_COLOR_SUCCESS;
    } else if (ok == 0) {
        status_text = "Failed";
        status_icon = ICON_STATUS_FAIL;
        status_color = SMART_HOME_UI_COLOR_DANGER;
    } else {
        status_text = "Running";
        status_icon = ICON_LOADING;
        status_color = SMART_HOME_UI_COLOR_WARNING;
    }

    row = create_chat_row(ui, 0);
    width = smart_home_lvgl_content_w() * 78 / 100;
    card = lv_obj_create(row);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, width);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(card, 10, 0);
    smart_home_lvgl_card_style(card);
    smart_home_lvgl_set_bg(card, SMART_HOME_UI_COLOR_TOOL_CARD_BG);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_border_color(card,
                                  ok == 0 ? SMART_HOME_UI_COLOR_DANGER :
                                            SMART_HOME_UI_COLOR_WARNING,
                                  0);

    /* Header row: tool icon + name + status icon + status text */
    header = lv_obj_create(card);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(header, 6, 0);

    icon = smart_home_lvgl_icon_create(header, ICON_TOOL, 16, 16);
    (void)icon;

    snprintf(text, sizeof(text), "%s", name ? name : "tool");
    label = smart_home_lvgl_label_create(header,
                                         text,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         12);
    lv_obj_set_flex_grow(label, 1);

    icon = smart_home_lvgl_icon_create(header, status_icon, 14, 14);
    (void)icon;

    label = smart_home_lvgl_label_create(header,
                                         status_text,
                                         status_color,
                                         12);

    /* Call ID line */
    if (call_id && call_id[0]) {
        snprintf(text, sizeof(text), "id: %s", call_id);
        id_label = smart_home_lvgl_label_create(card,
                                                 text,
                                                 SMART_HOME_UI_COLOR_TEXT_MUTED,
                                                 12);
        (void)id_label;
    }

    scroll_chat_to_bottom(ui);
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

    snprintf(text, sizeof(text), "Error: %s", msg ? msg : "no detail");
    label = smart_home_lvgl_label_create(bubble,
                                         text,
                                         SMART_HOME_UI_COLOR_TEXT_PRIMARY,
                                         12);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, width - 20);
    scroll_chat_to_bottom(ui);
}

/* ── chat_send_text: used by NSH command-line input path ───── */

void smart_home_lvgl_chat_send_text(smart_home_lvgl_t *ui, const char *text)
{
    if (!ui || !text || !text[0]) {
        return;
    }
    if (ui->request_inflight) {
        smart_home_lvgl_append_error_bubble(ui, "请等待当前回复结束。");
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
    smart_home_lvgl_submit_agent_job(ui, text);

    /* Update status hint */
    if (ui->chat_status) {
        lv_label_set_text(ui->chat_status,
                          "正在请求智能管家…");
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
    lv_obj_t *screen;
    lv_obj_t *input_bar;
    lv_obj_t *label;
    lv_obj_t *new_button;
    int content_w = smart_home_lvgl_content_w();

    screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(screen);
    smart_home_lvgl_set_bg(screen, SMART_HOME_UI_COLOR_BG);
    ui->screen_chat = screen;

    smart_home_lvgl_build_top_bar(screen, ui, "智能管家");

    /* Chat message list — scrollable, fills most of the screen */
    ui->chat_list = lv_obj_create(screen);
    lv_obj_remove_style_all(ui->chat_list);
    lv_obj_set_size(ui->chat_list,
                    content_w,
                    chat_list_height(0));
    lv_obj_align(ui->chat_list,
                 LV_ALIGN_TOP_MID,
                 0,
                 SMART_HOME_TOPBAR_H + 12);
    lv_obj_set_flex_flow(ui->chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ui->chat_list, 4, 0);
    lv_obj_set_style_pad_row(ui->chat_list, 8, 0);
    smart_home_lvgl_set_bg(ui->chat_list, SMART_HOME_UI_COLOR_BG);

    input_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(input_bar);
    lv_obj_set_size(input_bar, content_w, SMART_HOME_CHAT_INPUT_H);
    lv_obj_set_flex_flow(input_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(input_bar,
                          LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(input_bar, 0, 0);
    lv_obj_set_style_pad_column(input_bar, 8, 0);
    smart_home_lvgl_set_bg(input_bar, SMART_HOME_UI_COLOR_BG);
    ui->chat_input_bar = input_bar;

    ui->chat_input = lv_textarea_create(input_bar);
#ifdef CONFIG_SMART_HOME_VOICE_ASR
    lv_obj_set_size(ui->chat_input, content_w - 208, SMART_HOME_CHAT_INPUT_H);
#else
    lv_obj_set_size(ui->chat_input, content_w - 152, SMART_HOME_CHAT_INPUT_H);
#endif
    lv_textarea_set_one_line(ui->chat_input, true);
    lv_textarea_set_placeholder_text(ui->chat_input, "问问 OpenVela…");
    lv_obj_set_style_text_font(ui->chat_input, smart_home_lvgl_font(12), 0);
    lv_obj_set_style_radius(ui->chat_input, 8, 0);
    lv_obj_set_style_border_color(ui->chat_input, SMART_HOME_UI_COLOR_BORDER, 0);
    lv_obj_add_event_cb(ui->chat_input,
                        chat_input_event_cb,
                        LV_EVENT_ALL,
                        ui);

    new_button = lv_btn_create(input_bar);
    lv_obj_remove_style_all(new_button);
    lv_obj_set_size(new_button, 74, SMART_HOME_CHAT_INPUT_H);
    lv_obj_set_style_radius(new_button, 8, 0);
    smart_home_lvgl_set_bg(new_button, SMART_HOME_UI_COLOR_SURFACE);
    lv_obj_add_event_cb(new_button, chat_new_event_cb, LV_EVENT_CLICKED, ui);
    label = smart_home_lvgl_label_create(new_button, "新对话",
                                         SMART_HOME_UI_COLOR_PRIMARY, 12);
    lv_obj_center(label);

#ifdef CONFIG_SMART_HOME_VOICE_ASR
    ui->chat_mic_btn = lv_btn_create(input_bar);
    lv_obj_remove_style_all(ui->chat_mic_btn);
    lv_obj_set_size(ui->chat_mic_btn, 48, SMART_HOME_CHAT_INPUT_H);
    lv_obj_set_style_radius(ui->chat_mic_btn, 8, 0);
    smart_home_lvgl_set_bg(ui->chat_mic_btn, SMART_HOME_UI_COLOR_PRIMARY);
    lv_obj_add_event_cb(ui->chat_mic_btn,
                        chat_mic_event_cb,
                        LV_EVENT_CLICKED,
                        ui);
    ui->chat_mic_label = smart_home_lvgl_label_create(ui->chat_mic_btn,
                                                      "语音",
                                                      lv_color_white(),
                                                      12);
    lv_obj_center(ui->chat_mic_label);
#endif

    ui->chat_send_btn = lv_btn_create(input_bar);
    lv_obj_remove_style_all(ui->chat_send_btn);
    lv_obj_set_size(ui->chat_send_btn, 62, SMART_HOME_CHAT_INPUT_H);
    lv_obj_set_style_radius(ui->chat_send_btn, 8, 0);
    smart_home_lvgl_set_bg(ui->chat_send_btn, SMART_HOME_UI_COLOR_PRIMARY);
    lv_obj_add_event_cb(ui->chat_send_btn,
                        chat_send_event_cb,
                        LV_EVENT_CLICKED,
                        ui);
    label = smart_home_lvgl_label_create(ui->chat_send_btn,
                                         "发送",
                                         lv_color_white(),
                                         12);
    lv_obj_center(label);

    ui->chat_keyboard = lv_keyboard_create(screen);
    lv_keyboard_set_textarea(ui->chat_keyboard, ui->chat_input);
    smart_home_lvgl_style_keyboard(ui->chat_keyboard);
    lv_obj_add_event_cb(ui->chat_keyboard,
                        chat_keyboard_event_cb,
                        LV_EVENT_ALL,
                        ui);
    lv_obj_add_flag(ui->chat_keyboard, LV_OBJ_FLAG_HIDDEN);

    ui->chat_status = smart_home_lvgl_label_create(screen,
                                                    "",
                                                    SMART_HOME_UI_COLOR_TEXT_SECONDARY,
                                                    12);
    lv_obj_align(ui->chat_status,
                 LV_ALIGN_BOTTOM_MID,
                 0,
                 -SMART_HOME_NAV_H - SMART_HOME_NAV_BOTTOM_PAD - 44);
    lv_obj_add_flag(ui->chat_status, LV_OBJ_FLAG_HIDDEN);

    /* Nav bar */
    smart_home_lvgl_build_nav_bar(screen, ui);
    layout_chat_controls(ui, 0);
}
