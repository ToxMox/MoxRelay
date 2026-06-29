// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// PropertyPanel -- the descriptor-JSON-driven property form (contract PropertyDescriptor ->
// Qt widgets). The panel consumes ONLY the wire descriptor/settings JSON, through three injected
// seams (provider/applier/buttonInvoker -- the ControlVerbs gui* dispatch lambdas bound by
// main.cpp), so a GUI edit takes the EXACT write path a wire SetSourceProperties takes:
// key validation, the `applied` echo, and the propertyChanged event all happen in the verb
// layer. The same widget serves filter properties -- the seams carry the method choice.
//
// Apply model (per the descriptor type):
//   - slider DRAGS live-apply while the handle moves (throttled to one dispatch per ~50 ms
//     window with a trailing flush), so the output follows the gesture; the form rebuild is
//     deferred to the release, which reconciles the final value;
//   - TYPED edits never apply mid-typing: text/path/editable-list fields commit on
//     focus-leave/Tab/Enter (multiline on focus-leave; Enter is a newline there), exactly once
//     per actual change; spin boxes run with keyboard tracking off, so typed values land on
//     the same commit edges while arrow steps keep the ~400 ms batch window;
//   - discrete widgets (checkbox, combo pick, color, font, path browse) debounce to their final
//     value on the SAME ~400 ms batch window the typed/spin edits use (the value paints
//     optimistically, so the window is invisible).
// Disabled INPUT rows are hidden (dependent fields gated off by the current mode); disabled
// INFO text remains visible.
// After a successful apply the panel KEEPS the user's optimistic value and rebuilds ONLY when the
// apply CASCADED (a dependent control's items/visibility/value changed -- e.g. a device pick
// repopulating the resolution/fps lists), detected from the reply's post-apply descriptors; a
// non-cascading apply skips the rebuild entirely (no churn, no focus loss). A rejected apply (1006)
// flags the offending field inline and keeps the user's value on screen.
//
// THREADING: Qt main thread only (the seams hop nowhere -- ControlVerbs is main-thread).
// Rebuilds are always DEFERRED (queued, coalesced): a rebuild must never run inside one of the
// form's own widget signals (the sender would be destroyed mid-emission).

#pragma once

#include <nlohmann/json.hpp>

#include <QWidget>

#include <functional>
#include <map>
#include <string>

class QScrollArea;
class QTimer;

namespace moxrelay {

class PropertyPanel : public QWidget {
	Q_OBJECT

public:
	// Each seam returns the FULL reply envelope ({id, result|error}) of its wire method.
	using Provider = std::function<nlohmann::json()>;                         // ListXProperties
	using Applier = std::function<nlohmann::json(const nlohmann::json &)>;    // SetXProperties(settings)
	using ButtonInvoker = std::function<nlohmann::json(const std::string &)>; // InvokeSourceButton(prop)

	explicit PropertyPanel(QWidget *parent = nullptr);

	// Bind the panel to one source/filter (queued rebuild), or show the idle placeholder.
	void setTarget(Provider provider, Applier applier, ButtonInvoker buttonInvoker);
	void clearTarget();

	// Idle-placeholder wording (the same panel serves source and filter properties).
	void setPlaceholderText(const QString &text);

	// Coalesced deferred refresh -- safe to call from anywhere on the main thread, including
	// (indirectly) from inside this panel's own apply path. While an edit is still pending in
	// the debounce window the rebuild waits for that apply to flush first.
	void scheduleReload();

signals:
	// The hosted content (form or placeholder) was rebuilt -- its natural height may have
	// changed. Containers that size around the panel listen to this.
	void contentRebuilt();

public:
	// Natural (unscrolled) height of the current content; containers use it to grant the
	// panel enough room that scrollbars appear only on genuine excess.
	int naturalHeight() const;

private:
	bool typingInProgress() const;
	void reloadNow();
	void rebuildForm(const nlohmann::json &properties, const nlohmann::json &settings);
	void showPlaceholder(const QString &text);
	QWidget *buildEditor(const nlohmann::json &descriptor, const nlohmann::json &settings);

	void queueChange(const std::string &key, nlohmann::json value); // -> debounced batch apply
	void queueLiveChange(const std::string &key, nlohmann::json value); // -> throttled drag apply
	void applyNow(nlohmann::json settings);                         // immediate single apply
	void flushPending();
	void flushLive();   // mid-drag apply (rebuild suppressed)
	void endLiveDrag(); // release: final apply or deferred reconcile
	void markFieldError(const std::string &key, const QString &message);

	Provider provider_;
	Applier applier_;
	ButtonInvoker invoker_;

	QScrollArea *scroll_ = nullptr;
	QWidget *form_ = nullptr; // current form host (rebuilt wholesale)
	QTimer *debounce_ = nullptr;
	QTimer *liveTrailing_ = nullptr; // trailing drag-apply flush (single shot)
	qint64 liveAppliedMs_ = 0;       // last live dispatch time (throttle anchor)
	nlohmann::json pending_; // accumulated continuous-widget changes
	bool reloadQueued_ = false;
	nlohmann::json lastProperties_; // descriptors the current form was built from (#1 cascade test)
	nlohmann::json lastSettings_;   // values the current form was built from (#1 cascade test)
	bool suppressQueuedReload_ = false; // drop the self-echo loopback's redundant rebuild after an optimistic apply
	bool reloadDeferred_ = false; // a reload was parked behind an in-flight apply
	bool resetScrollOnRebuild_ = false; // new target: next build starts at the top, no focus grab
	bool applying_ = false;
	bool liveDragActive_ = false;  // a slider handle is held; rebuilds must wait
	bool suppressReload_ = false;  // mid-drag apply: skip the success-path rebuild
	QString placeholder_ = QStringLiteral("Select a source to edit its properties");
	std::map<std::string, QWidget *> fieldWidgets_; // wire key -> editor (errors/focus restore)
};

} // namespace moxrelay
