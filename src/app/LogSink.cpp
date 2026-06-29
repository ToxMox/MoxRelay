// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// LogSink implementation. See LogSink.hpp for the contract. Windows-only (this is a Windows app).
// The log root reuses ObsBootstrap::moduleConfigDir() (the SAME %LOCALAPPDATA%/MoxRelay LocalAppData
// resolution) so there is a single known-folder idiom, never a duplicated SHGetKnownFolderPath.

#include "LogSink.hpp"

#include "AppSettings.hpp"  // item 04: OR-in the log/toFile QSetting toggle
#include "ObsBootstrap.hpp" // moduleConfigDir() -- shared %LOCALAPPDATA%/MoxRelay resolver

#include <windows.h> // AttachConsole / GetCurrentProcessId / GetLastError

#include <io.h>       // _dup2 / _fileno / _open_osfhandle / _get_osfhandle / _close -- bind stdout's fd
#include <fcntl.h>    // _O_WRONLY / _O_TEXT for _open_osfhandle
#include <chrono>     // age-prune cutoff
#include <cstdio>     // freopen_s
#include <cstdlib>    // getenv (file-logging opt-in hatch)
#include <cstring>    // strcmp (env-var value check)
#include <filesystem> // create_directories + age-prune (C++20: CMAKE_CXX_STANDARD 20)
#include <string>
#include <system_error>

namespace moxrelay {

namespace {

// File logging is DEFAULT OFF: a normal run creates NO log file and NOT even the logs dir. Two
// opt-ins (either turns it on): (1) the quiet env-var debug hatch -- set MOXRELAY_LOG_FILE to any
// non-empty value other than "0" -- which stays the UNIVERSAL override in both modes; and (2) item
// 04's `log/toFile` QSetting. The QSetting read works here even though this runs before the
// QApplication ctor: run_gui sets the org/app-name statics + IniFormat default BEFORE
// installFileLogSink(), and AppSettings constructs with explicit IniFormat/UserScope/org/app, so the
// %APPDATA%/MoxRelay/MoxRelay.ini store resolves with no QApplication instance.
bool envFileLoggingEnabled()
{
	// Secure CRT variant (matches the freopen_s usage; std::getenv is deprecated under MSVC).
	// _dupenv_s heap-allocates the value; free() it after the check. MSVC-only -- fine, Windows file.
	char *v = nullptr;
	size_t len = 0;
	if (_dupenv_s(&v, &len, "MOXRELAY_LOG_FILE") != 0 || !v)
		return false; // unset or error -> disabled
	const bool enabled = (*v != '\0') && std::strcmp(v, "0") != 0;
	free(v);
	return enabled;
}

bool fileLoggingEnabled()
{
	if (envFileLoggingEnabled())
		return true; // env hatch is the universal override (both modes)
	// Item 04: the persisted standalone toggle.
	return AppSettings().logToFile();
}

// Resolve %LOCALAPPDATA%/MoxRelay/logs, creating BOTH segments (moduleConfigDir() does not create
// the parent). Returns "" on any failure -- caller treats that as "no file logging" and runs on.
std::string ensureLogDir()
{
	const std::string base = moduleConfigDir(); // <LocalAppData>/MoxRelay (no trailing slash)
	if (base.empty())
		return std::string();

	// Require an ABSOLUTE base. moduleConfigDir() degrades to a bare relative name when the known-
	// folder resolution fails; honoring that here would scatter a relative logs/ tree under the CWD of
	// a detached run. Treat a non-absolute base as "no file logging" (the caller runs on without it).
	if (!std::filesystem::path(base).is_absolute())
		return std::string();

	const std::string logDir = base + "/logs";

	// Best-effort directory creation: create_directories makes the parent + /logs in one call and
	// is a no-op (no error) when they already exist. Swallow every error -- startup must not throw.
	std::error_code ec;
	std::filesystem::create_directories(logDir, ec);
	// Even if create_directories reported an error, the dir may already exist; let freopen decide.
	return logDir;
}

// Delete moxrelay-*.log files older than ~7 days. Best-effort: swallow ALL errors, never throw or
// block startup. Run once on sink install so the log dir stays bounded across many launches.
void pruneOldLogs(const std::string &logDir)
{
	namespace fs = std::filesystem;
	std::error_code ec;

	const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 7);

	fs::directory_iterator it(logDir, ec), end;
	if (ec)
		return;
	for (; it != end; it.increment(ec)) {
		if (ec)
			return;
		const fs::directory_entry &entry = *it;

		std::error_code e2;
		if (!entry.is_regular_file(e2) || e2)
			continue;

		const std::string name = entry.path().filename().string();
		if (name.rfind("moxrelay-", 0) != 0)
			continue;
		if (name.size() < 4 || name.compare(name.size() - 4, 4, ".log") != 0)
			continue;

		const auto mtime = fs::last_write_time(entry.path(), e2);
		if (e2)
			continue;
		if (mtime < cutoff)
			fs::remove(entry.path(), e2); // ignore failure
	}
}

// Point stdout (and raw lowio fd 2) at the per-pid log file. Captures third-party libobs writes too
// (not just mox_log_handler), because the redirect is at the C runtime / OS-handle level. Best-effort
// no-op on failure -- startup never blocks or crashes on a logging problem.
//
// /SUBSYSTEM:WINDOWS HAZARD (item 03 flip): in a no-console process the CRT marks the std streams with
// _NO_CONSOLE_FILENO (_fileno == -2). TWO consequences shaped this code:
//   * stderr: freopen_s on the _fileno==-2 stderr drives ucrt into its invalid-fd path and the
//     invalid-parameter handler __fastfail's (STATUS_STACK_BUFFER_OVERRUN, 0xC0000409) -> GUI crash on
//     boot. So we NEVER freopen stderr. A _NO_CONSOLE_FILENO stderr FILE* also cannot be repaired by fd
//     tricks (its write path is short-circuited), so fprintf(stderr,...) is dropped regardless. The fix
//     has two parts: (1) here, _dup2 the stdout fd onto lowio fd 2 so raw write(2,...) from child
//     processes / FFmpeg / direct libobs lowio still lands in the log; (2) in ObsBootstrap.cpp,
//     mox_log_handler + boot diagnostics write to STDOUT (which IS captured), not stderr.
//   * stdout: the old secure freopen_s bound the file here but used _SH_DENYRW (exclusive), so nothing
//     could tail the live log; switching to a plain share-friendly freopen(path) regressed harder --
//     plain freopen does NOT reliably (re)bind stdout in the no-console process (returns NULL), so NO
//     log file was written at all (a console-launched --selftest masked this: a console gives stdout a
//     real fd). So stdout is bound via the explicit NUL-then-CreateFileW(share-read)-then-_dup2 dance
//     below, which works WITH OR WITHOUT a console AND opens the log FILE_SHARE_READ so other processes
//     can read/tail it while moxrelay runs.
void redirectStreamsToFile()
{
	const std::string logDir = ensureLogDir();
	if (logDir.empty())
		return;

	pruneOldLogs(logDir);

	const std::string path =
		logDir + "/moxrelay-" + std::to_string(GetCurrentProcessId()) + ".log";

	// (a) First bind stdout to the NUL device using the SECURE freopen_s. Its ONLY job is to give stdout
	// a VALID lowio fd in the no-console GUI process (where _fileno(stdout) starts at _NO_CONSOLE_FILENO
	// == -2); without it the _dup2 in (d) would have no valid target fd. freopen_s (NOT plain freopen) is
	// deliberate: in a no-console /SUBSYSTEM:WINDOWS process freopen_s rebinds stdout successfully but
	// plain freopen returns NULL (that was the original regression). NUL opening exclusive is irrelevant
	// -- nothing else opens NUL, and (d) immediately dup2's the share-read CreateFileW log over it, so no
	// lock is introduced. On failure stdout may be unusable, but we just run on without file logging.
	FILE *nulFp = nullptr;
	if (freopen_s(&nulFp, "NUL", "w", stdout) != 0 || !nulFp)
		return; // could not give stdout a real fd; run on without file logging

	// (b) Widen the UTF-8 log path (moduleConfigDir() emits UTF-8) for the wide CreateFile.
	std::wstring wpath;
	const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	if (wlen > 1) {
		wpath.resize(static_cast<size_t>(wlen) - 1);
		MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
	}
	if (wpath.empty())
		return; // path conversion failed; stdout harmlessly points at NUL

	// (c) Open the real log SHARE-READ (and -WRITE/-DELETE so it can be tailed and rotated/renamed while
	// open). CREATE_ALWAYS truncates, matching the old "w" freopen. This is the deterministic open that
	// works regardless of console state -- unlike plain freopen(path) which returns NULL with no console.
	const HANDLE h = CreateFileW(wpath.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return; // could not create the log; stdout harmlessly points at NUL

	const int logFd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_TEXT);
	if (logFd == -1) {
		CloseHandle(h); // _open_osfhandle did not take ownership on failure
		return;
	}

	// (d) Point stdout's now-valid fd at the share-read log. _dup2 duplicates logFd's OS handle onto
	// stdout's fd (closing stdout's prior NUL handle first), so stdout owns an INDEPENDENT handle to the
	// log; _close(logFd) then releases the now-redundant fd and the original `h`. After this,
	// fprintf(stdout,...) -- the mox_log_handler channel -- lands in the file.
	if (_dup2(logFd, _fileno(stdout)) != 0) {
		_close(logFd); // closes logFd + h
		return;        // stdout harmlessly points at NUL
	}
	_close(logFd);

	// Make GetStdHandle(STD_OUTPUT_HANDLE) users + inherited child processes follow the same file. Use
	// the LIVE handle stdout's fd now owns (the _dup2 copy); the original `h` was closed with logFd.
	const HANDLE outHandle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
	if (outHandle != INVALID_HANDLE_VALUE)
		SetStdHandle(STD_OUTPUT_HANDLE, outHandle);
	// Unbuffered: a freopen'd file stream is FULLY buffered, so on a fast/abnormal exit (the boot
	// FAILURE case this sink exists to capture) nothing flushes -> a 0-byte log. _IONBF forces every
	// write through immediately. NOT _IOLBF: the MSVC CRT treats _IOLBF as full buffering, so it
	// would not fix this. Low-volume diagnostics, so the per-write cost is irrelevant.
	setvbuf(stdout, nullptr, _IONBF, 0);

	// Catch raw lowio stderr (fd 2) writes WITHOUT freopen (which __fastfail's on the no-console
	// stderr; see note above). _dup2 the stdout file descriptor onto stderr's fd when it is valid,
	// else onto literal fd 2. This does NOT make the stderr FILE* writable (that channel is routed
	// through stdout in ObsBootstrap.cpp); it only redirects byte-level write(2,...) traffic from
	// child processes / FFmpeg / direct libobs lowio. Best-effort -- never a crash on failure.
	const int outFd = _fileno(stdout);
	if (outFd >= 0) {
		const int errFd = _fileno(stderr);
		_dup2(outFd, errFd >= 0 ? errFd : 2);
	}
}

// Reopen stdout + stderr on the attached parent console so dev-shell output stays visible.
void redirectStreamsToConsole()
{
	FILE *fp = nullptr;
	if (freopen_s(&fp, "CONOUT$", "w", stdout) == 0 && fp)
		setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: survive a hard exit (see redirectStreamsToFile)
	fp = nullptr;
	if (freopen_s(&fp, "CONOUT$", "w", stderr) == 0 && fp)
		setvbuf(stderr, nullptr, _IONBF, 0);
}

} // namespace

void installFileLogSink()
{
	// Default OFF: with no opt-in this is a COMPLETE no-op -- no create_directories, no freopen, no
	// file, no prune. Only the env-var hatch (item 04: or the settings toggle) turns the sink on.
	if (!fileLoggingEnabled())
		return;
	redirectStreamsToFile();
}

void installConsoleOrFileLogSink()
{
	// Explicit file-log request (MOXRELAY_LOG_FILE set): write to the file sink and return WITHOUT
	// ever calling AttachConsole -- even when a parent console exists. Reattaching the parent console
	// routes gate output (and the console reset on exit) into whatever terminal launched us; when that
	// terminal is a live interactive host, those writes corrupt it. Gating on the env var here makes
	// the file mode safe by construction regardless of how the process was launched. Use the env-only
	// check (NOT fileLoggingEnabled()): this runs before the QApplication ctor, so the QSettings-backed
	// toggle must not be consulted on this path.
	if (envFileLoggingEnabled()) {
		redirectStreamsToFile();
		return;
	}
	// Env unset, human at a real dev shell: reattach the parent console so --selftest/--perf output
	// stays visible on the invoking terminal.
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		redirectStreamsToConsole();
		return;
	}
	// No parent console and no explicit file request (detached / CI): fall back to the file sink so
	// diagnostics still persist.
	redirectStreamsToFile();
}

} // namespace moxrelay
