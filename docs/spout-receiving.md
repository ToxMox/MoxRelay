# Receiving MoxRelay's Spout senders

MoxRelay publishes each broadcasting source as its own **Spout** sender on the local
machine. Any Spout-aware application (a game engine, a VJ tool, a compositor, your own
program) can receive those textures with no copy back through system memory. This guide
explains how a receiver finds a MoxRelay sender, decodes its pixels correctly, and why
a sender you expect might not be there.

For driving MoxRelay itself from the keyboard or GUI, see `docs/usage.md`.

## Sender names

Every MoxRelay sender has a globally unique name on the machine, built from a fixed
scheme:

```
{MachineName}:Helper_{port}_{SourceName}
```

- `{MachineName}` is the local computer name.
- `{port}` is the instance's local control port. It makes names unique across multiple
  MoxRelay instances running at once (each binds its own port).
- `{SourceName}` is the source's display name.

The leading part, `{MachineName}:Helper_{port}`, is the **per-instance prefix** shared
by every sender from one MoxRelay instance. So if the machine is `STUDIO`, the port is
`7341`, and you have a source named `Desktop`, the sender is:

```
STUDIO:Helper_7341_Desktop
```

### Collision suffixes

If two sources would resolve to the same name, MoxRelay appends `_2`, `_3`, and so on
to keep every name unique (it starts at `_2` because Spout's own fallback already uses
`_1`). So a second `ColorA` source becomes `STUDIO:Helper_7341_ColorA_2`.

**Do not predict or hard-code names.** Because of collision suffixes and re-attaches
(below), the name a receiver must bind to is the **actual registered** name, not the
one you computed from the scheme. Always read it back (from the GUI, the discovery
file, or the control API) and bind to it exactly.

## Decoding the pixels: sRGB, linear, and fp16

Each source's sender uses one of three pixel formats, chosen by its **Format** picker
in MoxRelay (see `docs/usage.md`). A receiver must decode to match, or colors will be
wrong:

| Format | Texture | How to treat it |
|-|-|-|
| `srgb87` (default) | 8-bit BGRA (DXGI `B8G8R8A8_UNORM`, 87), sRGB-encoded bytes | Decode sRGB once on import (sample through an sRGB view / treat as an sRGB texture). This is the common, correct-by-default case. |
| `linear87` | The same 8-bit BGRA container, but linear bytes | Do **not** sRGB-decode. Treat the bytes as already-linear. An sRGB-decoding receiver will render this too dark by design. |
| `fp16` | 16-bit float RGBA (DXGI `R16G16B16A16_FLOAT`, 10), linear half-float | Linear half-float; ideal for HDR / high-precision pipelines. No sRGB decode. |

All three formats carry an alpha channel. A receiver can also read the actual DXGI
format straight from the Spout sender itself and branch on it, rather than tracking
which MoxRelay format was selected.

## Why a sender might not appear

If a receiver does not see a sender it expects, it is almost always one of these:

- **The source is audio-only.** The `audio_input` and `audio_output` source types have
  no video and never create a sender. A `media` source that turns out to carry only
  audio likewise publishes no sender. There is simply nothing to receive.
- **Broadcast has not been started.** Publishing is instance-wide and off until the
  user starts it (the **Start Broadcast** button, or **Ctrl+B**). No senders exist
  until then.
- **The name has not resolved yet.** A sender's real name is assigned only after the
  source transmits its **first frame**. Until then the name reads as `null`. A source
  that has not produced a frame (for example a capture target that is not yet
  rendering) will not show up.
- **A format change re-attached the sender.** Changing a broadcasting source's pixel
  format tears its sender down and re-creates it under the new format. During that
  brief window the name goes `null` and then resolves again (usually back to the same
  name). A receiver bound to the old handle should re-bind when the name re-resolves.

## Binding programmatically

For a receiver that drives MoxRelay through its local control API, the robust way to
bind is event-driven rather than name-prediction. MoxRelay emits a
**`senderNameResolved`** event whenever a source's actual registered sender name
becomes known (after its first transmitted frame), changes (the rare collision case),
or re-resolves after a `StartBroadcast` or a format re-attach. The event payload
carries the `sourceId` and the resolved `senderName`.

The correct pattern is:

1. Subscribe to `senderNameResolved` (and, if you want lifecycle coverage, the
   broadcast and source events).
2. When the event fires for a source you care about, bind your Spout receiver to the
   `senderName` exactly as delivered.
3. If a later `senderNameResolved` arrives for the same `sourceId` with a different
   name, re-bind to the new name.

The instance also publishes its `spoutPrefix` (the per-instance
`{MachineName}:Helper_{port}` prefix) so a client can recognize which senders belong
to which instance without re-deriving the scheme.

For the full event schema, the subscription model, and the rest of the control verbs,
see `docs/control-api.md`.
