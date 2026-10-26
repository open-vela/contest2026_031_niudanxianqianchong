# Optional File Store

`agent_file_store.h` provides synchronous byte-file ops and bounded helpers.
It lives in `providers/`, not Core; it has no JSON, Markdown, filesystem or
platform SDK dependency. A store borrows its immutable ops and context. Missing
callbacks mean `AGENT_ERROR_NOT_SUPPORTED`, allowing read-only backends.

Names are length-delimited **single components**. Empty names, `.`/`..`, NUL,
slash, backslash and the private `.cagent-` prefix are rejected. Each backend
instance binds one trusted application-owned root. Nested folders use separate
instances; model arguments must not select roots or authorization.

| Operation | Contract |
|---|---|
| `size` | Size in bytes; a missing file returns `AGENT_ERROR_NOT_FOUND`. |
| `read` | Offset + capacity; successful short reads allowed, zero bytes means EOF. |
| `visit` | Unordered regular-file leaf names, borrowed during callback; stop/error ends enumeration. No backend reentry from callbacks. |
| `append` | Append bytes; failure may leave a prefix. Record rollback belongs to the consumer. |
| `truncate` | Change an existing file's length. |
| `sync` | Request backend synchronization, not an unconditional power-loss guarantee. |
| `remove` | Remove one named file; an error may follow deletion if later synchronization fails. No rollback or whole-directory clear primitive. |
| `replace` | Publish a complete replacement; `published=true` survives a later synchronization failure. |

`agent_file_read_exact` joins short reads and rejects unexpected EOF.
`agent_file_read_all` checks both caller capacity and `max_bytes`, verifies EOF
and final size, and never reports a truncated document as success. This detects
observed growth/shrink, **not all concurrent changes**. It is **not a snapshot**: callers must serialize
edits, including same-size replacement. `agent_file_read_text` adds a NUL
terminator, rejects embedded NUL and returns a borrowed view of caller storage.
It does not validate UTF-8, parse Markdown or grant permission to write Soul.

State, roots and buffers remain valid while consumers use them. Inputs, outputs,
result objects and backend scratch must be disjoint; all operations borrow
buffers only until return. Serialize each instance and shared files across
instances. No implicit heap allocation occurs in these helpers; filesystem/SDK
allocations are outside this guarantee.

Build with `AGENT_BUILD_FILE_STORE=ON`, link `cagent::file_store`. Kconfig users
can select `CONFIG_AGENT_FILE_STORE`. For physical file access also select a
[POSIX](../../../ports/posix/storage/README.md), ESP-IDF or OpenVela Port.
Session JSONL connects through `agent_session_jsonl_files.h`; applications can
continue supplying the original JSONL callbacks without this component.

```sh
bash tests/providers/files/compile.sh
bash tests/ports/posix/file_store_compile.sh
bash tests/ports/posix/file_store_faults.sh # GNU-compatible linker
```
