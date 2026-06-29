// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// MoxDisplayWidget implementation -- ported from OBS Studio (frontend/widgets/OBSQTDisplay.cpp)
// (Windows-only) + the OBSProjector::OBSRender single-source draw body
// (frontend/widgets/OBSProjector.cpp:151-203) and the startRegion/endRegion +
// GetScaleAndCenterPos / GetPixelSize helpers (frontend/components/Multiview.hpp:56-68,
// frontend/utility/display-helpers.hpp:27-60), replicated inline against the plain libobs C ABI.
//
// THREADING: createDisplay / destroyDisplay / resize run on the Qt UI thread. DrawPreview runs on
// the libobs RENDER thread -- it touches ONLY libobs graphics + the borrowed source, never Qt.

#include "MoxDisplayWidget.hpp"

#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QWindow>

#include <graphics/graphics.h>

#ifdef _WIN32
#include <windows.h> // MSG, WM_DISPLAYCHANGE for nativeEvent
#endif

#include <algorithm>
#include <cstdio>

namespace moxrelay {

namespace {

// display-helpers.hpp:57 -- PIXEL size = logical size * devicePixelRatioF().
QSize pixelSize(const QWidget *w)
{
	return w->size() * w->devicePixelRatioF();
}

// display-helpers.hpp:27 -- aspect-correct letterbox of base CX/CY into a window CX/CY.
void getScaleAndCenterPos(int baseCX, int baseCY, int windowCX, int windowCY, int &x, int &y, float &scale)
{
	double windowAspect = double(windowCX) / double(windowCY);
	double baseAspect = double(baseCX) / double(baseCY);
	int newCX, newCY;

	if (windowAspect > baseAspect) {
		scale = float(windowCY) / float(baseCY);
		newCX = int(double(windowCY) * baseAspect);
		newCY = windowCY;
	} else {
		scale = float(windowCX) / float(baseCX);
		newCX = windowCX;
		newCY = int(float(windowCX) / baseAspect);
	}

	x = windowCX / 2 - newCX / 2;
	y = windowCY / 2 - newCY / 2;
}

// Multiview.hpp:56 / :64 -- push a letterboxed viewport + orthographic projection, then restore.
void startRegion(int vX, int vY, int vCX, int vCY, float oL, float oR, float oT, float oB)
{
	gs_projection_push();
	gs_viewport_push();
	gs_set_viewport(vX, vY, vCX, vCY);
	gs_ortho(oL, oR, oT, oB, -100.0f, 100.0f);
}

void endRegion()
{
	gs_viewport_pop();
	gs_projection_pop();
}

} // namespace

// SurfaceEventFilter port (frontend/utility/SurfaceEventFilter.hpp): on the backing QWindow's
// SurfaceAboutToBeDestroyed, tear the display down so libobs releases the swapchain BEFORE the
// HWND dies, and flip the `destroying_` guard so a stray paint/resize can't re-create it.
class MoxSurfaceEventFilter : public QObject {
public:
	explicit MoxSurfaceEventFilter(MoxDisplayWidget *w) : QObject(w), widget_(w) {}

protected:
	bool eventFilter(QObject *obj, QEvent *event) override
	{
		bool result = QObject::eventFilter(obj, event);
		if (event->type() == QEvent::PlatformSurface) {
			auto *se = static_cast<QPlatformSurfaceEvent *>(event);
			if (se->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
				widget_->destroying_ = true;
				widget_->destroyDisplay();
			}
		}
		return result;
	}

private:
	MoxDisplayWidget *widget_;
};

MoxDisplayWidget::MoxDisplayWidget(QWidget *parent) : QWidget(parent)
{
	// OBSQTDisplay.cpp:75-80 -- "I own this native surface" attributes (Windows-only set).
	setAttribute(Qt::WA_PaintOnScreen);
	setAttribute(Qt::WA_StaticContents);
	setAttribute(Qt::WA_NoSystemBackground);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setAttribute(Qt::WA_DontCreateNativeAncestors);
	setAttribute(Qt::WA_NativeWindow);

	// WA_NativeWindow guarantees a backing QWindow exists here (winId() already materialized it).
	if (QWindow *wh = windowHandle()) {
		// OBSQTDisplay.cpp:82-105 (Windows subset): create lazily once exposed; resize if already up.
		connect(wh, &QWindow::visibleChanged, this, [this](bool visible) {
			if (!visible)
				return;
			if (!display_) {
				createDisplay();
			} else {
				QSize s = pixelSize(this);
				obs_display_resize(display_, s.width(), s.height());
			}
		});
		// OBSQTDisplay.cpp:98-103 -- re-create on monitor change (swapchain follows the screen).
		connect(wh, &QWindow::screenChanged, this, [this](QScreen *) {
			createDisplay();
			if (display_) {
				QSize s = pixelSize(this);
				obs_display_resize(display_, s.width(), s.height());
			}
		});
		// OBSQTDisplay.cpp:108 -- teardown on SurfaceAboutToBeDestroyed.
		wh->installEventFilter(new MoxSurfaceEventFilter(this));
	}
}

MoxDisplayWidget::~MoxDisplayWidget()
{
	// dtor also tears down (OBSQTDisplay.hpp:29 nulls the RAII handle; we destroy explicitly).
	destroying_ = true;
	destroyDisplay();
	// destroyDisplay removed the draw callback, so no render-thread reader remains; drop our weak ref.
	setSource(nullptr);
}

// R7: store a WEAK ref under the mutex. The old weak handle is released inside the critical
// section, so the render thread (which upgrades under the same mutex) can never observe a freed
// handle. The strong ref the render thread holds per frame is independent of this swap.
void MoxDisplayWidget::setSource(obs_source_t *source)
{
	{
		obs_weak_source_t *weak = source ? obs_source_get_weak_source(source) : nullptr;
		std::lock_guard<std::mutex> lock(sourceMutex_);
		if (weakSource_)
			obs_weak_source_release(weakSource_);
		weakSource_ = weak;
	}
	// Mirror the change to the full-rate preview consume (outside sourceMutex_ -- never call
	// external code under our lock). Fires on every selection switch, the teardown clear, and the
	// dtor's setSource(nullptr), so PreviewConsumeEngine always tracks the previewed source.
	if (sourceChangedHook_)
		sourceChangedHook_(source);
}

void MoxDisplayWidget::setSourceChangedHook(std::function<void(obs_source_t *)> hook)
{
	sourceChangedHook_ = std::move(hook);
}

void MoxDisplayWidget::setBackgroundColor(uint32_t bgra)
{
	backgroundColor_ = bgra;
	if (display_)
		obs_display_set_background_color(display_, backgroundColor_); // OBSQTDisplay.cpp:128
}

// OBSQTDisplay.cpp:131-156 -- lazy, exposed-guarded create. HARD ORDERING: obs_reset_video() must
// already have succeeded (ObsBootstrap::startup) before this; obs_display_create enters the graphics
// context + creates the HWND swapchain.
void MoxDisplayWidget::createDisplay()
{
	if (display_ || destroying_)
		return;

	QWindow *wh = windowHandle();
	if (!wh || !wh->isExposed())
		return;

	QSize size = pixelSize(this); // PIXEL dims (logical * DPR)

	gs_init_data info = {};
	info.cx = static_cast<uint32_t>(size.width());
	info.cy = static_cast<uint32_t>(size.height());
	info.format = GS_BGRA;
	info.zsformat = GS_ZS_NONE;
	info.window.hwnd = reinterpret_cast<void *>(wh->winId()); // QTToGSWindow, Windows branch

	display_ = obs_display_create(&info, backgroundColor_);
	if (!display_)
		return;

	// OBSBasic.cpp:1139 pattern -- register the render-thread draw callback once the display exists.
	obs_display_add_draw_callback(display_, &MoxDisplayWidget::DrawPreview, this);

	emit displayCreated();
}

// remove draw callback BEFORE destroy (OBSBasic.cpp:1448), then destroy + null the handle.
void MoxDisplayWidget::destroyDisplay()
{
	if (!display_)
		return;

	obs_display_remove_draw_callback(display_, &MoxDisplayWidget::DrawPreview, this);
	obs_display_destroy(display_);
	display_ = nullptr;
}

void MoxDisplayWidget::paintEvent(QPaintEvent *event)
{
	createDisplay(); // OBSQTDisplay.cpp:158-163
	QWidget::paintEvent(event);
}

void MoxDisplayWidget::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	createDisplay(); // no-op if already made
	if (isVisible() && display_) {
		QSize s = pixelSize(this);
		obs_display_resize(display_, s.width(), s.height()); // OBSQTDisplay.cpp:187-199
	}
}

// WM_DISPLAYCHANGE -> update_color_space (OBSQTDisplay.cpp:172-185). SDR M1: harmless to forward;
// HDR/cross-monitor color tracking is otherwise stubbed.
bool MoxDisplayWidget::nativeEvent(const QByteArray &, void *message, qintptr *)
{
#ifdef _WIN32
	const MSG *msg = static_cast<MSG *>(message);
	if (msg && msg->message == WM_DISPLAYCHANGE && display_)
		obs_display_update_color_space(display_);
#else
	(void)message;
#endif
	return false;
}

// ===== RENDER THREAD ONLY. No Qt here. (OBSProjector::OBSRender, OBSProjector.cpp:151-203) =====
void MoxDisplayWidget::DrawPreview(void *data, uint32_t cx, uint32_t cy)
{
	auto *self = static_cast<MoxDisplayWidget *>(data);

	// R7: upgrade the weak ref to a per-frame strong ref under the mutex (null if the source was
	// destroyed or none is set). Only the upgrade is inside the critical section; the strong ref
	// is held for the rest of this frame and released at the end of the callback.
	obs_source_t *source = nullptr;
	{
		std::lock_guard<std::mutex> lock(self->sourceMutex_);
		if (self->weakSource_)
			source = obs_weak_source_get_source(self->weakSource_);
	}

	// A source is only DRAWABLE if it reports real dimensions. A non-producing or placeholder source
	// (e.g. an unregistered id, or capture not yet warmed up) reports 0x0; rendering it would feed
	// degenerate 1x1 letterbox math and a confusing blank. Treat 0-dim as background-only.
	uint32_t srcCX = source ? obs_source_get_width(source) : 0;
	uint32_t srcCY = source ? obs_source_get_height(source) : 0;
	const bool drawSource = source && srcCX > 0 && srcCY > 0;

	// ONE-SHOT (first-call, render-thread-only static guard): confirm frames are actually flowing.
	// Only libobs reads + a local static bool -- no Qt, thread-safe enough for a single debug line.
	static bool logged = false;
	if (!logged) {
		logged = true;
		if (drawSource)
			std::fprintf(stderr, "[MoxRelay] first preview frame: source %ux%u\n", srcCX, srcCY);
		else
			std::fprintf(stderr, "[MoxRelay] first preview frame: BACKGROUND-ONLY (source %s, %ux%u)\n",
				     source ? "present" : "null", srcCX, srcCY);
		std::fflush(stderr);
	}

	uint32_t targetCX, targetCY;
	if (drawSource) {
		targetCX = srcCX;
		targetCY = srcCY;
	} else {
		// Background-only path: letterbox against the canvas base size so the cleared region matches.
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		targetCX = std::max(ovi.base_width, 1u);
		targetCY = std::max(ovi.base_height, 1u);
	}

	int x, y;
	float scale;
	getScaleAndCenterPos(int(targetCX), int(targetCY), int(cx), int(cy), x, y, scale);

	int newCX = int(scale * float(targetCX));
	int newCY = int(scale * float(targetCY));

	startRegion(x, y, newCX, newCY, 0.0f, float(targetCX), 0.0f, float(targetCY));

	if (drawSource)
		obs_source_video_render(source);
	// else: nothing drawn inside the region -> the display's background color shows through, which
	// still proves obs_display_create + the draw callback fire end-to-end.

	endRegion();

	// R7: drop the per-frame strong ref (paired with the upgrade at the top of this callback).
	if (source)
		obs_source_release(source);
}

} // namespace moxrelay
