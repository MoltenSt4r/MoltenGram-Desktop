/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_nowplaying.h"

#include "core/application.h"
#include "main/main_domain.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_emoji_statuses.h"
#include "apiwrap.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "extera/extera_config.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/player/media_player_instance.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Extera::NowPlaying {
namespace {

constexpr auto kConfigNowPlayingEnabled = "nowplaying_enabled";
constexpr auto kConfigBroadcastToBio = "nowplaying_to_bio";
constexpr auto kConfigBroadcastToEmoji = "nowplaying_to_emoji_status";
constexpr auto kConfigZeroWidthHidden = "nowplaying_zero_width_hidden";
constexpr auto kConfigLastFmUser = "nowplaying_lastfm_user";
constexpr auto kConfigLastFmSessionKey = "nowplaying_lastfm_session_key";
constexpr auto kConfigLastFmApiKey = "nowplaying_lastfm_api_key";
constexpr auto kConfigLastFmApiSecret = "nowplaying_lastfm_api_secret";

// Default Last.fm API Key for MoltenGram (can be overridden by user)
constexpr auto kDefaultLastFmApiKey = "b25b959554ed76058ac220b7b2e0a026";
constexpr auto kDefaultLastFmSecret = "425b0f4a86470f7e904bd25e15bf9429";

// Default standard animated music note emoji document ID
constexpr auto kDefaultMusicNoteDocId = DocumentId(5334751416972093240ULL);

[[nodiscard]] QNetworkAccessManager *NetworkManager() {
	static auto nam = std::make_unique<QNetworkAccessManager>();
	return nam.get();
}

} // namespace

namespace ZeroWidth {

QString Strip(const QString &text) {
	if (text.isEmpty()) {
		return QString();
	}
	auto result = QString();
	result.reserve(text.size());
	for (const auto &ch : text) {
		if (ch == kMark || ch == kOwnerMark) {
			continue;
		}
		if (ch >= kAlphabet[0] && ch <= kAlphabet[3]) {
			continue;
		}
		result.append(ch);
	}
	return result.trimmed();
}

QString Encode(const QString &text) {
	if (text.isEmpty()) {
		return QString();
	}
	const auto utf8 = text.toUtf8();
	auto encoded = QString();
	encoded.reserve(utf8.size() * 4 + 2);
	encoded.append(kMark);
	for (int i = 0; i < utf8.size(); ++i) {
		const auto byte = static_cast<uint8_t>(utf8[i]);
		encoded.append(kAlphabet[(byte >> 6) & 0x03]);
		encoded.append(kAlphabet[(byte >> 4) & 0x03]);
		encoded.append(kAlphabet[(byte >> 2) & 0x03]);
		encoded.append(kAlphabet[byte & 0x03]);
	}
	encoded.append(kMark);
	return encoded;
}

QString Decode(const QString &text) {
	if (text.isEmpty()) {
		return QString();
	}
	const auto firstMark = text.indexOf(kMark);
	const auto lastMark = text.lastIndexOf(kMark);
	QString payload;
	if (firstMark >= 0 && lastMark > firstMark) {
		payload = text.mid(firstMark + 1, lastMark - firstMark - 1);
	} else {
		payload = text;
	}

	QByteArray bytes;
	bytes.reserve(payload.size() / 4);
	uint8_t currentByte = 0;
	int bitCount = 0;

	for (const auto &ch : payload) {
		int val = -1;
		for (int i = 0; i < 4; ++i) {
			if (ch == kAlphabet[i]) {
				val = i;
				break;
			}
		}
		if (val >= 0) {
			currentByte = (currentByte << 2) | static_cast<uint8_t>(val);
			bitCount += 2;
			if (bitCount == 8) {
				bytes.append(static_cast<char>(currentByte));
				currentByte = 0;
				bitCount = 0;
			}
		}
	}
	return QString::fromUtf8(bytes);
}

QString StampBio(const QString &originalBio, const QString &trackText, bool hidden) {
	const auto cleanBio = Strip(originalBio);
	if (trackText.trimmed().isEmpty()) {
		return cleanBio;
	}
	if (hidden) {
		const auto encoded = Encode(trackText);
		if (cleanBio.isEmpty()) {
			return encoded;
		}
		return cleanBio + u" "_q + encoded;
	} else {
		const auto visibleNote = u"🎶 "_q + trackText;
		if (cleanBio.isEmpty()) {
			return visibleNote;
		}
		return cleanBio + u"\n"_q + visibleNote;
	}
}

QString ExtractTrackFromBio(const QString &bio) {
	if (bio.isEmpty()) {
		return QString();
	}
	const auto decoded = Decode(bio);
	if (!decoded.isEmpty()) {
		return decoded;
	}
	const auto noteIndex = bio.indexOf(u"🎶"_q);
	if (noteIndex >= 0) {
		return bio.mid(noteIndex + 2).trimmed();
	}
	return QString();
}

} // namespace ZeroWidth

QString TrackInfo::formatted() const {
	if (title.isEmpty()) {
		return QString();
	}
	if (artist.isEmpty()) {
		return title;
	}
	return artist + u" — "_q + title;
}

bool IsEnabled() {
	return Config::Instance().boolValue(kConfigNowPlayingEnabled);
}

void SetEnabled(bool enabled) {
	Config::Instance().setBool(kConfigNowPlayingEnabled, enabled);
	if (!enabled) {
		Service::Instance().clearBroadcast();
	}
}

bool BroadcastToBioEnabled() {
	return Config::Instance().boolValue(kConfigBroadcastToBio);
}

void SetBroadcastToBioEnabled(bool enabled) {
	Config::Instance().setBool(kConfigBroadcastToBio, enabled);
}

bool BroadcastToEmojiStatusEnabled() {
	return Config::Instance().boolValue(kConfigBroadcastToEmoji);
}

void SetBroadcastToEmojiStatusEnabled(bool enabled) {
	Config::Instance().setBool(kConfigBroadcastToEmoji, enabled);
}

bool ZeroWidthHiddenEnabled() {
	return Config::Instance().boolValue(kConfigZeroWidthHidden);
}

void SetZeroWidthHiddenEnabled(bool hidden) {
	Config::Instance().setBool(kConfigZeroWidthHidden, hidden);
}

QString LastFmUsername() {
	return Config::Instance().stringValue(kConfigLastFmUser);
}

void SetLastFmUsername(const QString &user) {
	Config::Instance().setString(kConfigLastFmUser, user);
}

QString LastFmSessionKey() {
	return Config::Instance().stringValue(kConfigLastFmSessionKey);
}

void SetLastFmSessionKey(const QString &key) {
	Config::Instance().setString(kConfigLastFmSessionKey, key);
}

QString LastFmApiKey() {
	const auto key = Config::Instance().stringValue(kConfigLastFmApiKey);
	return key.isEmpty() ? QString::fromUtf8(kDefaultLastFmApiKey) : key;
}

void SetLastFmApiKey(const QString &key) {
	Config::Instance().setString(kConfigLastFmApiKey, key);
}

// Service Implementation
Service &Service::Instance() {
	static Service instance;
	return instance;
}

Service::Service() {
	Core::App().domain().activeSessionValue(
	) | rpl::on_next([this](Main::Session *session) {
		_session = session;
	}, _lifetime);

	// Hook into player updates
	Media::Player::instance()->updatedNotifier(
	) | rpl::filter([](const Media::Player::TrackState &state) {
		return (state.id.type() == AudioMsgId::Type::Song);
	}) | rpl::on_next([this](const Media::Player::TrackState &state) {
		const auto isPlaying = Media::Player::ShowPauseIcon(state.state);
		if (!isPlaying) {
			stopPlayback();
			return;
		}

		const auto document = state.id.audio();
		if (!document) {
			stopPlayback();
			return;
		}

		TrackInfo info;
		info.isPlaying = true;
		info.duration = (state.frequency > 0 && state.length > 0)
			? int(state.length / state.frequency)
			: document->duration();

		if (const auto song = document->song()) {
			info.title = song->title;
			info.artist = song->performer;
		}
		if (info.title.isEmpty()) {
			info.title = document->filename();
			const auto dot = info.title.lastIndexOf('.');
			if (dot > 0) {
				info.title = info.title.mid(0, dot);
			}
		}
		if (info.artist.isEmpty()) {
			const auto split = info.title.indexOf(u" - "_q);
			if (split > 0) {
				info.artist = info.title.mid(0, split).trimmed();
				info.title = info.title.mid(split + 3).trimmed();
			}
		}

		updatePlayback(info, true);
	}, _lifetime);
}

Service::~Service() {
	clearBroadcast();
}

void Service::init(not_null<Main::Session*> session) {
	_session = session;
}

const TrackInfo &Service::currentTrack() const {
	return _currentTrack;
}

bool Service::isBroadcasting() const {
	return _isBroadcasting;
}

void Service::updatePlayback(const TrackInfo &track, bool isPlaying) {
	if (!IsEnabled()) {
		return;
	}
	if (!isPlaying || track.empty()) {
		stopPlayback();
		return;
	}
	if (_isBroadcasting
		&& _currentTrack.title == track.title
		&& _currentTrack.artist == track.artist) {
		return;
	}

	onTrackStarted(track);
}

void Service::stopPlayback() {
	if (_isBroadcasting) {
		onTrackStopped();
	}
}

void Service::broadcastNow(const TrackInfo &track) {
	if (track.empty()) {
		return;
	}
	onTrackStarted(track);
}

void Service::clearBroadcast() {
	if (_isBroadcasting) {
		onTrackStopped();
	}
}

void Service::onTrackStarted(const TrackInfo &track) {
	_currentTrack = track;
	_isBroadcasting = true;

	if (BroadcastToBioEnabled()) {
		applyToBio(track);
	}
	if (BroadcastToEmojiStatusEnabled()) {
		applyToEmojiStatus(track);
	}

	// Last.fm Now Playing scrobble
	if (!LastFmSessionKey().isEmpty()) {
		updateLastFmNowPlaying(track);
	}
}

void Service::onTrackStopped() {
	if (!_isBroadcasting) {
		return;
	}
	_isBroadcasting = false;

	if (BroadcastToBioEnabled()) {
		restoreBio();
	}
	if (BroadcastToEmojiStatusEnabled()) {
		restoreEmojiStatus();
	}

	// Scrobble on finish if session exists
	if (!LastFmSessionKey().isEmpty() && !_currentTrack.empty()) {
		scrobbleLastFm(_currentTrack);
	}

	_currentTrack = TrackInfo();
}

void Service::applyToBio(const TrackInfo &track) {
	if (!_session) {
		return;
	}
	const auto user = _session->user();
	if (!user) {
		return;
	}

	if (!_bioSaved) {
		_originalBio = user->about();
		_bioSaved = true;
	}

	const auto stamped = ZeroWidth::StampBio(
		_originalBio,
		track.formatted(),
		ZeroWidthHiddenEnabled());

	using Flag = MTPaccount_UpdateProfile::Flag;
	_session->api().request(MTPaccount_UpdateProfile(
		MTP_flags(Flag::f_about),
		MTPstring(),
		MTPstring(),
		MTP_string(stamped)
	)).done([=](const MTPUser &result) {
		_session->data().processUser(result);
		_session->user()->setAbout(stamped);
	}).send();
}

void Service::restoreBio() {
	if (!_session || !_bioSaved) {
		return;
	}
	_bioSaved = false;

	using Flag = MTPaccount_UpdateProfile::Flag;
	_session->api().request(MTPaccount_UpdateProfile(
		MTP_flags(Flag::f_about),
		MTPstring(),
		MTPstring(),
		MTP_string(_originalBio)
	)).done([=](const MTPUser &result) {
		_session->data().processUser(result);
		_session->user()->setAbout(_originalBio);
	}).send();
}

void Service::applyToEmojiStatus(const TrackInfo &track) {
	if (!_session) {
		return;
	}
	const auto user = _session->user();
	if (!user) {
		return;
	}

	if (!_emojiStatusSaved) {
		_originalEmojiStatusId = user->emojiStatusId().documentId;
		_emojiStatusSaved = true;
	}

	const auto noteStatus = MTP_emojiStatus(
		MTP_flags(0),
		MTP_long(kDefaultMusicNoteDocId),
		MTP_int(0));

	_session->api().request(MTPaccount_UpdateEmojiStatus(
		noteStatus
	)).send();
}

void Service::restoreEmojiStatus() {
	if (!_session || !_emojiStatusSaved) {
		return;
	}
	_emojiStatusSaved = false;

	const auto restoredStatus = (_originalEmojiStatusId == 0)
		? MTP_emojiStatusEmpty()
		: MTP_emojiStatus(
			MTP_flags(0),
			MTP_long(_originalEmojiStatusId),
			MTP_int(0));

	_session->api().request(MTPaccount_UpdateEmojiStatus(
		restoredStatus
	)).send();
}

void Service::updateLastFmNowPlaying(const TrackInfo &track) {
	const auto key = LastFmApiKey();
	const auto session = LastFmSessionKey();
	if (key.isEmpty() || session.isEmpty() || track.title.isEmpty()) {
		return;
	}

	auto params = std::vector<std::pair<QString, QString>>{
		{ u"api_key"_q, key },
		{ u"artist"_q, track.artist },
		{ u"method"_q, u"track.updateNowPlaying"_q },
		{ u"sk"_q, session },
		{ u"track"_q, track.title },
	};
	if (!track.album.isEmpty()) {
		params.push_back({ u"album"_q, track.album });
	}
	if (track.duration > 0) {
		params.push_back({ u"duration"_q, QString::number(track.duration) });
	}

	std::sort(params.begin(), params.end());
	auto sigSource = QString();
	for (const auto &[k, v] : params) {
		sigSource += k + v;
	}
	sigSource += QString::fromUtf8(kDefaultLastFmSecret);
	const auto apiSig = QString::fromUtf8(
		QCryptographicHash::hash(sigSource.toUtf8(), QCryptographicHash::Md5).toHex());

	QUrlQuery query;
	for (const auto &[k, v] : params) {
		query.addQueryItem(k, v);
	}
	query.addQueryItem(u"api_sig"_q, apiSig);
	query.addQueryItem(u"format"_q, u"json"_q);

	auto request = QNetworkRequest(QUrl(u"https://ws.audioscrobbler.com/2.0/"_q));
	request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_q);
	NetworkManager()->post(request, query.query(QUrl::FullyEncoded).toUtf8());
}

void Service::scrobbleLastFm(const TrackInfo &track) {
	const auto key = LastFmApiKey();
	const auto session = LastFmSessionKey();
	if (key.isEmpty() || session.isEmpty() || track.title.isEmpty()) {
		return;
	}

	const auto timestamp = QString::number(QDateTime::currentSecsSinceEpoch());
	auto params = std::vector<std::pair<QString, QString>>{
		{ u"api_key"_q, key },
		{ u"artist"_q, track.artist },
		{ u"method"_q, u"track.scrobble"_q },
		{ u"sk"_q, session },
		{ u"timestamp"_q, timestamp },
		{ u"track"_q, track.title },
	};
	if (!track.album.isEmpty()) {
		params.push_back({ u"album"_q, track.album });
	}

	std::sort(params.begin(), params.end());
	auto sigSource = QString();
	for (const auto &[k, v] : params) {
		sigSource += k + v;
	}
	sigSource += QString::fromUtf8(kDefaultLastFmSecret);
	const auto apiSig = QString::fromUtf8(
		QCryptographicHash::hash(sigSource.toUtf8(), QCryptographicHash::Md5).toHex());

	QUrlQuery query;
	for (const auto &[k, v] : params) {
		query.addQueryItem(k, v);
	}
	query.addQueryItem(u"api_sig"_q, apiSig);
	query.addQueryItem(u"format"_q, u"json"_q);

	auto request = QNetworkRequest(QUrl(u"https://ws.audioscrobbler.com/2.0/"_q));
	request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/x-www-form-urlencoded"_q);
	NetworkManager()->post(request, query.query(QUrl::FullyEncoded).toUtf8());
}

void Service::fetchLastFmRecent(
		const QString &username,
		Fn<void(std::optional<TrackInfo>)> callback) {
	if (username.isEmpty()) {
		if (callback) callback(std::nullopt);
		return;
	}

	const auto url = QUrl(u"https://www.last.fm/user/"_q
		+ QUrl::toPercentEncoding(username)
		+ u"/partial/recenttracks?ajax=1&page=1"_q);

	auto request = QNetworkRequest(url);
	request.setRawHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64; MoltenGram Desktop)");
	auto *reply = NetworkManager()->get(request);

	QObject::connect(reply, &QNetworkReply::finished, [reply, callback] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			if (callback) callback(std::nullopt);
			return;
		}

		const auto html = QString::fromUtf8(reply->readAll());
		const auto rowRegex = QRegularExpression(u"<tr[^>]*\\bchartlist-row\\b[^>]*>(.*?)</tr>"_q, QRegularExpression::DotMatchesEverythingOption);
		const auto match = rowRegex.match(html);
		if (!match.hasMatch()) {
			if (callback) callback(std::nullopt);
			return;
		}

		const auto row = match.captured(1);
		TrackInfo track;
		track.isPlaying = row.contains(u"chartlist-row--now-scrobbling"_q)
			|| row.contains(u"chartlist-now-scrobbling"_q);

		const auto trackRegex = QRegularExpression(u"data-track-name=\"([^\"]*)\""_q);
		const auto artistRegex = QRegularExpression(u"data-artist-name=\"([^\"]*)\""_q);
		const auto urlRegex = QRegularExpression(u"data-track-url=\"([^\"]*)\""_q);
		const auto coverRegex = QRegularExpression(u"src=\"(https://lastfm-img[^\"]*)\""_q);

		const auto tm = trackRegex.match(row);
		const auto am = artistRegex.match(row);
		const auto um = urlRegex.match(row);
		const auto cm = coverRegex.match(row);

		if (tm.hasMatch()) track.title = tm.captured(1);
		if (am.hasMatch()) track.artist = am.captured(1);
		if (um.hasMatch()) track.trackUrl = um.captured(1);
		if (cm.hasMatch()) track.coverUrl = cm.captured(1).replace(u"/64s/"_q, u"/300x300/"_q);

		if (track.title.isEmpty() || track.artist.isEmpty()) {
			if (callback) callback(std::nullopt);
		} else {
			if (callback) callback(track);
		}
	});
}

void SetupSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSubsectionTitle(container, rpl::single(u"Now Playing & Music Status"_q));

	const auto button = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Now Playing & Last.fm Broadcast"_q),
			st::settingsButtonNoIcon));

	button->setClickedCallback([=] {
		ShowSettingsBox(controller);
	});
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	Service::Instance().init(&controller->session());

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"Now Playing & Music Status"_q));
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

		const auto container = box->verticalLayout();

		// 1. Music Broadcast Section
		Ui::AddSubsectionTitle(container, rpl::single(u"Music Broadcast & Status"_q));

		const auto enabledCheckbox = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				u"Broadcast Currently Playing Music"_q,
				IsEnabled(),
				st::defaultCheckbox));

		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(u"Automatically detect active playback from media players and display it on your profile."_q),
				st::boxDividerLabel));

		const auto broadcastWrap = container->add(
			object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
				container,
				object_ptr<Ui::VerticalLayout>(container)));

		const auto broadcastOptions = broadcastWrap->entity();

		Ui::AddSkip(broadcastOptions, 8);
		const auto toBioCheckbox = broadcastOptions->add(
			object_ptr<Ui::Checkbox>(
				broadcastOptions,
				u"Broadcast Track to Profile Bio"_q,
				BroadcastToBioEnabled(),
				st::defaultCheckbox));

		broadcastOptions->add(
			object_ptr<Ui::FlatLabel>(
				broadcastOptions,
				rpl::single(u"Appends the currently playing track and artist to your profile Bio."_q),
				st::boxDividerLabel));

		Ui::AddSkip(broadcastOptions, 12);
		const auto hiddenCheckbox = broadcastOptions->add(
			object_ptr<Ui::Checkbox>(
				broadcastOptions,
				u"Hide Track in Bio with Zero-Width Codec (Invisible)"_q,
				ZeroWidthHiddenEnabled(),
				st::defaultCheckbox));

		broadcastOptions->add(
			object_ptr<Ui::FlatLabel>(
				broadcastOptions,
				rpl::single(u"Encodes track info using invisible Unicode symbols. Only MoltenGram users will see it in your Bio."_q),
				st::boxDividerLabel));

		Ui::AddSkip(broadcastOptions, 12);
		const auto toEmojiCheckbox = broadcastOptions->add(
			object_ptr<Ui::Checkbox>(
				broadcastOptions,
				u"Broadcast to Animated Emoji Status (🎵)"_q,
				BroadcastToEmojiStatusEnabled(),
				st::defaultCheckbox));

		broadcastOptions->add(
			object_ptr<Ui::FlatLabel>(
				broadcastOptions,
				rpl::single(u"Sets a dynamic musical note emoji status on your profile while playing."_q),
				st::boxDividerLabel));

		broadcastWrap->toggle(IsEnabled(), anim::type::instant);
		enabledCheckbox->checkedChanges(
		) | rpl::on_next([=](bool checked) {
			broadcastWrap->toggle(checked, anim::type::normal);
		}, enabledCheckbox->lifetime());

		// 2. Last.fm Integration Section
		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Last.fm Scrobbler Integration"_q));
		Ui::AddSkip(container, 8);

		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(u"Synchronize playback and scrobble tracks directly to your Last.fm account."_q),
				st::boxDividerLabel));

		Ui::AddSkip(container, 8);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(u"Last.fm Username:"_q),
				st::defaultFlatLabel));

		const auto lastFmUserField = container->add(
			object_ptr<Ui::InputField>(
				container,
				st::defaultInputField,
				rpl::single(u"Enter Last.fm username"_q),
				LastFmUsername()));

		Ui::AddSkip(container, 8);
		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(u"Last.fm Session Key (Optional):"_q),
				st::defaultFlatLabel));

		const auto lastFmKeyField = container->add(
			object_ptr<Ui::InputField>(
				container,
				st::defaultInputField,
				rpl::single(u"Enter session key or leave empty"_q),
				LastFmSessionKey()));

		// 3. Status Section
		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Current Playback Status"_q));
		Ui::AddSkip(container, 8);

		const auto statusText = Service::Instance().isBroadcasting()
			? (u"🎵 Broadcasting: "_q + Service::Instance().currentTrack().formatted())
			: u"⏸ Player is currently idle (no active playback)"_q;

		container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(statusText),
				st::defaultFlatLabel));

		// Action buttons
		box->addButton(tr::lng_settings_save(), [=] {
			SetEnabled(enabledCheckbox->checked());
			SetBroadcastToBioEnabled(toBioCheckbox->checked());
			SetZeroWidthHiddenEnabled(hiddenCheckbox->checked());
			SetBroadcastToEmojiStatusEnabled(toEmojiCheckbox->checked());
			SetLastFmUsername(lastFmUserField->getLastText().trimmed());
			SetLastFmSessionKey(lastFmKeyField->getLastText().trimmed());
			box->closeBox();
		});

		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
}

} // namespace Extera::NowPlaying
