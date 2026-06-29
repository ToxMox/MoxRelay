# MoxRelay command-line reference

MoxRelay is a self-contained screen-capture / broadcast helper. It runs in one of
three modes, selected by the presence of a mode flag:

| Mode | Selected by | Purpose |
|-|-|-|
| GUI / managed run | (default, no mode flag) | Normal interactive or host-managed run. |
| Self-test | `--selftest` | Headless dev/QA gate harness; exits 0/1/2. |
| Perf | `--perf` | Headless dev/benchmark harness. |

Parsing is **strict**. See [Parsing rules](#parsing-rules) below: unknown flags and
stray positional arguments are hard errors, and mode-scoped flags are rejected
outside their mode.

Numeric values are validated after parsing; the accepted range is noted in the
Notes column for each option.

---

## GUI / managed run (default)

Default mode when neither `--selftest` nor `--perf` is given. A single GUI instance
launches. With no flags, behaviour is unchanged from a plain double-click launch.

| Flag | Value | Default | Description | Notes |
|-|-|-|-|-|
| `--start-minimized` | (none) | off | Start hidden in the system tray (minimized window when no tray is available). | Boolean switch. |
| `--owner-id <token>` | token | unowned | Opaque owner token advertised at the process level (GetVersion + helper-config.json); absent = unowned. | Managed-mode flag. |
| `--client-name <name>` | name | falls back to `--owner-id` | Optional managed display name shown in the window title and tray tooltip; absent = fall back to `--owner-id`. | Managed-mode flag. |
| `--owner-pid <N>` | integer | not watching | Exit automatically when the process with this PID terminates (managed mode). | Integer >= 1. Managed-mode flag. |
| `--discovery-path <file>` | path | `%APPDATA%/MoxRelay/helper-config.json` | Explicit discovery-file path to write helper-config.json. | Mutually exclusive with `--rendezvous-pipe`. |
| `--rendezvous-pipe <name>` | name | (write discovery file) | Write the instance over this named pipe instead of the discovery file. | Mutually exclusive with `--discovery-path`. Managed-mode flag. |
| `--port <N>` | integer | 7341 | Requested control-port base; the resolver auto-falls-back if it is busy. | Integer >= 1. |
| `--fps-tier <N>` | integer | (instance global) | This instance's global fps tier. | Integer >= 1. Also valid in `--perf`. |
| `--rundir <path>` | path | exe-relative (`data/libobs` beside the exe) | Absolute libobs runtime tree. | Required if that path is absent. Valid in all modes. |
| `--adapter <N>` | integer | 0 (primary) | GPU adapter index for the rendering device. | Integer >= 0. Valid in all libobs-initializing modes (GUI/managed, `--selftest`, `--perf`) -- same all-modes treatment as `--rundir`. |
| `-h`, `--help` | (none) | -- | Show the help text and exit. | Valid in all modes. |

### Managed-mode flags (host integration)

The following flags are **only meaningful when the helper is launched by a host
application** that manages the MoxRelay process. They are inert for a normal
standalone launch:

- `--owner-id <token>` -- the opaque owner token the host advertises for this instance.
- `--client-name <name>` -- the managed display name the host wants shown in the title / tray.
- `--owner-pid <N>` -- the host process PID; MoxRelay self-exits when that PID terminates.
- `--rendezvous-pipe <name>` -- the named pipe the host listens on for the instance handoff (an alternative to the on-disk discovery file).

A host launches MoxRelay in exactly one handoff transport: the discovery file
(`--discovery-path`) **or** the rendezvous pipe (`--rendezvous-pipe`), never both.

Either transport publishes the instance's loopback control port **and** a
per-launch `controlToken`. The host then drives the running instance over its
control endpoint (`ws://127.0.0.1:{port}/control?token=<controlToken>`); see
[Control API](control-api.md) for the discovery file, the token handshake, and
the verb/event protocol. Note that the profile verbs are standalone-only and are
rejected once an `--owner-id` makes the instance managed.

---

## Self-test mode (`--selftest`)

`--selftest` runs the headless self-test gates and exits 0/1/2. This is the dev/QA
gate harness. Only the flags below (plus `--rundir` and `-h`/`--help`) are valid
alongside `--selftest`.

| Flag | Value | Default | Description | Notes |
|-|-|-|-|-|
| `--selftest` | (none) | -- | Run the headless self-test gates and exit 0/1/2. | Mode flag. Mutually exclusive with `--perf`. |
| `--gates <list>` | comma-separated list | full suite | Run only the named gates (valid: `a, b, c, d, d2, e, f, g, h, i, j`; `a` and `b` always run -- they assert the engine boot itself). | Unknown gate names are rejected. |
| `--reps <N>` | integer | 1 | Repeat the pass N times in sequential child processes, stopping at the first failing pass. | Integer >= 1. |
| `--hold <N>` | integer | 0 (no hold) | Keep the gate senders open N seconds for an external receiver. | Integer >= 1. Requires gate `d` when `--gates` is given (gate d owns the hold window). |

---

## Perf mode (`--perf`)

`--perf` runs the headless perf harness: one process per (tier, resolution,
flush-mode) cell, ramping synthetic sources/senders at a single fps tier to find the
per-instance ceiling. This is a dev/benchmark harness. `--fps-tier` is **required**
in this mode (fps can never change in-process).

| Flag | Value | Default | Description | Notes |
|-|-|-|-|-|
| `--perf` | (none) | -- | Run the headless perf harness (one process per tier/resolution/flush-mode cell). | Mode flag. Mutually exclusive with `--selftest`. |
| `--fps-tier <N>` | integer | -- (required) | This instance's global fps tier. | Integer >= 1. **Required** with `--perf`. |
| `--width <W>` | integer | 1920 | Synthetic source width. | Integer >= 16. |
| `--height <H>` | integer | 1080 | Synthetic source height. | Integer >= 16. |
| `--max-n <N>` | integer | 64 | Ramp cap on sender count. | Integer >= 1. |
| `--flush-mode <mode>` | `batched` \| `immediate` | `batched` | `batched` (one Flush per frame; product default) or `immediate` (per-sender Flush; A/B comparison). | Only these two values are accepted. |
| `--out <file>` | path | (none) | Also append the JSONL lines to this file. | |
| `--settle-sec <S>` | integer | 2 | Settle seconds after each attach. | Integer >= 0. |
| `--measure-sec <S>` | integer | 5 | Measure-window seconds per step. | Integer >= 1. |
| `--budget-pct <P>` | number | 90 | Ceiling when avg frame time exceeds this % of the frame budget. | Number > 0. |
| `--lagged-pct <P>` | number | 1 | Ceiling when windowed lagged/total exceeds this %. | Number > 0. |
| `--rundir <path>` | path | exe-relative (`data/libobs` beside the exe) | Absolute libobs runtime tree. | Required if that path is absent. Valid in all modes. |
| `--adapter <N>` | integer | 0 (primary) | GPU adapter index for the rendering device. | Integer >= 0. Valid in all modes. |

---

## Parsing rules

The command line is parsed strictly. A launch whose arguments were mangled fails
loudly rather than silently opening a window:

- **Unknown flags and stray positional arguments are hard errors.** The process
  prints the error and exits; it never falls through to the GUI.
- **`--selftest` and `--perf` are mutually exclusive.** Passing both is an error.
- **Mode-scoped flags are rejected outside their mode.** `--hold`, `--gates`, and
  `--reps` require `--selftest`. The perf knobs (`--width`, `--height`, `--max-n`,
  `--flush-mode`, `--out`, `--settle-sec`, `--measure-sec`, `--budget-pct`,
  `--lagged-pct`) require `--perf`. The managed/GUI flags (`--start-minimized`,
  `--owner-id`, `--discovery-path`, `--rendezvous-pipe`, `--port`, `--owner-pid`)
  are rejected with both `--selftest` and `--perf`. `--fps-tier` is allowed only in
  GUI and `--perf` (rejected with `--selftest`).
- **`--discovery-path` and `--rendezvous-pipe` are mutually exclusive.** A managed
  launch picks exactly one handoff transport.
- **Numeric options must parse and satisfy their range** (see the per-flag Notes);
  out-of-range or non-numeric values are errors.

---

## See also

- [Control API](control-api.md) -- the loopback WebSocket control protocol a host
  or tool uses to drive a running instance (discovery file, per-launch token,
  verbs, and events).
- [`docs/control-api.asyncapi.yaml`](control-api.asyncapi.yaml) -- the canonical
  machine-readable wire contract.
