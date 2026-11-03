# POSIX File Storage

## Shared byte-file backend

`agent_posix_file_store.h` is the entry for shared file access, with
no JSONL dependency. It implements `agent_file_store_t` over one existing,
trusted absolute directory. It does not mount, format or create directories.

```c
#include <agent_posix_file_store.h>

static agent_posix_file_store_t files;
static char paths[512];
static char user[2048];
agent_file_store_t store;
agent_string_view_t text;
agent_posix_file_store_config_t config = {
    "/data/user", paths, sizeof(paths), false, false
};
agent_error_t status = agent_posix_file_store_init(&files, &config, &store);
if (status == AGENT_OK)
    status = agent_file_read_text(&store, agent_string_view("USER.md", 7u),
                                 user, sizeof(user), sizeof(user) - 1u, &text);
```

The config is copied; state, root string and caller scratch remain valid. Each
operation closes its descriptors before return, including failure paths. Access
is synchronous and serialized by the caller. Leaf symlinks/nonregular files
are rejected by the standard profile; enumeration skips them and private temporary files. This is **not
a sandbox against concurrent untrusted directory mutation**: the whole root and
its ancestors must be trusted and stable. Use a different backend if that
assumption cannot hold. Inputs/outputs must not overlap path scratch or state.
`AGENT_POSIX_FILE_STORE_NO_SYMLINKS=1` is an explicit build profile for VFS mounts
without symlink support or lstat (used by ESP-IDF). It uses stat, does not provide
symlink rejection, and must never be used on a filesystem that admits symlinks.

`read_only=true` binds only size/read/visit. For ordinary paths, scratch needs
`root + slash + name + NUL`. Replacement requires **two full paths** at once:
target plus private `.cagent-xxxxxxxx` temporary path. Too-small scratch returns
`AGENT_ERROR_CAPACITY` before modifying the target.

Replacement writes a same-directory exclusive temporary file, synchronizes it,
closes it, then renames it over the target. New files have mode 0600; replacement
does not preserve old permissions or metadata. It never truncates the old target
first. On failure before rename, cleanup is attempted and `published=false`;
after rename, `published=true`, even if directory synchronization fails. Do not
blindly retry an ambiguous result. Stale private files after a crash are hidden
from enumeration, not automatically deleted; applications own maintenance.

`sync_directory=true` attempts directory `fsync` during init and requests it
after sync/remove/replace. Unsupported operations fail explicitly. With `false`,
file `fsync` is still requested, but directory metadata durability is not claimed.
Both modes require filesystem-specific reboot/power-loss validation. POSIX
rename publication is not itself proof of crash consistency on an embedded VFS.
Removal unlinks before directory synchronization. A synchronization error may
therefore be reported after the target has disappeared; no rollback is provided.

Host build: `AGENT_BUILD_FILE_STORE=ON` and `AGENT_BUILD_POSIX_FILE_STORE=ON`,
then link `cagent::posix_file_store`. Core-only and RAM Session builds do not
require these components. See [file contracts](../../../providers/storage/files/README.md).

## JSONL Session binding

Session naming/count/clear belong to the Provider's
`agent_session_jsonl_files_t`, not this Port. Initialize a writable store for
the chosen Session root, then bind it with independent filename scratch:

```c
#include <agent_session_jsonl_files.h>

agent_session_jsonl_files_t files;
agent_session_jsonl_config_t config = {0};
char names[160];
/* Fill the existing JSONL buffers and keep all state/scratch alive while bound. */
agent_error_t status = agent_session_jsonl_files_init(
    &files, &store, names, sizeof(names), &config);
```

Enable `AGENT_BUILD_FILE_STORE`, `AGENT_BUILD_POSIX_FILE_STORE`,
`AGENT_BUILD_JSON_CODEC` and `AGENT_BUILD_SESSION_JSONL`; link
`cagent::posix_file_store` and `cagent::session_jsonl_files`. The JSONL Provider
still owns records and tail repair; the bridge owns `session-<hex>.jsonl` names
and namespace-specific cleanup. No Session-only Port API or compatibility
wrapper remains. Existing filenames and JSONL records are unchanged.
