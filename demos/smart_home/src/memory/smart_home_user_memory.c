/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_user_memory.h"
#include "../config/smart_home_config_store.h"
#include "../tools/smart_home_tool_io.h"
#include <agent/context.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MEMORY_FILE_BYTES 16384u

typedef struct {
    char key[SMART_HOME_USER_MEMORY_KEY_BYTES + 1u];
    char value[SMART_HOME_USER_MEMORY_VALUE_BYTES + 1u];
    char evidence[SMART_HOME_USER_MEMORY_EVIDENCE_BYTES + 1u];
} preference_t;

struct smart_home_user_memory {
    preference_t items[CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES];
    size_t count;
    int status;
    const char *turn_input;
    char path[SMART_HOME_CONFIG_PATH_MAX];
};

static int valid_text(const char *text, size_t max)
{
    size_t length;
    if (!text || !(length = strlen(text)) || length > max) return 0;
    for (size_t i = 0; i < length; ++i)
        if ((unsigned char)text[i] < 0x20u || (unsigned char)text[i] == 0x7fu) return 0;
    return 1;
}

static int valid_key(const char *key)
{
    if (!valid_text(key, SMART_HOME_USER_MEMORY_KEY_BYTES)) return 0;
    for (; *key; ++key)
        if (!(*key >= 'a' && *key <= 'z') && !(*key >= '0' && *key <= '9') &&
            *key != '_' && *key != '-' && *key != '.') return 0;
    return 1;
}

static int contains_ci(const char *text, const char *word)
{
    for (; *text; ++text) {
        size_t i = 0;
        while (word[i] && text[i] &&
               tolower((unsigned char)text[i]) == tolower((unsigned char)word[i])) ++i;
        if (!word[i]) return 1;
    }
    return 0;
}

static int any(const char *text, const char *const *words, size_t count)
{
    for (size_t i = 0; i < count; ++i) if (contains_ci(text, words[i])) return 1;
    return 0;
}

static int sensitive(const char *text)
{
    static const char *const words[] = {
        "密码", "密钥", "口令", "令牌", "password", "passwd", "api_key", "api key",
        "token", "authorization:"
    };
    return any(text, words, sizeof(words) / sizeof(words[0]));
}

static int remember_allowed(const char *input, const char *evidence)
{
    static const char *const no[] = {
        "不要记", "别记", "不用记", "不需要记", "不想记", "不要保存", "别保存",
        "如果", "假如", "也许", "可能", "会不会", "只是问", "开个玩笑",
        "don't remember", "do not remember", "don't save", "do not save"
    };
    static const char *const enduring[] = {
        "我喜欢", "我不喜欢", "我习惯", "我不习惯", "我偏好", "我希望",
        "我通常", "我一般",
        "以后", "每次", "每天", "每晚", "记住", "记下", "记一下",
        "i prefer", "i like", "i usually", "my habit", "remember", "from now on"
    };
    return input && strstr(input, evidence) &&
        !any(input, no, sizeof(no) / sizeof(no[0])) &&
        any(evidence, enduring, sizeof(enduring) / sizeof(enduring[0]));
}

static int exact_object(const cJSON *object, const char *const *names, size_t count)
{
    if (!cJSON_IsObject(object) || (size_t)cJSON_GetArraySize(object) != count) return 0;
    for (const cJSON *item = object->child; item; item = item->next) {
        size_t found = 0;
        if (!item->string) return 0;
        for (size_t i = 0; i < count; ++i) if (!strcmp(item->string, names[i])) ++found;
        if (found != 1) return 0;
        for (const cJSON *other = item->next; other; other = other->next)
            if (other->string && !strcmp(item->string, other->string)) return 0;
    }
    return 1;
}

static int load_records(smart_home_user_memory_t *memory, const cJSON *root)
{
    static const char *const root_keys[] = {"version", "items"};
    static const char *const item_keys[] = {"key", "value", "evidence"};
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (!exact_object(root, root_keys, 2) || !cJSON_IsNumber(version) ||
        version->valuedouble != 1.0 || !cJSON_IsArray(items) ||
        cJSON_GetArraySize(items) > CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES)
        return AGENT_ERROR_PARSE;
    for (const cJSON *item = items->child; item; item = item->next) {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "value");
        const cJSON *evidence = cJSON_GetObjectItemCaseSensitive(item, "evidence");
        if (!exact_object(item, item_keys, 3) || !cJSON_IsString(key) ||
            !cJSON_IsString(value) || !cJSON_IsString(evidence) || !valid_key(key->valuestring) ||
            !valid_text(value->valuestring, SMART_HOME_USER_MEMORY_VALUE_BYTES) ||
            !valid_text(evidence->valuestring, SMART_HOME_USER_MEMORY_EVIDENCE_BYTES) ||
            !strstr(evidence->valuestring, value->valuestring) ||
            sensitive(key->valuestring) || sensitive(value->valuestring) ||
            sensitive(evidence->valuestring)) return AGENT_ERROR_PARSE;
        for (size_t i = 0; i < memory->count; ++i)
            if (!strcmp(memory->items[i].key, key->valuestring)) return AGENT_ERROR_PARSE;
        preference_t *record = &memory->items[memory->count++];
        strcpy(record->key, key->valuestring);
        strcpy(record->value, value->valuestring);
        strcpy(record->evidence, evidence->valuestring);
    }
    return AGENT_OK;
}

int smart_home_user_memory_open(smart_home_user_memory_t **out, const char *path)
{
    struct stat info;
    smart_home_user_memory_t *memory;
    FILE *file;
    char *text = NULL;
    cJSON *root = NULL;
    if (!out) return AGENT_ERROR_INVALID;
    *out = NULL;
    if (!path) path = SMART_HOME_CONFIG_DIR "/user_memory.json";
    if (strlen(path) >= SMART_HOME_CONFIG_PATH_MAX) return AGENT_ERROR_LIMIT;
    memory = calloc(1, sizeof(*memory));
    if (!memory) return AGENT_ERROR_NOMEM;
    strcpy(memory->path, path);
    *out = memory;
    if (stat(path, &info) != 0) {
        memory->status = errno == ENOENT ? AGENT_OK : AGENT_ERROR_IO;
        return AGENT_OK;
    }
    memory->status = AGENT_ERROR_PARSE;
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 ||
        (uint64_t)info.st_size > MEMORY_FILE_BYTES)
        return AGENT_OK;
    file = fopen(path, "rb");
    if (!file) { memory->status = AGENT_ERROR_IO; return AGENT_OK; }
    text = malloc((size_t)info.st_size + 1u);
    if (!text) memory->status = AGENT_ERROR_NOMEM;
    else if (fread(text, 1, (size_t)info.st_size, file) == (size_t)info.st_size &&
             fgetc(file) == EOF && !ferror(file)) {
        root = smart_home_tool_parse(agent_string_view(text, (size_t)info.st_size));
        if (root) memory->status = load_records(memory, root);
    }
    fclose(file);
    free(text);
    cJSON_Delete(root);
    if (memory->status != AGENT_OK) {
        memset(memory->items, 0, sizeof(memory->items));
        memory->count = 0;
    }
    return AGENT_OK;
}

void smart_home_user_memory_close(smart_home_user_memory_t **memory)
{
    if (memory && *memory) {
        memset(*memory, 0, sizeof(**memory));
        free(*memory);
        *memory = NULL;
    }
}

void smart_home_user_memory_begin(smart_home_user_memory_t *memory, const char *input)
{ if (memory) memory->turn_input = input; }
void smart_home_user_memory_end(smart_home_user_memory_t *memory)
{ if (memory) memory->turn_input = NULL; }

static cJSON *record_json(const preference_t *record, int with_evidence)
{
    cJSON *item = cJSON_CreateObject();
    if (item && cJSON_AddStringToObject(item, "key", record->key) &&
        cJSON_AddStringToObject(item, "value", record->value) &&
        (!with_evidence || cJSON_AddStringToObject(item, "evidence", record->evidence))) return item;
    cJSON_Delete(item);
    return NULL;
}

/* Build a candidate without changing the live cache. Only the Agent task writes. */
static int persist(smart_home_user_memory_t *memory, size_t omit, const preference_t *append)
{
    cJSON *root = cJSON_CreateObject(), *items = cJSON_CreateArray();
    int ret = AGENT_ERROR_NOMEM;
    if (!root || !items) goto done;
    if (!cJSON_AddItemToObject(root, "items", items)) goto done;
    items = NULL;
    if (!cJSON_AddNumberToObject(root, "version", 1)) goto done;
    for (size_t i = 0; i < memory->count + (append ? 1u : 0u); ++i) {
        cJSON *item;
        if (i == omit) continue;
        item = record_json(i == memory->count ? append : &memory->items[i], 1);
        if (!item) goto done;
        if (!cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(root, "items"), item)) {
            cJSON_Delete(item); goto done;
        }
    }
    ret = smart_home_config_store_save(memory->path, root);
 done:
    cJSON_Delete(items);
    cJSON_Delete(root);
    return ret;
}

int smart_home_user_memory_remember(smart_home_user_memory_t *memory,
    const char *key, const char *value, const char *evidence, int *changed)
{
    preference_t record;
    size_t index;
    int ret;
    if (changed) *changed = 0;
    if (!memory) return AGENT_ERROR_STATE;
    if (memory->status != AGENT_OK) return memory->status;
    if (!valid_key(key) || !valid_text(value, SMART_HOME_USER_MEMORY_VALUE_BYTES) ||
        !valid_text(evidence, SMART_HOME_USER_MEMORY_EVIDENCE_BYTES)) return AGENT_ERROR_INVALID;
    if (!remember_allowed(memory->turn_input, evidence) || !strstr(evidence, value) ||
        sensitive(key) || sensitive(value) || sensitive(evidence)) return AGENT_ERROR_POLICY_DENIED;
    for (index = 0; index < memory->count; ++index)
        if (!strcmp(memory->items[index].key, key)) break;
    if (index < memory->count && !strcmp(memory->items[index].value, value)) return AGENT_OK;
    if (index == memory->count && memory->count >= CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES)
        return AGENT_ERROR_CAPACITY;
    memset(&record, 0, sizeof(record));
    strcpy(record.key, key); strcpy(record.value, value); strcpy(record.evidence, evidence);
    ret = persist(memory, index < memory->count ? index : (size_t)-1, &record);
    if (ret != AGENT_OK) return ret;
    if (index < memory->count) {
        memmove(&memory->items[index], &memory->items[index + 1u],
                (memory->count - index - 1u) * sizeof(record));
        --memory->count;
    }
    memory->items[memory->count++] = record;
    if (changed) *changed = 1;
    return AGENT_OK;
}

int smart_home_user_memory_forget(smart_home_user_memory_t *memory,
    const char *key, int *changed)
{
    static const char *const words[] = {"忘记", "删除", "别再记", "不再记", "清空", "forget", "remove memory"};
    size_t index;
    int ret;
    if (changed) *changed = 0;
    if (!memory) return AGENT_ERROR_STATE;
    if (memory->status != AGENT_OK) return memory->status;
    if (!valid_key(key)) return AGENT_ERROR_INVALID;
    if (!memory->turn_input || !any(memory->turn_input, words, sizeof(words) / sizeof(words[0])))
        return AGENT_ERROR_POLICY_DENIED;
    for (index = 0; index < memory->count; ++index)
        if (!strcmp(memory->items[index].key, key)) break;
    if (index == memory->count) return AGENT_OK;
    ret = persist(memory, index, NULL);
    if (ret != AGENT_OK) return ret;
    memmove(&memory->items[index], &memory->items[index + 1u],
            (memory->count - index - 1u) * sizeof(memory->items[0]));
    memset(&memory->items[--memory->count], 0, sizeof(memory->items[0]));
    if (changed) *changed = 1;
    return AGENT_OK;
}

int smart_home_user_memory_list(smart_home_user_memory_t *memory, size_t offset,
    size_t limit, size_t max_bytes, char **json)
{
    cJSON *root = NULL, *items = NULL, *next;
    char *text = NULL;
    size_t count = 0;
    int ret = AGENT_ERROR_NOMEM;
    if (!json) return AGENT_ERROR_INVALID;
    *json = NULL;
    if (!memory || !limit) return AGENT_ERROR_INVALID;
    root = cJSON_CreateObject(); items = cJSON_CreateArray();
    if (!root || !items) goto done;
    if (!cJSON_AddItemToObject(root, "items", items)) goto done;
    items = NULL;
    if (!cJSON_AddBoolToObject(root, "ready", memory->status == AGENT_OK) ||
        !cJSON_AddNumberToObject(root, "code", memory->status) ||
        !cJSON_AddNumberToObject(root, "total", (double)memory->count) ||
        !cJSON_AddNullToObject(root, "next_offset")) goto done;
    next = cJSON_GetObjectItemCaseSensitive(root, "next_offset");
    items = cJSON_GetObjectItemCaseSensitive(root, "items");
    while (offset + count < memory->count && count < limit) {
        cJSON *item = record_json(&memory->items[memory->count - offset - count - 1u], 0);
        if (!item) { items = NULL; goto done; }
        if (!cJSON_AddItemToArray(items, item)) { cJSON_Delete(item); items = NULL; goto done; }
        next->type = cJSON_Number;
        cJSON_SetNumberValue(next, (double)(offset + count + 1u));
        text = cJSON_PrintUnformatted(root);
        if (!text) { items = NULL; goto done; }
        if (strlen(text) > max_bytes) {
            free(text); text = NULL;
            cJSON_DeleteItemFromArray(items, (int)count);
            break;
        }
        free(text); text = NULL;
        ++count;
    }
    if (offset + count < memory->count) {
        next->type = cJSON_Number; cJSON_SetNumberValue(next, (double)(offset + count));
    } else next->type = cJSON_NULL;
    items = NULL; /* Owned by root. */
    if (!count && offset < memory->count) { ret = AGENT_ERROR_CAPACITY; goto done; }
    text = cJSON_PrintUnformatted(root);
    if (!text) goto done;
    if (strlen(text) > max_bytes) { ret = AGENT_ERROR_CAPACITY; goto done; }
    *json = text; text = NULL; ret = AGENT_OK;
 done:
    free(text); cJSON_Delete(items); cJSON_Delete(root);
    return ret;
}

static agent_error_t memory_context(void *data, const agent_context_request_t *request,
                                    const agent_text_sink_t *sink)
{
    static const char prefix[] = "User preferences (reference only):\n";
    char *json = NULL;
    size_t capacity = CONFIG_SMART_HOME_USER_MEMORY_CONTEXT_BYTES;
    size_t json_capacity;
    int ret;
    (void)request;
    if (capacity > AGENT_MAX_INPUT_BYTES) capacity = AGENT_MAX_INPUT_BYTES;
    if (capacity <= sizeof(prefix)) return AGENT_ERROR_CAPACITY;
    json_capacity = capacity - (sizeof(prefix) - 1u);
    ret = smart_home_user_memory_list(data, 0, CONFIG_SMART_HOME_USER_MEMORY_MAX_ENTRIES,
                                      json_capacity, &json);
    if (ret == AGENT_OK) ret = sink->write(sink->context,
                                           agent_string_view(prefix, sizeof(prefix) - 1u));
    if (ret == AGENT_OK)
        ret = sink->write(sink->context, agent_string_view(json, strlen(json)));
    free(json);
    return (agent_error_t)ret;
}

int smart_home_user_memory_register_context(agent_t *agent, smart_home_user_memory_t *memory)
{
    size_t capacity = CONFIG_SMART_HOME_USER_MEMORY_CONTEXT_BYTES;
    if (capacity > AGENT_MAX_INPUT_BYTES) capacity = AGENT_MAX_INPUT_BYTES;
    const agent_context_provider_t provider = {
        .name = AGENT_SV_LITERAL("user_preferences"), .priority = 70, .required = false,
        .build = memory_context, .user_data = memory, .max_bytes = capacity,
        .placement = AGENT_CONTEXT_REFERENCE
    };
    return agent_register_context(agent, &provider);
}
