/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include <QtCore/QString>
#include <QtCore/QChar>
#include <QtCore/QObject>
#include <optional>

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Extera::NowPlaying {

// Zero-Width Steganography Codec
// Wire-compatible with Android MoltenGram ZeroWidthCodec
namespace ZeroWidth {

inline constexpr QChar kMark = QChar(0x2064);       // Invisible Plus
inline constexpr QChar kOwnerMark = QChar(0xFEFF);  // Zero Width No-Break Space / BOM
inline constexpr QChar kAlphabet[4] = {
	QChar(0x2060), // Word Joiner (00)
	QChar(0x2061), // Function Application (01)
	QChar(0x2062), // Invisible Times (10)
	QChar(0x2063), // Invisible Separator (11)
};

[[nodiscard]] QString Strip(const QString &text);
[[nodiscard]] QString Encode(const QString &text);
[[nodiscard]] QString Decode(const QString &text);
[[nodiscard]] QString StampBio(const QString &originalBio, const QString &trackText, bool hidden = true);
[[nodiscard]] QString ExtractTrackFromBio(const QString &bio);

} // namespace ZeroWidth

// Last.fm & Media Player Track info
struct TrackInfo {
	QString title;
	QString artist;
	QString album;
	QString coverUrl;
	QString trackUrl;
	int duration = 0;
	bool isPlaying = false;

	[[nodiscard]] bool empty() const {
		return title.isEmpty();
	}
	[[nodiscard]] QString formatted() const;
};

enum class BroadcastTarget : int {
	None = 0,
	Bio = (1 << 0),
	EmojiStatus = (1 << 1),
	LastFm = (1 << 2),
	All = Bio | EmojiStatus | LastFm,
};

// Configuration getters/setters
[[nodiscard]] bool IsEnabled();
void SetEnabled(bool enabled);
[[nodiscard]] bool BroadcastToBioEnabled();
void SetBroadcastToBioEnabled(bool enabled);
[[nodiscard]] bool BroadcastToEmojiStatusEnabled();
void SetBroadcastToEmojiStatusEnabled(bool enabled);
[[nodiscard]] bool ZeroWidthHiddenEnabled();
void SetZeroWidthHiddenEnabled(bool hidden);
[[nodiscard]] QString LastFmUsername();
void SetLastFmUsername(const QString &user);
[[nodiscard]] QString LastFmSessionKey();
void SetLastFmSessionKey(const QString &key);
[[nodiscard]] QString LastFmApiKey();
void SetLastFmApiKey(const QString &key);

// Controller Service
class Service final {
public:
	[[nodiscard]] static Service &Instance();

	void init(not_null<Main::Session*> session);
	void updatePlayback(const TrackInfo &track, bool isPlaying);
	void stopPlayback();

	[[nodiscard]] const TrackInfo &currentTrack() const;
	[[nodiscard]] bool isBroadcasting() const;

	// Last.fm API methods
	void scrobbleLastFm(const TrackInfo &track);
	void updateLastFmNowPlaying(const TrackInfo &track);

	// Fetch Last.fm recent tracks
	void fetchLastFmRecent(
		const QString &username,
		Fn<void(std::optional<TrackInfo>)> callback);

	// Manual trigger
	void broadcastNow(const TrackInfo &track);
	void clearBroadcast();

private:
	Service();
	~Service();

	void onTrackStarted(const TrackInfo &track);
	void onTrackStopped();
	void applyToBio(const TrackInfo &track);
	void restoreBio();
	void applyToEmojiStatus(const TrackInfo &track);
	void restoreEmojiStatus();

	Main::Session *_session = nullptr;
	TrackInfo _currentTrack;
	bool _isBroadcasting = false;
	QString _originalBio;
	bool _bioSaved = false;
	uint64 _originalEmojiStatusId = 0;
	bool _emojiStatusSaved = false;
	rpl::lifetime _lifetime;
};

void ShowSettingsBox(not_null<Window::SessionController*> controller);
void SetupSettings(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Extera::NowPlaying
