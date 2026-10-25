# OpenVela Transport

This optional subpackage implements `agent_transport_ops_t` using NuttX
`netutils/webclient`. Include `ports/openvela/transport/include` and compile `transport/src/transport.c` only when
`CONFIG_NETUTILS_WEBCLIENT` is enabled. The supplied `Kconfig` and `CMakeLists.txt` are integration
fragments for NuttX application builds; they do not change the portable Core build.

Required behavior:

- complete the bounded synchronous request and stop all callbacks before returning;
- enforce response header/body limits before forwarding data to the sink;
- map link, DNS, socket, and TLS failures without converting HTTP status into transport failure;
- keep TLS state, certificates, sockets, and I/O buffers outside Core workspace.

The Model Provider owns OpenAI JSON, SSE, authorization semantics, and HTTP status interpretation.

Initialize a caller-owned `agent_port_openvela_transport_t` and its URL, request-header, I/O and
response-header buffers with `agent_port_openvela_transport_init()`. The adapter currently supports
GET and POST. It delivers 401/429/5xx status and body to the Model Provider and rejects 3xx instead
of allowing `webclient` to follow redirects with credentials.

HTTPS requires a `webclient_tls_ops` implementation and state: enable the bundled mbedTLS backend
under [../tls/](../tls/README.md) or supply your own. This adapter cannot verify whether that
implementation authenticates the server: the bundled backend defaults to OPTIONAL verification
(documented debt in its README), and application-supplied ops must enforce CA-chain and hostname
verification themselves. Without TLS ops, HTTPS returns `AGENT_ERROR_NOT_SUPPORTED`; it never falls
back to HTTP. `webclient` uses an inactivity timeout in blocking mode and DNS resolution can block,
so the request deadline and cancellation token are checked before/after the call and in callbacks,
not continuously while the SDK blocks. A hard deadline or prompt cross-task cancellation needs a
different backend or a verified nonblocking integration. Native OpenVela device testing remains open.
