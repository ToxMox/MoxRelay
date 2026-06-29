// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioMixerStrip + LevelMeter implementation. See AudioMixerStrip.hpp for the apply model and
// docs/control-api.asyncapi.yaml (GetSourceAudio/SetSourceAudio/audioChanged/audioLevels) for
// the wire semantics this surfaces.

#include "AudioMixerStrip.hpp"

#include "WheelGuard.hpp"

#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace moxrelay {

namespace {

using nlohmann::json;

// Live-drag apply cadence: at most one dispatch per window while a slider moves, with a
// trailing flush so the final position always lands. 50 ms (20 Hz) spaces successive engine
// gain ramps (~10 ms each) five ramp-lengths apart and bounds the audioChanged rate per control.
constexpr int kLiveApplyMs = 50;
constexpr double kGainCeiling = 20.0; // wire clamp ceiling (the verb echoes the effective value)
constexpr double kFloorDb = -60.0;    // fader/meter visual floor; below this reads as silence
constexpr int kGainSliderMax = 860;   // 1..max == -60 dB..+26 dB in 0.1 dB steps; 0 == -inf
constexpr qint64 kClipHoldMs = 1000;
// Sync-offset apply cadence: unlike the continuous gain/balance ramps, every applied offset
// change is one fade-wrapped splice in the engine, so edits coalesce into a single dispatch
// (arrow-click bursts and typing settle first); editingFinished flushes immediately.
constexpr int kSyncOffsetMaxMs = 950; // wire clamp ceiling (the verb echoes the effective value)
constexpr int kSyncApplyMs = 400;

// -60..0 dB ramp position for a linear level (meters).
double dbFraction(double linear)
{
	if (linear <= 0.0)
		return 0.0;
	const double db = 20.0 * std::log10(linear);
	return std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// LevelMeter
// ---------------------------------------------------------------------------------------------

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent)
{
	setAttribute(Qt::WA_OpaquePaintEvent);
}

void LevelMeter::setLevels(double peak, double rms)
{
	peak_ = peak;
	rms_ = rms;
	update();
}

void LevelMeter::clearLevels()
{
	setLevels(0.0, 0.0);
}

void LevelMeter::setClipped(bool clipped)
{
	if (clipped)
		clipUntilMs_ = QDateTime::currentMSecsSinceEpoch() + kClipHoldMs;
}

QSize LevelMeter::sizeHint() const
{
	return {120, 10};
}

QSize LevelMeter::minimumSizeHint() const
{
	return {48, 8};
}

void LevelMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const QRect r = rect();
	p.fillRect(r, QColor(0x2D, 0x2D, 0x30)); // raised surface token

	const int rmsW = int(std::lround(dbFraction(rms_) * r.width()));
	if (rmsW > 0)
		p.fillRect(QRect(r.left(), r.top(), rmsW, r.height()), QColor(0x2D, 0x7D, 0x9A)); // accent

	const int peakX = int(std::lround(dbFraction(peak_) * r.width()));
	if (peakX > 1)
		p.fillRect(QRect(r.left() + std::min(peakX, r.width() - 2), r.top(), 2, r.height()),
			   QColor(0xD4, 0xD4, 0xD4)); // text token as the peak tick

	const bool clipped = QDateTime::currentMSecsSinceEpoch() < clipUntilMs_;
	if (clipped)
		p.fillRect(QRect(r.right() - 3, r.top(), 4, r.height()), QColor(0xB0, 0x41, 0x3E)); // error token

	p.setPen(QColor(0x3C, 0x3C, 0x3C)); // border token
	p.drawRect(r.adjusted(0, 0, -1, -1));
}

// ---------------------------------------------------------------------------------------------
// AudioMixerStrip
// ---------------------------------------------------------------------------------------------

AudioMixerStrip::AudioMixerStrip(QWidget *parent) : QWidget(parent)
{
	setObjectName(QStringLiteral("AudioStrip"));
	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(8, 8, 8, 8);
	layout->setSpacing(8);

	auto *gainRow = new QHBoxLayout();
	gainRow->setSpacing(8);
	gainRow->addWidget(new QLabel(QStringLiteral("Gain"), this));
	gainSlider_ = new QSlider(Qt::Horizontal, this);
	gainSlider_->setRange(0, kGainSliderMax);
	gainSlider_->setValue(gainToSlider(1.0));
	gainSlider_->setToolTip(QStringLiteral("Source gain (-inf to +26 dB)"));
	installWheelGuard(gainSlider_);
	gainRow->addWidget(gainSlider_, /*stretch=*/1);
	gainLabel_ = new QLabel(gainText(1.0), this);
	gainLabel_->setMinimumWidth(64);
	gainLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
	gainRow->addWidget(gainLabel_);
	layout->addLayout(gainRow);

	auto *stateRow = new QHBoxLayout();
	stateRow->setSpacing(8);
	muteCheck_ = new QCheckBox(QStringLiteral("Mute"), this);
	stateRow->addWidget(muteCheck_);
	stateRow->addSpacing(8);
	stateRow->addWidget(new QLabel(QStringLiteral("Balance"), this));
	balanceSlider_ = new QSlider(Qt::Horizontal, this);
	balanceSlider_->setObjectName(QStringLiteral("BalanceSlider"));
	balanceSlider_->setRange(0, 100);
	balanceSlider_->setValue(50);
	balanceSlider_->setToolTip(QStringLiteral("Stereo balance (center = 0.5)"));
	installWheelGuard(balanceSlider_);
	stateRow->addWidget(balanceSlider_, /*stretch=*/1);
	layout->addLayout(stateRow);

	auto *syncRow = new QHBoxLayout();
	syncRow->setSpacing(8);
	syncRow->addWidget(new QLabel(QStringLiteral("Sync Offset"), this));
	syncSpin_ = new QSpinBox(this);
	syncSpin_->setRange(0, kSyncOffsetMaxMs);
	syncSpin_->setSingleStep(10);
	syncSpin_->setSuffix(QStringLiteral(" ms"));
	syncSpin_->setToolTip(QStringLiteral(
		"Audio delay for A/V alignment (positive-only: audio plays this much later).\n"
		"To make audio earlier instead, delay the video with a Render/Video Delay filter."));
	installWheelGuard(syncSpin_);
	syncRow->addWidget(syncSpin_);
	syncRow->addStretch(1);
	layout->addLayout(syncRow);

	meter_ = new LevelMeter(this);
	meter_->setFixedHeight(10);
	meter_->setToolTip(QStringLiteral("Source level (post gain/mute)"));
	layout->addWidget(meter_);

	gainTrailing_ = new QTimer(this);
	gainTrailing_->setSingleShot(true);
	gainTrailing_->setInterval(kLiveApplyMs);
	connect(gainTrailing_, &QTimer::timeout, this, &AudioMixerStrip::flushGain);

	balanceTrailing_ = new QTimer(this);
	balanceTrailing_->setSingleShot(true);
	balanceTrailing_->setInterval(kLiveApplyMs);
	connect(balanceTrailing_, &QTimer::timeout, this, &AudioMixerStrip::flushBalance);

	syncTrailing_ = new QTimer(this);
	syncTrailing_->setSingleShot(true);
	syncTrailing_->setInterval(kSyncApplyMs);
	connect(syncTrailing_, &QTimer::timeout, this, &AudioMixerStrip::flushSyncOffset);

	// Live apply model: every value change (drag step, groove click, keyboard step) dispatches
	// through the throttle, so the mix follows the handle WHILE it moves; the release flush
	// (and the trailing timer for non-drag changes) guarantees the final position lands.
	connect(gainSlider_, &QSlider::valueChanged, this, [this](int value) {
		if (loading_)
			return;
		gainLabel_->setText(gainText(sliderToGain(value)));
		liveGain();
	});
	connect(gainSlider_, &QSlider::sliderReleased, this, &AudioMixerStrip::flushGain);
	connect(muteCheck_, &QCheckBox::toggled, this, [this](bool on) {
		if (loading_ || sourceId_.empty() || !seams_.setSourceAudio)
			return;
		reconcile(seams_.setSourceAudio(sourceId_, json{{"muted", on}}));
	});
	connect(balanceSlider_, &QSlider::valueChanged, this, [this](int) {
		if (loading_)
			return;
		liveBalance();
	});
	connect(balanceSlider_, &QSlider::sliderReleased, this, &AudioMixerStrip::flushBalance);
	connect(syncSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int) {
		if (loading_)
			return;
		syncTrailing_->start(); // coalesce: the burst settles, then one dispatch
	});
	connect(syncSpin_, &QSpinBox::editingFinished, this, &AudioMixerStrip::flushSyncOffset);

	setVisible(false);
}

void AudioMixerStrip::setSeams(AudioSeams seams)
{
	seams_ = std::move(seams);
}

void AudioMixerStrip::setSource(const std::string &sourceId)
{
	// A pending edit belongs to the PREVIOUS source -- drop it (never cross-apply).
	gainTrailing_->stop();
	balanceTrailing_->stop();
	syncTrailing_->stop();
	sourceId_ = sourceId;
	meter_->clearLevels();
	if (!sourceId_.empty())
		loadFromSeam();
}

void AudioMixerStrip::loadFromSeam()
{
	if (sourceId_.empty() || !seams_.getSourceAudio)
		return;
	const json reply = seams_.getSourceAudio(sourceId_);
	if (!reply.is_object() || !reply.contains("result"))
		return;
	const json &r = reply["result"];
	applyState(r.value("gain", 1.0), r.value("muted", false), r.value("balance", 0.5),
		   r.value("syncOffsetMs", 0));
}

void AudioMixerStrip::applyState(double gain, bool muted, double balance, int syncOffsetMs)
{
	loading_ = true;
	{
		QSignalBlocker blockGain(gainSlider_);
		gainSlider_->setValue(gainToSlider(gain));
	}
	gainLabel_->setText(gainText(gain));
	{
		QSignalBlocker blockMute(muteCheck_);
		muteCheck_->setChecked(muted);
	}
	{
		QSignalBlocker blockBalance(balanceSlider_);
		balanceSlider_->setValue(int(std::lround(std::clamp(balance, 0.0, 1.0) * 100.0)));
	}
	{
		QSignalBlocker blockSync(syncSpin_);
		syncSpin_->setValue(std::clamp(syncOffsetMs, 0, kSyncOffsetMaxMs));
	}
	loading_ = false;
}

void AudioMixerStrip::reconcile(const nlohmann::json &reply)
{
	// The full-state echo is authoritative (it carries the clamp result). Errors leave the
	// widgets as-is -- the next audioChanged/selection reload re-syncs. Each widget skips
	// reconciliation while its own edit is in flight (mid-drag or a pending trailing apply):
	// the in-flight edit wins and its final flush echo reconciles afterward.
	if (!reply.is_object() || !reply.contains("result"))
		return;
	const json &r = reply["result"];
	loading_ = true;
	if (!gainInFlight()) {
		const double gain = r.value("gain", 1.0);
		QSignalBlocker blockGain(gainSlider_);
		gainSlider_->setValue(gainToSlider(gain));
		gainLabel_->setText(gainText(gain));
	}
	{
		QSignalBlocker blockMute(muteCheck_);
		muteCheck_->setChecked(r.value("muted", false));
	}
	if (!balanceInFlight()) {
		QSignalBlocker blockBalance(balanceSlider_);
		balanceSlider_->setValue(
			int(std::lround(std::clamp(r.value("balance", 0.5), 0.0, 1.0) * 100.0)));
	}
	if (!syncInFlight()) {
		QSignalBlocker blockSync(syncSpin_);
		syncSpin_->setValue(std::clamp(r.value("syncOffsetMs", 0), 0, kSyncOffsetMaxMs));
	}
	loading_ = false;
}

bool AudioMixerStrip::gainInFlight() const
{
	return gainSlider_->isSliderDown() || gainTrailing_->isActive();
}

bool AudioMixerStrip::balanceInFlight() const
{
	return balanceSlider_->isSliderDown() || balanceTrailing_->isActive();
}

bool AudioMixerStrip::syncInFlight() const
{
	// Focus = the user may still be typing; the editingFinished flush reconciles afterward.
	return syncSpin_->hasFocus() || syncTrailing_->isActive();
}

void AudioMixerStrip::liveGain()
{
	if (sourceId_.empty() || !seams_.setSourceAudio)
		return;
	if (QDateTime::currentMSecsSinceEpoch() - gainAppliedMs_ >= kLiveApplyMs)
		flushGain();
	else
		gainTrailing_->start();
}

void AudioMixerStrip::liveBalance()
{
	if (sourceId_.empty() || !seams_.setSourceAudio)
		return;
	if (QDateTime::currentMSecsSinceEpoch() - balanceAppliedMs_ >= kLiveApplyMs)
		flushBalance();
	else
		balanceTrailing_->start();
}

void AudioMixerStrip::flushGain()
{
	if (sourceId_.empty() || !seams_.setSourceAudio)
		return;
	gainTrailing_->stop();
	gainAppliedMs_ = QDateTime::currentMSecsSinceEpoch();
	reconcile(seams_.setSourceAudio(sourceId_, json{{"gain", sliderToGain(gainSlider_->value())}}));
}

void AudioMixerStrip::flushBalance()
{
	if (sourceId_.empty() || !seams_.setSourceAudio)
		return;
	balanceTrailing_->stop();
	balanceAppliedMs_ = QDateTime::currentMSecsSinceEpoch();
	reconcile(seams_.setSourceAudio(sourceId_, json{{"balance", balanceSlider_->value() / 100.0}}));
}

void AudioMixerStrip::flushSyncOffset()
{
	if (sourceId_.empty() || !seams_.setSourceAudio)
		return;
	syncTrailing_->stop();
	// A same-value set is a wire no-op (no event, no splice), so the focus-out flush is safe.
	reconcile(seams_.setSourceAudio(sourceId_, json{{"syncOffsetMs", syncSpin_->value()}}));
}

void AudioMixerStrip::onAudioChanged(const nlohmann::json &data)
{
	if (!data.is_object())
		return;
	loading_ = true;
	if (data.contains("gain") && data["gain"].is_number() && !gainInFlight()) {
		const double gain = data["gain"].get<double>();
		QSignalBlocker block(gainSlider_);
		gainSlider_->setValue(gainToSlider(gain));
		gainLabel_->setText(gainText(gain));
	}
	if (data.contains("muted") && data["muted"].is_boolean()) {
		QSignalBlocker block(muteCheck_);
		muteCheck_->setChecked(data["muted"].get<bool>());
	}
	if (data.contains("balance") && data["balance"].is_number() && !balanceInFlight()) {
		QSignalBlocker block(balanceSlider_);
		balanceSlider_->setValue(
			int(std::lround(std::clamp(data["balance"].get<double>(), 0.0, 1.0) * 100.0)));
	}
	if (data.contains("syncOffsetMs") && data["syncOffsetMs"].is_number_integer() && !syncInFlight()) {
		QSignalBlocker block(syncSpin_);
		syncSpin_->setValue(std::clamp(data["syncOffsetMs"].get<int>(), 0, kSyncOffsetMaxMs));
	}
	loading_ = false;
}

void AudioMixerStrip::setSourceLevels(double peak, double rms)
{
	meter_->setLevels(peak, rms);
}

void AudioMixerStrip::clearSourceLevels()
{
	meter_->clearLevels();
}

double AudioMixerStrip::sliderToGain(int s)
{
	if (s <= 0)
		return 0.0;
	if (s >= kGainSliderMax)
		return kGainCeiling; // top of travel is exactly the wire clamp ceiling
	const double db = kFloorDb + double(s) * 0.1;
	return std::pow(10.0, db / 20.0);
}

int AudioMixerStrip::gainToSlider(double gain)
{
	if (gain >= kGainCeiling)
		return kGainSliderMax;
	if (gain <= 0.0)
		return 0;
	const double db = 20.0 * std::log10(gain);
	if (db <= kFloorDb)
		return 0;
	return std::clamp(int(std::lround((db - kFloorDb) * 10.0)), 1, kGainSliderMax);
}

QString AudioMixerStrip::gainText(double gain)
{
	if (gain <= 0.0)
		return QStringLiteral("-inf dB");
	const double db = 20.0 * std::log10(gain);
	if (db <= kFloorDb)
		return QStringLiteral("-inf dB");
	return QStringLiteral("%1%2 dB").arg(db >= 0.05 ? QStringLiteral("+") : QString()).arg(db, 0, 'f', 1);
}

} // namespace moxrelay
