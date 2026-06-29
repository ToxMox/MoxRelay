# Vendored: Spout2 SDK (SpoutDX subset)

| Field | Value |
|-|-|
| Upstream | https://github.com/leadedge/Spout2 |
| Version | tag `2.007.017` |
| Commit | `f49e2f469f8cb25f559a6eaa61a3f5b8173fc100` |
| License | BSD-2-Clause (see `LICENSE` in this directory; per-file headers retained verbatim) |
| Vendored | 2026-06-09 |

## File set

The minimal SpoutDX (DirectX-only) subset -- the same 7-translation-unit recipe the project's
color probes validated. The OpenGL side of the SDK (SpoutGL.cpp, Spout.cpp, SpoutGLextensions,
SpoutSender/SpoutReceiver wrappers) is deliberately NOT vendored.

```
SpoutDirectX/SpoutDX/SpoutDX.{h,cpp}
SpoutGL/SpoutCommon.h
SpoutGL/SpoutCopy.{h,cpp}
SpoutGL/SpoutDirectX.{h,cpp}
SpoutGL/SpoutFrameCount.{h,cpp}
SpoutGL/SpoutSenderNames.{h,cpp}
SpoutGL/SpoutSharedMemory.{h,cpp}
SpoutGL/SpoutUtils.{h,cpp}
```

The SDK's two-directory layout is preserved so both include forms found in upstream sources
(`"SpoutCommon.h"` via the include path and `"../../SpoutGL/..."` relative) resolve unchanged.

Built as the static library `moxspout` (top-level CMakeLists.txt), MSVC runtime **/MD**
(MultiThreadedDLL) to match the Qt6 + libobs runtime of the moxrelay executable. With neither
`SPOUT_BUILD_DLL` nor `SPOUT_IMPORT_DLL` defined, `SPOUT_DLLEXP` collapses to nothing and the
sources link straight in as plain symbols.

## Local modifications

ONE (additive; applied 2026-06-09 for the M2.4 perf harness Flush A/B, PROMOTED to the product
default send path 2026-06-09 after the measured ~5-7x A/B delta and a full M2.3 fleet-gate
re-run):

| File | Change | Reason |
|-|-|-|
| `SpoutDirectX/SpoutDX/SpoutDX.h` | + declarations `SendTextureNoFlush(ID3D11Texture2D*)`, `SignalNewFrame()` inside a `MOXRELAY LOCAL MOD` marker block | batched flush mode (PRODUCT DEFAULT) |
| `SpoutDirectX/SpoutDX/SpoutDX.cpp` | + definitions of the two methods (marker block before the partial-texture `SendTexture` overload) | same |

`SendTextureNoFlush` is byte-identical to the stock single-argument `SendTexture` minus the
per-send `m_pImmediateContext->Flush()` and the `frame.SetNewFrame()` signal; `SignalNewFrame`
is the mutex-bracketed signal for use AFTER the caller's single batched Flush (the signal must
never precede the flush -- the flush-before-readers rule is why the per-send Flush exists
upstream). No upstream line is modified or removed; both methods are additive. Since the
2026-06-09 promotion they are the PRODUCT DEFAULT send path (`SpoutSenderEngine` `FlushMode::
Batched`: per-sender `SendTextureNoFlush`, ONE engine-issued `Flush()` per frame, then per-sender
`SignalNewFrame()`); the stock per-sender-Flush `SendTexture` path remains selectable via
`--perf --flush-mode stock` for A/B comparison.

### Overrun-safe sender-map sizing

TWO (additive; applied 2026-06-28 to let the user raise the Spout sender cap -- Settings > Max
Spout senders, default 64, range 1..1024 -- without risking a read past a smaller pre-existing
shared map):

| File | Change | Reason |
|-|-|-|
| `SpoutGL/SpoutSharedMemory.cpp` | in `Create()`, after `MapViewOfFile`: on the ALREADY-EXISTS path, `VirtualQuery` the mapped view and clamp `m_size` to the true committed `RegionSize` instead of the requested size (`MOXRELAY LOCAL MOD` marker) | a pre-existing smaller map keeps its original size; recording the larger requested size let `m_size`-bounded readers run past the real mapping |
| `SpoutGL/SpoutSenderNames.cpp` | in `CreateSenderSet()`, after `Create`: clamp `m_MaxSenders` down to `m_senderNames.Size() / SpoutMaxSenderNameLen` (the real buffer capacity) (`MOXRELAY LOCAL MOD` marker) | so every `m_MaxSenders`-bounded loop and the `RegisterSenderName` cap respect the actual mapping when another app created a smaller map |

Both are additive; no upstream line is removed. Together they make a raised `SetMaxSenders(n)` safe
even when another Spout app already created the shared `SpoutSenderNames` map at a smaller size.

## Update procedure

1. `git -C <research>/Spout2 fetch && git checkout <new tag>`
2. Re-copy the file set above; re-apply any local modifications logged here.
3. Update the Version/Commit fields; re-run `--selftest` and verify the senders with an
   external Spout receiver.
