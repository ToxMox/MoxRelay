<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# MoxRelay application icons

Source-of-truth and derived assets for the app/taskbar/tray icon.

| File | Role | Consumed by |
|-|-|-|
| `moxrelay.svg` | Vector master (artwork source of truth) | none at build time; regeneration source |
| `moxrelay.ico` | Windows multi-res icon (16/32/48 + 256) | `moxrelay.rc` -> `rc.exe` -> embedded in `moxrelay.exe` |
| `moxrelay.rc` | Win32 resource script (`1 ICON "moxrelay.ico"`) | `CMakeLists.txt` (VS-generator-gated `target_sources`) |
| `moxrelay-{16,32,48,64,128,256}.png` | Runtime `QIcon` sizes | `resources/resources.qrc` (`:/icons/moxrelay-*.png`), via AUTORCC |

## Why this split

- The **`.ico`** becomes the executable's own icon (Explorer / taskbar / Alt-Tab) via the Win32 `.rc`
  resource. The `.rc` resolves `moxrelay.ico` relative to its own directory, so the two MUST stay
  co-located in this folder.
- The **PNGs** drive the runtime `QApplication::setWindowIcon` and the system-tray icon. PNG is a
  built-in QtGui image handler, so they load with no `imageformats` plugin deployed (`DeployRuntime`
  ships only the `platforms` plugin). The `.ico`/`.svg` masters are deliberately NOT embedded in the
  Qt resource bundle.

## Regenerating from the master

If the artwork changes, update `moxrelay.svg`, rebuild `moxrelay.ico` from it (any icon tool;
keep the 16/32/48/256 sizes), then regenerate the PNGs from the `.ico` so every surface stays in
sync. The PNGs here were produced with Python + Pillow:

```python
from PIL import Image
ico = Image.open("moxrelay.ico")
for s in (16, 32, 48, 256):                 # native frames straight from the .ico
    ico.ico.getimage((s, s)).convert("RGBA").save(f"moxrelay-{s}.png")
big = ico.ico.getimage((256, 256)).convert("RGBA")
for s in (64, 128):                          # intermediate high-DPI sizes, Lanczos-downscaled
    big.resize((s, s), Image.LANCZOS).save(f"moxrelay-{s}.png")
```

After regenerating, a normal CMake build re-runs AUTORCC (PNGs) and the RC compiler (`.ico`); no
CMake edits are needed unless sizes are added/removed (then update `resources/resources.qrc` and the
size list in `moxRelayAppIcon()` in `src/main.cpp`).
