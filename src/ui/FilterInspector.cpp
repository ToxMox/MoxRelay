// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// FilterInspector implementation. See the header for the seam/registry-SoT contract.

#include "FilterInspector.hpp"

#include "ui/PropertyPanel.hpp"

#include <QAbstractItemModel>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

namespace moxrelay {

namespace {

using nlohmann::json;

// Item data roles: identity + the last-known label/enabled so onItemChanged can tell a checkbox
// toggle from an inline rename (QListWidget emits the same signal for both).
constexpr int kFilterIdRole = Qt::UserRole + 1;
constexpr int kNameRole = Qt::UserRole + 2;
constexpr int kEnabledRole = Qt::UserRole + 3;

} // namespace

FilterInspector::FilterInspector(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	auto *buttonRow = new QHBoxLayout();
	buttonRow->setContentsMargins(8, 4, 8, 0);
	buttonRow->setSpacing(4);
	addButton_ = new QPushButton(QStringLiteral("+"), this);
	addButton_->setObjectName(QStringLiteral("FilterAddButton"));
	addButton_->setFixedWidth(28);
	addButton_->setToolTip(QStringLiteral("Add a filter to this source"));
	removeButton_ = new QPushButton(QStringLiteral("−"), this);
	removeButton_->setObjectName(QStringLiteral("FilterRemoveButton"));
	removeButton_->setFixedWidth(28);
	removeButton_->setToolTip(QStringLiteral("Remove the selected filter"));
	buttonRow->addWidget(addButton_);
	buttonRow->addWidget(removeButton_);
	buttonRow->addStretch(1);
	layout->addLayout(buttonRow);
	connect(addButton_, &QPushButton::clicked, this, &FilterInspector::addFilterDialog);
	connect(removeButton_, &QPushButton::clicked, this, &FilterInspector::removeSelectedFilter);

	list_ = new QListWidget(this);
	// Content-sized: a short chain leaves the vertical room to the configure pane below,
	// whose placeholder/form otherwise gets squeezed behind a needless scrollbar. Long
	// names elide instead of forcing a horizontal scrollbar.
	list_->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
	list_->setMinimumHeight(72);
	list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	list_->setTextElideMode(Qt::ElideRight);
	list_->setWordWrap(false);
	list_->setDragDropMode(QAbstractItemView::InternalMove);
	list_->setDefaultDropAction(Qt::MoveAction);
	list_->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
	list_->setContextMenuPolicy(Qt::CustomContextMenu);
	layout->addWidget(list_);
	connect(list_, &QListWidget::itemChanged, this, &FilterInspector::onItemChanged);
	connect(list_, &QListWidget::itemSelectionChanged, this, &FilterInspector::onSelectionChanged);
	connect(list_->model(), &QAbstractItemModel::rowsMoved, this, [this] { onRowsMoved(); });
	connect(list_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
		QListWidgetItem *item = list_->itemAt(pos);
		if (!item || !seamsSet_)
			return;
		list_->setCurrentItem(item);
		QMenu menu(list_);
		QAction *rename = menu.addAction(QStringLiteral("Rename Filter"));
		connect(rename, &QAction::triggered, this, [this, item] { list_->editItem(item); });
		QAction *remove = menu.addAction(QStringLiteral("Remove Filter"));
		connect(remove, &QAction::triggered, this, &FilterInspector::removeSelectedFilter);
		menu.exec(list_->mapToGlobal(pos));
	});

	configurePanel_ = new PropertyPanel(this);
	// This pane edits a FILTER; the panel's default placeholder names sources (and would
	// double the Properties dock's idle message on screen).
	configurePanel_->setPlaceholderText(QStringLiteral("Select a filter to edit its settings"));
	layout->addWidget(configurePanel_, /*stretch=*/1);
	// The dock split is rebalanced around the configure form's natural height.
	connect(configurePanel_, &PropertyPanel::contentRebuilt, this,
		&FilterInspector::desiredHeightChanged);

	setSource(std::string());
}

int FilterInspector::desiredHeight() const
{
	// Button row + content-sized list + the configure pane's unscrolled form, plus this
	// widget's own layout chrome (margins/spacing deltas are inside the sizeHints).
	int h = 0;
	if (addButton_)
		h += addButton_->sizeHint().height() + 4 /*buttonRow top margin*/;
	if (list_)
		h += std::max(list_->sizeHint().height(), list_->minimumHeight());
	if (configurePanel_)
		h += configurePanel_->naturalHeight();
	h += 2 * 4; // layout spacing (two gaps at spacing 4)
	return h;
}

void FilterInspector::setSeams(FilterSeams seams)
{
	seams_ = std::move(seams);
	seamsSet_ = true;
}

void FilterInspector::setSource(const std::string &sourceId)
{
	if (seamsSet_ && !sourceId.empty() && sourceId == sourceId_)
		return; // same binding: keep selection + in-flight configure-pane edits
	sourceId_ = sourceId;
	boundFilterId_.clear();
	configurePanel_->clearTarget();
	const bool active = seamsSet_ && !sourceId_.empty();
	addButton_->setEnabled(active);
	removeButton_->setEnabled(false);
	if (!active) {
		refilling_ = true;
		list_->clear();
		refilling_ = false;
		emit desiredHeightChanged();
		return;
	}
	refreshNow();
}

void FilterInspector::scheduleRefresh()
{
	if (refreshQueued_)
		return;
	refreshQueued_ = true;
	QTimer::singleShot(0, this, [this] {
		refreshQueued_ = false;
		refreshNow();
	});
}

void FilterInspector::onFilterPropertyChanged(const std::string &filterId)
{
	if (!filterId.empty() && filterId == boundFilterId_)
		configurePanel_->scheduleReload();
}

void FilterInspector::refreshNow()
{
	if (!seamsSet_ || sourceId_.empty() || !seams_.listFilters)
		return;
	const json reply = seams_.listFilters(sourceId_);
	if (!reply.is_object() || !reply.contains("result"))
		return;

	const std::string keepId = selectedFilterId();
	{
		QSignalBlocker block(list_);
		refilling_ = true;
		list_->clear();
		for (const auto &f : reply["result"].value("filters", json::array())) {
			const QString id = QString::fromStdString(f.value("filterId", std::string()));
			const QString name = QString::fromStdString(f.value("name", std::string()));
			const QString type = QString::fromStdString(f.value("filterType", std::string()));
			const QString kind = QString::fromStdString(f.value("kind", std::string()));
			const bool enabled = f.value("enabled", true);
			auto *item = new QListWidgetItem(name);
			item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable |
				       Qt::ItemIsDragEnabled);
			item->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
			// Full name first: rows elide, the tooltip is where a long label survives.
			item->setToolTip(QStringLiteral("%1 - %2 (%3)").arg(name, type, kind));
			item->setData(kFilterIdRole, id);
			item->setData(kNameRole, name);
			item->setData(kEnabledRole, enabled);
			list_->addItem(item);
			if (!keepId.empty() && id.toStdString() == keepId)
				list_->setCurrentItem(item);
		}
		refilling_ = false;
	}
	onSelectionChanged(); // rebind (guarded) to the possibly changed selection
	emit desiredHeightChanged(); // the content-sized list grew or shrank
}

std::string FilterInspector::selectedFilterId() const
{
	const QListWidgetItem *item = list_ ? list_->currentItem() : nullptr;
	return item ? item->data(kFilterIdRole).toString().toStdString() : std::string();
}

void FilterInspector::onSelectionChanged()
{
	const std::string filterId = selectedFilterId();
	removeButton_->setEnabled(!filterId.empty());
	if (!filterId.empty() && filterId == boundFilterId_)
		return; // unchanged binding: keep in-flight configure-pane edits
	boundFilterId_ = filterId;
	if (filterId.empty()) {
		configurePanel_->clearTarget();
		return;
	}
	const std::string sourceId = sourceId_;
	configurePanel_->setTarget(
		[this, sourceId, filterId] { return seams_.listFilterProperties(sourceId, filterId); },
		[this, sourceId, filterId](const json &settings) {
			return seams_.setFilterProperties(sourceId, filterId, settings);
		},
		[this, sourceId, filterId](const std::string &property) {
			return seams_.invokeFilterButton(sourceId, filterId, property);
		});
}

void FilterInspector::onItemChanged(QListWidgetItem *item)
{
	if (refilling_ || !item || !seamsSet_)
		return;
	const std::string filterId = item->data(kFilterIdRole).toString().toStdString();
	if (filterId.empty())
		return;

	// One signal serves both affordances; the stored roles say which one fired.
	const bool wasEnabled = item->data(kEnabledRole).toBool();
	const bool nowEnabled = item->checkState() == Qt::Checked;
	if (nowEnabled != wasEnabled) {
		item->setData(kEnabledRole, nowEnabled);
		if (seams_.setFilterEnabled)
			seams_.setFilterEnabled(sourceId_, filterId, nowEnabled);
		return;
	}

	const QString oldName = item->data(kNameRole).toString();
	const QString newName = item->text();
	if (newName != oldName) {
		if (newName.trimmed().isEmpty()) { // keep the label usable; revert blank edits
			QSignalBlocker block(list_);
			item->setText(oldName);
			return;
		}
		item->setData(kNameRole, newName);
		if (seams_.renameFilter)
			seams_.renameFilter(sourceId_, filterId, newName.toStdString());
	}
}

void FilterInspector::onRowsMoved()
{
	if (refilling_ || !seamsSet_ || !seams_.reorderFilter)
		return;
	// Qt already moved the row; the item's new row IS the requested chain index. The reply's
	// effective index + the filterChanged event re-sync the list (scheduleRefresh coalesces).
	QListWidgetItem *item = list_->currentItem();
	if (!item)
		return;
	const std::string filterId = item->data(kFilterIdRole).toString().toStdString();
	const int newRow = list_->row(item);
	if (filterId.empty() || newRow < 0)
		return;
	seams_.reorderFilter(sourceId_, filterId, newRow);
	scheduleRefresh();
}

void FilterInspector::addFilterDialog()
{
	if (!seamsSet_ || sourceId_.empty() || !seams_.listAvailableFilters || !seams_.addFilter)
		return;
	const json reply = seams_.listAvailableFilters(sourceId_);
	if (!reply.is_object() || !reply.contains("result"))
		return;
	const json filters = reply["result"].value("filters", json::array());
	if (filters.empty()) {
		QMessageBox::information(this, QStringLiteral("Add Filter"),
					 QStringLiteral("No filter types are applicable to this source."));
		return;
	}

	QDialog dialog(this);
	dialog.setWindowTitle(QStringLiteral("Add Filter"));
	auto *form = new QFormLayout(&dialog);
	auto *typeCombo = new QComboBox(&dialog);
	for (const auto &f : filters) {
		const QString label = QString::fromStdString(f.value("label", std::string()));
		const QString kind = QString::fromStdString(f.value("kind", std::string()));
		const QString type = QString::fromStdString(f.value("filterType", std::string()));
		typeCombo->addItem(QStringLiteral("%1 (%2)").arg(label, kind), type);
	}
	auto *nameEdit = new QLineEdit(&dialog);
	nameEdit->setPlaceholderText(QStringLiteral("(type label)"));
	form->addRow(QStringLiteral("Filter"), typeCombo);
	form->addRow(QStringLiteral("Name"), nameEdit);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
	form->addRow(buttons);

	if (dialog.exec() != QDialog::Accepted || typeCombo->currentData().toString().isEmpty())
		return;

	const json added = seams_.addFilter(sourceId_, typeCombo->currentData().toString().toStdString(),
					    nameEdit->text().toStdString());
	if (added.is_object() && added.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Add Filter"),
				     QString::fromStdString(
					     added["error"].value("message", std::string("failed"))));
		return;
	}
	// filterAdded also lands via the event tap; refresh now and select the new filter.
	refreshNow();
	if (added.is_object() && added.contains("result")) {
		const QString newId =
			QString::fromStdString(added["result"].value("filterId", std::string()));
		for (int i = 0; i < list_->count(); ++i) {
			if (list_->item(i)->data(kFilterIdRole).toString() == newId) {
				list_->setCurrentRow(i);
				break;
			}
		}
	}
}

void FilterInspector::removeSelectedFilter()
{
	const std::string filterId = selectedFilterId();
	if (filterId.empty() || !seams_.removeFilter)
		return;
	const json reply = seams_.removeFilter(sourceId_, filterId);
	if (reply.is_object() && reply.contains("error")) {
		QMessageBox::warning(this, QStringLiteral("Remove Filter"),
				     QString::fromStdString(
					     reply["error"].value("message", std::string("failed"))));
		return;
	}
	refreshNow();
}

} // namespace moxrelay
