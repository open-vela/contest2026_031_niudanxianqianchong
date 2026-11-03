# jsmn

Unmodified `jsmn.h` from upstream tag `v1.1.0` (commit
`fdcef3ebf886fa210d14956d3c068a653e76a24e`). SHA-256:
`1ed6154dedf009212a08a397e9c4ed50a0ce31d5a8301bb294e137ae3188c13b`.

Upstream: <https://github.com/zserge/jsmn>. The MIT license notice is embedded in
the header. `codecs/json/reader.c` compiles it with internal linkage; other
translation units include declarations only. The surrounding codec performs
strict JSON grammar validation because jsmn itself only tokenizes the input.
