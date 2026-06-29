// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// Wheel guard for value widgets that live inside scrollable or dockable layouts: a mouse
// wheel over an UNFOCUSED slider/combo/spin box scrolls the surrounding view instead of
// silently mutating the value under the cursor. Focus stays click/tab-driven (the wheel
// never grants focus), so a deliberate edit -- click the control, then wheel -- behaves
// exactly as before.

#pragma once

#include <QEvent>
#include <QObject>
#include <QWidget>

namespace moxrelay {

class WheelGuard : public QObject {
public:
	using QObject::QObject;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::Wheel) {
			auto *w = qobject_cast<QWidget *>(watched);
			if (w && !w->hasFocus()) {
				event->ignore();
				return true; // not an edit; the parent view scrolls instead
			}
		}
		return QObject::eventFilter(watched, event);
	}
};

// Installs the guard. WheelFocus is downgraded to StrongFocus (the wheel must not grant
// focus, or the guard would defeat itself); stricter policies (e.g. ClickFocus) are kept.
inline void installWheelGuard(QWidget *w)
{
	if (!w)
		return;
	if (w->focusPolicy() == Qt::WheelFocus)
		w->setFocusPolicy(Qt::StrongFocus);
	w->installEventFilter(new WheelGuard(w));
}

} // namespace moxrelay
