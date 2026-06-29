// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// PerfHarness (M2.4) -- the headless per-instance sender-ceiling harness (`moxrelay --perf`).
//
// One process per (fps tier, resolution, flush mode) measurement cell. libobs CAN re-tier in-process
// (obs_reset_video re-invoked live -- see ObsBootstrap::retier, item 05), but the harness DELIBERATELY
// CHOOSES a fresh process per cell: a hermetic run gives clean libobs frame counters and isolates each
// cell from any prior cell's GPU/thread state, which is the right measurement design here -- not a
// libobs limitation. The harness ramps N synthetic color sources (each with its own Spout sender
// through the REAL SpoutSenderEngine path), measures a window per step, and stops at the failure
// criterion:
//
//   windowed avg frame time > --budget-pct % of the frame budget (default 90), OR
//   windowed lagged/total frame ratio > --lagged-pct % (default 1).
//
// Counters are libobs-side (obs_get_average_frame_time_ns / total / lagged -- all valid headless;
// the video_output_* skipped counters stay 0 with no raw output and are deliberately NOT used)
// plus engine-local QPC timings (pass1/pass2/send/flush per frame, summed across slots).
//
// Output: JSONL on stdout (one line per measured window + one final {"ceiling": ...} summary),
// optionally appended to --out <file>. Human-readable progress goes to stderr so stdout stays
// machine-parseable. Exit 0 = run completed (whether or not the criterion fired -- the ceiling is
// DATA), 2 = boot/config failure, 3 = engine/source failure mid-run.

#pragma once

namespace moxrelay {

struct CliOptions;

int run_perf(int argc, char **argv, const CliOptions &options);

} // namespace moxrelay
