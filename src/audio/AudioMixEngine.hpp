// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioMixEngine -- the instance's audio production path: per-source capture taps feeding
// lock-free SPSC rings, mixed by ONE render thread onto a shared-mode WASAPI endpoint.
//
// SHAPE (the audio counterpart of SpoutSenderEngine, stack-owned beside it):
//   - attachSource registers one obs_source_add_audio_capture_callback on the source; the tap
//     fires on that source's OWN push thread (capture thread / media decode thread), applies the
//     engine-side volume/mute (the tap data is post-resample/post-balance/post-filter but
//     PRE-volume/PRE-mute -- volume lives in the consumer paths, never in the tap buffer), and
//     writes the gained samples into the source's ring. The attach edge is the audio lifecycle
//     edge: a source is mixed exactly while it broadcasts.
//   - The consumer is ONE event-driven shared-mode WASAPI render client per instance (MMCSS
//     "Pro Audio"), pulling every attached ring into a float accumulator (NaN-scrub + hard
//     clamp [-1, 1], no limiter -- the engine's own mixdown policy), then one swresample stage
//     from the 48 kHz stereo float line to the device mix format. The resampler is a directly
//     owned SwrContext (the libobs wrapper exposes no compensation hook).
//   - ANTI-CLICK: every discontinuity this mixer can cause is fade-wrapped -- volume changes
//     ramp ~10 ms at the producer (mute/unmute is the same ramp to/from zero, never a hard
//     gate); a tap timestamp jump (seek/loop/restart), a producer overflow drop, a ring
//     dry-out/refill, a detach, and a device-recovery restart each get a ~5 ms fade. A ~5 ms
//     consumer holdback reserves the fade material, so a stall at any chunk alignment still
//     gets the full-length taper (one constant ~5 ms of added latency).
//   - FILL SERVO: the consumer measures aggregate ring fill against a 20-30 ms target,
//     EMA-filters the error (tau ~4 s), and drives bounded swr_set_compensation (~+/-500 ppm,
//     ~250 ms cadence, warmup-gated past the post-Start transient) so clock drift between the
//     source clocks and the render device is corrected, not absorbed. A saturated servo falls
//     back to ONE fade-wrapped resync drop to target fill (counted; expected never).
//   - NEVER-BACK-PRESSURE: producers drop-on-full (counted); the render thread's device waits
//     are bounded (fail-open) and HRESULT failures trigger reactive reconnect at a bounded
//     cadence; nothing here ever blocks a source push thread or a verb thread.
//   - SYNC OFFSET: a per-source positive delay (0..950 ms) holds that many milliseconds of
//     ring fill resident beyond the working cushion, so the source's contribution runs that
//     far behind its producer (audio/video alignment against live-relayed video). The reserve
//     is fill the regulation layer is taught to ignore: the servo/trim operate on the
//     delay-exempt trough, every cushion/resync target adds the reserve on top (the capped
//     cushion math sees the capacity minus the reserve, so re-prime stays reachable at every
//     burst-envelope state), and live offset changes splice fade-wrapped -- an increase
//     refills through the priming path (the audible gap IS the added delay, attributed as
//     transport), a decrease discards exactly the removed reserve.
//
// METERS: per-source peak/RMS accumulate at the consumer's emission site (post-gain/mute, post
// delay reserve -- exactly the heard contribution entering the mix accumulator); the consumer
// accumulates the master meter post-sum PRE-clamp and latches whether the clamp engaged.
// stats() drains the accumulators as one snapshot window.
//
// THREADING: attachSource/detachSource/start/stop/stats run on any non-realtime thread (main
// thread in practice). Tap callbacks run on source push threads; the mix runs on the render
// thread. Rings are SPSC by construction (one producer thread per source, one consumer).
//
// LIFECYCLE CONTRACT: stop() (or the destructor) MUST run while libobs is still up -- tap
// removal touches live sources. Taps are removed BEFORE the render client is torn down, and
// detachSource must precede the caller's release of the source (the same discipline as the
// media-signal contexts). stop() and detachSource() are idempotent.

#pragma once

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace moxrelay {

class AudioDeviceNotify;

// One stats() window for a single attached source. Meter values are linear floats measured at
// the consumer's emission site post-gain/mute (the heard contribution); fill is the ring's
// current buffered duration, RAW -- a sync-offset reserve honestly shows as resident depth.
struct AudioSourceStats {
	std::string sourceId;
	double fillMs = 0.0;
	uint64_t drops = 0; // producer chunks dropped ring-full (cumulative)
	float peak = 0.0f;  // window sample-peak, max across channels
	float rms = 0.0f;   // window RMS, max across channels
};

// One stats() snapshot of the whole engine. Counters are cumulative since start(); meter values
// cover the window since the previous stats() call.
struct AudioEngineStats {
	bool deviceUp = false;   // render client currently open and started
	double fillMs = 0.0;     // mean fill of primed attached rings
	double fillMinMs = 0.0;  // windowed fill trough -- the value the servo regulates
	// Monotonic count of completed servo windows (one per fillMinMs publication). A sampler
	// that keys on this consumes each window exactly once; a free-running poll of fillMinMs
	// alone can double-read or skip windows as the two clocks drift through each other.
	uint64_t servoWindows = 0;
	double servoPpm = 0.0;   // current servo compensation command
	uint64_t producerDrops = 0;
	uint64_t underruns = 0;     // genuine starvation dry-outs (consumer outran a steady, still-pushing producer)
	uint64_t transportGaps = 0; // dry-outs attributed to transport (seek / loop / pause-resume / playback restart)
	uint64_t resyncs = 0;       // servo saturation backstop drops
	uint64_t deviceRecoveries = 0;
	// Render-thread stalls long enough to fully drain the device queue mid-stream (the engine
	// rendered silence). The resume seam is fade-wrapped; the count makes the stall visible.
	uint64_t deviceStarves = 0;
	// Device-queue fill (GetCurrentPadding) sampled each wake past warmup, since the current
	// stream opened. This is the audible-latency component the ring servo cannot see: the
	// write path must hold it at the ~30 ms target, never at buffer capacity.
	double deviceFillAvgMs = 0.0;
	double deviceFillMaxMs = 0.0;
	float masterPeak = 0.0f; // post-sum, pre-clamp
	float masterRms = 0.0f;
	bool clipped = false;    // the [-1,1] clamp engaged during the window
	std::vector<AudioSourceStats> sources;
};

class AudioMixEngine {
public:
	AudioMixEngine();
	~AudioMixEngine(); // calls stop() -- see the lifecycle contract above

	AudioMixEngine(const AudioMixEngine &) = delete;
	AudioMixEngine &operator=(const AudioMixEngine &) = delete;

	// TEST SINK SEAM: replaces the WASAPI render client with an in-process consumer receiving
	// the mixed output (interleaved stereo float at the 48 kHz line rate, post-clamp, post-
	// resampler -- the exact bytes a 48 kHz float endpoint would receive). The full production
	// path (rings, mixer, fades, servo, meters, resampler) runs identically; only the device
	// I/O is swapped, so the self-test asserts the engine without any audio endpoint. Must be
	// set before start(); refused while running.
	using TestSink = std::function<void(const float *interleaved, size_t frames)>;
	bool setTestSink(TestSink sink);

	// Start the render thread (device acquisition is reactive: a missing/failed endpoint is
	// retried at a bounded cadence while rings drop at capacity). Idempotent. Also registers
	// the default-endpoint notification client (real sink only) so a "default" selection
	// follows the system default render endpoint live.
	bool start();

	// Remove every remaining tap (taps FIRST, then the client -- the donor teardown order),
	// stop and join the render thread, release the device. Idempotent. Live taps are routed
	// through the drain-fade path and the render thread is given a short bounded window to
	// emit them, so process exit is fade-wrapped like every other mixer-caused discontinuity.
	void stop();

	// The render endpoint selection: a system endpoint id, or the "default" sentinel (follows
	// the system default render endpoint live). Stores the id and marshals the device swap to
	// the render thread (never blocks the caller); validation/probing is the CALLER's concern
	// (the verb layer probes synchronously, the boot path stores fail-open). Idempotent on the
	// current selection.
	void setOutputDevice(const std::string &deviceId);
	std::string outputDevice() const;

	bool isRunning() const;

	// Register the source's audio tap + ring. `sourceId` labels the per-source meter/stats
	// entry. The engine takes its own strong source ref (released at detach). Idempotent per
	// source. Call beside the sender-engine attach -- the attach edge IS the audio edge.
	void attachSource(obs_source_t *source, const std::string &sourceId);

	// Remove the tap (serializes against in-flight callbacks) and release the engine's ref.
	// MUST run before the caller releases the source. No-op on an unknown source.
	void detachSource(obs_source_t *source);

	// Stamp a transport discontinuity on the source's tap. A settings change restarts a playing
	// media pipeline, and the resume is timestamp-smoothed by the engine -- without this hint a
	// restart-induced dry-out would read as a starvation underrun. Attribution only; audio
	// handling is unchanged. No-op on an unknown source.
	void noteSourceTransport(obs_source_t *source);

	// Per-source sync-offset ceiling, milliseconds; values clamp here at every apply site.
	static constexpr int kSyncOffsetMaxMs = 950;

	// Set the source's audio delay (positive-only sync offset, clamped to 0..kSyncOffsetMaxMs).
	// Stores the target; the render thread applies it at its next chunk as one fade-wrapped
	// splice (increase: re-prime through the priming path -- the audible gap is the added
	// delay, attributed as transport; decrease: one discard of exactly the removed reserve).
	// Creation/boot-seeded offsets are read from the source's own stored sync offset at attach
	// instead (that store is inert for the capture-callback path this engine consumes), so a
	// seeded tap primes at cushion + reserve from silence with no splice at all. No-op on an
	// unknown source.
	void setSourceSyncOffset(obs_source_t *source, int offsetMs);

	// Snapshot counters + drain the meter window (peak/RMS accumulators reset; counters and
	// fill are point-in-time reads). Safe on any thread.
	AudioEngineStats stats();

private:
	struct Tap;

	static void audioCaptureThunk(void *param, obs_source_t *source, const struct audio_data *audio,
				      bool muted);
	void renderThread();

	mutable std::mutex mutex_; // guards taps_/running_/sink swap bookkeeping
	std::vector<std::shared_ptr<Tap>> taps_;
	std::thread thread_;
	std::atomic<bool> running_{false};
	void *wakeEvent_ = nullptr; // HANDLE; wakes the device wait for stop/test-sink pacing
	TestSink testSink_;

	// Endpoint selection ("default" or a system endpoint id). The verb/boot threads write
	// under devMutex_; the render thread reads under it at each (re)open. reopenRequested_
	// marshals a device swap (or a default-follow re-init) onto the render thread.
	mutable std::mutex devMutex_;
	std::string deviceId_ = "default";
	std::atomic<bool> reopenRequested_{false};
	std::unique_ptr<AudioDeviceNotify> notify_;

	// Cumulative counters (render thread writes; stats() reads).
	std::atomic<uint64_t> underruns_{0};
	std::atomic<uint64_t> transportGaps_{0};
	std::atomic<uint64_t> resyncs_{0};
	std::atomic<uint64_t> deviceRecoveries_{0};
	std::atomic<uint64_t> deviceStarves_{0};
	std::atomic<bool> deviceUp_{false};

	// Servo + fill telemetry (render thread writes; stats() reads).
	std::atomic<double> servoPpm_{0.0};
	std::atomic<double> meanFillMs_{0.0};
	std::atomic<double> fillMinMs_{0.0};
	std::atomic<uint64_t> servoWindows_{0}; // incremented after each fillMinMs_ publication
	std::atomic<double> devFillAvgMs_{0.0};
	std::atomic<double> devFillMaxMs_{0.0};

	// Master meter window accumulators (render thread writes; stats() drains).
	std::atomic<float> masterPeak_{0.0f};
	std::atomic<double> masterSumSq_{0.0};
	std::atomic<uint64_t> masterSamples_{0};
	std::atomic<bool> clipped_{false};
};

} // namespace moxrelay
