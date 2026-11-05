/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse device stubs; callbacks and the reminder worker are real. */
#define main legacy_tools_test_main
#include "../tools_v2_test.c"
#undef main
#include "reminder/smart_home_reminder.h"
#include <agent/context.h>
#include <stdlib.h>
#include <unistd.h>

agent_error_t agent_register_context(agent_t *agent, const agent_context_provider_t *provider)
{
    (void)agent;
    assert(provider->required && provider->placement==AGENT_CONTEXT_INSTRUCTIONS);
    assert(provider->max_bytes>=512);
    const agent_text_sink_t sink = {write_output,NULL};
    assert(provider->build(NULL,NULL,&sink)==AGENT_OK);
    assert(strlen(output)<=provider->max_bytes && strstr(output,"persistent reminders"));
    return AGENT_OK;
}

static int64_t clock_wall(void *arg) { (void)arg; return INT64_C(1793718000); }
static int64_t clock_mono(void *arg) { (void)arg; return 100; }

int main(void)
{
    char dir[]="/tmp/reminder-tools-XXXXXX", path[256];
    smart_home_reminder_snapshot_t snapshot;
    smart_home_device_service_t service = {0};
    assert(mkdtemp(dir));
    snprintf(path,sizeof(path),"%s/reminders.json",dir);
    smart_home_reminder_options_t options={path,clock_wall,clock_mono,NULL};
    assert(!smart_home_reminder_start(&options));
    for (int i=0;i<200;++i) {
        smart_home_reminder_snapshot(&snapshot);
        if (snapshot.ready) break;
        usleep(10000);
    }
    assert(snapshot.ready);
    assert(!smart_home_tools_register((agent_t *)&service,&service));
    assert(count==9);
    assert(!invoke("set_timer","{\"delay_seconds\":60,\"message\":\"喝水\\n测试\"}"));
    assert(invoke("set_timer","{\"delay_seconds\":0,\"message\":\"bad\"}")==AGENT_ERROR_INVALID);
    assert(invoke("set_timer","{\"delay_seconds\":1.5,\"message\":\"bad\"}")==AGENT_ERROR_TOOL_ARGUMENT);
    assert(invoke("set_timer","{\"delay_seconds\":10,\"message\":\"bad\",\"name\":false}")!=AGENT_OK);
    assert(invoke("set_alarm","{\"at_local\":\"2027-02-29T08:00:00\",\"message\":\"bad\"}")!=AGENT_OK);
    assert(!invoke("set_alarm","{\"at_local\":\"2028-02-29T08:00:00\",\"message\":\"起床\"}"));
    assert(!invoke("list_timers","{}"));
    assert(strstr(output,"UTC+08:00") && strstr(output,"now_local") && strstr(output,"alarm"));
    assert(!invoke("cancel_timer","{\"id\":1}"));
    assert(!invoke("cancel_timer","{\"id\":2}"));
    assert(invoke("cancel_timer","{\"id\":2}")==AGENT_ERROR_NOT_FOUND);
    smart_home_reminder_stop();
    unlink(path); rmdir(dir);
    puts("PASS: real timer/alarm tool callbacks, schemas, time query and persistence service");
    return 0;
}
