#include <nuttx/config.h>
#include "smart_home_session.h"

#ifdef CONFIG_SMART_HOME_SESSION_PERSIST
#include "../config/smart_home_config.h"
#include "../smart_home_memory.h"
#include <agent_openvela_file_store.h>
#include <agent_session_jsonl_files.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

struct smart_home_session {
    agent_session_jsonl_t jsonl;
    agent_openvela_file_store_t port;
    agent_file_store_t store;
    agent_session_jsonl_files_t files;
    char paths[512];
    char name[32];
    char id[8];
    char write_line[CONFIG_SMART_HOME_SESSION_LINE_BYTES];
    char read_line[CONFIG_SMART_HOME_SESSION_LINE_BYTES];
    char decoded[CONFIG_SMART_HOME_SESSION_PAYLOAD_BYTES];
    int tokens[16384];
    agent_message_view_t messages[AGENT_MAX_PROJECTED_MESSAGES];
    agent_tool_call_view_t calls[AGENT_MAX_PROJECTED_MESSAGES];
};

static agent_string_view_t session_id(void)
{
    return agent_string_view(SMART_HOME_SESSION_ID, sizeof(SMART_HOME_SESSION_ID) - 1u);
}

static int make_directory(const char *path)
{
    struct stat info;
    if (mkdir(path, 0700) == 0) return AGENT_OK;
    if (errno == EEXIST && stat(path, &info) == 0 && S_ISDIR(info.st_mode)) return AGENT_OK;
    return AGENT_ERROR_IO;
}

static agent_error_t finish_complete_only(void *context, void *transaction,
                                          agent_session_turn_outcome_t outcome)
{
    agent_session_jsonl_t *jsonl = context;
    agent_session_storage_t storage;
    if (!jsonl || transaction != jsonl || !jsonl->active) return AGENT_ERROR_INVALID;
    if (outcome == AGENT_SESSION_TURN_ABORTED) {
        jsonl->active = false;
        return AGENT_OK;
    }
    agent_session_jsonl_bind(jsonl, &storage);
    return storage.ops.finish(context, transaction, outcome);
}

int smart_home_session_open(smart_home_session_t **out, agent_session_storage_t *storage)
{
    smart_home_session_t *session;
    agent_session_jsonl_config_t jsonl = {0};
    int ret;
    if (!out || !storage) return AGENT_ERROR_INVALID;
    *out = NULL;
    if (CONFIG_SMART_HOME_SESSION_RETAIN_TURNS < 2u ||
        CONFIG_SMART_HOME_SESSION_FILE_BYTES <= CONFIG_SMART_HOME_SESSION_LINE_BYTES)
        return AGENT_ERROR_INVALID;
    ret = make_directory(CONFIG_SMART_HOME_DEMO_DATA_ROOT "/smart_home");
    if (ret == AGENT_OK) ret = make_directory(SMART_HOME_SESSION_DIR);
    if (ret != AGENT_OK) return ret;
    session = smart_home_bulk_alloc(sizeof(*session));
    if (!session) return AGENT_ERROR_NOMEM;
    memset(session, 0, sizeof(*session));
    {
        const agent_openvela_file_store_config_t config = {
            SMART_HOME_SESSION_DIR, session->paths, sizeof(session->paths), false, false
        };
        ret = agent_port_openvela_file_store_init(&session->port, &config, &session->store);
    }
    if (ret != AGENT_OK) goto fail;
    ret = agent_session_jsonl_files_init(&session->files, &session->store,
                                        session->name, sizeof(session->name), &jsonl);
    if (ret != AGENT_OK) goto fail;
    jsonl.write_line = session->write_line;
    jsonl.write_line_capacity = sizeof(session->write_line);
    jsonl.read_line = session->read_line;
    jsonl.read_line_capacity = sizeof(session->read_line);
    jsonl.decoded = session->decoded;
    jsonl.decoded_capacity = sizeof(session->decoded);
    jsonl.token_buffer = session->tokens;
    jsonl.token_buffer_bytes = sizeof(session->tokens);
    jsonl.read_messages = session->messages;
    jsonl.read_message_capacity = AGENT_MAX_PROJECTED_MESSAGES;
    jsonl.read_calls = session->calls;
    jsonl.read_call_capacity = AGENT_MAX_PROJECTED_MESSAGES;
    jsonl.session_id = session->id;
    jsonl.session_id_capacity = sizeof(session->id);
    ret = agent_session_jsonl_init(&session->jsonl, &jsonl);
    if (ret == AGENT_OK) ret = agent_session_jsonl_bind(&session->jsonl, storage);
    if (ret != AGENT_OK) goto fail;
    storage->ops.finish = finish_complete_only;
    *out = session;
    return AGENT_OK;
fail:
    smart_home_bulk_free(session);
    return ret;
}

void smart_home_session_close(smart_home_session_t **session)
{
    if (session && *session) {
        memset(*session, 0, sizeof(**session));
        smart_home_bulk_free(*session);
        *session = NULL;
    }
}

int smart_home_session_prepare(smart_home_session_t *session)
{
    const agent_string_view_t name = agent_string_view(SMART_HOME_SESSION_FILE,
                                                       sizeof(SMART_HOME_SESSION_FILE) - 1u);
    uint64_t bytes;
    size_t end, start, count = 0u;
    char *data;
    bool published;
    int ret;
    if (!session) return AGENT_ERROR_INVALID;
    if (session->jsonl.active) return AGENT_ERROR_BUSY;
    ret = agent_file_size(&session->store, name, &bytes);
    if (ret == AGENT_ERROR_NOT_FOUND) return AGENT_OK;
    if (ret != AGENT_OK || !bytes) return ret;
    if (bytes > CONFIG_SMART_HOME_SESSION_FILE_BYTES) return AGENT_ERROR_CAPACITY;
    data = smart_home_bulk_alloc((size_t)bytes);
    if (!data) return AGENT_ERROR_NOMEM;
    ret = agent_file_read_exact(&session->store, name, 0u, data, (size_t)bytes);
    if (ret != AGENT_OK) goto done;
    end = (size_t)bytes;
    while (end && data[end - 1u] != '\n') --end;
    start = end;
    while (start && count < CONFIG_SMART_HOME_SESSION_RETAIN_TURNS - 1u) {
        size_t candidate = start - 1u;
        while (candidate && data[candidate - 1u] != '\n') --candidate;
        if (end - candidate > CONFIG_SMART_HOME_SESSION_FILE_BYTES -
                              CONFIG_SMART_HOME_SESSION_LINE_BYTES) break;
        start = candidate;
        ++count;
    }
    if (start || end != bytes)
        ret = agent_file_replace(&session->store, name, data + start, end - start, &published);
done:
    smart_home_bulk_free(data);
    return ret;
}

int smart_home_session_visit(smart_home_session_t *session,
                             agent_session_visit_fn visit, void *context)
{
    agent_session_storage_t storage;
    uint64_t bytes;
    int ret;
    if (!session || !visit) return AGENT_ERROR_INVALID;
    if (session->jsonl.active) return AGENT_ERROR_BUSY;
    ret = agent_file_size(&session->store,
                         agent_string_view(SMART_HOME_SESSION_FILE,
                                           sizeof(SMART_HOME_SESSION_FILE) - 1u), &bytes);
    if (ret == AGENT_ERROR_NOT_FOUND) return AGENT_OK;
    if (ret != AGENT_OK) return ret;
    if (bytes > CONFIG_SMART_HOME_SESSION_FILE_BYTES) return AGENT_ERROR_CAPACITY;
    agent_session_jsonl_bind(&session->jsonl, &storage);
    return storage.ops.recent(storage.context, session_id(),
                              CONFIG_SMART_HOME_SESSION_RETAIN_TURNS, visit, context);
}
#endif
