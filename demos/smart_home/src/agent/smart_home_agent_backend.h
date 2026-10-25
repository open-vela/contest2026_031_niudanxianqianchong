#pragma once

#include <agent.h>
#include <stdint.h>

#ifndef CONFIG_SMART_HOME_MODEL_REQUEST_BUFFER_SIZE
#define CONFIG_SMART_HOME_MODEL_REQUEST_BUFFER_SIZE 16384u
#endif
#ifndef CONFIG_SMART_HOME_MODEL_RESPONSE_BUFFER_SIZE
#define CONFIG_SMART_HOME_MODEL_RESPONSE_BUFFER_SIZE 16384u
#endif
#ifndef CONFIG_SMART_HOME_MODEL_DECODED_BUFFER_SIZE
#define CONFIG_SMART_HOME_MODEL_DECODED_BUFFER_SIZE 8192u
#endif
#ifndef CONFIG_SMART_HOME_MODEL_JSON_TOKENS
#define CONFIG_SMART_HOME_MODEL_JSON_TOKENS 512u
#endif

#ifndef CONFIG_SMART_HOME_SESSION_HISTORY_TURNS
#define CONFIG_SMART_HOME_SESSION_HISTORY_TURNS 3u
#endif
#ifndef CONFIG_SMART_HOME_SESSION_PAYLOAD_BYTES
#define CONFIG_SMART_HOME_SESSION_PAYLOAD_BYTES 65536u
#endif

typedef struct {
    char backend_id[32];
    char host[128];
    char path[128];
    char port[8];
    char api_key[256];
    char model[64];
    int timeout_ms;
    uint32_t request_buffer_size;
    uint32_t response_buffer_size;
    uint32_t max_output_tokens;
} smart_home_model_config_t;

typedef struct smart_home_agent_backend smart_home_agent_backend_t;

/* Creates a CONFIGURING Agent; register contributions before agent_start(). */
int smart_home_agent_backend_create(smart_home_agent_backend_t **out,
                                    agent_t **agent,
                                    const char *system_prompt,
                                    const smart_home_model_config_t *config);
/* Caller serializes configuration, agent_run and destruction. */
int smart_home_agent_backend_apply(smart_home_agent_backend_t *backend,
                                   const smart_home_model_config_t *config);
void smart_home_agent_backend_destroy(smart_home_agent_backend_t **backend);

int smart_home_agent_backend_prepare_turn(smart_home_agent_backend_t *backend);
