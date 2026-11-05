/* SPDX-License-Identifier: Apache-2.0 */
#include <agent.h>
#include <agent/model.h>
#include <agent_openvela_runtime.h>
#include "device/smart_home_device_service.h"
#include "skills/smart_home_skills.h"
#include "tools/smart_home_tools.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int model_calls, reads, tools;
static int contains(agent_string_view_t text, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= text.size; ++i)
        if (!memcmp(text.data + i, needle, n)) return 1;
    return 0;
}
static agent_error_t complete(void *data, const agent_model_request_t *request,
                             const agent_model_sink_t *sink)
{
    int reference = 0, result = 0;
    (void)data;
    ++model_calls;
    assert(contains(request->system_prompt, "Do not claim that a device changed"));
    assert(contains(request->system_prompt, "Skill smart_home_device_control:"));
    assert(contains(request->system_prompt, "Skill smart_home_device_routing:"));
    assert(!contains(request->system_prompt, "Device inventory:"));
    assert(!contains(request->system_prompt, "\"environment\""));
    for (size_t i = 0; i < request->message_count; ++i) {
        const agent_message_view_t *message = &request->messages[i];
        if (message->role == AGENT_MESSAGE_ROLE_USER && contains(message->content, "\"environment\"")) ++reference;
        if (message->role == AGENT_MESSAGE_ROLE_TOOL) {
            assert(contains(message->content, "Device inventory:"));
            result = 1;
            ++reads;
        }
    }
    assert(reference == 1);
    if (!result) {
        const agent_tool_call_view_t call = {
            .id = AGENT_SV_LITERAL("read1"),
            .name = AGENT_SV_LITERAL("read_skill"),
            .arguments_json = AGENT_SV_LITERAL("{\"name\":\"smart_home_device_control\"}")
        };
        return sink->tool_call(sink->context, &call);
    }
    return sink->text(sink->context, agent_string_view("ok", 2));
}
static agent_policy_decision_t allow(void *data, const agent_policy_request_t *request)
{ (void)data; (void)request; return AGENT_POLICY_ALLOW; }
static agent_error_t visit(void *data, const agent_tool_t *tool)
{ (void)data; assert(tool->group.size == 5); ++tools; return AGENT_OK; }
int main(int argc, char **argv)
{
    static agent_workspace_t workspace;
    agent_model_workspace_t model_workspace;
    agent_t *agent;
    agent_model_t *model;
    const agent_model_ops_t ops = {.complete = complete};
    agent_config_t config = agent_config_default();
    smart_home_state_t state;
    smart_home_device_service_t service;
    smart_home_skill_store_t store;
    agent_request_t request = {.input = AGENT_SV_LITERAL("read")};
    char output[32];
    agent_response_t response = {.output = output, .output_size = sizeof(output)};
    assert(!agent_port_openvela_runtime_init(&config.runtime));
    assert(!agent_init(&agent, &workspace, &config));
    assert(!agent_model_init(&model, &model_workspace, &ops, NULL));
    assert(!agent_set_model(agent, model));
    smart_home_device_init(&state);
    assert(!smart_home_device_service_init(&service, &state));
    assert(!smart_home_device_service_register_context(agent, &service));
    smart_home_skill_store_init(&store);
    (void)argv;
    if (argc > 1) {
        assert(smart_home_skills_register(agent, &store) == AGENT_ERROR_INVALID);
        agent_destroy(agent);
        agent_model_destroy(model);
        smart_home_skill_store_deinit(&store);
        smart_home_device_service_deinit(&service);
        puts("PASS: malformed primary Skill is reported without fallback");
        return 0;
    }
    assert(!smart_home_skills_register(agent, &store));
    assert(store.count == 8);
    assert(!smart_home_tools_register(agent, &service));
    assert(!agent_tool_enumerate(agent, visit, NULL) && tools == 9);
    assert(!agent_set_policy_callback(agent, allow, NULL));
    assert(!agent_start(agent));
    int ret = agent_run(agent, &request, &response);
    if (ret) fprintf(stderr, "context run: %d %s\n", ret, agent_error_str(ret));
    assert(!ret && !strcmp(output, "ok") && model_calls == 2 && reads == 1);
    const smart_home_loaded_skill_t *scene = smart_home_skill_store_find(&store, "smart_home_scenes");
    smart_home_scene_catalog_t catalog;
    assert(smart_home_scene_catalog_load(scene->context_text, &catalog) == AGENT_ERROR_NOT_FOUND);
    agent_destroy(agent);
    agent_model_destroy(model);
    smart_home_skill_store_deinit(&store);
    smart_home_device_service_deinit(&service);
    puts("PASS: real Core, device snapshot reference, seven file Skills and on-demand read_skill");
    return 0;
}
