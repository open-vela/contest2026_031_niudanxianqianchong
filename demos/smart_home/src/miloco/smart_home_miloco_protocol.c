/* SPDX-License-Identifier: Apache-2.0 */
#include "smart_home_miloco_protocol.h"
#include "../config/cjson_compat.h"
#include <agent/error.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

static bool integer(const cJSON *item, int32_t *value)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < INT32_MIN || item->valuedouble > INT32_MAX ||
        trunc(item->valuedouble) != item->valuedouble) return false;
    *value = (int32_t)item->valuedouble;
    return true;
}

static bool success(const cJSON *object)
{
    int32_t code;
    return cJSON_IsObject(object) &&
        integer(cJSON_GetObjectItemCaseSensitive(object, "code"), &code) && code == 0;
}

static bool control_success(const cJSON *object)
{
    int32_t code;
    return cJSON_IsObject(object) &&
        integer(cJSON_GetObjectItemCaseSensitive(object, "code"), &code) &&
        (code == 0 || code == -702000000 || code == -702010000);
}

int smart_home_miloco_power_index(const smart_home_miloco_device_t *device)
{
    int found = -1;
    if (!device || device->control_count > SMART_HOME_MILOCO_MAX_CONTROLS) return -1;
    for (unsigned i = 0; i < device->control_count; i++) {
        const smart_home_miloco_control_t *c = &device->controls[i];
        if (c->type != SMART_HOME_MILOCO_CTRL_BOOL || c->write_only ||
            strcmp(c->type_name, "on")) continue;
        /* Multiple on properties may include indicator lights. No guessing. */
        if (found >= 0) return -1;
        found = (int)i;
    }
    return found;
}

bool smart_home_miloco_value_allowed(const smart_home_miloco_control_t *c,
                                      int32_t value)
{
    if (!c) return false;
    if (c->type == SMART_HOME_MILOCO_CTRL_BOOL) return value == 0 || value == 1;
    if (c->type == SMART_HOME_MILOCO_CTRL_RANGE)
        return c->range.step > 0 && value >= c->range.minimum &&
            value <= c->range.maximum &&
            ((int64_t)value - c->range.minimum) % c->range.step == 0;
    if (c->type == SMART_HOME_MILOCO_CTRL_ENUM) {
        for (unsigned i = 0; i < c->option_count && i < SMART_HOME_MILOCO_MAX_OPTIONS; i++)
            if (c->options[i].value == value) return true;
    }
    return false;
}

int smart_home_miloco_status_apply(smart_home_miloco_device_t *device,
                                    const char *body, bool *changed)
{
    cJSON *root;
    const cJSON *data, *item;
    bool seen[SMART_HOME_MILOCO_MAX_CONTROLS] = {false};
    bool valid[SMART_HOME_MILOCO_MAX_CONTROLS] = {false};
    int32_t values[SMART_HOME_MILOCO_MAX_CONTROLS] = {0};
    int ret = AGENT_OK;
    if (!device || !changed || device->control_count > SMART_HOME_MILOCO_MAX_CONTROLS)
        return AGENT_ERROR_INVALID;
    *changed = false;
    root = body ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) data = cJSON_GetObjectItemCaseSensitive(data, "properties");
    if (!success(root) || !cJSON_IsArray(data)) ret = AGENT_ERROR_PARSE;
    else cJSON_ArrayForEach(item, data) {
        const cJSON *iid = cJSON_GetObjectItemCaseSensitive(item, "iid");
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "value");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "code");
        int32_t result;
        if (!cJSON_IsString(iid)) continue;
        for (unsigned i = 0; i < device->control_count; i++) {
            const smart_home_miloco_control_t *c = &device->controls[i];
            if (strcmp(c->iid, iid->valuestring)) continue;
            if (seen[i]) { valid[i] = false; break; }
            seen[i] = true;
            if (c->write_only || c->type == SMART_HOME_MILOCO_CTRL_ACTION ||
                (code && (!integer(code, &result) || result != 0))) break;
            if (c->type == SMART_HOME_MILOCO_CTRL_BOOL && cJSON_IsBool(value)) {
                values[i] = cJSON_IsTrue(value);
                valid[i] = true;
            } else if (integer(value, &values[i])) {
                valid[i] = smart_home_miloco_value_allowed(c, values[i]);
            }
            break;
        }
    }
    for (unsigned i = 0; i < device->control_count; i++) {
        smart_home_miloco_control_t *c = &device->controls[i];
        if (valid[i]) {
            if (c->value != values[i]) *changed = true;
            c->value = values[i];
            if (++c->observation == 0) ++c->observation;
        }
        if (c->value_valid != valid[i]) *changed = true;
        c->value_valid = valid[i];
    }
    {
        int power = smart_home_miloco_power_index(device);
        bool power_valid = power >= 0 && device->controls[power].value_valid;
        bool on = power >= 0 && device->controls[power].value != 0;
        if (device->power_valid != power_valid || device->power_on != on) *changed = true;
        device->controllable = power >= 0;
        device->power_valid = power_valid;
        device->power_on = on;
    }
    cJSON_Delete(root);
    return ret;
}

int smart_home_miloco_control_reply(const char *body, const char *did,
                                     const char *iid, bool action)
{
    cJSON *root = body ? cJSON_ParseWithOpts(body, NULL, true) : NULL;
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    const cJSON *result;
    int ret = AGENT_ERROR_PARSE;
    unsigned siid, piid;
    int consumed = 0;
    const char *format = action ? "action.%u.%u%n" : "prop.%u.%u%n";
    if (!iid || !did || sscanf(iid, format, &siid, &piid, &consumed) != 2 ||
        iid[consumed] || !success(root)) goto done;
    result = cJSON_GetObjectItemCaseSensitive(data, action ? "result" : "results");
    if (!result && cJSON_IsArray(data)) result = data;
    if (cJSON_IsArray(result)) {
        if (cJSON_GetArraySize(result) != 1) goto done;
        result = cJSON_GetArrayItem(result, 0);
    }
    if (!cJSON_IsObject(result)) goto done;
    {
        const cJSON *key = cJSON_GetObjectItemCaseSensitive(result, "iid");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(result, "did");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(result, "siid");
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(result, action ? "aiid" : "piid");
        int32_t number;
        if ((key && (!cJSON_IsString(key) || strcmp(key->valuestring, iid))) ||
            (id && (!cJSON_IsString(id) || strcmp(id->valuestring, did))) ||
            (s && (!integer(s, &number) || number < 0 || (unsigned)number != siid)) ||
            (p && (!integer(p, &number) || number < 0 || (unsigned)number != piid))) goto done;
    }
    {
        int32_t code;
        if (!integer(cJSON_GetObjectItemCaseSensitive(result, "code"), &code))
            goto done;
        if (code != 0)
            syslog(LOG_INFO, "[miloco] control item code=%ld\n", (long)code);
        ret = control_success(result) ? AGENT_OK : AGENT_ERROR_TOOL_FAILED;
    }
done:
    cJSON_Delete(root);
    return ret;
}
