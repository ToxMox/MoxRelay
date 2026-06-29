// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 ToxMox / MoxRelay contributors
//
// AudioMixerStrip -- the per-source audio block in the Properties dock: gain fader (dB-scaled
// travel), mute, balance, a sync-offset spinbox, and a level bar. Shown only for audio-bearing
// source types. Every mutation rides the injected audio seams (the ControlVerbs gui* dispatch
// lambdas bound by main.cpp), so a GUI edit takes the EXACT path a wire SetSourceAudio takes:
// validation, the clamps + full-state echo, and the audioChanged event all happen in the verb
// layer; the strip reconciles its widgets to the echo.
//
// Apply model (live):
//   - the gain fader and balance slider apply WHILE the handle moves, throttled to one
//     dispatch per ~50 ms window with a trailing flush (and a release flush) so the final
//     position always lands -- the mix follows the drag, and the engine-side ramps keep
//     every step pop-free;
//   - mute applies immediately (the engine-side fade keeps the edge pop-free);
//   - the sync-offset spinbox COALESCES edits into one dispatch (trailing window + an
//     editingFinished flush): every applied change is one fade-wrapped engine splice, so
//     arrow-click bursts and typing must not splice per step.
// Wire-originated changes arrive via onAudioChanged (the chained event sink): changed fields
// update the widgets unless that widget has an edit in flight (mid-drag or a pending trailing
// apply) -- the in-flight edit wins and its own echo reconciles afterward.
//
// LevelMeter -- the shared dB-scaled level bar: per-source rows here, the master meter in the
// toolbar. Fed from the SINGLE audioLevels stream (the same event data wire subscribers get,
// delivered in-process); linear levels are drawn on a -60..0 dB ramp with a sample-peak tick
// and a latched clip edge.

#pragma once

#include <nlohmann/json.hpp>

#include <QWidget>

#include <functional>
#include <string>

class QCheckBox;
class QLabel;
class QSlider;
class QSpinBox;
class QTimer;

namespace moxrelay {

class LevelMeter : public QWidget {
	Q_OBJECT

public:
	explicit LevelMeter(QWidget *parent = nullptr);

	// Linear levels (the wire's audioLevels units); drawn on a -60..0 dB ramp.
	void setLevels(double peak, double rms);
	void clearLevels();
	// Latches a visible clip edge for ~1 s past the last clipped frame (master meter only;
	// per-source rows never receive a clip flag -- the wire carries none).
	void setClipped(bool clipped);

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	double peak_ = 0.0;
	double rms_ = 0.0;
	qint64 clipUntilMs_ = 0; // epoch ms; clip edge shown while now < this
};

class AudioMixerStrip : public QWidget {
	Q_OBJECT

public:
	// Each seam returns the FULL reply envelope ({id, result|error}) of its wire method.
	struct AudioSeams {
		std::function<nlohmann::json(const std::string &sourceId)> getSourceAudio;
		std::function<nlohmann::json(const std::string &sourceId, const nlohmann::json &fields)>
			setSourceAudio;
	};

	explicit AudioMixerStrip(QWidget *parent = nullptr);

	void setSeams(AudioSeams seams);

	// Bind to one source (loads the current audio state through the seam) or "" to unbind.
	// Visibility is the owner's concern (the window knows the selected source's type).
	void setSource(const std::string &sourceId);

	// Wire/in-process coherence: an audioChanged for the bound source (changed fields only).
	// Main thread; the owner defers the call out of the emitting dispatch.
	void onAudioChanged(const nlohmann::json &data);

	// The bound source's audioLevels row (post-gain/mute, linear).
	void setSourceLevels(double peak, double rms);
	void clearSourceLevels();

private:
	void loadFromSeam();
	// widgets <- state (no dispatch)
	void applyState(double gain, bool muted, double balance, int syncOffsetMs);
	void reconcile(const nlohmann::json &reply); // widgets <- full-state echo
	bool gainInFlight() const;    // mid-drag or trailing apply pending
	bool balanceInFlight() const;
	bool syncInFlight() const;    // focused or trailing apply pending
	void liveGain();              // throttled live apply (drag steps, groove clicks, keys)
	void liveBalance();
	void flushGain();             // immediate apply of the current slider position
	void flushBalance();
	void flushSyncOffset();       // immediate apply of the current spinbox value

	// Fader mapping: slider 0 == silence (-inf), 1..kGainSliderMax == -60 dB .. +26 dB in
	// 0.1 dB steps; the top of travel is exactly the wire clamp ceiling (linear 20.0).
	static double sliderToGain(int s);
	static int gainToSlider(double gain);
	static QString gainText(double gain);

	QSlider *gainSlider_ = nullptr;
	QLabel *gainLabel_ = nullptr;
	QCheckBox *muteCheck_ = nullptr;
	QSlider *balanceSlider_ = nullptr;
	QSpinBox *syncSpin_ = nullptr;
	LevelMeter *meter_ = nullptr;
	QTimer *gainTrailing_ = nullptr;    // trailing live-apply flush (single shot)
	QTimer *balanceTrailing_ = nullptr;
	QTimer *syncTrailing_ = nullptr;    // coalescing flush (each apply is one engine splice)
	qint64 gainAppliedMs_ = 0;          // last dispatch time (the throttle window anchor)
	qint64 balanceAppliedMs_ = 0;

	AudioSeams seams_;
	std::string sourceId_;
	bool loading_ = false; // widget updates from state/echo never re-dispatch
};

} // namespace moxrelay
