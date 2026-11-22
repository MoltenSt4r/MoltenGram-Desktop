/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_songrec.h"

#include "base/call_delayed.h"
#include "base/timer.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "extera/extera_config.h"
#include "ffmpeg/ffmpeg_utility.h"
#include "lang/lang_keys.h"
#include "media/audio/media_audio_capture.h"
#include "media/player/media_player_instance.h"
#include "settings.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>
#include <QtGui/QClipboard>
#include <QtGui/QDesktopServices>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtNetwork/QHttpMultiPart>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

namespace Extera::SongRec {
namespace {

constexpr auto kConfigSongRecEnabled = "songrec_enabled";
constexpr auto kConfigSongRecProvider = "songrec_provider";
constexpr auto kConfigSongRecAudDToken = "songrec_audd_token";
constexpr auto kConfigSongRecOnlineArtwork = "songrec_online_artwork";

constexpr auto kDefaultAudDToken = "test";
constexpr int kSampleRate = 16000;
constexpr int kListenSeconds = 5;

// Working directories for caching
[[nodiscard]] QString SongRecCacheDir() {
	const auto base = cWorkingDir().isEmpty()
		? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
		: (cWorkingDir() + u"tdata"_q);
	const auto dir = base + u"/molten_songrec"_q;
	QDir().mkpath(dir);
	return dir;
}

[[nodiscard]] QString SongRecCoversDir() {
	const auto base = cWorkingDir().isEmpty()
		? QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
		: (cWorkingDir() + u"tdata"_q);
	const auto dir = base + u"/molten_songrec_covers"_q;
	QDir().mkpath(dir);
	return dir;
}

[[nodiscard]] QString HashString(const QString &input) {
	return QString::fromLatin1(
		QCryptographicHash::hash(input.toUtf8(), QCryptographicHash::Md5).toHex());
}

[[nodiscard]] QString HashData(const QByteArray &data) {
	return QString::fromLatin1(
		QCryptographicHash::hash(data, QCryptographicHash::Md5).toHex());
}

// Floor modulo for wrapping circular indices
[[nodiscard]] inline int FloorMod(int a, int b) {
	return ((a % b) + b) % b;
}

// IEEE 802.3 CRC32 calculation for Shazam signature checksum
[[nodiscard]] uint32 ComputeCrc32(const void *data, size_t length) {
	static const auto table = [] {
		std::array<uint32, 256> t{};
		for (uint32 i = 0; i < 256; ++i) {
			uint32 c = i;
			for (int j = 0; j < 8; ++j) {
				c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
			}
			t[i] = c;
		}
		return t;
	}();
	uint32 c = 0xFFFFFFFFu;
	const auto *p = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < length; ++i) {
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	}
	return c ^ 0xFFFFFFFFu;
}

// Resampling helper: anti-aliasing decimation / linear interpolation
[[nodiscard]] std::vector<int16_t> Resample(
		const int16_t *src,
		int len,
		int fromRate,
		int toRate) {
	if (fromRate == toRate || len <= 0) {
		return std::vector<int16_t>(src, src + len);
	}
	const int outLen = int(int64(len) * toRate / fromRate);
	std::vector<int16_t> out(outLen, 0);
	const double ratio = double(fromRate) / double(toRate);
	if (fromRate > toRate) {
		for (int i = 0; i < outLen; ++i) {
			const int a = int(i * ratio);
			const int b = std::min(len, int((i + 1) * ratio));
			int64 sum = 0;
			for (int k = a; k < b; ++k) {
				sum += src[k];
			}
			out[i] = int16_t(b > a ? (sum / (b - a)) : src[std::min(a, len - 1)]);
		}
	} else {
		for (int i = 0; i < outLen; ++i) {
			const double p = i * ratio;
			const int a = int(p);
			const int b = std::min(len - 1, a + 1);
			const double t = p - double(a);
			out[i] = int16_t(double(src[a]) * (1.0 - t) + double(src[b]) * t);
		}
	}
	return out;
}

// Builds a 16-bit mono RIFF/WAVE container in memory
[[nodiscard]] QByteArray MakeWav(const int16_t *samples, int count, int sampleRate) {
	const uint32 dataSize = uint32(count * sizeof(int16_t));
	const uint32 fileSize = 36 + dataSize;
	QByteArray wav;
	wav.resize(44 + dataSize);
	char *p = wav.data();

	auto write32 = [&](int off, uint32 val) {
		p[off + 0] = char(val & 0xFF);
		p[off + 1] = char((val >> 8) & 0xFF);
		p[off + 2] = char((val >> 16) & 0xFF);
		p[off + 3] = char((val >> 24) & 0xFF);
	};
	auto write16 = [&](int off, uint16 val) {
		p[off + 0] = char(val & 0xFF);
		p[off + 1] = char((val >> 8) & 0xFF);
	};

	std::memcpy(p + 0, "RIFF", 4);
	write32(4, fileSize);
	std::memcpy(p + 8, "WAVE", 4);
	std::memcpy(p + 12, "fmt ", 4);
	write32(16, 16); // Subchunk1Size
	write16(20, 1);  // AudioFormat = PCM
	write16(22, 1);  // NumChannels = 1
	write32(24, uint32(sampleRate));
	write32(28, uint32(sampleRate * sizeof(int16_t))); // ByteRate
	write16(32, sizeof(int16_t)); // BlockAlign
	write16(34, 16); // BitsPerSample
	std::memcpy(p + 36, "data", 4);
	write32(40, dataSize);
	std::memcpy(p + 44, samples, dataSize);

	return wav;
}

// Decodes the first segment (maxSeconds) of an audio file using FFmpeg into 16kHz mono S16 PCM
[[nodiscard]] std::vector<int16_t> DecodeAudioSegment(
		const QString &filePath,
		int maxSeconds = 6) {
	std::vector<int16_t> result;
	if (filePath.isEmpty() || !QFile::exists(filePath)) {
		return result;
	}

	AVFormatContext *fmtCtx = nullptr;
	const auto u8Path = filePath.toUtf8();
	if (avformat_open_input(&fmtCtx, u8Path.constData(), nullptr, nullptr) < 0) {
		return result;
	}
	const auto closeFmt = gsl::finally([&] {
		avformat_close_input(&fmtCtx);
	});

	if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
		return result;
	}

	int audioStream = -1;
	for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
		if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
			audioStream = int(i);
			break;
		}
	}
	if (audioStream < 0) {
		return result;
	}

	const auto codecPar = fmtCtx->streams[audioStream]->codecpar;
	const auto codec = avcodec_find_decoder(codecPar->codec_id);
	if (!codec) {
		return result;
	}

	auto codecCtx = avcodec_alloc_context3(codec);
	if (!codecCtx) {
		return result;
	}
	const auto freeCodec = gsl::finally([&] {
		avcodec_free_context(&codecCtx);
	});

	if (avcodec_parameters_to_context(codecCtx, codecPar) < 0) {
		return result;
	}
	if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
		return result;
	}

	// Resample to 16000Hz MONO S16
	SwrContext *swr = nullptr;
	AVChannelLayout monoLayout = AV_CHANNEL_LAYOUT_MONO;
	const int res = swr_alloc_set_opts2(
		&swr,
		&monoLayout,
		AV_SAMPLE_FMT_S16,
		kSampleRate,
		&codecCtx->ch_layout,
		codecCtx->sample_fmt,
		codecCtx->sample_rate,
		0,
		nullptr);
	if (res < 0 || !swr) {
		return result;
	}
	const auto freeSwr = gsl::finally([&] {
		swr_free(&swr);
	});

	if (swr_init(swr) < 0) {
		return result;
	}

	auto frame = av_frame_alloc();
	auto pkt = av_packet_alloc();
	const auto freePktFrame = gsl::finally([&] {
		av_frame_free(&frame);
		av_packet_free(&pkt);
	});

	const int targetSamples = kSampleRate * maxSeconds;
	result.reserve(targetSamples);

	while (av_read_frame(fmtCtx, pkt) >= 0) {
		const auto unrefPkt = gsl::finally([&] {
			av_packet_unref(pkt);
		});
		if (pkt->stream_index != audioStream) {
			continue;
		}
		if (avcodec_send_packet(codecCtx, pkt) < 0) {
			break;
		}
		while (avcodec_receive_frame(codecCtx, frame) >= 0) {
			const int outCount = swr_get_out_samples(swr, frame->nb_samples);
			const auto curSize = result.size();
			result.resize(curSize + outCount);
			auto *outPtr = reinterpret_cast<uint8_t*>(result.data() + curSize);
			const int converted = swr_convert(
				swr,
				&outPtr,
				outCount,
				const_cast<const uint8_t**>(frame->data),
				frame->nb_samples);
			if (converted > 0) {
				result.resize(curSize + converted);
			} else {
				result.resize(curSize);
			}
			if (int(result.size()) >= targetSamples) {
				result.resize(targetSamples);
				return result;
			}
		}
	}
	return result;
}

// ------------------------------------------------------------------
// SongRec / Shazam Acoustic Fingerprint Generator
// Port of marin-m/SongRec fingerprinting engine to C++20
// ------------------------------------------------------------------

struct Peak {
	int fftPass = 0;
	int magnitude = 0;
	int bin = 0;
};

class Generator final {
public:
	Generator() {
		for (int i = 0; i < kFftSize; ++i) {
			_hanning[i] = 0.5 * (1.0 - std::cos(
				2.0 * std::numbers::pi * double(i + 1) / 2049.0));
		}
		reset();
	}

	void reset() {
		_ring.fill(0);
		_ringIndex = 0;
		for (auto &row : _fftOutputs) {
			row.fill(0.0f);
		}
		_fftIndex = 0;
		for (auto &row : _spreadOutputs) {
			row.fill(0.0f);
		}
		_spreadIndex = 0;
		_spreadWritten = 0;
		for (auto &b : _bands) {
			b.clear();
		}
	}

	void process(const int16_t *pcm, int count) {
		for (int off = 0; off + kStep <= count; off += kStep) {
			doFft(pcm, off);
			doPeakSpreading();
			if (_spreadWritten >= 46) {
				doPeakRecognition();
			}
		}
	}

	[[nodiscard]] QByteArray encode(int numberSamples) const {
		QByteArray contents;
		for (int band = 0; band < 4; ++band) {
			const auto &peaks = _bands[band];
			if (peaks.empty()) {
				continue;
			}
			QByteArray pb;
			int pass = 0;
			for (const auto &p : peaks) {
				if (p.fftPass - pass >= 255) {
					pb.append(char(0xFF));
					writeU32(pb, uint32(p.fftPass));
					pass = p.fftPass;
				}
				pb.append(char(p.fftPass - pass));
				writeU16(pb, uint16(p.magnitude));
				writeU16(pb, uint16(p.bin));
				pass = p.fftPass;
			}
			writeU32(contents, 0x60030040u + band);
			writeU32(contents, uint32(pb.size()));
			contents.append(pb);
			const int pad = (4 - (pb.size() % 4)) % 4;
			for (int i = 0; i < pad; ++i) {
				contents.append(char(0));
			}
		}

		const uint32 sizeMinusHeader = uint32(contents.size() + 8);
		const int totalLen = 56 + contents.size();
		QByteArray all;
		all.resize(totalLen);
		auto *p = reinterpret_cast<uchar*>(all.data());

		auto put32 = [&](int offset, uint32 val) {
			p[offset + 0] = uchar(val & 0xFF);
			p[offset + 1] = uchar((val >> 8) & 0xFF);
			p[offset + 2] = uchar((val >> 16) & 0xFF);
			p[offset + 3] = uchar((val >> 24) & 0xFF);
		};

		put32(0, 0xcafe2580u);
		put32(4, 0); // CRC32 placeholder
		put32(8, sizeMinusHeader);
		put32(12, 0x94119c00u);
		put32(16, 0);
		put32(20, 0);
		put32(24, 0);
		put32(28, 3u << 27); // 16000 Hz ID
		put32(32, 0);
		put32(36, 0);
		put32(40, uint32(numberSamples + int(kSampleRate * 0.24)));
		put32(44, (15u << 19) + 0x40000u);
		put32(48, 0x40000000u);
		put32(52, sizeMinusHeader);
		std::memcpy(p + 56, contents.constData(), contents.size());

		const uint32 crc = ComputeCrc32(p + 8, totalLen - 8);
		put32(4, crc);

		return all;
	}

private:
	static constexpr int kFftSize = 2048;
	static constexpr int kStep = 128;
	static constexpr int kRingSize = 2048;
	static constexpr int kHistorySize = 256;
	static constexpr int kNumBins = 1025;

	static void writeU32(QByteArray &buf, uint32 v) {
		buf.append(char(v & 0xFF));
		buf.append(char((v >> 8) & 0xFF));
		buf.append(char((v >> 16) & 0xFF));
		buf.append(char((v >> 24) & 0xFF));
	}

	static void writeU16(QByteArray &buf, uint16 v) {
		buf.append(char(v & 0xFF));
		buf.append(char((v >> 8) & 0xFF));
	}

	static void radix2Fft(double *re, double *im) {
		constexpr int n = kFftSize;
		for (int i = 1, j = 0; i < n; ++i) {
			int bit = n >> 1;
			for (; (j & bit) != 0; bit >>= 1) {
				j ^= bit;
			}
			j ^= bit;
			if (i < j) {
				std::swap(re[i], re[j]);
				std::swap(im[i], im[j]);
			}
		}
		for (int len = 2; len <= n; len <<= 1) {
			const double ang = -2.0 * std::numbers::pi / double(len);
			const double wr = std::cos(ang);
			const double wi = std::sin(ang);
			for (int i = 0; i < n; i += len) {
				double cr = 1.0;
				double ci = 0.0;
				for (int k = 0; k < len / 2; ++k) {
					const int a = i + k;
					const int b = a + len / 2;
					const double xr = re[b] * cr - im[b] * ci;
					const double xi = re[b] * ci + im[b] * cr;
					re[b] = re[a] - xr;
					im[b] = im[a] - xi;
					re[a] += xr;
					im[a] += xi;
					const double ncr = cr * wr - ci * wi;
					ci = cr * wi + ci * wr;
					cr = ncr;
				}
			}
		}
	}

	void doFft(const int16_t *pcm, int off) {
		std::memcpy(_ring.data() + _ringIndex, pcm + off, kStep * sizeof(int16_t));
		_ringIndex = (_ringIndex + kStep) % kRingSize;
		for (int i = 0; i < kFftSize; ++i) {
			_re[i] = double(_ring[(i + _ringIndex) % kRingSize]) * _hanning[i];
			_im[i] = 0.0;
		}
		radix2Fft(_re.data(), _im.data());
		auto &out = _fftOutputs[_fftIndex];
		for (int i = 0; i <= 1024; ++i) {
			const double v = (_re[i] * _re[i] + _im[i] * _im[i]) / double(1 << 17);
			out[i] = float(std::max(v, 1e-10));
		}
		_fftIndex = (_fftIndex + 1) % kHistorySize;
	}

	void doPeakSpreading() {
		const auto &origin = _fftOutputs[FloorMod(_fftIndex - 1, kHistorySize)];
		auto &spread = _spreadOutputs[_spreadIndex];
		std::memcpy(spread.data(), origin.data(), kNumBins * sizeof(float));
		for (int p = 0; p <= 1022; ++p) {
			spread[p] = std::max(spread[p], std::max(spread[p + 1], spread[p + 2]));
		}
		static constexpr int kFormers[] = { 1, 3, 6 };
		for (int p = 0; p <= 1024; ++p) {
			for (const int former : kFormers) {
				auto &f = _spreadOutputs[FloorMod(_spreadIndex - former, kHistorySize)];
				f[p] = std::max(f[p], spread[p]);
			}
		}
		_spreadIndex = (_spreadIndex + 1) % kHistorySize;
		++_spreadWritten;
	}

	void doPeakRecognition() {
		static constexpr int kNeighbors[] = { -10, -7, -4, -3, 1, 2, 5, 8 };
		static constexpr int kOthers[] = {
			-53, -45, 165, 172, 179, 186, 193, 200, 214, 221, 228, 235, 242, 249
		};

		const auto &m46 = _fftOutputs[FloorMod(_fftIndex - 46, kHistorySize)];
		const auto &m49 = _spreadOutputs[FloorMod(_spreadIndex - 49, kHistorySize)];

		for (int bin = 10; bin <= 1014; ++bin) {
			const float v = m46[bin];
			if (v < (1.0f / 64.0f) || v < m49[bin - 1]) {
				continue;
			}
			float maxN = 0.0f;
			for (const int o : kNeighbors) {
				maxN = std::max(maxN, m49[bin + o]);
			}
			if (v <= maxN) {
				continue;
			}
			float maxO = maxN;
			for (const int o : kOthers) {
				maxO = std::max(
					maxO,
					_spreadOutputs[FloorMod(_spreadIndex + o, kHistorySize)][bin - 1]);
			}
			if (v <= maxO) {
				continue;
			}

			const int fftNumber = _spreadWritten - 46;
			const double peak = std::log(std::max(1.0 / 64.0, double(v))) * 1477.3 + 6144.0;
			const double before = std::log(std::max(1.0 / 64.0, double(m46[bin - 1]))) * 1477.3 + 6144.0;
			const double after = std::log(std::max(1.0 / 64.0, double(m46[bin + 1]))) * 1477.3 + 6144.0;
			const double var1 = peak * 2.0 - before - after;
			const double var2 = (std::abs(var1) > 1e-6) ? ((after - before) * 32.0 / var1) : 0.0;
			const double corrected = double(bin) * 64.0 + var2;
			const double hz = corrected * (16000.0 / 2.0 / 1024.0 / 64.0);

			int band = -1;
			if (hz >= 250.0 && hz < 520.0) {
				band = 0;
			} else if (hz >= 520.0 && hz < 1450.0) {
				band = 1;
			} else if (hz >= 1450.0 && hz < 3500.0) {
				band = 2;
			} else if (hz >= 3500.0 && hz <= 5500.0) {
				band = 3;
			} else {
				continue;
			}

			_bands[band].push_back(Peak{
				.fftPass = fftNumber,
				.magnitude = int(peak) & 0xFFFF,
				.bin = int(corrected) & 0xFFFF,
			});
		}
	}

	std::array<double, kFftSize> _hanning{};
	std::array<int16_t, kRingSize> _ring{};
	int _ringIndex = 0;

	std::array<std::array<float, kNumBins>, kHistorySize> _fftOutputs{};
	int _fftIndex = 0;

	std::array<std::array<float, kNumBins>, kHistorySize> _spreadOutputs{};
	int _spreadIndex = 0;
	int _spreadWritten = 0;

	std::array<std::vector<Peak>, 4> _bands;

	std::array<double, kFftSize> _re{};
	std::array<double, kFftSize> _im{};
};

} // namespace

// ------------------------------------------------------------------
// TrackResult methods
// ------------------------------------------------------------------

QString TrackResult::effectiveCover() const {
	if (!localCoverPath.isEmpty() && QFile::exists(localCoverPath)) {
		return localCoverPath;
	}
	return coverUrl;
}

QString TrackResult::formatted() const {
	if (!artist.isEmpty() && !title.isEmpty()) {
		return artist + u" \u2014 "_q + title;
	}
	return !title.isEmpty() ? title : artist;
}

QString TrackResult::searchUrl() const {
	const auto term = formatted();
	return u"https://www.youtube.com/results?search_query="_q
		+ QString::fromUtf8(QUrl::toPercentEncoding(term));
}

// ------------------------------------------------------------------
// Configuration Getters & Setters
// ------------------------------------------------------------------

bool IsEnabled() {
	const auto val = Extera::Config::Instance().valueOf(kConfigSongRecEnabled);
	return val.isBool() ? val.toBool() : true;
}

void SetEnabled(bool enabled) {
	Extera::Config::Instance().setBool(kConfigSongRecEnabled, enabled);
}

Provider CurrentProvider() {
	const auto val = Extera::Config::Instance().valueOf(kConfigSongRecProvider);
	return val.isDouble() ? Provider(val.toInt()) : Provider::Auto;
}

void SetProvider(Provider provider) {
	Extera::Config::Instance().setInt(kConfigSongRecProvider, int(provider));
}

QString AudDApiToken() {
	const auto val = Extera::Config::Instance().valueOf(kConfigSongRecAudDToken);
	return val.isString() ? val.toString().trimmed() : QString::fromLatin1(kDefaultAudDToken);
}

void SetAudDApiToken(const QString &token) {
	Extera::Config::Instance().setString(kConfigSongRecAudDToken, token.trimmed());
}

bool OnlineArtworkFallback() {
	const auto val = Extera::Config::Instance().valueOf(kConfigSongRecOnlineArtwork);
	return val.isBool() ? val.toBool() : true;
}

void SetOnlineArtworkFallback(bool enabled) {
	Extera::Config::Instance().setBool(kConfigSongRecOnlineArtwork, enabled);
}

// ------------------------------------------------------------------
// Service Implementation
// ------------------------------------------------------------------

Service &Service::Instance() {
	static Service instance;
	return instance;
}

Service::Service() {
}

Service::~Service() {
	cancel();
}

void Service::setState(State state) {
	_state = state;
	_stateChanges.fire_copy(state);
}

bool Service::isRecognizing() const {
	return _state == State::Listening || _state == State::Processing;
}

State Service::state() const {
	return _state;
}

rpl::producer<State> Service::stateChanges() const {
	return _stateChanges.events();
}

void Service::cancel() {
	if (_micListening) {
		_micListening = false;
		Media::Capture::instance()->stop();
	}
	_capturedBuffer.clear();
	setState(State::Idle);
}

std::optional<TrackResult> Service::readDiskCache(const QString &cacheKey) {
	const auto file = SongRecCacheDir() + u"/"_q + cacheKey + u".json"_q;
	QFile f(file);
	if (!f.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto doc = QJsonDocument::fromJson(f.readAll());
	if (!doc.isObject()) {
		return std::nullopt;
	}
	const auto obj = doc.object();
	TrackResult res;
	res.title = obj.value(u"title"_q).toString();
	res.artist = obj.value(u"artist"_q).toString();
	res.album = obj.value(u"album"_q).toString();
	res.coverUrl = obj.value(u"coverUrl"_q).toString();
	res.localCoverPath = obj.value(u"localCoverPath"_q).toString();
	res.songLink = obj.value(u"songLink"_q).toString();
	if (res.empty()) {
		return std::nullopt;
	}
	return res;
}

void Service::writeDiskCache(const QString &cacheKey, const TrackResult &result) {
	if (cacheKey.isEmpty() || result.empty()) {
		return;
	}
	const auto file = SongRecCacheDir() + u"/"_q + cacheKey + u".json"_q;
	QFile f(file);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return;
	}
	QJsonObject obj;
	obj[u"title"_q] = result.title;
	obj[u"artist"_q] = result.artist;
	obj[u"album"_q] = result.album;
	obj[u"coverUrl"_q] = result.coverUrl;
	obj[u"localCoverPath"_q] = result.localCoverPath;
	obj[u"songLink"_q] = result.songLink;
	f.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void Service::finishWithResult(
		not_null<Window::SessionController*> controller,
		const std::optional<TrackResult> &result,
		Fn<void(std::optional<TrackResult>)> callback) {
	if (result && !result->empty()) {
		setState(State::Success);
		Ui::Toast::Show(u"Identified: "_q + result->formatted());
		showResultBox(controller, *result);
	} else {
		setState(State::Failed);
		Ui::Toast::Show(u"Could not identify song"_q);
	}
	if (callback) {
		callback(result);
	}
	base::call_delayed(3000, this, [=] {
		if (_state == State::Success || _state == State::Failed) {
			setState(State::Idle);
		}
	});
}

void Service::recognizeAudioBuffer(
		not_null<Window::SessionController*> controller,
		const QByteArray &pcm16Mono,
		int sampleRate,
		Fn<void(std::optional<TrackResult>)> callback) {
	if (pcm16Mono.isEmpty() || sampleRate <= 0) {
		finishWithResult(controller, std::nullopt, callback);
		return;
	}

	setState(State::Processing);
	Ui::Toast::Show(u"Identifying song..."_q);

	const auto cacheKey = HashData(pcm16Mono);
	if (const auto cached = readDiskCache(cacheKey)) {
		finishWithResult(controller, cached, callback);
		return;
	}

	const int sampleCount = int(pcm16Mono.size() / sizeof(int16_t));
	const auto *srcSamples = reinterpret_cast<const int16_t*>(pcm16Mono.constData());
	const auto resampled = Resample(srcSamples, sampleCount, sampleRate, kSampleRate);
	if (resampled.size() < kSampleRate * 2) {
		finishWithResult(controller, std::nullopt, callback);
		return;
	}

	// Acoustic Fingerprint Generation
	Generator gen;
	gen.process(resampled.data(), int(resampled.size()));
	const auto sig = gen.encode(int(resampled.size()));
	const int sampleMs = int(resampled.size() * 1000 / kSampleRate);

	// WAV buffer for AudD fallback
	const auto wavData = MakeWav(resampled.data(), int(resampled.size()), kSampleRate);

	const auto provider = CurrentProvider();
	if (provider == Provider::AudD) {
		queryAudD(wavData, [=](std::optional<TrackResult> res) {
			if (res && !res->empty()) {
				writeDiskCache(cacheKey, *res);
				finishWithResult(controller, res, callback);
			} else {
				finishWithResult(controller, std::nullopt, callback);
			}
		});
		return;
	}

	// Query Shazam SongRec
	queryShazam(sig, sampleMs, [=](std::optional<TrackResult> shazamRes) {
		if (shazamRes && !shazamRes->empty()) {
			if (shazamRes->coverUrl.isEmpty() && OnlineArtworkFallback()) {
				fetchCoverByMetadata(shazamRes->artist, shazamRes->title, [=](QString cover) {
					auto updated = *shazamRes;
					if (!cover.isEmpty()) {
						updated.coverUrl = cover;
					}
					writeDiskCache(cacheKey, updated);
					finishWithResult(controller, updated, callback);
				});
			} else {
				writeDiskCache(cacheKey, *shazamRes);
				finishWithResult(controller, shazamRes, callback);
			}
			return;
		}

		// Fallback to AudD
		if (provider == Provider::Auto) {
			queryAudD(wavData, [=](std::optional<TrackResult> auddRes) {
				if (auddRes && !auddRes->empty()) {
					if (auddRes->coverUrl.isEmpty() && OnlineArtworkFallback()) {
						fetchCoverByMetadata(auddRes->artist, auddRes->title, [=](QString cover) {
							auto updated = *auddRes;
							if (!cover.isEmpty()) {
								updated.coverUrl = cover;
							}
							writeDiskCache(cacheKey, updated);
							finishWithResult(controller, updated, callback);
						});
					} else {
						writeDiskCache(cacheKey, *auddRes);
						finishWithResult(controller, auddRes, callback);
					}
				} else {
					finishWithResult(controller, std::nullopt, callback);
				}
			});
		} else {
			finishWithResult(controller, std::nullopt, callback);
		}
	});
}

void Service::recognizeFromMic(
		not_null<Window::SessionController*> controller,
		Fn<void(std::optional<TrackResult>)> callback) {
	if (isRecognizing()) {
		cancel();
	}

	setState(State::Listening);
	Ui::Toast::Show(u"Listening for music..."_q);

	_capturedBuffer.clear();
	_micListening = true;
	_captureFrequency = 48000;

	const auto capture = Media::Capture::instance();
	if (!capture->available()) {
		capture->check();
	}

	// Capture OpenAL microphone chunks
	capture->start([=](Media::Capture::Chunk chunk) {
		crl::on_main(this, [=, chunk = std::move(chunk)]() mutable {
			if (!_micListening) {
				return;
			}
			if (chunk.frequency > 0) {
				_captureFrequency = chunk.frequency;
			}
			_capturedBuffer.append(chunk.samples);

			const int neededBytes = _captureFrequency * kListenSeconds * sizeof(int16_t);
			if (_capturedBuffer.size() >= neededBytes) {
				_micListening = false;
				Media::Capture::instance()->stop();
				const auto buffer = base::take(_capturedBuffer);
				recognizeAudioBuffer(controller, buffer, _captureFrequency, callback);
			}
		});
	});

	// Timeout safeguard after 6.5s
	base::call_delayed(6500, this, [=] {
		if (_micListening) {
			_micListening = false;
			Media::Capture::instance()->stop();
			if (!_capturedBuffer.isEmpty()) {
				const auto buffer = base::take(_capturedBuffer);
				recognizeAudioBuffer(controller, buffer, _captureFrequency, callback);
			} else {
				finishWithResult(controller, std::nullopt, callback);
			}
		}
	});
}

void Service::recognizeCurrentPlayback(
		not_null<Window::SessionController*> controller,
		Fn<void(std::optional<TrackResult>)> callback) {
	const auto current = Media::Player::instance()->current(AudioMsgId::Type::Song);
	const auto document = current.audio();
	if (!document) {
		// If nothing playing, listen via mic
		recognizeFromMic(controller, callback);
		return;
	}

	const auto path = document->filepath(true);
	if (!path.isEmpty() && QFile::exists(path)) {
		recognizeFile(controller, path, callback);
		return;
	}

	// Audio document not yet downloaded locally, record from audio playback/mic
	recognizeFromMic(controller, callback);
}

void Service::recognizeFile(
		not_null<Window::SessionController*> controller,
		const QString &filePath,
		Fn<void(std::optional<TrackResult>)> callback) {
	if (filePath.isEmpty() || !QFile::exists(filePath)) {
		finishWithResult(controller, std::nullopt, callback);
		return;
	}

	setState(State::Processing);
	Ui::Toast::Show(u"Identifying song..."_q);

	const QFileInfo fi(filePath);
	const auto cacheKey = HashString(filePath + u":"_q + QString::number(fi.size()));
	if (const auto cached = readDiskCache(cacheKey)) {
		finishWithResult(controller, cached, callback);
		return;
	}

	// Decode in background thread
	crl::async([=] {
		auto pcm = DecodeAudioSegment(filePath, 6);
		crl::on_main(this, [=, pcm = std::move(pcm)]() mutable {
			if (pcm.empty()) {
				finishWithResult(controller, std::nullopt, callback);
				return;
			}
			const auto rawBytes = QByteArray(
				reinterpret_cast<const char*>(pcm.data()),
				int(pcm.size() * sizeof(int16_t)));
			recognizeAudioBuffer(controller, rawBytes, kSampleRate, callback);
		});
	});
}

// ------------------------------------------------------------------
// Remote Network Queries
// ------------------------------------------------------------------

void Service::queryShazam(
		const QByteArray &signatureBytes,
		int sampleMs,
		Fn<void(std::optional<TrackResult>)> callback) {
	if (signatureBytes.isEmpty()) {
		if (callback) callback(std::nullopt);
		return;
	}

	const auto uri = u"data:audio/vnd.shazam.sig;base64,"_q
		+ QString::fromLatin1(signatureBytes.toBase64());
	const auto now = QDateTime::currentMSecsSinceEpoch();

	QJsonObject body;
	QJsonObject geo;
	geo[u"altitude"_q] = 300;
	geo[u"latitude"_q] = 45;
	geo[u"longitude"_q] = 2;
	body[u"geolocation"_q] = geo;

	QJsonObject sig;
	sig[u"samplems"_q] = sampleMs;
	sig[u"timestamp"_q] = now;
	sig[u"uri"_q] = uri;
	body[u"signature"_q] = sig;

	body[u"timestamp"_q] = now;
	body[u"timezone"_q] = u"UTC"_q;

	const auto u1 = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
	const auto u2 = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
	const auto urlStr = QString(
		"https://amp.shazam.com/discovery/v5/en/US/android/-/tag/%1/%2"
		"?sync=true&webv3=true&sampling=true&connected=&shazamapiversion=v3&sharehub=true&video=v3")
		.arg(u1, u2);

	auto nam = new QNetworkAccessManager(this);
	auto req = QNetworkRequest(QUrl(urlStr));
	req.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_q);
	req.setRawHeader("Content-Language", "en_US");
	req.setRawHeader("User-Agent", "Dalvik/2.1.0 (Linux; U; Android 13; Pixel 7 Build/TQ3A.230805.001)");

	const auto reply = nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		nam->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			if (callback) callback(std::nullopt);
			return;
		}

		const auto doc = QJsonDocument::fromJson(reply->readAll());
		if (!doc.isObject()) {
			if (callback) callback(std::nullopt);
			return;
		}

		const auto track = doc.object().value(u"track"_q).toObject();
		if (track.isEmpty()) {
			if (callback) callback(std::nullopt);
			return;
		}

		TrackResult res;
		res.title = track.value(u"title"_q).toString();
		res.artist = track.value(u"subtitle"_q).toString();
		res.songLink = track.value(u"url"_q).toString();

		// Album from sections metadata
		const auto sections = track.value(u"sections"_q).toArray();
		for (const auto &secVal : sections) {
			const auto secObj = secVal.toObject();
			const auto metaArr = secObj.value(u"metadata"_q).toArray();
			for (const auto &metaVal : metaArr) {
				const auto metaObj = metaVal.toObject();
				if (metaObj.value(u"title"_q).toString().compare(u"Album"_q, Qt::CaseInsensitive) == 0) {
					res.album = metaObj.value(u"text"_q).toString();
					break;
				}
			}
			if (!res.album.isEmpty()) break;
		}

		// Artwork
		const auto images = track.value(u"images"_q).toObject();
		if (images.contains(u"coverarthq"_q)) {
			res.coverUrl = images.value(u"coverarthq"_q).toString();
		} else if (images.contains(u"coverart"_q)) {
			res.coverUrl = images.value(u"coverart"_q).toString();
		} else if (images.contains(u"background"_q)) {
			res.coverUrl = images.value(u"background"_q).toString();
		}

		if (res.empty()) {
			if (callback) callback(std::nullopt);
			return;
		}
		if (callback) callback(res);
	});
}

void Service::queryAudD(
		const QByteArray &wavData,
		Fn<void(std::optional<TrackResult>)> callback) {
	if (wavData.isEmpty()) {
		if (callback) callback(std::nullopt);
		return;
	}

	auto nam = new QNetworkAccessManager(this);
	auto multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType, nam);

	// Token
	QHttpPart tokenPart;
	tokenPart.setHeader(
		QNetworkRequest::ContentDispositionHeader,
		QVariant("form-data; name=\"api_token\""));
	tokenPart.setBody(AudDApiToken().toUtf8());
	multiPart->append(tokenPart);

	// Return options
	QHttpPart returnPart;
	returnPart.setHeader(
		QNetworkRequest::ContentDispositionHeader,
		QVariant("form-data; name=\"return\""));
	returnPart.setBody("apple_music,spotify,deezer");
	multiPart->append(returnPart);

	// File WAV
	QHttpPart filePart;
	filePart.setHeader(
		QNetworkRequest::ContentTypeHeader,
		QVariant("audio/wav"));
	filePart.setHeader(
		QNetworkRequest::ContentDispositionHeader,
		QVariant("form-data; name=\"file\"; filename=\"sample.wav\""));
	filePart.setBody(wavData);
	multiPart->append(filePart);

	auto req = QNetworkRequest(QUrl(u"https://api.audd.io/"_q));
	const auto reply = nam->post(req, multiPart);
	multiPart->setParent(reply);

	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		nam->deleteLater();

		if (reply->error() != QNetworkReply::NoError) {
			if (callback) callback(std::nullopt);
			return;
		}

		const auto doc = QJsonDocument::fromJson(reply->readAll());
		if (!doc.isObject()) {
			if (callback) callback(std::nullopt);
			return;
		}

		const auto root = doc.object();
		if (root.value(u"status"_q).toString() != u"success"_q) {
			if (callback) callback(std::nullopt);
			return;
		}

		const auto result = root.value(u"result"_q).toObject();
		if (result.isEmpty()) {
			if (callback) callback(std::nullopt);
			return;
		}

		TrackResult res;
		res.title = result.value(u"title"_q).toString();
		res.artist = result.value(u"artist"_q).toString();
		res.album = result.value(u"album"_q).toString();
		res.songLink = result.value(u"song_link"_q).toString();

		// Cover from Apple Music or Spotify
		const auto apple = result.value(u"apple_music"_q).toObject();
		const auto artwork = apple.value(u"artwork"_q).toObject();
		auto artUrl = artwork.value(u"url"_q).toString();
		if (!artUrl.isEmpty()) {
			artUrl.replace(u"{w}x{h}"_q, u"600x600"_q);
			res.coverUrl = artUrl;
		}

		if (res.coverUrl.isEmpty()) {
			const auto spotify = result.value(u"spotify"_q).toObject();
			const auto sAlbum = spotify.value(u"album"_q).toObject();
			const auto images = sAlbum.value(u"images"_q).toArray();
			if (!images.isEmpty()) {
				res.coverUrl = images.at(0).toObject().value(u"url"_q).toString();
			}
		}

		if (res.empty()) {
			if (callback) callback(std::nullopt);
			return;
		}
		if (callback) callback(res);
	});
}

void Service::fetchCoverByMetadata(
		const QString &artist,
		const QString &title,
		Fn<void(QString coverUrl)> callback) {
	if (title.isEmpty()) {
		if (callback) callback(QString());
		return;
	}

	const auto query = ((artist.isEmpty() ? QString() : (artist + u" "_q)) + title).trimmed();
	const auto itunesUrl = QUrl(
		u"https://itunes.apple.com/search?term="_q
		+ QString::fromUtf8(QUrl::toPercentEncoding(query))
		+ u"&media=music&entity=song&limit=1"_q);

	auto nam = new QNetworkAccessManager(this);
	const auto reply = nam->get(QNetworkRequest(itunesUrl));
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		nam->deleteLater();

		if (reply->error() == QNetworkReply::NoError) {
			const auto doc = QJsonDocument::fromJson(reply->readAll());
			const auto results = doc.object().value(u"results"_q).toArray();
			if (!results.isEmpty()) {
				auto art = results.at(0).toObject().value(u"artworkUrl100"_q).toString();
				if (!art.isEmpty()) {
					art.replace(u"100x100bb"_q, u"600x600bb"_q);
					if (callback) callback(art);
					return;
				}
			}
		}

		// Fallback to Deezer
		const auto deezerUrl = QUrl(
			u"https://api.deezer.com/search?q="_q
			+ QString::fromUtf8(QUrl::toPercentEncoding(query))
			+ u"&limit=1"_q);
		auto dNam = new QNetworkAccessManager(this);
		const auto dReply = dNam->get(QNetworkRequest(deezerUrl));
		connect(dReply, &QNetworkReply::finished, this, [=] {
			dReply->deleteLater();
			dNam->deleteLater();

			if (dReply->error() == QNetworkReply::NoError) {
				const auto doc = QJsonDocument::fromJson(dReply->readAll());
				const auto data = doc.object().value(u"data"_q).toArray();
				if (!data.isEmpty()) {
					const auto album = data.at(0).toObject().value(u"album"_q).toObject();
					const auto art = album.value(u"cover_xl"_q).toString();
					if (!art.isEmpty()) {
						if (callback) callback(art);
						return;
					}
				}
			}
			if (callback) callback(QString());
		});
	});
}

void Service::downloadCover(
		const QString &url,
		Fn<void(QString localPath)> callback) {
	if (url.isEmpty()) {
		if (callback) callback(QString());
		return;
	}

	const auto targetPath = SongRecCoversDir() + u"/"_q + HashString(url) + u".jpg"_q;
	if (QFile::exists(targetPath) && QFileInfo(targetPath).size() > 0) {
		if (callback) callback(targetPath);
		return;
	}

	auto nam = new QNetworkAccessManager(this);
	const auto reply = nam->get(QNetworkRequest(QUrl(url)));
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		nam->deleteLater();

		if (reply->error() == QNetworkReply::NoError) {
			QFile f(targetPath);
			if (f.open(QIODevice::WriteOnly)) {
				f.write(reply->readAll());
				f.close();
				if (callback) callback(targetPath);
				return;
			}
		}
		if (callback) callback(QString());
	});
}

// ------------------------------------------------------------------
// Result Dialog & Settings Dialog
// ------------------------------------------------------------------

void Service::showResultBox(
		not_null<Window::SessionController*> controller,
		const TrackResult &result) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Music Recognition Result"_q));

		// Cover Art Widget
		const auto coverContainer = box->addRow(
			object_ptr<Ui::RpWidget>(box));
		coverContainer->resize(box->width(), 160);

		auto coverState = box->lifetime().make_state<QImage>();

		coverContainer->paintRequest() | rpl::on_next([=] {
			auto p = QPainter(coverContainer);
			auto hq = PainterHighQualityEnabler(p);
			const int size = 140;
			const QRect r((coverContainer->width() - size) / 2, 10, size, size);

			p.setPen(Qt::NoPen);
			p.setBrush(QColor(36, 36, 44));
			p.drawRoundedRect(r, 12, 12);

			if (!coverState->isNull()) {
				p.setClipPath([&] {
					QPainterPath path;
					path.addRoundedRect(r, 12, 12);
					return path;
				}());
				p.drawImage(r, *coverState);
			} else {
				p.setPen(QColor(180, 180, 190));
				p.setFont(style::font(13, 0, 0));
				p.drawText(r, Qt::AlignCenter, u"🎵"_q);
			}
		}, coverContainer->lifetime());

		if (!result.coverUrl.isEmpty()) {
			downloadCover(result.coverUrl, crl::guard(coverContainer, [=](QString local) {
				if (!local.isEmpty()) {
					coverState->load(local);
					coverContainer->update();
				}
			}));
		}

		// Title Label
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				rpl::single(result.title),
				st::boxLabel));

		// Artist Label
		if (!result.artist.isEmpty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					rpl::single(u"Artist: "_q + result.artist),
					st::boxLabel));
		}

		// Album Label
		if (!result.album.isEmpty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					rpl::single(u"Album: "_q + result.album),
					st::boxLabel));
		}

		// Action Buttons
		box->addButton(rpl::single(u"Search on YouTube"_q), [=] {
			QDesktopServices::openUrl(QUrl(result.searchUrl()));
		});

		box->addButton(rpl::single(u"Copy Info"_q), [=] {
			QGuiApplication::clipboard()->setText(result.formatted());
			Ui::Toast::Show(u"Copied to clipboard"_q);
		});

		box->addButton(tr::lng_close(), [=] {
			box->closeBox();
		});
	}));
}

void Service::showSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Music Recognition (SongRec / AudD)"_q));

		// Main Toggle
		const auto enabledCheckbox = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				u"Enable SongRec music recognition"_q,
				IsEnabled()));

		// Online high-res artwork toggle
		const auto artworkCheckbox = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				u"Search high-resolution album artwork online"_q,
				OnlineArtworkFallback()));

		// AudD API Section
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"AudD Music Recognition API (Optional token)"_q,
				st::boxLabel));

		const auto auddTokenField = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"AudD API Token"_q),
				AudDApiToken()));

		// Status / action
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				Service::Instance().isRecognizing()
					? u"Status: Listening / Identifying music..."_q
					: u"Status: Ready"_q,
				st::boxLabel));

		// Test Recognition Button
		box->addButton(rpl::single(u"Listen with Mic"_q), [=] {
			box->closeBox();
			RecognizeFromMic(controller);
		});

		// Save Settings Button
		box->addButton(tr::lng_settings_save(), [=] {
			SetEnabled(enabledCheckbox->checked());
			SetOnlineArtworkFallback(artworkCheckbox->checked());
			SetAudDApiToken(auddTokenField->getLastText().trimmed());
			box->closeBox();
		});

		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
}

// ------------------------------------------------------------------
// Global Helper Wrappers
// ------------------------------------------------------------------

void RecognizeFromMic(not_null<Window::SessionController*> controller) {
	Service::Instance().recognizeFromMic(controller);
}

void RecognizeCurrentPlayback(not_null<Window::SessionController*> controller) {
	Service::Instance().recognizeCurrentPlayback(controller);
}

void RecognizeFile(not_null<Window::SessionController*> controller, const QString &filePath) {
	Service::Instance().recognizeFile(controller, filePath);
}

void ShowResultBox(not_null<Window::SessionController*> controller, const TrackResult &result) {
	Service::Instance().showResultBox(controller, result);
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	Service::Instance().showSettingsBox(controller);
}

} // namespace Extera::SongRec
