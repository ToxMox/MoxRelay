// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ObsBootstrap -- Qt-free, in-process libobs bootstrap in the corrected boot order.
// libobs is a per-PROCESS singleton; this owns the once-per-process startup/shutdown (obs_startup /
// obs_reset_audio / module paths run exactly once per process). No QApplication is involved in the
// boot path (proven by obs-studio-node's server). M2.3 (R1): the boot is parameterized via
// BootstrapOptions so each process boots at its OWN initial fps. The fps tier is NOT immutable:
// obs_reset_video can be re-invoked in-process to re-tier live (item 05, single instance) via
// retier() -- exactly what standard OBS does on every video-settings apply -- so a new tier is no
// longer necessarily a new process.

#pragma once

#include <cstdint>
#include <string>

namespace moxrelay {

// The libobs per-module config BASE: an absolute, MACHINE-LOCAL, UNBRANDED %LOCALAPPDATA%/MoxRelay
// (FOLDERID_LocalAppData, forward-slash normalized, NO trailing slash, NO file segment). libobs
// appends /<module>/<file> itself. Does NOT create the directory. Shared so other seams (e.g. the
// log sink) reuse the SAME LocalAppData root resolution rather than duplicating the known-folder
// logic. Returns "MoxRelay" (relative) only if the known folder cannot be resolved.
std::string moduleConfigDir();

// Log-level threshold for the libobs log sink (mox_log_handler). libobs levels are NUMERIC with
// LOWER == MORE SEVERE (util/base.h: LOG_ERROR=100, LOG_WARNING=200, LOG_INFO=300, LOG_DEBUG=400).
// The handler emits a line only when its level is <= the threshold, so a smaller threshold is
// quieter. Map a settings string (error/warning/info/debug) to that numeric threshold; an
// unrecognized string maps to LOG_INFO (the default). Call setLogLevelThreshold ONCE at startup
// (run_gui) from the resolved log/level QSetting. Backed by an atomic<int>; thread-safe to read
// from the libobs log thread. Default threshold = LOG_DEBUG (emit everything) until set.
int logLevelThresholdFor(const std::string &level);
void setLogLevelThreshold(int threshold);

// R1: the per-process boot parameters. A default-constructed BootstrapOptions reproduces the M1
// boot exactly (1920x1080 @ 60, adapter 0, compiled-in/exe-relative rundir).
struct BootstrapOptions {
	int fpsNum = 60;       // this process's GLOBAL fps tier (numerator)
	int fpsDen = 1;
	int baseWidth = 1920;  // canvas size (per-source senders render at SOURCE size regardless)
	int baseHeight = 1080;
	int outputWidth = 0;   // 0 = match baseWidth
	int outputHeight = 0;  // 0 = match baseHeight
	int adapter = 0;       // D3D11 adapter index -- MUST match the consuming application's GPU
	std::string rundir;    // empty = resolveRundir() default (exe-relative, then compiled-in)

	// Test/gate override for the libobs per-module config base (obs->module_config_path).
	// Empty = use the production default (%LOCALAPPDATA%/MoxRelay). Gates set this to a temp dir so a
	// selftest run never writes into the real machine-local app-data.
	std::string moduleConfigDirOverride;   // empty = production default
};

// Result of a bootstrap attempt. Gate A = libobs found its core data (default.effect resolves).
// Gate B = zero module-load failures (obs_module_failure_info.count == 0).
struct BootstrapResult {
	bool   started      = false; // obs_startup + obs_reset_video + obs_reset_audio all succeeded
	bool   gateA        = false; // obs_find_data_file("default.effect") != nullptr
	bool   gateB        = false; // module failure count == 0
	size_t moduleFailures = 0;   // obs_module_failure_info.count
	int    modulesLoaded  = 0;   // count from obs_enum_modules (LOADED modules)

	// L9 (R1): the ACHIEVED video parameters, read back via obs_get_video_info after
	// obs_reset_video -- consumers (worker READY line, perf harness) record these rather than
	// trusting the request.
	uint32_t achievedFpsNum = 0;
	uint32_t achievedFpsDen = 0;
	uint32_t achievedBaseWidth = 0;
	uint32_t achievedBaseHeight = 0;
	uint32_t achievedOutputWidth = 0;
	uint32_t achievedOutputHeight = 0;
	int      achievedAdapter = -1;

	std::string rundirUsed; // the rundir the boot actually ran against (after resolution)
};

class ObsBootstrap {
public:
	// Item 05: the narrow input for a live re-tier -- only the new frame rate plus the CURRENT canvas /
	// output dims (so the re-run ovi keeps the same resolution). Adapter / rundir are fixed for the
	// process lifetime and deliberately NOT threaded here (obs_reset_video keeps the existing D3D11
	// device, so the adapter field is inert on a re-tier). Nested in the class so the retier() member
	// and its parameter/return types share one scope (no namespace-scope decl/def mismatch).
	struct RetierOptions {
		int fpsNum = 60;
		int fpsDen = 1;
		int baseWidth = 1920;
		int baseHeight = 1080;
		int outputWidth = 0;   // 0 = match baseWidth
		int outputHeight = 0;  // 0 = match baseHeight
	};

	// Item 05: the result of a live re-tier. `ok` is the obs_reset_video success; the achieved fps is
	// read back via obs_get_video_info so the caller reports the achieved tier, never the request.
	struct RetierResult {
		bool     ok = false;
		uint32_t achievedFpsNum = 0;
		uint32_t achievedFpsDen = 0;
	};

	// Boots libobs against an absolute runtime tree (rundir/<cfg>): expects
	//   <rundir>/data/libobs        (core data; default.effect lives here)
	//   <rundir>/obs-plugins/64bit  (plugin DLLs)
	//   <rundir>/data/obs-plugins   (per-module data, %module% substituted)
	// Returns the gate results; on a hard failure `started` is false.
	static BootstrapResult startup(const BootstrapOptions &options = BootstrapOptions());

	// Item 05: live in-process re-tier -- re-runs obs_reset_video with a fresh ovi (new fps) WITHOUT
	// re-running obs_startup / obs_reset_audio / the module paths. The CALLER must have removed the
	// render callback and drained ALL wait=true graphics-thread teardown FIRST (obs_reset_video joins
	// the graphics thread; a wait=true graphics task queued afterwards hangs forever). MUST be called
	// from the control/Qt/main thread, never the graphics thread (self-join deadlock). Returns ok =
	// the obs_reset_video success + the achieved fps. No-op-refuses (ok=false) if libobs is not up.
	static RetierResult retier(const RetierOptions &options);

	// Tears libobs down (obs_shutdown). R3: idempotent -- safe after a failed startup and safe to
	// call twice (obs_shutdown itself has no guard for either case).
	static void shutdown();

	// R2: resolve the runtime tree, in priority order:
	//   1. `explicitRundir` (--rundir / BootstrapOptions.rundir) when non-empty;
	//   2. the EXE-RELATIVE tree (<exeDir> containing data/libobs) -- the relocatable install
	//      layout, where the deployed exe sits at the root of its own rundir.
	// There is no compiled-in fallback. Returns "" if neither resolves.
	static std::string resolveRundir(const std::string &explicitRundir = std::string());

	// Always returns "" -- there is no compiled-in runtime tree. Kept only as the final
	// fall-through for resolveRundir.
	static std::string defaultRundir();
};

} // namespace moxrelay
