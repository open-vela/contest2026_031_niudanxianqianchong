/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_reminder_tools.h"
#include "smart_home_reminder.h"
#include "../tools/smart_home_tools.h"
#include "../tools/smart_home_tool_io.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static agent_error_t reply(const agent_text_sink_t *sink, int status, int id)
{
    char output[256];
    snprintf(output,sizeof(output),
        "{\"ok\":%s,\"id\":%d,\"code\":%d,\"error\":\"%s\"}",
        status == AGENT_OK ? "true" : "false",id,status,
        status == AGENT_OK ? "" : status == AGENT_ERROR_STATE ?
        "service_or_clock_not_ready_check_list_timers" :
        status == AGENT_ERROR_CAPACITY ? "task_capacity" :
        status == AGENT_ERROR_INVALID || status == AGENT_ERROR_TOOL_ARGUMENT ?
        "invalid_argument_or_time_not_in_future" : "task_operation_failed");
    return smart_home_tool_reply(sink,output,(agent_error_t)status);
}

static agent_error_t create_tool(void *data, const agent_tool_context_t *context,
                                 const agent_text_sink_t *sink)
{
    char name[32] = "", message[128], at[20];
    int delay = 0, id = 0, ret;
    int64_t epoch = 0;
    int absolute = data != NULL;
    ret = smart_home_tool_get_string(context->call.arguments_json,"message",
                                    message,sizeof(message));
    if (ret == AGENT_OK) {
        int name_result = smart_home_tool_get_string(context->call.arguments_json,
                                                     "name",name,sizeof(name));
        if (name_result != AGENT_OK && name_result != AGENT_ERROR_NOT_FOUND)
            ret = name_result;
    }
    if (ret == AGENT_OK && absolute) {
        ret = smart_home_tool_get_string(context->call.arguments_json,"at_local",at,sizeof(at));
        if (ret == AGENT_OK) ret = smart_home_reminder_parse_local(at,&epoch);
    } else if (ret == AGENT_OK) {
        ret = smart_home_tool_get_int(context->call.arguments_json,"delay_seconds",&delay);
        if (ret == AGENT_OK && delay <= 0) ret = AGENT_ERROR_INVALID;
    }
    if (ret == AGENT_OK)
        ret = smart_home_reminder_create(delay,epoch,name,message,&id);
    return reply(sink,ret,id);
}

static agent_error_t cancel_tool(void *data, const agent_tool_context_t *context,
                                 const agent_text_sink_t *sink)
{
    int id = 0;
    int ret = smart_home_tool_get_int(context->call.arguments_json,"id",&id);
    (void)data;
    if (ret == AGENT_OK) ret = smart_home_reminder_cancel(id);
    return reply(sink,ret,id);
}

static agent_error_t list_tool(void *data, const agent_tool_context_t *context,
                               const agent_text_sink_t *sink)
{
    smart_home_reminder_snapshot_t snapshot;
    cJSON *root = cJSON_CreateObject(), *tasks = cJSON_CreateArray();
    char *output = NULL;
    char local[32] = "";
    struct tm tm;
    time_t beijing;
    int i, status = AGENT_ERROR_NOMEM;
    (void)data; (void)context;
    smart_home_reminder_snapshot(&snapshot);
    beijing = (time_t)(snapshot.now_epoch + 8 * 3600);
    if (snapshot.clock_ready && gmtime_r(&beijing,&tm))
        strftime(local,sizeof(local),"%Y-%m-%dT%H:%M:%S",&tm);
    if (!root || !tasks) goto done;
    if (!cJSON_AddItemToObject(root,"timers",tasks)) goto done;
    tasks = NULL;
    if (!cJSON_AddBoolToObject(root,"ready",snapshot.ready) ||
        !cJSON_AddBoolToObject(root,"clock_ready",snapshot.clock_ready) ||
        !cJSON_AddNumberToObject(root,"last_error",snapshot.last_error) ||
        !cJSON_AddStringToObject(root,"now_local",local) ||
        !cJSON_AddStringToObject(root,"timezone","UTC+08:00") ||
        !cJSON_AddNumberToObject(root,"count",snapshot.count)) goto done;
    for (i = 0; i < snapshot.count; ++i) {
        smart_home_reminder_t *r = &snapshot.items[i];
        cJSON *obj = cJSON_CreateObject();
        if (!obj) goto done;
        if (!cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root,"timers"),obj)) {
            cJSON_Delete(obj); goto done;
        }
        if (!cJSON_AddNumberToObject(obj,"id",r->id) ||
            !cJSON_AddStringToObject(obj,"name",r->name) ||
            !cJSON_AddStringToObject(obj,"message",r->message) ||
            !cJSON_AddNumberToObject(obj,"due_epoch",(double)r->due_epoch) ||
            !cJSON_AddNumberToObject(obj,"remaining_seconds",(double)r->remaining_seconds) ||
            !cJSON_AddStringToObject(obj,"kind",r->countdown ? "countdown" : "alarm") ||
            !cJSON_AddStringToObject(obj,"state",r->state == SMART_HOME_REMINDER_ACTIVE ?
                "active" : r->state == SMART_HOME_REMINDER_DUE ? "due" : "missed")) goto done;
    }
    output = cJSON_PrintUnformatted(root);
    if (output) status = smart_home_tool_reply(sink,output,
        snapshot.ready ? AGENT_OK : AGENT_ERROR_STATE);
 done:
    free(output); cJSON_Delete(tasks); cJSON_Delete(root);
    return (agent_error_t)status;
}

static agent_error_t reminder_context(void *data,
    const agent_context_request_t *request, const agent_text_sink_t *sink)
{
    static const char policy[] =
        "This build supports persistent reminders. Reminder requests must use "
        "set_timer/set_alarm/list_timers/cancel_timer, including non-device requests. "
        "Runtime tool definitions supersede old demo-timer skill descriptions. "
        "Query list_timers for current Beijing UTC+08:00 time before calendar alarms; "
        "ask about ambiguous dates. Confirm only successful tool results. "
        "Reminders never execute device actions. No power-off wake or recurrence.";
    (void)data; (void)request;
    return smart_home_tool_reply(sink,policy,AGENT_OK);
}

int smart_home_reminder_tools_register(agent_t *agent)
{
    static const struct {
        const char *name, *description, *schema;
        agent_tool_execute_fn execute;
        int absolute;
        int query;
    } tools[] = {
        {"set_timer","Create a persistent countdown reminder (1..31622400 seconds). Popup and local tone, no device control.",
         "{\"type\":\"object\",\"properties\":{\"delay_seconds\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":31622400},\"message\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"delay_seconds\",\"message\"],\"additionalProperties\":false}",create_tool,0,0},
        {"set_alarm","Create a one-shot alarm. at_local is Beijing UTC+08:00 YYYY-MM-DDTHH:MM:SS (2024..2037). First call list_timers for current time. Ask if date/time is ambiguous.",
         "{\"type\":\"object\",\"properties\":{\"at_local\":{\"type\":\"string\"},\"message\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"at_local\",\"message\"],\"additionalProperties\":false}",create_tool,1,0},
        {"list_timers","List persisted reminders, their states and the current Beijing time. clock_ready=false means set the board clock first.",
         "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",list_tool,0,1},
        {"cancel_timer","Durably remove a reminder/alarm by id, including a ringing one.",
         "{\"type\":\"object\",\"properties\":{\"id\":{\"type\":\"integer\"}},\"required\":[\"id\"],\"additionalProperties\":false}",cancel_tool,0,0}
    };
    size_t i;
    for (i = 0; i < sizeof(tools)/sizeof(tools[0]); ++i) {
        int ret = smart_home_tool_register_local(agent,tools[i].name,tools[i].description,
            tools[i].schema,tools[i].query ? SMART_HOME_TOOL_CATEGORY_QUERY :
            SMART_HOME_TOOL_CATEGORY_CONTROL,tools[i].execute,
            tools[i].absolute ? (void *)&tools[i] : NULL,
            tools[i].query ? AGENT_TOOL_READ_ONLY : AGENT_TOOL_SIDE_EFFECT);
        if (ret != AGENT_OK) return ret;
    }
    {
        const agent_context_provider_t provider = {
            .name = AGENT_SV_LITERAL("reminder_policy"),
            .priority = 90, .required = true, .build = reminder_context,
            .max_bytes = 640, .placement = AGENT_CONTEXT_INSTRUCTIONS
        };
        return agent_register_context(agent,&provider);
    }
}
