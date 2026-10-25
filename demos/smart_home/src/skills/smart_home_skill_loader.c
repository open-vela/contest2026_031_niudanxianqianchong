#include "smart_home_skill_loader.h"
#include <agent/skill.h>
#include "../tools/smart_home_tool_io.h"
#include <errno.h>
#include <limits.h>

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SMART_HOME_SKILL_ENABLED 1u
#define SMART_HOME_SKILL_VISIBLE 2u
#define SMART_HOME_SKILL_SUMMARY 4u

#define SMART_HOME_SKILL_READ_MAX 4096u

static int has_md_suffix(const char *name)
{
    size_t len;

    if (!name) {
        return 0;
    }

    len = strlen(name);
    return len > 3u && strcmp(name + len - 3u, ".md") == 0;
}

static char *trim(char *text)
{
    char *end;

    if (!text) {
        return text;
    }

    while (*text == ' ' || *text == '\t' ||
           *text == '\r' || *text == '\n') {
        text++;
    }

    end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' ||
            end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }

    return text;
}

static int parse_flags(const char *text)
{
    uint32_t flags = 0u;

    if (!text) {
        return AGENT_ERROR_INVALID;
    }

    while (*text) {
        size_t n = strcspn(text, ",");
        if (n == 7u && !memcmp(text, "enabled", n)) flags |= SMART_HOME_SKILL_ENABLED;
        else if (n == 11u && !memcmp(text, "llm_visible", n)) flags |= SMART_HOME_SKILL_VISIBLE;
        else if (n == 12u && !memcmp(text, "summary_only", n)) flags |= SMART_HOME_SKILL_SUMMARY;
        else return AGENT_ERROR_INVALID;
        text += n;
        if (*text) ++text;
    }

    return (int)flags;
}

static int parse_front_matter(char *buffer,
                              smart_home_loaded_skill_t *loaded,
                              agent_skill_t *skill)
{
    char *body;
    char *meta;
    char *line;
    char *save = NULL;
    unsigned flags = SMART_HOME_SKILL_ENABLED | SMART_HOME_SKILL_VISIBLE;

    if (!buffer || !loaded || !skill || strncmp(buffer, "---", 3) != 0) {
        return AGENT_ERROR_INVALID;
    }

    meta = buffer + 3;
    if (*meta == '\r') {
        meta++;
    }
    if (*meta == '\n') {
        meta++;
    }

    body = strstr(meta, "\n---");
    if (!body) {
        return AGENT_ERROR_INVALID;
    }

    *body = '\0';
    body++;
    if (strncmp(body, "---", 3) == 0) {
        body += 3;
    }
    if (*body == '\r') {
        body++;
    }
    if (*body == '\n') {
        body++;
    }

    memset(skill, 0, sizeof(*skill));
    line = strtok_r(meta, "\n", &save);
    while (line) {
        char *colon = strchr(line, ':');

        if (colon) {
            char *key;
            char *value;

            *colon = '\0';
            key = trim(line);
            value = trim(colon + 1);

            if (strcmp(key, "name") == 0) {
                loaded->name = value;
                skill->name = agent_string_view(value, strlen(value));
            } else if (strcmp(key, "description") == 0) {
                loaded->description = value;
                skill->description = agent_string_view(value, strlen(value));
            } else if (strcmp(key, "priority") == 0) {
                char *end;
                long priority;
                errno = 0;
                priority = strtol(value, &end, 10);
                if (errno || !value[0] || *end || priority < INT32_MIN || priority > INT32_MAX)
                    return AGENT_ERROR_INVALID;
                skill->priority = (int32_t)priority;
            } else if (strcmp(key, "flags") == 0) {
                int parsed = parse_flags(value);
                if (parsed < 0) {
                    return parsed;
                }
                flags = (unsigned)parsed;
            }
        }
        line = strtok_r(NULL, "\n", &save);
    }

    loaded->context_text = trim(body);
    if (!skill->name.size || !loaded->context_text[0]) return AGENT_ERROR_INVALID;
    loaded->visible = (flags & (SMART_HOME_SKILL_ENABLED | SMART_HOME_SKILL_VISIBLE)) ==
                      (SMART_HOME_SKILL_ENABLED | SMART_HOME_SKILL_VISIBLE);
    if (flags & SMART_HOME_SKILL_SUMMARY) {
        int n = snprintf(loaded->summary, sizeof(loaded->summary),
                         "Skill %s: %s Use read_skill(name=\"%s\") for full instructions.",
                         loaded->name, loaded->description ? loaded->description : "",
                         loaded->name);
        if (n < 0 || (size_t)n >= sizeof(loaded->summary)) return AGENT_ERROR_CAPACITY;
        skill->content = agent_string_view(loaded->summary, (size_t)n);
    } else {
        skill->content = agent_string_view(loaded->context_text, strlen(loaded->context_text));
    }

    return AGENT_OK;
}

static int read_file(const char *path, char **buffer_out)
{
    FILE *fp;
    long size;
    char *buffer;
    size_t nread;

    if (!path || !buffer_out) {
        return AGENT_ERROR_INVALID;
    }

    *buffer_out = NULL;
    fp = fopen(path, "rb");
    if (!fp) {
        return AGENT_ERROR_IO;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return AGENT_ERROR;
    }
    size = ftell(fp);
    if (size <= 0 || (size_t)size > SMART_HOME_SKILL_READ_MAX) {
        fclose(fp);
        return AGENT_ERROR_LIMIT;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return AGENT_ERROR;
    }

    buffer = (char *)calloc(1u, (size_t)size + 1u);
    if (!buffer) {
        fclose(fp);
        return AGENT_ERROR_NOMEM;
    }

    nread = fread(buffer, 1u, (size_t)size, fp);
    fclose(fp);
    if (nread != (size_t)size) {
        free(buffer);
        return AGENT_ERROR;
    }

    buffer[nread] = '\0';
    *buffer_out = buffer;
    return AGENT_OK;
}

void smart_home_skill_store_init(smart_home_skill_store_t *store)
{
    if (store) {
        memset(store, 0, sizeof(*store));
    }
}

void smart_home_skill_store_deinit(smart_home_skill_store_t *store)
{
    size_t i;

    if (!store) {
        return;
    }

    for (i = 0u; i < store->count; i++) {
        free(store->skills[i].buffer);
    }
    memset(store, 0, sizeof(*store));
}

const smart_home_loaded_skill_t *smart_home_skill_store_find(
    const smart_home_skill_store_t *store,
    const char *name)
{
    size_t i;

    if (!store || !name) {
        return NULL;
    }

    for (i = 0u; i < store->count; i++) {
        if (store->skills[i].name &&
            strcmp(store->skills[i].name, name) == 0) {
            return &store->skills[i];
        }
    }

    return NULL;
}

int smart_home_skill_loader_load_dir(agent_t *agent,
                                     smart_home_skill_store_t *store,
                                     const char *dir)
{
    DIR *dp;
    struct dirent *entry;
    size_t loaded_count = 0u;

    if (!agent || !store || !dir) {
        return AGENT_ERROR_INVALID;
    }

    dp = opendir(dir);
    if (!dp) {
        return errno == ENOENT ? AGENT_ERROR_NOT_FOUND : AGENT_ERROR_IO;
    }

    while ((entry = readdir(dp)) != NULL) {
        smart_home_loaded_skill_t *loaded;
        agent_skill_t skill;
        char path[256];
        char *buffer;
        int ret;

        if (!has_md_suffix(entry->d_name)) {
            continue;
        }
        if (store->count >= SMART_HOME_SKILL_STORE_MAX) {
            closedir(dp);
            return AGENT_ERROR_LIMIT;
        }

        ret = snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
        if (ret < 0 || (size_t)ret >= sizeof(path)) {
            closedir(dp);
            return AGENT_ERROR_CAPACITY;
        }
        ret = read_file(path, &buffer);
        if (ret != AGENT_OK) {
            closedir(dp);
            return ret;
        }

        loaded = &store->skills[store->count];
        memset(loaded, 0, sizeof(*loaded));
        loaded->buffer = buffer;
        ret = parse_front_matter(buffer, loaded, &skill);
        if (ret != AGENT_OK) {
            free(buffer);
            closedir(dp);
            return ret;
        }

        ret = loaded->visible ? agent_register_skill(agent, &skill) : AGENT_OK;
        if (ret != AGENT_OK) {
            free(buffer);
            closedir(dp);
            return ret;
        }

        store->count++;
        loaded_count++;
    }

    closedir(dp);
    return loaded_count > 0u ? AGENT_OK : AGENT_ERROR_NOT_FOUND;
}

agent_error_t smart_home_skill_read_tool(void *user_data,
                                        const agent_tool_context_t *context,
                                        const agent_text_sink_t *output)
{
    const smart_home_skill_store_t *store = user_data;
    const smart_home_loaded_skill_t *skill;
    char name[64];
    char *encoded;
    cJSON *json;
    agent_error_t ret;

    if (!context || smart_home_tool_get_string(context->call.arguments_json,
                                               "name", name, sizeof(name)))
        return AGENT_ERROR_TOOL_ARGUMENT;
    skill = smart_home_skill_store_find(store, name);
    if (!skill || !skill->visible)
        return smart_home_tool_reply(output, "{\"ok\":false,\"error\":\"skill_not_found\"}", AGENT_ERROR_NOT_FOUND);
    json = cJSON_CreateObject();
    if (!json) return AGENT_ERROR_NOMEM;
    if (!cJSON_AddBoolToObject(json, "ok", 1) ||
        !cJSON_AddStringToObject(json, "name", name) ||
        !cJSON_AddStringToObject(json, "content", skill->context_text)) {
        cJSON_Delete(json);
        return AGENT_ERROR_NOMEM;
    }
    encoded = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!encoded) return AGENT_ERROR_NOMEM;
    ret = smart_home_tool_reply(output, encoded, AGENT_OK);
    cJSON_free(encoded);
    return ret;
}
