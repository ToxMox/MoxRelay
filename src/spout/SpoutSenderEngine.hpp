// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SpoutSenderEngine -- the per-source Spout sender engine (M2.1).
//
// A plain engine object, NOT a registered obs filter type. It owns ONE
// obs_add_main_render_callback and, per video frame, composites every attached source through its
// full filter chain into a Direct3D 11 texture and publishes that texture to a Spout2 DX sender in
// the slot's SenderFormat (fixed at attach).
//
// COLOR CONTRACT (derived from the libobs gs_color_format->DXGI mapping and sRGB/DXGI semantics):
//   - Srgb87 (DEFAULT): publish DXGI B8G8R8A8_UNORM (87) carrying sRGB-ENCODED bytes with NO sRGB
//     view, so receivers sampling raw bytes get correct color and never double-decode. Correct
//     compositing needs LINEAR-light blending, which needs an sRGB-capable render target; the
//     publish format (GS_BGRA_UNORM) has no UNORM_SRGB view and physically cannot encode, so the
//     8-bit path is two passes: PASS 1 composites in linear into a GS_BGRA intermediate (its
//     UNORM_SRGB RTV encodes linear->sRGB on store), PASS 2 launders the bytes verbatim into the
//     GS_BGRA_UNORM publish buffer (no sRGB RTV -> the store is always raw).
//   - Linear87: same 87 container but LINEAR 8-bit values. PASS 1 composites into a GS_RGBA16F
//     (linear half-float) intermediate, PASS 2 quantizes ONCE into 8-bit UNORM (a receiver that
//     assumes sRGB renders this dark -- the expected "linear 8-bit" semantics).
//   - Fp16: publish DXGI R16G16B16A16_FLOAT (10) linear half-float -- inherently linear with no
//     sRGB view, so the source composites straight into the publish buffer in a single pass.
//   All formats carry alpha untouched (the sRGB encode/decode never touches the A channel) and use
//   a REPLACE blend so semi-transparent pixels are not premultiplied against the cleared background.
//
// Why no filter type: obs_source_video_render(source) already dispatches through the source's full
// filter chain and libobs ticks ALL sources regardless of view membership, so a registered filter
// would add only a visibility gate that never primes in a headless process.
//
// THREADING:
//   - attach/detach/start/stop/slotInfos run on any non-graphics thread (UI / selftest).
//   - The render callback runs on the libobs GRAPHICS thread with the gs context entered.
//   - GPU objects (texrenders, the spoutDX instance) are created and destroyed ONLY on the
//     graphics thread; teardown is marshalled there via obs_queue_task(OBS_TASK_GRAPHICS, wait).
//   - The slot list mutex is held only for snapshots/bookkeeping, never across gs_*/Send calls.
//
// LIFECYCLE CONTRACT: stop() (or the destructor) MUST run while libobs is still up -- i.e. before
// ObsBootstrap::shutdown(). stop() and detach() are idempotent.

#pragma once

#include "SenderNameAllocator.hpp"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class spoutDX; // vendored SpoutDX (third_party/Spout2); full include stays in the .cpp

namespace moxrelay {

// One frame's engine-side timing sample (M2.4 perf harness; collected only when perf stats are
// enabled). Times are MILLISECONDS, summed across all slots rendered that frame.
struct PerfFrameSample {
	double pass1Ms = 0.0; // composite pass: source -> intermediate (or, for Fp16, straight to output)
	double pass2Ms = 0.0; // 8-bit launder pass: intermediate -> UNORM publish buffer (0 for Fp16)
	double sendMs = 0.0;  // SendTexture / SendTextureNoFlush calls
	double flushMs = 0.0; // batched mode only: the single end-of-frame context Flush
	uint32_t slotsSent = 0;
};

// Pixel format a slot's sender emits, fixed at attach (the wire SourceFormat vocabulary).
// A format change is detach + re-attach by design -- Spout's shared texture is per-name and
// receivers assume a stable format per sender (CheckSender derives the registered format from
// the texture desc, so the texrender format IS the published format; no SetSenderFormat call).
enum class SenderFormat {
	Srgb87,   // DXGI 87, sRGB-encoded bytes (DEFAULT)
	Linear87, // DXGI 87, linear bytes (non-decoding receivers only)
	Fp16,     // DXGI 10, linear half-float HDR
};

// A point-in-time snapshot of one sender slot, safe to read on any thread.
struct SenderSlotInfo {
	int slotId = -1;
	std::string requestedName; // the caller-requested name (SpoutNaming scheme)
	std::string resolvedName;  // after the pre-create collision resolve (set at slot init)
	std::string actualName;    // GetName() read-back after the first successful send ("" before)
	uint64_t sends = 0;        // successful SendTexture count
	bool lastSendOk = false;
	bool initFailed = false;
	bool audioOnly = false; // audio-without-video slot: no GPU objects, no sender name, sends stay 0
	SenderFormat format = SenderFormat::Srgb87;
};

class SpoutSenderEngine {
public:
	// How sends hit the GPU queue. Batched is the PRODUCT DEFAULT (promoted
	// 2026-06-09 after the measured ~5-7x A/B delta and a full M2.3 fleet-gate re-run); Immediate
	// remains selectable via the --perf harness for A/B comparison.
	//   Immediate -- SendTexture per sender: one immediate-context Flush per sender per frame.
	//   Batched   -- SendTextureNoFlush per sender, ONE Flush after all slots, then a per-sender
	//                frame signal (flush-before-signal rule).
	enum class FlushMode { Immediate, Batched };

	// Both defined in the .cpp: the members include std::unique_ptr<Slot> with Slot incomplete
	// here, so neither may be instantiated inline.
	SpoutSenderEngine();
	~SpoutSenderEngine(); // calls stop() -- see the lifecycle contract above

	SpoutSenderEngine(const SpoutSenderEngine &) = delete;
	SpoutSenderEngine &operator=(const SpoutSenderEngine &) = delete;

	// Register the engine's single main render callback. Idempotent; cheap with zero slots.
	bool start();

	// Remove the render callback, tear down every slot's GPU objects on the graphics thread,
	// release all source refs. Idempotent; safe to call twice (asserted by the self-test).
	void stop();

	bool isRunning();

	// Attach a source: the engine takes its OWN strong ref + obs_source_inc_active (MAIN_VIEW
	// activation raises BOTH the showing and active counts -- attached-to-sender IS this app's
	// "live" state: media sources start/stop playback with attach/detach, and capture sources
	// keep their capture state exactly as under the previous showing-only ref). The sender name
	// is built from the
	// SpoutNaming scheme ({machine}:Helper_{port}_{sourceName}) and PRE-RESOLVED through the
	// SenderNameAllocator (M2.2): session-registry probe + process-local reservation, so two
	// same-frame attaches can never race to one name and Spout never silently auto-suffixes a
	// published name. All name strings are owned copies (never an alias into transient settings
	// memory). The sender itself is created lazily on the
	// graphics thread at the next frame. Returns a slot id, or -1 (no source / no free name).
	// `format` is fixed for the slot's lifetime (format change = detach + re-attach).
	int attach(obs_source_t *source, const std::string &machine, int port, const std::string &sourceName,
		   SenderFormat format = SenderFormat::Srgb87);

	// Detach one slot: GPU teardown is marshalled to the graphics thread (blocking), then the
	// source ref is released. Safe while frames are in flight. No-op on an unknown/dead id.
	void detach(int slotId);

	// Count of live (attached, not init-failed) slots.
	size_t activeCount();

	// Snapshot of all live slots (status bar / self-test assertions).
	std::vector<SenderSlotInfo> slotInfos();

	// TRUE if `name` is currently registered in the session-wide Spout sender registry (a pure
	// shared-memory lookup -- no D3D init). Used by the self-test's enumeration assertion.
	static bool senderRegistered(const std::string &name);

	// Wire/config vocabulary for SenderFormat ("srgb87" | "linear87" | "fp16" -- the contract's
	// SourceFormat enum doubles as the fleet-config value set). parseFormat returns false on an
	// unknown name (callers reject; never a silent default).
	static bool parseFormat(const std::string &name, SenderFormat &out);
	static const char *formatName(SenderFormat format);

	// ---- M2.4 perf harness surface (no effect on product paths unless called) ----

	// Select the flush mode. MUST be called before start() (refused while running -- the mode
	// is read lock-free on the graphics thread).
	bool setFlushMode(FlushMode mode);
	FlushMode flushMode();

	// Set the process-wide Spout sender cap (max simultaneously published senders). MUST be called
	// before start() (refused while running -- read lock-free on the graphics thread, same contract
	// as the flush mode). Applied at the first slot's init via the SDK's SetMaxSenders; the value
	// is clamped to >= 1 at that call site. Resolved from AppSettings().maxSenders() (default 64).
	bool setMaxSenders(int maxSenders);

	// Enable per-frame timing collection (harness only -- a few hundred ns/frame when on).
	void enablePerfStats(bool on);

	// Drain all samples collected since the last drain (harness reads once per measure window).
	std::vector<PerfFrameSample> drainPerfSamples();

private:
	struct Slot;

	static void renderAll(void *param, uint32_t cx, uint32_t cy); // graphics thread
	void renderAllImpl();
	// Renders one slot; accumulates timings into `sample` when stats are on. In batched mode a
	// successful copy appends the slot's sender to `toSignal` (signalled after the single Flush).
	void renderSlot(Slot *slot, bool batched, bool collectStats, PerfFrameSample &sample,
			std::vector<spoutDX *> &toSignal); // graphics thread, gs context entered
	bool initSlotOnRenderThread(Slot *slot);           // graphics thread
	static void teardownSlotGpuTask(void *param);      // graphics thread (queued task)

	std::mutex mutex_; // guards slots_/running_/nextSlotId_ + the Slot bookkeeping fields
	std::vector<std::unique_ptr<Slot>> slots_;
	SenderNameAllocator allocator_; // pre-create name resolution + local reservations (own lock)
	bool running_ = false;
	int nextSlotId_ = 1;
	uint32_t lastRenderedFrame_ = 0; // multi-mix guard (callback fires once per video mix)

	// Perf surface state. flushMode_ is written only while not running (setFlushMode contract)
	// and read on the graphics thread; statsEnabled_ is an atomic toggle; the sample buffer has
	// its own lock (never held across gs_*/Send calls -- appended once per frame).
	FlushMode flushMode_ = FlushMode::Batched; // product default (see FlushMode docs)
	int maxSenders_ = 64; // process-wide Spout sender cap; set before start() (setMaxSenders contract)
	std::atomic<bool> statsEnabled_{false};
	std::mutex statsMutex_;
	std::vector<PerfFrameSample> samples_;
};

} // namespace moxrelay
