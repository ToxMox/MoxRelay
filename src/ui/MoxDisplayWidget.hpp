// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// MoxDisplayWidget -- a standalone Qt6 port of obs-studio's OBSQTDisplay (Windows-only).
// A plain QWidget (NOT QOpenGLWidget) that hands its native HWND to libobs; libobs renders
// directly into a swapchain bound to that HWND. The widget owns the obs_display_t lifecycle on
// the Qt UI thread; the registered draw callback (DrawPreview) runs on the libobs RENDER thread
// and must NEVER touch Qt. Ported from OBS Studio (frontend/widgets/OBSQTDisplay.{hpp,cpp})
// + the OBSProjector::OBSRender single-source draw pattern; mac/X11/Wayland branches dropped.

#pragma once

#include <QWidget>

#include <obs.h>

#include <cstdint>
#include <functional>
#include <mutex>

namespace moxrelay {

class MoxDisplayWidget : public QWidget {
	Q_OBJECT

public:
	explicit MoxDisplayWidget(QWidget *parent = nullptr);
	~MoxDisplayWidget() override;

	// Qt owns nothing about the surface; libobs draws into our HWND swapchain.
	QPaintEngine *paintEngine() const override { return nullptr; }

	obs_display_t *display() const { return display_; }

	// Set the source rendered by the draw callback. Pass nullptr to render the background only
	// (still proves obs_display_create + the callback fire). R7: holds a WEAK ref internally
	// (swapped atomically); the render thread upgrades it per frame, so the source may be
	// switched -- or even destroyed -- at any time without racing the draw callback.
	void setSource(obs_source_t *source);

	// Optional hook invoked by setSource AFTER the weak-ref swap, with the SAME source pointer
	// (nullptr included). Set once in run_gui so PreviewConsumeEngine follows the previewed source
	// through this single chokepoint -- every selection switch, the teardown clear, and the dtor's
	// setSource(nullptr) propagate automatically. The hook target must outlive this widget.
	void setSourceChangedHook(std::function<void(obs_source_t *)> hook);

	// Background color (BGRA, e.g. 0xFF1E1E1E). Applied on create + immediately if created.
	void setBackgroundColor(uint32_t bgra);

signals:
	void displayCreated();

protected:
	void paintEvent(QPaintEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

private:
	friend class MoxSurfaceEventFilter;

	// UI-thread lifecycle. CreateDisplay is lazy + guarded on windowHandle()+isExposed().
	void createDisplay();
	void destroyDisplay(); // remove draw callback + obs_display_destroy + null the handle

	// Runs on the libobs RENDER thread. Letterboxes the source (or background-only) into cx/cy.
	static void DrawPreview(void *data, uint32_t cx, uint32_t cy);

	obs_display_t *display_ = nullptr;
	// R7: weak ref to the previewed source. sourceMutex_ makes the render thread's load+upgrade
	// atomic against the UI thread's swap+release (a lone atomic exchange would still let the UI
	// thread free the weak handle between the render thread's load and its upgrade). The render
	// thread holds the upgraded STRONG ref only for the duration of one frame. Replaces the
	// previous unsynchronized borrowed pointer (UI-thread write racing render-thread read).
	std::mutex sourceMutex_;
	obs_weak_source_t *weakSource_ = nullptr;
	// UI-thread-only: invoked by setSource (outside sourceMutex_) so the full-rate preview consume
	// follows the previewed source. Empty until run_gui wires it.
	std::function<void(obs_source_t *)> sourceChangedHook_;
	uint32_t backgroundColor_ = 0xFF1E1E1E;
	bool destroying_ = false; // set by the surface filter so createDisplay can't re-fire mid-teardown
};

} // namespace moxrelay
