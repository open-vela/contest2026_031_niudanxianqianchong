/* SPDX-License-Identifier: Apache-2.0 */
#include "ui/lvgl/smart_home_lvgl_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/ui/lvgl/smart_home_lvgl_chat.c"

static int clear_calls, cleaned;
static int clear_status;
static lv_obj_t *tool_label, *header_label;
static char tool_text[32], header_text[32];
int smart_home_agent_app_clear_session(smart_home_agent_app_t *app)
{ assert(app); ++clear_calls; return clear_status; }
void lv_obj_clean(lv_obj_t *obj) { assert(obj); ++cleaned; }
void lv_textarea_set_text(lv_obj_t *obj, const char *text) { assert(obj && !text[0]); }
void lv_obj_add_flag(lv_obj_t *obj, lv_obj_flag_t flag) { (void)obj; (void)flag; }
void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t flag) { (void)obj; (void)flag; }
void lv_obj_set_size(lv_obj_t *obj, int32_t width, int32_t height) { (void)obj; assert(width > 0 && height > 0); }
void lv_obj_align(lv_obj_t *obj, lv_align_t align, int32_t x, int32_t y)
{ (void)obj; (void)align; (void)x; (void)y; }
void lv_obj_align_to(lv_obj_t *obj, const lv_obj_t *base, lv_align_t align, int32_t x, int32_t y)
{ (void)obj; (void)base; (void)align; (void)x; (void)y; }
lv_display_t *lv_display_get_default(void) { return NULL; }
int32_t lv_display_get_horizontal_resolution(const lv_display_t *display) { (void)display; return 1024; }
int32_t lv_display_get_vertical_resolution(const lv_display_t *display) { (void)display; return 600; }
void lv_label_set_text(lv_obj_t *obj, const char *text)
{
    if (obj == tool_label) snprintf(tool_text, sizeof(tool_text), "%s", text);
    if (obj == header_label) snprintf(header_text, sizeof(header_text), "%s", text);
}
void lv_obj_set_style_text_color(lv_obj_t *obj, lv_color_t color, lv_style_selector_t selector)
{ (void)obj; (void)color; (void)selector; }
void lv_obj_invalidate(const lv_obj_t *obj) { (void)obj; }
void lv_obj_update_layout(const lv_obj_t *obj) { (void)obj; }
void lv_obj_scroll_to_y(lv_obj_t *obj, int32_t y, lv_anim_enable_t animation)
{ (void)obj; (void)y; (void)animation; }
int main(void)
{
    smart_home_agent_app_t app = {0};
    smart_home_lvgl_t ui = {0};
    int objects[6];
    ui.app = &app;
    tool_label = (lv_obj_t *)&objects[0];
    header_label = (lv_obj_t *)&objects[1];
    ui.chat_list = (lv_obj_t *)&objects[2];
    ui.chat_input = (lv_obj_t *)&objects[3];
    ui.chat_keyboard = (lv_obj_t *)&objects[4];
    ui.chat_status = (lv_obj_t *)&objects[5];
    ui.chat_trace_status = header_label;
    ui.chat_trace_tool_count = 1;
    ui.chat_trace_tools[0].state = -1;
    ui.chat_trace_tools[0].status_label = tool_label;
    smart_home_lvgl_finish_thinking(&ui);
    assert(ui.chat_trace_tools[0].state == -2);
    assert(!strcmp(tool_text, "未确认") && !strcmp(header_text, "结果待确认"));
    ui.chat_trace_tools[0].state = 1;
    smart_home_lvgl_finish_thinking(&ui);
    assert(!strcmp(header_text, "已返回"));
    ui.request_inflight = 1;
    assert(smart_home_lvgl_chat_clear(&ui) == AGENT_ERROR_BUSY && clear_calls == 0 && cleaned == 0);
    ui.request_inflight = 0;
    clear_status = AGENT_ERROR_IO;
    assert(smart_home_lvgl_chat_clear(&ui) == AGENT_ERROR_IO && cleaned == 0 && ui.chat_trace_status);
    clear_status = AGENT_OK;
    assert(!smart_home_lvgl_chat_clear(&ui) && cleaned == 1 && clear_calls == 2);
    assert(!ui.chat_trace_row && !ui.chat_trace_body && !ui.chat_trace_status && !ui.chat_trace_title);
    assert(ui.chat_trace_tool_count == 0 && ui.chat_msg_count == 0);
    puts("PASS: real chat clear/reset, busy/failure preservation and unconfirmed tool state");
    return 0;
}
