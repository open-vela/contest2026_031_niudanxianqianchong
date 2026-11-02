/* SPDX-License-Identifier: Apache-2.0 */
/* 专用 secrets.json loader；刻意不复用可回退、可写回的 config_store。 */

#include "smart_home_secrets.h"

#include "cjson_compat.h"

#include <agent/error.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SMART_HOME_SECRETS_VERSION 1

/* Serialize every read-modify-write, including Wi-Fi and Miloco updates. */
static pthread_mutex_t g_secrets_writer = PTHREAD_MUTEX_INITIALIZER;

static void secure_clear(void *memory, size_t size)
{
    volatile unsigned char *p = memory;

    while (p && size-- > 0u) {
        *p++ = 0u;
    }
}

static int valid_backend_id(const char *id)
{
    const unsigned char *p = (const unsigned char *)id;
    size_t length = 0u;

    if (!p || !*p) {
        return 0;
    }

    while (*p) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            return 0;
        }
        if (++length > 32u) {
            return 0;
        }
        p++;
    }

    return 1;
}

static int contains_control(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;

    if (!p) {
        return 1;
    }
    while (*p) {
        if (*p < 0x20u || *p == 0x7fu) {
            return 1;
        }
        p++;
    }
    return 0;
}

static int read_secrets_file(char **out_text, size_t *out_size)
{
    FILE *file;
    long size = -1;
    char *text;

    *out_text = NULL;
    *out_size = 0u;
    file = fopen(CONFIG_SMART_HOME_MODEL_SECRETS_PATH, "rb");
    if (!file) {
        return errno == ENOENT ? AGENT_ERROR_NOT_FOUND : AGENT_ERROR;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return AGENT_ERROR;
    }
    size = ftell(file);
    if (size < 0) {
        fclose(file);
        return AGENT_ERROR;
    }
    if ((size_t)size > CONFIG_SMART_HOME_MODEL_SECRETS_MAX_FILE_SIZE) {
        fclose(file);
        return AGENT_ERROR_LIMIT;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return AGENT_ERROR;
    }

    text = malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return AGENT_ERROR_NOMEM;
    }
    if (fread(text, 1u, (size_t)size, file) != (size_t)size) {
        fclose(file);
        secure_clear(text, (size_t)size + 1u);
        free(text);
        return AGENT_ERROR;
    }
    fclose(file);
    text[size] = '\0';
    *out_text = text;
    *out_size = (size_t)size;
    return AGENT_OK;
}

static int lookup_api_key(smart_home_secret_service_t service,
                                const char *backend_id,
                                char *out,
                                size_t out_size)
{
    char *text = NULL;
    size_t text_size = 0u;
    cJSON *root = NULL;
    cJSON *version;
    cJSON *keys;
    cJSON *value;
    int ret;

    const char *section = smart_home_secrets_key_section(service);

    if (!section || !backend_id || !out || out_size == 0u || !valid_backend_id(backend_id)) {
        return AGENT_ERROR_INVALID;
    }
    out[0] = '\0';

    ret = read_secrets_file(&text, &text_size);
    if (ret != AGENT_OK) {
        return ret;
    }

    root = cJSON_ParseWithLength(text, text_size);
    secure_clear(text, text_size + 1u);
    free(text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return AGENT_ERROR_PARSE;
    }

    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    keys = cJSON_GetObjectItemCaseSensitive(root, section);
    value = cJSON_IsObject(keys)
                ? cJSON_GetObjectItemCaseSensitive(keys, backend_id) : NULL;
    if (!cJSON_IsNumber(version) || version->valueint != SMART_HOME_SECRETS_VERSION ||
        (keys != NULL && !cJSON_IsObject(keys)) || !cJSON_IsString(value) ||
        !value->valuestring[0]) {
        ret = (!cJSON_IsNumber(version) || version->valueint != SMART_HOME_SECRETS_VERSION ||
               (keys != NULL && !cJSON_IsObject(keys))) ? AGENT_ERROR_PARSE : AGENT_ERROR_NOT_FOUND;
        cJSON_Delete(root);
        return ret;
    }
    if (contains_control(value->valuestring)) {
        cJSON_Delete(root);
        return AGENT_ERROR_PARSE;
    }
    if (strlen(value->valuestring) >= out_size) {
        cJSON_Delete(root);
        return AGENT_ERROR_LIMIT;
    }

    memcpy(out, value->valuestring, strlen(value->valuestring) + 1u);
    secure_clear(value->valuestring, strlen(value->valuestring));
    cJSON_Delete(root);
    return AGENT_OK;
}

const char *smart_home_secrets_key_section(smart_home_secret_service_t service)
{
    switch (service) {
    case SMART_HOME_SECRET_LLM: return "model_api_keys";
    case SMART_HOME_SECRET_ASR: return "asr_api_keys";
    case SMART_HOME_SECRET_TTS: return "tts_api_keys";
    default: return NULL;
    }
}

const char *smart_home_secrets_error_text(int result)
{
    switch (result) {
    case AGENT_OK: return "已配置（尚未验证云端鉴权）";
    case AGENT_ERROR_NOT_FOUND: return "密钥未配置（文件、服务条目缺失或为空）";
    case AGENT_ERROR_PARSE: return "凭据 JSON、版本或密钥格式错误";
    case AGENT_ERROR_LIMIT: return "凭据文件或密钥超过长度限制";
    case AGENT_ERROR_INVALID: return "凭据参数不合法";
    case AGENT_ERROR_NOMEM: return "读取凭据时内存不足";
    default: return "凭据文件读写失败";
    }
}

int smart_home_secrets_get_api_key(smart_home_secret_service_t service,
                                   const char *backend_id,
                                   char *out, size_t out_size)
{
    int ret;
    if (out && out_size) secure_clear(out, out_size);
    ret = lookup_api_key(service, backend_id, out, out_size);
    if (ret != AGENT_OK && out && out_size) secure_clear(out, out_size);
    return ret;
}

int smart_home_secrets_api_key_status(smart_home_secret_service_t service,
                                      const char *backend_id)
{
    char key[256];
    int ret = smart_home_secrets_get_api_key(service, backend_id, key, sizeof(key));
    secure_clear(key, sizeof(key));
    return ret;
}

int smart_home_secrets_get_model_api_key(const char *backend_id,
                                         char *out, size_t out_size)
{
    return smart_home_secrets_get_api_key(SMART_HOME_SECRET_LLM,
                                          backend_id, out, out_size);
}

int smart_home_secrets_model_api_key_status(const char *backend_id)
{
    return smart_home_secrets_api_key_status(SMART_HOME_SECRET_LLM, backend_id);
}

/* 三个服务各自默认的密钥条目。MiMo 是 LLM/ASR/TTS 共同的默认后端，
 * 新建或补齐文档时必须留下它的空条目，否则界面读不到"未配置"以外的
 * 状态，也无法在同一个 backend_id 下继续沿用既有密钥。 */
static const smart_home_secret_service_t g_secret_services[] = {
    SMART_HOME_SECRET_LLM,
    SMART_HOME_SECRET_ASR,
    SMART_HOME_SECRET_TTS
};

#define SMART_HOME_SECRET_DEFAULT_BACKEND "mimo"

/* 保证某个服务段存在，并且含默认后端的空条目。已有条目一律不动。
 * 返回 AGENT_OK 或错误码。 */
static int ensure_key_section(cJSON *root,
                              smart_home_secret_service_t service)
{
    const char *section = smart_home_secrets_key_section(service);
    cJSON *keys;

    if (!section) {
        return AGENT_ERROR_INVALID;
    }
    keys = cJSON_GetObjectItemCaseSensitive(root, section);
    if (keys && !cJSON_IsObject(keys)) {
        return AGENT_ERROR_PARSE;
    }
    if (!keys) {
        keys = cJSON_AddObjectToObject(root, section);
        if (!keys) {
            return AGENT_ERROR_NOMEM;
        }
    }
    if (!cJSON_GetObjectItemCaseSensitive(keys, SMART_HOME_SECRET_DEFAULT_BACKEND) &&
        !cJSON_AddStringToObject(keys, SMART_HOME_SECRET_DEFAULT_BACKEND, "")) {
        return AGENT_ERROR_NOMEM;
    }
    return AGENT_OK;
}

/* Refuse malformed existing documents rather than replacing other credentials. */
static int load_update_root(cJSON **out)
{
    char *text = NULL;
    size_t size = 0;
    cJSON *root;
    const cJSON *version;
    int ret, i;

    ret = read_secrets_file(&text, &size);
    if (ret == AGENT_ERROR_NOT_FOUND) {
        root = cJSON_CreateObject();
        if (!root || !cJSON_AddNumberToObject(root, "version", 1)) {
            cJSON_Delete(root);
            return AGENT_ERROR_NOMEM;
        }
    } else {
        if (ret != AGENT_OK) return ret;
        root = cJSON_ParseWithLength(text, size);
        secure_clear(text, size + 1);
        free(text);
        version = cJSON_GetObjectItemCaseSensitive(root, "version");
        if (!cJSON_IsObject(root) || !cJSON_IsNumber(version) ||
            version->valuedouble != SMART_HOME_SECRETS_VERSION) {
            cJSON_Delete(root);
            return AGENT_ERROR_PARSE;
        }
    }

    /* 新建文档补齐全部三段；老文档只补缺失的段，绝不覆盖已有密钥。
     * 这样拆分 asr/tts 段之后，原有的 model_api_keys（含 mimo）仍然留在
     * 文件里，不会因为一次保存就消失。 */
    for (i = 0; i < (int)(sizeof(g_secret_services) / sizeof(g_secret_services[0])); i++) {
        ret = ensure_key_section(root, g_secret_services[i]);
        if (ret != AGENT_OK) {
            cJSON_Delete(root);
            return ret;
        }
    }

    *out = root;
    return AGENT_OK;
}

static int save_update_root(const cJSON *root)
{
    char temp[192];
    char *text = cJSON_PrintUnformatted(root);
    struct timespec now;
    size_t length, done = 0;
    int fd = -1, ret = AGENT_ERROR, attempt;
    if (!text) return AGENT_ERROR_NOMEM;
    length = strlen(text);
    if (length > CONFIG_SMART_HOME_MODEL_SECRETS_MAX_FILE_SIZE) {
        ret = AGENT_ERROR_LIMIT;
        goto out;
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (attempt = 0; attempt < 16; attempt++) {
        unsigned long generation = ((unsigned long)now.tv_nsec ^
            (unsigned long)now.tv_sec) + (unsigned long)attempt;
        if (snprintf(temp, sizeof(temp), "%s.tmp.%08lx",
            CONFIG_SMART_HOME_MODEL_SECRETS_PATH, generation) >= (int)sizeof(temp)) {
            ret = AGENT_ERROR_LIMIT;
            goto out;
        }
        fd = open(temp, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) goto out;
    while (done < length) {
        ssize_t n = write(fd, text + done, length - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto failed;
        done += (size_t)n;
    }
    if (fsync(fd) < 0) goto failed;
    if (close(fd) < 0) { fd = -1; goto failed; }
    fd = -1;
    if (rename(temp, CONFIG_SMART_HOME_MODEL_SECRETS_PATH) < 0) goto failed;
    ret = AGENT_OK;
    goto out;
failed:
    if (fd >= 0) close(fd);
    unlink(temp);
out:
    secure_clear(text, length);
    free(text);
    return ret;
}

static cJSON *update_object(cJSON *root, const char *name)
{
    cJSON *object = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!object) object = cJSON_AddObjectToObject(root, name);
    return cJSON_IsObject(object) ? object : NULL;
}

int smart_home_secrets_set_api_key(smart_home_secret_service_t service,
                                   const char *backend_id, const char *key)
{
    cJSON *root = NULL, *keys;
    const char *section = smart_home_secrets_key_section(service);
    int ret;
    if (!section || !valid_backend_id(backend_id) || !key ||
        strlen(key) >= 256 || contains_control(key)) return AGENT_ERROR_INVALID;
    pthread_mutex_lock(&g_secrets_writer);
    ret = load_update_root(&root);
    if (ret != AGENT_OK) goto out;
    keys = update_object(root, section);
    if (!keys) { ret = AGENT_ERROR_PARSE; goto out; }
    cJSON_DeleteItemFromObjectCaseSensitive(keys, backend_id);
    if (!cJSON_AddStringToObject(keys, backend_id, key)) {
        ret = AGENT_ERROR_NOMEM;
        goto out;
    }
    ret = save_update_root(root);
out:
    cJSON_Delete(root);
    pthread_mutex_unlock(&g_secrets_writer);
    return ret;
}

int smart_home_secrets_get_wifi_credentials(char *ssid, size_t ssid_size,
                                            char *password,
                                            size_t password_size)
{
    char *text = NULL;
    size_t text_size = 0u;
    cJSON *root = NULL;
    cJSON *version;
    cJSON *wifi;
    cJSON *stored_ssid;
    cJSON *stored_password;
    int ret;

    if (!ssid || ssid_size == 0u || !password || password_size == 0u) {
        return AGENT_ERROR_INVALID;
    }
    ssid[0] = '\0';
    password[0] = '\0';
    ret = read_secrets_file(&text, &text_size);
    if (ret != AGENT_OK) {
        return ret;
    }
    root = cJSON_ParseWithLength(text, text_size);
    secure_clear(text, text_size + 1u);
    free(text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return AGENT_ERROR_PARSE;
    }
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!cJSON_IsNumber(version) ||
        version->valueint != SMART_HOME_SECRETS_VERSION) {
        cJSON_Delete(root);
        return AGENT_ERROR_PARSE;
    }
    wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    stored_ssid = cJSON_IsObject(wifi) ?
        cJSON_GetObjectItemCaseSensitive(wifi, "ssid") : NULL;
    stored_password = cJSON_IsObject(wifi) ?
        cJSON_GetObjectItemCaseSensitive(wifi, "password") : NULL;
    if (!cJSON_IsString(stored_ssid) || !cJSON_IsString(stored_password) ||
        !stored_ssid->valuestring[0] ||
        strlen(stored_ssid->valuestring) >= ssid_size ||
        strlen(stored_password->valuestring) >= password_size ||
        contains_control(stored_ssid->valuestring) ||
        contains_control(stored_password->valuestring)) {
        cJSON_Delete(root);
        return AGENT_ERROR_NOT_FOUND;
    }
    memcpy(ssid, stored_ssid->valuestring, strlen(stored_ssid->valuestring) + 1u);
    memcpy(password, stored_password->valuestring,
           strlen(stored_password->valuestring) + 1u);
    cJSON_Delete(root);
    return AGENT_OK;
}

int smart_home_secrets_set_wifi_credentials(const char *ssid,
                                            const char *password)
{
    cJSON *root = NULL, *wifi;
    int ret;
    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 ||
        strlen(password) > 64 || contains_control(ssid) || contains_control(password))
        return AGENT_ERROR_INVALID;
    pthread_mutex_lock(&g_secrets_writer);
    ret = load_update_root(&root);
    if (ret != AGENT_OK) goto out;
    wifi = update_object(root, "wifi");
    if (!wifi) { ret = AGENT_ERROR_PARSE; goto out; }
    cJSON_DeleteItemFromObjectCaseSensitive(wifi, "ssid");
    cJSON_DeleteItemFromObjectCaseSensitive(wifi, "password");
    if (!cJSON_AddStringToObject(wifi, "ssid", ssid) ||
        !cJSON_AddStringToObject(wifi, "password", password)) {
        ret = AGENT_ERROR_NOMEM;
        goto out;
    }
    ret = save_update_root(root);
out:
    cJSON_Delete(root);
    pthread_mutex_unlock(&g_secrets_writer);
    return ret;
}

/*
 * 读取或更新 secrets.json 的 miloco 段（家庭服务器地址/端口/token）。
 * token 允许为空（Miloco 未配置鉴权时使用）。port 缺省按 1810 处理。
 */
int smart_home_secrets_get_miloco(char *host, size_t host_size,
                                  uint16_t *port,
                                  char *token, size_t token_size)
{
    char *text = NULL;
    size_t text_size = 0u;
    cJSON *root = NULL;
    cJSON *version;
    cJSON *miloco;
    const cJSON *stored_host;
    const cJSON *stored_port;
    const cJSON *stored_token;
    uint16_t resolved_port = SMART_HOME_MILOCO_DEFAULT_PORT_SECRET;
    int ret;

    if (!host || host_size == 0u || !port || !token || token_size == 0u) {
        return AGENT_ERROR_INVALID;
    }
    host[0] = '\0';
    token[0] = '\0';
    *port = resolved_port;
    ret = read_secrets_file(&text, &text_size);
    if (ret != AGENT_OK) {
        return ret;
    }
    root = cJSON_ParseWithLength(text, text_size);
    secure_clear(text, text_size + 1u);
    free(text);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return AGENT_ERROR_PARSE;
    }
    version = cJSON_GetObjectItemCaseSensitive(root, "version");
    miloco = cJSON_GetObjectItemCaseSensitive(root, "miloco");
    if (!cJSON_IsNumber(version) ||
        version->valueint != SMART_HOME_SECRETS_VERSION ||
        !cJSON_IsObject(miloco)) {
        cJSON_Delete(root);
        return AGENT_ERROR_NOT_FOUND;
    }
    stored_host = cJSON_GetObjectItemCaseSensitive(miloco, "host");
    stored_port = cJSON_GetObjectItemCaseSensitive(miloco, "port");
    stored_token = cJSON_GetObjectItemCaseSensitive(miloco, "token");
    if (!cJSON_IsString(stored_host) || !stored_host->valuestring[0] ||
        strlen(stored_host->valuestring) >= host_size ||
        contains_control(stored_host->valuestring)) {
        cJSON_Delete(root);
        return AGENT_ERROR_NOT_FOUND;
    }
    if (cJSON_IsNumber(stored_port) && stored_port->valueint > 0 &&
        stored_port->valueint <= 65535) {
        resolved_port = (uint16_t)stored_port->valueint;
    }
    if (cJSON_IsString(stored_token) &&
        strlen(stored_token->valuestring) < token_size &&
        !contains_control(stored_token->valuestring)) {
        memcpy(token, stored_token->valuestring,
               strlen(stored_token->valuestring) + 1u);
    }
    memcpy(host, stored_host->valuestring,
           strlen(stored_host->valuestring) + 1u);
    *port = resolved_port;
    cJSON_Delete(root);
    return AGENT_OK;
}

int smart_home_secrets_set_miloco(const char *host, uint16_t port,
                                  const char *token)
{
    cJSON *root = NULL, *miloco;
    int ret;
    if (!host || !host[0] || strlen(host) > 63 || !port ||
        (token && strlen(token) > 63) || contains_control(host) ||
        (token && contains_control(token))) return AGENT_ERROR_INVALID;
    pthread_mutex_lock(&g_secrets_writer);
    ret = load_update_root(&root);
    if (ret != AGENT_OK) goto out;
    miloco = update_object(root, "miloco");
    if (!miloco) { ret = AGENT_ERROR_PARSE; goto out; }
    cJSON_DeleteItemFromObjectCaseSensitive(miloco, "host");
    cJSON_DeleteItemFromObjectCaseSensitive(miloco, "port");
    cJSON_DeleteItemFromObjectCaseSensitive(miloco, "token");
    if (!cJSON_AddStringToObject(miloco, "host", host) ||
        !cJSON_AddNumberToObject(miloco, "port", port) ||
        !cJSON_AddStringToObject(miloco, "token", token ? token : "")) {
        ret = AGENT_ERROR_NOMEM;
        goto out;
    }
    ret = save_update_root(root);
out:
    cJSON_Delete(root);
    pthread_mutex_unlock(&g_secrets_writer);
    return ret;
}
