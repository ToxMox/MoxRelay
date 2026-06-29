// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// PreviewConsumeEngine implementation. See the header for the bug/fix rationale. The render block
// mirrors SpoutSenderEngine::renderSlot (gs_texrender_reset/begin + gs_ortho +
// obs_source_video_render + gs_texrender_end) and MoxDisplayWidget::DrawPreview's per-frame weak->
// strong upgrade under the mutex. The texrender output is discarded -- the ONLY purpose of the
// render is to drive obs_source_update_async_video (the per-tick async upload).

#include "PreviewConsumeEngine.hpp"

#include <graphics/graphics.h>
#include <graphics/vec4.h>

#include <cstdio>

namespace moxrelay {

PreviewConsumeEngine::~PreviewConsumeEngine()
{
	stop();
}

bool PreviewConsumeEngine::start()
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (running_)
		return true;
	if (!obs_initialized()) {
		std::fprintf(stderr, "[PreviewConsumeEngine] start() before libobs is up -- refused\n");
		return false;
	}
	obs_add_main_render_callback(&PreviewConsumeEngine::renderTick, this);
	running_ = true;
	return true;
}

// Queued-task payload: destroy the throwaway texrender. Runs on the graphics thread (or inline if
// already there). The render callback was removed first, so no render iteration can be touching it.
void PreviewConsumeEngine::teardownGpuTask(void *param)
{
	auto *self = static_cast<PreviewConsumeEngine *>(param);
	if (self->texrender_) {
		gs_texrender_destroy(self->texrender_);
		self->texrender_ = nullptr;
	}
}

void PreviewConsumeEngine::stop()
{
	// 1. Remove the render callback FIRST. obs_remove_main_render_callback takes the same mutex the
	//    callbacks execute under, so once it returns no callback is mid-flight and none will fire
	//    again -- the synchronization point for everything below. Idempotent via running_.
	bool wasRunning = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		wasRunning = running_;
		running_ = false;
	}
	if (wasRunning && obs_initialized())
		obs_remove_main_render_callback(&PreviewConsumeEngine::renderTick, this);

	// 2. Tear the GPU object down on the graphics thread (blocking). Guarded on obs_initialized()
	//    so a stop() after libobs is gone degrades to a leak instead of a crash. Safe to read
	//    texrender_ unsynchronized here: the callback (its only other reader) is provably removed.
	if (texrender_ && obs_initialized())
		obs_queue_task(OBS_TASK_GRAPHICS, &PreviewConsumeEngine::teardownGpuTask, this, true);

	// 3. Release the weak source ref.
	std::lock_guard<std::mutex> lock(mutex_);
	if (weakSource_) {
		obs_weak_source_release(weakSource_);
		weakSource_ = nullptr;
	}
}

// R7: store a WEAK ref under the mutex (mirror MoxDisplayWidget::setSource). The old weak handle is
// released inside the critical section, so the render thread (which upgrades under the same mutex)
// can never observe a freed handle.
void PreviewConsumeEngine::setSource(obs_source_t *source)
{
	obs_weak_source_t *weak = source ? obs_source_get_weak_source(source) : nullptr;
	std::lock_guard<std::mutex> lock(mutex_);
	if (weakSource_)
		obs_weak_source_release(weakSource_);
	weakSource_ = weak;
}

// ===== GRAPHICS THREAD from here down. The gs context is already entered (the main render callback
// fires inside render_main_texture); never obs_enter_graphics here, never touch Qt. =====

void PreviewConsumeEngine::renderTick(void *param, uint32_t cx, uint32_t cy)
{
	(void)cx;
	(void)cy;
	static_cast<PreviewConsumeEngine *>(param)->renderTickImpl();
}

void PreviewConsumeEngine::renderTickImpl()
{
	// Upgrade the weak ref to a per-frame strong ref under the mutex (null if no source is set or
	// it was destroyed). Only the upgrade is inside the critical section; the strong ref is held
	// for the rest of this frame and released at the end (mirror MoxDisplayWidget::DrawPreview).
	obs_source_t *source = nullptr;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (weakSource_)
			source = obs_weak_source_get_source(weakSource_);
	}
	if (!source)
		return;

	// A non-producing / warming / audio-only source reports 0x0 -- nothing to upload, skip.
	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);
	if (width == 0 || height == 0) {
		obs_source_release(source);
		return;
	}

	// DEDUP: a source with an active Spout slot is already rendered UNGATED every tick by
	// SpoutSenderEngine. In MoxRelay obs_source_active() is true IFF a slot's inc_active is held
	// (the app's broadcast edge -- the factory raises only inc_showing, never inc_active), so an
	// active source's async texture is already kept fresh; rendering it again here is wasted work.
	if (obs_source_active(source)) {
		obs_source_release(source);
		return;
	}

	// Lazily create the throwaway offscreen target on the graphics thread (gs context entered).
	if (!texrender_)
		texrender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!texrender_) {
		obs_source_release(source);
		return;
	}

	// Render the source into the discarded target. obs_source_video_render dispatches through the
	// source's full filter chain and triggers obs_source_update_async_video (the per-tick async
	// upload) -- the whole point of this consume. The output pixels are never read.
	gs_texrender_reset(texrender_);
	if (gs_texrender_begin(texrender_, width, height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_texrender_end(texrender_);
	}

	obs_source_release(source);
}

} // namespace moxrelay
