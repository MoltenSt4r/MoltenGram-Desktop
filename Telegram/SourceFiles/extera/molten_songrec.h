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
#include <QtCore/QObject>
#include <optional>

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Extera::SongRec {

// Result of music recognition (Shazam / AudD)
struct TrackResult {
	QString title;
	QString artist;
	QString album;
	QString coverUrl;
	QString localCoverPath;
	QString songLink;
	int duration = 0;

	[[nodiscard]] bool empty() const {
		return title.trimmed().isEmpty();
	}

	[[nodiscard]] QString effectiveCover() const;
	[[nodiscard]] QString formatted() const;
	[[nodiscard]] QString searchUrl() const;
};

enum class Provider : int {
	Auto = 0,    // Shazam first, fallback to AudD
	Shazam = 1,  // Shazam SongRec acoustic fingerprint
	AudD = 2,    // AudD Music Recognition API
};

enum class Source : int {
	Microphone,
	Playback,
	File,
};

enum class State : int {
	Idle,
	Listening,
	Processing,
	Success,
	Failed,
};

// Configuration getters and setters
[[nodiscard]] bool IsEnabled();
void SetEnabled(bool enabled);
[[nodiscard]] Provider CurrentProvider();
void SetProvider(Provider provider);
[[nodiscard]] QString AudDApiToken();
void SetAudDApiToken(const QString &token);
[[nodiscard]] bool OnlineArtworkFallback();
void SetOnlineArtworkFallback(bool enabled);

// Recognition Service singleton
class Service final : public QObject {
public:
	[[nodiscard]] static Service &Instance();

	// Asynchronous recognition entry points
	void recognizeFromMic(
		not_null<Window::SessionController*> controller,
		Fn<void(std::optional<TrackResult>)> callback = nullptr);

	void recognizeCurrentPlayback(
		not_null<Window::SessionController*> controller,
		Fn<void(std::optional<TrackResult>)> callback = nullptr);

	void recognizeFile(
		not_null<Window::SessionController*> controller,
		const QString &filePath,
		Fn<void(std::optional<TrackResult>)> callback = nullptr);

	void recognizeAudioBuffer(
		not_null<Window::SessionController*> controller,
		const QByteArray &pcm16Mono,
		int sampleRate,
		Fn<void(std::optional<TrackResult>)> callback = nullptr);

	void cancel();
	[[nodiscard]] bool isRecognizing() const;
	[[nodiscard]] State state() const;
	[[nodiscard]] rpl::producer<State> stateChanges() const;

	// UI Dialogs
	void showResultBox(
		not_null<Window::SessionController*> controller,
		const TrackResult &result);

	void showSettingsBox(not_null<Window::SessionController*> controller);

	// Cover art download & metadata fallback
	void downloadCover(
		const QString &url,
		Fn<void(QString localPath)> callback);

	void fetchCoverByMetadata(
		const QString &artist,
		const QString &title,
		Fn<void(QString coverUrl)> callback);

	// Query endpoints
	void queryShazam(
		const QByteArray &signatureBytes,
		int sampleMs,
		Fn<void(std::optional<TrackResult>)> callback);

	void queryAudD(
		const QByteArray &wavData,
		Fn<void(std::optional<TrackResult>)> callback);

private:
	Service();
	~Service();

	void setState(State state);
	void finishWithResult(
		not_null<Window::SessionController*> controller,
		const std::optional<TrackResult> &result,
		Fn<void(std::optional<TrackResult>)> callback);

	[[nodiscard]] std::optional<TrackResult> readDiskCache(const QString &cacheKey);
	void writeDiskCache(const QString &cacheKey, const TrackResult &result);

	State _state = State::Idle;
	rpl::event_stream<State> _stateChanges;
	QByteArray _capturedBuffer;
	int _captureFrequency = 48000;
	bool _micListening = false;
	rpl::lifetime _lifetime;
};

// Global UI / Action helpers
void RecognizeFromMic(not_null<Window::SessionController*> controller);
void RecognizeCurrentPlayback(not_null<Window::SessionController*> controller);
void RecognizeFile(not_null<Window::SessionController*> controller, const QString &filePath);
void ShowResultBox(not_null<Window::SessionController*> controller, const TrackResult &result);
void ShowSettingsBox(not_null<Window::SessionController*> controller);
void SetupSettings(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Extera::SongRec
