// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// Application style tweak: a left-click on a slider groove jumps the handle to the clicked
// position (and starts a drag from there) instead of paging the value by a step. Every
// slider in the app applies its value live while the handle moves, so the pointer position
// IS the intended value -- paging toward it is an extra, slower gesture for the same intent.

#pragma once

#include <QProxyStyle>

namespace moxrelay {

class SliderJumpStyle : public QProxyStyle {
public:
	using QProxyStyle::QProxyStyle;

	int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
		      QStyleHintReturn *returnData = nullptr) const override
	{
		if (hint == SH_Slider_AbsoluteSetButtons)
			return Qt::LeftButton;
		return QProxyStyle::styleHint(hint, option, widget, returnData);
	}
};

} // namespace moxrelay
