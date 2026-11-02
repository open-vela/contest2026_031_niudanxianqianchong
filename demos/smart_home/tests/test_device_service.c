/* SPDX-License-Identifier: Apache-2.0 */

#include <agent.h>
#include <agent/context.h>
#include <smart_home_device.h>
#include <smart_home_device_service.h>
#include <smart_home_scene_catalog.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_event_count;
static smart_home_device_event_t g_last_event;

static void on_event(const smart_home_device_event_t *event, void *user_data)
{
    int *count = (int *)user_data;

    assert(event != NULL);
    (*count)++;
    g_last_event = *event;
}

static void test_single_light_default_and_legacy_migration(void)
{
    smart_home_state_t state, restored;
    smart_home_device_init(&state);
    assert(smart_home_device_count(&state) == 1);
    assert(state.devices[0].type == SMART_HOME_DEVICE_LIGHT);
    assert(smart_home_device_add(&state, "bedroom", "Bedroom Light", SMART_HOME_DEVICE_LIGHT, NULL) == AGENT_OK);
    assert(smart_home_device_add(&state, "bedroom", "Bedroom AC", SMART_HOME_DEVICE_AC, NULL) == AGENT_OK);
    int user_id;
    assert(smart_home_device_add(&state, "bedroom", "User light", SMART_HOME_DEVICE_LIGHT, &user_id) == AGENT_OK);
    cJSON *saved = smart_home_device_state_to_json(&state);
    assert(saved && smart_home_device_state_from_json(&restored, saved) == AGENT_OK);
    cJSON_Delete(saved);
    smart_home_device_remove_legacy_demos(&restored);
    assert(smart_home_device_count(&restored) == 2);
    assert(smart_home_device_find_by_id(&restored, 1));
    assert(smart_home_device_find_by_id(&restored, user_id));
    assert(!smart_home_device_find_by_id(&restored, 2));
    assert(!smart_home_device_find_by_id(&restored, 3));
    assert(smart_home_room_count(&restored) == 2);
    smart_home_device_remove_legacy_demos(&restored);
    assert(smart_home_device_count(&restored) == 2);
}

static void test_state_mutation_emits_revisioned_event(void)
{
    smart_home_state_t state;
    smart_home_device_service_t service;
    char snapshot[2048];
    char events[2048];

    smart_home_device_init(&state);
    assert(smart_home_device_service_init(&service, &state) == AGENT_OK);
    smart_home_device_service_set_event_listener(&service, on_event,
                                                 &g_event_count);

    assert(smart_home_device_service_set_light(&service, "living_room", 1,
                                               73) == AGENT_OK);
    assert(smart_home_device_service_revision(&service) == 1u);
    assert(g_event_count == 1);
    assert(strcmp(g_last_event.type, "device_state_changed") == 0);
    assert(g_last_event.revision == 1u);
    assert(strstr(g_last_event.data_json, "\"brightness\":73") != NULL);

    assert(smart_home_device_service_build_snapshot_json(&service, snapshot,
                                                         sizeof(snapshot)) == AGENT_OK);
    assert(strstr(snapshot, "\"revision\":1") != NULL);
    assert(strstr(snapshot, "\"id\":\"1\"") != NULL);
    assert(strstr(snapshot, "\"brightness\":73") != NULL);

    assert(smart_home_device_service_build_events_json(&service, 0u, events,
                                                       sizeof(events)) == AGENT_OK);
    assert(strstr(events, "\"eventId\":\"evt-1\"") != NULL);
    assert(strstr(events, "\"latestRevision\":1") != NULL);
    smart_home_device_service_deinit(&service);
}

static void test_scene_catalog_drives_one_logical_revision(void)
{
    smart_home_state_t state;
    smart_home_device_service_t service;
    smart_home_scene_catalog_t catalog;
    static const char scene_skill[] =
        "# Scene policy\n"
        "<!-- scene_catalog:start -->\n"
        "{\"version\":1,\"scenes\":[{\"id\":\"focus\",\"actions\":["
        "{\"type\":\"light\",\"room\":\"living_room\",\"on\":true,\"brightness\":42},"
        "{\"type\":\"ac\",\"room\":\"bedroom\",\"on\":true,\"mode\":\"heat\",\"fanSpeed\":\"high\",\"temperature\":23}"
        "]}]}\n"
        "<!-- scene_catalog:end -->\n";

    smart_home_device_init(&state);
    int ac_id;
    assert(smart_home_device_add(&state, "bedroom", "Test AC", SMART_HOME_DEVICE_AC, &ac_id) == AGENT_OK);
    assert(smart_home_device_service_init(&service, &state) == AGENT_OK);
    assert(smart_home_scene_catalog_load(scene_skill, &catalog) == AGENT_OK);
    assert(smart_home_device_service_set_scene_catalog(&service, &catalog)
           == AGENT_OK);
    assert(smart_home_device_service_run_scene(&service, "focus") == AGENT_OK);
    assert(smart_home_device_service_revision(&service) == 1u);
    assert(state.devices[0].on == 1);
    assert(state.devices[0].brightness == 42);
    assert(smart_home_device_find_by_id(&state, ac_id)->on == 1);
    assert(smart_home_device_find_by_id(&state, ac_id)->ac_mode == 1);
    assert(smart_home_device_find_by_id(&state, ac_id)->ac_fan_speed == 2);
    assert(smart_home_device_find_by_id(&state, ac_id)->temperature == 23);
    assert(smart_home_device_service_run_scene(&service, "movie")
           == AGENT_ERROR_NOT_FOUND);
    assert(smart_home_device_service_revision(&service) == 1u);
    smart_home_device_service_deinit(&service);
}

static void test_room_catalog_allows_device_reassignment(void)
{
    smart_home_state_t state;
    smart_home_device_service_t service;
    smart_home_device_t *device;

    smart_home_device_init(&state);
    assert(smart_home_device_service_init(&service, &state) == AGENT_OK);

    assert(smart_home_device_service_add_room(&service, "kitchen") == AGENT_OK);
    assert(smart_home_room_count(&state) == 3);
    assert(smart_home_room_index(&state, "kitchen") == 2);
    assert(smart_home_device_service_revision(&service) == 1u);

    device = smart_home_device_get_by_slot(&state, 0);
    assert(device != NULL);
    assert(smart_home_device_service_update_meta(&service, device->id,
                                                  "kitchen", device->name)
           == AGENT_OK);
    assert(strcmp(device->room, "kitchen") == 0);
    assert(smart_home_device_service_revision(&service) == 2u);
    smart_home_device_service_deinit(&service);
}

static void test_miloco_scene_skill_omits_virtual_catalog(void)
{
    FILE *file;
    long size;
    char *text;
    smart_home_scene_catalog_t catalog;

    file = fopen(SMART_HOME_SCENES_SKILL_PATH, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    size = ftell(file);
    assert(size > 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    text = calloc(1u, (size_t)size + 1u);
    assert(text != NULL);
    assert(fread(text, 1u, (size_t)size, file) == (size_t)size);
    fclose(file);

    assert(smart_home_scene_catalog_load(text, &catalog) == AGENT_ERROR_NOT_FOUND);
    assert(catalog.version == 0u && catalog.scene_count == 0u);
    free(text);
}

int main(void)
{
    test_single_light_default_and_legacy_migration();
    test_state_mutation_emits_revisioned_event();
    test_scene_catalog_drives_one_logical_revision();
    test_room_catalog_allows_device_reassignment();
    test_miloco_scene_skill_omits_virtual_catalog();
    printf("all device_service tests passed\n");
    return 0;
}

/* device.c 引用了 cAGENT 的 context provider 注册，测试提供 stub。 */
agent_error_t agent_register_context(agent_t *agent,
                                    const agent_context_provider_t *provider)
{
    (void)agent;
    (void)provider;
    return AGENT_OK;
}
