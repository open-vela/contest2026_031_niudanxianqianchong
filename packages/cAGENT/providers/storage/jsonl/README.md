# JSONL Session Storage

`cagent::session_jsonl` is an optional, filesystem-independent Session Storage
backend. It writes one versioned JSON object per finished turn to a file chosen
by the application. The Core only sees `agent_session_storage_t`; it does not
open files or know JSONL. The backend uses the private bounded JSON codec and
caller-owned buffers. No heap allocation is performed by this backend.

## File operations

The application supplies `agent_session_jsonl_file_ops_t`. Each `session_id`
addresses a separate file; the application maps the length-delimited ID to a
safe path or handle. Never concatenate an unchecked ID into a filesystem path.

| Operation | Required behavior |
|---|---|
| `size` | Return the current byte size; `AGENT_ERROR_NOT_FOUND` means no file yet. |
| `read` | Read exactly the requested bytes at the given offset, or return an error. |
| `append` | Append the supplied bytes in order. On error it may leave a partial tail; the backend attempts `truncate` rollback. |
| `truncate` | Set the existing file to the requested byte length; used for tail repair and `clear`. |
| `sync` | Apply the target filesystem's durability operation after append/truncate; the application owns its actual persistence guarantee. |
| `remove` / `clear_all` / `count` | Delete one identity, delete all identities, or count identities in the application's namespace. |

All callbacks are synchronous. A backend instance supports one active turn and
is not thread-safe. The application must serialize shared use. `clear` truncates
a file but retains its identity; `remove` deletes it. `recent` returns complete
turns newest first and excludes aborted turns. It reads backwards in bounded
chunks and never loads an entire history file.

The caller supplies separate write-line, read-line, decoded-text, token,
message-view, Tool-call-view and session-ID buffers. They must not overlap and
remain alive while bound. The token buffer must be aligned for `int`; `init`
checks sizes and overlap. A record exceeding the configured line/view capacity
returns `AGENT_ERROR_CAPACITY`, not a truncated record.

## Shared file-store binding

With `AGENT_BUILD_FILE_STORE=ON`, the separate `cagent::session_jsonl_files`
target provides `agent_session_jsonl_files.h`. This optional bridge copies an
`agent_file_store_t` and binds the existing eight JSONL callbacks:

```c
agent_session_jsonl_files_t files;
char names[160];
agent_session_jsonl_config_t config = {0};
/* Initialize store with a platform Port; fill the existing JSONL buffers. */
agent_error_t status = agent_session_jsonl_files_init(
    &files, &store, names, sizeof(names), &config);
if (status == AGENT_OK)
    status = agent_session_jsonl_init(&jsonl, &config);
```

State, name scratch, underlying backend and all JSONL buffers must be disjoint
and outlive consumers. The bridge requires size/read/visit/append/truncate/sync/
remove; read-only stores are rejected at binding time. It converts short reads
to exact reads. IDs become `session-<lowercase hex>.jsonl`, including IDs with
NUL or separators; filenames remain unchanged from the earlier implementation. `count` and
`clear_all` only affect matching regular files, not Markdown or unknown names.
Clear iterates and removes outside visitor callbacks; it may partially finish
before error and uses repeated enumeration, not an unbounded list allocation.
Previously deleted files are not restored. Removal can also return an error
after deleting a file if directory synchronization fails; a subsequent removal
may report NOT_FOUND. Callers must not interpret failure as an unchanged store.

`agent_session_jsonl_file_ops_t` remains the domain file contract, also allowing
application-supplied callbacks without a generic store. Session-only platform
entry points have been removed; physical I/O uses the shared file store.
JSONL-only builds do not require the file-store target. Formats,
transactions and tail repair are unchanged; Core still has no file dependency.

## Record and recovery

Each committed line has this shape:

```json
{"v":1,"sid":"home","m":[{"r":2,"c":"hello","tid":"","tc":[]}],"o":2}
```

`r` is the normalized message role; `o` is 1 for complete, 2 for aborted.
Tool-call arguments are stored as escaped JSON text in each call's `a` field,
then validated as an object on read. Unknown versions and malformed terminated
lines fail with `AGENT_ERROR_PARSE`; they are not silently skipped. A final
line without a newline is ignored by `recent` and truncated before the next
`begin`. `append` failure attempts to restore the previous file length.

`finish()` appends the full line and calls `sync`. A `sync` failure is
ambiguous: the line might already be durable, so callers must not assume a
failed return implies the record is absent. There is no exactly-once guarantee.
This format persists **finished turns only**. It does not flush Tool intent
before a side-effecting Tool runs; crash-time reconstruction of an in-progress
Tool requires a separate journal/commit protocol. Retention, compaction,
encryption and media wear policy remain application/backend responsibilities.

## Build and test

```sh
cmake -S . -B build -DAGENT_BUILD_JSON_CODEC=ON -DAGENT_BUILD_SESSION_JSONL=ON
cmake --build build
bash tests/session/jsonl_compile.sh
```

Link `cagent::session_jsonl`, then initialize `agent_session_jsonl_t` with
`agent_session_jsonl_config_t`, bind it to an `agent_session_storage_t`, and
call `agent_set_session_storage()` while the Agent is idle. ESP-IDF builds can
enable `CONFIG_AGENT_SESSION_JSONL` and supply their own filesystem callbacks.
The Host contract test provides a real POSIX temporary-directory adapter; no
POSIX headers are part of the library backend.

For applications with a compatible filesystem, the optional
[`ports/posix/storage/`](../../../ports/posix/storage/README.md) package now
provides production file operations. It does not mount a filesystem or
replace the JSONL provider's caller-owned buffers.
