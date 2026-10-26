#include "smart_home_quickapp_ipc_server.h"

#include <smart_home_quickapp_ipc.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>

#define SMART_HOME_QUICKAPP_IPC_MAX_MESSAGES 4

static void copy_text(char *destination, size_t destination_size,
                      const char *source)
{
    if (!destination || destination_size == 0u) {
        return;
    }

    if (!source) {
        destination[0] = '\0';
        return;
    }

    strncpy(destination, source, destination_size - 1u);
    destination[destination_size - 1u] = '\0';
}

static int provider_status_to_ipc_status(int status)
{
    if (status == AGENT_OK) {
        return SMART_HOME_QUICKAPP_IPC_STATUS_OK;
    }

    if (status == SMART_HOME_DEVICE_SERVICE_ERROR_STALE_REVISION) {
        return SMART_HOME_QUICKAPP_IPC_STATUS_STALE;
    }

    if (status == AGENT_ERROR_INVALID || status == AGENT_ERROR_NOT_FOUND) {
        return SMART_HOME_QUICKAPP_IPC_STATUS_INVALID;
    }

    return SMART_HOME_QUICKAPP_IPC_STATUS_UNAVAILABLE;
}

static void response_from_snapshot(
    struct smart_home_quickapp_ipc_response_s *response,
    const smart_home_quickapp_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }

    copy_text(response->device_id, sizeof(response->device_id),
              SMART_HOME_QUICKAPP_DEVICE_ID);
    response->revision = snapshot->revision;
    response->online = snapshot->online != 0;
    response->power = snapshot->power != 0;
    response->brightness = (uint8_t)snapshot->brightness;
}

static void handle_request(smart_home_quickapp_ipc_server_t *server,
                           const struct smart_home_quickapp_ipc_request_s *request,
                           struct smart_home_quickapp_ipc_response_s *response)
{
    smart_home_quickapp_snapshot_t snapshot;
    int ret;

    memset(response, 0, sizeof(*response));
    response->magic = SMART_HOME_QUICKAPP_IPC_MAGIC;
    response->version = SMART_HOME_QUICKAPP_IPC_VERSION;
    response->operation = request->operation;
    copy_text(response->request_id, sizeof(response->request_id),
              request->request_id);

    if (request->magic != SMART_HOME_QUICKAPP_IPC_MAGIC ||
        request->version != SMART_HOME_QUICKAPP_IPC_VERSION ||
        request->request_id[0] == '\0') {
        response->status = SMART_HOME_QUICKAPP_IPC_STATUS_INVALID;
        return;
    }

    if (request->operation == SMART_HOME_QUICKAPP_IPC_GET_CAPABILITIES) {
        response->status = SMART_HOME_QUICKAPP_IPC_STATUS_OK;
        response->api_version = SMART_HOME_QUICKAPP_API_VERSION;
        copy_text(response->device_id, sizeof(response->device_id),
                  SMART_HOME_QUICKAPP_DEVICE_ID);
        copy_text(response->type, sizeof(response->type), "light");
        copy_text(response->command, sizeof(response->command),
                  SMART_HOME_QUICKAPP_COMMAND_SET_POWER);
        return;
    }

    if (request->operation == SMART_HOME_QUICKAPP_IPC_GET_SNAPSHOT) {
        if (strcmp(request->device_id, SMART_HOME_QUICKAPP_DEVICE_ID) != 0) {
            response->status = SMART_HOME_QUICKAPP_IPC_STATUS_INVALID;
            return;
        }

        ret = smart_home_quickapp_provider_get_snapshot(server->provider,
                                                         &snapshot);
        response->status = provider_status_to_ipc_status(ret);
        if (ret == AGENT_OK) {
            response_from_snapshot(response, &snapshot);
        }
        return;
    }

    if (request->operation == SMART_HOME_QUICKAPP_IPC_CONTROL_DEVICE) {
        ret = smart_home_quickapp_provider_control(
            server->provider, request->request_id, request->device_id,
            request->command, request->value ? 1 : 0,
            request->expected_revision, &snapshot);
        response->status = provider_status_to_ipc_status(ret);
        response_from_snapshot(response, &snapshot);
        return;
    }

    response->status = SMART_HOME_QUICKAPP_IPC_STATUS_INVALID;
}

static void send_response(const struct smart_home_quickapp_ipc_response_s *response)
{
    struct mq_attr attr = {
        .mq_maxmsg = SMART_HOME_QUICKAPP_IPC_MAX_MESSAGES,
        .mq_msgsize = sizeof(*response),
    };
    mqd_t response_queue;

    response_queue = mq_open(SMART_HOME_QUICKAPP_IPC_RESPONSE_QUEUE,
                             O_WRONLY | O_NONBLOCK | O_CREAT, 0660, &attr);
    if (response_queue == (mqd_t)-1) {
        syslog(LOG_WARNING, "smart_home: QuickApp response queue unavailable: %d\n",
               errno);
        return;
    }

    if (mq_send(response_queue, (const char *)response, sizeof(*response), 0) < 0) {
        syslog(LOG_WARNING, "smart_home: QuickApp response send failed: %d\n",
               errno);
    }
    mq_close(response_queue);
}

static void *ipc_server_worker(void *argument)
{
    smart_home_quickapp_ipc_server_t *server = argument;
    struct smart_home_quickapp_ipc_request_s request;
    struct smart_home_quickapp_ipc_response_s response;
    ssize_t received;

    while (server->running) {
        received = mq_receive(server->request_queue, (char *)&request,
                              sizeof(request), NULL);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (server->running) {
                syslog(LOG_WARNING, "smart_home: QuickApp request receive failed: %d\n",
                       errno);
            }
            break;
        }

        if ((size_t)received != sizeof(request)) {
            syslog(LOG_WARNING, "smart_home: malformed QuickApp request\n");
            continue;
        }

        if (request.operation == SMART_HOME_QUICKAPP_IPC_STOP_SERVER) {
            break;
        }

        handle_request(server, &request, &response);
        send_response(&response);
    }

    return NULL;
}

int smart_home_quickapp_ipc_server_init(
    smart_home_quickapp_ipc_server_t *server,
    smart_home_quickapp_provider_t *provider)
{
    struct mq_attr attr = {
        .mq_maxmsg = SMART_HOME_QUICKAPP_IPC_MAX_MESSAGES,
        .mq_msgsize = sizeof(struct smart_home_quickapp_ipc_request_s),
    };

    if (!server || !provider) {
        return AGENT_ERROR_INVALID;
    }

    memset(server, 0, sizeof(*server));
    server->request_queue = (mqd_t)-1;
    server->request_queue = mq_open(SMART_HOME_QUICKAPP_IPC_REQUEST_QUEUE,
                                    O_RDONLY | O_CREAT, 0660, &attr);
    if (server->request_queue == (mqd_t)-1) {
        syslog(LOG_ERR, "smart_home: QuickApp request queue open failed: %d\n",
               errno);
        return AGENT_ERROR;
    }

    server->provider = provider;
    server->running = 1;
    if (pthread_create(&server->worker, NULL, ipc_server_worker, server) != 0) {
        server->running = 0;
        mq_close(server->request_queue);
        memset(server, 0, sizeof(*server));
        server->request_queue = (mqd_t)-1;
        return AGENT_ERROR;
    }

    server->worker_started = 1;
    return AGENT_OK;
}

void smart_home_quickapp_ipc_server_deinit(
    smart_home_quickapp_ipc_server_t *server)
{
    struct smart_home_quickapp_ipc_request_s request;
    mqd_t writer;

    if (!server) {
        return;
    }

    server->running = 0;
    if (server->worker_started) {
        memset(&request, 0, sizeof(request));
        request.magic = SMART_HOME_QUICKAPP_IPC_MAGIC;
        request.version = SMART_HOME_QUICKAPP_IPC_VERSION;
        request.operation = SMART_HOME_QUICKAPP_IPC_STOP_SERVER;
        writer = mq_open(SMART_HOME_QUICKAPP_IPC_REQUEST_QUEUE,
                         O_WRONLY | O_NONBLOCK);
        if (writer != (mqd_t)-1) {
            (void)mq_send(writer, (const char *)&request, sizeof(request), 0);
            mq_close(writer);
        }
        pthread_join(server->worker, NULL);
    }

    if (server->request_queue != (mqd_t)-1) {
        mq_close(server->request_queue);
    }
    mq_unlink(SMART_HOME_QUICKAPP_IPC_REQUEST_QUEUE);
    mq_unlink(SMART_HOME_QUICKAPP_IPC_RESPONSE_QUEUE);
    memset(server, 0, sizeof(*server));
}
