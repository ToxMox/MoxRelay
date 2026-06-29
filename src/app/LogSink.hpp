// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// LogSink -- redirect diagnostics to a durable location now that the executable is built as the
// WINDOWS PE subsystem (no console window). Because the console was the only diagnostics sink
// (mox_log_handler fprintf(stderr) plus any direct libobs printf/fprintf), the subsystem flip would
// otherwise discard every libobs/MoxRelay line. This redirects stdout+stderr to a per-pid file under
// %LOCALAPPDATA%/MoxRelay/logs/ (and, for dev-shell --selftest/--perf, reattaches the parent
// console). Best-effort: any failure is swallowed and the app still runs, just without file logging.
//
// MVP for this milestone: file always, NO level filter and NO on/off toggle -- a later settings item
// layers a level threshold + toggle on top of this sink, so keep this small and self-contained.

#pragma once

namespace moxrelay {

// Redirect stdout+stderr to %LOCALAPPDATA%/MoxRelay/logs/moxrelay-<pid>.log. Best-effort; never
// throws. Call EARLY -- before ObsBootstrap registers the libobs log handler -- so the very first
// libobs boot lines are captured. Used by GUI mode (standalone and managed child alike): a GUI
// launch has no useful parent console, so this never attaches one.
void installFileLogSink();

// For --selftest / --perf launched from a developer shell: AttachConsole(ATTACH_PARENT_PROCESS)
// and reopen CONOUT$ so output stays visible on the invoking console. When there is no parent
// console (detached / CI), fall back to installFileLogSink() so diagnostics still land in a file.
// Never AllocConsole (that would re-create the window the subsystem flip removed).
void installConsoleOrFileLogSink();

} // namespace moxrelay
