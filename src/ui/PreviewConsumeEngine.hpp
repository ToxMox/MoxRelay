// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// PreviewConsumeEngine -- keeps the CURRENTLY PREVIEWED source's async texture fresh at the FULL
// graphics-tick rate, independent of the present (vsync) gate.
//
// THE BUG IT FIXES: a preview-only webcam is GPU-consumed only inside MoxDisplayWidget's
// present-ready-GATED display draw (obs_source_video_render in DrawPreview), which fires only at the
// monitor refresh (~60/s) even when the canvas ticks at 120. libobs async frames live exactly one
// tick (async_tick frees + reselects cur_async_frame every tick; the GPU upload happens only when
// the source is RENDERED), so at canvas fps > refresh, frames selected on non-present ticks are
// freed before upload = DROPPED -> laggy preview. Broadcasting masks this because SpoutSenderEngine
// renders the source UNGATED every tick via its own obs_add_main_render_callback.
//
// THE FIX: this engine owns ONE obs_add_main_render_callback that renders the previewed source into
// a throwaway offscreen target every graphics tick. obs_source_video_render is what triggers
// obs_source_update_async_video (the per-tick async upload) -- so the async texture is refreshed
// every tick and the present-gated display draw always finds a fresh frame. The present stays
// vsync-gated and unchanged. Crucially this uses obs_source_video_render ONLY (never
// obs_source_activate), so previewing a source never makes it "live" (no media autoplay).
//
// SHAPE: deliberately mirrors SpoutSenderEngine -- start()/stop() add/remove the single render
// callback; the GPU object (one reusable texrender) is created and destroyed ONLY on the graphics
// thread (teardown marshalled via obs_queue_task(OBS_TASK_GRAPHICS, wait)); the previewed-source
// pointer is a mutex-guarded weak ref (the same R7 swap MoxDisplayWidget uses), upgraded per frame
// on the graphics thread. setSource is driven through MoxDisplayWidget::setSource so it follows
// selection switches, the teardown clear, and the dtor automatically.
//
// LIFECYCLE CONTRACT: stop() (or the destructor) MUST run while libobs is still up -- i.e. before
// ObsBootstrap::shutdown(). start()/stop()/setSource are idempotent / safe to call repeatedly.

#pragma once

#include <obs.h>

#include <mutex>

namespace moxrelay {

class PreviewConsumeEngine {
public:
	PreviewConsumeEngine() = default;
	~PreviewConsumeEngine(); // calls stop() -- see the lifecycle contract above

	PreviewConsumeEngine(const PreviewConsumeEngine &) = delete;
	PreviewConsumeEngine &operator=(const PreviewConsumeEngine &) = delete;

	// Register the engine's single main render callback. Idempotent; a no-op with no source set.
	bool start();

	// Remove the render callback, then tear the texrender down on the graphics thread, then
	// release the weak source ref. Idempotent; safe to call twice.
	void stop();

	// Set the source kept fresh by the per-tick consume. Pass nullptr to consume nothing (empty
	// selection / teardown). R7: stores a WEAK ref swapped under the mutex; the render thread
	// upgrades it per frame, so the source may be switched -- or destroyed -- at any time without
	// racing the callback.
	void setSource(obs_source_t *source);

private:
	static void renderTick(void *param, uint32_t cx, uint32_t cy); // graphics thread
	void renderTickImpl();                                         // graphics thread, gs context entered
	static void teardownGpuTask(void *param);                     // graphics thread (queued task)

	std::mutex mutex_; // guards running_ + weakSource_
	bool running_ = false;
	obs_weak_source_t *weakSource_ = nullptr;
	// Graphics-thread-only after creation: lazily created on the first tick, destroyed in stop()
	// via the queued graphics task (the callback is provably removed first, so no render-thread
	// reader remains when the teardown runs).
	gs_texrender_t *texrender_ = nullptr;
};

} // namespace moxrelay
