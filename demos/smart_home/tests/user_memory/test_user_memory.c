/* SPDX-License-Identifier: Apache-2.0 */
#include "memory/smart_home_user_memory.h"
#include "tools/smart_home_user_memory_tools.h"
#include "config/cjson_compat.h"
#include <agent/context.h>
#include <agent/model.h>
#include <agent_openvela_runtime.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail_rename, fail_fsync;
int __real_rename(const char *old, const char *next);
int __wrap_rename(const char *old, const char *next)
{
    if (fail_rename) { errno = EIO; return -1; }
    return __real_rename(old, next);
}
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_fsync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}

static char path[512];
static smart_home_user_memory_t *memory;
static void begin(const char *input) { smart_home_user_memory_begin(memory, input); }
static int remember(const char *key, const char *value, const char *quote, int *changed)
{ return smart_home_user_memory_remember(memory, key, value, quote, changed); }
static cJSON *listing(size_t offset, size_t limit, size_t budget)
{
    char *text = NULL;
    assert(!smart_home_user_memory_list(memory, offset, limit, budget, &text));
    cJSON *root = cJSON_Parse(text);
    assert(root && strlen(text) <= budget);
    free(text);
    return root;
}
static const char *first_value(const cJSON *root)
{
    cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    cJSON *first = cJSON_GetArrayItem(items, 0);
    return cJSON_GetObjectItemCaseSensitive(first, "value")->valuestring;
}
static void test_storage(void)
{
    int changed = -1;
    unlink(path);
    assert(!smart_home_user_memory_open(&memory, path));
    begin("我喜欢卧室空调26度");
    assert(!remember("bedroom.ac_temperature", "26度", "我喜欢卧室空调26度", &changed) && changed);
    assert(!remember("bedroom.ac_temperature", "26度", "我喜欢卧室空调26度", &changed) && !changed);
    smart_home_user_memory_end(memory);
    assert(remember("bedroom.ac_temperature", "26度", "我喜欢卧室空调26度", &changed) == AGENT_ERROR_POLICY_DENIED);
    smart_home_user_memory_close(&memory);
    assert(!smart_home_user_memory_open(&memory, path));
    cJSON *root = listing(0, 2, 1024);
    assert(cJSON_GetObjectItemCaseSensitive(root, "total")->valueint == 1);
    assert(!strcmp(first_value(root), "26度"));
    cJSON_Delete(root);
    begin("以后卧室空调改成24度");
    fail_rename = 1;
    assert(remember("bedroom.ac_temperature", "24度", "以后卧室空调改成24度", &changed) != AGENT_OK && !changed);
    fail_rename = 0;
    root = listing(0, 2, 1024); assert(!strcmp(first_value(root), "26度")); cJSON_Delete(root);
    fail_fsync = 1;
    assert(remember("bedroom.ac_temperature", "24度", "以后卧室空调改成24度", &changed) != AGENT_OK && !changed);
    fail_fsync = 0;
    smart_home_user_memory_close(&memory);
    assert(!smart_home_user_memory_open(&memory, path));
    root = listing(0, 2, 1024); assert(!strcmp(first_value(root), "26度")); cJSON_Delete(root);
    begin("以后卧室空调改成24度");
    assert(!remember("bedroom.ac_temperature", "24度", "以后卧室空调改成24度", &changed) && changed);
    assert(smart_home_user_memory_forget(memory, "bedroom.ac_temperature", &changed) == AGENT_ERROR_POLICY_DENIED);
    begin("忘记卧室空调温度偏好");
    assert(!smart_home_user_memory_forget(memory, "bedroom.ac_temperature", &changed) && changed);
    assert(!smart_home_user_memory_forget(memory, "bedroom.ac_temperature", &changed) && !changed);
    puts("PASS: real-file restart, deduplication, replacement, forgetting and failed writes preserve live/disk state");
}

static void test_validation(void)
{
    int changed;
    begin("我想要现在打开空调");
    assert(remember("ac.power", "打开空调", "我想要现在打开空调", &changed) == AGENT_ERROR_POLICY_DENIED);
    assert(remember("ac.temperature", "26度", "我喜欢26度", &changed) == AGENT_ERROR_POLICY_DENIED);
    begin("不要记住我喜欢26度");
    assert(remember("ac.temperature", "26度", "我喜欢26度", &changed) == AGENT_ERROR_POLICY_DENIED);
    begin("我喜欢26度");
    assert(remember("../preferences", "26度", "我喜欢26度", &changed) == AGENT_ERROR_INVALID);
    assert(remember("ac.temperature", "24度", "我喜欢26度", &changed) == AGENT_ERROR_POLICY_DENIED);
    begin("如果我喜欢26度，会怎么样");
    assert(remember("ac.temperature", "26度", "我喜欢26度", &changed) == AGENT_ERROR_POLICY_DENIED);
    begin("我希望回答简短一些");
    assert(!remember("response.style", "回答简短一些", "我希望回答简短一些", &changed));
    begin("忘记回答偏好");
    assert(!smart_home_user_memory_forget(memory, "response.style", &changed));
    begin("记住我的密码是123456");
    assert(remember("account.password", "123456", "记住我的密码是123456", &changed) == AGENT_ERROR_POLICY_DENIED);
    char key[32], quote[96];
    for (int i = 0; i < CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES; ++i) {
        snprintf(key, sizeof(key), "preference.%d", i);
        snprintf(quote, sizeof(quote), "我喜欢长期偏好%d", i);
        begin(quote);
        assert(!remember(key, quote, quote, &changed));
    }
    begin("我喜欢长期偏好999");
    assert(remember("preference.999", "我喜欢长期偏好999", "我喜欢长期偏好999", &changed) == AGENT_ERROR_CAPACITY);
    cJSON *root = listing(0, 2, 1024);
    assert(cJSON_GetObjectItemCaseSensitive(root, "next_offset")->valueint == 2);
    cJSON_Delete(root);
    root = listing(2, 2, 1024);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(root, "items")) == 2);
    cJSON_Delete(root);
    char *text = NULL;
    assert(smart_home_user_memory_list(memory, 0, 2, 20, &text) == AGENT_ERROR_CAPACITY && !text);
    smart_home_user_memory_close(&memory);
    FILE *file = fopen(path, "wb"); assert(file); fputs("{broken", file); fclose(file);
    assert(!smart_home_user_memory_open(&memory, path));
    root = listing(0, 2, 1024);
    assert(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "ready")));
    cJSON_Delete(root);
    begin("我喜欢26度");
    assert(remember("ac.temperature", "26度", "我喜欢26度", &changed) == AGENT_ERROR_PARSE);
    file = fopen(path, "rb"); assert(file); char damaged[16] = {0}; assert(fread(damaged, 1, 7, file) == 7); fclose(file);
    assert(!strcmp(damaged, "{broken"));
    smart_home_user_memory_close(&memory);
    unlink(path);
    assert(!smart_home_user_memory_open(&memory, path));
    puts("PASS: current-input evidence, temporary requests, opt-out, credentials, bounds, pagination and corrupt-file preservation");
}

static int step, reference_seen, tool_seen;
static const char *arguments;
static int want_result_error;
static int contains(agent_string_view_t text, const char *needle)
{
    size_t size = strlen(needle);
    for (size_t i = 0; i + size <= text.size; ++i)
        if (!memcmp(text.data + i, needle, size)) return 1;
    return 0;
}
static agent_error_t complete(void *data, const agent_model_request_t *request,
                              const agent_model_sink_t *sink)
{
    (void)data;
    for (size_t i = 0; i < request->message_count; ++i) {
        const agent_message_view_t *m = &request->messages[i];
        if (m->role == AGENT_MESSAGE_ROLE_USER && contains(m->content, "response.style")) reference_seen++;
        if (m->role == AGENT_MESSAGE_ROLE_TOOL) {
            if (want_result_error) {
                if (!contains(m->content, "\"ok\":false"))
                    fprintf(stderr, "error tool envelope: %.*s\n", (int)m->content.size, m->content.data);
                assert(contains(m->content, "\"ok\":false") || contains(m->content, "AGENT_ERROR_"));
            }
            else assert(contains(m->content, "\"ok\":true") || contains(m->content, "\"items\""));
            tool_seen++;
        }
    }
    assert(!contains(request->system_prompt, "response.style"));
    if (arguments && !step++) {
        const agent_tool_call_view_t call = {
            .id = AGENT_SV_LITERAL("memory1"), .name = AGENT_SV_LITERAL("user_memory"),
            .arguments_json = {arguments, strlen(arguments)}
        };
        return sink->tool_call(sink->context, &call);
    }
    return sink->text(sink->context, agent_string_view("ok", 2));
}
static agent_policy_decision_t allow(void *data, const agent_policy_request_t *request)
{ (void)data; (void)request; return AGENT_POLICY_ALLOW; }
static void run(agent_t *agent, const char *input, const char *args, int error)
{
    char output[32];
    agent_request_t request = {.input = {input, strlen(input)}};
    agent_response_t response = {.output = output, .output_size = sizeof(output)};
    arguments = args; step = 0; want_result_error = error;
    begin(input);
    int ret = agent_run(agent, &request, &response);
    if (ret) fprintf(stderr, "memory Core turn failed: %d %s input=%s\n", ret, agent_error_str(ret), input);
    assert(!ret);
    smart_home_user_memory_end(memory);
    assert(!strcmp(output, "ok"));
}
static void test_core(void)
{
    static agent_workspace_t workspace;
    agent_model_workspace_t model_workspace;
    agent_t *agent;
    agent_model_t *model;
    const agent_model_ops_t ops = {.complete = complete};
    agent_config_t config = agent_config_default();
    assert(!agent_port_openvela_runtime_init(&config.runtime));
    assert(!agent_init(&agent, &workspace, &config));
    assert(!agent_model_init(&model, &model_workspace, &ops, NULL));
    assert(!agent_set_model(agent, model));
    assert(!agent_set_policy_callback(agent, allow, NULL));
    assert(!smart_home_user_memory_register_context(agent, memory));
    assert(!smart_home_user_memory_tools_register(agent, memory));
    assert(!agent_start(agent));
    run(agent, "以后回答简短一些", "{\"action\":\"remember\",\"key\":\"response.style\",\"value\":\"简短\",\"evidence\":\"以后回答简短一些\"}", 0);
    assert(tool_seen == 1);
    run(agent, "问候", NULL, 0);
    assert(reference_seen);
    run(agent, "列出偏好", "{\"action\":\"list\",\"offset\":0}", 0);
    run(agent, "我想要现在打开灯", "{\"action\":\"remember\",\"key\":\"light\",\"value\":\"打开灯\",\"evidence\":\"我想要现在打开灯\"}", 1);
    run(agent, "删除回复偏好", "{\"action\":\"forget\",\"key\":\"response.style\"}", 0);
    run(agent, "查询偏好", "{\"action\":\"list\",\"offset\":-1}", 1);
    agent_destroy(agent); agent_model_destroy(model);
    smart_home_user_memory_close(&memory);
    puts("PASS: real Core model/tool turns, saved reference context, tool-time writes without idle reentry, and error results");
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    snprintf(path, sizeof(path), "%s/user_memory.json", argv[1]);
    test_storage(); test_validation(); test_core();
    unlink(path);
    return 0;
}
