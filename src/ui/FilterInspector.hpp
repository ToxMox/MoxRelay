// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// FilterInspector -- the per-source filter-chain inspector: an ordered list (drag to reorder,
// checkbox to enable/disable, inline-editable label) over a reused PropertyPanel configure pane
// for the selected filter, plus the typed add picker (fed by ListAvailableFilters, so the
// video/audio applicability decision stays server-side).
//
// Every read/mutation goes through the injected FilterSeams -- thin wrappers main.cpp binds to
// the ControlVerbs gui* dispatch methods -- so a GUI filter action takes the EXACT path a wire
// request takes: filterId allocation stays in the verb layer, and filterAdded/filterRemoved/
// filterChanged fire identically for GUI and WS actions. The inspector NEVER touches libobs.
//
// The list reloads from ListFilters (the registry view -- vector order IS apply order; the
// inspector never derives order itself). External changes arrive via scheduleRefresh()
// (MainWindow forwards the filter events for the bound source); refreshes are queued +
// coalesced, and the bound configure pane survives refreshes that keep its filter selected
// (the same-binding guard pattern -- rebins would drop in-flight debounced edits).
//
// THREADING: Qt main thread only.

#pragma once

#include <nlohmann/json.hpp>

#include <QWidget>

#include <functional>
#include <string>

class QListWidget;
class QListWidgetItem;
class QPushButton;

namespace moxrelay {

class PropertyPanel;

class FilterInspector : public QWidget {
	Q_OBJECT

public:
	// Each seam returns the full reply envelope ({id, result|error}) of its wire method.
	struct FilterSeams {
		std::function<nlohmann::json(const std::string &sourceId)> listAvailableFilters;
		std::function<nlohmann::json(const std::string &sourceId)> listFilters;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterType,
					     const std::string &name)>
			addFilter;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId)> removeFilter;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId, bool enabled)>
			setFilterEnabled;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId, int index)>
			reorderFilter;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId,
					     const std::string &name)>
			renameFilter;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId)>
			listFilterProperties;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId,
					     const nlohmann::json &settings)>
			setFilterProperties;
		std::function<nlohmann::json(const std::string &sourceId, const std::string &filterId,
					     const std::string &property)>
			invokeFilterButton;
	};

	explicit FilterInspector(QWidget *parent = nullptr);

	void setSeams(FilterSeams seams);

	// Bind to one source ("" = none). Same-binding guard: re-binding the bound source keeps
	// the list selection and the configure pane (in-flight edits) intact.
	void setSource(const std::string &sourceId);

	// Queued + coalesced list reload (safe from event handlers / inside own dispatches).
	void scheduleRefresh();

	// A filter-scoped propertyChanged landed for the bound source: refresh the configure pane
	// when it shows that filter.
	void onFilterPropertyChanged(const std::string &filterId);

	// The toolbar "Add Filter" action delegates here.
	void addFilterDialog();

	// Natural height of the whole inspector (buttons + content-sized list + the configure
	// pane's unscrolled form). MainWindow rebalances the dock split around it so filter
	// settings scroll only on genuine excess.
	int desiredHeight() const;

signals:
	// desiredHeight() may have changed (list refilled or the configure form rebuilt).
	void desiredHeightChanged();

private:
	void refreshNow();
	void onItemChanged(QListWidgetItem *item);   // checkbox toggle -> enable; text edit -> rename
	void onRowsMoved();                          // drag-reorder -> ReorderFilter dispatch
	void onSelectionChanged();                   // rebind the configure pane
	void removeSelectedFilter();
	std::string selectedFilterId() const;

	FilterSeams seams_;
	bool seamsSet_ = false;
	std::string sourceId_;        // bound source ("" = none)
	std::string boundFilterId_;   // configure-pane binding ("" = none)
	bool refreshQueued_ = false;
	bool refilling_ = false;      // programmatic refill: ignore itemChanged

	QListWidget *list_ = nullptr;
	QPushButton *addButton_ = nullptr;
	QPushButton *removeButton_ = nullptr;
	PropertyPanel *configurePanel_ = nullptr;
};

} // namespace moxrelay
