# MoxRelay Control API

A running MoxRelay process is one **instance**. Each instance serves its own
control endpoint so a host or tool can drive it programmatically: enumerate and
create sources, edit properties and filter chains, route audio, start/stop the
Spout broadcast, and subscribe to live state events.

This page is the human integrator's guide. The **canonical wire contract** is the
machine-readable AsyncAPI document shipped alongside it:

- [`docs/control-api.asyncapi.yaml`](control-api.asyncapi.yaml) -- every method,
  parameter, result, event payload, and error code, validated as AsyncAPI 3.0.
- [`examples/contract-test/contract_test.py`](../examples/contract-test/contract_test.py)
  -- a dependency-free **reference client** (Python standard library only) that
  drives a live instance and asserts the contract. Its `WsClient` class is a
  minimal, copy-able implementation of the wire. See
  [`examples/contract-test/README.md`](../examples/contract-test/README.md).

When this prose and the YAML disagree, the YAML wins.

---

## Overview

| Property | Value |
|-|-|
| Transport | WebSocket, text frames |
| Endpoint | `ws://127.0.0.1:{port}/control` |
| Binding | loopback only (`127.0.0.1`); never a network service |
| Encoding | JSON (one JSON value per text frame) |
| TLS | not used (loopback) |
| Protocol version | `apiVersion` 1 (echoed by `GetVersion`) |
| Compression | disabled (no permessage-deflate) |

The endpoint is a **local control surface**, not a remote API. It binds the
loopback interface only, and the authentication scheme below assumes a local
client; there is no remote/TLS mode.

---

## Authentication / the API key

There is **no static key, password, or env var**. Each launch mints a fresh,
random, per-launch token (`controlToken`) and publishes it in the discovery file.
A client reads it from that file and presents it on the WebSocket upgrade. The
token is fully automatic: the user never sees it, there is no prompt and no flag.

### The token

- 32 hex characters = 16 bytes from the Windows CSPRNG (`BCryptGenRandom`), not
  `rand()`.
- Generated **once per process launch** and held for that process's lifetime.
- **Rotates on every launch.** A client must re-read it from the discovery file
  before each new connection; a token from a previous run will be rejected.

### Connect flow (step by step)

1. Read `port` and `controlToken` from the discovery file (see
   [Discovery](#discovery) below) -- the same read a client already does to learn
   the port.
2. Open a WebSocket to:

   ```
   ws://127.0.0.1:{port}/control?token={controlToken}
   ```

   The token is a plain `?token=` query parameter on the upgrade URI. It is
   `[0-9a-f]` only, so no URL-encoding is needed.
3. **Do not send a browser `Origin` header.** Native WebSocket clients typically
   send none, which is correct here. (A `ws://` or `wss://` loopback origin is
   also accepted; any other origin is rejected -- see below.)
4. On success the upgrade completes and the connection is ready for requests. On
   failure the server **closes the socket** with one of the codes below before any
   message is exchanged.

### Two gates on the upgrade

The server enforces both checks during the handshake, before the connection is
registered or any verb runs:

1. **Origin fence (always on).** An upgrade with **no** `Origin` header is
   allowed. An upgrade **with** an `Origin` is allowed only if it is a
   `ws://` / `wss://` loopback origin (`127.0.0.1`, `localhost`, or `::1`).
   Anything else -- `http`/`https`, `null`, or a non-loopback host -- is rejected.
   A browser always sends its page's `http(s)`/`null` origin and can never forge a
   `ws(s)` loopback origin, so this fences out every browser-reachable page while
   letting native clients through. Rejection closes with **4403**.
2. **Token (constant-time compare).** When a token exists, the `?token=` value
   must match it exactly, compared in constant time (no length or content leak). A
   missing or wrong token closes with **4401**.

### Degraded mode (RNG failure only)

If the OS CSPRNG fails at startup, the generated token is empty and **token auth
is disabled** for that run (a one-time WARNING is logged). This is a fail-open
**only** on the helper's own RNG failure -- it never fails open on a missing or
wrong client token. **The Origin fence still applies** in this mode. In normal
operation the RNG does not fail and the token is always enforced.

### Upgrade close codes

| Code | Meaning |
|-|-|
| 4401 | Unauthorized: missing or wrong `?token=` |
| 4403 | Origin not allowed (a non-loopback / browser origin was sent) |

---

## Discovery

A client locates the instance through the **discovery file**, written by the
instance itself.

| Item | Value |
|-|-|
| Default path | `%APPDATA%/MoxRelay/helper-config.json` |
| Shape | a bare single-instance JSON object (not an array) |
| Writer | the instance process is the single writer |

### File contents

```json
{
  "instanceId": "tier-60",
  "port": 7341,
  "version": "<build/protocol version string>",
  "fpsTier": 60,
  "spoutPrefix": "MACHINE:Helper_7341",
  "ownerId": "",
  "controlToken": "<32 hex chars>",
  "sources": [{ "name": "MACHINE:Helper_7341_Desktop" }]
}
```

- `port` -- the loopback control port to connect to.
- `controlToken` -- the per-launch token for `?token=` (see
  [Authentication](#authentication--the-api-key)). Empty string = auth disabled
  (RNG-failure degraded mode).
- `sources[].name` -- the **actual** registered Spout sender names, already
  collision-suffixed (e.g. `_2`) where needed. Bind them verbatim.
- `ownerId` -- opaque owner token; empty = standalone (unowned).

### Lifecycle

- The file is rewritten on every engine-state change.
- Writes are **atomic**: a temp file is written then renamed over the target, so a
  concurrent reader never sees a torn file.
- On a **clean exit** the file is cleared to a bare empty object `{}` (no live
  instance). Stale non-empty content therefore only survives a **crash**.

### Discovery alternatives

A managed launch can redirect the handoff (see [`docs/cli.md`](cli.md)):

- `--discovery-path <file>` -- write the same JSON object to an explicit path
  instead of the default location.
- `--rendezvous-pipe <name>` -- write the instance JSON once over a named pipe the
  launcher owns, with no on-disk file. (Standalone launches always use the
  discovery file.)

---

## Message envelope

All frames are JSON. There are three shapes.

**Request (client to instance):**

```json
{ "id": 1, "method": "GetVersion", "params": {} }
```

- `id` -- any JSON value the client chooses; the reply echoes it. Use it to
  correlate (demux) replies to requests on a single connection.
- `method` -- a verb name (string, required).
- `params` -- an object (optional; defaults to `{}`). Most verbs require it to be
  an object when present.

**Reply (instance to client) -- exactly one per request, success or error:**

```json
{ "id": 1, "result": { "...": "..." } }
```

```json
{ "id": 1, "error": { "code": 1001, "message": "No such source: src_9",
                       "data": { "category": "source", "sourceId": "src_9" } } }
```

- Success carries `result`; failure carries `error` with `code`, `message`, and
  an optional `data` object. The `id` is the request's `id` (or `null` if the
  request could not be parsed far enough to recover it).

**Event (instance to client, unsolicited):**

```json
{ "event": "status", "data": { "instanceId": "tier-60", "ts": 1719600000000, "...": "..." } }
```

- Events have **no `id`** -- they carry `event` + `data`. Every event `data`
  includes `instanceId` and a `ts` (epoch milliseconds). Demux events by the
  `event` field, replies by the presence of `id`.

---

## Verb catalog

Names are exact and case-sensitive. Parameter and result schemas are in the
[AsyncAPI contract](control-api.asyncapi.yaml); the groupings below enumerate the
full set with a one-line purpose each. An unknown method returns error
`-32601`.

### Lifecycle / version / status

| Verb | Purpose |
|-|-|
| `GetVersion` | Version, `apiVersion`, `instanceId`, `ownerId`, `fpsTier`, `port`, and `capabilities` (`sourceTypes`, `filterTypes`, subscribable `events`, `audioOutput`). |
| `GetStatus` | Full instance snapshot: instance state, every source's detail, and `publishedSenderNames[]`. |
| `Shutdown` | Ask the instance to exit. `params.drain` (bool, default `true`): drain the pipeline then exit, vs. immediate clean exit. Idempotent: a repeat while already stopping re-acks the first committed `drain`. |

### Sources

| Verb | Purpose |
|-|-|
| `ListSources` | List current sources. |
| `ListSourceTypes` | List source types this instance can create. |
| `CreateSource` | Create a source (`type`, optional `displayName`, `settings`, `format`, `externalId`, `startBroadcast`). |
| `RemoveSource` | Remove a source by `sourceId`. |
| `SetSourceFormat` | Set a source's color format (re-attaches while broadcasting). |
| `SetSourceIdleMode` | Set per-source idle behavior (release-when-idle / disabled). |

### Properties / variants

| Verb | Purpose |
|-|-|
| `ListSourceProperties` | The source's editable property schema + current values. |
| `SetSourceProperties` | Apply a `settings` patch; echoes the applied keys. |
| `EnumerateSourceVariants` | Enumerate the selectable variants for a source type (e.g. available capture targets). |
| `InvokeSourceButton` | Invoke a button-type property on a source (or, with a `filterId`, on one of its filters). |

### Filters

| Verb | Purpose |
|-|-|
| `ListAvailableFilters` | Filter types addable to a source. |
| `ListFilters` | The source's current filter chain (vector order = apply order). |
| `AddFilter` | Add a filter (`filterType`, optional `name`, `settings`); mints a `filterId`. |
| `RemoveFilter` | Remove a filter by `filterId`. |
| `SetFilterEnabled` | Enable/disable a filter. |
| `ReorderFilter` | Move a filter to a new `index` in the chain. |
| `SetFilterName` | Rename a filter (display label). |
| `ListFilterProperties` | A filter's editable property schema + values. |
| `SetFilterProperties` | Apply a `settings` patch to a filter; echoes applied keys. |

### Audio

| Verb | Purpose |
|-|-|
| `ListAudioDevices` | Enumerate audio devices for a `flow` (input/output). |
| `SetAudioOutputDevice` | Select the instance's audio output device by `deviceId`. |
| `GetSourceAudio` | A source's audio state (gain / muted / balance / sync offset). |
| `SetSourceAudio` | Patch any subset of a source's audio fields; echoes the clamped values. |

### Broadcast

| Verb | Purpose |
|-|-|
| `StartBroadcast` | Begin emitting Spout senders for the instance's broadcastable sources. |
| `StopBroadcast` | Stop emitting senders. |

### Media

| Verb | Purpose |
|-|-|
| `GetMediaStatus` | Media transport status (`state`, `positionMs`, `durationMs`, `looping`) for a media-capable source. |
| `ControlMedia` | Transport action: `play` / `pause` / `restart` / `stop`. |
| `SeekMedia` | Seek to `positionMs`. |

Non-media sources answer media verbs with error `1012`.

### Profiles (standalone only)

| Verb | Purpose |
|-|-|
| `ListProfiles` | List saved profiles. |
| `LoadProfile` | Load a profile by `name` (rebuilds sources/filters/audio; may re-tier fps). |
| `SaveProfile` | Save the current scene as `name`. |
| `DeleteProfile` | Delete a profile by `name`. |

Profiles are a standalone-mode feature. When the instance is **managed** (an
`ownerId` is present), all four verbs reject with error **`1013`**
("Profiles are unavailable in managed mode"). They are not absent -- the verb
exists but is unavailable in this mode.

### Subscription

| Verb | Purpose |
|-|-|
| `Subscribe` | Subscribe this connection to one or more `events`. Reply: `{ "subscribed": [...] }`. Names the instance does not support are silently omitted from the ack (capability discovery). |
| `Unsubscribe` | Unsubscribe `events`. Reply: `{ "unsubscribed": [...] }`. |

`Subscribe`/`Unsubscribe` take `params.events` (a non-empty string array) and are
answered immediately on the connection without a main-loop round trip.
Subscriptions are **per-connection** and are lost when the connection drops; a
reconnecting client must re-subscribe and re-snapshot via `GetStatus`.

---

## Events

Events are **opt-in per connection** via `Subscribe`, with one exception
(`instanceShuttingDown`, always delivered). Every event `data` carries
`instanceId` and `ts`.

### Subscribable events

| Event | Fires when |
|-|-|
| `status` | Periodic instance snapshot (~1 s cadence) -- only while subscribed. |
| `audioLevels` | Periodic audio meters (~100 ms cadence) -- only while subscribed; master peak/rms/clipped + per-source levels, framed "since the previous emit". |
| `sourceAdded` | A source was created. |
| `sourceRemoved` | A source was removed. |
| `senderNameResolved` | A source's actual Spout sender name resolved (post-attach read-back). |
| `broadcastChanged` | Broadcast state changed (instance-wide or per source). |
| `propertyChanged` | Source or filter properties were applied. |
| `filterAdded` | A filter was added. |
| `filterRemoved` | A filter was removed. |
| `filterChanged` | A filter was enabled/disabled, reordered, or renamed. |
| `audioChanged` | A source's audio (gain/muted/balance/sync) changed. |
| `mediaChanged` | A media source's transport state changed. |
| `sourceIdleModeChanged` | A source's idle mode changed. |
| `devicesChanged` | The set of present video-capture devices changed (plug/unplug). `data.added` / `data.removed` list `{id, name}`. A device arrival also auto-recovers any source waiting on that device. Additive; older helpers never emit it. |

The exact subscribable set is also reported live by
`GetVersion` -> `capabilities.events`.

### Ungated event

| Event | Fires when |
|-|-|
| `instanceShuttingDown` | The instance is tearing down. Sent to **all** connected clients regardless of subscription, before the socket server stops. `data.reason` is `shutdown` / `restart` / `fault`. |

---

## Error and close codes

### Reply error codes (`error.code` in a reply envelope)

Protocol-level (JSON-RPC style):

| Code | Meaning |
|-|-|
| -32700 | Parse error (invalid JSON) |
| -32600 | Invalid request (not an object / missing string `method`) |
| -32601 | Method not found (unknown verb) |
| -32602 | Invalid params |
| 1009 | Instance is shutting down (request arrived after stop began) |

Application-level (the verb ran but failed):

| Code | Meaning |
|-|-|
| 1001 | Source not found |
| 1002 | Filter not found |
| 1003 | Source type unavailable |
| 1004 | Filter type unavailable |
| 1005 | Device busy |
| 1006 | Invalid property value |
| 1007 | Not implemented (in this context) |
| 1008 | Not ready (defined in the contract; not emitted in this build) |
| 1010 | Source creation failed |
| 1011 | Broadcast state conflict (e.g. `SetSourceFormat` re-attach failure) |
| 1012 | Media transport not supported by this source |
| 1013 | Profiles unavailable (managed mode) |
| 1014 | Profile invalid |
| 1015 | Profile not found |

Application error `data` (when present) carries a `category` and the relevant id
(e.g. `sourceId`), per the contract.

### WebSocket close codes (upgrade handshake)

| Code | Meaning |
|-|-|
| 4401 | Unauthorized (missing/wrong token) |
| 4403 | Origin not allowed |

Note that `1009` above is a **reply error code**, not a WebSocket close code: a
shutting-down instance answers in-flight requests with `error 1009` rather than
closing the socket out from under them.

---

## See also

- [`docs/control-api.asyncapi.yaml`](control-api.asyncapi.yaml) -- canonical wire
  contract (source of truth).
- [`examples/contract-test/`](../examples/contract-test/) -- reference client +
  conformance suite.
- [`docs/cli.md`](cli.md) -- command-line flags, including discovery and managed
  launch options.
