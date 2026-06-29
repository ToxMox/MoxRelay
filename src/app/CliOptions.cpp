// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// CliOptions implementation (R5). See CliOptions.hpp for the surface. Strictness rules:
//   - unknown options / positional arguments are parse errors (exit, never GUI);
//   - mode flags are mutually exclusive (--selftest vs --perf);
//   - mode-scoped options are rejected outside their mode (--hold/--gates/--reps without
//     --selftest, the perf knobs outside --perf; --fps-tier is allowed in GUI + --perf);
//   - numeric options must parse and be positive (adapter: non-negative).

#include "CliOptions.hpp"

#include <QCommandLineOption>
#include <QCommandLineParser>

namespace moxrelay {

namespace {

struct ParserBundle {
	QCommandLineParser parser;
	QCommandLineOption selftest{QStringLiteral("selftest"),
				    QStringLiteral("Run the headless self-test gates and exit 0/1/2.")};
	QCommandLineOption hold{QStringLiteral("hold"),
				QStringLiteral("(--selftest) keep the gate senders open N seconds for an external receiver."),
				QStringLiteral("N")};
	QCommandLineOption gates{QStringLiteral("gates"),
				 QStringLiteral("(--selftest) run only the named gates, comma-separated (valid: a, b, c, d, d2, e, f, g, h, i, j; a and b always run -- they assert the engine boot itself)."),
				 QStringLiteral("list")};
	QCommandLineOption reps{QStringLiteral("reps"),
				QStringLiteral("(--selftest) repeat the pass N times in sequential child processes, stopping at the first failing pass."),
				QStringLiteral("N")};
	QCommandLineOption startMinimized{
		QStringLiteral("start-minimized"),
		QStringLiteral("(GUI) start hidden in the system tray (minimized window when no tray is available).")};
	QCommandLineOption ownerId{QStringLiteral("owner-id"),
				   QStringLiteral("(GUI) opaque owner token advertised at the process level (GetVersion + helper-config.json); absent = unowned."),
				   QStringLiteral("token")};
	QCommandLineOption clientName{QStringLiteral("client-name"),
				      QStringLiteral("(GUI) optional managed display name shown in the window title and tray tooltip; absent = fall back to --owner-id."),
				      QStringLiteral("name")};
	QCommandLineOption discoveryPath{QStringLiteral("discovery-path"),
					 QStringLiteral("(GUI) explicit discovery-file path to write helper-config.json (default: %APPDATA%/MoxRelay/helper-config.json)."),
					 QStringLiteral("file")};
	QCommandLineOption rendezvousPipe{QStringLiteral("rendezvous-pipe"),
					  QStringLiteral("(GUI) write the instance over this named pipe instead of the discovery file (mutually exclusive with --discovery-path)."),
					  QStringLiteral("name")};
	QCommandLineOption port{QStringLiteral("port"),
				QStringLiteral("(GUI) requested control-port base (default 7341; the resolver auto-falls-back if it is busy)."),
				QStringLiteral("N")};
	QCommandLineOption ownerPid{QStringLiteral("owner-pid"),
				    QStringLiteral("Exit automatically when the process with this PID terminates (managed mode)."),
				    QStringLiteral("N")};
	QCommandLineOption fpsTier{QStringLiteral("fps-tier"),
				   QStringLiteral("(GUI/--perf) this instance's global fps tier."),
				   QStringLiteral("N")};
	QCommandLineOption rundir{QStringLiteral("rundir"),
				  QStringLiteral("Absolute libobs runtime tree (default: exe-relative -- data/libobs beside the exe; required if that is absent)."),
				  QStringLiteral("path")};
	QCommandLineOption adapter{QStringLiteral("adapter"),
				   QStringLiteral("GPU adapter index for the rendering device (default 0 = primary)."),
				   QStringLiteral("N")};
	QCommandLineOption perf{QStringLiteral("perf"),
				QStringLiteral("Run the headless perf harness (one process per tier/resolution/flush-mode cell).")};
	QCommandLineOption width{QStringLiteral("width"),
				 QStringLiteral("(--perf) synthetic source width (default 1920)."),
				 QStringLiteral("W")};
	QCommandLineOption height{QStringLiteral("height"),
				  QStringLiteral("(--perf) synthetic source height (default 1080)."),
				  QStringLiteral("H")};
	QCommandLineOption maxN{QStringLiteral("max-n"),
				QStringLiteral("(--perf) ramp cap on sender count (default 64)."),
				QStringLiteral("N")};
	QCommandLineOption flushMode{QStringLiteral("flush-mode"),
				     QStringLiteral("(--perf) 'batched' (one Flush per frame; product default) or 'immediate' (per-sender Flush; A/B comparison)."),
				     QStringLiteral("mode")};
	QCommandLineOption out{QStringLiteral("out"),
			       QStringLiteral("(--perf) also append the JSONL lines to this file."),
			       QStringLiteral("file")};
	QCommandLineOption settleSec{QStringLiteral("settle-sec"),
				     QStringLiteral("(--perf) settle seconds after each attach (default 2)."),
				     QStringLiteral("S")};
	QCommandLineOption measureSec{QStringLiteral("measure-sec"),
				      QStringLiteral("(--perf) measure-window seconds per step (default 5)."),
				      QStringLiteral("S")};
	QCommandLineOption budgetPct{QStringLiteral("budget-pct"),
				     QStringLiteral("(--perf) ceiling when avg frame time exceeds this %% of the frame budget (default 90)."),
				     QStringLiteral("P")};
	QCommandLineOption laggedPct{QStringLiteral("lagged-pct"),
				     QStringLiteral("(--perf) ceiling when windowed lagged/total exceeds this %% (default 1)."),
				     QStringLiteral("P")};
	QCommandLineOption help{{QStringLiteral("h"), QStringLiteral("help")},
				QStringLiteral("Show this help and exit.")};

	ParserBundle()
	{
		parser.setApplicationDescription(
			QStringLiteral("MoxRelay -- libobs->Spout capture helper (M2.4 perf build)"));
		parser.addOption(selftest);
		parser.addOption(hold);
		parser.addOption(gates);
		parser.addOption(reps);
		parser.addOption(startMinimized);
		parser.addOption(ownerId);
		parser.addOption(clientName);
		parser.addOption(discoveryPath);
		parser.addOption(rendezvousPipe);
		parser.addOption(port);
		parser.addOption(ownerPid);
		parser.addOption(fpsTier);
		parser.addOption(rundir);
		parser.addOption(adapter);
		parser.addOption(perf);
		parser.addOption(width);
		parser.addOption(height);
		parser.addOption(maxN);
		parser.addOption(flushMode);
		parser.addOption(out);
		parser.addOption(settleSec);
		parser.addOption(measureSec);
		parser.addOption(budgetPct);
		parser.addOption(laggedPct);
		parser.addOption(help);
	}
};

// Parse a decimal int option; ok=false (with a message in *error) on garbage or out-of-range.
int int_option(const QCommandLineParser &parser, const QCommandLineOption &opt, int minValue,
	       bool *ok, QString *error)
{
	const QString raw = parser.value(opt);
	bool numOk = false;
	const int v = raw.toInt(&numOk, 10);
	if (!numOk || v < minValue) {
		*ok = false;
		*error = QStringLiteral("--%1 expects an integer >= %2 (got '%3')")
				 .arg(opt.names().first())
				 .arg(minValue)
				 .arg(raw);
		return 0;
	}
	*ok = true;
	return v;
}

// Parse a positive double option (perf thresholds); same contract as int_option.
double double_option(const QCommandLineParser &parser, const QCommandLineOption &opt, bool *ok,
		     QString *error)
{
	const QString raw = parser.value(opt);
	bool numOk = false;
	const double v = raw.toDouble(&numOk);
	if (!numOk || v <= 0.0) {
		*ok = false;
		*error = QStringLiteral("--%1 expects a number > 0 (got '%2')")
				 .arg(opt.names().first())
				 .arg(raw);
		return 0.0;
	}
	*ok = true;
	return v;
}

} // namespace

CliOptions CliOptions::parse(const QStringList &args)
{
	CliOptions out;
	ParserBundle b;

	if (!b.parser.parse(args)) {
		out.error = b.parser.errorText();
		return out;
	}
	if (!b.parser.positionalArguments().isEmpty()) {
		out.error = QStringLiteral("unexpected positional argument(s): %1")
				    .arg(b.parser.positionalArguments().join(QStringLiteral(" ")));
		return out;
	}
	if (b.parser.isSet(b.help)) {
		out.ok = true;
		out.helpRequested = true;
		return out;
	}

	const bool isSelftest = b.parser.isSet(b.selftest);
	const bool isPerf = b.parser.isSet(b.perf);
	if (isSelftest && isPerf) {
		out.error = QStringLiteral("--selftest and --perf are mutually exclusive");
		return out;
	}

	out.rundir = b.parser.value(b.rundir);
	if (b.parser.isSet(b.adapter)) {
		bool ok = false;
		out.adapter = int_option(b.parser, b.adapter, 0, &ok, &out.error);
		if (!ok)
			return out;
	}

	// Perf-only knobs are rejected everywhere else (strictness rule).
	if (!isPerf && (b.parser.isSet(b.width) || b.parser.isSet(b.height) || b.parser.isSet(b.maxN) ||
			b.parser.isSet(b.flushMode) || b.parser.isSet(b.out) ||
			b.parser.isSet(b.settleSec) || b.parser.isSet(b.measureSec) ||
			b.parser.isSet(b.budgetPct) || b.parser.isSet(b.laggedPct))) {
		out.error = QStringLiteral(
			"--width/--height/--max-n/--flush-mode/--out/--settle-sec/--measure-sec/--budget-pct/--lagged-pct are only valid with --perf");
		return out;
	}

	if (isPerf) {
		out.mode = Mode::Perf;
		if (b.parser.isSet(b.startMinimized) || b.parser.isSet(b.ownerId) ||
		    b.parser.isSet(b.discoveryPath) || b.parser.isSet(b.rendezvousPipe) ||
		    b.parser.isSet(b.port) || b.parser.isSet(b.ownerPid) || b.parser.isSet(b.hold) ||
		    b.parser.isSet(b.gates) || b.parser.isSet(b.reps)) {
			out.error = QStringLiteral(
				"--start-minimized/--owner-id/--discovery-path/--rendezvous-pipe/--port/--owner-pid/--hold/--gates/--reps are not valid with --perf");
			return out;
		}
		if (!b.parser.isSet(b.fpsTier)) {
			out.error = QStringLiteral("--perf requires --fps-tier");
			return out;
		}
		bool ok = false;
		out.fpsTier = int_option(b.parser, b.fpsTier, 1, &ok, &out.error);
		if (!ok)
			return out;
		if (b.parser.isSet(b.width)) {
			out.perfWidth = int_option(b.parser, b.width, 16, &ok, &out.error);
			if (!ok)
				return out;
		}
		if (b.parser.isSet(b.height)) {
			out.perfHeight = int_option(b.parser, b.height, 16, &ok, &out.error);
			if (!ok)
				return out;
		}
		if (b.parser.isSet(b.maxN)) {
			out.perfMaxN = int_option(b.parser, b.maxN, 1, &ok, &out.error);
			if (!ok)
				return out;
		}
		if (b.parser.isSet(b.flushMode)) {
			const QString mode = b.parser.value(b.flushMode);
			if (mode == QStringLiteral("batched")) {
				out.perfFlushBatched = true;
			} else if (mode == QStringLiteral("immediate")) {
				out.perfFlushBatched = false;
			} else {
				out.error = QStringLiteral("--flush-mode expects 'immediate' or 'batched' (got '%1')")
						    .arg(mode);
				return out;
			}
		}
		out.perfOutPath = b.parser.value(b.out);
		if (b.parser.isSet(b.settleSec)) {
			out.perfSettleSec = int_option(b.parser, b.settleSec, 0, &ok, &out.error);
			if (!ok)
				return out;
		}
		if (b.parser.isSet(b.measureSec)) {
			out.perfMeasureSec = int_option(b.parser, b.measureSec, 1, &ok, &out.error);
			if (!ok)
				return out;
		}
		if (b.parser.isSet(b.budgetPct)) {
			out.perfBudgetPct = double_option(b.parser, b.budgetPct, &ok, &out.error);
			if (!ok)
				return out;
		}
		if (b.parser.isSet(b.laggedPct)) {
			out.perfLaggedPct = double_option(b.parser, b.laggedPct, &ok, &out.error);
			if (!ok)
				return out;
		}
		out.ok = true;
		return out;
	}

	if (isSelftest) {
		out.mode = Mode::Selftest;
		if (b.parser.isSet(b.fpsTier)) {
			out.error = QStringLiteral("--fps-tier is not valid with --selftest");
			return out;
		}
		if (b.parser.isSet(b.startMinimized) || b.parser.isSet(b.ownerId) ||
		    b.parser.isSet(b.discoveryPath) || b.parser.isSet(b.rendezvousPipe) ||
		    b.parser.isSet(b.port) || b.parser.isSet(b.ownerPid)) {
			out.error = QStringLiteral(
				"--start-minimized/--owner-id/--discovery-path/--rendezvous-pipe/--port/--owner-pid are not valid with --selftest");
			return out;
		}
		if (b.parser.isSet(b.hold)) {
			bool ok = false;
			out.holdSeconds = int_option(b.parser, b.hold, 1, &ok, &out.error);
			if (!ok)
				return out;
		}
		if (b.parser.isSet(b.gates)) {
			static const QStringList kValidGates = {
				QStringLiteral("a"), QStringLiteral("b"),  QStringLiteral("c"),
				QStringLiteral("d"), QStringLiteral("d2"), QStringLiteral("e"),
				QStringLiteral("f"), QStringLiteral("g"),  QStringLiteral("h"),
				QStringLiteral("i"), QStringLiteral("j")};
			const QStringList raw =
				b.parser.value(b.gates).split(QLatin1Char(','), Qt::SkipEmptyParts);
			for (const QString &entry : raw) {
				const QString name = entry.trimmed().toLower();
				if (name.isEmpty())
					continue;
				if (!kValidGates.contains(name)) {
					out.error = QStringLiteral(
							    "--gates: unknown gate '%1' (valid: a, b, c, d, d2, e, f, g, h, i, j)")
							    .arg(entry.trimmed());
					return out;
				}
				if (!out.selftestGates.contains(name))
					out.selftestGates << name;
			}
			if (out.selftestGates.isEmpty()) {
				out.error = QStringLiteral(
					"--gates expects a comma-separated list of gate names (a, b, c, d, d2, e, f, g, h, i, j)");
				return out;
			}
			if (b.parser.isSet(b.hold) && !out.selftestGates.contains(QStringLiteral("d"))) {
				out.error = QStringLiteral(
					"--hold requires gate d in --gates (gate d owns the hold window)");
				return out;
			}
		}
		if (b.parser.isSet(b.reps)) {
			bool ok = false;
			out.selftestReps = int_option(b.parser, b.reps, 1, &ok, &out.error);
			if (!ok)
				return out;
		}
		out.ok = true;
		return out;
	}

	// GUI (default mode).
	out.mode = Mode::Gui;
	if (b.parser.isSet(b.hold) || b.parser.isSet(b.gates) || b.parser.isSet(b.reps)) {
		out.error = QStringLiteral("--hold/--gates/--reps are only valid with --selftest");
		return out;
	}
	// Managed mode uses exactly ONE handoff transport: the discovery file OR the rendezvous pipe,
	// never both (two transports for the same instance is a launch-config error).
	if (b.parser.isSet(b.rendezvousPipe) && b.parser.isSet(b.discoveryPath)) {
		out.error = QStringLiteral(
			"--rendezvous-pipe and --discovery-path are mutually exclusive (pick one handoff transport)");
		return out;
	}
	out.startMinimized = b.parser.isSet(b.startMinimized);
	out.ownerId = b.parser.value(b.ownerId);
	out.clientName = b.parser.value(b.clientName);
	out.discoveryPath = b.parser.value(b.discoveryPath);
	out.rendezvousPipe = b.parser.value(b.rendezvousPipe);
	if (b.parser.isSet(b.port)) {
		bool ok = false;
		out.port = int_option(b.parser, b.port, 1, &ok, &out.error);
		if (!ok) {
			return out;
		}
	}
	if (b.parser.isSet(b.ownerPid)) {
		bool ok = false;
		out.ownerPid = int_option(b.parser, b.ownerPid, 1, &ok, &out.error);
		if (!ok) {
			return out;
		}
	}
	if (b.parser.isSet(b.fpsTier)) {
		bool ok = false;
		out.fpsTier = int_option(b.parser, b.fpsTier, 1, &ok, &out.error);
		if (!ok) {
			return out;
		}
	}
	out.ok = true;
	return out;
}

QString CliOptions::helpText()
{
	ParserBundle b;
	return b.parser.helpText();
}

} // namespace moxrelay
