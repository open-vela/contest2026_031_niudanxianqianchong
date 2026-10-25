# OpenVela TLS Backend

Blocking mbedTLS sessions exposed as `struct webclient_tls_ops` for the
[Transport](../transport/README.md) adapter and any other NuttX `webclient`
consumer (voice pipelines, bridges, or plain application requests). The
module produces the ops vector; it never touches Core workspace state.

## Lineage and license

`src/tls_core.c` is Apache-2.0 code derived from openvela
`packages/ai_agent` `src/infra/vela_tls.c` via contest cAGENT
`packages/cAGENT` `src/runtime/runtime_openvela.c`. It preserves that line's
device-tested behaviour:

- non-blocking `connect()` with a `select()` timeout (a filtered remote port
  must not block forever — `mbedtls_net_connect()` does on NuttX);
- CTR-DRBG seeded from `/dev/random` on NuttX (mbedTLS 4 on ESP targets does
  not wire the generic entropy context to the hardware RNG);
- ALPN disabled (some servers reject connections carrying it);
- SNI always sent via `mbedtls_ssl_set_hostname()` (CDN virtual hosting
  routes on it even while chain verification is relaxed);
- `WANT_READ`/`WANT_WRITE` handled by bounded `poll()` loops;
- writes advanced in bounded slices (default 1024 bytes) so ESP-Hosted (C6)
  Wi-Fi forwarding does not stall on large requests.

The wrapper (`src/tls_ops.c`) and the public header are MIT like the rest of
this repository.

## Configuration

`agent_port_openvela_tls_init()` validates and copies a borrowed
configuration into caller-owned state:

```c
agent_port_openvela_tls_t tls;
agent_port_openvela_tls_config_t config = { 0 }; /* OPTIONAL verify */

agent_port_openvela_tls_init(&tls, &config);
/* Wire into the Transport adapter or any webclient context: */
transport_config.tls_ops = tls.ops;
transport_config.tls_context = &tls;
```

- `verify` — `OPTIONAL` (zero-init default) checks the chain and logs the
  result without enforcing it, `REQUIRED` fails the handshake on an
  untrusted chain (requires `ca_pem`), `NONE` skips chain checks.
- `ca_pem` — borrowed NUL-terminated PEM trust bundle.
- `log` / `log_context` — optional diagnostics mirroring
  `agent_runtime_t.log`.
- `io_timeout_ms` — per-I/O inactivity ceiling (default 60000).
- `write_chunk_bytes` — per-write slice ceiling (default 1024, the C6 fix).

## Known debt: OPTIONAL verification

The current default verifies nothing that matters: encryption and handshake
integrity hold, but a man-in-the-middle is not rejected. This mirrors the
deployed V1 behaviour and is accepted for the contest's trusted networks.
Every OPTIONAL session logs the ignored chain-verify result, so real-world
chains can be collected before switching to `VERIFY_REQUIRED` with a bundled
CA (a few kilobytes of PEM for the model backends in use). The switch is a
configuration change, not a code change.

## Limits

- Blocking adapter: callbacks drive `poll()` internally; webclient
  non-blocking mode is unsupported (`get_poll_info` returns `-ENOTSUP`).
- No proxy tunneling (`init_connection` stays NULL).
- One request per connection at a time; the ops create and destroy
  connections, the state itself carries only borrowed configuration.
- IPv4 only, matching the deployed network stack.
