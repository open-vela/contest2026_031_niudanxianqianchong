#include "agent/smart_home_session.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_sync, fail_write, writes;
static size_t live, seen;
static char newest[8192];
static const char *file = SMART_HOME_SESSION_DIR "/" SMART_HOME_SESSION_FILE;
void *smart_home_bulk_alloc(size_t size) { void *p = malloc(size); if (p) ++live; return p; }
void smart_home_bulk_free(void *p) { if (p) { assert(live); --live; free(p); } }
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_sync) { fail_sync = 0; errno = EIO; return -1; }
    return __real_fsync(fd);
}
ssize_t __real_write(int fd, const void *data, size_t size);
ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    if (fail_write) {
        if (++writes == 1) return __real_write(fd, data, size > 7u ? 7u : size);
        errno = ENOSPC;
        return -1;
    }
    return __real_write(fd, data, size);
}
static agent_string_view_t text(const char *s) { return agent_string_view(s, strlen(s)); }
static agent_error_t collect(void *context, const agent_session_group_view_t *group)
{
    (void)context;
    assert(group->message_count == 2u);
    assert(group->messages[0].role == AGENT_MESSAGE_ROLE_USER);
    assert(group->messages[1].role == AGENT_MESSAGE_ROLE_ASSISTANT);
    if (!seen) {
        assert(group->messages[1].content.size < sizeof(newest));
        memcpy(newest, group->messages[1].content.data, group->messages[1].content.size);
        newest[group->messages[1].content.size] = '\0';
    }
    ++seen;
    return AGENT_OK;
}
static int save(agent_session_storage_t *storage, const char *answer, int complete)
{
    void *transaction;
    agent_message_view_t user = {.role = AGENT_MESSAGE_ROLE_USER, .content = text("中文\n\"question\"\\")};
    agent_message_view_t assistant = {.role = AGENT_MESSAGE_ROLE_ASSISTANT, .content = text(answer)};
    int ret = storage->ops.begin(storage->context, text("home"), &transaction);
    if (ret) return ret;
    assert(!storage->ops.append(storage->context, transaction, &user));
    assert(!storage->ops.append(storage->context, transaction, &assistant));
    return storage->ops.finish(storage->context, transaction,
                               complete ? AGENT_SESSION_TURN_COMPLETE : AGENT_SESSION_TURN_ABORTED);
}
static off_t bytes(void) { struct stat st; assert(!stat(file, &st)); return st.st_size; }
static void add_tail(const char *value)
{
    FILE *f = fopen(file, "ab"); assert(f);
    assert(fwrite(value, 1, strlen(value), f) == strlen(value)); assert(!fclose(f));
}
int main(void)
{
    smart_home_session_t *session;
    agent_session_storage_t storage;
    char answer[4096];
    size_t count;
    assert(!smart_home_session_open(&session, &storage));
    assert(!save(&storage, "第一轮\nhello", 1));
    off_t original = bytes();
    assert(!save(&storage, "aborted", 0) && bytes() == original);
    smart_home_session_close(&session);
    assert(!live);
    assert(!smart_home_session_open(&session, &storage));
    seen = 0;
    assert(!smart_home_session_visit(session, collect, NULL) && seen == 1 && !strcmp(newest, "第一轮\nhello"));
    add_tail("{\"v\":1,\"sid\":");
    seen = 0;
    assert(!smart_home_session_visit(session, collect, NULL) && seen == 1);
    assert(!smart_home_session_prepare(session) && bytes() == original);
    {
        char oversized[8192];
        void *transaction;
        memset(oversized, '\n', sizeof(oversized));
        agent_message_view_t message = {
            .role = AGENT_MESSAGE_ROLE_ASSISTANT,
            .content = agent_string_view(oversized, sizeof(oversized))
        };
        assert(!storage.ops.begin(storage.context, text("home"), &transaction));
        assert(storage.ops.append(storage.context, transaction, &message) == AGENT_ERROR_CAPACITY);
        assert(!storage.ops.finish(storage.context, transaction, AGENT_SESSION_TURN_ABORTED));
        assert(bytes() == original);
    }
    fail_write = 1; writes = 0;
    assert(save(&storage, "partial write", 1) == AGENT_ERROR_IO);
    fail_write = 0;
    assert(bytes() == original);
    fail_sync = 1;
    assert(save(&storage, "sync uncertain", 1) == AGENT_ERROR_IO);
    seen = 0;
    assert(!smart_home_session_visit(session, collect, NULL) && seen == 2);
    assert(!storage.ops.clear_all(storage.context));
    memset(answer, '\n', sizeof(answer) - 1u); answer[sizeof(answer) - 1u] = '\0';
    for (int i = 0; i < 27; ++i) {
        assert(!smart_home_session_prepare(session));
        assert(!save(&storage, answer, 1));
        assert(bytes() <= CONFIG_SMART_HOME_SESSION_FILE_BYTES);
    }
    seen = 0;
    assert(!smart_home_session_visit(session, collect, NULL));
    assert(seen > 0 && seen <= CONFIG_SMART_HOME_SESSION_RETAIN_TURNS);
    assert(!strcmp(newest, answer));
    add_tail("broken\n");
    original = bytes(); seen = 0;
    assert(smart_home_session_visit(session, collect, NULL) == AGENT_ERROR_PARSE);
    assert(bytes() == original);
    assert(!storage.ops.clear_all(storage.context));
    smart_home_session_close(&session);
    assert(!live);
    assert(!smart_home_session_open(&session, &storage));
    seen = 0;
    assert(!smart_home_session_visit(session, collect, NULL) && seen == 0);
    assert(!storage.ops.count(storage.context, &count) && count == 0);
    smart_home_session_close(&session);
    assert(!live);
    puts("PASS: real file restart, escaping, bounded rotation, aborted exclusion, tail repair, short-write rollback, sync ambiguity, corruption and durable clear");
    return 0;
}
