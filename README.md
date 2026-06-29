# MoxRelay

Capture and render Windows video sources and share each one with other applications as a
GPU texture over **Spout**.

## What it is

MoxRelay is a small, standalone Windows app for capturing live video and handing it to other
software on the same machine. It can capture webcams, capture cards, individual windows, whole
displays, and games, apply per-source filters, mix audio, and publish each source as its own
GPU-shared **Spout** sender that any Spout-aware receiver can pick up with no copy back through
system memory.

It is designed to be lightweight and scriptable: alongside the normal GUI it exposes a small
local control API (a WebSocket) so another program on the same machine can drive it. Launch
`moxrelay.exe` with no arguments for a normal GUI run. A full command-line reference, covering
the headless self-test and perf modes and the host-managed launch flags, ships in the package's
`docs/` folder (`docs/cli.md`) and is also in the project source repository.

## Built with libobs

MoxRelay is built with **libobs**, the open-source capture, rendering, and media engine behind
OBS Studio. libobs and several of its bundled plugins do the heavy lifting for source capture,
compositing, filtering, and encoding.

MoxRelay is an independent project. **It is not affiliated with, endorsed by, or sponsored by
the OBS Project or OBS Studio.** "OBS" and "OBS Studio" are used here only to describe the
upstream engine MoxRelay is built on.

## No telemetry

**MoxRelay collects nothing and sends nothing.**

- **No telemetry.** It does not measure, record, or transmit how you use it.
- **No analytics.** There is no usage tracking of any kind.
- **No crash reporting.** Nothing about errors or crashes is sent anywhere.
- **No outbound connections of its own.** MoxRelay does not phone home, check for updates, or
  contact any remote server.

The only network listener MoxRelay opens is its local control API, and it is bound exclusively
to loopback (`127.0.0.1`). It is reachable only from the same machine and never accepts a
connection from the network. Origin checks further restrict it to loopback clients.

(Some bundled engine components inside the OBS runtime, such as the streaming-protocol
libraries, contain their own networking code. Those code paths are part of OBS Studio's media
stack and are not invoked by MoxRelay, which performs no streaming or remote networking.)

## Control & automation

For automation, MoxRelay exposes a small control API: a WebSocket bound exclusively to loopback
(`127.0.0.1`), authenticated on every launch with a fresh random token written to
`%APPDATA%/MoxRelay/helper-config.json`, and browser-fenced by origin checks so only
same-machine clients can reach it. See `docs/control-api.md` for the protocol.

## License

MoxRelay is licensed under the **GNU General Public License, version 3 or later
(GPL-3.0-or-later)**. The full license text is in the `LICENSE` file at the root of this
package. Because MoxRelay links GPL-licensed components (libobs, a GPL FFmpeg build, and x264),
the combined binary is conveyed as a whole under the GPL.

**Corresponding source (GPLv3 section 6(d)).** The complete corresponding source code for this
binary, including the public Git repository URL and the exact pinned upstream revision of every
GPL and LGPL component, is provided in `SOURCES.txt` in this package (next to this file). As
required by GPLv3 section 6(d), that corresponding source is available at no charge from the
project's public Git repository at <https://github.com/ToxMox/MoxRelay>, the same place this
binary is built from, and a three-year written-offer fallback is also included there.

## Credits and acknowledgements

MoxRelay stands on the work of these projects:

| Project | Role | License |
|-|-|-|
| OBS Studio / libobs | Capture, rendering, and media engine (and bundled plugins) | GPLv2-or-later |
| FFmpeg (GPL build) | Media decode/encode and filtering | GPLv2-or-later |
| x264 | H.264 video encoding | GPLv2-or-later |
| Qt 6 | Application GUI toolkit (dynamically linked) | LGPLv3 |
| Spout2 (Lynn Jarvis) | GPU texture sharing on Windows | BSD-2-Clause |

MoxRelay also statically links **IXWebSocket** (BSD-3-Clause) for its local control API and
**nlohmann/json** (MIT). Additional components bundled inside the OBS runtime are listed in
`THIRD-PARTY-NOTICES.txt`.

The Spout integration uses the **Spout2** library by Lynn Jarvis. Thank you to Lynn Jarvis and
the Spout community for making Spout, and to the OBS Project for libobs and OBS Studio.

## Support the projects MoxRelay is built on

MoxRelay would not exist without the open-source projects it stands on. If MoxRelay is useful to
you, please consider supporting them directly:

- **OBS Project** - https://obsproject.com/contribute
- **Spout** - https://spout.zeal.co/ (donations via the download flow)

## Building from source

MoxRelay is a Windows-only, x64 build using MSVC and CMake. Community builds are welcome under
the GPL. After running the bootstrap step, configure with `cmake --preset local` (the
bootstrap-generated user preset that fills in the machine-specific OBS and Qt paths; the bare
`cmake --preset msvc-x64` fatal-errors on empty OBS paths).

See docs/building.md (https://github.com/ToxMox/MoxRelay/blob/main/docs/building.md) for
building from source.

## Documentation

- **`docs/usage.md`** - using MoxRelay
- **`docs/spout-receiving.md`** - receiving Spout senders
- **`docs/control-api.md`** - control/automation API
- **`docs/control-api.asyncapi.yaml`** - machine-readable control-API contract
- **`docs/cli.md`** - command line
- **building** - https://github.com/ToxMox/MoxRelay/blob/main/docs/building.md (hosted-only, not shipped)

## Third-party notices

A complete index of every redistributed third-party component, its license, and the location of
its verbatim license text is in **`THIRD-PARTY-NOTICES.txt`**, with the full per-component
license texts under **`licenses/`**. The GPL corresponding-source written offer and the exact
pinned upstream revisions are in **`SOURCES.txt`**.
