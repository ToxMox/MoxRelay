// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// PropertyPanel implementation. Widget mapping per the contract PropertyDescriptor types:
//   bool  -> QCheckBox                       text  -> QLineEdit / password / QPlainTextEdit / info label
//   int   -> QSpinBox (+slider)              path  -> QLineEdit + Browse (file dialog by pathMode)
//   float -> QDoubleSpinBox (+scaled slider) list  -> QComboBox (items[]; value typed by listFormat)
//   color -> swatch button -> QColorDialog   font  -> preview button -> QFontDialog (FontValue)
//   button-> QPushButton -> InvokeSourceButton seam
// Engine color ints pack ABGR (alpha << 24 | blue << 16 | green << 8 | red); FontValue carries
// {face, style, size, flags: 1 bold | 2 italic | 4 underline | 8 strikeout}.

#include "PropertyPanel.hpp"

#include "WheelGuard.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCursor>
#include <QDateTime>
#include <QEvent>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFontDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace moxrelay {

namespace {

using nlohmann::json;

constexpr int kDebounceMs = 400; // typed-edit batch window (spin boxes, text edits)
// Slider drags live-apply through a throttle (one dispatch per window, trailing flush) so the
// output follows the handle while it moves; the form rebuild is deferred until release.
constexpr int kLiveApplyMs = 50;

QColor colorFromEngine(int64_t value)
{
	const auto v = static_cast<uint32_t>(value);
	return QColor(int(v & 0xFF), int((v >> 8) & 0xFF), int((v >> 16) & 0xFF), int((v >> 24) & 0xFF));
}

int64_t engineFromColor(const QColor &c)
{
	return (int64_t(c.alpha()) << 24) | (int64_t(c.blue()) << 16) | (int64_t(c.green()) << 8) |
	       int64_t(c.red());
}

QString fontPreviewText(const json &fontValue)
{
	const QString face = QString::fromStdString(fontValue.value("face", std::string("(default)")));
	const int size = fontValue.value("size", 0);
	return size > 0 ? QStringLiteral("%1, %2pt").arg(face).arg(size) : face;
}

// "*.png *.jpg" (descriptor pathFilter) -> a Qt file-dialog filter string.
QString dialogFilter(const json &descriptor)
{
	const std::string raw = descriptor.value("pathFilter", std::string());
	if (raw.empty())
		return QStringLiteral("All files (*)");
	return QStringLiteral("Files (%1);;All files (*)").arg(QString::fromStdString(raw));
}

void setFieldError(QWidget *w, bool on, const QString &message)
{
	if (!w)
		return;
	w->setProperty("fieldError", on);
	w->setToolTip(on ? message : QString());
	if (QStyle *s = w->style()) {
		s->unpolish(w);
		s->polish(w);
	}
}

// Runs a commit callback when the watched editor loses focus (multiline text has no
// editingFinished signal of its own).
class FocusOutCommit : public QObject {
public:
	FocusOutCommit(QObject *parent, std::function<void()> commit)
		: QObject(parent), commit_(std::move(commit))
	{
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() == QEvent::FocusOut)
			commit_();
		return QObject::eventFilter(watched, event);
	}

private:
	std::function<void()> commit_;
};

// Typed edits commit on focus-leave, but Qt only moves focus when the click target is
// focusable -- a click on the preview, dock chrome, or empty panel space moves nothing and
// would leave the edit silently uncommitted. This application-level filter clears the
// editor's focus on any press outside its commit container, which runs the normal
// FocusOut/editingFinished commit path. It never consumes the click.
class ClickAwayDefocus : public QObject {
public:
	explicit ClickAwayDefocus(QObject *parent) : QObject(parent) {}

protected:
	bool eventFilter(QObject *, QEvent *event) override
	{
		if (event->type() != QEvent::MouseButtonPress)
			return false;
		if (QApplication::activePopupWidget())
			return false; // a combo popup owns this interaction
		QWidget *focus = QApplication::focusWidget();
		if (!focus)
			return false;
		// A NON-editable combo holds focus on the QComboBox itself (no embedded line edit),
		// so it must be matched directly here -- this was the original gap that left the fps /
		// format dropdowns focused (and live to the wheel/arrows) after a click-away.
		if (!qobject_cast<QLineEdit *>(focus) && !qobject_cast<QPlainTextEdit *>(focus) &&
		    !qobject_cast<QAbstractSpinBox *>(focus) && !qobject_cast<QComboBox *>(focus))
			return false;
		// A spin box's or editable combo's embedded line edit defocuses with its parent
		// control, so presses on the control's own arrows/buttons stay internal. (A
		// non-editable combo is already its own root; the walk just leaves it unchanged.)
		QWidget *root = focus;
		for (QWidget *p = focus->parentWidget(); p; p = p->parentWidget()) {
			if (qobject_cast<QComboBox *>(p) || qobject_cast<QAbstractSpinBox *>(p))
				root = p;
		}
		QWidget *target = QApplication::widgetAt(QCursor::pos());
		if (target && (target == root || root->isAncestorOf(target)))
			return false;
		focus->clearFocus();
		return false;
	}
};

} // namespace

PropertyPanel::PropertyPanel(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	scroll_ = new QScrollArea(this);
	scroll_->setWidgetResizable(true);
	scroll_->setFrameShape(QFrame::NoFrame);
	layout->addWidget(scroll_);

	debounce_ = new QTimer(this);
	debounce_->setSingleShot(true);
	debounce_->setInterval(kDebounceMs);
	connect(debounce_, &QTimer::timeout, this, &PropertyPanel::flushPending);

	liveTrailing_ = new QTimer(this);
	liveTrailing_->setSingleShot(true);
	liveTrailing_->setInterval(kLiveApplyMs);
	connect(liveTrailing_, &QTimer::timeout, this, &PropertyPanel::flushLive);

	pending_ = json::object();
	showPlaceholder(placeholder_);

	// One per application (both property panes construct one of these).
	static bool clickAwayInstalled = false;
	if (!clickAwayInstalled) {
		clickAwayInstalled = true;
		qApp->installEventFilter(new ClickAwayDefocus(qApp));
	}
}

void PropertyPanel::setPlaceholderText(const QString &text)
{
	placeholder_ = text;
	if (!provider_)
		showPlaceholder(placeholder_);
}

void PropertyPanel::setTarget(Provider provider, Applier applier, ButtonInvoker buttonInvoker)
{
	// #2 explicit commit: a discrete/typed edit still in the debounce window belongs to the CURRENT
	// target -- commit it through the still-bound applier_ before rebinding (the value already painted
	// optimistically; dropping it would silently lose the user's change). The old target's own loopback
	// refresh is for a now-deselected source, so it self-skips.
	if (applier_ && !pending_.empty())
		flushPending();
	debounce_->stop();
	liveTrailing_->stop();
	liveDragActive_ = false;
	pending_ = json::object();
	reloadDeferred_ = false; // the rebind reloads unconditionally below
	resetScrollOnRebuild_ = true; // a DIFFERENT target starts at the top of its form
	provider_ = std::move(provider);
	applier_ = std::move(applier);
	invoker_ = std::move(buttonInvoker);
	scheduleReload();
}

void PropertyPanel::clearTarget()
{
	// #2 explicit commit: flush a pending edit to the still-bound target before clearing (a deselect
	// must not drop the user's last change). If the target was REMOVED the apply returns a harmless
	// not-found error (no rebuild).
	if (applier_ && !pending_.empty())
		flushPending();
	debounce_->stop();
	liveTrailing_->stop();
	liveDragActive_ = false;
	pending_ = json::object();
	reloadDeferred_ = false;
	provider_ = nullptr;
	applier_ = nullptr;
	invoker_ = nullptr;
	fieldWidgets_.clear();
	showPlaceholder(placeholder_);
}

void PropertyPanel::scheduleReload()
{
	suppressQueuedReload_ = false; // an explicit reload request overrides a pending self-apply skip
	if (reloadQueued_)
		return;
	reloadQueued_ = true;
	// Deferred: never rebuild inside a form widget's own signal emission, and let a pending
	// debounced apply flush first (flushPending -> applyNow -> success path re-schedules).
	QTimer::singleShot(0, this, [this] {
		reloadQueued_ = false;
		if (suppressQueuedReload_) {
			// A self-apply already reconciled optimistically; this queued reload was the self-echo
			// loopback's redundant rebuild -- drop it so the user's value stays.
			suppressQueuedReload_ = false;
			return;
		}
		if (applying_ || debounce_->isActive() || liveDragActive_ || typingInProgress()) {
			// An apply is in flight (or queued behind the debounce), a slider drag is
			// in progress, or the user is mid-edit in a text field -- a rebuild now
			// would destroy the widget mid-gesture (or discard half-typed input).
			// Defer: the apply path (or the gesture's end / the edit's commit)
			// re-schedules once it settles, SUCCESS OR FAILURE (a 1006 must not
			// swallow an external propertyChanged refresh).
			reloadDeferred_ = true;
			return;
		}
		reloadNow();
	});
}

bool PropertyPanel::typingInProgress() const
{
	// True while the focused widget is one of THIS form's text editors holding an uncommitted
	// edit (typed edits commit on focus-leave/enter, never live -- a rebuild mid-typing would
	// discard the user's input). Spin boxes surface here through their internal line edit.
	QWidget *focus = QApplication::focusWidget();
	if (!focus || !form_ || !form_->isAncestorOf(focus))
		return false;
	if (auto *line = qobject_cast<QLineEdit *>(focus))
		return line->isModified();
	if (auto *plain = qobject_cast<QPlainTextEdit *>(focus))
		return plain->document()->isModified();
	return false;
}

void PropertyPanel::reloadNow()
{
	if (!provider_) {
		showPlaceholder(placeholder_);
		return;
	}
	const json reply = provider_();
	if (!reply.is_object() || !reply.contains("result")) {
		const std::string message = reply.is_object() && reply.contains("error")
						    ? reply["error"].value("message", std::string("error"))
						    : std::string("no reply");
		showPlaceholder(QStringLiteral("Properties unavailable: %1")
					.arg(QString::fromStdString(message)));
		return;
	}
	rebuildForm(reply["result"].value("properties", json::array()),
		    reply["result"].value("settings", json::object()));
}

void PropertyPanel::showPlaceholder(const QString &text)
{
	fieldWidgets_.clear();
	auto *host = new QWidget(scroll_);
	auto *layout = new QVBoxLayout(host);
	// The label's own theme padding supplies the breathing room; heavy host margins here
	// push the minimum height past a short pane and produce a pointless scrollbar.
	layout->setContentsMargins(8, 8, 8, 8);
	auto *label = new QLabel(text, host);
	label->setObjectName(QStringLiteral("PlaceholderLabel"));
	label->setWordWrap(true);
	layout->addWidget(label);
	layout->addStretch(1);
	delete scroll_->takeWidget();
	scroll_->setWidget(host);
	scroll_->verticalScrollBar()->setSingleStep(28); // ~one row per wheel line, anywhere over the pane
	form_ = host;
	resetScrollOnRebuild_ = false;
	emit contentRebuilt();
}

void PropertyPanel::rebuildForm(const json &properties, const json &settings)
{
	// #1: cache the descriptors/values this form is built from so a later apply can tell a cascading
	// edit (dependent lists changed -> rebuild) from a no-op reconcile (skip).
	lastProperties_ = properties;
	lastSettings_ = settings;

	// Preserve the viewport + the focused field across the wholesale rebuild.
	const int scrollPos = scroll_->verticalScrollBar()->value();
	std::string focusedKey;
	for (const auto &[key, widget] : fieldWidgets_) {
		if (widget && (widget->hasFocus() || widget->isAncestorOf(focusWidget()))) {
			focusedKey = key;
			break;
		}
	}

	fieldWidgets_.clear();
	auto *host = new QWidget(scroll_);
	auto *form = new QFormLayout(host);
	form->setContentsMargins(8, 8, 8, 8);
	form->setHorizontalSpacing(8);
	form->setVerticalSpacing(8);
	form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
	form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

	for (const auto &descriptor : properties) {
		if (!descriptor.is_object())
			continue;
		if (!descriptor.value("visible", true))
			continue; // contract: invisible descriptors are not shown
		const std::string type = descriptor.value("type", std::string());
		const std::string name = descriptor.value("name", std::string());
		const QString label = QString::fromStdString(descriptor.value("label", name));

		// A disabled INPUT row is dead weight: the engine disables dependent fields whose
		// governing mode makes them inapplicable (a camera's custom resolution/fps fields
		// while the resolution type is device-default), so hide them outright. Disabled
		// INFO text stays visible -- it is a message, not an input.
		const bool isInfoText = type == "text" &&
					descriptor.value("textMode", std::string("default")) == "info";
		if (!descriptor.value("enabled", true) && !isInfoText)
			continue;

		QWidget *editor = buildEditor(descriptor, settings);
		if (!editor)
			continue;
		editor->setEnabled(descriptor.value("enabled", true));
		fieldWidgets_[name] = editor;

		// bool/button rows carry their own caption, and info text IS its own message: all
		// three span the form (a labeled info row would print the message twice, and the
		// unwrapped label column forces the form wider than the pane).
		if (type == "bool" || type == "button" || isInfoText)
			form->addRow(editor);
		else
			form->addRow(label, editor);
	}
	if (fieldWidgets_.empty()) {
		auto *none = new QLabel(QStringLiteral("This source has no editable properties"), host);
		none->setObjectName(QStringLiteral("PlaceholderLabel"));
		none->setWordWrap(true);
		form->addRow(none);
	}

	delete scroll_->takeWidget();
	scroll_->setWidget(host);
	scroll_->verticalScrollBar()->setSingleStep(28); // ~one row per wheel line, anywhere over the pane
	form_ = host;

	if (resetScrollOnRebuild_) {
		// First build for a NEW target: start at the top and never steal focus into the
		// form (position/focus restore is for same-target refreshes only).
		resetScrollOnRebuild_ = false;
		scroll_->verticalScrollBar()->setValue(0);
	} else {
		scroll_->verticalScrollBar()->setValue(scrollPos);
		if (!focusedKey.empty()) {
			const auto it = fieldWidgets_.find(focusedKey);
			if (it != fieldWidgets_.end() && it->second)
				it->second->setFocus(Qt::OtherFocusReason);
		}
	}
	emit contentRebuilt();
}

int PropertyPanel::naturalHeight() const
{
	return form_ ? form_->sizeHint().height() : 0;
}

QWidget *PropertyPanel::buildEditor(const json &descriptor, const json &settings)
{
	const std::string type = descriptor.value("type", std::string());
	const std::string name = descriptor.value("name", std::string());
	const QString label = QString::fromStdString(descriptor.value("label", name));
	const json current = settings.contains(name) ? settings[name] : json();

	if (type == "bool") {
		auto *box = new QCheckBox(label);
		box->setChecked(current.is_boolean() && current.get<bool>());
		connect(box, &QCheckBox::toggled, this,
			[this, name](bool on) { queueChange(name, on); });
		return box;
	}

	if (type == "int") {
		const int min = int(descriptor.value("min", -1000000000.0));
		const int max = int(descriptor.value("max", 1000000000.0));
		const int step = std::max(1, int(descriptor.value("step", 1.0)));
		const int value = current.is_number() ? current.get<int>() : 0;
		auto *spin = new QSpinBox();
		spin->setRange(min, max);
		spin->setSingleStep(step);
		spin->setValue(value);
		// Typed digits must not dispatch per keystroke: with tracking off, valueChanged
		// fires only on Enter/Tab/focus-leave (and on arrow steps, which stay batched).
		spin->setKeyboardTracking(false);
		connect(spin, &QAbstractSpinBox::editingFinished, this, &PropertyPanel::flushPending);
		if (descriptor.value("display", std::string()) == "slider") {
			auto *row = new QWidget();
			auto *layout = new QHBoxLayout(row);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(8);
			auto *slider = new QSlider(Qt::Horizontal);
			slider->setRange(min, max);
			slider->setSingleStep(step);
			slider->setValue(value);
			layout->addWidget(slider, /*stretch=*/1);
			layout->addWidget(spin);
			installWheelGuard(slider);
			installWheelGuard(spin);
			connect(slider, &QSlider::valueChanged, this, [this, name, spin, slider](int v) {
				QSignalBlocker block(spin);
				spin->setValue(v);
				if (slider->isSliderDown())
					queueLiveChange(name, v);
				else
					queueChange(name, v);
			});
			connect(slider, &QSlider::sliderPressed, this,
				[this] { liveDragActive_ = true; });
			connect(slider, &QSlider::sliderReleased, this, &PropertyPanel::endLiveDrag);
			connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
				[this, name, slider](int v) {
					QSignalBlocker block(slider);
					slider->setValue(v);
					queueChange(name, v);
				});
			return row;
		}
		connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
			[this, name](int v) { queueChange(name, v); });
		installWheelGuard(spin);
		return spin;
	}

	if (type == "float") {
		const double min = descriptor.value("min", -1.0e9);
		const double max = descriptor.value("max", 1.0e9);
		const double step = descriptor.value("step", 0.01) > 0 ? descriptor.value("step", 0.01) : 0.01;
		const double value = current.is_number() ? current.get<double>() : 0.0;
		const int decimals = step >= 1.0 ? 1 : (step >= 0.1 ? 2 : (step >= 0.01 ? 3 : 4));
		auto *spin = new QDoubleSpinBox();
		spin->setRange(min, max);
		spin->setSingleStep(step);
		spin->setDecimals(decimals);
		spin->setValue(value);
		spin->setKeyboardTracking(false); // same commit edges as the int spin above
		connect(spin, &QAbstractSpinBox::editingFinished, this, &PropertyPanel::flushPending);
		if (descriptor.value("display", std::string()) == "slider") {
			auto *row = new QWidget();
			auto *layout = new QHBoxLayout(row);
			layout->setContentsMargins(0, 0, 0, 0);
			layout->setSpacing(8);
			auto *slider = new QSlider(Qt::Horizontal);
			const int ticks = std::max(1, int(std::lround((max - min) / step)));
			slider->setRange(0, ticks);
			slider->setValue(int(std::lround((value - min) / step)));
			layout->addWidget(slider, /*stretch=*/1);
			layout->addWidget(spin);
			installWheelGuard(slider);
			installWheelGuard(spin);
			connect(slider, &QSlider::valueChanged, this,
				[this, name, spin, slider, min, step](int t) {
					const double v = min + double(t) * step;
					QSignalBlocker block(spin);
					spin->setValue(v);
					if (slider->isSliderDown())
						queueLiveChange(name, v);
					else
						queueChange(name, v);
				});
			connect(slider, &QSlider::sliderPressed, this,
				[this] { liveDragActive_ = true; });
			connect(slider, &QSlider::sliderReleased, this, &PropertyPanel::endLiveDrag);
			connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
				[this, name, slider, min, step](double v) {
					QSignalBlocker block(slider);
					slider->setValue(int(std::lround((v - min) / step)));
					queueChange(name, v);
				});
			return row;
		}
		connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
			[this, name](double v) { queueChange(name, v); });
		installWheelGuard(spin);
		return spin;
	}

	if (type == "text") {
		const std::string mode = descriptor.value("textMode", std::string("default"));
		const QString value = QString::fromStdString(
			current.is_string() ? current.get<std::string>() : std::string());
		if (mode == "info") {
			auto *info = new QLabel(value.isEmpty() ? label : value);
			info->setWordWrap(true);
			return info;
		}
		if (mode == "multiline") {
			auto *edit = new QPlainTextEdit();
			edit->setPlainText(value);
			edit->setFixedHeight(88); // ~4 lines on the 8px grid
			edit->document()->setModified(false);
			// Commit on focus-leave only -- Enter is a newline in multiline text.
			edit->installEventFilter(new FocusOutCommit(edit, [this, name, edit] {
				if (!edit->document()->isModified())
					return;
				edit->document()->setModified(false);
				applyNow(json{{name, edit->toPlainText().toStdString()}});
			}));
			return edit;
		}
		auto *edit = new QLineEdit(value);
		if (mode == "password")
			edit->setEchoMode(QLineEdit::Password);
		connect(edit, &QLineEdit::editingFinished, this, [this, name, edit] {
			// Fires on Enter AND again on the focus-leave that follows -- isModified()
			// gates the dispatch to exactly once per actual change.
			if (!edit->isModified())
				return;
			edit->setModified(false);
			applyNow(json{{name, edit->text().toStdString()}});
		});
		return edit;
	}

	if (type == "path") {
		auto *row = new QWidget();
		auto *layout = new QHBoxLayout(row);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(8);
		auto *edit = new QLineEdit(QString::fromStdString(
			current.is_string() ? current.get<std::string>() : std::string()));
		auto *browse = new QPushButton(QStringLiteral("Browse..."));
		layout->addWidget(edit, /*stretch=*/1);
		layout->addWidget(browse);
		connect(edit, &QLineEdit::editingFinished, this, [this, name, edit] {
			if (!edit->isModified())
				return; // once per actual change (Enter then focus-leave double-fire)
			edit->setModified(false);
			applyNow(json{{name, edit->text().toStdString()}});
		});
		const bool isDirectory = descriptor.value("pathMode", std::string("file")) == "directory";
		const QString filter = dialogFilter(descriptor);
		connect(browse, &QPushButton::clicked, this, [this, name, edit, isDirectory, filter, label] {
			const QString picked =
				isDirectory ? QFileDialog::getExistingDirectory(this, label, edit->text())
					    : QFileDialog::getOpenFileName(this, label, edit->text(), filter);
			if (picked.isEmpty())
				return;
			edit->setText(picked);
			queueChange(name, picked.toStdString());
		});
		return row;
	}

	if (type == "list") {
		auto *combo = new QComboBox();
		const std::string format = descriptor.value("listFormat", std::string("string"));
		// The PRIMARY target list (camera device / display / window) on an unconfigured source: it
		// must start BLANK on a type-aware "Select a <type>..." placeholder and commit NOTHING until
		// the user picks, so the source stays inert. Marked by TypeVocabulary::describeProperties.
		const bool isTarget = descriptor.value("isTarget", false);
		const json items = descriptor.value("items", json::array());
		int currentIndex = -1;
		for (const auto &item : items) {
			if (!item.is_object())
				continue;
			combo->addItem(QString::fromStdString(item.value("label", std::string())));
			const int idx = combo->count() - 1;
			const json value = item.contains("value") ? item["value"] : json();
			combo->setItemData(idx, QString::fromStdString(value.dump()));
			if (item.value("disabled", false)) {
				if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
					if (auto *mi = model->item(idx))
						mi->setFlags(mi->flags() & ~Qt::ItemIsEnabled);
				}
			}
			if (value == current)
				currentIndex = idx;
		}
		combo->setEditable(descriptor.value("listEditable", false));
		if (combo->isEditable()) {
			// Free-typed values go to the ENGINE, never into the local item list.
			combo->setInsertPolicy(QComboBox::NoInsert);
		}
		if (currentIndex >= 0)
			combo->setCurrentIndex(currentIndex);
		else if (combo->isEditable() && current.is_string())
			combo->setEditText(QString::fromStdString(current.get<std::string>()));
		else if (isTarget) {
			// Inert TARGET list with nothing committed (a freshly-added camera/display/window):
			// show a DISABLED, type-aware "Select a <type>..." placeholder at index 0 and commit
			// NOTHING. The source stays inert until the user picks a real entry (index >= 1), which
			// fires `activated` -> applyNow -> the helper raises the showing ref (SetSourceProperties).
			auto *model = qobject_cast<QStandardItemModel *>(combo->model());
			bool firstIsDisabled = false;
			if (model && combo->count() > 0)
				if (auto *mi0 = model->item(0))
					firstIsDisabled = !(mi0->flags() & Qt::ItemIsEnabled);
			const QString prompt = QString::fromStdString(
				descriptor.value("placeholder", std::string("Select...")));
			if (firstIsDisabled) {
				combo->setItemText(0, prompt); // retag the engine's placeholder with our wording
			} else {
				combo->insertItem(0, prompt); // synthesize one; real items shift to index >= 1
				if (model)
					if (auto *mi = model->item(0))
						mi->setFlags(mi->flags() & ~Qt::ItemIsEnabled);
			}
			combo->setCurrentIndex(0); // show the placeholder; nothing is committed
		}
		// Non-editable list whose stored value matched no item (the source key is empty/unset, or
		// names a device the enumerator no longer lists): a non-editable combo can only DISPLAY one
		// of its items, so it shows the first one -- but nothing was ever committed, leaving "what
		// is shown" != "what is set". A user who never re-opens the popup keeps that uncommitted
		// default (e.g. a freshly-added camera's empty video_device_id, which win-dshow rejects with
		// "DecodeDeviceId failed"; whether the wanted device sits at index 0 is machine-dependent).
		// Mirror OBS's "default to the first list item" by committing the shown selection now, so a
		// pick is never required to make the displayed item real. Deferred to the next event-loop
		// tick so the dispatch never runs mid-rebuild. Editable combos are untouched: their
		// empty/free-typed state is itself a valid value. TARGET lists (isTarget) are also SKIPPED:
		// auto-committing a default would defeat the blank "Select a <type>..." state and auto-grab a
		// target. Non-target dependent lists (resolution/fps/format) still auto-commit so their shown
		// item stays real.
		if (!combo->isEditable() && currentIndex < 0 && !isTarget) {
			auto *model = qobject_cast<QStandardItemModel *>(combo->model());
			int defaultIdx = -1;
			for (int i = 0; i < combo->count(); ++i) {
				const QStandardItem *it = model ? model->item(i) : nullptr;
				if (!it || (it->flags() & Qt::ItemIsEnabled)) { // skip a disabled placeholder row
					defaultIdx = i;
					break;
				}
			}
			if (defaultIdx >= 0) {
				combo->setCurrentIndex(defaultIdx); // show the item we are about to commit
				const json value = json::parse(combo->itemData(defaultIdx).toString().toStdString(),
							       nullptr, /*allow_exceptions=*/false);
				if (!value.is_discarded() && !value.is_null())
					QTimer::singleShot(0, this,
							   [this, name, value] { applyNow(json{{name, value}}); });
			}
		}
		connect(combo, qOverload<int>(&QComboBox::activated), this, [this, name, combo](int idx) {
			const json value = json::parse(
				combo->itemData(idx).toString().toStdString(), nullptr,
				/*allow_exceptions=*/false);
			if (!value.is_discarded() && !value.is_null())
				queueChange(name, value);
		});
		if (combo->isEditable() && format == "string") {
			connect(combo->lineEdit(), &QLineEdit::editingFinished, this, [this, name, combo] {
				// A popup pick sets the text programmatically (isModified stays
				// false) and is applied by `activated` -- this path commits only
				// FREE-TYPED entries, once per actual edit.
				if (!combo->lineEdit()->isModified())
					return;
				combo->lineEdit()->setModified(false);
				applyNow(json{{name, combo->currentText().toStdString()}});
			});
		}
		// The closed field elides long item labels instead of demanding panel width (device
		// and display names run long); the popup still opens wide enough to read them.
		combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
		combo->setMinimumContentsLength(14);
		if (QAbstractItemView *view = combo->view())
			view->setMinimumWidth(std::min(view->sizeHintForColumn(0) + 24, 480));
		installWheelGuard(combo);
		return combo;
	}

	if (type == "color") {
		auto *swatch = new QPushButton();
		swatch->setObjectName(QStringLiteral("ColorSwatch"));
		swatch->setFixedHeight(24);
		const int64_t packed = current.is_number() ? current.get<int64_t>() : int64_t(0xFFFFFFFF);
		const QColor color = colorFromEngine(packed);
		swatch->setProperty("engineColor", QVariant::fromValue<qlonglong>(packed));
		swatch->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #3C3C3C;")
					      .arg(color.name(QColor::HexRgb)));
		const bool hasAlpha = descriptor.value("hasAlpha", false);
		connect(swatch, &QPushButton::clicked, this, [this, name, swatch, hasAlpha] {
			// Seed from the stored engine color (panel rebuilds refresh the property).
			const QColor seed =
				colorFromEngine(swatch->property("engineColor").toLongLong());
			QColorDialog::ColorDialogOptions options;
			if (hasAlpha)
				options |= QColorDialog::ShowAlphaChannel;
			const QColor picked =
				QColorDialog::getColor(seed, this, QStringLiteral("Select color"), options);
			if (!picked.isValid())
				return;
			swatch->setProperty("engineColor",
					    QVariant::fromValue<qlonglong>(engineFromColor(picked)));
			swatch->setStyleSheet(
				QStringLiteral("background-color: %1; border: 1px solid #3C3C3C;")
					.arg(picked.name(QColor::HexRgb)));
			queueChange(name, engineFromColor(picked));
		});
		return swatch;
	}

	if (type == "font") {
		auto *button = new QPushButton(fontPreviewText(current.is_object() ? current : json::object()));
		connect(button, &QPushButton::clicked, this, [this, name, current, button] {
			// Seed the dialog from the stored FontValue.
			QFont seed;
			if (current.is_object()) {
				if (current.contains("face") && current["face"].is_string())
					seed.setFamily(QString::fromStdString(current["face"].get<std::string>()));
				if (current.contains("size") && current["size"].is_number())
					seed.setPointSize(current["size"].get<int>());
				const int flags = current.value("flags", 0);
				seed.setBold(flags & 1);
				seed.setItalic(flags & 2);
				seed.setUnderline(flags & 4);
				seed.setStrikeOut(flags & 8);
			}
			bool okPicked = false;
			const QFont picked =
				QFontDialog::getFont(&okPicked, seed, this, QStringLiteral("Select font"));
			if (!okPicked)
				return;
			// QFont -> contract FontValue {face, style, size, flags: 1|2|4|8}.
			const int flags = (picked.bold() ? 1 : 0) | (picked.italic() ? 2 : 0) |
					  (picked.underline() ? 4 : 0) | (picked.strikeOut() ? 8 : 0);
			const json fontValue = json{{"face", picked.family().toStdString()},
						    {"style", picked.styleName().toStdString()},
						    {"size", picked.pointSize()},
						    {"flags", flags}};
			button->setText(fontPreviewText(fontValue));  // optimistic: the cascade-gated reconcile no longer reasserts it
			queueChange(name, fontValue);
		});
		return button;
	}

	if (type == "button") {
		auto *button = new QPushButton(label);
		connect(button, &QPushButton::clicked, this, [this, name, button] {
			if (!invoker_)
				return;
			const json reply = invoker_(name);
			if (reply.is_object() && reply.contains("error")) {
				setFieldError(button, true,
					      QString::fromStdString(
						      reply["error"].value("message", std::string("error"))));
				return;
			}
			// Contract: button side effects do not emit propertyChanged -- re-fetch.
			scheduleReload();
		});
		return button;
	}

	return nullptr; // unknown descriptor type (forward-compat: skip, never crash)
}

void PropertyPanel::queueChange(const std::string &key, json value)
{
	pending_[key] = std::move(value);
	debounce_->start(); // restart the batch window
}

void PropertyPanel::queueLiveChange(const std::string &key, json value)
{
	pending_[key] = std::move(value);
	if (QDateTime::currentMSecsSinceEpoch() - liveAppliedMs_ >= kLiveApplyMs)
		flushLive();
	else
		liveTrailing_->start();
}

void PropertyPanel::flushLive()
{
	if (pending_.empty())
		return;
	liveTrailing_->stop();
	liveAppliedMs_ = QDateTime::currentMSecsSinceEpoch();
	json batch = pending_;
	pending_ = json::object();
	// Mid-drag applies skip the success-path rebuild (it would destroy the slider under the
	// cursor); the release path reconciles the form once the gesture ends.
	suppressReload_ = true;
	applyNow(std::move(batch));
	suppressReload_ = false;
}

void PropertyPanel::endLiveDrag()
{
	liveDragActive_ = false;
	liveTrailing_->stop();
	if (!pending_.empty()) {
		// The last drag step has not been dispatched yet -- the normal apply path sends it
		// and its success rebuild reconciles the form (engine clamping surfaces here).
		flushPending();
		return;
	}
	// Everything already applied live; reconcile the form now (also runs any refresh parked
	// behind the drag).
	reloadDeferred_ = false;
	scheduleReload();
}

void PropertyPanel::flushPending()
{
	if (pending_.empty()) {
		if (reloadDeferred_) { // a refresh was parked behind an edit that never materialized
			reloadDeferred_ = false;
			scheduleReload();
		}
		return;
	}
	json batch = pending_;
	pending_ = json::object();
	applyNow(std::move(batch));
}

void PropertyPanel::applyNow(json settings)
{
	if (!applier_ || settings.empty())
		return;
	applying_ = true;
	const json reply = applier_(settings);
	applying_ = false;

	if (reply.is_object() && reply.contains("error")) {
		// 1006 names the offending key in error.data.property; flag it inline and KEEP the
		// user's value on screen (no rebuild) so they can correct it.
		const QString message =
			QString::fromStdString(reply["error"].value("message", std::string("rejected")));
		std::string property;
		if (reply["error"].contains("data") && reply["error"]["data"].is_object())
			property = reply["error"]["data"].value("property", std::string());
		if (!property.empty())
			markFieldError(property, message);
		if (reloadDeferred_) {
			// An EXTERNAL change arrived while this edit was pending; the live state
			// outranks a rejected value -- run the parked refresh after all.
			reloadDeferred_ = false;
			scheduleReload();
		}
		return;
	}

	// Success: clear stale error flags. The reply already carries the post-apply state (settings +
	// property descriptors -- same shape ListSourceProperties returns), so decide the reconcile WITHOUT
	// a second round trip:
	//   - the submitted control already shows the user's value (painted optimistically), so a full
	//     rebuild would only churn the form and clobber that value -> skip it;
	//   - rebuild ONLY when the apply CASCADED: a dependent control's descriptor (items/visibility/
	//     enabled) or a non-submitted value changed (e.g. a camera device pick repopulating the
	//     resolution/fps lists). Mid-drag live applies skip this entirely (the release reconciles).
	for (auto &[key, widget] : fieldWidgets_)
		setFieldError(widget, false, QString());
	if (suppressReload_)
		return;
	bool cascade = reloadDeferred_; // an external refresh parked behind this edit must still run
	reloadDeferred_ = false;
	if (reply.is_object() && reply.contains("result") && reply["result"].is_object()) {
		const json &res = reply["result"];
		const json newProps = res.value("properties", json::array());
		const json newSettings = res.value("settings", json::object());
		if (!cascade) {
			cascade = (newProps != lastProperties_);
			if (!cascade) {
				for (auto it = newSettings.begin(); it != newSettings.end(); ++it) {
					if (settings.contains(it.key()))
						continue; // the edited control owns its optimistic value
					if (!lastSettings_.contains(it.key()) || lastSettings_[it.key()] != it.value()) {
						cascade = true;
						break;
					}
				}
			}
		}
		lastProperties_ = newProps;  // keep caches current for the NEXT edit's cascade test
		lastSettings_ = newSettings; // (a cascade's rebuild rewrites them anyway)
	} else {
		cascade = true; // no echo to compare -> fall back to the old always-rebuild behavior
	}
	if (cascade)
		scheduleReload(); // dependent controls changed -> reconcile (coalesces with the self-echo loopback)
	else if (reloadQueued_)
		suppressQueuedReload_ = true; // neutralize the self-echo loopback's already-queued redundant rebuild
}

void PropertyPanel::markFieldError(const std::string &key, const QString &message)
{
	const auto it = fieldWidgets_.find(key);
	if (it != fieldWidgets_.end())
		setFieldError(it->second, true, message);
}

} // namespace moxrelay
