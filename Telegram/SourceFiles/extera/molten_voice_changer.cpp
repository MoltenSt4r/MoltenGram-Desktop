/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_voice_changer.h"

#include "base/random.h"
#include "base/unixtime.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "extera/extera_config.h"
#include "ffmpeg/ffmpeg_bytes_io_wrap.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio_capture_common.h"
#include "media/player/media_player_instance.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/image/image_location.h"
#include "ui/layers/generic_box.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <rpl/single.h>
#include <rpl/variable.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <numeric>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

namespace Extera::VoiceChanger {
namespace {

constexpr auto kConfigVoiceChangerEnabled = "voice_changer_enabled";
constexpr auto kConfigVoiceChangerPreview = "voice_changer_preview";
constexpr auto kConfigVoiceChangerPreset = "voice_changer_preset";
constexpr auto kConfigVoiceChangerPitch = "voice_changer_pitch";
constexpr auto kConfigVoiceChangerTempo = "voice_changer_tempo";
constexpr auto kConfigVoiceChangerFreqCrush = "voice_changer_freq_crush";
constexpr auto kConfigVoiceChangerBitCrush = "voice_changer_bit_crush";
constexpr auto kConfigVoiceChangerEchoDelay = "voice_changer_echo_delay";
constexpr auto kConfigVoiceChangerEchoFeedback = "voice_changer_echo_feedback";
constexpr auto kConfigVoiceChangerRobotRate = "voice_changer_robot_rate";
constexpr auto kConfigVoiceChangerTelephone = "voice_changer_telephone";
constexpr auto kConfigVoiceChangerGlitchRate = "voice_changer_glitch_rate";
constexpr auto kConfigVoiceChangerGlitchRepeats = "voice_changer_glitch_repeats";

constexpr auto kDefaultFrequency = 48000;
constexpr auto kWaveformSamplesCount = 100;

inline constexpr double kPi = std::numbers::pi;

// ============================================================================
// DSP Audio Processors
// ============================================================================

// Dual-head crossfading delay-line pitch shifter (Hann-windowed)
class PitchShifter final {
public:
	static constexpr int kBufferSize = 8192;
	static constexpr int kWindowSize = 2048;

	void reset() {
		_writePos = 0;
		_phase = 0.0;
		std::fill(_buffer.begin(), _buffer.end(), 0.0f);
	}

	void process(int16_t *samples, int count, int semitones) {
		if (semitones == 0 || count <= 0) {
			return;
		}
		const double pitchRatio = std::pow(2.0, semitones / 12.0);
		const double phaseStep = (1.0 - pitchRatio) / kWindowSize;

		for (int i = 0; i < count; ++i) {
			_buffer[_writePos] = samples[i];

			double p1 = _phase;
			double p2 = p1 + 0.5;
			if (p2 >= 1.0) {
				p2 -= 1.0;
			}

			const double delay1 = p1 * kWindowSize;
			const double delay2 = p2 * kWindowSize;

			double rp1 = _writePos - delay1;
			while (rp1 < 0) rp1 += kBufferSize;
			double rp2 = _writePos - delay2;
			while (rp2 < 0) rp2 += kBufferSize;

			const int idx1 = int(rp1) % kBufferSize;
			const int idx1Next = (idx1 + 1) % kBufferSize;
			const double frac1 = rp1 - int(rp1);
			const double s1 = _buffer[idx1] * (1.0 - frac1) + _buffer[idx1Next] * frac1;

			const int idx2 = int(rp2) % kBufferSize;
			const int idx2Next = (idx2 + 1) % kBufferSize;
			const double frac2 = rp2 - int(rp2);
			const double s2 = _buffer[idx2] * (1.0 - frac2) + _buffer[idx2Next] * frac2;

			const double w1 = 0.5 * (1.0 - std::cos(2.0 * kPi * p1));
			const double pitchOut = s1 * w1 + s2 * (1.0 - w1);

			samples[i] = static_cast<int16_t>(std::clamp(
				static_cast<int>(std::round(pitchOut)),
				-32768,
				32767));

			_writePos = (_writePos + 1) % kBufferSize;
			_phase += phaseStep;
			if (_phase >= 1.0) {
				_phase -= 1.0;
			} else if (_phase < 0.0) {
				_phase += 1.0;
			}
		}
	}

private:
	std::array<float, kBufferSize> _buffer = {};
	int _writePos = 0;
	double _phase = 0.0;
};

// Robot Ring Modulation and metallic comb feedback
class RobotEffect final {
public:
	static constexpr int kCombDelayMax = 4800; // 100ms at 48kHz

	void reset() {
		_phase = 0.0;
		_combPos = 0;
		std::fill(_combBuffer.begin(), _combBuffer.end(), 0);
	}

	void process(int16_t *samples, int count, int robotRate, int sampleRate = kDefaultFrequency) {
		if (robotRate <= 0 || count <= 0) {
			return;
		}
		const double phaseStep = static_cast<double>(robotRate) / sampleRate;
		const int combDelay = std::clamp((sampleRate * 25) / 1000, 1, kCombDelayMax - 1);
		constexpr float combFeedback = 0.45f;

		for (int i = 0; i < count; ++i) {
			const double carrier = std::cos(2.0 * kPi * _phase);
			_phase += phaseStep;
			if (_phase >= 1.0) {
				_phase -= 1.0;
			}

			const int modulated = static_cast<int>(std::round(samples[i] * carrier));
			const int readIdx = (_combPos - combDelay + kCombDelayMax) % kCombDelayMax;
			const int16_t delayed = _combBuffer[readIdx];
			const int mixed = static_cast<int>(modulated * 0.7f + delayed * combFeedback);

			_combBuffer[_combPos] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
			_combPos = (_combPos + 1) % kCombDelayMax;

			samples[i] = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));
		}
	}

private:
	double _phase = 0.0;
	std::array<int16_t, kCombDelayMax> _combBuffer = {};
	int _combPos = 0;
};

// 2nd-order Biquad Filter (Direct Form II Transposed)
class BiquadFilter final {
public:
	void setHighpass(float freq, float q, float sampleRate) {
		const float w0 = 2.0f * float(kPi) * freq / sampleRate;
		const float cosw0 = std::cos(w0);
		const float alpha = std::sin(w0) / (2.0f * q);
		const float a0 = 1.0f + alpha;
		_b0 = ((1.0f + cosw0) / 2.0f) / a0;
		_b1 = (-(1.0f + cosw0)) / a0;
		_b2 = ((1.0f + cosw0) / 2.0f) / a0;
		_a1 = (-2.0f * cosw0) / a0;
		_a2 = (1.0f - alpha) / a0;
		reset();
	}

	void setLowpass(float freq, float q, float sampleRate) {
		const float w0 = 2.0f * float(kPi) * freq / sampleRate;
		const float cosw0 = std::cos(w0);
		const float alpha = std::sin(w0) / (2.0f * q);
		const float a0 = 1.0f + alpha;
		_b0 = ((1.0f - cosw0) / 2.0f) / a0;
		_b1 = (1.0f - cosw0) / a0;
		_b2 = ((1.0f - cosw0) / 2.0f) / a0;
		_a1 = (-2.0f * cosw0) / a0;
		_a2 = (1.0f - alpha) / a0;
		reset();
	}

	void reset() {
		_z1 = _z2 = 0.0f;
	}

	float process(float in) {
		const float out = _b0 * in + _z1;
		_z1 = _b1 * in - _a1 * out + _z2;
		_z2 = _b2 * in - _a2 * out;
		return out;
	}

private:
	float _b0 = 1.0f, _b1 = 0.0f, _b2 = 0.0f;
	float _a1 = 0.0f, _a2 = 0.0f;
	float _z1 = 0.0f, _z2 = 0.0f;
};

// Echo & Reverb Delay Line
class EchoEffect final {
public:
	static constexpr int kEchoBufSize = 48000; // 1 second at 48kHz

	void reset() {
		_writePos = 0;
		std::fill(_buffer.begin(), _buffer.end(), 0);
	}

	void process(int16_t *samples, int count, int delayMs, int feedbackPercent, int sampleRate = kDefaultFrequency) {
		if (delayMs <= 0 || feedbackPercent <= 0 || count <= 0) {
			return;
		}
		const int delaySamples = std::clamp(
			static_cast<int>(static_cast<int64_t>(delayMs) * sampleRate / 1000),
			1,
			kEchoBufSize - 1);
		const float fb = std::min(0.9f, feedbackPercent / 100.0f);

		for (int i = 0; i < count; ++i) {
			const int readIdx = (_writePos - delaySamples + kEchoBufSize) % kEchoBufSize;
			const int16_t delayed = _buffer[readIdx];
			const int mixed = static_cast<int>(samples[i] + delayed * fb);
			const int16_t clamped = static_cast<int16_t>(std::clamp(mixed, -32768, 32767));

			_buffer[_writePos] = clamped;
			_writePos = (_writePos + 1) % kEchoBufSize;
			samples[i] = clamped;
		}
	}

private:
	std::array<int16_t, kEchoBufSize> _buffer = {};
	int _writePos = 0;
};

// Freqcrush (Downsampling / Sample-Rate Decimation)
class FreqCrushEffect final {
public:
	void reset() {
		_accumulator = 0.0f;
		_heldSample = 0;
	}

	void process(int16_t *samples, int count, int freqCrush, int sampleRate = kDefaultFrequency) {
		if (freqCrush >= sampleRate || freqCrush < 1000 || count <= 0) {
			return;
		}
		const float step = static_cast<float>(sampleRate) / freqCrush;
		for (int i = 0; i < count; ++i) {
			_accumulator += 1.0f;
			if (_accumulator >= step) {
				_heldSample = samples[i];
				_accumulator -= step;
			} else {
				samples[i] = _heldSample;
			}
		}
	}

private:
	float _accumulator = 0.0f;
	int16_t _heldSample = 0;
};

// Bitcrush (Bit-depth Quantization)
void ProcessBitCrush(int16_t *samples, int count, int bitCrush) {
	if (bitCrush >= 16 || bitCrush < 2 || count <= 0) {
		return;
	}
	const int bitShift = 16 - bitCrush;
	const int bitStep = 1 << bitShift;
	const int halfStep = bitStep >> 1;

	for (int i = 0; i < count; ++i) {
		const int rounded = ((samples[i] + halfStep) / bitStep) * bitStep;
		samples[i] = static_cast<int16_t>(std::clamp(rounded, -32768, 32767));
	}
}

// Random Glitch / Stutter Effect
class GlitchEffect final {
public:
	static constexpr int kMaxGrainSize = 2400; // 50ms at 48kHz

	void reset() {
		_isGlitching = false;
		_writePos = 0;
		_readPos = 0;
		_repeatsLeft = 0;
		_cooldown = 0;
		std::fill(_grain.begin(), _grain.end(), 0);
	}

	void process(int16_t *samples, int count, int glitchRate, int glitchRepeats, int sampleRate = kDefaultFrequency) {
		if (glitchRate <= 0 || count <= 0) {
			return;
		}
		for (int i = 0; i < count; ++i) {
			if (!_isGlitching) {
				_grain[_writePos] = samples[i];
				_writePos = (_writePos + 1) % kMaxGrainSize;

				if (_cooldown > 0) {
					--_cooldown;
				} else if ((std::rand() % 10000) < (glitchRate * 4)) {
					_isGlitching = true;
					const int base = std::max(240, static_cast<int>(sampleRate * 0.02f));
					const int randAdd = std::max(240, static_cast<int>(sampleRate * 0.025f));
					_grainLength = std::min(kMaxGrainSize, base + (std::rand() % randAdd));
					_repeatsLeft = std::max(2, glitchRepeats);
					_readPos = 0;
				}
			} else {
				samples[i] = _grain[_readPos++];
				if (_readPos >= _grainLength) {
					_readPos = 0;
					if (--_repeatsLeft <= 0) {
						_isGlitching = false;
						_cooldown = static_cast<int>(
							static_cast<int64_t>(sampleRate) * (100 - glitchRate) / 200
							+ sampleRate / 20);
					}
				}
			}
		}
	}

private:
	std::array<int16_t, kMaxGrainSize> _grain = {};
	int _writePos = 0;
	int _readPos = 0;
	int _grainLength = 1440;
	int _repeatsLeft = 0;
	int _cooldown = 0;
	bool _isGlitching = false;
};

// Granular Overlap-Add (WSOLA) Time Stretching for Tempo Scaling
[[nodiscard]] std::vector<int16_t> ApplyTempoScale(
		const std::vector<int16_t> &input,
		float tempo,
		int sampleRate) {
	if (input.empty() || std::abs(tempo - 1.0f) <= 0.02f) {
		return input;
	}
	const int grainSize = std::max(256, (sampleRate * 25) / 1000); // 25ms (~1200 samples at 48kHz)
	const int hopOut = grainSize / 2;
	const int hopIn = std::max(1, static_cast<int>(std::round(hopOut * tempo)));

	std::vector<float> window(grainSize);
	for (int i = 0; i < grainSize; ++i) {
		window[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * kPi * i / (grainSize - 1))));
	}

	const size_t estimatedOutSamples = static_cast<size_t>(input.size() / tempo) + grainSize;
	std::vector<float> accum(estimatedOutSamples, 0.0f);
	std::vector<float> weight(estimatedOutSamples, 0.0f);

	size_t inPos = 0;
	size_t outPos = 0;
	while (inPos + grainSize <= input.size()) {
		for (int i = 0; i < grainSize; ++i) {
			const size_t target = outPos + i;
			if (target < accum.size()) {
				const float w = window[i];
				accum[target] += input[inPos + i] * w;
				weight[target] += w;
			}
		}
		inPos += hopIn;
		outPos += hopOut;
	}

	const size_t totalValid = outPos;
	std::vector<int16_t> output(totalValid);
	for (size_t i = 0; i < totalValid; ++i) {
		float sample = accum[i];
		if (weight[i] > 1e-4f) {
			sample /= weight[i];
		}
		output[i] = static_cast<int16_t>(std::clamp(
			static_cast<int>(std::round(sample)),
			-32768,
			32767));
	}
	return output;
}

// Dummy Document for preview audio playback
[[nodiscard]] not_null<DocumentData*> CreatePreviewDocument(
		not_null<Data::Session*> owner) {
	return owner->document(
		base::RandomValue<DocumentId>(),
		uint64(0),
		QByteArray(),
		base::unixtime::now(),
		QVector<MTPDocumentAttribute>(),
		QString(),
		InlineImageLocation(),
		ImageWithLocation(),
		ImageWithLocation(),
		false, // isPremiumSticker
		owner->session().mainDcId(),
		int32(0));
}

} // namespace

// ============================================================================
// Processor::Private Definition
// ============================================================================

struct Processor::Private {
	Parameters params;
	PitchShifter pitchShifter;
	RobotEffect robotEffect;
	BiquadFilter telephoneHp;
	BiquadFilter telephoneLp;
	EchoEffect echoEffect;
	FreqCrushEffect freqCrushEffect;
	GlitchEffect glitchEffect;
	bool telephoneInitialized = false;

	void initTelephone(int sampleRate) {
		telephoneHp.setHighpass(350.0f, 0.707f, float(sampleRate));
		telephoneLp.setLowpass(3400.0f, 0.707f, float(sampleRate));
		telephoneInitialized = true;
	}

	void reset() {
		pitchShifter.reset();
		robotEffect.reset();
		telephoneHp.reset();
		telephoneLp.reset();
		echoEffect.reset();
		freqCrushEffect.reset();
		glitchEffect.reset();
	}
};

Processor::Processor() : _d(std::make_unique<Private>()) {
	_d->initTelephone(kDefaultFrequency);
}

Processor::~Processor() = default;

void Processor::reset() {
	_d->reset();
}

void Processor::setParameters(const Parameters &params) {
	_d->params = params;
}

const Parameters &Processor::parameters() const {
	return _d->params;
}

void Processor::process(int16_t *samples, int count, int sampleRate) {
	if (count <= 0 || !samples || _d->params.isPassthrough()) {
		return;
	}

	// 1. Pitch Shifter
	if (_d->params.pitch != 0) {
		_d->pitchShifter.process(samples, count, _d->params.pitch);
	}

	// 2. Robot Ring Modulation & metallic comb resonance
	if (_d->params.robotRate > 0) {
		_d->robotEffect.process(samples, count, _d->params.robotRate, sampleRate);
	}

	// 3. Glitch & Stutter
	if (_d->params.glitchRate > 0) {
		_d->glitchEffect.process(samples, count, _d->params.glitchRate, _d->params.glitchRepeats, sampleRate);
	}

	// 4. Telephone Bandpass (350Hz .. 3400Hz)
	if (_d->params.telephone) {
		if (!_d->telephoneInitialized) {
			_d->initTelephone(sampleRate);
		}
		for (int i = 0; i < count; ++i) {
			float s = float(samples[i]);
			s = _d->telephoneHp.process(s);
			s = _d->telephoneLp.process(s);
			samples[i] = static_cast<int16_t>(std::clamp(
				static_cast<int>(std::round(s)),
				-32768,
				32767));
		}
	}

	// 5. Freqcrush (Downsampling)
	if (_d->params.freqCrush < sampleRate && _d->params.freqCrush >= 1000) {
		_d->freqCrushEffect.process(samples, count, _d->params.freqCrush, sampleRate);
	}

	// 6. Bitcrush (Quantization)
	if (_d->params.bitCrush < 16 && _d->params.bitCrush >= 2) {
		ProcessBitCrush(samples, count, _d->params.bitCrush);
	}

	// 7. Echo / Reverb Delay & Feedback
	if (_d->params.echoDelay > 0 && _d->params.echoFeedback > 0) {
		_d->echoEffect.process(samples, count, _d->params.echoDelay, _d->params.echoFeedback, sampleRate);
	}
}

void Processor::ProcessBuffer(
		std::vector<int16_t> &pcm,
		const Parameters &params,
		int sampleRate) {
	if (pcm.empty() || params.isPassthrough()) {
		return;
	}

	// Step A: Apply tempo stretching if requested
	if (std::abs(params.tempo - 1.0f) > 0.02f) {
		pcm = ApplyTempoScale(pcm, params.tempo, sampleRate);
	}

	// Step B: Apply streaming DSP chain
	Processor proc;
	proc.setParameters(params);
	constexpr int kChunk = 2048;
	for (size_t offset = 0; offset < pcm.size(); offset += kChunk) {
		const int count = std::min(int(pcm.size() - offset), kChunk);
		proc.process(pcm.data() + offset, count, sampleRate);
	}
}

// ============================================================================
// FFmpeg Decoding & Encoding Helpers
// ============================================================================

namespace {

[[nodiscard]] std::vector<int16_t> DecodeToPcm(
		const QByteArray &opusOggData,
		int targetSampleRate = kDefaultFrequency) {
	if (opusOggData.isEmpty()) {
		return {};
	}
	auto inputWrap = FFmpeg::ReadBytesWrap{
		.size = static_cast<int64>(opusOggData.size()),
		.offset = 0,
		.data = reinterpret_cast<const uchar*>(opusOggData.constData()),
	};
	auto inputFormat = FFmpeg::MakeFormatPointer(
		&inputWrap,
		&FFmpeg::ReadBytesWrap::Read,
		nullptr,
		&FFmpeg::ReadBytesWrap::Seek);
	if (!inputFormat) {
		return {};
	}
	FFmpeg::RestrictToCustomIO(inputFormat.get());

	if (avformat_find_stream_info(inputFormat.get(), nullptr) < 0) {
		return {};
	}

	const auto streamIndex = av_find_best_stream(
		inputFormat.get(),
		AVMEDIA_TYPE_AUDIO,
		-1,
		-1,
		nullptr,
		0);
	if (streamIndex < 0) {
		return {};
	}

	const auto stream = inputFormat->streams[streamIndex];
	const auto codec = avcodec_find_decoder(stream->codecpar->codec_id);
	if (!codec) {
		return {};
	}

	auto codecCtx = FFmpeg::CodecPointer(avcodec_alloc_context3(codec));
	if (!codecCtx) {
		return {};
	}

	if (avcodec_parameters_to_context(codecCtx.get(), stream->codecpar) < 0) {
		return {};
	}

	if (avcodec_open2(codecCtx.get(), codec, nullptr) < 0) {
		return {};
	}

	SwrContext *swr = nullptr;
	AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_MONO;
	if (swr_alloc_set_opts2(
			&swr,
			&outLayout,
			AV_SAMPLE_FMT_S16,
			targetSampleRate,
			&codecCtx->ch_layout,
			codecCtx->sample_fmt,
			codecCtx->sample_rate,
			0,
			nullptr) < 0 || !swr) {
		return {};
	}
	const auto swrGuard = gsl::finally([&] {
		if (swr) {
			swr_free(&swr);
		}
	});

	if (swr_init(swr) < 0) {
		return {};
	}

	auto frame = FFmpeg::MakeFramePointer();
	if (!frame) {
		return {};
	}

	auto packet = av_packet_alloc();
	const auto pktGuard = gsl::finally([&] {
		av_packet_free(&packet);
	});

	std::vector<int16_t> pcm;
	pcm.reserve(targetSampleRate * 4);

	const auto drainFrame = [&] {
		const int outSamples = av_rescale_rnd(
			swr_get_delay(swr, targetSampleRate) + frame->nb_samples,
			targetSampleRate,
			frame->sample_rate ? frame->sample_rate : targetSampleRate,
			AV_ROUND_UP);
		if (outSamples <= 0) {
			return;
		}
		const size_t prevSize = pcm.size();
		pcm.resize(prevSize + outSamples);
		uint8_t *outData = reinterpret_cast<uint8_t*>(pcm.data() + prevSize);
		const int converted = swr_convert(
			swr,
			&outData,
			outSamples,
			const_cast<const uint8_t**>(frame->extended_data),
			frame->nb_samples);
		pcm.resize(prevSize + std::max(0, converted));
	};

	while (av_read_frame(inputFormat.get(), packet) >= 0) {
		const auto pktGuardInner = gsl::finally([&] {
			av_packet_unref(packet);
		});
		if (packet->stream_index != streamIndex) {
			continue;
		}
		if (avcodec_send_packet(codecCtx.get(), packet) < 0) {
			break;
		}
		while (avcodec_receive_frame(codecCtx.get(), frame.get()) >= 0) {
			drainFrame();
		}
	}

	// Drain decoder
	avcodec_send_packet(codecCtx.get(), nullptr);
	while (avcodec_receive_frame(codecCtx.get(), frame.get()) >= 0) {
		drainFrame();
	}

	return pcm;
}

[[nodiscard]] QByteArray EncodePcmToOpus(
		const int16_t *samples,
		int sampleCount,
		int sampleRate = kDefaultFrequency) {
	if (!samples || sampleCount <= 0) {
		return {};
	}

	auto outputWrap = FFmpeg::WriteBytesWrap();
	auto outputFormat = FFmpeg::MakeWriteFormatPointer(
		static_cast<void*>(&outputWrap),
		nullptr,
		&FFmpeg::WriteBytesWrap::Write,
		&FFmpeg::WriteBytesWrap::Seek,
		"opus"_q);
	if (!outputFormat) {
		return {};
	}

	const auto encoder = avcodec_find_encoder(AV_CODEC_ID_OPUS);
	if (!encoder) {
		return {};
	}

	auto outStream = avformat_new_stream(outputFormat.get(), encoder);
	if (!outStream) {
		return {};
	}

	auto encCtx = FFmpeg::CodecPointer(avcodec_alloc_context3(encoder));
	if (!encCtx) {
		return {};
	}

	encCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
	encCtx->bit_rate = 32000;
	AVChannelLayout monoLayout = AV_CHANNEL_LAYOUT_MONO;
	av_channel_layout_copy(&encCtx->ch_layout, &monoLayout);
	encCtx->sample_rate = sampleRate;
	outStream->time_base = AVRational{ 1, sampleRate };

	if (outputFormat->oformat->flags & AVFMT_GLOBALHEADER) {
		encCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	if (avcodec_open2(encCtx.get(), encoder, nullptr) < 0) {
		return {};
	}

	if (avcodec_parameters_from_context(outStream->codecpar, encCtx.get()) < 0) {
		return {};
	}

	if (avformat_write_header(outputFormat.get(), nullptr) < 0) {
		return {};
	}

	SwrContext *swr = nullptr;
	if (swr_alloc_set_opts2(
			&swr,
			&monoLayout,
			encCtx->sample_fmt,
			sampleRate,
			&monoLayout,
			AV_SAMPLE_FMT_S16,
			sampleRate,
			0,
			nullptr) < 0 || !swr) {
		return {};
	}
	const auto swrGuard = gsl::finally([&] {
		if (swr) {
			swr_free(&swr);
		}
	});

	if (swr_init(swr) < 0) {
		return {};
	}

	const int frameSize = encCtx->frame_size > 0 ? encCtx->frame_size : 960;
	auto frame = FFmpeg::MakeFramePointer();
	if (!frame) {
		return {};
	}
	frame->nb_samples = frameSize;
	frame->format = encCtx->sample_fmt;
	av_channel_layout_copy(&frame->ch_layout, &monoLayout);
	frame->sample_rate = sampleRate;
	if (av_frame_get_buffer(frame.get(), 0) < 0) {
		return {};
	}

	auto pkt = av_packet_alloc();
	const auto pktGuard = gsl::finally([&] {
		av_packet_free(&pkt);
	});

	const auto writePackets = [&] {
		while (avcodec_receive_packet(encCtx.get(), pkt) >= 0) {
			pkt->stream_index = outStream->index;
			av_packet_rescale_ts(pkt, encCtx->time_base, outStream->time_base);
			av_interleaved_write_frame(outputFormat.get(), pkt);
			av_packet_unref(pkt);
		}
	};

	int64_t pts = 0;
	std::vector<int16_t> paddedBuffer;
	for (int offset = 0; offset < sampleCount; offset += frameSize) {
		const int samplesInFrame = std::min(sampleCount - offset, frameSize);
		const int16_t *srcSamples = samples + offset;
		if (samplesInFrame < frameSize) {
			paddedBuffer.assign(frameSize, 0);
			std::copy(srcSamples, srcSamples + samplesInFrame, paddedBuffer.begin());
			srcSamples = paddedBuffer.data();
		}

		const uint8_t *inData = reinterpret_cast<const uint8_t*>(srcSamples);
		swr_convert(swr, frame->extended_data, frameSize, &inData, frameSize);
		frame->pts = pts;
		pts += frameSize;

		if (avcodec_send_frame(encCtx.get(), frame.get()) >= 0) {
			writePackets();
		}
	}

	// Drain encoder
	avcodec_send_frame(encCtx.get(), nullptr);
	writePackets();

	av_write_trailer(outputFormat.get());

	return outputWrap.content;
}

} // namespace

// ============================================================================
// Public Waveform and Byte Processing
// ============================================================================

VoiceWaveform CollectWaveformFromPcm(
		const std::vector<int16_t> &pcm,
		int sampleRate) {
	if (pcm.empty()) {
		return {};
	}
	const int count = static_cast<int>(pcm.size());
	std::vector<uint16_t> peaks;
	peaks.reserve(kWaveformSamplesCount);

	int64_t sum = 0;
	uint16_t peak = 0;
	for (int i = 0; i < count; ++i) {
		const uint16_t sample = std::abs(static_cast<int>(pcm[i]));
		if (sample > peak) {
			peak = sample;
		}
		sum += kWaveformSamplesCount;
		if (sum >= count) {
			sum -= count;
			peaks.push_back(peak);
			peak = 0;
		}
	}
	if (peaks.empty()) {
		return {};
	}

	const int64_t totalSum = std::accumulate(peaks.begin(), peaks.end(), int64_t(0));
	const uint16_t maxPeak = std::max(
		static_cast<int32_t>(totalSum * 1.8 / peaks.size()),
		2500);

	VoiceWaveform waveform;
	waveform.resize(peaks.size());
	for (size_t i = 0; i < peaks.size(); ++i) {
		waveform[i] = static_cast<signed char>(std::min(
			31U,
			static_cast<uint32_t>(std::min(peaks[i], maxPeak)) * 31 / maxPeak));
	}
	return waveform;
}

ProcessResult ProcessVoiceBytes(
		const QByteArray &opusOggData,
		const Parameters &params) {
	if (opusOggData.isEmpty()) {
		return {};
	}
	if (params.isPassthrough()) {
		return { .bytes = opusOggData };
	}

	// 1. Decode Opus/Ogg to 16-bit mono PCM 48kHz
	auto pcm = DecodeToPcm(opusOggData, kDefaultFrequency);
	if (pcm.empty()) {
		return { .bytes = opusOggData };
	}

	// 2. Apply Audio Effects DSP pipeline
	Processor::ProcessBuffer(pcm, params, kDefaultFrequency);

	// 3. Re-encode transformed PCM back to standard Opus/Ogg container
	auto encoded = EncodePcmToOpus(pcm.data(), static_cast<int>(pcm.size()), kDefaultFrequency);
	if (encoded.isEmpty()) {
		return { .bytes = opusOggData };
	}

	auto waveform = CollectWaveformFromPcm(pcm, kDefaultFrequency);
	const crl::time duration = (static_cast<crl::time>(pcm.size()) * 1000) / kDefaultFrequency;

	return {
		.bytes = std::move(encoded),
		.waveform = std::move(waveform),
		.duration = duration,
	};
}

ProcessResult ProcessVoiceBytes(
		const QByteArray &opusOggData,
		Preset preset) {
	return ProcessVoiceBytes(opusOggData, ParametersForPreset(preset));
}

ProcessResult ProcessVoiceBytes(const QByteArray &opusOggData) {
	if (!Enabled()) {
		return { .bytes = opusOggData };
	}
	return ProcessVoiceBytes(opusOggData, CurrentParameters());
}

// ============================================================================
// Real-time Streaming PCM Processing Hook
// ============================================================================

namespace {

std::unique_ptr<Processor> RealtimeStreamProcessor;

} // namespace

void ProcessPcmStream(int16_t *samples, int count, int sampleRate) {
	if (!Enabled() || !samples || count <= 0) {
		return;
	}
	if (!RealtimeStreamProcessor) {
		RealtimeStreamProcessor = std::make_unique<Processor>();
	}
	RealtimeStreamProcessor->setParameters(CurrentParameters());
	RealtimeStreamProcessor->process(samples, count, sampleRate);
}

void ResetPcmStream() {
	if (RealtimeStreamProcessor) {
		RealtimeStreamProcessor->reset();
	}
}

// ============================================================================
// Configuration & Presets
// ============================================================================

bool Enabled() {
	return Config::Instance().boolValue(kConfigVoiceChangerEnabled);
}

void SetEnabled(bool enabled) {
	Config::Instance().setBool(kConfigVoiceChangerEnabled, enabled);
	if (!enabled) {
		ResetPcmStream();
	}
}

bool PreviewBeforeSend() {
	return Config::Instance().boolValue(kConfigVoiceChangerPreview);
}

void SetPreviewBeforeSend(bool preview) {
	Config::Instance().setBool(kConfigVoiceChangerPreview, preview);
}

Preset ActivePreset() {
	const int val = Config::Instance().intValue(kConfigVoiceChangerPreset);
	if (val >= 0 && val <= static_cast<int>(Preset::Custom)) {
		return static_cast<Preset>(val);
	}
	return Preset::Normal;
}

void SetPreset(Preset preset) {
	Config::Instance().setInt(kConfigVoiceChangerPreset, static_cast<int>(preset));
	ResetPcmStream();
}

QString PresetName(Preset preset) {
	switch (preset) {
	case Preset::Normal: return u"Normal (Original)"_q;
	case Preset::DeepBass: return u"Deep / Bass"_q;
	case Preset::Helium: return u"Helium / High Pitch"_q;
	case Preset::Robot: return u"Robot Dalek"_q;
	case Preset::FastChipmunk: return u"Fast / Chipmunk"_q;
	case Preset::Slow: return u"Slow Motion"_q;
	case Preset::EchoReverb: return u"Echo / Reverb"_q;
	case Preset::Telephone: return u"Telephone Lo-Fi"_q;
	case Preset::Custom: return u"Custom (Manual)"_q;
	}
	return u"Unknown"_q;
}

QString PresetDescription(Preset preset) {
	switch (preset) {
	case Preset::Normal:
		return u"Original natural voice without modification."_q;
	case Preset::DeepBass:
		return u"Deep baritone pitch with warm body and subtle acoustic delay."_q;
	case Preset::Helium:
		return u"High-pitched squeaky helium balloon vocal effect."_q;
	case Preset::Robot:
		return u"Metallic Dalek cyborg voice with ring modulation and comb resonance."_q;
	case Preset::FastChipmunk:
		return u"High pitch speech accelerated by 35% for rapid chipmunk effect."_q;
	case Preset::Slow:
		return u"Deepened pitch slowed to 75% tempo for deliberate slurred speech."_q;
	case Preset::EchoReverb:
		return u"Spacious cathedral echo chamber with 280ms feedback delay."_q;
	case Preset::Telephone:
		return u"Vintage 300-3400Hz bandpass walkie-talkie and landline filter."_q;
	case Preset::Custom:
		return u"Manually configured pitch, tempo, echo, robot and bitcrush settings."_q;
	}
	return QString();
}

const std::vector<Preset> &AllPresets() {
	static const auto kPresets = std::vector<Preset>{
		Preset::Normal,
		Preset::DeepBass,
		Preset::Helium,
		Preset::Robot,
		Preset::FastChipmunk,
		Preset::Slow,
		Preset::EchoReverb,
		Preset::Telephone,
		Preset::Custom,
	};
	return kPresets;
}

Parameters ParametersForPreset(Preset preset) {
	switch (preset) {
	case Preset::Normal:
		return Parameters{};
	case Preset::DeepBass:
		return Parameters{
			.pitch = -6,
			.tempo = 0.95f,
			.freqCrush = 48000,
			.bitCrush = 16,
			.echoDelay = 90,
			.echoFeedback = 25,
			.robotRate = 0,
			.telephone = false,
			.glitchRate = 0,
		};
	case Preset::Helium:
		return Parameters{
			.pitch = 8,
			.tempo = 1.05f,
			.freqCrush = 48000,
			.bitCrush = 16,
			.echoDelay = 0,
			.echoFeedback = 0,
			.robotRate = 0,
			.telephone = false,
			.glitchRate = 0,
		};
	case Preset::Robot:
		return Parameters{
			.pitch = -2,
			.tempo = 1.0f,
			.freqCrush = 12000,
			.bitCrush = 8,
			.echoDelay = 30,
			.echoFeedback = 40,
			.robotRate = 60,
			.telephone = false,
			.glitchRate = 0,
		};
	case Preset::FastChipmunk:
		return Parameters{
			.pitch = 7,
			.tempo = 1.35f,
			.freqCrush = 48000,
			.bitCrush = 16,
			.echoDelay = 0,
			.echoFeedback = 0,
			.robotRate = 0,
			.telephone = false,
			.glitchRate = 0,
		};
	case Preset::Slow:
		return Parameters{
			.pitch = -4,
			.tempo = 0.75f,
			.freqCrush = 48000,
			.bitCrush = 16,
			.echoDelay = 0,
			.echoFeedback = 0,
			.robotRate = 0,
			.telephone = false,
			.glitchRate = 0,
		};
	case Preset::EchoReverb:
		return Parameters{
			.pitch = 0,
			.tempo = 1.0f,
			.freqCrush = 48000,
			.bitCrush = 16,
			.echoDelay = 280,
			.echoFeedback = 65,
			.robotRate = 0,
			.telephone = false,
			.glitchRate = 0,
		};
	case Preset::Telephone:
		return Parameters{
			.pitch = 1,
			.tempo = 1.0f,
			.freqCrush = 6000,
			.bitCrush = 6,
			.echoDelay = 0,
			.echoFeedback = 0,
			.robotRate = 0,
			.telephone = true,
			.glitchRate = 8,
			.glitchRepeats = 3,
		};
	case Preset::Custom:
		return Parameters{
			.pitch = Config::Instance().intValue(kConfigVoiceChangerPitch),
			.tempo = float(std::max(50, Config::Instance().intValue(kConfigVoiceChangerTempo) ? Config::Instance().intValue(kConfigVoiceChangerTempo) : 100)) / 100.0f,
			.freqCrush = Config::Instance().intValue(kConfigVoiceChangerFreqCrush) ? Config::Instance().intValue(kConfigVoiceChangerFreqCrush) : 48000,
			.bitCrush = Config::Instance().intValue(kConfigVoiceChangerBitCrush) ? Config::Instance().intValue(kConfigVoiceChangerBitCrush) : 16,
			.echoDelay = Config::Instance().intValue(kConfigVoiceChangerEchoDelay),
			.echoFeedback = Config::Instance().intValue(kConfigVoiceChangerEchoFeedback),
			.robotRate = Config::Instance().intValue(kConfigVoiceChangerRobotRate),
			.telephone = Config::Instance().boolValue(kConfigVoiceChangerTelephone),
			.glitchRate = Config::Instance().intValue(kConfigVoiceChangerGlitchRate),
			.glitchRepeats = Config::Instance().intValue(kConfigVoiceChangerGlitchRepeats) ? Config::Instance().intValue(kConfigVoiceChangerGlitchRepeats) : 4,
		};
	}
	return Parameters{};
}

Parameters CurrentParameters() {
	return ParametersForPreset(ActivePreset());
}

void SetCustomParameters(const Parameters &params) {
	Config::Instance().setInt(kConfigVoiceChangerPitch, params.pitch);
	Config::Instance().setInt(kConfigVoiceChangerTempo, int(std::round(params.tempo * 100.0f)));
	Config::Instance().setInt(kConfigVoiceChangerFreqCrush, params.freqCrush);
	Config::Instance().setInt(kConfigVoiceChangerBitCrush, params.bitCrush);
	Config::Instance().setInt(kConfigVoiceChangerEchoDelay, params.echoDelay);
	Config::Instance().setInt(kConfigVoiceChangerEchoFeedback, params.echoFeedback);
	Config::Instance().setInt(kConfigVoiceChangerRobotRate, params.robotRate);
	Config::Instance().setBool(kConfigVoiceChangerTelephone, params.telephone);
	Config::Instance().setInt(kConfigVoiceChangerGlitchRate, params.glitchRate);
	Config::Instance().setInt(kConfigVoiceChangerGlitchRepeats, params.glitchRepeats);
	ResetPcmStream();
}

// ============================================================================
// UI: Settings Box & Section
// ============================================================================

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Voice Changer & Pitch Shifter"_q));

		const auto container = box->verticalLayout();

		// Master Enable Checkbox
		const auto enableCheckbox = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				u"Enable Voice Changer"_q,
				Enabled(),
				st::defaultCheckbox));
		enableCheckbox->checkedChanges() | rpl::on_next([=](bool checked) {
			SetEnabled(checked);
		}, enableCheckbox->lifetime());

		Ui::AddSkip(container, 4);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Apply pitch, tempo, robot, echo and voice filters to recorded voice notes."_q,
				st::boxLabel));

		Ui::AddSkip(container, st::settingsCheckboxesSkip);

		// Preview Before Send Checkbox
		const auto previewCheckbox = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				u"Show Effects Preview Before Sending"_q,
				PreviewBeforeSend(),
				st::defaultCheckbox));
		previewCheckbox->checkedChanges() | rpl::on_next([=](bool checked) {
			SetPreviewBeforeSend(checked);
		}, previewCheckbox->lifetime());

		Ui::AddSkip(container, 4);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Open an effect selection dialog whenever you record a voice note to preview sounds."_q,
				st::boxLabel));

		Ui::AddSkip(container, st::settingsCheckboxesSkip);
		Ui::AddSubsectionTitle(container, rpl::single(u"Voice Effect Presets"_q));

		const auto radioGroup = std::make_shared<Ui::RadiobuttonGroup>(
			static_cast<int>(ActivePreset()));

		for (const auto preset : AllPresets()) {
			const auto name = PresetName(preset);
			const auto desc = PresetDescription(preset);
			const auto fullText = name + u"\n"_q + desc;

			const auto btn = container->add(
				object_ptr<Ui::Radiobutton>(
					container,
					radioGroup,
					static_cast<int>(preset),
					fullText,
					st::defaultRadio));
			btn->setCheckAlignment(style::al_topleft);
			Ui::AddSkip(container, 4);
		}

		radioGroup->setChangedCallback([=](int val) {
			SetPreset(static_cast<Preset>(val));
		});

		Ui::AddSkip(container, st::settingsCheckboxesSkip);
		Ui::AddSubsectionTitle(container, rpl::single(u"Custom Parameters Adjuster"_q));

		auto currentCustom = ParametersForPreset(Preset::Custom);

		// Pitch Slider (-12 .. +12)
		const auto pitchLabel = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Pitch Shift: "_q + QString::number(currentCustom.pitch) + u" semitones"_q,
				st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
		const auto pitchSlider = container->add(
			object_ptr<Ui::MediaSlider>(container, st::settingsScale),
			st::boxRowPadding);
		pitchSlider->resize(st::settingsScale.seekSize);
		pitchSlider->setPseudoDiscrete(
			25,
			[](int index) { return index - 12; },
			currentCustom.pitch,
			[=](int value) {
				pitchLabel->setText(u"Pitch Shift: "_q + QString::number(value) + u" semitones"_q);
				Config::Instance().setInt(kConfigVoiceChangerPitch, value);
				ResetPcmStream();
			});

		Ui::AddSkip(container, 4);

		// Tempo / Speed Slider (50% .. 200%)
		const int initialTempoPercent = int(std::round(currentCustom.tempo * 100.0f));
		const auto tempoLabel = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Speech Tempo / Speed: "_q + QString::number(initialTempoPercent) + u"%"_q,
				st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
		const auto tempoSlider = container->add(
			object_ptr<Ui::MediaSlider>(container, st::settingsScale),
			st::boxRowPadding);
		tempoSlider->resize(st::settingsScale.seekSize);
		tempoSlider->setPseudoDiscrete(
			16,
			[](int index) { return 50 + index * 10; },
			initialTempoPercent,
			[=](int value) {
				tempoLabel->setText(u"Speech Tempo / Speed: "_q + QString::number(value) + u"%"_q);
				Config::Instance().setInt(kConfigVoiceChangerTempo, value);
				ResetPcmStream();
			});

		Ui::AddSkip(container, 4);

		// Echo Delay Slider (0 .. 600 ms)
		const auto echoDelayLabel = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Echo Delay: "_q + QString::number(currentCustom.echoDelay) + u" ms"_q,
				st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
		const auto echoDelaySlider = container->add(
			object_ptr<Ui::MediaSlider>(container, st::settingsScale),
			st::boxRowPadding);
		echoDelaySlider->resize(st::settingsScale.seekSize);
		echoDelaySlider->setPseudoDiscrete(
			25,
			[](int index) { return index * 25; },
			currentCustom.echoDelay,
			[=](int value) {
				echoDelayLabel->setText(u"Echo Delay: "_q + QString::number(value) + u" ms"_q);
				Config::Instance().setInt(kConfigVoiceChangerEchoDelay, value);
				ResetPcmStream();
			});

		Ui::AddSkip(container, 4);

		// Echo Feedback Slider (0% .. 90%)
		const auto echoFbLabel = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Echo Feedback: "_q + QString::number(currentCustom.echoFeedback) + u"%"_q,
				st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
		const auto echoFbSlider = container->add(
			object_ptr<Ui::MediaSlider>(container, st::settingsScale),
			st::boxRowPadding);
		echoFbSlider->resize(st::settingsScale.seekSize);
		echoFbSlider->setPseudoDiscrete(
			19,
			[](int index) { return index * 5; },
			currentCustom.echoFeedback,
			[=](int value) {
				echoFbLabel->setText(u"Echo Feedback: "_q + QString::number(value) + u"%"_q);
				Config::Instance().setInt(kConfigVoiceChangerEchoFeedback, value);
				ResetPcmStream();
			});

		Ui::AddSkip(container, st::settingsCheckboxesSkip);

		// Reset to Defaults Button
		const auto resetBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Reset Voice Changer to Defaults"_q),
				st::settingsButtonNoIcon));
		resetBtn->setClickedCallback([=] {
			SetPreset(Preset::Normal);
			SetEnabled(false);
			SetPreviewBeforeSend(false);
			SetCustomParameters(Parameters{});
			box->closeBox();
			Ui::Toast::Show(u"Voice changer reset to default settings."_q);
		});

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void SetupSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSubsectionTitle(container, rpl::single(u"Audio & Voice Effects"_q));

	const auto toggleBtn = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Voice Changer & Pitch Shifter"_q),
			st::settingsButtonNoIcon));
	toggleBtn->toggleOn(rpl::single(Enabled()));
	toggleBtn->toggledChanges() | rpl::on_next([](bool enabled) {
		SetEnabled(enabled);
	}, toggleBtn->lifetime());

	const auto settingsBtn = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Voice Effects & Presets Settings"_q),
			st::settingsButtonNoIcon));
	settingsBtn->setClickedCallback([=] {
		ShowSettingsBox(controller);
	});
}

// ============================================================================
// UI: Interactive Preview & Selection Dialog before sending voice note
// ============================================================================

void ShowVoiceChangerDialog(
		not_null<Window::SessionController*> controller,
		const QByteArray &recordedOpus,
		crl::time originalDuration,
		const VoiceWaveform &originalWaveform,
		Fn<void(ProcessResult&&)> onApply) {
	if (recordedOpus.isEmpty()) {
		if (onApply) {
			onApply({ .waveform = originalWaveform, .duration = originalDuration });
		}
		return;
	}

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Voice Changer - Effect Preview"_q));

		const auto container = box->verticalLayout();

		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Select a voice preset below. You can preview the processed voice before sending."_q,
				st::boxLabel));

		Ui::AddSkip(container, st::settingsCheckboxesSkip);

		const auto selectedPreset = std::make_shared<Preset>(ActivePreset());
		const auto radioGroup = std::make_shared<Ui::RadiobuttonGroup>(
			static_cast<int>(*selectedPreset));

		for (const auto preset : AllPresets()) {
			const auto name = PresetName(preset);
			const auto desc = PresetDescription(preset);
			const auto fullText = name + u"\n"_q + desc;

			const auto btn = container->add(
				object_ptr<Ui::Radiobutton>(
					container,
					radioGroup,
					static_cast<int>(preset),
					fullText,
					st::defaultRadio));
			btn->setCheckAlignment(style::al_topleft);
			Ui::AddSkip(container, 4);
		}

		radioGroup->setChangedCallback([=](int val) {
			*selectedPreset = static_cast<Preset>(val);
		});

		Ui::AddSkip(container, st::settingsCheckboxesSkip);

		// Playback preview state
		struct PreviewState {
			bool isPlaying = false;
			ProcessResult processedData;
			Preset lastProcessedPreset = Preset::Normal;
			DocumentData *document = nullptr;
		};
		const auto state = std::make_shared<PreviewState>();

		const auto previewBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Play / Stop Audio Preview"_q),
				st::settingsButtonNoIcon));

		previewBtn->setClickedCallback([=] {
			using namespace ::Media::Player;
			if (state->isPlaying) {
				instance()->stop(AudioMsgId::Type::Voice, true);
				state->isPlaying = false;
				Ui::Toast::Show(u"Preview stopped."_q);
				return;
			}

			// Generate processed audio if not already generated for this preset
			if (state->processedData.bytes.isEmpty() || state->lastProcessedPreset != *selectedPreset) {
				if (*selectedPreset == Preset::Normal) {
					state->processedData = {
						.bytes = recordedOpus,
						.waveform = originalWaveform,
						.duration = originalDuration,
					};
				} else {
					state->processedData = ProcessVoiceBytes(recordedOpus, *selectedPreset);
				}
				state->lastProcessedPreset = *selectedPreset;
			}

			if (state->processedData.bytes.isEmpty()) {
				Ui::Toast::Show(u"Unable to process audio preview."_q);
				return;
			}

			state->document = CreatePreviewDocument(&controller->session().data());
			const auto mediaView = state->document->createMediaView();
			mediaView->setBytes(state->processedData.bytes);
			state->document->size = state->processedData.bytes.size();
			state->document->type = VoiceDocument;

			instance()->play({ state->document, FullMsgId() });
			state->isPlaying = true;
			Ui::Toast::Show(u"Playing effect preview..."_q);
		});

		// Clean up playback when box is destroyed
		box->lifetime().add([=] {
			if (state->isPlaying) {
				::Media::Player::instance()->stop(AudioMsgId::Type::Voice, true);
				state->isPlaying = false;
			}
		});

		// Apply & Send Button
		box->addButton(rpl::single(u"Apply & Send"_q), [=] {
			if (state->isPlaying) {
				::Media::Player::instance()->stop(AudioMsgId::Type::Voice, true);
				state->isPlaying = false;
			}
			auto result = (*selectedPreset == Preset::Normal)
				? ProcessResult{
					.bytes = recordedOpus,
					.waveform = originalWaveform,
					.duration = originalDuration,
				}
				: (state->lastProcessedPreset == *selectedPreset && !state->processedData.bytes.isEmpty())
				? std::move(state->processedData)
				: ProcessVoiceBytes(recordedOpus, *selectedPreset);

			box->closeBox();
			if (onApply) {
				onApply(std::move(result));
			}
		});

		// Send Original Button
		box->addButton(rpl::single(u"Send Original"_q), [=] {
			if (state->isPlaying) {
				::Media::Player::instance()->stop(AudioMsgId::Type::Voice, true);
				state->isPlaying = false;
			}
			box->closeBox();
			if (onApply) {
				onApply(ProcessResult{
					.bytes = recordedOpus,
					.waveform = originalWaveform,
					.duration = originalDuration,
				});
			}
		});

		box->addButton(tr::lng_cancel(), [=] {
			if (state->isPlaying) {
				::Media::Player::instance()->stop(AudioMsgId::Type::Voice, true);
				state->isPlaying = false;
			}
			box->closeBox();
		});
	}));
}

} // namespace Extera::VoiceChanger
