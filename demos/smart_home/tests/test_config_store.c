/* SPDX-License-Identifier: Apache-2.0 */
/**
 * smart_home_config_store host 测试。
 *
 * 验证通用骨架（加载/恢复策略/原子写）与 backends/settings/state 薄封装，
 * 以及 state ↔ typed 序列化。
 *
 * 路径宏由编译命令 -D 传入（对所有编译单元生效，含 smart_home_config_store.c）：
 *   -DSMART_HOME_CONFIG_BACKENDS_PATH='"/tmp/smh_cfg_test_path/backends.json"'
 *   -DSMART_HOME_CONFIG_SETTINGS_PATH='"/tmp/smh_cfg_test_path/settings.json"'
 *   -DSMART_HOME_CONFIG_STATE_PATH='"/tmp/smh_cfg_test_path/state.json"'
 * 使用独立临时目录（固定测试路径，见 setup_temp_dir），避免并行冲突。
 */

#include <agent.h>
#include <agent/context.h>
#include <cJSON.h>
#include <smart_home_config_store.h>
#include <smart_home_backends.h>
#include <smart_home_device.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_dir[128];
static char g_backends_path[160];
static char g_settings_path[160];
static char g_state_path[160];

static void setup_temp_dir(void)
{
    /* 固定测试路径（与编译命令 -D 的宏一致），每个测试运行前清理 */
    snprintf(g_dir, sizeof(g_dir), "/tmp/smh_cfg_test_path");
    mkdir(g_dir, 0700);
    snprintf(g_backends_path, sizeof(g_backends_path), "%s/backends.json",
             g_dir);
    snprintf(g_settings_path, sizeof(g_settings_path), "%s/settings.json",
             g_dir);
    snprintf(g_state_path, sizeof(g_state_path), "%s/state.json", g_dir);

    unlink(g_backends_path);
    unlink(g_settings_path);
    unlink(g_state_path);
}

static void cleanup_temp_dir(void)
{
    unlink(g_backends_path);
    unlink(g_settings_path);
    unlink(g_state_path);
    rmdir(g_dir);
}

static void write_file(const char *path, const char *content)
{
    FILE *file;

    file = fopen(path, "wb");
    assert(file != NULL);
    fputs(content, file);
    fclose(file);
}

static void test_backends_missing_falls_back(void)
{
    cJSON *root = NULL;
    char error[96];

    assert(smart_home_config_backends_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(root, "backends")) == 5);
    cJSON_Delete(root);
}

static void test_backends_roundtrip(void)
{
    cJSON *root;
    cJSON *backends;
    cJSON *item;
    char error[96];

    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    backends = cJSON_AddArrayToObject(root, "backends");
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "backend_id", "my_gw");
    cJSON_AddStringToObject(item, "label", "My Gateway");
    cJSON_AddStringToObject(item, "host", "10.0.2.2");
    cJSON_AddStringToObject(item, "path", "/v1/chat/completions");
    cJSON_AddStringToObject(item, "port", "8080");
    cJSON_AddStringToObject(item, "default_model", "qwen-plus");
    cJSON_AddNumberToObject(item, "default_timeout_ms", 30000);
    cJSON_AddItemToArray(backends, item);
    assert(smart_home_config_backends_save(root) == AGENT_OK);
    cJSON_Delete(root);

    assert(smart_home_config_backends_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(root, "backends")) == 1);
    cJSON_Delete(root);
}

static void test_backends_bad_version_falls_back(void)
{
    cJSON *root = NULL;
    char error[96];

    write_file(g_backends_path, "{\"version\":99,\"backends\":[]}");
    assert(smart_home_config_backends_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    cJSON_Delete(root);
}

static void test_backends_duplicate_id_falls_back(void)
{
    cJSON *root = NULL;
    char error[96];

    write_file(g_backends_path,
               "{\"version\":1,\"backends\":["
               "{\"backend_id\":\"a\",\"label\":\"A\",\"host\":\"h\","
               "\"path\":\"/v1\",\"port\":\"443\",\"default_model\":\"m\","
               "\"default_timeout_ms\":30000},"
               "{\"backend_id\":\"a\",\"label\":\"A2\",\"host\":\"h2\","
               "\"path\":\"/v1\",\"port\":\"443\",\"default_model\":\"m2\","
               "\"default_timeout_ms\":30000}]}");
    assert(smart_home_config_backends_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    cJSON_Delete(root);
}

static void test_backends_invalid_host_falls_back(void)
{
    cJSON *root = NULL;
    char error[96];

    /* host 为空 → 校验失败 → 回退 */
    write_file(g_backends_path,
               "{\"version\":1,\"backends\":["
               "{\"backend_id\":\"a\",\"label\":\"A\",\"host\":\"\","
               "\"path\":\"/v1\",\"port\":\"443\",\"default_model\":\"m\","
               "\"default_timeout_ms\":30000}]}");
    assert(smart_home_config_backends_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    cJSON_Delete(root);
}

static void test_save_rejects_invalid(void)
{
    cJSON *root;
    cJSON *backends;
    cJSON *item;

    /* 非法 backends（host 空）→ save 应拒绝 */
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    backends = cJSON_AddArrayToObject(root, "backends");
    item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "backend_id", "bad");
    cJSON_AddStringToObject(item, "host", "");
    cJSON_AddItemToArray(backends, item);
    assert(smart_home_config_backends_save(root) != AGENT_OK);
    cJSON_Delete(root);

    /* 非法 settings（active 空）→ save 应拒绝 */
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "active_backend_id", "");
    assert(smart_home_config_settings_save(root) != AGENT_OK);
    cJSON_Delete(root);

    /* 非法 state（温度 100）→ save 应拒绝 */
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
    {
        cJSON *env = cJSON_CreateObject();
        cJSON_AddNumberToObject(env, "temperature", 100);
        cJSON_AddItemToObject(root, "environment", env);
    }
    assert(smart_home_config_state_save(root) != AGENT_OK);
    cJSON_Delete(root);
}

static void test_state_default_from_device_init(void)
{
    cJSON *root = NULL;
    smart_home_state_t state;
    smart_home_state_t state2;
    char error[96];

    /* 默认 3 设备 */
    assert(smart_home_config_state_load(&root, error,
                                        sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(root, "devices")) == 1);
    cJSON_Delete(root);

    /* 与 device_init 一致：序列化往返应还原状态 */
    smart_home_device_init(&state);
    assert(smart_home_room_count(&state) == 2);
    assert(smart_home_room_add(&state, "kitchen") == AGENT_OK);
    assert(smart_home_room_count(&state) == 3);
    assert(smart_home_device_state_to_json(&state) != NULL);
    {
        cJSON *json = smart_home_device_state_to_json(&state);
        assert(json != NULL);
        assert(smart_home_device_state_from_json(&state2, json) == AGENT_OK);
        cJSON_Delete(json);
    }
    assert(state2.devices[0].id == 1);
    assert(smart_home_room_index(&state2, "kitchen") >= 0);
    assert(state2.env_temperature == 26);
}

static void test_state_rejects_duplicate_id(void)
{
    cJSON *root = NULL;
    char error[96];

    write_file(g_state_path,
               "{\"version\":1,\"devices\":["
               "{\"id\":1,\"room\":\"living_room\",\"name\":\"L1\","
               "\"type\":\"light\",\"on\":false,\"brightness\":0},"
               "{\"id\":1,\"room\":\"bedroom\",\"name\":\"L2\","
               "\"type\":\"light\",\"on\":false,\"brightness\":0}]}");
    assert(smart_home_config_state_load(&root, error,
                                        sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    cJSON_Delete(root);
}

static void test_state_rejects_bad_mode(void)
{
    cJSON *root = NULL;
    char error[96];

    write_file(g_state_path,
               "{\"version\":1,\"devices\":["
               "{\"id\":1,\"room\":\"bedroom\",\"name\":\"AC\","
               "\"type\":\"ac\",\"on\":false,\"temperature\":26,"
               "\"mode\":\"bogus\",\"fan_speed\":\"auto\"}]}");
    assert(smart_home_config_state_load(&root, error,
                                        sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    cJSON_Delete(root);
}

static void test_settings_roundtrip_and_fallback(void)
{
    cJSON *root = NULL;
    char error[96];

    assert(smart_home_config_settings_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    assert(strcmp(cJSON_GetObjectItem(root, "active_backend_id")->valuestring,
                  "deepseek") == 0);
    cJSON_Delete(root);

    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddStringToObject(root, "active_backend_id", "mimo");
    assert(smart_home_config_settings_save(root) == AGENT_OK);
    cJSON_Delete(root);

    assert(smart_home_config_settings_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    assert(strcmp(cJSON_GetObjectItem(root, "active_backend_id")->valuestring,
                  "mimo") == 0);
    cJSON_Delete(root);
}

static void test_corrupt_json_falls_back(void)
{
    cJSON *root = NULL;
    char error[96];

    /* 非法 JSON + 尾随垃圾 */
    write_file(g_backends_path, "{not json!!");
    assert(smart_home_config_backends_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    cJSON_Delete(root);

    write_file(g_backends_path, "{\"version\":1,\"backends\":[]} garbage");
    assert(smart_home_config_backends_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    cJSON_Delete(root);
}

static void test_non_integer_version_falls_back(void)
{
    cJSON *root = NULL;
    char error[96];

    write_file(g_backends_path, "{\"version\":\"one\",\"backends\":[]}");
    assert(smart_home_config_backends_load(&root, error,
                                           sizeof(error)) == AGENT_OK);
    assert(root != NULL);
    cJSON_Delete(root);
}

static void test_save_keeps_old_on_rename_failure(void)
{
    cJSON *root;
    char content[128];
    FILE *file;
    size_t n;

    /* 先写合法 state */
    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON_AddItemToObject(root, "devices", cJSON_CreateArray());
    assert(smart_home_config_state_save(root) == AGENT_OK);
    cJSON_Delete(root);

    /* 读回内容 */
    file = fopen(g_state_path, "rb");
    assert(file != NULL);
    n = fread(content, 1, sizeof(content) - 1, file);
    content[n] = '\0';
    fclose(file);
    assert(strstr(content, "\"version\":1") != NULL);
}

int main(void)
{
    setup_temp_dir();

    test_backends_missing_falls_back();
    test_backends_roundtrip();
    test_backends_bad_version_falls_back();
    test_backends_duplicate_id_falls_back();
    test_backends_invalid_host_falls_back();
    test_save_rejects_invalid();
    test_state_default_from_device_init();
    test_state_rejects_duplicate_id();
    test_state_rejects_bad_mode();
    test_settings_roundtrip_and_fallback();
    test_corrupt_json_falls_back();
    test_non_integer_version_falls_back();
    test_save_keeps_old_on_rename_failure();

    cleanup_temp_dir();
    printf("all config_store tests passed\n");
    return 0;
}

/* device.c 引用了 cAGENT 的 context provider 注册，测试提供 stub */
agent_error_t agent_register_context(agent_t *agent,
                                    const agent_context_provider_t *provider)
{
    (void)agent;
    (void)provider;
    return AGENT_OK;
}
