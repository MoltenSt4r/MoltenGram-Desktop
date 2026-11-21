/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QByteArray>
#include <QtCore/QVector>
#include <cstdint>
#include <memory>
#include <vector>

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Extera::VoiceChanger {

using VoiceWaveform = QVector<signed char>;

enum class Preset : int {
	Normal = 0,       // Original voice / Passthrough
	DeepBass = 1,     // Deep baritone bass (-6 semitones, subtle warm echo)
	Helium = 2,       // Helium high pitch (+8 semitones)
	Robot = 3,        // Robotic Dalek synthesizer (ring mod + bitcrush)
	FastChipmunk = 4, // Fast chipmunk (+7 pitch, 1.35x tempo)
	Slow = 5,         // Slow deep speech (-4 pitch, 0.75x tempo)
	EchoReverb = 6,   // Spacious cathedral echo (280ms delay, 65% feedback)
	Telephone = 7,    // Vintage lo-fi telephone (300-3400Hz bandpass, 6kHz crush)
	Custom = 8,       // User customizable parameters
};

struct Parameters {
	int pitch = 0;             // [-12 .. +12] semitones
	float tempo = 1.0f;        // [0.5 .. 2.0] speed factor
	int freqCrush = 48000;     // [1000 .. 48000] Hz downsampling
	int bitCrush = 16;         // [2 .. 16] bits quantization
	int echoDelay = 0;         // [0 .. 1000] ms delay
	int echoFeedback = 0;      // [0 .. 90] % feedback
	int robotRate = 0;         // [0 .. 200] Hz ring mod carrier (0 = off)
	bool telephone = false;    // 300-3400Hz telephone bandpass
	int glitchRate = 0;        // [0 .. 100] % random glitch/stutter
	int glitchRepeats = 4;     // [2 .. 8] glitch repeat count

	[[nodiscard]] bool isPassthrough() const {
		return (pitch == 0)
			&& (std::abs(tempo - 1.0f) < 0.02f)
			&& (freqCrush >= 48000)
			&& (bitCrush >= 16)
			&& (echoDelay <= 0 || echoFeedback <= 0)
			&& (robotRate <= 0)
			&& (!telephone)
			&& (glitchRate <= 0);
	}
};

struct ProcessResult {
	QByteArray bytes;
	VoiceWaveform waveform;
	crl::time duration = 0;
};

// Global toggle for voice changer
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

// Toggle to show interactive effects preview dialog before sending voice note
[[nodiscard]] bool PreviewBeforeSend();
void SetPreviewBeforeSend(bool preview);

// Preset management
[[nodiscard]] Preset ActivePreset();
void SetPreset(Preset preset);
[[nodiscard]] QString PresetName(Preset preset);
[[nodiscard]] QString PresetDescription(Preset preset);
[[nodiscard]] const std::vector<Preset> &AllPresets();
[[nodiscard]] Parameters ParametersForPreset(Preset preset);
[[nodiscard]] Parameters CurrentParameters();
void SetCustomParameters(const Parameters &params);

// Audio DSP Processor for 16-bit mono PCM streams and buffers
class Processor final {
public:
	Processor();
	~Processor();

	void reset();
	void setParameters(const Parameters &params);
	[[nodiscard]] const Parameters &parameters() const;

	// In-place processing of 16-bit mono PCM samples
	void process(int16_t *samples, int count, int sampleRate = 48000);

	// In-place processing of an entire PCM buffer (includes tempo stretching)
	static void ProcessBuffer(
		std::vector<int16_t> &pcm,
		const Parameters &params,
		int sampleRate = 48000);

private:
	struct Private;
	std::unique_ptr<Private> _d;
};

// Process recorded Opus/Ogg voice note buffer
[[nodiscard]] ProcessResult ProcessVoiceBytes(
	const QByteArray &opusOggData,
	const Parameters &params);

[[nodiscard]] ProcessResult ProcessVoiceBytes(
	const QByteArray &opusOggData,
	Preset preset);

[[nodiscard]] ProcessResult ProcessVoiceBytes(
	const QByteArray &opusOggData);

// Real-time streaming PCM processing hook (for audio capture pipeline)
void ProcessPcmStream(int16_t *samples, int count, int sampleRate = 48000);
void ResetPcmStream();

// Waveform generator from PCM samples
[[nodiscard]] VoiceWaveform CollectWaveformFromPcm(
	const std::vector<int16_t> &pcm,
	int sampleRate = 48000);

// UI Settings Box and embedded Settings section
void ShowSettingsBox(not_null<Window::SessionController*> controller);
void SetupSettings(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

// UI Effects Preview and Selection Dialog before sending a recorded voice note
void ShowVoiceChangerDialog(
	not_null<Window::SessionController*> controller,
	const QByteArray &recordedOpus,
	crl::time originalDuration,
	const VoiceWaveform &originalWaveform,
	Fn<void(ProcessResult&&)> onApply);

} // namespace Extera::VoiceChanger
