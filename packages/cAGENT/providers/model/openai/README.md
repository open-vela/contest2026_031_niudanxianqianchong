# OpenAI-compatible Model Provider

This optional provider implements one synchronous, non-streaming Chat Completions
request. It translates the canonical `agent_model_request_t` to JSON, calls the
injected `agent_transport_ops_t`, validates the full response, and forwards
assistant text or complete tool calls to `agent_model_sink_t`. It does not own a
socket, TLS stack, session, Core workspace, or heap allocation. It does not
implement streaming, Responses API, image/audio input, or vendor-specific features.

## Build

On Host CMake, enable both the private JSON codec and provider:

```sh
cmake -S . -B build -DAGENT_BUILD_JSON_CODEC=ON -DAGENT_BUILD_OPENAI_PROVIDER=ON
cmake --build build
```

Link `cagent::provider_openai`; its Core and JSON dependencies are propagated by
CMake. For ESP-IDF, enable `CONFIG_AGENT_PROVIDER_OPENAI`; the top-level component
compiles the provider and private JSON codec into the `cagent` component. The
HTTP backend remains a separately selected Port or application implementation.
For another build system, compile `model_openai.c`, `openai_request.c`,
`openai_response.c`, `codecs/json/reader.c`, and `codecs/json/writer.c` alongside
Core, with the provider, codec, and vendored jsmn private include paths.

## Ownership and lifetime

The application supplies an `agent_openai_config_t` with a complete endpoint URL,
model ID, optional complete `Bearer ...` header value, monotonic clock, synchronous
Transport binding, and three *pairwise disjoint* buffers. The endpoint, model,
authorization text, Transport state, and buffers must stay alive and unmodified
until no completion is running. The provider copies the config struct but borrows
everything it points to. An authenticated endpoint must use HTTPS; HTTP is
accepted only without an authorization header for local compatible services.
The endpoint, model, and authorization storage must not overlap any mutable
Provider buffer. Per-call Model request/sink state, input views, and their
arrays must not overlap the Provider buffers, which are overwritten during
completion.

| Buffer | Use |
|---|---|
| `request_buffer` | Request body until HTTP returns; then aligned token storage for response parsing. Must fit both the largest request and `response_token_capacity` tokens. |
| `response_buffer` | Bounded full non-streaming HTTP body. Transport may have its own additional buffers. |
| `decoded_buffer` | Complete assistant text or one tool call's decoded strings; reused between synchronous sink callbacks. |

`agent_openai_provider_init()` validates the configuration and makes no HTTP
request. `agent_openai_model_ops()` returns an immutable ops table. Initialize
an `agent_model_t` wrapper using `agent_model_init()` with caller-owned
`agent_model_workspace_t` and the provider as context; bind it with
`agent_set_model()`. Keep both wrapper and provider alive while bound. After
destroying the Agent, or replacing its borrowed Model while idle, destroy the
old wrapper and then reclaim provider storage. Do not run concurrent
completions on one provider instance or mutate its config while in use.

The provider itself does not allocate heap memory. Core, provider, and HTTP/TLS
backend have separate budgets. Set `max_json_depth` within 1..32, choose
`response_token_capacity` for the expected envelope, and size `decoded_buffer`
for the largest complete text or tool call. Insufficient response body, token,
request, or decode storage returns `AGENT_ERROR_CAPACITY` without truncation.
`AGENT_MAX_*` profile caps still apply to individual messages, tool schemas,
arguments, and output. Server certificate and hostname validation are the
responsibility of the selected HTTPS Transport; the provider does not bypass them.

## Wire and errors

The provider writes `POST` JSON with `model`, ordered `messages`, optional
function `tools`, optional `max_completion_tokens`, `n:1`, and `stream:false`. Tool
schema and historical arguments must be complete JSON objects; raw model text
cannot be injected into JSON. It consumes the first Chat Completion choice,
requires a supported `finish_reason`, and validates the selected message and all
tool calls before invoking any Model sink callback. Tool-call `arguments` are
decoded from the wire string and must themselves be a complete JSON object.
The optional `tool_calls: null` is treated as no calls; a non-null `refusal`
string is delivered as text when ordinary content is absent. Views delivered
to sinks are valid only during their callback.

HTTP `401/403` maps to `AGENT_ERROR_AUTH`, `429` to
`AGENT_ERROR_MODEL_RATE_LIMIT`, `408/504` to `AGENT_ERROR_TIMEOUT`, and `5xx`
to `AGENT_ERROR_MODEL_UNAVAILABLE`. Other unsuccessful statuses map to
`AGENT_ERROR_MODEL_FAILED`. Invalid response envelopes map to
`AGENT_ERROR_MODEL_PARSE`; invalid tool arguments map to
`AGENT_ERROR_TOOL_ARGUMENT`. The provider propagates cancellation, timeout,
Transport, capacity, and sink errors. It preserves the provider instance for a
subsequent call after any failure.

Run the Host contract test with `bash tests/providers/openai/compile.sh`. This
uses a deterministic fake HTTP Transport; it does not perform a live API request.
The current Core `agent_run()` loop remains unfinished, so the contract test
exercises the Model ops directly.
