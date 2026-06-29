// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AppSettings implementation. See AppSettings.hpp for the contract + the mode/precedence notes.

#include "AppSettings.hpp"

#include "ObsBootstrap.hpp" // moduleConfigDir() -- shared <LocalAppData>/MoxRelay resolver

#include <QString>

namespace moxrelay {

namespace {
// Key names kept in one place so the dialog, resolver, and store can never drift.
constexpr const char *kMinimizeToTray = "tray/minimizeToTray";
constexpr const char *kCloseToTray = "tray/closeToTray";
constexpr const char *kStartMinimized = "window/startMinimized";
constexpr const char *kLogLevel = "log/level";
constexpr const char *kLogToFile = "log/toFile";
constexpr const char *kLogDir = "log/dir";
constexpr const char *kFpsTier = "engine/fpsTier";
constexpr const char *kAdapterIndex = "engine/adapterIndex";
constexpr const char *kMaxSenders = "engine/maxSenders";
} // namespace

AppSettings::AppSettings()
	// Explicit Ini/UserScope/org/app -- correct even if QSettings::setDefaultFormat was missed.
	: settings_(QSettings::IniFormat, QSettings::UserScope, QStringLiteral("MoxRelay"),
		    QStringLiteral("MoxRelay"))
{
}

QString AppSettings::defaultLogDir()
{
	// Mirror the sink's resolution (moduleConfigDir() + "/logs"); moduleConfigDir() is already
	// forward-slash normalized with no trailing slash.
	return QString::fromStdString(moduleConfigDir()) + QStringLiteral("/logs");
}

bool AppSettings::minimizeToTray() const
{
	return settings_.value(QString::fromUtf8(kMinimizeToTray), false).toBool();
}
void AppSettings::setMinimizeToTray(bool v)
{
	settings_.setValue(QString::fromUtf8(kMinimizeToTray), v);
}

bool AppSettings::closeToTray() const
{
	return settings_.value(QString::fromUtf8(kCloseToTray), false).toBool();
}
void AppSettings::setCloseToTray(bool v)
{
	settings_.setValue(QString::fromUtf8(kCloseToTray), v);
}

bool AppSettings::startMinimized() const
{
	return settings_.value(QString::fromUtf8(kStartMinimized), false).toBool();
}
void AppSettings::setStartMinimized(bool v)
{
	settings_.setValue(QString::fromUtf8(kStartMinimized), v);
}

QString AppSettings::logLevel() const
{
	return settings_.value(QString::fromUtf8(kLogLevel), QStringLiteral("info")).toString();
}
void AppSettings::setLogLevel(const QString &v)
{
	settings_.setValue(QString::fromUtf8(kLogLevel), v);
}

bool AppSettings::logToFile() const
{
	return settings_.value(QString::fromUtf8(kLogToFile), false).toBool();
}
void AppSettings::setLogToFile(bool v)
{
	settings_.setValue(QString::fromUtf8(kLogToFile), v);
}

QString AppSettings::logDir() const
{
	return settings_.value(QString::fromUtf8(kLogDir), defaultLogDir()).toString();
}
void AppSettings::setLogDir(const QString &v)
{
	settings_.setValue(QString::fromUtf8(kLogDir), v);
}

int AppSettings::fpsTier() const
{
	return settings_.value(QString::fromUtf8(kFpsTier), 60).toInt();
}
void AppSettings::setFpsTier(int v)
{
	settings_.setValue(QString::fromUtf8(kFpsTier), v);
}

int AppSettings::adapterIndex() const
{
	return settings_.value(QString::fromUtf8(kAdapterIndex), 0).toInt();
}
void AppSettings::setAdapterIndex(int v)
{
	settings_.setValue(QString::fromUtf8(kAdapterIndex), v);
}

int AppSettings::maxSenders() const
{
	return settings_.value(QString::fromUtf8(kMaxSenders), 64).toInt();
}
void AppSettings::setMaxSenders(int v)
{
	settings_.setValue(QString::fromUtf8(kMaxSenders), v);
}

} // namespace moxrelay
