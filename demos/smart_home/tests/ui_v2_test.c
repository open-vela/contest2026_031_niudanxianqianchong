/* SPDX-License-Identifier: Apache-2.0 */
#include "ui/lvgl/smart_home_lvgl_internal.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_int reject_allocations;
static void *ui_test_calloc(size_t count, size_t size)
{
    if (atomic_load(&reject_allocations)) return NULL;
    return calloc(count, size);
}
#define calloc ui_test_calloc
#include "../src/ui/lvgl/smart_home_lvgl_agent.c"
#undef calloc

static pthread_t ui_thread;
static int thinking, finished, traces, replies, errors, refreshes;
static char traced_name[80], traced_id[80];
static void on_ui(void) { assert(pthread_equal(pthread_self(), ui_thread)); }
void smart_home_lvgl_show_thinking(smart_home_lvgl_t *ui) { (void)ui; on_ui(); ++thinking; }
void smart_home_lvgl_finish_thinking(smart_home_lvgl_t *ui) { (void)ui; on_ui(); ++finished; }
void smart_home_lvgl_refresh_cards(smart_home_lvgl_t *ui) { (void)ui; on_ui(); ++refreshes; }
void smart_home_lvgl_trace_tool(smart_home_lvgl_t *ui, const char *name, const char *id, int status)
{
    (void)ui; (void)status; on_ui(); ++traces;
    snprintf(traced_name, sizeof(traced_name), "%s", name);
    snprintf(traced_id, sizeof(traced_id), "%s", id);
}
void smart_home_lvgl_append_msg_bubble(smart_home_lvgl_t *ui, const char *text, int user)
{ (void)ui; (void)user; on_ui(); assert(!strcmp(text, "answer")); ++replies; }
void smart_home_lvgl_append_error_bubble(smart_home_lvgl_t *ui, const char *text)
{ (void)ui; on_ui(); assert(text && text[0]); ++errors; }
void lv_label_set_text(lv_obj_t *obj, const char *text) { (void)obj; (void)text; on_ui(); }
void lv_obj_remove_flag(lv_obj_t *obj, lv_obj_flag_t flag) { (void)obj; (void)flag; on_ui(); }
void lv_refr_now(lv_display_t *display) { (void)display; on_ui(); }
void smart_home_cpu_debug_log(const char *stage) { (void)stage; }
void smart_home_memory_log_region(const char *name, const void *p) { (void)name; (void)p; }
void smart_home_bulk_diag(const char *point) { (void)point; }
void *smart_home_bulk_alloc(size_t size) { return malloc(size); }
void smart_home_bulk_free(void *p) { free(p); }
static smart_home_lvgl_t *current_ui;
int smart_home_agent_run(smart_home_agent_app_t *app, const char *input, char *out, size_t capacity)
{
    char name[] = "set_light";
    char id[] = "call1";
    agent_event_t event = {.type = AGENT_EVENT_MODEL_BEGIN};
    (void)app;
    smart_home_lvgl_event_cb(current_ui, &event);
    event.tool_call.name = agent_string_view(name, sizeof(name) - 1);
    event.tool_call.id = agent_string_view(id, sizeof(id) - 1);
    event.type = AGENT_EVENT_TOOL_BEGIN;
    smart_home_lvgl_event_cb(current_ui, &event);
    event.type = AGENT_EVENT_TOOL_END;
    smart_home_lvgl_event_cb(current_ui, &event);
    event.type = AGENT_EVENT_TURN_END;
    event.status = !strcmp(input, "failure") ? AGENT_ERROR_AUTH : AGENT_OK;
    smart_home_lvgl_event_cb(current_ui, &event);
    memset(name, 'x', sizeof(name));
    memset(id, 'x', sizeof(id));
    snprintf(out, capacity, "answer");
    /* Heap pressure after model completion must not lose the queued completion. */
    atomic_store(&reject_allocations, 1);
    return !strcmp(input, "failure") ? AGENT_ERROR_AUTH :
           !strcmp(input, "truncated") ? AGENT_ERROR_TRUNCATED : AGENT_OK;
}
static void run_job(smart_home_lvgl_t *ui, const char *input)
{
    struct timespec start, now;
    atomic_store(&reject_allocations, 0);
    assert(!smart_home_lvgl_submit_agent_job(ui, input));
    assert(!clock_gettime(CLOCK_MONOTONIC, &start));
    while (ui->request_inflight) {
        smart_home_lvgl_process_pending(ui);
        sched_yield();
        assert(!clock_gettime(CLOCK_MONOTONIC, &now));
        assert(now.tv_sec - start.tv_sec < 5);
    }
    assert(!ui->agent_worker_active && !ui->pending_head && !ui->pending_tail);
    assert(!strcmp(traced_name, "set_light") && !strcmp(traced_id, "call1"));
}
int main(void)
{
    smart_home_lvgl_t ui = {0};
    smart_home_agent_app_t app = {0};
    ui_thread = pthread_self();
    ui.app = &app;
    current_ui = &ui;
    assert(!pthread_mutex_init(&ui.pending_mutex, NULL));
    run_job(&ui, "success");
    assert(replies == 1 && errors == 0 && traces == 2 && thinking == 1 && finished >= 1 && refreshes >= 1);
    run_job(&ui, "failure");
    assert(replies == 1 && errors == 1); /* Exactly one error, despite TURN_END and completion. */
    run_job(&ui, "truncated");
    assert(replies == 2 && errors == 2); /* Show the partial answer plus a truncation notice. */
    smart_home_lvgl_discard_pending(&ui);
    smart_home_bulk_free(ui.agent_worker_stack_alloc);
    assert(!pthread_mutex_destroy(&ui.pending_mutex));
    puts("PASS: real LVGL bridge queue, UI-thread delivery, copied views, terminal errors, truncation and completion under OOM");
    return 0;
}
