/* SPDX-License-Identifier: Apache-2.0 */

#include <agent.h>
#include <agent/context.h>
#include <smart_home_device.h>
#include <smart_home_device_service.h>
#include <smart_home_quickapp_provider.h>

#include <assert.h>
#include <stdio.h>

static void test_provider_exposes_the_p0_fixture(void)
{
    smart_home_state_t state;
    smart_home_device_service_t service;
    smart_home_quickapp_provider_t provider;
    smart_home_quickapp_snapshot_t snapshot;

    smart_home_device_init(&state);
    assert(smart_home_device_service_init(&service, &state) == AGENT_OK);
    assert(smart_home_quickapp_provider_init(&provider, &service) == AGENT_OK);
    assert(smart_home_quickapp_provider_get_snapshot(&provider, &snapshot) == AGENT_OK);
    assert(snapshot.revision == 1u);
    assert(snapshot.online == 1);
    assert(snapshot.power == 0);
    assert(snapshot.brightness == 0);

    smart_home_quickapp_provider_deinit(&provider);
    smart_home_device_service_deinit(&service);
}

static void test_control_is_revisioned_and_idempotent(void)
{
    smart_home_state_t state;
    smart_home_device_service_t service;
    smart_home_quickapp_provider_t provider;
    smart_home_quickapp_snapshot_t before;
    smart_home_quickapp_snapshot_t after;
    smart_home_quickapp_snapshot_t duplicate;

    smart_home_device_init(&state);
    assert(smart_home_device_service_init(&service, &state) == AGENT_OK);
    assert(smart_home_quickapp_provider_init(&provider, &service) == AGENT_OK);
    assert(smart_home_quickapp_provider_get_snapshot(&provider, &before) == AGENT_OK);

    assert(smart_home_quickapp_provider_control(
               &provider, "request-1", SMART_HOME_QUICKAPP_DEVICE_ID,
               SMART_HOME_QUICKAPP_COMMAND_SET_POWER, 1, before.revision,
               &after) == AGENT_OK);
    assert(after.revision == before.revision + 1u);
    assert(after.power == 1);
    assert(after.brightness == SMART_HOME_QUICKAPP_DEFAULT_BRIGHTNESS);

    assert(smart_home_quickapp_provider_control(
               &provider, "request-1", SMART_HOME_QUICKAPP_DEVICE_ID,
               SMART_HOME_QUICKAPP_COMMAND_SET_POWER, 0, before.revision,
               &duplicate) == AGENT_OK);
    assert(duplicate.revision == after.revision);
    assert(duplicate.power == 1);
    assert(smart_home_device_service_revision(&service) == after.revision);

    assert(smart_home_quickapp_provider_control(
               &provider, "request-2", SMART_HOME_QUICKAPP_DEVICE_ID,
               SMART_HOME_QUICKAPP_COMMAND_SET_POWER, 0, before.revision,
               &duplicate) == SMART_HOME_DEVICE_SERVICE_ERROR_STALE_REVISION);
    assert(duplicate.revision == after.revision);
    assert(duplicate.power == 1);

    smart_home_quickapp_provider_deinit(&provider);
    smart_home_device_service_deinit(&service);
}

static void test_control_rejects_an_unknown_public_contract(void)
{
    smart_home_state_t state;
    smart_home_device_service_t service;
    smart_home_quickapp_provider_t provider;
    smart_home_quickapp_snapshot_t snapshot;

    smart_home_device_init(&state);
    assert(smart_home_device_service_init(&service, &state) == AGENT_OK);
    assert(smart_home_quickapp_provider_init(&provider, &service) == AGENT_OK);

    assert(smart_home_quickapp_provider_control(
               &provider, "request-3", "unknown-device",
               SMART_HOME_QUICKAPP_COMMAND_SET_POWER, 1, 1u,
               &snapshot) == AGENT_ERROR_INVALID);
    assert(smart_home_quickapp_provider_control(
               &provider, "request-4", SMART_HOME_QUICKAPP_DEVICE_ID,
               "setBrightness", 1, 1u, &snapshot) == AGENT_ERROR_INVALID);

    smart_home_quickapp_provider_deinit(&provider);
    smart_home_device_service_deinit(&service);
}

int main(void)
{
    test_provider_exposes_the_p0_fixture();
    test_control_is_revisioned_and_idempotent();
    test_control_rejects_an_unknown_public_contract();
    printf("all quickapp provider tests passed\n");
    return 0;
}

/* device.c registers a cAGENT context provider; host tests only need a stub. */
agent_error_t agent_register_context(agent_t *agent,
                                    const agent_context_provider_t *provider)
{
    (void)agent;
    (void)provider;
    return AGENT_OK;
}
