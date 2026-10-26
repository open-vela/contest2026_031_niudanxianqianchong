/* SPDX-License-Identifier: Apache-2.0 */
/* 专用 secrets.json loader；刻意不复用可回退、可写回的 config_store。 */

#include "smart_home_secrets.h"

#include "cjson_compat.h"

#include <agent/error.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <syslog.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SMART_HOME_SECRETS_VERSION 1

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

static int lookup_model_api_key(const char *backend_id,
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

    if (!backend_id || !out || out_size == 0u || !valid_backend_id(backend_id)) {
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
    keys = cJSON_GetObjectItemCaseSensitive(root, "model_api_keys");
    value = cJSON_IsObject(keys)
                ? cJSON_GetObjectItemCaseSensitive(keys, backend_id) : NULL;
    if (!cJSON_IsNumber(version) || version->valueint != SMART_HOME_SECRETS_VERSION ||
        !cJSON_IsObject(keys) || !cJSON_IsString(value) ||
        !value->valuestring[0]) {
        ret = (!cJSON_IsNumber(version) || version->valueint != SMART_HOME_SECRETS_VERSION ||
               !cJSON_IsObject(keys)) ? AGENT_ERROR_PARSE : AGENT_ERROR_NOT_FOUND;
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

int smart_home_secrets_get_model_api_key(const char *backend_id,
                                         char *out,
                                         size_t out_size)
{
    int ret;

    if (out && out_size > 0u) {
        out[0] = '\0';
    }
    ret = lookup_model_api_key(backend_id, out, out_size);
    if (ret != AGENT_OK && out && out_size > 0u) {
        secure_clear(out, out_size);
    }
    return ret;
}

int smart_home_secrets_model_api_key_status(const char *backend_id)
{
    char key[256];
    int ret = smart_home_secrets_get_model_api_key(backend_id,
                                                   key,
                                                   sizeof(key));

    secure_clear(key, sizeof(key));
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
    char *text = NULL;
    size_t text_size = 0u;
    cJSON *root = NULL;
    cJSON *wifi;
    cJSON *version;
    char *serialized = NULL;
    char temp_path[192];
    FILE *file = NULL;
    int ret;

    if (!ssid || !password || !ssid[0] || strlen(ssid) > 32u ||
        strlen(password) > 64u || contains_control(ssid) ||
        contains_control(password)) {
        return AGENT_ERROR_INVALID;
    }
    ret = read_secrets_file(&text, &text_size);
    if (ret == AGENT_ERROR_NOT_FOUND) {
        /* A first-time Wi-Fi setup must work before a model credential has
         * been provisioned.  Create the minimal versioned document. */
        root = cJSON_CreateObject();
        if (!root || !cJSON_AddNumberToObject(root, "version",
                                               SMART_HOME_SECRETS_VERSION) ||
            !cJSON_AddObjectToObject(root, "model_api_keys")) {
            cJSON_Delete(root);
            return AGENT_ERROR_NOMEM;
        }
    } else {
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
    }
    wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
    if (!cJSON_IsObject(wifi)) {
        wifi = cJSON_AddObjectToObject(root, "wifi");
    }
    if (!wifi) {
        cJSON_Delete(root);
        return AGENT_ERROR_NOMEM;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(wifi, "ssid");
    cJSON_DeleteItemFromObjectCaseSensitive(wifi, "password");
    if (!cJSON_AddStringToObject(wifi, "ssid", ssid) ||
        !cJSON_AddStringToObject(wifi, "password", password)) {
        cJSON_Delete(root);
        return AGENT_ERROR_NOMEM;
    }
    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    syslog(LOG_INFO, "[milo] sec: serialized\n");
    if (!serialized) {
        return AGENT_ERROR_NOMEM;
    }
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                 CONFIG_SMART_HOME_MODEL_SECRETS_PATH) >= (int)sizeof(temp_path)) {
        secure_clear(serialized, strlen(serialized));
        free(serialized);
        return AGENT_ERROR_LIMIT;
    }
    file = fopen(temp_path, "wb");
    if (!file) {
        secure_clear(serialized, strlen(serialized));
        free(serialized);
        return AGENT_ERROR;
    }
    if (fwrite(serialized, 1u, strlen(serialized), file) != strlen(serialized) ||
        fflush(file) != 0) {
        fclose(file);
        unlink(temp_path);
        secure_clear(serialized, strlen(serialized));
        free(serialized);
        return AGENT_ERROR;
    }
    if (fclose(file) != 0) {
        unlink(temp_path);
        secure_clear(serialized, strlen(serialized));
        free(serialized);
        return AGENT_ERROR;
    }
    if (rename(temp_path, CONFIG_SMART_HOME_MODEL_SECRETS_PATH) != 0) {
        unlink(temp_path);
        secure_clear(serialized, strlen(serialized));
        free(serialized);
        return AGENT_ERROR;
    }
    secure_clear(serialized, strlen(serialized));
    free(serialized);
    return AGENT_OK;
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
    char *text = NULL;
    size_t text_size = 0u;
    cJSON *root = NULL;
    cJSON *miloco;
    cJSON *version;
    char *serialized = NULL;
    char temp_path[192];
    FILE *file = NULL;
    int ret;

    if (!host || !host[0] || strlen(host) > 63u || port == 0u ||
        (token && strlen(token) > 63u) || contains_control(host) ||
        (token && contains_control(token))) {
        return AGENT_ERROR_INVALID;
    }
    syslog(LOG_INFO, "[milo] sec: read begin\n");
    ret = read_secrets_file(&text, &text_size);
    syslog(LOG_INFO, "[milo] sec: read ret=%d\n", ret);
    if (ret == AGENT_ERROR_NOT_FOUND) {
        root = cJSON_CreateObject();
        if (!root || !cJSON_AddNumberToObject(root, "version",
                                               SMART_HOME_SECRETS_VERSION) ||
            !cJSON_AddObjectToObject(root, "model_api_keys")) {
            cJSON_Delete(root);
            return AGENT_ERROR_NOMEM;
        }
    } else {
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
    }
    miloco = cJSON_GetObjectItemCaseSensitive(root, "miloco");
    if (!cJSON_IsObject(miloco)) {
        miloco = cJSON_AddObjectToObject(root, "miloco");
    }
    if (!miloco) {
        cJSON_Delete(root);
        return AGENT_ERROR_NOMEM;
    }
    cJSON_DeleteItemFromObjectCaseSensitive(miloco, "host");
    cJSON_DeleteItemFromObjectCaseSensitive(miloco, "port");
    cJSON_DeleteItemFromObjectCaseSensitive(miloco, "token");
    if (!cJSON_AddStringToObject(miloco, "host", host) ||
        !cJSON_AddNumberToObject(miloco, "port", (double)port) ||
        !cJSON_AddStringToObject(miloco, "token", token ? token : "")) {
        cJSON_Delete(root);
        return AGENT_ERROR_NOMEM;
    }
    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!serialized) {
        return AGENT_ERROR_NOMEM;
    }
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp",
                 CONFIG_SMART_HOME_MODEL_SECRETS_PATH) >= (int)sizeof(temp_path)) {
        free(serialized);
        return AGENT_ERROR_LIMIT;
    }
    file = fopen(temp_path, "wb");
    syslog(LOG_INFO, "[milo] sec: fopen ret=%d\n", file ? 0 : -1);
    if (!file) {
        free(serialized);
        return AGENT_ERROR;
    }
    if (fwrite(serialized, 1u, strlen(serialized), file) != strlen(serialized) ||
        fflush(file) != 0) {
        fclose(file);
        unlink(temp_path);
        free(serialized);
        return AGENT_ERROR;
    }
    syslog(LOG_INFO, "[milo] sec: fwrite done\n");
    if (fclose(file) != 0) {
        unlink(temp_path);
        free(serialized);
        return AGENT_ERROR;
    }
    syslog(LOG_INFO, "[milo] sec: fclose done\n");
    if (rename(temp_path, CONFIG_SMART_HOME_MODEL_SECRETS_PATH) != 0) {
        unlink(temp_path);
        free(serialized);
        return AGENT_ERROR;
    }
    syslog(LOG_INFO, "[milo] sec: rename done\n");
    free(serialized);
    return AGENT_OK;
}
