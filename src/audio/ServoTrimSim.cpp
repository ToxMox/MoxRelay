// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ServoTrimSim implementation. See the header for the scope. FIDELITY RULES (the whole value
// of the sweep rests on these):
//   - every constant and every decision comes from ServoTrimLogic.hpp -- nothing is re-derived;
//   - the per-tick consumer ordering mirrors the render thread's mixChunk: (re)prime trim ->
//     dry-out attribution -> trough sample PRE-read -> read -> dry handling -> deferred-shed
//     execution against the POST-read fill;
//   - the producer mirrors the capture tap: the window burst maximum updates BEFORE the
//     drop-on-full check (an oversized push that drops still drives the envelope's attack --
//     deliberate fidelity to the production path), drops never stamp the jump clock, floods do;
//   - the window phase mirrors the servo tick: troughs settle, then servoWindowTick decides,
//     then the orders apply to the modeled rings.
// The model's one abstraction: resampler compensation has no plant here (ring drain is a fixed
// chunk per tick, exactly the engine's own test-sink physics), so scenarios that need ppm
// actuation to converge (slow clock drift rescued by compensation) are out of scope -- the
// real-time audio gate keeps proving threads, clocks, and fades.

// std::getenv (the MOXRELAY_SIM_TRACE knob) without the CRT deprecation warning; the define
// must precede every CRT include.
#define _CRT_SECURE_NO_WARNINGS

#include "ServoTrimSim.hpp"

#include "AudioRing.hpp"
#include "ServoTrimLogic.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace moxrelay {

namespace {

constexpr int kTicksPerWindow = 25;                      // 25 x 10 ms chunks = one servo window
constexpr int kWarmupTicks = int(kWarmupSec * 100.0);    // 10 ms ticks inside the warmup
constexpr size_t kRingCapFrames = AudioRing::kDefaultFrames;

// One synthetic producer event. A Flood is a seek catch-up insertion: a rapid TRAIN of
// normal block-sized pushes (the real decode catch-up delivers block by block, so the burst
// high-water mark does NOT inflate), the first one stamping the jump clock. An OverPush is a
// genuinely oversized SINGLE push (the high-water-mark inflation cases). Gaps schedule a
// production hole; Jumps stamp without payload (a phase-continuous loop wrap). SetDelay
// changes the tap's sync-offset target (the consumer reconciles at its next tick, exactly
// like the render thread picking up a verb-thread store).
struct SimEvent {
	enum Kind { Flood, OverPush, Jump, Gap, SetDelay };
	int tick = 0;
	Kind kind = Flood;
	size_t frames = 0; // Flood / OverPush payload
	int gapTicks = 0;  // Gap length
	bool stamp = true; // Gap: stamp the late-delivery clock at resume (the producer self-report)
	int delayMs = 0;   // SetDelay target
};

struct SimTap {
	// Producer pacing.
	size_t blockFrames = 480;
	double rateFactor = 1.0; // producer clock rate relative to the consumer
	int startTick = 0;       // production phase offset
	std::vector<SimEvent> events;

	// Deterministic pacing jitter: real producers (decode timers, device packets, scheduler)
	// deliver each push milliseconds off the nominal cadence, so sawtooth bottoms VISIT a
	// range instead of parking on one phase orbit -- the trough's burst-immunity and the
	// saturating counter are designed around exactly that. A per-tap xorshift stream keeps
	// the sweep deterministic without the phase-locks a short rotating table produces;
	// only the lockstep report case turns it off.
	bool jitter = true;
	uint32_t jitterRng = 0x9E3779B9u;

	// Ring + tap state (mirrors AudioMixEngine::Tap for the decision-relevant fields).
	uint64_t fill = 0;
	size_t winBurst = 0;   // per-window push maximum (the envelope's attack input)
	size_t burstEnv = 0;   // the decaying burst envelope (committed per window)
	size_t winMaxFill = 0; // per-window post-read fill peak (the orbit's reach)
	size_t lastWinMaxFill = 0;
	bool primed = false;
	double winMinMs = -1.0;
	double lastWinTroughMs = -1.0;
	size_t pendingShed = 0;
	int pendingAgeWindows = 0; // windows survived since arming (validity instrumentation)
	bool dryPending = false;
	int64_t dryStartMs = 0;
	// Attach IS a transport edge (the engine stamps the jump clock at attachSource): the
	// cold-open turbulence window opens at t=0, so an early margin-edge dry classifies as
	// transport, exactly as it does live.
	int64_t lastJumpMs = 0;
	// Continuous push schedule (fractional ticks): the producer's own clock paces deliveries
	// at exactly one block period, so inter-push drain equals the block and the sawtooth
	// bottom is PINNED by the cushion construction (the engine's design assumption) instead
	// of random-walking on read-quantization residue. Jitter displaces each delivery
	// individually; it never accumulates into the schedule.
	double schedTick = -1.0;
	double jitterHold = 0.0;
	int gapUntilTick = -1;
	bool stampAtResume = false;

	// Sync-offset delay reserve (mirrors the engine tap): events store the target, the
	// consumer reconciles it at its next tick with the render thread's exact splice.
	int delayTargetMs = 0;
	int delayAppliedMs = 0;
	uint64_t delayFrames = 0;

	// Per-tap counters.
	uint64_t drops = 0, underruns = 0, transportGaps = 0;
	uint64_t shedsDeferred = 0, staleExecs = 0;
	int maxOrderAge = 0;
};

struct Outcome {
	uint64_t underruns = 0, transportGaps = 0, drops = 0;
	uint64_t shedsImmediate = 0, shedsDeferred = 0, staleExecs = 0;
	uint64_t armedWindows = 0, expiredWindows = 0, resyncs = 0;
	int windowsPublished = 0;
	int maxHighRun = 0;        // longest post-warmup run of published troughs above threshold
	double settledMaxMs = 0.0; // max published trough across the final 8 windows
	bool endedPrimed = true;   // every tap contributing at scenario end (re-prime reachability)
	int maxOrderAge = 0;
	double lastTroughMs = 0.0;
};

struct Scenario {
	std::string name;
	std::vector<SimTap> taps;
	int ticks = 1500;
	// Underrun budget. Default 0, the strict discipline -- and a deferred shed cannot cause a
	// dry by construction (the deferred floor anchors the landing above the worst remaining
	// drain plus the slack), so even the flood families assert zero.
	uint64_t maxUnderruns = 0;
	bool expectZeroDrops = true;
	int minSheds = 0;
	int maxSheds = -1; // -1 = unbounded
	// Resolution criteria. The high-run bound is a RESOLUTION bound, not the latch patience:
	// reach-capped orders execute within a push cycle of arming, but a standing fill larger
	// than one window's reach (or one ridden by a still-releasing burst envelope) resolves in
	// bounded STEPS -- far below the unbounded runs a real dribble/stale-shed regression
	// produces (50+). The settled tail asserts the standing fill actually resolved; its
	// threshold sits above the healthy isolated-excursion zone the real-time gate also
	// tolerates.
	int maxHighRun = 18;
	bool settledTail = true;
	uint64_t minUnderruns = 0; // attribution scenarios assert the classifier counts them
	uint64_t maxResyncs = 0;
	bool requirePrimedAtEnd = false; // re-prime reachability scenarios assert contribution
};

size_t burstNow(const SimTap &t)
{
	return std::max(t.burstEnv, t.winBurst);
}

// The burst-adaptive prime cushion plus the sync-offset reserve (the engine tap's exact
// computation: the cushion math sees the capacity minus the reserve, so cushion + reserve
// always preserves the cap's chunk of producer headroom -- re-prime stays reachable).
size_t primeTargetOf(const SimTap &t)
{
	return primeTargetFrames(burstNow(t), kRingCapFrames - size_t(t.delayFrames)) +
	       size_t(t.delayFrames);
}

void push(SimTap &t, size_t frames, int64_t nowMs, bool stampJump)
{
	if (stampJump)
		t.lastJumpMs = nowMs;
	// Window burst maximum BEFORE the drop check -- the production tap's exact order (an
	// oversized push that drops still drives the envelope's attack).
	if (frames > t.winBurst)
		t.winBurst = frames;
	if (t.fill + frames > kRingCapFrames) {
		t.drops++;
		return;
	}
	t.fill += frames;
}

void producerTick(SimTap &t, int tick, int64_t nowMs)
{
	for (const SimEvent &ev : t.events) {
		if (ev.tick != tick)
			continue;
		switch (ev.kind) {
		case SimEvent::Flood: {
			// Catch-up train: block-sized pushes in rapid succession -- the burst
			// high-water mark stays at the block size, exactly like a real seek.
			size_t left = ev.frames;
			bool first = true;
			while (left) {
				const size_t n = std::min(left, t.blockFrames);
				push(t, n, nowMs, first);
				first = false;
				left -= n;
			}
			break;
		}
		case SimEvent::OverPush:
			push(t, ev.frames, nowMs, true);
			break;
		case SimEvent::Jump:
			t.lastJumpMs = nowMs;
			break;
		case SimEvent::Gap:
			t.gapUntilTick = tick + ev.gapTicks;
			t.stampAtResume = ev.stamp;
			break;
		case SimEvent::SetDelay:
			t.delayTargetMs = ev.delayMs;
			break;
		}
	}
	if (tick < t.startTick)
		return;
	const double period = double(t.blockFrames) / (double(kChunkFrames) * t.rateFactor);
	if (t.schedTick < 0.0)
		t.schedTick = double(t.startTick);
	if (t.gapUntilTick >= 0) {
		if (tick < t.gapUntilTick)
			return; // decode hole: no production; pacing resumes at the hole's end
		if (tick == t.gapUntilTick) {
			if (t.stampAtResume)
				t.lastJumpMs = nowMs; // the producer's late-delivery self-report
			t.schedTick = std::max(t.schedTick, double(tick));
		}
	}
	// Deliver every push whose (scheduled time + its own jitter displacement) has arrived.
	// Jitter is CENTERED, +/-1 ms per delivery: real pushes land early as often as late
	// around their pacing clock (late-only jitter would bias inter-push gaps longer and
	// manufacture margin-edge dries the live engine does not show); the displacement never
	// accumulates into the schedule.
	while (t.schedTick + t.jitterHold <= double(tick)) {
		push(t, t.blockFrames, nowMs, false);
		t.schedTick += period;
		if (t.jitter) {
			uint32_t x = t.jitterRng; // xorshift32: deterministic per-tap stream
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			t.jitterRng = x;
			t.jitterHold = (double(x % 97) - 48.0) / double(kChunkFrames);
		}
	}
}

// Mirrors the render thread's per-tap chunk path for everything decision-relevant.
// Returns the number of deferred sheds executed this tick (feeds the window's resolution).
int consumerTick(SimTap &t, int64_t nowMs, double &deferredTroughMs)
{
	// Sync-offset reconcile (the render thread's exact splice): an increase drops to
	// priming -- the gap is the added reserve, stamped as deliberate transport; a decrease
	// discards exactly the removed reserve. Armed sheds drop either way (old-basis excess).
	if (t.delayTargetMs != t.delayAppliedMs) {
		const uint64_t targetFrames = uint64_t(t.delayTargetMs) * uint64_t(kLineRate) / 1000;
		if (t.delayTargetMs > t.delayAppliedMs) {
			t.lastJumpMs = nowMs;
			if (t.primed) {
				t.primed = false;
				t.dryPending = true;
				t.dryStartMs = nowMs;
			}
		} else if (t.primed) {
			t.fill -= std::min<uint64_t>(t.fill, t.delayFrames - targetFrames);
		}
		t.pendingShed = 0;
		t.pendingAgeWindows = 0;
		t.delayAppliedMs = t.delayTargetMs;
		t.delayFrames = targetFrames;
	}
	if (!t.primed) {
		const size_t prime = primeTargetOf(t);
		if (t.fill < prime)
			return 0; // still buffering its latency cushion
		if (t.fill > prime)
			t.fill = prime; // (re)prime trim: shed the overshoot beyond the cushion
		if (t.dryPending) {
			t.dryPending = false;
			if (dryIsTransport(t.lastJumpMs, t.dryStartMs))
				t.transportGaps++;
			else
				t.underruns++;
		}
		t.primed = true;
		t.pendingShed = 0; // the prime trim above already squared the fill
		t.pendingAgeWindows = 0;
	}
	// The regulation basis is delay-exempt (the engine's trough rebasing): the window trough
	// subtracts the applied reserve, so a deliberate delay never reads as sheddable excess.
	const double fillMs = double(t.fill) * 1000.0 / double(kLineRate) - double(t.delayAppliedMs);
	if (t.winMinMs < 0.0 || fillMs < t.winMinMs)
		t.winMinMs = fillMs; // the servo regulates this window trough (PRE-read)
	const size_t got = size_t(std::min<uint64_t>(t.fill, kChunkFrames));
	t.fill -= got;
	const bool dry = got < kChunkFrames;
	if (dry) {
		t.primed = false;
		t.dryPending = true;
		t.dryStartMs = nowMs;
	} else if (size_t(t.fill) > t.winMaxFill) {
		t.winMaxFill = size_t(t.fill); // post-read peak: the orbit's reach
	}
	if (t.pendingShed && t.primed && !dry &&
	    deferredShedReady(t.pendingShed, size_t(t.fill), burstNow(t))) {
		t.fill -= t.pendingShed;
		deferredTroughMs = t.lastWinTroughMs;
		if (t.pendingAgeWindows >= 1)
			t.staleExecs++; // executed past its arming window's validity
		t.maxOrderAge = std::max(t.maxOrderAge, t.pendingAgeWindows);
		t.pendingShed = 0;
		t.shedsDeferred++;
		return 1;
	}
	return 0;
}

// Set the environment variable MOXRELAY_SIM_TRACE to a scenario name to dump that scenario's
// per-window decisions to stderr (development aid; off otherwise).
bool traceWanted(const std::string &name)
{
	const char *t = std::getenv("MOXRELAY_SIM_TRACE");
	return t && name == t;
}

Outcome runScenario(Scenario &s)
{
	const bool trace = traceWanted(s.name);
	Outcome out;
	ServoTrimState state;
	uint64_t shedDoneCount = 0;
	double shedDoneTrough = 0.0;
	int highRun = 0;
	std::vector<double> published;
	std::vector<TrimTapView> views;

	for (int tick = 0; tick < s.ticks; ++tick) {
		const int64_t nowMs = int64_t(tick) * 10;
		const double nowSec = double(tick) * 0.010;

		// Consumer BEFORE producer: a real push lands strictly between two reads, so it
		// becomes visible to the read AFTER it -- producer-first would land pushes exactly
		// AT the read instant (a measure-zero alignment) and systematically understate the
		// post-read peak the deferred-shed test keys on by one whole chunk.
		for (SimTap &t : s.taps)
			shedDoneCount += uint64_t(consumerTick(t, nowMs, shedDoneTrough));
		for (SimTap &t : s.taps)
			producerTick(t, tick, nowMs);

		if ((tick + 1) % kTicksPerWindow != 0)
			continue;

		// Window phase: settle troughs, decide, apply -- the servo tick's exact shape.
		double troughSum = 0.0;
		int troughCount = 0;
		for (SimTap &t : s.taps) {
			if (t.winMinMs >= 0.0) {
				troughSum += t.winMinMs;
				troughCount++;
				t.lastWinTroughMs = t.winMinMs;
				t.winMinMs = -1.0;
			}
			t.lastWinMaxFill = t.winMaxFill;
			t.winMaxFill = 0;
			// The envelope commits its release step every window boundary -- the
			// render thread's exact cadence, primed or not (a re-buffering tap's
			// cushion normalizes while it refills).
			t.burstEnv = burstEnvelopeStep(t.burstEnv, t.winBurst);
			t.winBurst = 0;
		}
		if (!troughCount)
			continue;
		const double trough = troughSum / double(troughCount);
		out.windowsPublished++;
		published.push_back(trough);
		// The high-run metric uses the trim threshold plus 1 ms of measurement tolerance --
		// the same calibration the real-time gate documents (a boundary-kissing window is
		// healthy regulation; a REGRESSION is a run the trim never sheds).
		if (tick >= kWarmupTicks) {
			if (trough > kTrimTroughMs + 1.0) {
				highRun++;
				out.maxHighRun = std::max(out.maxHighRun, highRun);
			} else {
				highRun = 0;
			}
		}
		out.lastTroughMs = trough;

		// Standing orders age one window before this tick's orders land.
		for (SimTap &t : s.taps) {
			if (t.pendingShed) {
				t.pendingAgeWindows++;
				out.maxOrderAge = std::max(out.maxOrderAge, t.pendingAgeWindows);
			}
		}

		views.clear();
		for (SimTap &t : s.taps) {
			TrimTapView v;
			v.eligible = t.primed;
			v.lastWinTroughMs = t.lastWinTroughMs;
			v.fillFrames = size_t(t.fill);
			v.winMaxFillFrames = t.lastWinMaxFill;
			v.burstEnvFrames = t.burstEnv;
			views.push_back(v);
		}
		const ServoWindowResult res =
			servoWindowTick(state, nowSec, 0.0, trough, shedDoneCount != 0, views);
		if (trace) {
			std::fprintf(stderr,
				     "[sim %s] t=%d trough=%.2f hw=%d armed=%d res{def=%d imm=%d exp=%d arm=%d}",
				     s.name.c_str(), tick, trough, state.highWindows,
				     state.shedArmed ? 1 : 0, res.deferredResolved ? 1 : 0,
				     res.immediateShed ? 1 : 0, res.ordersExpired ? 1 : 0, res.armedCount);
			for (const SimTap &t : s.taps)
				std::fprintf(stderr, " tap{lw=%.2f fill=%llu env=%zu reach=%zu pend=%zu}",
					     t.lastWinTroughMs, (unsigned long long)t.fill, t.burstEnv,
					     t.lastWinMaxFill, t.pendingShed);
			std::fprintf(stderr, "\n");
		}

		if (res.deferredResolved) {
			out.transportGaps++;
			for (size_t i = 0; i < s.taps.size(); ++i) {
				if (res.orders[i].clearPending) {
					s.taps[i].pendingShed = 0;
					s.taps[i].pendingAgeWindows = 0;
				}
			}
			shedDoneCount = 0;
			continue;
		}
		if (res.ordersExpired)
			out.expiredWindows++;
		for (size_t i = 0; i < s.taps.size(); ++i) {
			const TrimTapOrder &o = res.orders[i];
			SimTap &t = s.taps[i];
			if (o.clearPending) {
				t.pendingShed = 0;
				t.pendingAgeWindows = 0;
			}
			if (o.shedNowFrames) {
				const uint64_t d = std::min<uint64_t>(t.fill, o.shedNowFrames);
				t.fill -= d;
				t.pendingShed = 0;
				out.shedsImmediate++;
			}
			if (o.armFrames) {
				t.pendingShed = o.armFrames;
				t.pendingAgeWindows = 0;
			}
		}
		if (res.immediateShed)
			out.transportGaps++;
		if (res.armedCount)
			out.armedWindows++;
		if (res.resync) {
			for (SimTap &t : s.taps) {
				const size_t prime = primeTargetOf(t);
				if (t.fill > prime)
					t.fill = prime;
			}
			out.resyncs++;
		}
	}

	for (const SimTap &t : s.taps) {
		out.underruns += t.underruns;
		out.transportGaps += t.transportGaps;
		out.drops += t.drops;
		out.shedsDeferred += t.shedsDeferred;
		out.staleExecs += t.staleExecs;
		out.maxOrderAge = std::max(out.maxOrderAge, t.maxOrderAge);
		if (!t.primed)
			out.endedPrimed = false;
	}
	const size_t tail = published.size() > 8 ? published.size() - 8 : 0;
	for (size_t i = tail; i < published.size(); ++i)
		out.settledMaxMs = std::max(out.settledMaxMs, published[i]);
	return out;
}

void judge(const Scenario &s, const Outcome &o, ServoTrimSimReport &rep)
{
	rep.scenarios++;
	std::string why;
	const uint64_t sheds = o.shedsImmediate + o.shedsDeferred;
	if (o.underruns > s.maxUnderruns)
		why += " underruns=" + std::to_string(o.underruns);
	if (s.minUnderruns && o.underruns < s.minUnderruns)
		why += " underruns-not-counted";
	if (s.expectZeroDrops && o.drops)
		why += " drops=" + std::to_string(o.drops);
	if (sheds < uint64_t(s.minSheds))
		why += " sheds=" + std::to_string(sheds) + "<" + std::to_string(s.minSheds);
	if (s.maxSheds >= 0 && sheds > uint64_t(s.maxSheds))
		why += " sheds=" + std::to_string(sheds) + ">" + std::to_string(s.maxSheds);
	if (s.settledTail && o.settledMaxMs > kTrimTroughMs + 3.0)
		why += " unsettled=" + std::to_string(o.settledMaxMs).substr(0, 5) + "ms";
	if (s.maxHighRun >= 0 && o.maxHighRun > s.maxHighRun)
		why += " highrun=" + std::to_string(o.maxHighRun);
	if (o.staleExecs)
		why += " stale-exec=" + std::to_string(o.staleExecs);
	if (o.resyncs > s.maxResyncs)
		why += " resyncs=" + std::to_string(o.resyncs);
	if (s.requirePrimedAtEnd && !o.endedPrimed)
		why += " not-primed-at-end";
	if (!why.empty()) {
		rep.failed++;
		if (rep.failures.size() < 500)
			rep.failures.push_back(s.name + ":" + why);
	}
}

// ---------------------------------------------------------------------------------------------
// Direct-drive checks of the window decision itself (no fill dynamics): latch arithmetic,
// patience boundaries, order validity, resolution clearing, EMA reset, saturation backstop.
// ---------------------------------------------------------------------------------------------
void directDriveChecks(ServoTrimSimReport &rep)
{
	auto fail = [&](const char *name, const char *why) {
		rep.failed++;
		if (rep.failures.size() < 12)
			rep.failures.push_back(std::string(name) + ": " + why);
	};
	const double t0 = kWarmupSec + 1.0; // clear of the warmup gate
	auto view = [](double troughMs, size_t fill, size_t burst) {
		TrimTapView v;
		v.eligible = true;
		v.lastWinTroughMs = troughMs;
		v.fillFrames = fill;
		v.winMaxFillFrames = fill; // direct drive: the orbit peak is the instant itself
		v.burstEnvFrames = burst;
		return v;
	};
	const double hi = kTrimTroughMs + 6.0;       // an over-threshold trough
	const size_t lean = shedBridgeFloorFrames(480); // can never cover a shed IMMEDIATELY
						        // (deferred reach = exactly one quantum)

	// 1. Patience latch: exactly kTrimWindows high windows arm a deferred order (lean fill).
	{
		rep.scenarios++;
		ServoTrimState st;
		ServoWindowResult r;
		for (int w = 0; w < kTrimWindows; ++w)
			r = servoWindowTick(st, t0 + 0.25 * w, 0.0, hi, false, {view(hi, lean, 480)});
		if (!r.armedCount || !st.shedArmed)
			fail("unit.latch-at-patience", "no order armed at the patience boundary");
		ServoTrimState st2;
		ServoWindowResult r2;
		for (int w = 0; w < kTrimWindows - 1; ++w)
			r2 = servoWindowTick(st2, t0 + 0.25 * w, 0.0, hi, false, {view(hi, lean, 480)});
		if (r2.armedCount || st2.shedArmed)
			fail("unit.latch-at-patience", "armed BEFORE the patience boundary");
	}
	// 2. Threshold is strict: a trough exactly AT kTrimTroughMs never counts high.
	{
		rep.scenarios++;
		ServoTrimState st;
		for (int w = 0; w < kTrimWindows * 3; ++w) {
			const ServoWindowResult r = servoWindowTick(st, t0 + 0.25 * w, 0.0, kTrimTroughMs,
								    false,
								    {view(kTrimTroughMs, lean, 480)});
			if (r.armedCount || r.immediateShed) {
				fail("unit.strict-threshold", "boundary trough latched the trim");
				break;
			}
		}
	}
	// 3. Saturating counter: a single low window only decrements (never resets) the count.
	{
		rep.scenarios++;
		ServoTrimState st;
		const double lo = kTroughTargetMs;
		const double seq[] = {hi, hi, lo, hi, hi};       // net: 1,2,1,2,3 -> latch at the 5th
		ServoWindowResult r;
		for (int w = 0; w < 5; ++w)
			r = servoWindowTick(st, t0 + 0.25 * w, 0.0, seq[w], false,
					    {view(seq[w], lean, 480)});
		if (!r.armedCount)
			fail("unit.saturating-counter", "net count did not survive the dip");
	}
	// 4. One-window validity: an armed order expires at the next tick (cleared, re-armable).
	{
		rep.scenarios++;
		ServoTrimState st;
		for (int w = 0; w < kTrimWindows; ++w)
			servoWindowTick(st, t0 + 0.25 * w, 0.0, hi, false, {view(hi, lean, 480)});
		const ServoWindowResult r = servoWindowTick(st, t0 + 0.25 * kTrimWindows, 0.0, hi, false,
							    {view(hi, lean, 480)});
		if (!r.ordersExpired || !r.orders[0].clearPending)
			fail("unit.order-validity", "stale order survived its window");
		if (!r.armedCount)
			fail("unit.order-validity", "persistent step did not re-arm fresh");
	}
	// 5. Resolution clears every sibling: a consumer-executed shed retires ALL orders + state.
	{
		rep.scenarios++;
		ServoTrimState st;
		std::vector<TrimTapView> two = {view(hi, lean, 480), view(hi, lean, 480)};
		for (int w = 0; w < kTrimWindows; ++w)
			servoWindowTick(st, t0 + 0.25 * w, 0.0, hi, false, two);
		const ServoWindowResult r =
			servoWindowTick(st, t0 + 0.25 * kTrimWindows, 0.0, hi, true, two);
		if (!r.deferredResolved || !r.orders[0].clearPending || !r.orders[1].clearPending)
			fail("unit.resolution-clears-all", "sibling order survived the resolution");
		if (st.shedArmed || st.highWindows != 0 || st.ema != 0.0)
			fail("unit.resolution-clears-all", "regulation state not reset");
	}
	// 6. Immediate shed resets the EMA and the patience count.
	{
		rep.scenarios++;
		ServoTrimState st;
		const size_t fat = shedBridgeFloorFrames(480) + size_t(kLineRate); // covers any excess
		ServoWindowResult r;
		for (int w = 0; w < kTrimWindows; ++w)
			r = servoWindowTick(st, t0 + 0.25 * w, 0.0, hi, false, {view(hi, fat, 480)});
		if (!r.immediateShed || !r.orders[0].shedNowFrames)
			fail("unit.immediate-shed", "satisfiable excess did not shed at the latch");
		if (st.ema != 0.0 || st.highWindows != 0)
			fail("unit.immediate-shed", "EMA/patience not reset by the shed");
		if (r.orders[0].shedNowFrames != trimShedExcessFrames(hi))
			fail("unit.immediate-shed", "shed amount is not the trough excess");
	}
	// 7. Mixed window: one tap sheds immediately while the sibling arms in the SAME window --
	//    the sibling's order rides under the validity latch and expires next window instead
	//    of surviving stale.
	{
		rep.scenarios++;
		ServoTrimState st;
		const size_t fat = shedBridgeFloorFrames(480) + size_t(kLineRate);
		std::vector<TrimTapView> two = {view(hi, fat, 480), view(hi, lean, 480)};
		ServoWindowResult r;
		for (int w = 0; w < kTrimWindows; ++w)
			r = servoWindowTick(st, t0 + 0.25 * w, 0.0, hi, false, two);
		if (!r.immediateShed || !r.orders[0].shedNowFrames)
			fail("unit.mixed-window-latch", "the fat tap did not shed immediately");
		if (!r.orders[1].armFrames)
			fail("unit.mixed-window-latch", "the lean sibling did not arm");
		if (!st.shedArmed)
			fail("unit.mixed-window-latch", "sibling order rides without its expiry latch");
		const ServoWindowResult r2 =
			servoWindowTick(st, t0 + 0.25 * kTrimWindows, 0.0, hi, false, two);
		if (!r2.ordersExpired || !r2.orders[1].clearPending)
			fail("unit.mixed-window-latch", "sibling order survived its window");
	}
	// 8. Warmup gate: nothing happens inside the post-start transient, including resolution.
	{
		rep.scenarios++;
		ServoTrimState st;
		const ServoWindowResult r =
			servoWindowTick(st, kWarmupSec * 0.5, 0.0, hi, true, {view(hi, lean, 480)});
		if (r.deferredResolved || r.servoCommanded || r.armedCount)
			fail("unit.warmup-gate", "the warmup window acted");
	}
	// 9. Saturation backstop: a persistently clamped command resyncs once after the dwell.
	{
		rep.scenarios++;
		ServoTrimState st;
		const double lo = 1.0; // huge negative error -> clamped command, no trim path
		bool resynced = false;
		double firstResyncAt = -1.0;
		// EMA tau means ~36 windows to saturate the command, then the dwell adds ~20 more.
		for (int w = 0; w < 100; ++w) {
			const double now = t0 + 0.25 * w;
			const ServoWindowResult r =
				servoWindowTick(st, now, 0.0, lo, false, {view(lo, lean, 480)});
			if (r.resync) {
				resynced = true;
				firstResyncAt = now;
				break;
			}
		}
		if (!resynced)
			fail("unit.saturation-resync", "no resync under a sustained clamped command");
		else if (firstResyncAt - t0 < kSaturationSec)
			fail("unit.saturation-resync", "resync fired before the dwell");
		if (resynced && st.ema != 0.0)
			fail("unit.saturation-resync", "EMA not reset by the resync");
	}
}

// Scenario builders ---------------------------------------------------------------------------

SimTap makeTap(size_t block, int startTick, double rate = 1.0)
{
	SimTap t;
	t.blockFrames = block;
	t.startTick = startTick;
	t.rateFactor = rate;
	// Per-tap deterministic jitter stream, decorrelated across the grid (never zero --
	// xorshift32 has a fixed point there).
	t.jitterRng = 0x9E3779B9u ^ uint32_t(block * 2654435761u) ^ uint32_t((startTick + 1) * 40503u);
	if (!t.jitterRng)
		t.jitterRng = 1u;
	return t;
}

// A seek-rebuffer insertion: source-side timestamp smoothing keeps the pacing alive while a
// catch-up flood lands on top of it -- the ring never dries, the fill steps up whole. (A seek
// that DOES dry the ring takes the re-prime trim path instead; family E covers that.)
void floodAt(SimTap &t, int tick, size_t floodFrames)
{
	SimEvent flood;
	flood.tick = tick;
	flood.kind = SimEvent::Flood;
	flood.frames = floodFrames;
	t.events.push_back(flood);
}

// A tap whose sync-offset reserve is seeded before the first contribution (the attach-seed
// path: no splice, the tap primes at cushion + reserve from silence).
SimTap makeDelayedTap(size_t block, int startTick, int delayMs, double rate = 1.0)
{
	SimTap t = makeTap(block, startTick, rate);
	t.delayTargetMs = delayMs;
	t.delayAppliedMs = delayMs;
	t.delayFrames = uint64_t(delayMs) * uint64_t(kLineRate) / 1000;
	return t;
}

// A live sync-offset change (the verb-thread store; the consumer reconciles next tick).
void setDelayAt(SimTap &t, int tick, int delayMs)
{
	SimEvent ev;
	ev.tick = tick;
	ev.kind = SimEvent::SetDelay;
	ev.delayMs = delayMs;
	t.events.push_back(ev);
}

} // namespace

ServoTrimSimReport runServoTrimSim()
{
	using clock = std::chrono::steady_clock;
	const auto begun = clock::now();
	ServoTrimSimReport rep;

	directDriveChecks(rep);

	// Fine blocks are deliberately NON-DIVISORS of the consumer chunk: a producer push-locked
	// to the exact read phase forever is a measure-zero physical case (real device clocks
	// slide and schedulers jitter), and at perfect lockstep the post-read fill sits exactly
	// ON the bridge floor -- that boundary is covered as a reported floor-family case below,
	// not smeared across every assert family. The coarse end stops at 4096 (~85 ms, the
	// coarsest real producer block known): beyond ~half a servo window the trough-window
	// semantic itself degrades (mid-sawtooth phantom troughs), outside the engine's
	// validated producer envelope.
	const size_t blocks[] = {441, 512, 1024, 2048, 4096};

	// A/B: steady producers across pacing and phase -- at most the prime-overshoot shed (the
	// non-divisor pacing can split it across the boundary), then converged; never an
	// underrun, never a recurring trim.
	for (size_t block : blocks) {
		for (int phase = 0; phase < kTicksPerWindow; ++phase) {
			Scenario s;
			s.name = "steady.b" + std::to_string(block) + ".p" + std::to_string(phase);
			s.taps.push_back(makeTap(block, phase));
			s.ticks = 1500;
			s.maxSheds = 3; // prime overshoot + orbit threshold kisses, never recurring
			judge(s, runScenario(s), rep);
		}
	}

	// C: seek catch-up floods leave a standing fill the trim must shed and settle. The grid
	// tops out at 200 ms: beyond that an insertion on top of the coarse sawtooth peak can
	// legitimately cross ring capacity (drop-on-full physics, not trim behavior).
	for (size_t block : {size_t(512), size_t(2048), size_t(4096)}) {
		for (int floodMs = 30; floodMs <= 200; floodMs += 10) {
			for (int phase = 0; phase < kTicksPerWindow; phase += 2) {
				Scenario s;
				s.name = "seek.b" + std::to_string(block) + ".f" + std::to_string(floodMs) +
					 ".p" + std::to_string(phase);
				SimTap t = makeTap(block, 0);
				floodAt(t, kWarmupTicks + 150 + phase,
					size_t(floodMs) * size_t(kLineRate) / 1000);
				s.taps.push_back(t);
				s.ticks = 2200;
				s.minSheds = 1; // the standing fill must shed -- and dry-free (default budget 0)
				judge(s, runScenario(s), rep);
			}
		}
	}

	// D: phase-continuous loop wraps -- stamps without payload disturb nothing.
	for (size_t block : blocks) {
		Scenario s;
		s.name = "loopwrap.b" + std::to_string(block);
		SimTap t = makeTap(block, 0);
		for (int w = 1; w <= 4; ++w) {
			SimEvent j;
			j.tick = kWarmupTicks + w * 400;
			j.kind = SimEvent::Jump;
			t.events.push_back(j);
		}
		s.taps.push_back(t);
		s.ticks = 2200;
		s.maxSheds = 3; // a wrap stamp moves no fill; only orbit housekeeping sheds
		judge(s, runScenario(s), rep);
	}

	// E: a decode hole dries the ring; the resume self-report classifies it as transport.
	for (size_t block : {size_t(512), size_t(4096)}) {
		for (int gapTicks = 30; gapTicks <= 90; gapTicks += 30) {
			Scenario s;
			s.name = "gapdry.b" + std::to_string(block) + ".g" + std::to_string(gapTicks);
			SimTap t = makeTap(block, 0);
			SimEvent gap;
			gap.tick = kWarmupTicks + 200;
			gap.kind = SimEvent::Gap;
			gap.gapTicks = gapTicks;
			t.events.push_back(gap);
			s.taps.push_back(t);
			s.ticks = 2000;
			s.maxSheds = 3; // prime overshoot + post-resume trims, never recurring
			judge(s, runScenario(s), rep);
		}
	}

	// E2: the consumer genuinely outruns a silent stall (no self-report) -- the classifier
	// MUST count an underrun (the strict zero-underrun discipline depends on this side too).
	{
		Scenario s;
		s.name = "attrib.true-underrun";
		SimTap t = makeTap(512, 0);
		SimEvent gap;
		gap.tick = kWarmupTicks + int(kTransportTurbulenceMs / 10) + 200; // clear of turbulence
		gap.kind = SimEvent::Gap;
		gap.gapTicks = 60;
		gap.stamp = false; // a stall the producer never reports
		t.events.push_back(gap);
		s.taps.push_back(t);
		s.ticks = kWarmupTicks + int(kTransportTurbulenceMs / 10) + 600;
		s.maxUnderruns = 99;
		s.minUnderruns = 1;
		s.maxSheds = 3;
		s.maxHighRun = -1;     // the classifier is the assert here, not regulation
		s.settledTail = false; // the stall + re-prime can leave the tail anywhere healthy
		judge(s, runScenario(s), rep);
	}

	// F: two concurrent taps, both seeking inside one window -- the resolved shed on one tap
	// must retire the sibling's order (the multi-tap clearing rule), and a MIXED window (one
	// immediate shed + one fresh arm) must leave the sibling's order under the validity
	// latch: no stale executions anywhere (the judge asserts stale-exec == 0).
	for (int phase = 0; phase < kTicksPerWindow; ++phase) {
		for (int floodMs : {60, 120, 200}) {
			Scenario s;
			s.name = "twintap.f" + std::to_string(floodMs) + ".p" + std::to_string(phase);
			SimTap a = makeTap(4096, 0);
			SimTap b = makeTap(4096, 7);
			const int at = kWarmupTicks + 150 + phase;
			floodAt(a, at, size_t(floodMs) * size_t(kLineRate) / 1000);
			floodAt(b, at, size_t(floodMs * 2 / 3) * size_t(kLineRate) / 1000);
			s.taps.push_back(a);
			s.taps.push_back(b);
			s.ticks = 2400;
			s.minSheds = 1;
			judge(s, runScenario(s), rep);
		}
	}

	// G: burst-envelope release after an oversized push -- delivered, or dropped at capacity
	// (the window burst maximum updates either way, production fidelity). The push both
	// inflates the envelope and (when delivered) steps the fill; the trim must shed the step
	// in reach-capped steps as the envelope releases, settle back to target, and never dry.
	// The high-run bound here is the release pacing (a few envelope steps, each gated by the
	// floors it lowers), not the latch patience.
	for (size_t block : {size_t(512), size_t(2048), size_t(4096)}) {
		for (size_t overMs : {40, 60, 90, 120, 160, 200, 250, 300}) {
			for (int floodMs = 30; floodMs <= 200; floodMs += 20) {
				Scenario s;
				s.name = "floor.b" + std::to_string(block) + ".o" + std::to_string(overMs) +
					 ".f" + std::to_string(floodMs);
				SimTap t = makeTap(block, 0);
				SimEvent over;
				over.tick = kWarmupTicks + 100;
				over.kind = SimEvent::OverPush; // the single oversized push IS the case
				over.frames = overMs * size_t(kLineRate) / 1000;
				t.events.push_back(over);
				floodAt(t, kWarmupTicks + 400,
					size_t(floodMs) * size_t(kLineRate) / 1000);
				s.taps.push_back(t);
				s.ticks = 3000;
				s.minSheds = 1;
				s.expectZeroDrops = false; // a capacity-crossing push drops by design
				s.maxHighRun = 30;         // envelope-release pacing, asserted bounded
				judge(s, runScenario(s), rep);
			}
		}
	}

	// H: a ring-capacity flood drops at the producer, never misattributes as an underrun --
	// and with the envelope releasing afterward, the orbit settles instead of riding an
	// inflated floor forever.
	{
		Scenario s;
		s.name = "capdrop.b4096";
		SimTap t = makeTap(4096, 0);
		SimEvent over;
		over.tick = kWarmupTicks + 150;
		over.kind = SimEvent::OverPush;
		over.frames = kRingCapFrames; // guaranteed over capacity on a primed ring
		t.events.push_back(over);
		s.taps.push_back(t);
		s.ticks = 1800;
		s.expectZeroDrops = false;
		s.maxSheds = 3;
		s.maxHighRun = 30; // envelope-release pacing
		judge(s, runScenario(s), rep);
	}

	// I: mild producer-clock skew (fast side): the trim periodically relieves the creep the
	// bounded servo cannot see in this plant-free model; never an underrun.
	for (double rate : {1.0003, 1.0006}) {
		Scenario s;
		s.name = "skewfast.r" + std::to_string(int((rate - 1.0) * 1e6)) + "ppm";
		s.taps.push_back(makeTap(480, 0, rate));
		s.ticks = 4000;
		s.minSheds = 1;
		s.maxSheds = -1;
		s.maxHighRun = -1;     // boundary-crawl execution can straddle a window edge --
				       // model pacing, not the engine contract; relief is the assert
		s.settledTail = false; // mid-climb at scenario end is legitimate
		judge(s, runScenario(s), rep);
	}

	// G2: perfect push/read lockstep -- chunk-synchronous pacing puts the pre-read trough
	// sample one whole chunk above the true just-before-push bottom, the satisfiability
	// boundary case. The policy must converge it to a quiet bounded state (a reach-capped
	// shed or a quiet hold within one chunk of target), never an arm/expire loop and never a
	// dry. Physically reachable only as a transient (real clocks slide through it).
	{
		Scenario s;
		s.name = "floor.lockstep-equality";
		SimTap t = makeTap(480, 0); // exact chunk-sized, phase-locked pacing
		t.jitter = false;           // the lockstep IS the case
		s.taps.push_back(t);
		s.ticks = 2000;
		s.maxSheds = 3;
		// The chunk-quantized sample can park up to one chunk above target; the settled
		// tail (target + 4 ms < threshold + 3 ms) and the default zero-underrun budget
		// are the asserts; the stalemate hold keeps it order-free and quiet.
		judge(s, runScenario(s), rep);
	}

	// G3: a capacity-class burst drives the envelope's attack to the ring's own size; the
	// CAPPED prime cushion still fits the ring, so a later dry-out re-primes (with the
	// envelope releasing back to real pacing) instead of waiting on a cushion the ring can
	// never hold -- contribution always resumes.
	{
		SimTap t = makeTap(480, 0);
		SimEvent over;
		over.tick = kWarmupTicks + 100;
		over.kind = SimEvent::OverPush;
		over.frames = kRingCapFrames; // drops at capacity; the attack happens anyway
		t.events.push_back(over);
		SimEvent gap;
		gap.tick = kWarmupTicks + 300;
		gap.kind = SimEvent::Gap;
		gap.gapTicks = 30;
		t.events.push_back(gap);
		Scenario s;
		s.name = "floor.reprime-unreachable";
		s.taps.push_back(t);
		s.ticks = 3000;
		s.expectZeroDrops = false; // the capacity-class push drops by design
		s.maxHighRun = 30;         // envelope-release pacing around the re-prime
		s.requirePrimedAtEnd = true;
		judge(s, runScenario(s), rep);
	}

	// J: sync-offset steady state -- a seeded delay reserve is regulation-invisible: the
	// delay-exempt trough converges exactly like the undelayed baseline (no latch, no
	// recurring trim, no dry) with the reserve resident the whole run.
	for (size_t block : blocks) {
		for (int delayMs : {30, 120, 250, 500, 950}) {
			for (int phase = 0; phase < kTicksPerWindow; phase += 5) {
				Scenario s;
				s.name = "sync.steady.b" + std::to_string(block) + ".d" +
					 std::to_string(delayMs) + ".p" + std::to_string(phase);
				s.taps.push_back(makeDelayedTap(block, phase, delayMs));
				s.ticks = 1800;
				s.maxSheds = 3;
				judge(s, runScenario(s), rep);
			}
		}
	}

	// K: a transport step on a delayed tap sheds back to the delay-exempt target -- the
	// reserve survives the shed whole (the landing sits the entire reserve above the floors,
	// which consume RAW fill and stay conservative by construction).
	for (size_t block : {size_t(512), size_t(2048), size_t(4096)}) {
		for (int delayMs : {120, 500, 950}) {
			for (int floodMs : {60, 120, 200}) {
				Scenario s;
				s.name = "sync.seek.b" + std::to_string(block) + ".d" +
					 std::to_string(delayMs) + ".f" + std::to_string(floodMs);
				SimTap t = makeDelayedTap(block, 0, delayMs);
				floodAt(t, kWarmupTicks + 150, size_t(floodMs) * size_t(kLineRate) / 1000);
				s.taps.push_back(t);
				s.ticks = 2400;
				s.minSheds = 1;
				judge(s, runScenario(s), rep);
			}
		}
	}

	// L: a live offset ENABLE mid-stream -- the deliberate re-prime gap classifies as
	// transport (never an underrun: the reconcile stamps the jump clock), the tap re-primes
	// at cushion + reserve, and the trough re-converges on the new basis.
	for (size_t block : {size_t(512), size_t(4096)}) {
		for (int delayMs : {60, 250, 950}) {
			for (int phase : {0, 12}) {
				Scenario s;
				s.name = "sync.enable.b" + std::to_string(block) + ".d" +
					 std::to_string(delayMs) + ".p" + std::to_string(phase);
				SimTap t = makeTap(block, 0);
				setDelayAt(t, kWarmupTicks + 200 + phase, delayMs);
				s.taps.push_back(t);
				s.ticks = 2400;
				s.maxSheds = 3;
				s.requirePrimedAtEnd = true;
				judge(s, runScenario(s), rep);
			}
		}
	}

	// M: the wedge corner at the ceiling -- a capacity-class envelope flood on top of the
	// maximum reserve. The delay-reserved cushion algebra keeps cushion + reserve at most one
	// chunk under capacity at EVERY envelope state, so re-prime stays reachable (the
	// permanent-silence wedge stays closed) and the post-prime standing step resolves as the
	// envelope releases.
	for (int delayMs : {500, 950}) {
		for (int gapTicks : {0, 30}) {
			Scenario s;
			s.name = "sync.wedge.d" + std::to_string(delayMs) +
				 (gapTicks ? ".gapdry" : ".steady");
			SimTap t = makeDelayedTap(480, 0, delayMs);
			SimEvent over;
			over.tick = kWarmupTicks + 100;
			over.kind = SimEvent::OverPush;
			over.frames = kRingCapFrames; // drops at capacity; the attack happens anyway
			t.events.push_back(over);
			if (gapTicks) {
				SimEvent gap;
				gap.tick = kWarmupTicks + 300;
				gap.kind = SimEvent::Gap;
				gap.gapTicks = gapTicks;
				t.events.push_back(gap);
			}
			s.taps.push_back(t);
			s.ticks = 3600;
			s.expectZeroDrops = false; // the capacity-class push drops by design
			s.maxHighRun = 30;         // envelope-release pacing around the re-prime
			s.requirePrimedAtEnd = true;
			judge(s, runScenario(s), rep);
		}
	}

	// N: a decrease cut removes exactly the reserve delta -- the trough is already at target
	// on the new basis, so the latch never fires (no double-shed) and the cut cannot dry.
	for (size_t block : {size_t(512), size_t(4096)}) {
		for (auto fromTo : {std::pair<int, int>{250, 0}, {950, 0}, {950, 475}}) {
			Scenario s;
			s.name = "sync.decrease.b" + std::to_string(block) + ".d" +
				 std::to_string(fromTo.first) + "to" + std::to_string(fromTo.second);
			SimTap t = makeDelayedTap(block, 0, fromTo.first);
			setDelayAt(t, kWarmupTicks + 300, fromTo.second);
			s.taps.push_back(t);
			s.ticks = 2200;
			s.maxSheds = 3;
			judge(s, runScenario(s), rep);
		}
	}

	// O: delay + transport interaction -- a seek flood and a later decode hole on a delayed
	// tap: attribution stays transport-clean through both, the re-prime carries the reserve,
	// and the standing step resolves on the delay-exempt basis.
	for (int delayMs : {120, 950}) {
		for (size_t block : {size_t(512), size_t(4096)}) {
			Scenario s;
			s.name = "sync.transport.b" + std::to_string(block) + ".d" +
				 std::to_string(delayMs);
			SimTap t = makeDelayedTap(block, 0, delayMs);
			floodAt(t, kWarmupTicks + 150, size_t(120) * size_t(kLineRate) / 1000);
			SimEvent gap;
			gap.tick = kWarmupTicks + 600;
			gap.kind = SimEvent::Gap;
			gap.gapTicks = 60;
			t.events.push_back(gap);
			s.taps.push_back(t);
			s.ticks = 2800;
			s.minSheds = 1;
			s.requirePrimedAtEnd = true;
			judge(s, runScenario(s), rep);
		}
	}

	// P: mixed delays across taps -- per-tap rebasing isolates the bases, so the sibling
	// order rules (resolution clearing, one-window validity, no stale executions) hold
	// unchanged with one reserve-carrying tap and one live tap flooded in the same window.
	for (int delayMs : {120, 500}) {
		for (int floodMs : {60, 200}) {
			for (int phase : {0, 12}) {
				Scenario s;
				s.name = "sync.mixed.d" + std::to_string(delayMs) + ".f" +
					 std::to_string(floodMs) + ".p" + std::to_string(phase);
				SimTap a = makeDelayedTap(4096, 0, delayMs);
				SimTap b = makeTap(4096, 7);
				const int at = kWarmupTicks + 150 + phase;
				floodAt(a, at, size_t(floodMs) * size_t(kLineRate) / 1000);
				floodAt(b, at, size_t(floodMs * 2 / 3) * size_t(kLineRate) / 1000);
				s.taps.push_back(a);
				s.taps.push_back(b);
				s.ticks = 2400;
				s.minSheds = 1;
				judge(s, runScenario(s), rep);
			}
		}
	}

	rep.elapsedMs =
		std::chrono::duration<double, std::milli>(clock::now() - begun).count();
	return rep;
}

} // namespace moxrelay
