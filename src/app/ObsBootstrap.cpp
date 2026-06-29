// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ObsBootstrap implementation -- the corrected libobs boot order, replicating the project's proven
// headless-boot pattern verbatim (no new pattern invented):
//   base_set_log_handler FIRST -> ABSOLUTE obs_add_data_path WITH trailing slash (BEFORE startup;
//   the missing-slash gotcha -> default.effect miss / silent black frames) -> obs_startup (the
//   once-per-process singleton; libobs does its own COM init) -> obs_reset_video (hand-filled standard
//   obs_video_info; obs_create_video_info is Streamlabs-fork-only) -> obs_reset_audio ONCE
//   (FLOAT_PLANAR is forced internally, not a caller field) -> ABSOLUTE obs_add_module_path AFTER
//   startup (it early-returns if obs == NULL) -> obs_load_all_modules2(&mfi) + obs_post_load_modules().
// Gate A = obs_find_data_file("default.effect") != NULL. Gate B = mfi.count == 0 (OUT param; freed
// with obs_module_failure_info_free).

#include "ObsBootstrap.hpp"

#include <obs.h>
#include <util/base.h>
#include <util/bmem.h>

#include <atomic>
#include <chrono> // B0: system_clock epoch-ms stamp on every libobs log line
#include <cstdarg>
#include <cstdio>
#include <cstdlib> // _dupenv_s -- LOCALAPPDATA fallback when SHGetKnownFolderPath is unavailable

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace moxrelay {

namespace {

// Item 04: the log-level threshold. libobs levels are NUMERIC, LOWER == MORE SEVERE; a line is
// emitted only when its level is <= this threshold. Default = LOG_DEBUG (emit everything) so the
// channel behaves exactly as before until run_gui sets the resolved log/level. atomic so the libobs
// log thread reads it without a lock; set once at startup.
std::atomic<int> g_logLevelThreshold{LOG_DEBUG};

// libobs hands the log callback an UNFORMATTED format string + va_list (util/base.h), so we must
// vsnprintf it ourselves. This is the channel that surfaces libobs's own
// "Failed to find file 'default.effect'" error and any module-load detail.
//
// Sink = stdout, NOT stderr. Under /SUBSYSTEM:WINDOWS (the no-console build, item 03) the CRT marks
// stderr as _NO_CONSOLE_FILENO (_fileno == -2) and silently DROPS every fprintf(stderr) write -- and
// it cannot be repaired by fd tricks (freopen on it __fastfail's; _dup2 onto fd 2 does not reattach
// the FILE*). The LogSink redirects stdout to the log file with a real fd, so routing libobs
// diagnostics through stdout is what actually makes them survive to %LOCALAPPDATA%/MoxRelay/logs.
void mox_log_handler(int level, const char *format, va_list args, void *)
{
	// Item 04 threshold: drop lines less severe than the configured level (e.g. INFO/DEBUG when
	// the user picked "warning"). ERROR/WARNING always pass at the default INFO threshold.
	if (level > g_logLevelThreshold.load(std::memory_order_relaxed))
		return;
	char buf[4096];
	vsnprintf(buf, sizeof(buf), format, args);
	const char *tag = level <= LOG_ERROR     ? "ERROR"
			  : level <= LOG_WARNING ? "WARN "
			  : level <= LOG_INFO    ? "INFO "
						 : "DEBUG";
	// B0: stamp epoch-ms + thread id BETWEEN the existing [libobs TAG] and the message, so the
	// win-dshow device-reopen block ("video device:" / "settings updated:") is timestamped +
	// thread-tagged while every existing substring (greps) stays intact. stdout only (stderr is
	// dead in the no-console build).
	const long long epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					  std::chrono::system_clock::now().time_since_epoch())
					  .count();
	fprintf(stdout, "[libobs %s][t=%lld tid=%lu] %s\n", tag, epochMs,
		(unsigned long)GetCurrentThreadId(), buf);
}

// obs_enum_modules enumerates only LOADED modules -- the authoritative loaded count.
void count_loaded_cb(void *param, obs_module_t *)
{
	(*static_cast<int *>(param))++;
}

bool has_trailing_sep(const std::string &s)
{
	return !s.empty() && (s.back() == '/' || s.back() == '\\');
}

// R3: set once obs_startup succeeds, cleared by shutdown(). Guards the double-/never-started
// shutdown paths -- obs_shutdown has NO null guard (obs.c:1377; obs_wait_for_destroy_queue
// dereferences obs->video immediately), so calling it twice, or after a failed startup, crashes.
bool g_started = false;

} // namespace

std::string ObsBootstrap::defaultRundir()
{
	// No compiled-in fallback path: the rundir resolves via --rundir or exe-relative
	// resolution only (data/ ships beside the exe). Returns empty so resolveRundir falls
	// through to those sources.
	return std::string();
}

namespace {

// True if `dir` exists and is a directory.
bool dir_exists(const std::string &dir)
{
#ifdef _WIN32
	const DWORD attrs = GetFileAttributesA(dir.c_str());
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
	(void)dir;
	return false;
#endif
}

// Hand-fill the standard obs_video_info from the boot parameters (obs_create_video_info is a
// Streamlabs-fork-only API). Factored out so BOTH the boot reset (startup) AND the live re-tier
// (retier) build the SAME ovi -- only fps_num/fps_den differ between calls. colorspace/range live
// in the ovi (VIDEO_CS_DEFAULT / VIDEO_RANGE_DEFAULT), so re-running obs_reset_video reapplies the
// video levels automatically -- there is no separate obs_set_video_levels step to keep in sync.
obs_video_info make_ovi(const BootstrapOptions &options)
{
	struct obs_video_info ovi = {};
	ovi.graphics_module = "libobs-d3d11";
	ovi.fps_num         = static_cast<uint32_t>(options.fpsNum > 0 ? options.fpsNum : 60);
	ovi.fps_den         = static_cast<uint32_t>(options.fpsDen > 0 ? options.fpsDen : 1);
	ovi.base_width      = static_cast<uint32_t>(options.baseWidth > 0 ? options.baseWidth : 1920);
	ovi.base_height     = static_cast<uint32_t>(options.baseHeight > 0 ? options.baseHeight : 1080);
	ovi.output_width    = options.outputWidth > 0 ? static_cast<uint32_t>(options.outputWidth) : ovi.base_width;
	ovi.output_height   = options.outputHeight > 0 ? static_cast<uint32_t>(options.outputHeight) : ovi.base_height;
	ovi.output_format   = VIDEO_FORMAT_NV12;
	ovi.adapter         = static_cast<uint32_t>(options.adapter >= 0 ? options.adapter : 0);
	ovi.gpu_conversion  = true;
	ovi.colorspace      = VIDEO_CS_DEFAULT;
	ovi.range           = VIDEO_RANGE_DEFAULT;
	ovi.scale_type      = OBS_SCALE_BICUBIC;
	return ovi;
}

// The directory containing the running exe (forward-slash normalized), or "".
std::string exe_dir()
{
#ifdef _WIN32
	char buf[MAX_PATH] = {};
	const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	if (n == 0 || n >= MAX_PATH)
		return std::string();
	std::string path(buf, n);
	for (char &c : path)
		if (c == '\\')
			c = '/';
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? std::string() : path.substr(0, slash);
#else
	return std::string();
#endif
}

} // namespace

// The libobs per-module config BASE (obs->module_config_path): an absolute, MACHINE-LOCAL,
// UNBRANDED %LOCALAPPDATA%/MoxRelay. libobs appends /<module>/<file> itself, so this is a base dir
// with NO trailing file segment. Without an absolute base, plugins like win-capture mkdir a RELATIVE
// "win-capture/" folder under the process CWD (which would spill into a repo root when the helper is
// launched from there). Mirrors HelperConfig::canonicalConfigPath but uses FOLDERID_LocalAppData
// (machine-local, not roaming) and appends no file name. External linkage (declared in the header)
// so the log sink reuses the SAME root resolution.
std::string moduleConfigDir()
{
#ifdef _WIN32
	PWSTR appData = nullptr;
	std::string base;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appData)) && appData) {
		// Narrow the wide %LOCALAPPDATA% path (ASCII for typical install paths; lossy only for exotic
		// non-ASCII profile dirs, acceptable for this seam).
		int n = WideCharToMultiByte(CP_UTF8, 0, appData, -1, nullptr, 0, nullptr, nullptr);
		if (n > 0) {
			base.resize(static_cast<size_t>(n) - 1);
			WideCharToMultiByte(CP_UTF8, 0, appData, -1, base.data(), n, nullptr, nullptr);
		}
	}
	if (appData)
		CoTaskMemFree(appData);

	// Fallback: SHGetKnownFolderPath is COM-backed and can fail in a pre-COM / pre-Qt context (e.g.
	// --selftest reattaches its log sink before the Q(Core)Application ctor and before obs_startup's
	// COM init). In that case read the LOCALAPPDATA environment variable so the path still resolves
	// ABSOLUTELY instead of degrading to the bare relative "MoxRelay" name. The SHGetKnownFolderPath
	// success path above is untouched: this only runs when `base` is still empty.
	if (base.empty()) {
		char *env = nullptr;
		size_t len = 0;
		if (_dupenv_s(&env, &len, "LOCALAPPDATA") == 0 && env) {
			if (*env != '\0')
				base = env;
			free(env);
		}
	}

	// Normalize backslashes to forward slashes for a stable cross-tool path string.
	for (char &c : base)
		if (c == '\\')
			c = '/';

	if (base.empty())
		return "MoxRelay";
	return base + "/MoxRelay";
#else
	return "MoxRelay";
#endif
}

// Item 04: map a settings string (error/warning/info/debug) to the libobs numeric threshold. An
// unrecognized value maps to LOG_INFO (the default). See the threshold note in the header.
int logLevelThresholdFor(const std::string &level)
{
	if (level == "error")
		return LOG_ERROR;
	if (level == "warning")
		return LOG_WARNING;
	if (level == "debug")
		return LOG_DEBUG;
	return LOG_INFO; // "info" + any unrecognized string
}

void setLogLevelThreshold(int threshold)
{
	g_logLevelThreshold.store(threshold, std::memory_order_relaxed);
}

// R2: explicit (--rundir) > exe-relative (the relocatable install layout: the exe sits at the root
// of its own runtime tree, with data/libobs beside it). No compiled-in fallback.
std::string ObsBootstrap::resolveRundir(const std::string &explicitRundir)
{
	if (!explicitRundir.empty())
		return explicitRundir;

	const std::string exeDir = exe_dir();
	if (!exeDir.empty() && dir_exists(exeDir + "/data/libobs"))
		return exeDir;

	return defaultRundir();
}

BootstrapResult ObsBootstrap::startup(const BootstrapOptions &options)
{
	BootstrapResult r;

	const std::string rundir = resolveRundir(options.rundir);
	if (rundir.empty()) {
		fprintf(stdout,
			"[ObsBootstrap] no rundir: pass --rundir, or ensure data/libobs ships "
			"next to the exe\n");
		return r;
	}
	r.rundirUsed = rundir;

	// Derive the absolute libobs paths from the rundir.
	//   data path   = <rundir>/data/libobs/   (TRAILING SLASH is critical -- see below)
	//   plugin bin  = <rundir>/obs-plugins/64bit/
	//   plugin data = <rundir>/data/obs-plugins/%module%/
	std::string data_path = rundir + "/data/libobs";
	// CRITICAL: libobs's check_path() concatenates path+file with NO separator, so obs_add_data_path
	// REQUIRES a trailing separator. Missing it -> it looks for "...libobsdefault.effect" -> Gate A
	// miss / obs_reset_video failure / silent black frames. Normalize a trailing '/'.
	if (!has_trailing_sep(data_path))
		data_path += '/';

	const std::string plugin_bin  = rundir + "/obs-plugins/64bit/";
	const std::string plugin_data = rundir + "/data/obs-plugins/%module%/";

	// 1. Log handler FIRST so obs_startup/reset/module diagnostics are captured.
	base_set_log_handler(mox_log_handler, nullptr);

	// 2. ABSOLUTE data path BEFORE obs_startup. The Windows default lookup is CWD-relative; an
	//    absolute path is mandatory or default.effect silently fails to resolve.
	obs_add_data_path(data_path.c_str());

	// 3. obs_startup -- the once-per-process singleton (guard: if (obs) at obs.c:1324). libobs calls
	//    initialize_com() itself here; no app-level CoInitializeEx is needed as a libobs precondition.
	//    module_config_path = an ABSOLUTE %LOCALAPPDATA%/MoxRelay base (moduleConfigDir(), or the
	//    BootstrapOptions override for gates) so plugins like win-capture write their per-module
	//    config/cache there instead of mkdir-ing a RELATIVE win-capture/ folder under the CWD.
	//    libobs bstrdup's this arg internally, so it only needs to outlive the obs_startup call --
	//    the `moduleCfg` local below satisfies that.
	const std::string moduleCfg = !options.moduleConfigDirOverride.empty()
		? options.moduleConfigDirOverride
		: moduleConfigDir();
	if (!obs_startup("en-US", moduleCfg.c_str(), nullptr)) {
		fprintf(stdout, "[ObsBootstrap] obs_startup returned false\n");
		return r;
	}
	g_started = true; // from here on, a (guarded) obs_shutdown is required -- even on error paths

	// 4. obs_reset_video with a HAND-FILLED standard obs_video_info (obs_create_video_info is a
	//    Streamlabs-fork-only API). This is where default.effect compiles + the D3D11 device is
	//    created. Headless: no window is needed (only obs_display_create needs a window; the device
	//    does not). R1: fps/canvas/adapter come from BootstrapOptions -- this is the ONE place the
	//    BOOT fps tier is set. The tier is NOT immutable afterwards: a live re-tier re-runs
	//    obs_reset_video in-process via retier() (item 05) to change the frame rate without a
	//    restart, exactly as standard OBS does on every video-settings apply.
	struct obs_video_info ovi = make_ovi(options);

	const int rv = obs_reset_video(&ovi);
	if (rv != OBS_VIDEO_SUCCESS) {
		fprintf(stdout,
			"[ObsBootstrap] obs_reset_video -> %d (default.effect compile or D3D11 device init)\n",
			rv);
		shutdown();
		return r;
	}

	// 5. obs_reset_audio ONCE. Only samples_per_sec + speakers are caller-set; the sample FORMAT is
	//    hardcoded to FLOAT_PLANAR inside libobs (NOT a caller field).
	struct obs_audio_info oai = {};
	oai.samples_per_sec = 48000;
	oai.speakers        = SPEAKERS_STEREO;
	if (!obs_reset_audio(&oai)) {
		fprintf(stdout, "[ObsBootstrap] obs_reset_audio returned false\n");
		shutdown();
		return r;
	}

	r.started = true;

	// L9 (R1): read back the ACHIEVED video parameters -- consumers report these, never the request.
	struct obs_video_info achieved = {};
	if (obs_get_video_info(&achieved)) {
		r.achievedFpsNum       = achieved.fps_num;
		r.achievedFpsDen       = achieved.fps_den;
		r.achievedBaseWidth    = achieved.base_width;
		r.achievedBaseHeight   = achieved.base_height;
		r.achievedOutputWidth  = achieved.output_width;
		r.achievedOutputHeight = achieved.output_height;
		r.achievedAdapter      = static_cast<int>(achieved.adapter);
	}

	// 6. ABSOLUTE module paths AFTER obs_startup (obs_add_module_path early-returns if obs == NULL).
	//    Registering the real plugin set makes Gate B mean "modules were attempted and none FAILED"
	//    (vs. "0 failures because 0 attempted"), matching the proven headless-boot assertion.
	fprintf(stdout, "[ObsBootstrap] obs_add_module_path(bin=%s, data=%s)\n",
		plugin_bin.c_str(), plugin_data.c_str());
	obs_add_module_path(plugin_bin.c_str(), plugin_data.c_str());

	struct obs_module_failure_info mfi = {};
	obs_load_all_modules2(&mfi);
	obs_post_load_modules();

	// --- Gate A: default.effect resolves via the absolute data path. ---
	char *eff = obs_find_data_file("default.effect");
	r.gateA = (eff != nullptr);
	if (eff) {
		fprintf(stdout, "[ObsBootstrap] Gate A: default.effect -> %s\n", eff);
		bfree(eff);
	}

	// --- Gate B: zero module-load failures (mfi is an OUT param; free it). ---
	r.moduleFailures = mfi.count;
	r.gateB          = (mfi.count == 0);
	if (mfi.count) {
		fprintf(stdout, "[ObsBootstrap] Gate B: %zu module(s) failed to load:\n", mfi.count);
		for (size_t i = 0; i < mfi.count; i++)
			fprintf(stdout, "    - %s\n", mfi.failed_modules[i]);
	}
	obs_module_failure_info_free(&mfi);

	// Informational: how many modules actually loaded (LOADED-only enumeration).
	int loaded = 0;
	obs_enum_modules(count_loaded_cb, &loaded);
	r.modulesLoaded = loaded;

	return r;
}

// Item 05: live in-process re-tier. Re-runs obs_reset_video with a fresh ovi (new fps) WITHOUT
// re-running obs_startup / obs_reset_audio / the module paths -- those are once-per-process and the
// D3D11 device + the loaded sources/audio survive the reset (libobs only rebuilds the per-mix render
// textures and the video thread). The CALLER must have torn down ALL GPU work and removed the render
// callback first (SpoutSenderEngine::stop drains its wait=true OBS_TASK_GRAPHICS tasks while the
// graphics thread is still alive): obs_reset_video JOINS/stops the graphics thread, so any wait=true
// graphics task queued after this hangs forever. MUST run on the control/Qt/main thread, never from
// the graphics thread (self-join deadlock). Guarded on obs_initialized() (mirrors shutdown()); the
// precondition obs_video_active()==false holds structurally because MoxRelay drives Spout via
// obs_add_main_render_callback, which touches neither the raw nor the gpu-encoder active counter.
ObsBootstrap::RetierResult ObsBootstrap::retier(const ObsBootstrap::RetierOptions &options)
{
	RetierResult r;
	if (!g_started || !obs_initialized()) {
		fprintf(stdout, "[ObsBootstrap] retier() before libobs is up -- refused\n");
		return r;
	}

	// Carry the new fps + the current canvas/output dims into the shared ovi fill. Adapter/rundir are
	// fixed for the process lifetime, so a default-constructed BootstrapOptions (adapter 0) is fine --
	// obs_reset_video keeps the existing D3D11 device regardless of the adapter field.
	BootstrapOptions boot;
	boot.fpsNum       = options.fpsNum;
	boot.fpsDen       = options.fpsDen;
	boot.baseWidth    = options.baseWidth;
	boot.baseHeight   = options.baseHeight;
	boot.outputWidth  = options.outputWidth;
	boot.outputHeight = options.outputHeight;

	struct obs_video_info ovi = make_ovi(boot);
	const int rv = obs_reset_video(&ovi);
	if (rv != OBS_VIDEO_SUCCESS) {
		fprintf(stdout, "[ObsBootstrap] retier obs_reset_video -> %d\n", rv);
		return r;
	}

	r.ok = true;
	// Read back the ACHIEVED fps -- consumers report the achieved tier, never the request.
	struct obs_video_info achieved = {};
	if (obs_get_video_info(&achieved)) {
		r.achievedFpsNum = achieved.fps_num;
		r.achievedFpsDen = achieved.fps_den;
	}
	return r;
}

// R3: idempotent. Safe to call after a failed startup, and safe to call twice -- obs_shutdown
// itself has no guard for either case. Both the internal flag AND obs_initialized() are checked:
// the flag covers "this class never started libobs", obs_initialized() covers "already shut down".
void ObsBootstrap::shutdown()
{
	if (!g_started || !obs_initialized())
		return;
	g_started = false;
	obs_shutdown();
}

} // namespace moxrelay
