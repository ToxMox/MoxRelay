# Using MoxRelay

MoxRelay captures live video and audio sources on your Windows machine and publishes
each one as its own GPU-shared **Spout** sender, so any Spout-aware application on the
same PC can pick them up with no copy back through system memory. This guide walks
through the graphical app: adding sources, configuring them, mixing audio, and
broadcasting to Spout.

For a companion guide aimed at the receiving side (how another app finds and decodes
your senders), see `docs/spout-receiving.md`. For the command line, see `docs/cli.md`.

## The window at a glance

When you launch `moxrelay.exe` with no arguments you get the normal GUI:

- **Preview (center).** A live preview of the **currently selected source**. The
  preview always shows exactly one source: whichever row is selected in the Sources
  list. Selecting a different source switches the preview to it.
- **Sources (left dock).** The list of sources you have added. Selection here drives
  everything else in the window.
- **Properties (right dock).** Settings for the selected source: a sender-format
  picker, a media transport strip (media sources only), an audio strip
  (audio-bearing sources), and a settings form built from that source's own
  properties.
- **Filters (right dock, below Properties).** The selected source's filter chain.
- **Toolbar (top).** Add Source, Add Filter, Start/Stop Broadcast, the instance audio
  output device and master meter, and (in standalone runs) the Profiles controls.
- **Status bar (bottom).** Preview resolution and frame rate, the number of active
  Spout senders and the first sender's name, and the local control URL.

### Mental model: a per-source publisher, not a scene compositor

This is the most important thing to understand. MoxRelay is **not** an OBS-style scene
compositor. It does **not** stack your sources into a single composited canvas. Each
source you add is an **independent** capture that becomes its **own** Spout sender.
The preview pane is just a viewer for the one source you have selected; it is not the
"program output." When you broadcast, every eligible source publishes separately, and
a receiver subscribes to each sender by name. If you want sources combined, you
compose them on the receiving side.

## Adding a source

Click **Add Source** on the toolbar. Pick a **Type**, optionally give it a **Name**
(leave it blank to accept a generated name), and click OK. The new source appears in
the Sources list and is selected for you.

The type dropdown lists ten source types. They are shown by their wire identifiers;
the friendly meaning of each is:

| Dropdown id | Friendly name | What it captures |
|-|-|-|
| `camera` | Camera | A webcam or capture-card video device |
| `display` | Display | A whole monitor (defaults to the primary display) |
| `window` | Window | A single application window |
| `game` | Game | An accelerated game capture |
| `media` | Media | A media file or stream (with playback transport) |
| `image` | Image | A still image |
| `color` | Color | A solid color generator |
| `text` | Text | Rendered text |
| `audio_input` | Audio Input | An audio input device (microphone/line-in) |
| `audio_output` | Audio Output | System audio output (loopback capture) |

**Capture sources start blank.** Camera, Display, and Window do not grab anything
until you choose a specific target (a device, a display, or a window) in the
Properties form. Until then they stay inert and publish nothing. The other types
capture safely with their built-in defaults.

**Audio Input and Audio Output are audio-only.** They have no video and never create
a Spout sender. Their rows are tagged `[..., audio]` in the Sources list.

To remove a source, right-click it in the Sources list and choose **Remove Source**.

## Configuring a source

Select a source and use the **Properties** dock. The settings form is generated from
the source's own property list, so it shows exactly the controls that source supports
(device pickers, resolution/FPS, file paths, color, text, and so on). Edits apply as
you make them.

### Per-source Spout pixel format

At the top of the Properties dock is a **Format** picker. This sets the pixel format
of that source's Spout sender. It matters because the receiver has to decode the
pixels correctly.

| Format | What it is | Use it when the receiver... |
|-|-|-|
| `srgb87` (default) | 8-bit BGRA (DXGI 87) carrying sRGB-encoded bytes | decodes sRGB once on import (the common case) |
| `linear87` | The same 8-bit BGRA container, but carrying linear bytes | performs no sRGB decode; an sRGB-decoding receiver will render this dark/wrong by design |
| `fp16` | 16-bit float RGBA (DXGI 10), linear half-float | wants linear/HDR precision |

All three carry alpha. If you change the format while the source is broadcasting, its
sender is briefly torn down and re-created under the new format, so receivers see the
name disappear and then resolve again. See `docs/spout-receiving.md` for the receiver
side of this.

## Media transport

When a **Media** source is selected, a transport strip appears above the settings
form:

- **Play / Pause** toggles playback.
- **Restart** jumps back to the start and plays.
- **Stop** halts and resets the position.
- **Loop** (a toggle) makes a non-live clip repeat.
- The **seek slider** scrubs through the clip; drag it to jump to a position. Files
  with a known duration show `position / total`; live inputs show `position / live`
  and cannot be scrubbed.

## Filters

Each source has its **own** filter chain. Select a source, then use the **Filters**
dock (or the **Add Filter** toolbar button) to add, remove, reorder, enable/disable,
and rename filters. Each filter has its own settings, shown when you select it.

There are 22 filter types: **13 video** and **9 audio**. Audio filters act on a
source's audio; video filters act on its picture.

**Video (13):** Color Correction, Chroma Key, Color Key, Luma Key, Crop/Pad,
Image Mask/Blend, Apply LUT, Scaling/Aspect Ratio, Scroll, Sharpen, Render Delay,
Video Delay (Async), HDR Tone Mapping.

**Audio (9):** Gain, Compressor, Upward Compressor, Expander, Limiter, Noise Gate,
Noise Suppression, 3-Band Equalizer, Invert Polarity.

## Audio

### Per-source audio

When an audio-bearing source (Media, Audio Input, Audio Output) is selected, the
audio strip in the Properties dock gives you:

- **Gain** fader, from silence (`-inf dB`) up to about `+26 dB`.
- **Mute** checkbox.
- **Balance** slider, centered at the middle.
- **Sync Offset**, 0 to 950 ms. This is **positive-only**: it can only delay the
  audio (play it later) to line it up with the picture. To make the audio land
  *earlier* instead, delay the video with a Render Delay or Video Delay filter.
- A per-source **level meter** (post gain/mute).

### Instance audio output

The toolbar carries this instance's master audio: an **output device** selector (where
the mixed audio is played) and a **master meter** (the red edge lights on a clipped
peak).

## Broadcasting to Spout

Publishing is **instance-wide**. Click **Start Broadcast** on the toolbar (or press
**Ctrl+B**) to begin emitting; the button becomes **Stop Broadcast**. Every eligible
source then publishes as its own Spout sender. Audio-only sources publish no sender
(they have no picture to share). A source's sender name is shown in the status bar and
becomes available to receivers once it has sent its first frame.

For exactly how receivers find and bind to your senders, see `docs/spout-receiving.md`.

## App Settings

Open **Tools > Settings**:

- **Tray.** Minimize to tray, Close to tray (the window X hides to the tray instead of
  quitting), and Start minimized.
- **Logging.** Log level (error / warning / info / debug), Write logs to file, and a
  read-only Log folder path with an **Open log folder** button.
- **Rendering GPU** (standalone runs only). Which GPU the engine renders on.
  **Changing it requires a restart.**
- **Max Spout senders.** The cap on how many senders can be published at once
  (default **64**). Raise it to publish many sources simultaneously.
  **Changing it requires a restart.** Settings offers to restart for you when either
  restart-required value changes.
- **Control URL** and **Auth** (both read-only). The loopback address of the local
  control API and whether authentication is enabled.

## Profiles (standalone)

In standalone runs the toolbar shows a Profiles group. A profile saves your whole
setup: sources, their filters, audio, and the frame-rate tier.

- **FPS** dropdown sets the per-profile frame rate (24, 30, 48, 60, 120, 144, or
  240). Changing it re-tiers the running engine immediately.
- **Profile** dropdown selects the active profile. An asterisk marks unsaved changes.
- **Profile Actions** menu: Save, Save As, Rename, Duplicate, Delete, Import, and
  Export. Profile names cannot contain path separators or the characters
  `/ \ : * ? " < > |`.

## System tray

When Windows has a system tray, MoxRelay keeps a tray icon. Double-click it (or use
**Show MoxRelay** from its menu) to restore a hidden or minimized window. **Quit
MoxRelay** in the tray menu ends the app. Whether closing or minimizing the window
hides it to the tray is controlled by the tray options in App Settings.
