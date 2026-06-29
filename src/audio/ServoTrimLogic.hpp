// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// ServoTrimLogic -- the audio engine's fill-servo / standing-fill-trim DECISION CORE, extracted
// behind a pure seam: no threads, no devices, no wall clock, no engine types. The render thread
// (AudioMixEngine) gathers the inputs each servo window, calls servoWindowTick, and applies the
// returned orders to the real rings; the scenario self-test gate drives the SAME functions and
// the SAME constants with synthetic producer traces, so the decision logic asserted headlessly
// is bit-for-bit the logic that runs in production -- the two can never drift apart.
//
// CONTRACT (these constants ARE the trim contract; they are consumed by the self-test asserts):
//   - the servo regulates each ring's windowed fill TROUGH to kTroughTargetMs;
//   - a trough holding above kTrimTroughMs for kTrimWindows net windows (saturating counter,
//     never strictly-consecutive) latches the trim;
//   - a latched trim sheds each tap's excess IN FULL when the instantaneous fill can cover it
//     above the any-phase bridge floor, otherwise it arms a deferred order the consumer
//     executes whole; the armed amount is capped to what the orbit's own measured peak can
//     cover above the deferred floor (an excess beyond physical reach is chunk-quantization
//     overstatement of the trough sample, not sheddable fill -- the latch holds, quietly);
//   - an unexecuted order expires with its window (one-window validity), and a resolved
//     deferred shed retires every sibling order from the same arming, including siblings armed
//     in a window where another tap shed immediately;
//   - both floors ride the burst ENVELOPE -- up instantly on the largest push of the window in
//     progress, decaying toward current pacing once it normalizes -- and the deferred floor
//     additionally guarantees the post-shed trough lands no lower than the target minus a
//     fixed slack, so a shed can neither dry the ring nor undercut the regulation floor;
//   - the EMA'd trough error drives bounded resampler compensation; sustained saturation falls
//     back to one resync (shed to the prime cushion), expected never.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace moxrelay {

// The libobs audio line this engine consumes: fixed at boot (obs_reset_audio).
inline constexpr int kLineRate = 48000;

// Consumer geometry: the mixer pulls the line in 10 ms chunks; a ring contributes only once it
// has buffered its prime cushion. The cushion is BURST-ADAPTIVE: a producer that delivers in
// large paced blocks (media decode can push ~85 ms per block) needs a cushion covering one full
// block period, or playback starves between pushes -- so prime = max(floor, burst envelope +
// margin), capped to the ring. The servo then regulates the fill TROUGH (the pre-read minimum) to a
// fixed target: for fine-grained producers the resulting MEAN fill sits in the 20-30 ms
// steady-state band; for block-paced producers the mean adds the inherent block/2 -- bounded,
// converged, and the lowest latency that producer's cadence physically allows.
inline constexpr size_t kChunkFrames = 480;          // 10 ms line chunk
inline constexpr double kTroughTargetMs = 15.0;      // regulated just-before-push minimum fill
inline constexpr size_t kPrimeFloorFrames = 1200;    // 25 ms minimum cushion
inline constexpr size_t kPrimeMarginFrames = 720;    // 15 ms on top of the largest observed push

// Fill servo: EMA-filtered fill error drives bounded resampler compensation.
inline constexpr double kServoPeriodSec = 0.25;
inline constexpr double kServoTauSec = 4.0;
inline constexpr double kServoMaxPpm = 500.0;
inline constexpr double kServoPpmPerMs = 40.0;  // proportional gain: 1 ms of fill error -> 40 ppm
inline constexpr double kWarmupSec = 2.0;       // no servo inside the post-start transient
inline constexpr double kSaturationSec = 5.0;   // sustained saturation -> the resync backstop

// Standing-fill trim backstop: a transport event can step the fill up WITHOUT drying the ring
// (a seek catch-up flood, a source-side buffering insertion), so the trim-at-reprime never runs
// and the bounded servo would take tens of seconds to drain the step. The windowed TROUGH is
// burst-immune, so a trough holding this far above target across windows is a standing step,
// not drift (correctable drift never accumulates multi-ms error against the servo) -- shed it
// with the fade-wrapped trim, counted as a transport gap. The window counter saturates up/down
// rather than requiring strictly consecutive hits, so trough sampling noise cannot unlatch it.
// The shed amount is the measured TROUGH EXCESS (lowering the whole fill sawtooth by that much
// lands the next window's trough at target regardless of where in a push cycle the tick falls)
// -- an instantaneous fill-vs-cushion shed would only act in the brief post-push phase and a
// small standing step could dodge it for seconds on a block-paced producer.
inline constexpr double kTrimTroughMs = kTroughTargetMs + 4.0;
inline constexpr int kTrimWindows = 3; // net high windows (~750 ms) before the trim fires

// Burst envelope: the cushion/floor input tracks producer pacing as an attack/release
// envelope, not an all-time high-water mark. ATTACK is instantaneous -- consumers consult
// max(envelope, largest push of the window in progress), so a coarse producer is cushioned
// correctly from its very first block; RELEASE decays a quarter per servo window toward what
// the producer currently delivers, so one oversized push (a capture hiccup, a decode splice,
// a capacity-class flood) stops dictating the cushion and the shed floors within a couple of
// seconds instead of forever. Windows with no pushes hold the envelope: silence is absence of
// evidence, not evidence of finer pacing.
inline constexpr size_t kBurstDecayNum = 3, kBurstDecayDen = 4; // release: -25% per servo window
inline constexpr size_t burstEnvelopeStep(size_t envFrames, size_t windowMaxFrames)
{
	if (!windowMaxFrames)
		return envFrames;
	return std::max(windowMaxFrames, envFrames * kBurstDecayNum / kBurstDecayDen);
}

// Deferred-shed landing slack: the deferred floor anchors the post-shed trough at the target
// minus this slack (instead of exactly at target). The slack is what makes a full shed
// SATISFIABLE with real headroom -- anchored exactly at target, satisfiability balances on an
// equality (the orbit's fattest post-read instant clears the floor by precisely the excess)
// and execution waits on delivery jitter, arming and expiring for windows at a time. Five
// milliseconds is far above per-push jitter, and the landing trough it permits stays above
// the steady-regulation floor the real-time gate asserts.
inline constexpr size_t kShedSlackFrames = size_t(kLineRate) * 5 / 1000;
// The smallest deferred shed worth arming: below half a consumer chunk the latch holds
// instead -- a residue that small is chunk-quantization noise on the trough sample, and a
// shed that small buys one fade splice for no audible latency relief.
inline constexpr size_t kShedQuantumFrames = kChunkFrames / 2;

// Producer-side timestamp discontinuity threshold (seek / loop wrap / restart detection).
inline constexpr uint64_t kTsJumpNs = 50ull * 1000 * 1000; // 50 ms
// Producer late-delivery stamp: a real-time producer's wall-clock gap between pushes
// equals the audio DURATION of the chunk it just pushed (independent of block size or
// burstiness), so a gap exceeding that duration by more than scheduling jitter is
// under-delivery (smoothed loop wrap, decode stall, settings restart) -- the producer
// self-reports lateness in its own frame of reference. A consumer that outruns a
// producer delivering on time dries the ring WITHOUT such a gap and still counts as a
// genuine underrun.
inline constexpr int64_t kProducerLateMarginMs = 20;
// One transport event (seek, smoothed loop wrap, playback restart) disturbs a media
// producer's DELIVERY RATE for several seconds even while its push cadence stays on time
// (source-side timestamp smoothing re-buffers and then under-delivers while pacing catches
// up; measured tails run past 6 s). A dry-out inside this window after any transport stamp
// is turbulence aftermath, not the consumer outrunning a steady producer; outside it the
// zero-underrun discipline applies at full strictness.
inline constexpr int64_t kTransportTurbulenceMs = 8000;

// The burst-adaptive prime cushion (also the trim + resync target), capped so it always fits
// the ring: an envelope briefly inflated by a capacity-class push must never demand a cushion
// the ring cannot physically hold -- a dried tap could then never re-prime (permanent silence,
// strictly worse than the latency the cushion exists to bound). The cap leaves one chunk of
// producer headroom; the envelope's release then walks the cushion back to its normal size.
inline constexpr size_t primeTargetFrames(size_t burstEnvFrames, size_t ringCapacityFrames)
{
	const size_t want = std::max(kPrimeFloorFrames, burstEnvFrames + kPrimeMarginFrames);
	const size_t cap = ringCapacityFrames > kChunkFrames ? ringCapacityFrames - kChunkFrames
							     : ringCapacityFrames;
	return std::min(want, cap);
}

// The any-phase shed bridge floor (the IMMEDIATE shed site, the servo tick): the tick lands at
// an arbitrary point of the push cycle, so the post-shed fill must survive one full push
// period of drain plus a scheduling-jitter margin before the next on-time push.
inline constexpr size_t shedBridgeFloorFrames(size_t burstEnvFrames)
{
	return burstEnvFrames + size_t(kLineRate) * 5 / 1000;
}

// The deferred-shed floor (the consumer site, post-read fill): a post-read instant has at most
// one push period MINUS the chunk just read of drain still ahead of the next on-time push, and
// the floor anchors the landing at the trough target minus the satisfiability slack -- the
// post-shed trough can neither dry nor undercut the regulation floor, and the orbit's fattest
// post-read instant clears the test with the full slack to spare (see kShedSlackFrames).
inline constexpr size_t deferredShedFloorFrames(size_t burstEnvFrames)
{
	const size_t drainBound = burstEnvFrames > kChunkFrames ? burstEnvFrames - kChunkFrames : 0;
	const size_t landingFrames =
		size_t(kTroughTargetMs * double(kLineRate) / 1000.0) - kShedSlackFrames;
	return drainBound + landingFrames;
}

// A latched trim's shed amount for one tap: the measured trough excess, in frames.
inline size_t trimShedExcessFrames(double lastWinTroughMs)
{
	return size_t((lastWinTroughMs - kTroughTargetMs) * double(kLineRate) / 1000.0);
}

// Deferred-shed execution test (consumer side, post-read fill): the armed amount is shed whole
// the moment the instantaneous fill can cover it above the deferred floor.
inline constexpr bool deferredShedReady(size_t pendingShedFrames, size_t fillFrames,
					size_t burstEnvFrames)
{
	const size_t floorF = deferredShedFloorFrames(burstEnvFrames);
	return fillFrames > floorF && fillFrames - floorF >= pendingShedFrames;
}

// Dry-out attribution at re-prime: a timestamp jump OR a long push silence around the gap
// (both stamp the jump clock) = transport behavior (expected); neither = the consumer
// genuinely outran a steady, still-pushing producer (a real underrun).
inline constexpr bool dryIsTransport(int64_t lastJumpMs, int64_t dryStartMs)
{
	return lastJumpMs >= dryStartMs - kTransportTurbulenceMs;
}

// ------------------------------------------------------------------------------------------
// The per-window decision: latch / patience / full-shed-or-defer / order validity / EMA /
// saturation. The caller owns ALL side effects (ring discards, fades, counters, log lines);
// this function owns the choices and the regulation state.
// ------------------------------------------------------------------------------------------

struct ServoTrimState {
	double ema = 0.0;
	double satSince = -1.0; // wall seconds saturation began; <0 = not saturated
	int highWindows = 0;    // saturating net count of windows with the trough above threshold
	bool shedArmed = false; // a deferred shed order is outstanding (one-window validity)
};

// One attached tap's inputs to a window decision (gathered atomically by the caller).
struct TrimTapView {
	bool eligible = false;        // attached and primed (contributing to the mix)
	double lastWinTroughMs = -1.0;
	size_t fillFrames = 0;        // instantaneous ring fill at the tick
	size_t winMaxFillFrames = 0;  // the previous window's post-read fill PEAK (the orbit's
				      // reach -- what a deferred shed can actually clear)
	size_t burstEnvFrames = 0;    // the burst envelope (post-release for this window)
};

// What the caller must do to one tap (parallel to the input views).
struct TrimTapOrder {
	bool clearPending = false;  // retire the tap's deferred order
	size_t shedNowFrames = 0;   // discard this many frames now (fade-wrapped by the caller)
	size_t armFrames = 0;       // arm a deferred order for this many frames
};

struct ServoWindowResult {
	// A consumer-executed deferred shed resolves the latch: count one transport gap, apply
	// the clear orders, reset the regulation state, command nothing this window.
	bool deferredResolved = false;
	// At least one tap shed immediately: count one transport gap, command nothing this window.
	bool immediateShed = false;
	// An armed order outlived its window and was retired (the clear orders carry it).
	bool ordersExpired = false;
	int armedCount = 0;       // taps newly carrying a deferred order this window
	bool servoCommanded = false; // ppm is valid and compensation should be applied
	double ppm = 0.0;
	bool resync = false;      // saturation backstop: shed every tap to its cushion, count it
	std::vector<TrimTapOrder> orders; // parallel to the input views
};

inline ServoWindowResult servoWindowTick(ServoTrimState &st, double now, double streamStart,
					 double troughMs, bool deferredExecuted,
					 const std::vector<TrimTapView> &taps)
{
	ServoWindowResult res;
	res.orders.resize(taps.size());
	if (now - streamStart < kWarmupSec)
		return res; // post-start transient: no anchor, no compensation

	// A consumer-executed deferred shed resolves the trim latch exactly like an
	// immediate one: count it, reset the regulation state, start fresh next window.
	// Sibling straggler orders from the same arming window expire with it (the
	// unexecuted-order rule below): with the latch resolved here and disarmed,
	// the expiry block never sees them, and a leftover order is already a
	// window-old reading -- executing it later would shed healthy fill.
	if (deferredExecuted) {
		res.deferredResolved = true;
		for (auto &o : res.orders)
			o.clearPending = true;
		st.ema = 0.0;
		st.highWindows = 0;
		st.shedArmed = false;
		return res; // fresh fill state; command on the next window
	}
	// An unexecuted shed order expires with its window: the excess it measured is a
	// window-old reading, and an excursion that drained on its own before the consumer
	// found a fat-enough instant would otherwise be shed AGAIN later from healthy fill
	// (a stale shed dives the trough and dries the ring under a steady producer). The
	// latch logic below re-arms with a fresh excess if the step actually persists.
	if (st.shedArmed) {
		res.ordersExpired = true;
		for (auto &o : res.orders)
			o.clearPending = true;
		st.shedArmed = false;
	}

	// Standing-fill trim backstop (see the constants above): a persistent trough step
	// is shed at the cushion, fade-wrapped, instead of grinding the bounded servo.
	st.highWindows = troughMs > kTrimTroughMs ? st.highWindows + 1 : std::max(0, st.highWindows - 1);
	if (st.highWindows >= kTrimWindows) {
		size_t shedIntent = 0;
		int armed = 0;
		for (size_t i = 0; i < taps.size(); ++i) {
			const TrimTapView &tap = taps[i];
			if (!tap.eligible)
				continue;
			if (tap.lastWinTroughMs <= kTroughTargetMs)
				continue;
			// Shed this tap's measured trough excess. The excess is a
			// TROUGH-basis quantity (pre-read window minimum); the fill
			// here is a post-read instant at an arbitrary point of the
			// push sawtooth, so it frequently cannot cover the whole
			// excess on a block-paced producer. A partial shed would
			// only dribble (and each dribble used to restart the trim's
			// patience -- the standing fill then outlived the stated
			// 3-window contract): shed in FULL now when the instant
			// allows it, otherwise arm the consumer-side deferred shed,
			// which executes whole within one push period. The ARMED
			// amount is capped to the orbit's own reach -- the previous
			// window's post-read peak above the deferred floor: capping
			// keeps every order satisfiable by construction (an excess
			// beyond reach is either chunk-quantization overstatement of
			// the trough sample or a floor still riding a decaying burst
			// envelope -- arming it would only arm-and-expire), and each
			// order still executes WHOLE; a persistent step re-arms with
			// a freshly measured excess until the trough is back at
			// target. Residues below the shed quantum hold the latch
			// quietly instead of buying fade splices for noise.
			const size_t excess = trimShedExcessFrames(tap.lastWinTroughMs);
			const size_t floorF = shedBridgeFloorFrames(tap.burstEnvFrames);
			if (excess && tap.fillFrames > floorF && tap.fillFrames - floorF >= excess) {
				res.orders[i].shedNowFrames = excess;
				res.orders[i].clearPending = true;
				shedIntent += excess;
			} else if (excess) {
				const size_t floorD = deferredShedFloorFrames(tap.burstEnvFrames);
				const size_t reach = tap.winMaxFillFrames > floorD
							     ? tap.winMaxFillFrames - floorD
							     : 0;
				const size_t armAmt = std::min(excess, reach);
				if (armAmt >= kShedQuantumFrames) {
					res.orders[i].armFrames = armAmt;
					armed++;
				}
			}
		}
		if (shedIntent) {
			res.immediateShed = true;
			st.ema = 0.0;
			if (armed) {
				// A sibling armed in the same window rides under the SAME
				// validity latch -- without it the order would survive its
				// window unexpired and execute stale. The latch also stays
				// high: the sibling's standing fill is not relieved by the
				// other tap's shed.
				st.shedArmed = true;
				res.armedCount = armed;
			} else {
				st.highWindows = 0;
			}
			return res; // fresh fill state; command on the next window
		}
		if (armed) {
			// The latch stays high until the deferred shed lands -- arming is
			// not relief, and re-arming next window simply refreshes the excess.
			st.shedArmed = true;
			res.armedCount = armed;
		}
	}

	const double err = troughMs - kTroughTargetMs;
	const double alpha = 1.0 - std::exp(-kServoPeriodSec / kServoTauSec);
	st.ema += alpha * (err - st.ema);
	double ppm = st.ema * kServoPpmPerMs;
	ppm = std::clamp(ppm, -kServoMaxPpm, kServoMaxPpm);
	res.servoCommanded = true;
	res.ppm = ppm;
	// Sustained saturation means a faulting clock, not drift: ONE fade-wrapped resync
	// drop back to the cushion, counted. Expected never.
	if (std::fabs(ppm) >= kServoMaxPpm - 0.5) {
		if (st.satSince < 0.0)
			st.satSince = now;
		if (now - st.satSince >= kSaturationSec) {
			res.resync = true;
			st.ema = 0.0;
			st.satSince = -1.0;
		}
	} else {
		st.satSince = -1.0;
	}
	return res;
}

} // namespace moxrelay
