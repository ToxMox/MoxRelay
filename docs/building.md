# Building MoxRelay from source

MoxRelay is a Windows-only, x64 application built with MSVC and CMake, on top of **libobs**
(the OBS Studio capture/render/media engine), **Qt 6**, and a GPL **FFmpeg** build. Community
builds are welcome under the GPL.

> This document lives in the project source repository and is for people building from source.
> It is **not** part of the distributed application ZIP.

## How the dependency wiring works

MoxRelay does not vendor libobs, Qt, or FFmpeg. It links an **out-of-tree obs-studio build**
plus the prebuilt **obs-deps** bundle that an obs-studio build downloads for itself:

- `libobs` headers + `obs.lib` / `obs.dll` and the OBS plugins come from a local obs-studio
  source clone and its build tree.
- **Qt 6** (Core, Gui, Widgets) is resolved from the obs-deps Qt6 bundle via
  `CMAKE_PREFIX_PATH`.
- A couple of **FFmpeg** import libs (`swresample`, `avutil`) are linked directly from the
  obs-deps FFmpeg bundle for the audio resampler.

The tracked configure preset `msvc-x64` (in `CMakePresets.json`) deliberately leaves the
machine-specific absolute paths **empty** and fails fast if they are unset. Those paths are
supplied by a **git-ignored** `CMakeUserPresets.json` that defines a `local` preset inheriting
`msvc-x64`:

| Cache variable | Points at |
|-|-|
| `OBS_SRC` | obs-studio source clone (`<...>/obs-studio`) |
| `OBS_BUILD` | obs-studio build tree (`<...>/obs-studio/build_x64`) |
| `MOXRELAY_OBS_RUNDIR` | obs build rundir (`.../build_x64/rundir/RelWithDebInfo`) |
| `CMAKE_PREFIX_PATH` | obs-deps Qt6 bundle (`.../.deps/obs-deps-qt6-2025-08-23-x64`) |

`CMakeUserPresets.json` is listed in `.gitignore`, so nothing machine-specific is ever
committed. The bootstrap script below generates it for you.

## Pinned dependency versions

The bootstrap is fully pinned and reproducible:

| Component | Pin | Source |
|-|-|-|
| obs-studio / libobs | tag `32.1.2` (commit `fb4d98bf88fae5fc85cb11fc57f7c5e309282194`) | `github.com/obsproject/obs-studio` |
| obs-deps (FFmpeg/x264/...) | `2025-08-23` (`obs-deps-2025-08-23-x64`) | downloaded + hash-verified by the obs-studio build |
| obs-deps Qt6 | `2025-08-23` (`obs-deps-qt6-2025-08-23-x64`) | downloaded + hash-verified by the obs-studio build |

The obs-deps and Qt6 versions are **pinned inside obs-studio@32.1.2's own `CMakePresets.json`**
(with SHA-256 hashes), so checking out the pinned obs-studio commit pins the dependency bundles
too. The same revisions are recorded for GPL corresponding-source compliance in
`packaging/SOURCES.txt`.

## Prerequisites

- **Windows x64.**
- **Visual Studio 2022** with the C++ x64 toolchain. The build pins the VS2022 `v143` toolset
  (generator `Visual Studio 17 2022`, architecture x64) and compiles as C++20.
- **CMake 3.28 or newer** (the presets declare schema `version 6`).
- **Git** (the bootstrap clones obs-studio and its submodules).
- Enough disk + time for a full obs-studio build (the OBS build and its downloaded dependency
  bundles are several GB).

A Qt 6 install and a libobs build are **not** prerequisites you provide yourself; the bootstrap
step produces them.

## 1. Bootstrap the pinned dependencies

From the repository root:

```powershell
pwsh -File scripts/bootstrap-deps.ps1
```

The script:

1. Clones **obs-studio** at the pinned tag/commit (with submodules) into a sibling
   `moxrelay-deps/obs-studio` folder next to your clone (override with `-DepsRoot <path>`).
2. Configures and builds obs-studio (`RelWithDebInfo`, x64). The obs-studio configure step
   downloads the pinned **obs-deps `2025-08-23`** FFmpeg and Qt6 bundles into `<obs>/.deps`,
   verifying them against the SHA-256 hashes pinned in obs-studio's `CMakePresets.json`, and the
   build populates `obs.lib`/`obs.dll`, the generated `obsconfig.h`, and the runtime directory.
3. Verifies every artifact the MoxRelay build references and fails loudly on any miss.
4. Writes the git-ignored `CMakeUserPresets.json` with the `local` preset (real absolute paths)
   plus a `clangd` preset.

Useful switches:

- `-DepsRoot <path>` -- put the obs-studio tree somewhere else.
- `-NoBrowser` -- configure obs-studio with the browser source OFF (skips the large CEF
  download and shortens the OBS build; MoxRelay does not use the OBS browser source).
- `-SkipObsBuild` -- only (re)generate `CMakeUserPresets.json` against an already-built tree.
- `-Force` -- overwrite an existing `CMakeUserPresets.json` (otherwise it is left untouched).
- `-CheckLatest` -- report-only: query GitHub for the latest stable obs-studio release and nudge if it is newer than the pin; the default (flag absent) always builds the pinned versions.

## 2. Configure and build MoxRelay

From the repository root:

```powershell
cmake --preset local
cmake --build --preset msvc-x64
```

`cmake --preset local` configures with the Visual Studio 17 2022 generator into `build/`, using
the absolute paths from `CMakeUserPresets.json`. `cmake --build --preset msvc-x64` builds the
`RelWithDebInfo` configuration in that same `build/` tree (`cmake --build --preset local` is
equivalent). The resulting binary is `build/RelWithDebInfo/moxrelay.exe`, with its required
runtime DLLs (Qt, obs.dll + load-time deps, the obs `data/` and `obs-plugins/` trees, and the
MSVC runtime) copied next to it by a post-build step, so it launches with no extra `PATH` setup.

## 3. Package the portable ZIP (optional)

To produce the same self-contained distribution ZIP that ships the application:

```powershell
cpack -G ZIP -C RelWithDebInfo --config build/CPackConfig.cmake
```

The archive is written to `build/dist/` and extracts to one clean `MoxRelay/` folder containing
the executable, its runtime DLLs, and the compliance set (LICENSE, README, SOURCES.txt,
THIRD-PARTY-NOTICES.txt, and the per-component `licenses/` texts).

## Notes and troubleshooting

- **`OBS_SRC and OBS_BUILD must be set` at configure time** means you configured `msvc-x64`
  directly instead of `local`, or `CMakeUserPresets.json` is missing. Run the bootstrap, then
  `cmake --preset local`.
- **`CMakeUserPresets.json` is git-ignored** on purpose -- it holds absolute machine paths. Do
  not commit it; re-run the bootstrap on each machine.
- **clangd (optional).** The tracked `clangd-base` preset plus the generated `clangd` preset
  emit `cmake-build-clangd/compile_commands.json` for the `.clangd` config. Run
  `cmake --preset clangd` from a Visual Studio x64 developer shell after adding/removing source
  files. This sidecar is configure-only and is never built.
- **MoxRelay is not affiliated with the OBS Project.** "OBS" / "OBS Studio" are referenced only
  to describe the upstream engine MoxRelay builds on.
