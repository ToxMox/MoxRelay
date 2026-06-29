// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ServoTrimSim -- a pure-logic scenario sweep over the servo/trim decision core
// (ServoTrimLogic.hpp). Synthetic producer traces (seek catch-up floods, block pacing from
// fine-grained to whole-block media decode, loop wraps, delivery gaps, oversized one-time
// pushes, clock-rate skew) drive the SAME constants and the SAME window/shed decisions the
// render thread runs, with ring fill modeled as frame counts and time as 10 ms consumer
// ticks. No threads, no devices, no wall clock: thousands of deterministic scenarios per
// pass, in milliseconds -- the regression surface for the shed/latch/patience logic that
// statistical real-time repetition cannot enumerate.
//
// Every family asserts. The floor-policy families (burst-envelope release after an oversized
// push, chunk-synchronous pacing at the satisfiability boundary, re-prime after a
// capacity-class burst, mixed shed windows, post-shed landing margins) assert the policy's
// guarantees: orders are satisfiable by construction (reach-capped arming), the post-shed
// trough lands no lower than target-minus-slack, an inflated envelope releases instead of
// dictating floors forever, the prime cushion always fits the ring, and sibling orders never
// outlive the one-window validity. The sync-offset families assert the delay reserve: a
// seeded or live-set per-tap delay is regulation-invisible (delay-exempt trough rebasing),
// survives transport sheds whole, keeps re-prime reachable at the ceiling (delay-reserved
// cushion capacity), splices cleanly on live increase/decrease, and never perturbs the
// sibling order rules under mixed delays.

#pragma once

#include <string>
#include <vector>

namespace moxrelay {

struct ServoTrimSimReport {
	int scenarios = 0;
	int failed = 0;
	std::vector<std::string> failures; // first few failing scenarios, for the gate print
	double elapsedMs = 0.0;
};

// Run the full deterministic scenario sweep. Pure logic; safe on any thread, no engine state.
ServoTrimSimReport runServoTrimSim();

} // namespace moxrelay
