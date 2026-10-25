# Transport Internals

This directory implements the platform-neutral HTTP Transport contract: bounded request validation
and synchronous `agent_transport_ops_t` dispatch.

It must not include platform HTTP, socket, DNS, TLS, RTOS, or SDK headers. Concrete adapters live
in the selected `ports/<platform>/` package or in application code.
