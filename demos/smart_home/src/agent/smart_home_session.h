#pragma once

#include <agent/session.h>

#ifndef CONFIG_SMART_HOME_DEMO_DATA_ROOT
#define CONFIG_SMART_HOME_DEMO_DATA_ROOT "/data"
#endif
#ifndef CONFIG_SMART_HOME_SESSION_FILE_BYTES
#define CONFIG_SMART_HOME_SESSION_FILE_BYTES 1048576u
#endif
#ifndef CONFIG_SMART_HOME_SESSION_LINE_BYTES
#define CONFIG_SMART_HOME_SESSION_LINE_BYTES 131072u
#endif
#ifndef CONFIG_SMART_HOME_SESSION_RETAIN_TURNS
#define CONFIG_SMART_HOME_SESSION_RETAIN_TURNS 20u
#endif
#ifndef CONFIG_SMART_HOME_SESSION_PAYLOAD_BYTES
#define CONFIG_SMART_HOME_SESSION_PAYLOAD_BYTES 65536u
#endif

#define SMART_HOME_SESSION_DIR CONFIG_SMART_HOME_DEMO_DATA_ROOT "/smart_home/sessions"
#define SMART_HOME_SESSION_FILE "session-686f6d65.jsonl"

typedef struct smart_home_session smart_home_session_t;

int smart_home_session_open(smart_home_session_t **out, agent_session_storage_t *storage);
void smart_home_session_close(smart_home_session_t **session);
int smart_home_session_prepare(smart_home_session_t *session);
/* Borrowed complete groups, newest first; no reentry or retained pointers. */
int smart_home_session_visit(smart_home_session_t *session,
                             agent_session_visit_fn visit, void *context);
