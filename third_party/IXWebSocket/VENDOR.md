# Vendored: IXWebSocket

| Field | Value |
|-|-|
| Project | IXWebSocket (https://github.com/machinezone/IXWebSocket) |
| Version | v11.4.6 |
| Commit | 2efe037c9cc96fd536774f17bdb5215161ee5087 (tag `v11.4.6`) |
| License | BSD-3-Clause (see `LICENSE.txt`, retained verbatim) |
| Vendored | 2026-06-10 |

## What is vendored

The `ixwebsocket/` source directory, EXCLUDING the TLS backends (`IXSocketAppleSSL.*`,
`IXSocketMbedTLS.*`, `IXSocketOpenSSL.*`). MoxRelay's control endpoint binds loopback
only and never uses TLS or compression, so the library is built with neither
`IXWEBSOCKET_USE_TLS` nor `IXWEBSOCKET_USE_ZLIB` defined (the gzip/deflate translation
units compile to no-op paths without the zlib macro). No OpenSSL/mbedTLS/zlib enters
the tree.

Built as the static lib `moxws` (see the root `CMakeLists.txt`); compiled sources are
the upstream non-TLS `IXWEBSOCKET_SOURCES` list. Vendoring (rather than a
configure-time download) keeps this repository self-contained and offline-buildable --
the same reproducibility requirement that drove the `third_party/Spout2` vendoring.

## Local modifications

NONE. The sources are byte-identical to the upstream tag.
