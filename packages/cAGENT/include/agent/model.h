/*
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026 tanglinjie
 */
/* Synchronous model provider contract and explicit ownership binding. */
#pragma once

#include <agent/config.h>
#include <agent/error.h>
#include <agent/runtime.h>
#include <agent/tool.h>
#include <agent/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque model wrapper, independent of the Agent workspace. */
typedef struct agent_model agent_model_t;

/* Fixed caller-storage for a model wrapper; provider state is separate. */
typedef union {
    void* align_pointer;                                    /* Pointer alignment. */
    uint64_t align_u64;                                     /* 64-bit scalar alignment. */
    long double align_long_double;                           /* Conservative scalar alignment. */
    unsigned char bytes[AGENT_MODEL_WORKSPACE_BYTES];       /* Private wrapper storage. */
} agent_model_workspace_t;

/* Normalized transcript roles, independent of a vendor wire format. */
typedef enum {
    AGENT_MESSAGE_ROLE_SYSTEM = 1, /* System instructions. */
    AGENT_MESSAGE_ROLE_USER,       /* User input. */
    AGENT_MESSAGE_ROLE_ASSISTANT,  /* Model text and/or tool calls. */
    AGENT_MESSAGE_ROLE_TOOL        /* Result paired with a tool call. */
} agent_message_role_t;

/* Transcript item borrowed for one Model completion call. */
typedef struct {
    agent_message_role_t role;                /* Message role. */
    agent_string_view_t content;              /* Text, possibly empty. */
    agent_string_view_t tool_call_id;         /* TOOL only: matching call identifier. */
    const agent_tool_call_view_t* tool_calls; /* ASSISTANT only; NULL if count=0. */
    size_t tool_call_count;                   /* Number of calls. */
} agent_message_view_t;

/* Model-visible projection of a registered Tool. */
typedef struct {
    agent_string_view_t name;              /* Unique registered name. */
    agent_string_view_t description;       /* Human/model-readable description. */
    agent_string_view_t input_schema_json; /* Complete JSON Schema object. */
    uint32_t flags;                        /* agent_tool_flags_t bitmask, not authorization. */
} agent_tool_view_t;

/* Canonical model input; all views expire when complete returns. */
typedef struct {
    agent_string_view_t system_prompt;    /* Complete bounded context projection. */
    const agent_message_view_t* messages; /* Ordered messages, NULL only for count=0. */
    size_t message_count;                 /* Message count; excludes system_prompt. */
    const agent_tool_view_t* tools;       /* Callable model-visible definitions. */
    size_t tool_count;                    /* Tool count, zero disables tool generation. */
    agent_string_view_t session_id;       /* Effective session ID. */
    agent_string_view_t trace_id;         /* Optional trace label, not credentials. */
    const agent_cancel_token_t* cancel;   /* Poll until synchronous completion. */
    uint64_t deadline_ms;                 /* Absolute runtime deadline; 0=none. */
    uint32_t timeout_ms;                  /* Remaining call budget at entry, 0=unbounded. */
    uint32_t max_output_tokens;           /* Requested token budget; 0=unspecified. */
} agent_model_request_t;

/* Core-owned bounded response sink. Providers copy no data and retain no sink pointer. */
typedef struct {
    agent_error_t (*text)(void* context, agent_string_view_t text); /* Append text chunk. */
    agent_error_t (*tool_call)(void* context,
                                    const agent_tool_call_view_t* call); /* Append full call. */
    void* context; /* Borrowed sink state; never freed by provider. */
} agent_model_sink_t;

/* Synchronous provider operations copied into a model wrapper. */
typedef struct {
    agent_error_t (*complete)(void* context, const agent_model_request_t* request,
                                   const agent_model_sink_t* sink); /* Required synchronous operation. */
    void (*destroy)(void* context); /* Optional state cleanup; not wrapper deallocation. */
} agent_model_ops_t;

/* Initializes a wrapper in caller storage without opening I/O. */
agent_error_t agent_model_init(agent_model_t** model, agent_model_workspace_t* workspace,
                               const agent_model_ops_t* ops, void* context);

/* Allocates a wrapper through the supplied allocator. */
agent_model_t* agent_model_create(const agent_model_ops_t* ops, void* context,
                                  const agent_allocator_t* allocator);

/* Destroys an unbound idle wrapper. Caller-provided storage is not freed. */
void agent_model_destroy(agent_model_t* model);

/* Binds a borrowed model while idle; caller retains its lifetime. */
agent_error_t agent_set_model(agent_t* agent, agent_model_t* model);

/* Binds a model and transfers wrapper ownership only on success. */
agent_error_t agent_set_model_owned(agent_t* agent, agent_model_t* model);

/* Returns the current borrowed model for idle provider-specific configuration. */
agent_model_t* agent_get_model(const agent_t* agent);

#ifdef __cplusplus
}
#endif
