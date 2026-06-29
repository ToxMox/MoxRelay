// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// SpoutSenderEngine implementation. Per video frame, each slot composites its source through the
// source's full filter chain and publishes a Direct3D 11 texture to a Spout2 DX sender, in the
// slot's SenderFormat. The render expression follows the engine's color contract (see the header):
//
//   8-bit formats (Srgb87 / Linear87) -- TWO passes, because correct compositing needs LINEAR-light
//   blending (an sRGB-capable / float render target) but the publish buffer is GS_BGRA_UNORM
//   (DXGI 87, plain UNORM, NO sRGB view) which physically cannot encode linear->sRGB on store:
//     PASS 1: composite the source in linear light into an encode-capable intermediate
//             (Srgb87 -> GS_BGRA, whose UNORM_SRGB RTV encodes linear->sRGB on store; Linear87 ->
//             GS_RGBA16F linear half-float). gs_set_linear_srgb(true) is the MASTER switch the
//             source render path reads (gs_get_linear_srgb()) to bind sRGB SRVs (decode on sample)
//             and manage its own framebuffer-srgb; the target color space alone does not trigger it.
//     PASS 2: launder the intermediate verbatim into the GS_BGRA_UNORM publish buffer with the
//             default effect under a REPLACE blend. The output has no sRGB RTV, so the store is
//             always raw: Srgb87 transfers its sRGB bytes byte-for-byte (raw SRV, no decode);
//             Linear87 samples the linear floats and quantizes ONCE into 8-bit linear.
//
//   Fp16 -- ONE pass: GS_RGBA16F (DXGI 10) is inherently linear with no sRGB view, so the source
//   composites straight into the publish buffer and stores linear half-floats.
//
// DOUBLE BUFFER, LOW LATENCY: two publish texrenders (curr/prev). Each frame renders into curr,
// publishes curr (the buffer JUST rendered -- zero added latency, no priming frame), then flips
// curr/prev so the next frame renders into the other buffer while the published buffer's
// CopyResource drains. The spoutDX sender ADOPTS the OBS D3D11 device (gs_get_device_obj) -- never
// a device per sender -- so every Spout copy runs in order on the OBS immediate context, after the
// draws that produced the texture. All SpoutDX texture calls stay on the graphics thread.

#include "SpoutSenderEngine.hpp"

#include "app/SpoutNaming.hpp"

#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include "SpoutDX.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace moxrelay {

struct SpoutSenderEngine::Slot {
	int id = 0;

	// Set at attach (caller thread), constant afterwards.
	obs_source_t *source = nullptr; // STRONG ref, engine-owned (+ one showing ref)
	std::string requestedName;      // OWNED copy -- never an alias into transient settings memory
	SenderFormat format = SenderFormat::Srgb87; // fixed per attach (change = detach + re-attach)
	bool audioOnly = false; // audio-without-video: live slot with no GPU objects and no sender name

	// Graphics-thread-only after init.
	spoutDX *sender = nullptr;
	gs_texrender_t *curr = nullptr; // publish buffer: GS_BGRA_UNORM (87) or GS_RGBA16F (10, Fp16)
	gs_texrender_t *prev = nullptr; // second publish buffer, same format (curr/prev flip per frame)
	// PASS-1 linear-composite target, 8-bit formats only: GS_BGRA for Srgb87 (owns the UNORM_SRGB
	// encode RTV), GS_RGBA16F for Linear87 (linear half-float); null for Fp16 (single pass).
	gs_texrender_t *intermediate = nullptr;
	bool initialized = false;

	// Bookkeeping (written on the graphics thread / read anywhere) -- guarded by engine mutex_.
	std::string resolvedName;
	std::string actualName;
	uint64_t sends = 0;
	bool lastSendOk = false;
	bool initFailed = false;
	bool dead = false; // excluded from rendering; GPU teardown queued/done
	bool divergenceLogged = false;
};

SpoutSenderEngine::SpoutSenderEngine() = default;

SpoutSenderEngine::~SpoutSenderEngine()
{
	stop();
}

bool SpoutSenderEngine::start()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_)
		return true;
	if (!obs_initialized()) {
		std::fprintf(stderr, "[SpoutSenderEngine] start() before libobs is up -- refused\n");
		return false;
	}
	obs_add_main_render_callback(&SpoutSenderEngine::renderAll, this);
	running_ = true;
	return true;
}

// Queued-task payload: tear down ONE slot's GPU objects. Runs on the graphics thread (or inline
// if already there), which serializes it against renderAll -- by the time it runs, no render
// iteration can be touching the slot (it was marked dead under the mutex first).
void SpoutSenderEngine::teardownSlotGpuTask(void *param)
{
	auto *slot = static_cast<Slot *>(param);

	if (slot->sender) {
		slot->sender->ReleaseSender();
		slot->sender->CloseDirectX11(); // flushes the shared immediate context -- graphics thread only
		delete slot->sender;
		slot->sender = nullptr;
	}
	if (slot->intermediate) {
		gs_texrender_destroy(slot->intermediate);
		slot->intermediate = nullptr;
	}
	if (slot->prev) {
		gs_texrender_destroy(slot->prev);
		slot->prev = nullptr;
	}
	if (slot->curr) {
		gs_texrender_destroy(slot->curr);
		slot->curr = nullptr;
	}
	slot->initialized = false;
}

void SpoutSenderEngine::stop()
{
	// 1. Remove the render callback FIRST. obs_remove_main_render_callback takes the same mutex
	//    the callbacks execute under, so once it returns no callback is mid-flight and none will
	//    fire again -- the synchronization point for everything below. Idempotent via running_.
	bool wasRunning = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!running_ && slots_.empty())
			return;
		wasRunning = running_;
		running_ = false;
	}
	if (wasRunning && obs_initialized())
		obs_remove_main_render_callback(&SpoutSenderEngine::renderAll, this);

	// 2. Claim OWNERSHIP of every slot record (move the unique_ptrs out under the mutex), then
	//    tear down what we own: GPU objects on the graphics thread (blocking; guarded on
	//    obs_initialized() so a stop() after libobs is gone degrades to a leak report instead of
	//    a crash), source refs + name reservation on this thread. Ownership -- not raw pointers
	//    into slots_ -- is what makes a concurrent detach() safe: whichever side wins the mutex
	//    owns the record outright, so nothing is ever double-freed or freed under the other's
	//    feet. The loop re-checks slots_ so an attach() racing stop() can never leak: its slot
	//    lands in slots_ and is claimed by the next pass (it never initialized -- the callback is
	//    already removed -- so it owns no GPU objects).
	for (;;) {
		std::vector<std::unique_ptr<Slot>> taken;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			if (slots_.empty())
				break;
			taken = std::move(slots_);
			slots_.clear(); // moved-from state is unspecified; make "empty" definite
		}
		for (auto &owned : taken) {
			owned->dead = true;
			// Audio-only slots own no GPU objects and never enter a render snapshot --
			// nothing to tear down or serialize against on the graphics thread.
			if (!owned->audioOnly && obs_initialized())
				obs_queue_task(OBS_TASK_GRAPHICS, &SpoutSenderEngine::teardownSlotGpuTask,
					       owned.get(), true);
			if (owned->source) {
				obs_source_dec_active(owned->source); // pairs the attach-time inc_active
				obs_source_release(owned->source);
				owned->source = nullptr;
			}
			if (!owned->resolvedName.empty())
				allocator_.release(owned->resolvedName);
		} // <- the records die here, AFTER the graphics thread is provably done with them
	}
}

bool SpoutSenderEngine::isRunning()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return running_;
}

bool SpoutSenderEngine::parseFormat(const std::string &name, SenderFormat &out)
{
	if (name == "srgb87") {
		out = SenderFormat::Srgb87;
		return true;
	}
	if (name == "linear87") {
		out = SenderFormat::Linear87;
		return true;
	}
	if (name == "fp16") {
		out = SenderFormat::Fp16;
		return true;
	}
	return false;
}

const char *SpoutSenderEngine::formatName(SenderFormat format)
{
	switch (format) {
	case SenderFormat::Linear87:
		return "linear87";
	case SenderFormat::Fp16:
		return "fp16";
	case SenderFormat::Srgb87:
		break;
	}
	return "srgb87";
}

int SpoutSenderEngine::attach(obs_source_t *source, const std::string &machine, int port,
			      const std::string &sourceName, SenderFormat format)
{
	if (!source || sourceName.empty())
		return -1;

	// Audio-without-video sources (audio capture, audio-only media) hold a live slot -- the
	// active ref is the app's broadcast edge -- but never render: no GPU objects are created and
	// no sender name is allocated or reserved (there is no video to publish; senderName stays
	// null for the slot's lifetime by construction).
	const uint32_t outputFlags = obs_source_get_output_flags(source);
	const bool audioOnly = (outputFlags & OBS_SOURCE_AUDIO) && !(outputFlags & OBS_SOURCE_VIDEO);

	// Pre-resolve the published name NOW (M2.2 allocator): registry probe + local reservation.
	// Resolution must happen at attach, not at graphics-thread init -- a sender only appears in
	// the session registry at its FIRST SendTexture, so two attaches in the same frame would both
	// probe "free" and collide without the local reservation.
	std::string requested;
	std::string resolved;
	if (!audioOnly) {
		requested = SpoutNaming::makeSenderName(machine, port, sourceName);
		resolved = allocator_.resolve(machine, port, sourceName);
		if (resolved.empty()) {
			std::fprintf(stderr,
				     "[SpoutSenderEngine] attach refused: no free sender name for '%s'\n",
				     requested.c_str());
			return -1;
		}
		if (resolved != requested) {
			std::fprintf(stderr,
				     "[SpoutSenderEngine] name collision: requested '%s' -> resolved '%s'\n",
				     requested.c_str(), resolved.c_str());
		}
	}

	obs_source_t *ref = obs_source_get_ref(source);
	if (!ref) {
		if (!resolved.empty())
			allocator_.release(resolved);
		return -1; // source already destroyed
	}
	// MAIN_VIEW activation (raises showing AND active): attached-to-sender is the app's "live"
	// state -- media playback gates on obs_source_active(), capture sources only need showing.
	obs_source_inc_active(ref);

	auto slot = std::make_unique<Slot>();
	slot->source = ref;
	slot->requestedName = requested;
	slot->resolvedName = resolved;
	slot->format = format;
	slot->audioOnly = audioOnly;

	std::lock_guard<std::mutex> lock(mutex_);
	slot->id = nextSlotId_++;
	const int id = slot->id;
	slots_.push_back(std::move(slot));
	return id;
}

void SpoutSenderEngine::detach(int slotId)
{
	// 1. Claim OWNERSHIP of the record under the mutex (move it out of slots_) -- every render
	//    snapshot taken after this excludes it, and no concurrent stop()/detach() can reach it
	//    at all (exactly-once teardown by construction, no raw-pointer lifetime to reason about).
	std::unique_ptr<Slot> owned;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto it = slots_.begin(); it != slots_.end(); ++it) {
			if ((*it)->id == slotId && !(*it)->dead) {
				(*it)->dead = true;
				owned = std::move(*it);
				slots_.erase(it);
				break;
			}
		}
	}
	if (!owned)
		return;

	// 2. GPU teardown on the graphics thread (blocking). A render iteration that snapshotted the
	//    slot BEFORE step 1 has finished by the time this queued task runs (same thread), which
	//    is why the record must stay alive until here -- hence ownership, not erase-then-free.
	//    Audio-only slots own no GPU objects and never enter a render snapshot: skip the queue.
	if (!owned->audioOnly && obs_initialized())
		obs_queue_task(OBS_TASK_GRAPHICS, &SpoutSenderEngine::teardownSlotGpuTask, owned.get(), true);

	// 3. Release the source refs + the name reservation. We are the sole owner; no other thread
	//    can touch this record.
	if (owned->source) {
		obs_source_dec_active(owned->source); // pairs the attach-time inc_active
		obs_source_release(owned->source);
		owned->source = nullptr;
	}
	if (!owned->resolvedName.empty())
		allocator_.release(owned->resolvedName);
	// `owned` destructs here -- after the graphics thread is provably done with the slot.
}

size_t SpoutSenderEngine::activeCount()
{
	std::lock_guard<std::mutex> lock(mutex_);
	size_t n = 0;
	for (const auto &s : slots_) {
		if (!s->dead && !s->initFailed)
			n++;
	}
	return n;
}

std::vector<SenderSlotInfo> SpoutSenderEngine::slotInfos()
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<SenderSlotInfo> out;
	out.reserve(slots_.size());
	for (const auto &s : slots_) {
		if (s->dead)
			continue;
		SenderSlotInfo info;
		info.slotId = s->id;
		info.requestedName = s->requestedName;
		info.resolvedName = s->resolvedName;
		info.actualName = s->actualName;
		info.sends = s->sends;
		info.lastSendOk = s->lastSendOk;
		info.initFailed = s->initFailed;
		info.audioOnly = s->audioOnly;
		info.format = s->format;
		out.push_back(std::move(info));
	}
	return out;
}

bool SpoutSenderEngine::senderRegistered(const std::string &name)
{
	// Pure shared-memory registry lookup; no D3D11 init involved.
	spoutSenderNames names;
	return names.FindSenderName(name.c_str());
}

bool SpoutSenderEngine::setFlushMode(FlushMode mode)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_) {
		// flushMode_ is read lock-free on the graphics thread; changing it mid-run would
		// race (and mid-run mode flips are not a meaningful measurement anyway).
		std::fprintf(stderr, "[SpoutSenderEngine] setFlushMode refused while running\n");
		return false;
	}
	flushMode_ = mode;
	return true;
}

bool SpoutSenderEngine::setMaxSenders(int maxSenders)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_) {
		// maxSenders_ is read lock-free on the graphics thread (first-slot init); changing it
		// mid-run would race, and the SetMaxSenders call only happens once per process anyway.
		std::fprintf(stderr, "[SpoutSenderEngine] setMaxSenders refused while running\n");
		return false;
	}
	maxSenders_ = maxSenders;
	return true;
}

SpoutSenderEngine::FlushMode SpoutSenderEngine::flushMode()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return flushMode_;
}

void SpoutSenderEngine::enablePerfStats(bool on)
{
	statsEnabled_.store(on, std::memory_order_relaxed);
	if (!on) {
		std::lock_guard<std::mutex> lock(statsMutex_);
		samples_.clear();
	}
}

std::vector<PerfFrameSample> SpoutSenderEngine::drainPerfSamples()
{
	std::vector<PerfFrameSample> out;
	std::lock_guard<std::mutex> lock(statsMutex_);
	out.swap(samples_);
	return out;
}

// ===== GRAPHICS THREAD from here down. gs context is already entered (the main render callback
// fires inside render_main_texture); never obs_enter_graphics here, never touch Qt. =====

void SpoutSenderEngine::renderAll(void *param, uint32_t cx, uint32_t cy)
{
	(void)cx;
	(void)cy;
	static_cast<SpoutSenderEngine *>(param)->renderAllImpl();
}

void SpoutSenderEngine::renderAllImpl()
{
	// Multi-mix guard: main render callbacks fire once per video MIX per frame. The GUI process
	// has the preview display (one mix); should an extra view/canvas ever appear, this keeps each
	// sender at exactly one send per video frame.
	const uint32_t frame = obs_get_total_frames();
	if (frame != 0 && frame == lastRenderedFrame_)
		return;
	lastRenderedFrame_ = frame;

	// Snapshot live slots under the mutex; render outside it (never hold our mutex across
	// obs/gs/Send calls). flushMode_ is constant while running (setFlushMode contract), so reading
	// it under the same lock is exact.
	std::vector<Slot *> live;
	bool batched = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		live.reserve(slots_.size());
		for (auto &s : slots_) {
			if (!s->dead && !s->initFailed && !s->audioOnly)
				live.push_back(s.get());
		}
		batched = (flushMode_ == FlushMode::Batched);
	}

	const bool collectStats = statsEnabled_.load(std::memory_order_relaxed);
	PerfFrameSample sample;
	std::vector<spoutDX *> toSignal; // batched mode: senders to signal after the single Flush
	if (batched)
		toSignal.reserve(live.size());

	for (Slot *slot : live)
		renderSlot(slot, batched, collectStats, sample, toSignal);

	// Batched mode (M2.4 A/B lever): every slot's copy is queued; ONE Flush submits them all,
	// THEN each sender gets its frame signal (signal must never precede the flush). All senders
	// share the one OBS immediate context, so any sender's context works.
	if (batched && !toSignal.empty()) {
		const auto t0 = std::chrono::steady_clock::now();
		if (ID3D11DeviceContext *ctx = toSignal.front()->GetDX11Context())
			ctx->Flush();
		if (collectStats)
			sample.flushMs = std::chrono::duration<double, std::milli>(
						 std::chrono::steady_clock::now() - t0)
						 .count();
		for (spoutDX *sender : toSignal)
			sender->SignalNewFrame();
	}

	if (collectStats && !live.empty()) {
		std::lock_guard<std::mutex> lock(statsMutex_);
		// Backstop only -- the harness drains every measure window; 100k frames is minutes of
		// leakage headroom, not a working size.
		if (samples_.size() < 100000)
			samples_.push_back(sample);
	}
}

bool SpoutSenderEngine::initSlotOnRenderThread(Slot *slot)
{
	// Two publish buffers in the slot's SenderFormat: GS_BGRA_UNORM (DXGI 87) for the 8-bit
	// formats, GS_RGBA16F (DXGI 10) for Fp16. The underlying texture's DXGI format IS the published
	// format: CheckSender derives the registered sender format from the texture desc on the first
	// send, so no explicit SetSenderFormat call is needed.
	const gs_color_format outFormat = (slot->format == SenderFormat::Fp16) ? GS_RGBA16F : GS_BGRA_UNORM;
	slot->curr = gs_texrender_create(outFormat, GS_ZS_NONE);
	slot->prev = gs_texrender_create(outFormat, GS_ZS_NONE);

	// PASS-1 linear-composite target, allocated only for the 8-bit formats (Fp16 composites
	// straight into its own publish buffer -- GS_RGBA16F is inherently linear with no sRGB view):
	//   Srgb87   -> GS_BGRA    (typeless resource owning the UNORM_SRGB encode RTV)
	//   Linear87 -> GS_RGBA16F (full-precision linear half-float)
	slot->intermediate = (slot->format == SenderFormat::Srgb87)
				     ? gs_texrender_create(GS_BGRA, GS_ZS_NONE)
			     : (slot->format == SenderFormat::Linear87)
				     ? gs_texrender_create(GS_RGBA16F, GS_ZS_NONE)
				     : nullptr;

	slot->sender = new spoutDX;

	// Raise the global Spout sender cap once per PROCESS (a registry write, not per-sender state) to
	// the configured maxSenders_ (from AppSettings, default 64), clamped to >= 1. Graphics-thread-only
	// code path, so a plain local static is race-free. The vendored overrun-safe clamp (SpoutShared-
	// Memory/SpoutSenderNames) keeps a larger cap from reading past a smaller pre-existing shared map.
	static bool maxSendersSet = false;
	if (!maxSendersSet) {
		slot->sender->SetMaxSenders(maxSenders_ < 1 ? 1 : maxSenders_);
		maxSendersSet = true;
	}

	// Reuse the OBS D3D11 device -- never a device per sender (requires the gs context, which the
	// main render callback guarantees).
	auto *device = static_cast<ID3D11Device *>(gs_get_device_obj());
	if (!device) {
		std::fprintf(stderr, "[SpoutSenderEngine] gs_get_device_obj returned null\n");
		return false;
	}
	slot->sender->OpenDirectX11(device);
	// OpenDirectX11 returns true UNCONDITIONALLY -- the real check is the device pointer.
	if (!slot->sender->GetDX11Device()) {
		std::fprintf(stderr, "[SpoutSenderEngine] spoutDX adopted no D3D11 device\n");
		return false;
	}

	// The name was pre-resolved (and locally reserved) by the SenderNameAllocator at attach;
	// constant since. The GetName() read-back after the first send remains the backstop for the
	// residual attach->first-send race (a foreign process registering the name in between).
	slot->sender->SetSenderName(slot->resolvedName.c_str());

	slot->initialized = true;
	return true;
}

void SpoutSenderEngine::renderSlot(Slot *slot, bool batched, bool collectStats, PerfFrameSample &sample,
				   std::vector<spoutDX *> &toSignal)
{
	using clock = std::chrono::steady_clock;
	const auto msSince = [](clock::time_point t0) {
		return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
	};

	if (!slot->initialized) {
		if (!initSlotOnRenderThread(slot)) {
			std::fprintf(stderr, "[SpoutSenderEngine] slot %d init failed -- sender disabled\n", slot->id);
			std::lock_guard<std::mutex> lock(mutex_);
			slot->initFailed = true;
			return;
		}
	}

	// The texrenders track the SOURCE's own dimensions (per-source native resolution is free);
	// 0x0 (closed window, warming capture) skips the frame -- the sender freezes on the last
	// frame, which is the designed Spout behavior (receivers keep displaying it).
	const uint32_t width = obs_source_get_base_width(slot->source);
	const uint32_t height = obs_source_get_base_height(slot->source);
	if (width == 0 || height == 0)
		return;

	const SenderFormat fmt = slot->format;

	struct vec4 background;
	vec4_zero(&background); // transparent black; encoding-invariant at 0.

	// Capture the host's ambient device sRGB state so this shared main-render callback leaves the
	// compositor exactly as it found it.
	const bool prevLin = gs_get_linear_srgb();
	const bool prevFb = gs_framebuffer_srgb_enabled();

	// Set true ONLY when the publish buffer's (curr) begin succeeds this frame. A mid-stream begin
	// failure (e.g. device lost) leaves curr holding a stale texture, so without this guard the
	// publish below would send a stale frame AND advance the buffer -- gate both on a fresh render.
	bool publishBegan = false;

	if (fmt == SenderFormat::Fp16) {
		// SINGLE PASS: GS_RGBA16F (DXGI 10) is inherently linear with no sRGB view, so the
		// source composites straight into the publish buffer and stores linear half-floats.
		const auto passStart = clock::now();
		gs_set_linear_srgb(true); // MASTER switch: sources decode on sample, store linear
		gs_texrender_reset(slot->curr);
		if (gs_texrender_begin_with_color_space(slot->curr, width, height, GS_CS_SRGB_16F)) {
			gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
			gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

			// REPLACE blend: store the source's emitted RGBA verbatim (straight,
			// non-premultiplied alpha). The default SRCALPHA/INVSRCALPHA would premultiply
			// against the cleared black background and darken semi-transparent pixels.
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
			obs_source_video_render(slot->source);
			gs_blend_state_pop();

			gs_texrender_end(slot->curr);
			publishBegan = true;
		}
		if (collectStats)
			sample.pass1Ms += msSince(passStart);
	} else {
		// TWO PASS (Srgb87 / Linear87). Correct compositing needs LINEAR-light blending, which
		// needs an sRGB-capable / float render target; the GS_BGRA_UNORM publish buffer (DXGI 87,
		// plain UNORM, no sRGB view) physically cannot encode linear->sRGB on store, so PASS 1
		// composites into an encode-capable intermediate and PASS 2 launders it verbatim.

		// PASS 1: composite source -> intermediate in LINEAR light.
		const auto pass1Start = clock::now();
		gs_set_linear_srgb(true); // MASTER switch the source render path reads
		gs_texrender_reset(slot->intermediate);
		const gs_color_space space1 = (fmt == SenderFormat::Srgb87) ? GS_CS_SRGB : GS_CS_SRGB_16F;
		if (gs_texrender_begin_with_color_space(slot->intermediate, width, height, space1)) {
			// Srgb87's GS_BGRA target encodes linear->sRGB through its UNORM_SRGB RTV, which
			// requires framebuffer-srgb ON. Linear87's target is GS_RGBA16F (FLOAT), where the
			// flag is a no-op; keep it OFF to make the linear intent explicit.
			gs_enable_framebuffer_srgb(fmt == SenderFormat::Srgb87);

			gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
			gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO); // straight alpha
			obs_source_video_render(slot->source);
			gs_blend_state_pop();

			gs_enable_framebuffer_srgb(false);
			gs_texrender_end(slot->intermediate);
		}
		if (collectStats)
			sample.pass1Ms += msSince(pass1Start);
		// After PASS 1: Srgb87 intermediate holds composited 8-bit sRGB bytes; Linear87
		// intermediate holds full-precision linear half-float.

		// PASS 2: launder intermediate -> curr (GS_BGRA_UNORM, DXGI 87). The output has no sRGB
		// RTV, so the store is ALWAYS verbatim -- whatever the shader emits is stored.
		const auto pass2Start = clock::now();
		gs_set_linear_srgb(false); // determinism; the SRV choice below is explicit/ungated anyway
		gs_texrender_reset(slot->curr);
		if (gs_texrender_begin_with_color_space(slot->curr, width, height, GS_CS_SRGB)) {
			gs_enable_framebuffer_srgb(false); // physically inert on a UNORM-only target
			gs_clear(GS_CLEAR_COLOR, &background, 0.0f, 0);
			gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);

			// REPLACE blend so the blit copies RGBA bit-for-bit instead of premultiplying
			// against the cleared background.
			gs_blend_state_push();
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
			gs_texture_t *tex = gs_texrender_get_texture(slot->intermediate);
			if (tex) {
				// Bind the RAW (non-sRGB) SRV. Srgb87: the intermediate's sRGB-encoded bytes
				// transfer byte-perfect into the 87 resource without decoding (the _srgb SRV
				// would wrongly decode them to linear). Linear87: the FLOAT intermediate's raw
				// and _srgb SRVs are equivalent; this samples the linear half-floats and the
				// UNORM store performs a SINGLE quantize to linear 8-bit.
				gs_effect_set_texture(image, tex);

				while (gs_effect_loop(effect, "Draw"))
					gs_draw_sprite(tex, 0, width, height);
			}

			gs_blend_state_pop();
			gs_enable_framebuffer_srgb(false);
			gs_texrender_end(slot->curr);
			publishBegan = true;
		}
		if (collectStats)
			sample.pass2Ms += msSince(pass2Start);
	}

	// Restore the host's ambient sRGB state.
	gs_set_linear_srgb(prevLin);
	gs_enable_framebuffer_srgb(prevFb);

	// PUBLISH: curr already holds this frame's final published bytes. Send the CURRENT frame (the
	// buffer just rendered) -- zero added latency, no priming/first-frame skip -- then flip the
	// double buffer. The adopted OBS immediate context runs the recorded copy in order, after the
	// draws that produced curr; batched mode records the copy only (no flush, no frame signal),
	// and renderAllImpl issues the single shared Flush + per-sender signals afterwards.
	gs_texture_t *curr_tex = publishBegan ? gs_texrender_get_texture(slot->curr) : nullptr;
	if (!curr_tex)
		return; // begin failed (e.g. device lost): nothing valid to send, do NOT flip the buffer

	auto *curr_d3d11 = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(curr_tex));
	const auto sendStart = clock::now();
	const bool ok = batched ? slot->sender->SendTextureNoFlush(curr_d3d11)
				: slot->sender->SendTexture(curr_d3d11);
	if (collectStats)
		sample.sendMs += msSince(sendStart);
	if (ok) {
		if (batched)
			toSignal.push_back(slot->sender);
		if (collectStats)
			sample.slotsSent++;
	}

	// ADVANCE the double buffer (send-CURRENT + index flip): the next frame renders into the OTHER
	// buffer, leaving the just-published buffer untouched for a full frame while its copy drains.
	std::swap(slot->curr, slot->prev);

	std::string actual;
	if (ok && slot->actualName.empty()) {
		// Read back the name Spout actually registered. The pre-resolve makes divergence
		// near-impossible, but RegisterSenderName can still auto-suffix on a lost race;
		// published names (M2.3 helper-config) must always be the ACTUAL name.
		const char *n = slot->sender->GetName();
		actual = n ? n : "";
	}

	bool diverged = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		slot->lastSendOk = ok;
		if (ok) {
			slot->sends++;
			if (!actual.empty())
				slot->actualName = actual;
			if (!slot->actualName.empty() && slot->actualName != slot->resolvedName &&
			    !slot->divergenceLogged) {
				slot->divergenceLogged = true;
				diverged = true;
			}
		}
	}
	if (diverged) {
		std::fprintf(stderr,
			     "[SpoutSenderEngine] WARNING: Spout registered '%s' (requested '%s') -- "
			     "adopting the actual name\n",
			     slot->actualName.c_str(), slot->resolvedName.c_str());
	}
	if (!ok) {
		std::fprintf(stderr, "[SpoutSenderEngine] SendTexture failed (slot %d, '%s')\n", slot->id,
			     slot->requestedName.c_str());
	}
}

} // namespace moxrelay
