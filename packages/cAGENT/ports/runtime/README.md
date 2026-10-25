# OpenVela Runtime

This optional subpackage provides `agent_port_openvela_runtime_init()` in
`runtime/include/agent_openvela_runtime.h`. It fills a caller-owned `agent_runtime_t` with a
`CLOCK_MONOTONIC` millisecond clock; all other callbacks remain unset. Enable
`CONFIG_AGENT_PORT_OPENVELA_RUNTIME` or compile `runtime/src/runtime.c` with the public headers.

The builder probes the clock during init and returns `AGENT_ERROR_IO` if unavailable. A later clock
read failure returns `UINT64_MAX` so absolute deadlines fail closed. The normal path does not
allocate. The application can add allocator, logging, and a paired `cancel_sync` before `agent_init`;
the clock builder does not install an implicit global mutex or create threads.

Cross-task cancellation needs application-supplied synchronization. The callbacks must cover both
`agent_cancel_token_request()` and active-turn reads, and must not be called from an unsupported
ISR context. The selected NuttX board must support `clock_gettime(CLOCK_MONOTONIC)`.

This Runtime contains no socket, mbedTLS, HTTP, PSRAM, Flash, or JSONL storage policy. Native device
validation remains outstanding.
