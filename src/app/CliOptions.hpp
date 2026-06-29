// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// CliOptions (R5) -- the STRICT command-line surface. Unknown options and stray
// positional arguments are hard parse ERRORS, never a silent fall-through to the GUI: a
// launch whose arguments were mangled must fail loudly, not open a window.
//
//   moxrelay.exe [--start-minimized] [--owner-id <token>] [--client-name <name>] [--fps-tier N]
//                [--discovery-path <file>] [--port N] [--rendezvous-pipe <name>] [--rundir <path>]
//                [--adapter N]
//       GUI (single instance). --start-minimized starts hidden in the system tray (for launches
//       managed by another application); when no system tray is available it falls back to a
//       minimized window. --owner-id advertises an opaque owner token. --client-name sets a managed
//       display name shown in the window title and tray tooltip (absent = falls back to --owner-id).
//       --fps-tier sets the global
//       frame rate (default 60). --discovery-path overrides where the discovery file
//       (helper-config.json) is written (default: %APPDATA%/MoxRelay/helper-config.json).
//       --port sets the requested control-port base (default 7341; the resolver auto-falls-back
//       if it is busy). --rendezvous-pipe writes the instance over the named pipe <name> instead
//       of the discovery file (mutually exclusive with --discovery-path).
//       --adapter selects the GPU adapter index for the rendering device (default 0 = primary);
//       like --rundir it is accepted in every mode (GUI, --selftest, --perf).
//       Without the flags, launches are unchanged.
//   moxrelay.exe --selftest [--gates <list>] [--reps N] [--hold N] [--rundir <path>] [--adapter N]
//       Headless gates, exit 0/1/2 (contract unchanged since M1). --gates runs only the named
//       gates (comma-separated; a/b always assert -- they gate the engine boot itself);
//       --reps repeats the pass in sequential child processes, stopping at the first failure.
//   moxrelay.exe --perf --fps-tier N [--width W --height H] [--max-n N] [--flush-mode immediate|batched]
//                [--out <file>] [--settle-sec S] [--measure-sec S] [--budget-pct P] [--lagged-pct P]
//                [--rundir R] [--adapter N]
//       Headless perf harness (M2.4): ramps synthetic sources+senders at ONE fps tier and finds
//       the per-instance ceiling. One process per (tier, resolution, flush-mode) cell -- fps can
//       never change in-process. JSONL output; threshold flags override the built-in
//       defaults (90% frame budget / 1% lagged).
//
// QCommandLineParser is used via parse(QStringList) so parsing works BEFORE any Q*Application
// object exists (the GUI/selftest/perf split needs the mode first).

#pragma once

#include <QString>
#include <QStringList>

namespace moxrelay {

struct CliOptions {
	enum class Mode { Gui, Selftest, Perf };

	// Parse outcome. ok=false -> `error` says why (already user-readable); helpRequested -> the
	// caller prints helpText() and exits 0.
	bool ok = false;
	bool helpRequested = false;
	QString error;

	Mode mode = Mode::Gui;

	// GUI
	bool startMinimized = false; // start hidden in the system tray (minimized-window fallback)
	QString ownerId;             // opaque owner token advertised at the process level (empty = unowned)
	QString clientName;          // optional managed display name (empty = fall back to ownerId for the title)
	QString discoveryPath;       // explicit discovery-file path (empty = canonicalConfigPath())
	QString rendezvousPipe;      // managed rendezvous pipe name (empty = write the discovery file)
	int     port = 0;            // requested control-port base (0 = default 7341)
	int     ownerPid = 0;        // managed-mode owner PID to watch (0 = not watching; self-exit when it dies)

	// Selftest
	int holdSeconds = 0;
	QStringList selftestGates; // normalized lowercase gate names; empty = full suite
	int selftestReps = 1;      // >1 = sequential child-process passes, stop on first failure

	// GUI + Perf
	int fpsTier = 0;

	// All modes (GUI/selftest/perf); SENTINEL -1 = --adapter not passed. The parser's int_option
	// minValue 0 rejects negatives, so a user can never pass -1; only the default is -1. Every
	// consumer treats <0 as 0 (make_ovi clamps; the selftest/perf boots copy it through to make_ovi;
	// run_gui resolves -1 to the saved standalone setting, else 0).
	int adapter = -1;

	// Perf (locked harness defaults)
	int perfWidth = 1920;
	int perfHeight = 1080;
	int perfMaxN = 64;
	bool perfFlushBatched = true; // --flush-mode (default batched = the product default; 'immediate' for A/B)
	QString perfOutPath;           // JSONL also appended here when non-empty
	int perfSettleSec = 2;
	int perfMeasureSec = 5;
	double perfBudgetPct = 90.0; // avg frame time > this % of the frame budget = ceiling
	double perfLaggedPct = 1.0;  // windowed lagged/total > this % = ceiling

	// All modes
	QString rundir; // explicit libobs runtime tree (R2: else exe-relative, else compiled-in)

	// args[0] is the program name (pass QCoreApplication::arguments()-shaped lists or build one
	// from argv).
	static CliOptions parse(const QStringList &args);

	static QString helpText();
};

} // namespace moxrelay
