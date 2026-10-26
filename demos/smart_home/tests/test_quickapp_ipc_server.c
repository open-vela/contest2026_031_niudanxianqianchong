/* SPDX-License-Identifier: Apache-2.0 */

#include <agent.h>
#include <agent/context.h>
#include <smart_home_device.h>
#include <smart_home_device_service.h>
#include <smart_home_quickapp_ipc.h>
#include <smart_home_quickapp_ipc_server.h>
#include <smart_home_quickapp_provider.h>

#include <assert.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    strncpy(destination, source, destination_size - 1u);
    destination[destination_size - 1u] = '\0';
}

static void receive_response(mqd_t queue,
                             struct smart_home_quickapp_ipc_response_s *response)
{
    struct timespec deadline;

    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_sec += 2;
    assert(mq_timedreceive(queue, (char *)response, sizeof(*response), NULL,
                           &deadline) == (ssize_t)sizeof(*response));
}

static void prepare_request(struct smart_home_quickapp_ipc_request_s *request,
                            uint16_t operation, const char *request_id)
{
    memset(request, 0, sizeof(*request));
    request->magic = SMART_HOME_QUICKAPP_IPC_MAGIC;
    request->version = SMART_HOME_QUICKAPP_IPC_VERSION;
    request->operation = operation;
    copy_text(request->request_id, sizeof(request->request_id), request_id);
}

static void test_ipc_server_processes_real_provider_requests(void)
{
    struct mq_attr request_attr = {
        .mq_maxmsg = 4,
        .mq_msgsize = sizeof(struct smart_home_quickapp_ipc_request_s),
    };
    struct mq_attr response_attr = {
        .mq_maxmsg = 4,
        .mq_msgsize = sizeof(struct smart_home_quickapp_ipc_response_s),
    };
    smart_home_state_t state;
    smart_home_device_service_t service;
    smart_home_quickapp_provider_t provider;
    smart_home_quickapp_ipc_server_t server;
    struct smart_home_quickapp_ipc_request_s request;
    struct smart_home_quickapp_ipc_response_s response;
    mqd_t request_queue;
    mqd_t response_queue;

    mq_unlink(SMART_HOME_QUICKAPP_IPC_REQUEST_QUEUE);
    mq_unlink(SMART_HOME_QUICKAPP_IPC_RESPONSE_QUEUE);
    smart_home_device_init(&state);
    assert(smart_home_device_service_init(&service, &state) == AGENT_OK);
    assert(smart_home_quickapp_provider_init(&provider, &service) == AGENT_OK);
    assert(smart_home_quickapp_ipc_server_init(&server, &provider) == AGENT_OK);

    response_queue = mq_open(SMART_HOME_QUICKAPP_IPC_RESPONSE_QUEUE,
                             O_RDONLY | O_CREAT, 0660, &response_attr);
    assert(response_queue != (mqd_t)-1);
    request_queue = mq_open(SMART_HOME_QUICKAPP_IPC_REQUEST_QUEUE,
                            O_WRONLY, 0660, &request_attr);
    assert(request_queue != (mqd_t)-1);

    prepare_request(&request, SMART_HOME_QUICKAPP_IPC_GET_CAPABILITIES,
                    "capabilities-1");
    assert(mq_send(request_queue, (const char *)&request, sizeof(request), 0) == 0);
    receive_response(response_queue, &response);
    assert(response.status == 0);
    assert(response.api_version == SMART_HOME_QUICKAPP_API_VERSION);
    assert(strcmp(response.device_id, SMART_HOME_QUICKAPP_DEVICE_ID) == 0);

    prepare_request(&request, SMART_HOME_QUICKAPP_IPC_GET_SNAPSHOT,
                    "snapshot-1");
    copy_text(request.device_id, sizeof(request.device_id),
              SMART_HOME_QUICKAPP_DEVICE_ID);
    assert(mq_send(request_queue, (const char *)&request, sizeof(request), 0) == 0);
    receive_response(response_queue, &response);
    assert(response.status == 0);
    assert(response.revision == 1u);
    assert(response.power == 0u);

    prepare_request(&request, SMART_HOME_QUICKAPP_IPC_CONTROL_DEVICE,
                    "control-1");
    copy_text(request.device_id, sizeof(request.device_id),
              SMART_HOME_QUICKAPP_DEVICE_ID);
    copy_text(request.command, sizeof(request.command),
              SMART_HOME_QUICKAPP_COMMAND_SET_POWER);
    request.value = 1u;
    request.expected_revision = 1u;
    assert(mq_send(request_queue, (const char *)&request, sizeof(request), 0) == 0);
    receive_response(response_queue, &response);
    assert(response.status == 0);
    assert(response.power == 1u);
    assert(response.revision == 2u);

    prepare_request(&request, SMART_HOME_QUICKAPP_IPC_CONTROL_DEVICE,
                    "control-2");
    copy_text(request.device_id, sizeof(request.device_id),
              SMART_HOME_QUICKAPP_DEVICE_ID);
    copy_text(request.command, sizeof(request.command),
              SMART_HOME_QUICKAPP_COMMAND_SET_POWER);
    request.expected_revision = 1u;
    assert(mq_send(request_queue, (const char *)&request, sizeof(request), 0) == 0);
    receive_response(response_queue, &response);
    assert(response.status == SMART_HOME_QUICKAPP_IPC_STATUS_STALE);
    assert(response.revision == 2u);

    mq_close(request_queue);
    mq_close(response_queue);
    smart_home_quickapp_ipc_server_deinit(&server);
    smart_home_quickapp_provider_deinit(&provider);
    smart_home_device_service_deinit(&service);
}

int main(void)
{
    test_ipc_server_processes_real_provider_requests();
    printf("all quickapp IPC server tests passed\n");
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
