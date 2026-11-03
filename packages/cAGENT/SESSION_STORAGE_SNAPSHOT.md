# Session 文件存储来源

来源：本机 `/home/arongw/Documents/C/cAgentV2`，HEAD `059b568`，2026-10-26 的上述目录工作区快照。保留 MIT 声明。仅引入 File Store、JSONL 与 POSIX/OpenVela 文件端口，未引入 Memory provider。

移植修改另见 Git 差异；以下为引入时 SHA-256。

- `providers/storage/files/CMakeLists.txt`: `0a26bd4978f5c5c87e960a0ee8ba79759f7a302f6dd76c54bc131a0d2dc4cbc4`
- `providers/storage/files/README.md`: `4a04c207d552998bcf136c742cae1100bc3b35370d3651a0a7e62d0b723f180b`
- `providers/storage/files/include/agent_file_store.h`: `c622aaebace09e073a5767fa7834af99dcc12f4a3368817623459c23ae2338d1`
- `providers/storage/files/src/file_store.c`: `6f1afd9bc6ea472d1db251b298fdb5b6f9c44cfb333307eb185a0a4d337fc600`
- `providers/storage/jsonl/CMakeLists.txt`: `ce40f129b09dfbed4834514107c0fe29ccd3493b6c48ed2e51f1884e181c3ba3`
- `providers/storage/jsonl/README.md`: `ac8b4876518a4f3967b00e4ea09e02486d31d1d23c4e6b7693c24624f497d6e9`
- `providers/storage/jsonl/include/agent_session_jsonl.h`: `e5488230bd2eec890b9ba63df4465895ee1e217c99a79e3377915a91ba93018b`
- `providers/storage/jsonl/include/agent_session_jsonl_files.h`: `50e2e37fa39bd54a0a96cc85f7a9380ffa91880e18f059de28c0ad87b0e12465`
- `providers/storage/jsonl/src/file_store_bind.c`: `3a18f61e1d99c48614ef1a5b3fdf5b12f125e79aeae4e95f0b6dd97e81d56053`
- `providers/storage/jsonl/src/jsonl_internal.h`: `1a333b33669fa0e0b0b84cf4beeec11f72222d3fffb943fead65a9e23dfed2a5`
- `providers/storage/jsonl/src/jsonl_record.c`: `d37b377bfe51980884e2a618347f94222117ee4ced094e0d38dbb9ebdf7b596e`
- `providers/storage/jsonl/src/session_jsonl.c`: `37e2583881e3e208f10a15c5a53ed9cd7a65587f139c843a7bda1374e7527295`
- `ports/posix/storage/CMakeLists.txt`: `709aafabe47516f9abbdb8805dc462b74e87bc59480a654da59d336f5ef5b226`
- `ports/posix/storage/README.md`: `e5780943d3b67b4234fb120a1a235c808b5c46447f4c707665006daadb5e8b8f`
- `ports/posix/storage/include/agent_posix_file_store.h`: `54ce32211e61c425b334b428e4b89d43c1d3f1a5a93f4158d94f2b982742b326`
- `ports/posix/storage/src/file_store.c`: `aa6de2ebf3e840490a19518830989297c41998144cfffab79190291de7da933b`
- `ports/openvela/storage/README.md`: `6cce66645c1bd71228c139c2d45e16200d695cdbe7d232398c370b88d1a5ce4d`
- `ports/openvela/storage/include/agent_openvela_file_store.h`: `8abfe3edc320d76373e8e966fdfefcc85b1cec0ee14f3909bd04fdfb4571e381`
- `ports/openvela/storage/src/file_store.c`: `842ea69207c4beb16aed2ace9081f6d3c070f36c9654d1cb7a929ab968052838`
