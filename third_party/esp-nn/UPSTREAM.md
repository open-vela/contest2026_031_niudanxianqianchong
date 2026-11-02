# ESP-NN Upstream Provenance

This directory is a vendored copy of Espressif ESP-NN for the local
`ccf_audioevent` ESP32-S3/TFLite Micro integration work.

| Item | Value |
| --- | --- |
| Upstream repository | https://github.com/espressif/esp-nn |
| Imported commit | `10b6c0fc884a3b05f94a752f91d00ebadfe5d8d0` |
| Imported branch | `master` |
| Import date | 2026-08-01 |
| License | Apache License 2.0; see [LICENSE](LICENSE) |

The original Git metadata was intentionally removed after import so the files
are tracked directly by the `ccf_audioevent` repository rather than as a
submodule. Do not modify upstream implementation files in place. Keep any
openvela-specific changes as separately documented patches under
`ccf_audioevent/patches/esp-nn/`, with their rationale and the upstream commit
they apply to.

At this import stage the source is retained in full for provenance. The build
integration must explicitly whitelist the ESP32-S3 C and assembly files it
uses; ESP-IDF component metadata and test applications are not part of the
openvela production build.
