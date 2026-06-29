// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioMixEngine implementation. See the header for the architecture contract. The WASAPI render
// path is ported from the engine's own monitor output (device acquisition incl. the "default"
// sentinel, mix-format negotiation, padding-gated writes, reactive reconnect, tap-first teardown
// order); the event-driven thread + MMCSS shape follows the validated render prototype. The
// per-source ring layer, the sum/scrub/clamp accumulator, the anti-click ramps/fades, the fill
// servo on a directly-owned SwrContext, and the meter taps are this engine's own.

#include "AudioMixEngine.hpp"

#include "AudioDeviceNotify.hpp"
#include "AudioRing.hpp"
#include "ServoTrimLogic.hpp"

#include <windows.h> // WIN32_LEAN_AND_MEAN + NOMINMAX are target-wide compile definitions

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace moxrelay {

namespace {

// The line format, consumer geometry, fill-servo, and standing-fill-trim constants live in
// ServoTrimLogic.hpp -- the decision core shared verbatim with the scenario self-test gate.
constexpr int kLineChannels = 2;

// Anti-click envelope lengths at the line rate.
constexpr size_t kRampFrames = 480; // ~10 ms volume/mute ramp at the producer
constexpr size_t kFadeFrames = 240; // ~5 ms discontinuity / dry-out / refill / recovery fade

constexpr double kLogPeriodSec = 5.0;

// Device I/O bounds (fail-open: nothing here waits unbounded).
constexpr DWORD kEventWaitMs = 2000;
// The shared-mode buffer REQUEST is capacity headroom only -- queued device audio is audible
// latency, so the write path never tops the buffer up: each wake fills the queue back to the
// fill target (floored at 3 device periods so event cadence can never starve it), keeping
// steady-state padding at ~the target while the rest of the capacity stays available as slack.
constexpr REFERENCE_TIME kDeviceBuffer100ns = 10000000; // 1 s capacity request
constexpr double kDeviceFillTargetMs = 30.0;            // steady-state device-queue write target
constexpr int64_t kRetryIntervalMs = 3000;              // reconnect cadence after a device fault

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally (the ksmedia.h define needs the ks.h include
// chain; the GUID value is fixed by the platform contract).
const GUID kIeeeFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

void atomicMaxFloat(std::atomic<float> &target, float value)
{
	float cur = target.load(std::memory_order_relaxed);
	while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
	}
}

double nowSec()
{
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

std::wstring utf8ToWide(const std::string &s)
{
	if (s.empty())
		return {};
	const int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	if (len <= 1)
		return {};
	std::wstring out(size_t(len - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), len);
	return out;
}

// Taper the tail of a tap's holdback reserve to silence -- the OLD-side fade of any
// consumer-initiated splice (dry-out, standing trim, resync, device recovery); the new side
// fades back in via consFadeIn. Render thread only.
void taperHoldTail(std::vector<float> &hold)
{
	const size_t holdFrames = hold.size() / 2;
	const size_t fade = std::min(holdFrames, kFadeFrames);
	for (size_t i = 0; i < fade; ++i) {
		const float f = float(fade - i) / float(fade + 1);
		hold[(holdFrames - fade + i) * 2] *= f;
		hold[(holdFrames - fade + i) * 2 + 1] *= f;
	}
}

} // namespace

// -------------------------------------------------------------------------------------------
// Tap: one attached source. The producer fields are touched only by that source's push thread
// (inside the capture callback); the consumer fields only by the render thread; the atomics
// bridge to stats().
// -------------------------------------------------------------------------------------------
struct AudioMixEngine::Tap {
	obs_source_t *source = nullptr; // engine-owned strong ref
	std::string sourceId;
	AudioRing ring;

	// ---- producer state (push thread only) ----
	float rampFrom = 1.0f;   // gain ramp start value
	float rampTo = 1.0f;     // gain ramp target (vol, or 0 while muted)
	size_t rampPos = kRampFrames; // >= kRampFrames means the ramp is settled at rampTo
	bool haveTs = false;
	uint64_t expectedTs = 0; // predicted next chunk timestamp (jump detection)
	size_t fadeInLeft = 0;   // producer fade-in frames pending (post-jump / post-drop)
	std::atomic<int64_t> lastJumpMs{-1000000}; // steady-clock ms of the last ts jump (attribution)
	std::atomic<int64_t> lastPushMs{0};        // steady-clock ms of the last push (attribution)
	size_t lastPushFrames = 0; // previous push's chunk size (late-delivery detection; push thread only)

	// ---- meter accumulators (render thread writes at the emission site; drained by stats()) ----
	std::atomic<float> meterPeak{0.0f};
	std::atomic<double> meterSumSqL{0.0};
	std::atomic<double> meterSumSqR{0.0};
	std::atomic<uint64_t> meterFrames{0};

	// ---- sync-offset delay reserve ----
	// A verb thread stores the target; the render thread reconciles applied-vs-target at the
	// top of each chunk visit as one fade-wrapped splice. delayFrames is the APPLIED reserve:
	// ring fill deliberately held resident beyond the working cushion, exempted from the
	// regulation basis (the trough sample subtracts it) and added on top of every
	// cushion/resync target.
	std::atomic<int> delayTargetMs{0};
	int delayAppliedMs = 0; // render thread only
	size_t delayFrames = 0; // render thread only (delayAppliedMs at the line rate)

	std::atomic<uint64_t> drops{0}; // producer chunks dropped ring-full (cumulative)
	// Burst envelope inputs (the adaptive cushion + shed-floor source): the producer records
	// the largest push of the window in progress; the render thread folds it into the
	// decaying envelope at each servo window (burstEnvelopeStep) and consults
	// max(envelope, in-progress) everywhere -- an oversized push re-cushions instantly while
	// its influence decays once the pacing normalizes (it no longer dictates floors forever).
	std::atomic<size_t> winBurstFrames{0}; // producer-side per-window push maximum
	size_t burstEnvFrames = 0;             // render thread only (committed per servo window)

	// Discontinuity joint: the absolute ring position where post-jump (or post-drop) audio
	// begins. The producer fades the NEW side in; the consumer tapers the OLD side out as it
	// reads up to this position -- the splice is a smooth dip, never a step.
	static constexpr uint64_t kNoJoint = ~0ull;
	std::atomic<uint64_t> jointPos{kNoJoint};

	// ---- consumer state (render thread only) ----
	bool primed = false;    // ring reached the prime fill once; contributes to the mix
	size_t consFadeIn = 0;  // consumer fade-in frames pending (refill / recovery / resync)
	bool dryPending = false;   // a dry-out awaits attribution at the next re-prime
	int64_t dryStartMs = 0;    // steady-clock ms of the pending dry-out
	double winMinMs = -1.0;    // per-servo-window fill trough (-1 = no sample yet)
	double lastWinTroughMs = -1.0; // previous window's trough (the trim's per-tap shed input)
	size_t winMaxFill = 0;         // per-window post-read fill peak (the orbit's reach)
	size_t lastWinMaxFill = 0;     // previous window's peak (the trim's arm cap input)
	// Trim shed deferred to the consumer's fat phase: the servo tick lands at an arbitrary
	// point of the fill sawtooth, so its instantaneous fill often cannot cover the trough-
	// measured excess on a block-paced producer (the trough is a pre-read minimum; the tick
	// fill is post-read). The consumer executes the full shed within one push period, where
	// the two frames of reference meet.
	size_t pendingShedFrames = 0;

	// The burst envelope as the consumer sees it right now: attack is the window in progress.
	size_t burstNowFrames() const
	{
		return std::max(burstEnvFrames, winBurstFrames.load(std::memory_order_relaxed));
	}

	// The burst-adaptive prime cushion (also the trim + resync target), ring-capped -- plus
	// the sync-offset reserve on top. The cushion math sees the capacity MINUS the reserve,
	// so cushion + reserve always preserves the cap's chunk of producer headroom: re-prime
	// stays reachable at every burst-envelope state, reserve or none.
	size_t primeTarget() const
	{
		return primeTargetFrames(burstNowFrames(), ring.capacityFrames() - delayFrames) +
		       delayFrames;
	}

	// ---- detach drain (a detach mid-stream is fade-wrapped, never a hard cut) ----
	std::atomic<bool> detached{false}; // callback removed; consumer drains the hold fade then drops it

	// Consumer-side holdback: the most recent kFadeFrames of read-but-not-yet-emitted audio,
	// prefilled with silence at (re)prime so emission stays slot-aligned. A producer stall can
	// land at ANY chunk alignment -- the final partial read alone can be arbitrarily short (even
	// zero frames), which would leave the dry-out fade truncated to a click. The holdback
	// guarantees the full ~5 ms of fade material for every dry-out and for the detach drain, at
	// the cost of one constant kFadeFrames of added latency.
	std::vector<float> hold;
};

// -------------------------------------------------------------------------------------------
// Producer: the audio capture tap. Runs on the PUSHING source's thread under libobs's callback
// mutex. The buffer is the post-resample/post-balance/post-filter line (48 kHz planar float
// stereo) and is PRE-volume/PRE-mute: `muted` arrives as a flag and the engine volume is read
// here -- both are applied at this producer (ramped, never stepped) so the rings always hold
// exactly what the source contributes to the mix.
// -------------------------------------------------------------------------------------------
void AudioMixEngine::audioCaptureThunk(void *param, obs_source_t *source, const struct audio_data *audio,
				       bool muted)
{
	auto *tap = static_cast<Tap *>(param);
	if (!audio || !audio->frames)
		return;

	const float *left = reinterpret_cast<const float *>(audio->data[0]);
	const float *right = audio->data[1] ? reinterpret_cast<const float *>(audio->data[1]) : left;
	if (!left)
		return;
	const size_t frames = audio->frames;

	// Volume/mute target for this chunk; an actual change starts a ~10 ms linear ramp from the
	// CURRENTLY APPLIED value (a mid-ramp change re-anchors, so there is never a step).
	const float target = muted ? 0.0f : obs_source_get_volume(source);
	if (target != tap->rampTo) {
		const float applied = (tap->rampPos >= kRampFrames)
					      ? tap->rampTo
					      : tap->rampFrom + (tap->rampTo - tap->rampFrom) *
									(float(tap->rampPos) / float(kRampFrames));
		tap->rampFrom = applied;
		tap->rampTo = target;
		tap->rampPos = 0;
	}

	// Timestamp discontinuity (seek, loop head, restart): fade the first post-jump chunk in,
	// and stamp the jump so a consumer dry-out around it reads as a TRANSPORT gap (expected),
	// not a starvation underrun.
	const uint64_t ts = audio->timestamp;
	if (tap->haveTs) {
		const uint64_t expect = tap->expectedTs;
		const uint64_t diff = ts > expect ? ts - expect : expect - ts;
		if (diff > kTsJumpNs) {
			tap->fadeInLeft = kFadeFrames;
			tap->jointPos.store(tap->ring.tailPos(), std::memory_order_release);
			using namespace std::chrono;
			tap->lastJumpMs.store(
				duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(),
				std::memory_order_relaxed);
		}
	}
	tap->haveTs = true;
	tap->expectedTs = ts + uint64_t(frames) * 1000000000ull / kLineRate;

	// A push arriving later than the previous chunk's own duration allows is late
	// delivery (see kProducerLateMarginMs) -- transport-shaped even when it resumes
	// timestamp-continuous, so stamp it into the same attribution bracket as a ts jump.
	// Audio handling is unchanged (a drained ring already fades out at the dry and back
	// in at the re-prime).
	{
		const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
					      std::chrono::steady_clock::now().time_since_epoch())
					      .count();
		const int64_t prevMs = tap->lastPushMs.load(std::memory_order_relaxed);
		const int64_t prevDurMs =
			int64_t(tap->lastPushFrames) * 1000 / int64_t(kLineRate);
		if (prevMs != 0 && nowMs - prevMs > prevDurMs + kProducerLateMarginMs)
			tap->lastJumpMs.store(nowMs, std::memory_order_relaxed);
		tap->lastPushMs.store(nowMs, std::memory_order_relaxed);
		tap->lastPushFrames = frames;
	}

	// Gain + envelopes applied per frame into a thread-local interleaved scratch buffer.
	// (Per-source meters accumulate at the consumer's emission site, not here: a sync-offset
	// reserve makes producer-site levels LEAD the heard audio by the whole delay.)
	thread_local std::vector<float> scratch;
	scratch.resize(frames * 2);
	for (size_t i = 0; i < frames; ++i) {
		float g = (tap->rampPos >= kRampFrames)
				  ? tap->rampTo
				  : tap->rampFrom + (tap->rampTo - tap->rampFrom) *
							    (float(tap->rampPos) / float(kRampFrames));
		if (tap->rampPos < kRampFrames)
			tap->rampPos++;
		if (tap->fadeInLeft) {
			g *= 1.0f - float(tap->fadeInLeft) / float(kFadeFrames);
			tap->fadeInLeft--;
		}
		scratch[i * 2] = left[i] * g;
		scratch[i * 2 + 1] = right[i] * g;
	}

	// Track the window's largest push: the burst envelope's attack input (cushion + floors).
	if (frames > tap->winBurstFrames.load(std::memory_order_relaxed))
		tap->winBurstFrames.store(frames, std::memory_order_relaxed);

	// Drop-on-full: never block the push thread; the gap is a discontinuity, so the next
	// accepted chunk fades in.
	if (!tap->ring.writeFrames(scratch.data(), frames)) {
		tap->drops.fetch_add(1, std::memory_order_relaxed);
		tap->fadeInLeft = kFadeFrames;
		tap->jointPos.store(tap->ring.tailPos(), std::memory_order_release); // the hole is a joint
	}
}

// -------------------------------------------------------------------------------------------
// Engine surface
// -------------------------------------------------------------------------------------------

AudioMixEngine::AudioMixEngine() = default;

AudioMixEngine::~AudioMixEngine()
{
	stop();
}

bool AudioMixEngine::setTestSink(TestSink sink)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_.load())
		return false;
	testSink_ = std::move(sink);
	return true;
}

bool AudioMixEngine::start()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_.load())
		return true;
	if (!wakeEvent_) {
		wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!wakeEvent_)
			return false;
	}
	// Default-follow trigger (real sink only): a system default-render change re-opens the
	// client while the stored selection is the sentinel. The callback runs on a COM worker
	// thread, so it only flags + wakes -- the render thread owns the actual swap.
	if (!testSink_ && !notify_) {
		notify_ = std::make_unique<AudioDeviceNotify>([this] {
			if (outputDevice() != "default")
				return;
			reopenRequested_.store(true, std::memory_order_release);
			// The handle is touched under mutex_ everywhere (stop() closes it under the
			// same lock), so a late notification races nothing.
			std::lock_guard<std::mutex> lock(mutex_);
			if (wakeEvent_)
				SetEvent(static_cast<HANDLE>(wakeEvent_));
		});
	}
	running_.store(true);
	thread_ = std::thread([this] { renderThread(); });
	return true;
}

void AudioMixEngine::stop()
{
	// Taps FIRST (the donor teardown order): remove every capture callback so no producer
	// touches a ring past this point -- but route live taps through the DRAIN path (detached
	// flag) instead of dropping them, so the render thread tapers their holdback reserves out
	// and process exit is fade-wrapped, never a hard cut.
	bool anyDraining = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &tap : taps_) {
			if (!tap->source)
				continue; // already detached (draining)
			obs_source_remove_audio_capture_callback(tap->source, &AudioMixEngine::audioCaptureThunk,
								  tap.get());
			obs_source_release(tap->source);
			tap->source = nullptr;
			tap->detached.store(true, std::memory_order_release);
			anyDraining = true;
		}
	}

	// Bounded drain window: the render thread erases a draining tap once its hold is emitted
	// (~one mix chunk), and a clientless/faulted device simply times out here (no audio was
	// flowing, so there is nothing audible to cut).
	if (anyDraining && running_.load()) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
		while (std::chrono::steady_clock::now() < deadline) {
			{
				std::lock_guard<std::mutex> lock(mutex_);
				if (taps_.empty())
					break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}
	{
		std::lock_guard<std::mutex> lock(mutex_);
		taps_.clear(); // anything left after the window (or with no render thread) drops here
	}

	if (running_.exchange(false)) {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (wakeEvent_)
				SetEvent(static_cast<HANDLE>(wakeEvent_));
		}
		if (thread_.joinable())
			thread_.join();
	}
	notify_.reset(); // unregister BEFORE the handle goes away
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (wakeEvent_) {
			CloseHandle(static_cast<HANDLE>(wakeEvent_));
			wakeEvent_ = nullptr;
		}
	}
}

void AudioMixEngine::setOutputDevice(const std::string &deviceId)
{
	{
		std::lock_guard<std::mutex> lock(devMutex_);
		if (deviceId_ == deviceId)
			return; // idempotent: no client churn on the current selection
		deviceId_ = deviceId;
	}
	reopenRequested_.store(true, std::memory_order_release);
	std::lock_guard<std::mutex> lock(mutex_);
	if (wakeEvent_)
		SetEvent(static_cast<HANDLE>(wakeEvent_));
}

std::string AudioMixEngine::outputDevice() const
{
	std::lock_guard<std::mutex> lock(devMutex_);
	return deviceId_;
}

bool AudioMixEngine::isRunning() const
{
	return running_.load();
}

void AudioMixEngine::attachSource(obs_source_t *source, const std::string &sourceId)
{
	if (!source)
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &tap : taps_) {
		if (tap->source == source && !tap->detached.load())
			return; // idempotent (a draining predecessor may coexist briefly)
	}
	obs_source_t *ref = obs_source_get_ref(source);
	if (!ref)
		return;
	auto tap = std::make_shared<Tap>();
	tap->source = ref;
	tap->sourceId = sourceId;
	// Attach IS a transport edge (playback starts on activation): the cold-open decode
	// catch-up disturbs delivery pacing exactly like a seek, so the turbulence window
	// opens at creation rather than leaving the stamp at its never-stamped sentinel.
	tap->lastJumpMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
				      std::chrono::steady_clock::now().time_since_epoch())
				      .count(),
			      std::memory_order_relaxed);
	// Seed the gain envelope at the source's CURRENT volume/mute state so config- or
	// creation-seeded values apply from the very first sample (a ramp from unity would be a
	// fade-in artifact, not an anti-click measure -- there is no previous output to ramp from).
	const float seeded = obs_source_muted(ref) ? 0.0f : obs_source_get_volume(ref);
	tap->rampFrom = seeded;
	tap->rampTo = seeded;
	// Seed the sync-offset reserve from the source's own stored offset, so a creation- or
	// boot-seeded delay applies from the very first contribution: the tap simply primes at
	// cushion + reserve from silence, no splice. (The render thread owns the applied fields,
	// but it cannot see this tap until the push_back below.)
	const int seededOffsetMs =
		std::clamp(int(obs_source_get_sync_offset(ref) / 1000000), 0, kSyncOffsetMaxMs);
	tap->delayTargetMs.store(seededOffsetMs, std::memory_order_relaxed);
	tap->delayAppliedMs = seededOffsetMs;
	tap->delayFrames = size_t(seededOffsetMs) * size_t(kLineRate) / 1000;
	obs_source_add_audio_capture_callback(ref, &AudioMixEngine::audioCaptureThunk, tap.get());
	taps_.push_back(std::move(tap));
}

void AudioMixEngine::noteSourceTransport(obs_source_t *source)
{
	if (!source)
		return;
	const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
				      std::chrono::steady_clock::now().time_since_epoch())
				      .count();
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &tap : taps_) {
		if (tap->source == source && !tap->detached.load()) {
			tap->lastJumpMs.store(nowMs, std::memory_order_relaxed);
			return;
		}
	}
}

void AudioMixEngine::setSourceSyncOffset(obs_source_t *source, int offsetMs)
{
	if (!source)
		return;
	const int ms = std::clamp(offsetMs, 0, kSyncOffsetMaxMs);
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &tap : taps_) {
		if (tap->source == source && !tap->detached.load()) {
			tap->delayTargetMs.store(ms, std::memory_order_relaxed);
			return;
		}
	}
}

void AudioMixEngine::detachSource(obs_source_t *source)
{
	if (!source)
		return;
	std::shared_ptr<Tap> victim;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &tap : taps_) {
			if (tap->source == source && !tap->detached.load()) {
				victim = tap;
				break;
			}
		}
	}
	if (!victim)
		return;
	// Removal serializes against an in-flight callback (libobs holds the callback mutex), so
	// the producer is gone when this returns and the source may be released by the caller.
	// The tap itself stays listed in a DRAINING state: the render thread tapers its holdback
	// reserve (~5 ms of already-read audio) out and then drops it -- a mid-stream detach is
	// never a hard cut on the mixed output.
	obs_source_remove_audio_capture_callback(victim->source, &AudioMixEngine::audioCaptureThunk,
						 victim.get());
	obs_source_release(victim->source);
	victim->source = nullptr;
	victim->detached.store(true, std::memory_order_release);
}

AudioEngineStats AudioMixEngine::stats()
{
	AudioEngineStats out;
	out.deviceUp = deviceUp_.load();
	out.fillMs = meanFillMs_.load();
	// Counter BEFORE its paired trough: the publisher stores the trough first and bumps the
	// counter second (release), so counter-then-value can never pair a fresh counter with the
	// previous window's trough; a caller's double-snapshot equality check then pins the pair.
	out.servoWindows = servoWindows_.load(std::memory_order_acquire);
	out.fillMinMs = fillMinMs_.load();
	out.deviceFillAvgMs = devFillAvgMs_.load();
	out.deviceFillMaxMs = devFillMaxMs_.load();
	out.deviceStarves = deviceStarves_.load();
	out.servoPpm = servoPpm_.load();
	out.underruns = underruns_.load();
	out.transportGaps = transportGaps_.load();
	out.resyncs = resyncs_.load();
	out.deviceRecoveries = deviceRecoveries_.load();
	out.masterPeak = masterPeak_.exchange(0.0f);
	const double mSumSq = masterSumSq_.exchange(0.0);
	const uint64_t mFrames = masterSamples_.exchange(0);
	out.masterRms = mFrames ? float(std::sqrt(mSumSq / double(mFrames))) : 0.0f;
	out.clipped = clipped_.exchange(false);

	std::lock_guard<std::mutex> lock(mutex_);
	uint64_t dropSum = 0;
	for (const auto &tap : taps_) {
		if (tap->detached.load())
			continue; // draining; no longer an attached source
		AudioSourceStats s;
		s.sourceId = tap->sourceId;
		s.fillMs = double(tap->ring.fillFrames()) * 1000.0 / kLineRate;
		s.drops = tap->drops.load();
		s.peak = tap->meterPeak.exchange(0.0f);
		const double sl = tap->meterSumSqL.exchange(0.0);
		const double sr = tap->meterSumSqR.exchange(0.0);
		const uint64_t n = tap->meterFrames.exchange(0);
		s.rms = n ? float(std::sqrt(std::max(sl, sr) / double(n))) : 0.0f;
		dropSum += s.drops;
		out.sources.push_back(std::move(s));
	}
	out.producerDrops = dropSum;
	return out;
}

// -------------------------------------------------------------------------------------------
// Consumer: the render thread. One per engine; owns the WASAPI client (or hands the mix to the
// test sink), the line->device resampler, the fill servo, and the master meter.
// -------------------------------------------------------------------------------------------
void AudioMixEngine::renderThread()
{
	const bool test = static_cast<bool>(testSink_);
	const HANDLE wake = static_cast<HANDLE>(wakeEvent_);

	HRESULT hrCo = S_OK;
	bool comInit = false;
	if (!test) {
		hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		comInit = SUCCEEDED(hrCo) || hrCo == S_FALSE;
	}

	// Device state (real sink only).
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	UINT32 bufferFrames = 0;
	UINT32 fillTargetFrames = 0; // per-wake write ceiling (kDeviceFillTargetMs at the device rate)
	int devRate = kLineRate;
	int devChannels = kLineChannels;
	HANDLE mmcss = nullptr;
	DWORD mmcssTask = 0;
	int64_t nextRetryMs = 0;
	bool hadClient = false; // a fault happened and recovery is pending

	SwrContext *swr = nullptr;

	auto msNow = [] {
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	};

	auto destroySwr = [&] {
		if (swr) {
			swr_free(&swr);
			swr = nullptr;
		}
	};

	// Build the line -> output resampler. The compensation no-op right after init activates the
	// soft-compensation path while nothing is buffered, so the first real servo command never
	// re-initializes the resampler mid-stream.
	auto buildSwr = [&](int outRate, int outChannels) -> bool {
		destroySwr();
		AVChannelLayout inLayout;
		AVChannelLayout outLayout;
		av_channel_layout_default(&inLayout, kLineChannels);
		av_channel_layout_default(&outLayout, outChannels);
		if (swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_FLT, outRate, &inLayout,
					AV_SAMPLE_FMT_FLTP, kLineRate, 0, nullptr) < 0)
			return false;
		if (swr_init(swr) < 0) {
			destroySwr();
			return false;
		}
		swr_set_compensation(swr, 0, outRate); // pre-stream: activate the compensation path
		return true;
	};

	auto freeForReconnect = [&] {
		if (client)
			client->Stop();
		if (render) {
			render->Release();
			render = nullptr;
		}
		if (client) {
			client->Release();
			client = nullptr;
		}
		destroySwr();
		deviceUp_.store(false);
		nextRetryMs = msNow() + kRetryIntervalMs;
	};

	// Open the SELECTED render endpoint (shared mode, event driven): the "default" sentinel
	// resolves to the system default at every (re)open -- default-follow re-opens are triggered
	// by the notification client. Bounded: a failure leaves the engine clientless and the retry
	// cadence owns the next attempt (a yanked explicit endpoint keeps retrying that id until
	// the device returns or the selection changes).
	auto openDevice = [&]() -> bool {
		IMMDeviceEnumerator *enumr = nullptr;
		IMMDevice *device = nullptr;
		WAVEFORMATEX *wfx = nullptr;
		bool ok = false;
		const std::string wantedId = outputDevice();
		do {
			if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
						    __uuidof(IMMDeviceEnumerator), (void **)&enumr)) ||
			    !enumr)
				break;
			if (wantedId == "default") {
				if (FAILED(enumr->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
				    !device)
					break;
			} else {
				const std::wstring wide = utf8ToWide(wantedId);
				if (wide.empty() || FAILED(enumr->GetDevice(wide.c_str(), &device)) || !device)
					break;
			}
			if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
						    (void **)&client)) ||
			    !client)
				break;
			if (FAILED(client->GetMixFormat(&wfx)) || !wfx)
				break;
			// Shared-mode engine mix format is float; anything else is treated as an
			// open failure (retry; never an unchecked memcpy of mismatched bytes).
			bool isFloat = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
			if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
				auto *ext = reinterpret_cast<WAVEFORMATEXTENSIBLE *>(wfx);
				isFloat = IsEqualGUID(ext->SubFormat, kIeeeFloatSubtype) != 0;
			}
			if (!isFloat || wfx->wBitsPerSample != 32)
				break;
			if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,
						      AUDCLNT_STREAMFLAGS_EVENTCALLBACK, kDeviceBuffer100ns,
						      0, wfx, nullptr)))
				break;
			if (FAILED(client->SetEventHandle(wake)))
				break;
			if (FAILED(client->GetBufferSize(&bufferFrames)) || !bufferFrames)
				break;
			if (FAILED(client->GetService(__uuidof(IAudioRenderClient), (void **)&render)) ||
			    !render)
				break;
			devRate = int(wfx->nSamplesPerSec);
			devChannels = int(wfx->nChannels);
			// Write target: the fill the wake loop maintains in the device queue. Floored
			// at 3 default periods (event cadence is one period; 3 rides out scheduling
			// jitter -- the bound the drift probe validated with zero underruns), capped at
			// the actual capacity.
			REFERENCE_TIME defPeriod = 0;
			if (FAILED(client->GetDevicePeriod(&defPeriod, nullptr)) || defPeriod <= 0)
				defPeriod = 100000; // engine default 10 ms
			const auto periodFrames = UINT32(uint64_t(devRate) * uint64_t(defPeriod) / 10000000ull);
			fillTargetFrames = UINT32(double(devRate) * kDeviceFillTargetMs / 1000.0);
			fillTargetFrames = std::max(fillTargetFrames, 3 * std::max(periodFrames, 1u));
			fillTargetFrames = std::min(fillTargetFrames, bufferFrames);
			if (!buildSwr(devRate, devChannels))
				break;
			// Pre-roll the write target of silence (NOT the full buffer -- queued audio is
			// audible latency), then start.
			BYTE *data = nullptr;
			if (FAILED(render->GetBuffer(fillTargetFrames, &data)))
				break;
			if (FAILED(render->ReleaseBuffer(fillTargetFrames, AUDCLNT_BUFFERFLAGS_SILENT)))
				break;
			if (FAILED(client->Start()))
				break;
			ok = true;
		} while (false);
		if (wfx)
			CoTaskMemFree(wfx);
		if (device)
			device->Release();
		if (enumr)
			enumr->Release();
		if (!ok)
			freeForReconnect();
		return ok;
	};

	if (!test) {
		mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask); // best-effort
	} else {
		devRate = kLineRate;
		devChannels = kLineChannels;
		if (!buildSwr(devRate, devChannels)) {
			std::fprintf(stderr, "[audio] resampler init failed -- audio disabled\n");
			running_.store(false);
		}
		deviceUp_.store(true);
	}

	// Mixing state.
	std::vector<std::shared_ptr<Tap>> snapshot;
	std::vector<float> accL(kChunkFrames), accR(kChunkFrames);
	std::vector<float> pull(kChunkFrames * 2);
	std::vector<float> stage; // swr output staging (device-format interleaved)
	std::vector<float> fifo;  // mixed device-format samples awaiting delivery
	size_t fifoHead = 0;      // floats consumed from fifo

	auto fifoFrames = [&] { return (fifo.size() - fifoHead) / size_t(devChannels); };

	// Servo + telemetry state.
	double streamStart = nowSec();
	double fillSum = 0.0;
	uint64_t fillCount = 0;
	double devFillSum = 0.0; // device-queue padding telemetry (post-warmup, per wake)
	uint64_t devFillCnt = 0;
	double devFillMax = 0.0;
	UINT32 resumeFade = 0; // pending fade-in frames after a device-queue starvation
	ServoTrimState trimState;    // latch / patience / EMA / saturation (the shared decision core)
	uint64_t shedDone = 0;       // consumer-executed trim sheds awaiting the tick's bookkeeping
	double shedDoneTroughMs = 0; // trough basis of the last consumer-executed shed (log only)
	double nextServo = streamStart + kServoPeriodSec;
	double nextLog = streamStart + kLogPeriodSec;

	// Mix exactly one 10 ms line chunk from every primed ring into the accumulator, resample,
	// and append to the delivery fifo. Returns true when at least one tap contributed.
	auto mixChunk = [&]() -> bool {
		{
			std::lock_guard<std::mutex> lock(mutex_);
			snapshot = taps_;
		}
		std::fill(accL.begin(), accL.end(), 0.0f);
		std::fill(accR.begin(), accR.end(), 0.0f);
		bool any = false;
		bool anyDead = false;

		for (const auto &tap : snapshot) {
			if (tap->detached.load(std::memory_order_acquire)) {
				// Drain fade: taper the holdback reserve (~5 ms of already-read
				// audio, ending exactly where emission stopped) to silence, then
				// drop the tap from the list.
				auto &hold = tap->hold;
				const size_t n = std::min(hold.size() / 2, kChunkFrames);
				for (size_t i = 0; i < n; ++i) {
					const float f = float(n - i) / float(n + 1);
					accL[i] += hold[i * 2] * f;
					accR[i] += hold[i * 2 + 1] * f;
				}
				hold.clear();
				if (n)
					any = true;
				anyDead = true;
				continue;
			}
			// Tapered dry-out leftovers (a stall can leave up to kFadeFrames of
			// already-faded audio unemitted) drain in plain emission order.
			if (!tap->primed && !tap->hold.empty()) {
				const size_t n = std::min(tap->hold.size() / 2, kChunkFrames);
				for (size_t i = 0; i < n; ++i) {
					accL[i] += tap->hold[i * 2];
					accR[i] += tap->hold[i * 2 + 1];
				}
				tap->hold.erase(tap->hold.begin(), tap->hold.begin() + n * 2);
				if (n)
					any = true;
			}
			// Sync-offset reconcile: apply a changed delay target as ONE fade-wrapped
			// splice. An increase re-primes through the priming path -- the producer
			// keeps every sample while the consumer holds back until the new reserve
			// is resident, so the audible gap IS the added delay (stamped deliberate
			// transport, never a starvation underrun). A decrease discards exactly
			// the removed reserve and plays on. Either way any armed shed is dropped:
			// its excess was measured against the old basis.
			const int delayTarget = tap->delayTargetMs.load(std::memory_order_relaxed);
			if (delayTarget != tap->delayAppliedMs) {
				const size_t targetFrames =
					size_t(delayTarget) * size_t(kLineRate) / 1000;
				if (delayTarget > tap->delayAppliedMs) {
					tap->lastJumpMs.store(msNow(), std::memory_order_relaxed);
					if (tap->primed) {
						// The dry path's exact emission shape: taper the
						// holdback and emit it THIS chunk (a deferred drain
						// would leave one whole chunk of hard-cut silence
						// before the fade material plays -- an audible step).
						taperHoldTail(tap->hold);
						const size_t n =
							std::min(tap->hold.size() / 2, kChunkFrames);
						for (size_t i = 0; i < n; ++i) {
							accL[i] += tap->hold[i * 2];
							accR[i] += tap->hold[i * 2 + 1];
						}
						tap->hold.erase(tap->hold.begin(),
								tap->hold.begin() + n * 2);
						if (n)
							any = true;
						tap->primed = false;
						tap->dryPending = true;
						tap->dryStartMs = msNow();
					}
				} else if (tap->primed) {
					tap->ring.discardFrames(tap->delayFrames - targetFrames);
					taperHoldTail(tap->hold);
					tap->consFadeIn = kFadeFrames;
				}
				tap->pendingShedFrames = 0;
				tap->delayAppliedMs = delayTarget;
				tap->delayFrames = targetFrames;
			}
			size_t fill = tap->ring.fillFrames();
			if (!tap->primed) {
				const size_t prime = tap->primeTarget();
				if (fill < prime)
					continue; // still buffering its latency cushion
				// (Re)prime: shed any overshoot beyond the cushion. The tap is
				// contributing silence here, so the trim is inaudible by
				// construction -- without it a decode catch-up burst (seek, loop
				// wrap, cold open) becomes STANDING fill the bounded drift servo
				// would take ages to drain, blowing the latency budget.
				if (fill > prime) {
					tap->ring.discardFrames(fill - prime);
					fill = prime;
				}
				// Attribute the pending dry-out now that the producer told us what
				// happened: a timestamp jump OR a long push silence around the gap
				// (both stamp lastJumpMs) = transport behavior (seek / loop /
				// pause-resume / playback restart, expected); neither = the
				// consumer genuinely outran a steady, still-pushing producer
				// (a real underrun).
				if (tap->dryPending) {
					tap->dryPending = false;
					if (dryIsTransport(tap->lastJumpMs.load(std::memory_order_relaxed),
							   tap->dryStartMs))
						transportGaps_.fetch_add(1, std::memory_order_relaxed);
					else
						underruns_.fetch_add(1, std::memory_order_relaxed);
				}
				tap->primed = true;
				tap->consFadeIn = kFadeFrames;
				tap->pendingShedFrames = 0; // the prime trim above already squared the fill
				// Silence-prefill the holdback so emission stays slot-aligned from
				// the first chunk (the reserve becomes real audio as reads flow).
				tap->hold.assign(kFadeFrames * 2, 0.0f);
			}
			const double fillMs = double(fill) * 1000.0 / kLineRate;
			fillSum += fillMs;
			fillCount++;
			// The regulation basis is delay-exempt: the window trough subtracts the
			// applied reserve, so a delayed tap at steady state reads trough ~= target
			// (the trim never eats a deliberate reserve) while the published mean and
			// per-source fill stay the truthful raw depth.
			const double regMs = fillMs - double(tap->delayAppliedMs);
			if (tap->winMinMs < 0.0 || regMs < tap->winMinMs)
				tap->winMinMs = regMs; // the servo regulates this window trough

			const uint64_t headBefore = tap->ring.headPos();
			const size_t got = tap->ring.readFrames(pull.data(), kChunkFrames);

			// Joint fade-out: taper the up-to-5 ms of OLD audio leading into a
			// producer-marked discontinuity (the producer fades the new side in).
			uint64_t joint = tap->jointPos.load(std::memory_order_acquire);
			if (joint != Tap::kNoJoint) {
				if (headBefore >= joint) {
					// Already consumed (or trimmed past); CAS so a newer joint
					// the producer just published is never lost.
					tap->jointPos.compare_exchange_strong(joint, Tap::kNoJoint,
									      std::memory_order_relaxed);
				} else if (got) {
					const uint64_t fadeStart =
						joint > kFadeFrames ? joint - kFadeFrames : 0;
					for (size_t i = 0; i < got; ++i) {
						const uint64_t abs = headBefore + i;
						if (abs >= fadeStart && abs < joint) {
							const float f = float(joint - abs) /
									float(kFadeFrames + 1);
							pull[i * 2] *= f;
							pull[i * 2 + 1] *= f;
						}
					}
					if (headBefore + got >= joint)
						tap->jointPos.compare_exchange_strong(
							joint, Tap::kNoJoint, std::memory_order_relaxed);
				}
			}

			const bool dry = got < kChunkFrames;
			if (tap->consFadeIn && got) {
				const size_t n = std::min(got, tap->consFadeIn);
				for (size_t i = 0; i < n; ++i) {
					const float f = 1.0f - float(tap->consFadeIn - i) / float(kFadeFrames);
					pull[i * 2] *= f;
					pull[i * 2 + 1] *= f;
				}
				tap->consFadeIn -= n;
			}

			// Emission runs through the holdback: append what was read, keep the
			// newest kFadeFrames as the fade reserve, emit the rest slot-aligned.
			auto &hold = tap->hold;
			hold.insert(hold.end(), pull.begin(), pull.begin() + got * 2);
			if (dry) {
				// Producer stalled (transport or starvation): taper the final
				// ~5 ms of everything still unemitted -- the reserve guarantees
				// full fade material no matter where the stall landed -- and drop
				// back to priming; the underrun-vs-transport call is deferred to
				// the re-prime, when the producer's next timestamps are known.
				tap->primed = false;
				tap->dryPending = true;
				tap->dryStartMs = msNow();
				taperHoldTail(hold);
			}
			const size_t holdFrames = hold.size() / 2;
			const size_t reserve = dry ? 0 : kFadeFrames;
			const size_t emitN =
				std::min(holdFrames > reserve ? holdFrames - reserve : 0, kChunkFrames);
			// Per-source meter accumulation at the emission site (post-gain/mute, post
			// delay reserve): exactly the heard contribution entering the mix this
			// chunk -- a producer-site meter would lead a delayed source's audible
			// audio by the whole reserve.
			float emitPeak = 0.0f;
			double emitSumSqL = 0.0, emitSumSqR = 0.0;
			for (size_t i = 0; i < emitN; ++i) {
				const float l = hold[i * 2];
				const float r = hold[i * 2 + 1];
				accL[i] += l;
				accR[i] += r;
				const float al = std::fabs(l), ar = std::fabs(r);
				if (al > emitPeak)
					emitPeak = al;
				if (ar > emitPeak)
					emitPeak = ar;
				emitSumSqL += double(l) * double(l);
				emitSumSqR += double(r) * double(r);
			}
			hold.erase(hold.begin(), hold.begin() + emitN * 2);
			if (emitN) {
				any = true;
				atomicMaxFloat(tap->meterPeak, emitPeak);
				tap->meterSumSqL.fetch_add(emitSumSqL, std::memory_order_relaxed);
				tap->meterSumSqR.fetch_add(emitSumSqR, std::memory_order_relaxed);
				tap->meterFrames.fetch_add(uint64_t(emitN), std::memory_order_relaxed);
			}

			// Post-read fill peak: the orbit's reach, the trim's arm-cap input.
			const size_t postFill = tap->ring.fillFrames();
			if (!dry && postFill > tap->winMaxFill)
				tap->winMaxFill = postFill;

			// Deferred trim shed: execute the armed amount the moment the post-push
			// fill can cover it whole (within one push period of arming). One discard,
			// one fade-wrapped splice -- never a dribble of partial sheds. The floor
			// is the deferred-site bound: at most one push period minus the chunk just
			// read of drain still ahead, anchored so the landing trough stays at
			// target-minus-slack (deferredShedFloorFrames) -- satisfiable with real
			// headroom where the old any-phase bound balanced on an equality.
			if (tap->pendingShedFrames && tap->primed && !dry &&
			    deferredShedReady(tap->pendingShedFrames, postFill,
					      tap->burstNowFrames())) {
				tap->ring.discardFrames(tap->pendingShedFrames);
				taperHoldTail(hold);
				tap->consFadeIn = kFadeFrames;
				shedDoneTroughMs = tap->lastWinTroughMs;
				tap->pendingShedFrames = 0;
				shedDone++;
			}
		}

		if (anyDead) {
			std::lock_guard<std::mutex> lock(mutex_);
			taps_.erase(std::remove_if(taps_.begin(), taps_.end(),
						   [](const std::shared_ptr<Tap> &t) {
							   return t->detached.load() && t->hold.empty();
						   }),
				    taps_.end());
		}

		// Master meter: post-sum, PRE-clamp; latch whether the clamp engages.
		float peak = 0.0f;
		double sumSq = 0.0;
		bool clip = false;
		for (size_t i = 0; i < kChunkFrames; ++i) {
			// NaN-scrub then hard clamp [-1, 1] -- the engine mixdown policy, no limiter.
			float l = accL[i], r = accR[i];
			l = (l == l) ? l : 0.0f;
			r = (r == r) ? r : 0.0f;
			const float al = std::fabs(l), ar = std::fabs(r);
			if (al > peak)
				peak = al;
			if (ar > peak)
				peak = ar;
			sumSq += (double(l) * double(l) + double(r) * double(r)) * 0.5;
			if (l > 1.0f) {
				l = 1.0f;
				clip = true;
			} else if (l < -1.0f) {
				l = -1.0f;
				clip = true;
			}
			if (r > 1.0f) {
				r = 1.0f;
				clip = true;
			} else if (r < -1.0f) {
				r = -1.0f;
				clip = true;
			}
			accL[i] = l;
			accR[i] = r;
		}
		atomicMaxFloat(masterPeak_, peak);
		masterSumSq_.fetch_add(sumSq, std::memory_order_relaxed);
		masterSamples_.fetch_add(kChunkFrames, std::memory_order_relaxed);
		if (clip)
			clipped_.store(true, std::memory_order_relaxed);

		// Line -> device format. Output count is ratio-dependent (the servo trims it).
		const size_t outCap = size_t(double(kChunkFrames) * devRate / kLineRate) + 256;
		stage.resize(outCap * size_t(devChannels));
		const uint8_t *inPlanes[2] = {reinterpret_cast<const uint8_t *>(accL.data()),
					      reinterpret_cast<const uint8_t *>(accR.data())};
		uint8_t *outPlane = reinterpret_cast<uint8_t *>(stage.data());
		const int outFrames = swr ? swr_convert(swr, &outPlane, int(outCap), inPlanes,
							int(kChunkFrames))
					  : 0;
		if (outFrames > 0)
			fifo.insert(fifo.end(), stage.begin(),
				    stage.begin() + size_t(outFrames) * size_t(devChannels));
		return any;
	};

	auto compactFifo = [&] {
		if (fifoHead) {
			fifo.erase(fifo.begin(), fifo.begin() + fifoHead);
			fifoHead = 0;
		}
	};

	// Servo tick: EMA the windowed fill-TROUGH error (the just-before-push minimum -- robust
	// against block-paced producers, whose mean fill rides burst/2 above it by physics),
	// command bounded compensation, run the saturation backstop. The DECISIONS (latch,
	// patience, full-shed-or-defer, order validity, EMA, saturation) are the shared core in
	// ServoTrimLogic.hpp; this tick gathers the inputs atomically, applies the returned
	// orders to the rings (fade-wrapped), and owns every counter and log line.
	std::vector<std::shared_ptr<Tap>> trimSnap;
	std::vector<TrimTapView> trimViews;
	auto servoTick = [&](double now) {
		if (now < nextServo)
			return;
		nextServo = now + kServoPeriodSec;
		double troughSum = 0.0;
		int troughCount = 0;
		trimSnap.clear();
		trimViews.clear();
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (const auto &tap : taps_) {
				if (tap->winMinMs >= 0.0) {
					troughSum += tap->winMinMs;
					troughCount++;
					tap->lastWinTroughMs = tap->winMinMs;
					tap->winMinMs = -1.0;
				}
				tap->lastWinMaxFill = tap->winMaxFill;
				tap->winMaxFill = 0;
			}
			// Decision inputs, gathered under the same lock the troughs settle in:
			// the snapshot pins the tap objects so the orders apply to exactly the
			// taps the views described. The burst envelope commits its release step
			// here (one step per servo window); a push racing the exchange lands in
			// the next window's attack, and burstNowFrames() covers it meanwhile.
			trimSnap = taps_;
			trimViews.reserve(trimSnap.size());
			for (const auto &tap : trimSnap) {
				tap->burstEnvFrames = burstEnvelopeStep(
					tap->burstEnvFrames,
					tap->winBurstFrames.exchange(0, std::memory_order_relaxed));
				TrimTapView v;
				v.eligible = !tap->detached.load() && tap->primed;
				v.lastWinTroughMs = tap->lastWinTroughMs;
				v.fillFrames = tap->ring.fillFrames();
				v.winMaxFillFrames = tap->lastWinMaxFill;
				v.burstEnvFrames = tap->burstEnvFrames;
				trimViews.push_back(v);
			}
		}
		if (fillCount) {
			meanFillMs_.store(fillSum / double(fillCount));
			fillSum = 0.0;
			fillCount = 0;
		}
		if (!troughCount)
			return;
		const double trough = troughSum / double(troughCount);
		fillMinMs_.store(trough);
		servoWindows_.fetch_add(1, std::memory_order_release);

		const ServoWindowResult res =
			servoWindowTick(trimState, now, streamStart, trough, shedDone != 0, trimViews);

		if (res.deferredResolved) {
			std::printf("[audio] trim shed trough=%.1fms (deferred x%llu)\n",
				    shedDoneTroughMs, (unsigned long long)shedDone);
			transportGaps_.fetch_add(1, std::memory_order_relaxed);
			{
				std::lock_guard<std::mutex> lock(mutex_);
				for (size_t i = 0; i < trimSnap.size(); ++i)
					if (res.orders[i].clearPending)
						trimSnap[i]->pendingShedFrames = 0;
			}
			shedDone = 0;
			return; // fresh fill state; command on the next window
		}

		// Apply the per-tap orders (expiry clears, immediate sheds, deferred arms).
		size_t discarded = 0;
		bool anyOrder = false;
		for (const auto &o : res.orders) {
			if (o.clearPending || o.shedNowFrames || o.armFrames) {
				anyOrder = true;
				break;
			}
		}
		if (anyOrder) {
			std::lock_guard<std::mutex> lock(mutex_);
			for (size_t i = 0; i < trimSnap.size(); ++i) {
				const TrimTapOrder &o = res.orders[i];
				const auto &tap = trimSnap[i];
				if (o.clearPending)
					tap->pendingShedFrames = 0;
				if (o.shedNowFrames) {
					discarded += tap->ring.discardFrames(o.shedNowFrames);
					taperHoldTail(tap->hold);
					tap->consFadeIn = kFadeFrames;
					tap->pendingShedFrames = 0;
				}
				if (o.armFrames)
					tap->pendingShedFrames = o.armFrames;
			}
		}
		if (res.immediateShed) {
			// The shed content is transport backlog, not starvation. The log line
			// makes every shed visible (rare event; trough in ms, shed in frames).
			std::printf("[audio] trim shed trough=%.1fms frames=%zu\n", trough, discarded);
			transportGaps_.fetch_add(1, std::memory_order_relaxed);
			return; // fresh fill state; command on the next window
		}
		if (res.armedCount) {
			// The latch stays high until the deferred shed lands -- arming is
			// not relief, and re-arming next window simply refreshes the excess.
			std::printf("[audio] trim armed trough=%.1fms taps=%zu\n", trough,
				    size_t(res.armedCount));
		}
		if (!res.servoCommanded)
			return; // post-start transient: no anchor, no compensation
		servoPpm_.store(res.ppm);
		if (swr) {
			const int distance = devRate * 2;
			const int delta = -int(std::llround(double(distance) * res.ppm / 1e6));
			swr_set_compensation(swr, delta, distance);
		}
		if (res.resync) {
			std::lock_guard<std::mutex> lock(mutex_);
			for (const auto &tap : taps_) {
				const size_t fill = tap->ring.fillFrames();
				const size_t prime = tap->primeTarget();
				if (fill > prime) {
					tap->ring.discardFrames(fill - prime);
					taperHoldTail(tap->hold);
					tap->consFadeIn = kFadeFrames;
				}
			}
			resyncs_.fetch_add(1, std::memory_order_relaxed);
		}
	};

	auto logTick = [&](double now) {
		if (now < nextLog)
			return;
		nextLog = now + kLogPeriodSec;
		size_t tapCount;
		uint64_t dropSum = 0;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			tapCount = taps_.size();
			for (const auto &tap : taps_)
				dropSum += tap->drops.load();
		}
		if (!tapCount)
			return;
		std::printf("[audio] taps=%zu fill=%.1fms trough=%.1fms devfill=%.1f/%.1fms ppm=%+.0f "
			    "drops=%llu underruns=%llu gaps=%llu resyncs=%llu starves=%llu device=%s\n",
			    tapCount, meanFillMs_.load(), fillMinMs_.load(), devFillAvgMs_.load(),
			    devFillMaxMs_.load(), servoPpm_.load(), (unsigned long long)dropSum,
			    (unsigned long long)underruns_.load(), (unsigned long long)transportGaps_.load(),
			    (unsigned long long)resyncs_.load(), (unsigned long long)deviceStarves_.load(),
			    deviceUp_.load() ? "up" : "down");
		std::fflush(stdout);
	};

	// Test-sink pacing anchor.
	auto nextDue = std::chrono::steady_clock::now();

	while (running_.load()) {
		size_t framesNeeded = 0;

		if (test) {
			// Wall-clock paced: one 10 ms chunk per period, drift-free against the
			// steady clock (the wake event only shortens the wait on stop()).
			nextDue += std::chrono::milliseconds(10);
			const auto now = std::chrono::steady_clock::now();
			if (nextDue > now) {
				const auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
							    nextDue - now)
							    .count();
				if (waitMs > 0)
					WaitForSingleObject(wake, DWORD(waitMs));
			}
			if (!running_.load())
				break;
			framesNeeded = kChunkFrames;
		} else {
			// Marshalled device swap / default-follow re-init: tear the current client
			// down fade-equivalently (the reopen below sheds backlog + fades back in)
			// and retry IMMEDIATELY -- a deliberate swap is not a fault, so it neither
			// waits the reconnect cadence nor counts as a recovery.
			bool deliberateSwap = false;
			if (reopenRequested_.exchange(false, std::memory_order_acq_rel)) {
				deliberateSwap = true;
				if (client)
					freeForReconnect();
				nextRetryMs = msNow();
			}
			// Reactive device acquisition: clientless means retry at the bounded
			// cadence while rings drop at capacity (never back-pressure).
			if (!client) {
				const int64_t now = msNow();
				if (now < nextRetryMs) {
					WaitForSingleObject(wake, 50);
					continue;
				}
				if (!openDevice())
					continue;
				deviceUp_.store(true);
				streamStart = nowSec(); // fresh warmup on every (re)open
				nextServo = streamStart + kServoPeriodSec;
				trimState.ema = 0.0;
				trimState.satSince = -1.0;
				devFillSum = 0.0;
				devFillCnt = 0;
				devFillMax = 0.0;
				resumeFade = 0;
				devFillAvgMs_.store(0.0);
				devFillMaxMs_.store(0.0);
				fifo.clear();
				fifoHead = 0;
				if (hadClient) {
					if (!deliberateSwap)
						deviceRecoveries_.fetch_add(1, std::memory_order_relaxed);
					// (Re)open resync: shed the backlog the gap piled up
					// (latency must not absorb it), fade back in.
					std::lock_guard<std::mutex> lock(mutex_);
					for (const auto &tap : taps_) {
						const size_t fill = tap->ring.fillFrames();
						const size_t prime = tap->primeTarget();
						if (fill > prime)
							tap->ring.discardFrames(fill - prime);
						taperHoldTail(tap->hold); // (re)open onset starts from silence
						tap->consFadeIn = kFadeFrames;
					}
				}
				hadClient = true;
			}
			const DWORD w = WaitForSingleObject(wake, kEventWaitMs);
			if (!running_.load())
				break;
			if (w != WAIT_OBJECT_0) {
				freeForReconnect(); // fail-open: event timeout = device fault
				continue;
			}
			UINT32 padding = 0;
			if (FAILED(client->GetCurrentPadding(&padding))) {
				freeForReconnect();
				continue;
			}
			// Queued device audio IS audible latency (the ring servo cannot see it): track
			// it past warmup -- the gate bounds the max against the write target.
			if (nowSec() - streamStart > kWarmupSec) {
				const double padMs = double(padding) * 1000.0 / double(devRate);
				devFillSum += padMs;
				++devFillCnt;
				devFillMax = std::max(devFillMax, padMs);
				devFillAvgMs_.store(devFillSum / double(devFillCnt));
				devFillMaxMs_.store(devFillMax);
				// The wake loop keeps the queue topped, so an empty queue mid-stream
				// means the thread stalled past the cushion and the engine rendered
				// silence (a hard cut). Count it and fade the resume seam back in --
				// the cut itself is unrecoverable, the seam need not be a second pop.
				if (padding == 0) {
					deviceStarves_.fetch_add(1, std::memory_order_relaxed);
					resumeFade = UINT32(devRate / 200); // ~5 ms at the device rate
				}
			}
			// Top the queue up to the write target only -- never to capacity (the
			// pre-fill-target gap above the target is reconnect/trim slack, not latency).
			framesNeeded = fillTargetFrames > padding ? fillTargetFrames - padding : 0;
			if (!framesNeeded)
				continue;
		}

		// With NO source attached, the device stream still advances on SILENT frames (the
		// engine never stalls the clock); the mixed-zero path below covers the test sink.
		bool noTaps;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			noTaps = taps_.empty();
		}
		if (!test && noTaps && fifoFrames() == 0) {
			BYTE *out = nullptr;
			if (FAILED(render->GetBuffer(UINT32(framesNeeded), &out)) ||
			    FAILED(render->ReleaseBuffer(UINT32(framesNeeded), AUDCLNT_BUFFERFLAGS_SILENT))) {
				freeForReconnect();
				continue;
			}
			const double nowSilent = nowSec();
			servoTick(nowSilent);
			logTick(nowSilent);
			continue;
		}

		// Top up the delivery fifo. The guard bounds the loop against a faulted resampler
		// (swr_convert persistently <= 0 would otherwise spin): bail to the recovery path.
		int mixGuard = 0;
		while (fifoFrames() < framesNeeded && running_.load()) {
			const size_t before = fifoFrames();
			mixChunk();
			if (fifoFrames() <= before && ++mixGuard > 64)
				break;
		}
		compactFifo();
		if (fifoFrames() < framesNeeded) {
			if (!test)
				freeForReconnect();
			continue;
		}

		if (test) {
			testSink_(fifo.data(), framesNeeded);
			fifoHead = framesNeeded * size_t(devChannels);
		} else {
			BYTE *out = nullptr;
			if (FAILED(render->GetBuffer(UINT32(framesNeeded), &out))) {
				freeForReconnect();
				continue;
			}
			if (resumeFade) {
				// Starvation resume: the engine just played silence, so ramp this
				// chunk in from zero (the write target is >> the fade, so one chunk
				// always completes it).
				const size_t n = std::min<size_t>(resumeFade, framesNeeded);
				for (size_t i = 0; i < n; ++i) {
					const float f = float(i + 1) / float(n + 1);
					for (int c = 0; c < devChannels; ++c)
						fifo[i * size_t(devChannels) + size_t(c)] *= f;
				}
				resumeFade = 0;
			}
			std::memcpy(out, fifo.data(), framesNeeded * size_t(devChannels) * sizeof(float));
			if (FAILED(render->ReleaseBuffer(UINT32(framesNeeded), 0))) {
				freeForReconnect();
				continue;
			}
			fifoHead = framesNeeded * size_t(devChannels);
		}

		const double now = nowSec();
		servoTick(now);
		logTick(now);
	}

	// Thread-side teardown (taps were removed by stop() before the join).
	if (client)
		client->Stop();
	if (render)
		render->Release();
	if (client)
		client->Release();
	destroySwr();
	deviceUp_.store(false);
	if (mmcss)
		AvRevertMmThreadCharacteristics(mmcss);
	if (comInit)
		CoUninitialize();
}

} // namespace moxrelay
