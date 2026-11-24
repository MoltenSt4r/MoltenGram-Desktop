#include "extera/molten_playlists.h"

#include "base/random.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/file_location.h"
#include "core/file_utilities.h"
#include "data/data_audio_msg_id.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_peer_id.h"
#include "data/data_session.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/media_common.h"
#include "media/player/media_player_instance.h"
#include "settings.h"
#include "settings/settings_common.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTextStream>
#include <QtCore/QUuid>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Extera::Playlists {
namespace {

constexpr auto kChannelDialogBase = qint64(1000000000000LL);

[[nodiscard]] qint64 PeerIdToDialogId(PeerId peerId) {
	if (!peerId) {
		return 0;
	}
	if (peerIsUser(peerId)) {
		return qint64(peerToUser(peerId).bare);
	}
	if (peerIsChat(peerId)) {
		return -qint64(peerToChat(peerId).bare);
	}
	if (peerIsChannel(peerId)) {
		return -(kChannelDialogBase + qint64(peerToChannel(peerId).bare));
	}
	return 0;
}

[[nodiscard]] PeerId DialogIdToPeerId(qint64 dialogId) {
	if (dialogId > 0) {
		return peerFromUser(UserId(uint64(dialogId)));
	}
	if (dialogId < -kChannelDialogBase) {
		return peerFromChannel(
			ChannelId(uint64(-dialogId - kChannelDialogBase)));
	}
	if (dialogId < 0) {
		return peerFromChat(ChatId(uint64(-dialogId)));
	}
	return PeerId(0);
}

[[nodiscard]] QString SanitizeFileName(const QString &name) {
	static const auto invalid = QRegularExpression(u"[^a-zA-Z0-9\\x{0410}-\\x{044F}\\x{0401}\\x{0451}._-]"_q);
	auto safe = name;
	safe.replace(invalid, u"_"_q);
	safe = safe.trimmed();
	return safe.isEmpty() ? u"playlist"_q : safe;
}

[[nodiscard]] QString FormatDuration(int seconds) {
	const auto clamped = std::max(seconds, 0);
	return u"%1:%2"_q
		.arg(clamped / 60)
		.arg(clamped % 60, 2, 10, QChar('0'));
}

[[nodiscard]] QString TrackDisplayTitle(const PlaylistItem &item) {
	const auto title = item.title.isEmpty()
		? (item.fileName.isEmpty() ? u"Audio"_q : item.fileName)
		: item.title;
	if (!item.performer.isEmpty()) {
		return item.performer + u" — "_q + title;
	}
	return title;
}

[[nodiscard]] bool IsLocalPath(const QString &line) {
	if (line.startsWith(u"/"_q) || line.startsWith(u"file://"_q, Qt::CaseInsensitive)) {
		return true;
	}
	if (line.size() >= 3
		&& line[0].isLetter()
		&& line[1] == QChar(':')
		&& (line[2] == QChar('/') || line[2] == QChar('\\'))) {
		return true;
	}
	return false;
}

[[nodiscard]] AudioMsgId ResolveTrackAudioId(
		not_null<Main::Session*> session,
		const PlaylistItem &item) {
	auto &data = session->data();

	if (item.dialogId != 0 && item.messageId != 0) {
		const auto peerId = DialogIdToPeerId(item.dialogId);
		if (peerId) {
			const auto fullId = FullMsgId(peerId, MsgId(item.messageId));
			if (const auto msg = data.message(fullId)) {
				if (const auto media = msg->media()) {
					if (const auto doc = media->document()) {
						return AudioMsgId(doc, fullId);
					}
				}
			}
		}
	}

	if (item.documentId != 0) {
		const auto docId = DocumentId(uint64(item.documentId));
		auto attributes = QVector<MTPDocumentAttribute>();
		auto audioFlags = MTPDdocumentAttributeAudio::Flags(0);
		if (!item.title.isEmpty()) {
			audioFlags |= MTPDdocumentAttributeAudio::Flag::f_title;
		}
		if (!item.performer.isEmpty()) {
			audioFlags |= MTPDdocumentAttributeAudio::Flag::f_performer;
		}
		attributes.push_back(MTP_documentAttributeAudio(
			MTP_flags(audioFlags),
			MTP_int(item.duration),
			MTP_string(item.title),
			MTP_string(item.performer),
			MTP_bytes(QByteArray())));
		if (!item.fileName.isEmpty()) {
			attributes.push_back(MTP_documentAttributeFilename(
				MTP_string(item.fileName)));
		}

		DocumentData *doc = nullptr;
		if (item.accessHash != 0 && item.dcId != 0) {
			doc = data.document(
				docId,
				uint64(item.accessHash),
				item.fileReference,
				TimeId(0),
				attributes,
				item.mimeType.isEmpty() ? u"audio/mpeg"_q : item.mimeType,
				InlineImageLocation(),
				ImageWithLocation(),
				ImageWithLocation(),
				false,
				int32(item.dcId),
				int64(item.size));
		} else {
			doc = data.document(docId);
			if (doc->type == FileDocument && !attributes.isEmpty()) {
				doc->setattributes(attributes);
			}
		}

		if (doc) {
			if (!item.attachPath.isEmpty()
				&& QFileInfo::exists(item.attachPath)
				&& doc->filepath(true).isEmpty()) {
				doc->setLocation(Core::FileLocation(item.attachPath));
			}
			const auto peerId = DialogIdToPeerId(item.dialogId);
			const auto context = (peerId && item.messageId != 0)
				? FullMsgId(peerId, MsgId(item.messageId))
				: FullMsgId();
			return AudioMsgId(doc, context);
		}
	}

	auto localPath = item.attachPath;
	if (localPath.startsWith(u"file://"_q, Qt::CaseInsensitive)) {
		localPath = localPath.mid(7);
	}
	if (!localPath.isEmpty() && QFileInfo::exists(localPath)) {
		const auto syntheticId = DocumentId(
			0x7000000000000000ULL | uint64(qHash(localPath)));
		auto attributes = QVector<MTPDocumentAttribute>();
		auto audioFlags = MTPDdocumentAttributeAudio::Flags(0);
		if (!item.title.isEmpty()) {
			audioFlags |= MTPDdocumentAttributeAudio::Flag::f_title;
		}
		if (!item.performer.isEmpty()) {
			audioFlags |= MTPDdocumentAttributeAudio::Flag::f_performer;
		}
		attributes.push_back(MTP_documentAttributeAudio(
			MTP_flags(audioFlags),
			MTP_int(item.duration),
			MTP_string(item.title),
			MTP_string(item.performer),
			MTP_bytes(QByteArray())));
		const auto fname = item.fileName.isEmpty()
			? QFileInfo(localPath).fileName()
			: item.fileName;
		attributes.push_back(MTP_documentAttributeFilename(
			MTP_string(fname)));

		const auto doc = data.document(syntheticId);
		doc->setMimeString(
			item.mimeType.isEmpty() ? u"audio/mpeg"_q : item.mimeType);
		doc->size = QFileInfo(localPath).size();
		doc->setattributes(attributes);
		doc->setLocation(Core::FileLocation(localPath));
		return AudioMsgId(doc, FullMsgId());
	}

	return AudioMsgId();
}

struct ActiveQueue {
	base::weak_ptr<Main::Session> session;
	QString playlistId;
	std::vector<AudioMsgId> tracks;
	int index = -1;
	bool switching = false;
	rpl::lifetime lifetime;
};

[[nodiscard]] ActiveQueue &Queue() {
	static auto instance = ActiveQueue();
	return instance;
}

void ClearActiveQueue() {
	auto &queue = Queue();
	queue.lifetime.destroy();
	queue.session = nullptr;
	queue.playlistId.clear();
	queue.tracks.clear();
	queue.index = -1;
	queue.switching = false;
}

bool PlayQueueIndex(int targetIndex) {
	auto &queue = Queue();
	if (!queue.session
		|| queue.tracks.empty()
		|| targetIndex < 0
		|| targetIndex >= int(queue.tracks.size())) {
		return false;
	}
	queue.index = targetIndex;
	queue.switching = true;
	Media::Player::instance()->play(queue.tracks[targetIndex]);
	queue.switching = false;
	return true;
}

void InstallQueueHooks() {
	auto &queue = Queue();
	queue.lifetime.destroy();

	const auto player = Media::Player::instance();
	player->tracksFinished(
	) | rpl::filter(
		rpl::mappers::_1 == AudioMsgId::Type::Song
	) | rpl::on_next([] {
		auto &q = Queue();
		if (!q.session || q.tracks.empty() || q.index < 0) {
			ClearActiveQueue();
			return;
		}
		const auto &settings = Core::App().settings();
		const auto repeat = settings.playerRepeatMode();
		if (repeat == Media::RepeatMode::One) {
			PlayQueueIndex(q.index);
			return;
		}
		if (settings.playerOrderMode() == Media::OrderMode::Shuffle
			&& q.tracks.size() > 1) {
			auto next = q.index;
			for (auto attempt = 0; attempt < 8 && next == q.index; ++attempt) {
				next = base::RandomIndex(int(q.tracks.size()));
			}
			PlayQueueIndex(next);
			return;
		}
		const auto next = q.index + 1;
		if (next < int(q.tracks.size())) {
			PlayQueueIndex(next);
		} else if (repeat == Media::RepeatMode::All) {
			PlayQueueIndex(0);
		} else {
			ClearActiveQueue();
		}
	}, queue.lifetime);

	player->trackChanged(
	) | rpl::filter(
		rpl::mappers::_1 == AudioMsgId::Type::Song
	) | rpl::on_next([] {
		auto &q = Queue();
		if (!q.session) {
			ClearActiveQueue();
			return;
		}
		if (q.switching || q.tracks.empty()) {
			return;
		}
		const auto current = Media::Player::instance()->current(
			AudioMsgId::Type::Song);
		const auto it = std::find(
			q.tracks.begin(),
			q.tracks.end(),
			current);
		if (it == q.tracks.end()) {
			ClearActiveQueue();
		} else {
			q.index = int(std::distance(q.tracks.begin(), it));
		}
	}, queue.lifetime);
}

void ShowRenamePlaylistBox(
		not_null<Window::SessionController*> controller,
		const LocalPlaylist &playlist,
		Fn<void()> onDone) {
	const auto playlistId = playlist.id;
	const auto initialName = playlist.name;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Rename Playlist"_q));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Playlist name"_q),
			initialName));
		box->setFocusCallback([=] {
			field->setFocusFast();
		});
		const auto submit = [=] {
			const auto name = field->getLastText().trimmed();
			if (name.isEmpty()) {
				field->showError();
				return;
			}
			Store::Instance().renamePlaylist(playlistId, name);
			box->closeBox();
			if (onDone) {
				onDone();
			}
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->addButton(tr::lng_settings_save(), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace

bool PlaylistItem::isExternal() const {
	return source.compare(u"external"_q, Qt::CaseInsensitive) == 0;
}

bool PlaylistItem::isTelegram() const {
	return source.isEmpty()
		|| source.compare(u"telegram"_q, Qt::CaseInsensitive) == 0;
}

QJsonObject PlaylistItem::toJson() const {
	auto obj = QJsonObject();
	obj.insert(u"title"_q, title);
	obj.insert(u"performer"_q, performer);
	obj.insert(u"duration"_q, duration);
	obj.insert(u"dialogId"_q, dialogId);
	obj.insert(u"messageId"_q, messageId);
	obj.insert(u"docId"_q, documentId);
	obj.insert(u"accessHash"_q, accessHash);
	obj.insert(u"dcId"_q, dcId);
	obj.insert(u"size"_q, size);
	obj.insert(u"mimeType"_q, mimeType.isEmpty() ? u"audio/mpeg"_q : mimeType);
	obj.insert(u"fileName"_q, fileName);
	obj.insert(u"path"_q, attachPath);
	obj.insert(u"source"_q, source.isEmpty() ? u"telegram"_q : source);
	obj.insert(u"streamUrl"_q, streamUrl);
	obj.insert(u"coverUrl"_q, coverUrl);
	obj.insert(u"externalId"_q, externalId);
	if (!fileReference.isEmpty()) {
		obj.insert(
			u"fileRef"_q,
			QString::fromLatin1(fileReference.toBase64()));
	}
	return obj;
}

PlaylistItem PlaylistItem::FromJson(const QJsonObject &obj) {
	auto item = PlaylistItem();
	item.title = obj.value(u"title"_q).toString();
	item.performer = obj.value(u"performer"_q).toString();
	item.duration = int(obj.value(u"duration"_q).toInteger(0));
	item.dialogId = obj.value(u"dialogId"_q).toInteger(0);
	item.messageId = int(obj.value(u"messageId"_q).toInteger(0));
	item.documentId = obj.value(u"docId"_q).toInteger(0);
	item.accessHash = obj.value(u"accessHash"_q).toInteger(0);
	item.dcId = int(obj.value(u"dcId"_q).toInteger(0));
	item.size = obj.value(u"size"_q).toInteger(0);
	item.mimeType = obj.value(u"mimeType"_q).toString(u"audio/mpeg"_q);
	item.fileName = obj.value(u"fileName"_q).toString();
	item.attachPath = obj.value(u"path"_q).toString();
	item.source = obj.value(u"source"_q).toString(u"telegram"_q);
	item.streamUrl = obj.value(u"streamUrl"_q).toString();
	item.coverUrl = obj.value(u"coverUrl"_q).toString();
	item.externalId = obj.value(u"externalId"_q).toString();
	const auto fileRefStr = obj.value(u"fileRef"_q).toString();
	if (!fileRefStr.isEmpty()) {
		item.fileReference = QByteArray::fromBase64(fileRefStr.toLatin1());
	}
	return item;
}

PlaylistItem PlaylistItem::FromDocument(
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	auto item = PlaylistItem();
	if (const auto song = document->song()) {
		item.title = song->title;
		item.performer = song->performer;
	}
	if (item.title.isEmpty()) {
		item.title = document->filename();
		const auto dot = item.title.lastIndexOf(QChar('.'));
		if (dot > 0) {
			item.title = item.title.mid(0, dot);
		}
	}
	item.duration = int(document->duration() / 1000);
	item.dialogId = PeerIdToDialogId(contextId.peer);
	item.messageId = int(contextId.msg.bare);
	item.documentId = qint64(document->id);
	item.accessHash = document->mtpInput().match(
		[](const MTPDinputDocument &d) {
			return qint64(d.vaccess_hash().v);
		},
		[](const auto &) {
			return qint64(0);
		});
	item.dcId = int(document->exdDcId());
	item.size = qint64(document->size);
	item.mimeType = document->mimeString().isEmpty()
		? u"audio/mpeg"_q
		: document->mimeString();
	item.fileName = document->filename();
	item.attachPath = document->filepath(true);
	item.fileReference = document->fileReference();
	item.source = u"telegram"_q;
	return item;
}

QJsonObject LocalPlaylist::toJson() const {
	auto obj = QJsonObject();
	obj.insert(u"id"_q, id);
	obj.insert(u"name"_q, name);
	obj.insert(u"createdAt"_q, createdAt);
	auto arr = QJsonArray();
	for (const auto &track : tracks) {
		arr.append(track.toJson());
	}
	obj.insert(u"tracks"_q, arr);
	return obj;
}

LocalPlaylist LocalPlaylist::FromJson(const QJsonObject &obj) {
	auto pl = LocalPlaylist();
	pl.id = obj.value(u"id"_q).toString();
	if (pl.id.isEmpty()) {
		pl.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	pl.name = obj.value(u"name"_q).toString(u"Playlist"_q).trimmed();
	if (pl.name.isEmpty()) {
		pl.name = u"Playlist"_q;
	}
	pl.createdAt = obj.value(u"createdAt"_q).toInteger(
		QDateTime::currentMSecsSinceEpoch());
	const auto arr = obj.value(u"tracks"_q).toArray();
	pl.tracks.reserve(arr.size());
	for (const auto &entry : arr) {
		if (entry.isObject()) {
			pl.tracks.push_back(PlaylistItem::FromJson(entry.toObject()));
		}
	}
	return pl;
}

Store &Store::Instance() {
	static auto instance = Store();
	return instance;
}

QString Store::storagePath() const {
	if (!cWorkingDir().isEmpty()) {
		return cWorkingDir() + u"tdata/molten_playlists.json"_q;
	}
	return QStandardPaths::writableLocation(
		QStandardPaths::AppLocalDataLocation) + u"/molten_playlists.json"_q;
}

void Store::ensureLoaded() {
	if (!_loaded) {
		reload();
	}
}

void Store::reload() {
	_loaded = true;
	_playlists.clear();

	auto file = QFile(storagePath());
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isArray()) {
		return;
	}
	for (const auto &entry : doc.array()) {
		if (entry.isObject()) {
			_playlists.push_back(LocalPlaylist::FromJson(entry.toObject()));
		}
	}
}

void Store::save() {
	const auto path = storagePath();
	QDir().mkpath(QFileInfo(path).absolutePath());

	auto arr = QJsonArray();
	for (const auto &pl : _playlists) {
		arr.append(pl.toJson());
	}
	auto file = QSaveFile(path);
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
		file.commit();
	}
}

std::vector<LocalPlaylist> Store::playlists() {
	ensureLoaded();
	return _playlists;
}

std::optional<LocalPlaylist> Store::playlist(const QString &id) {
	ensureLoaded();
	for (const auto &pl : _playlists) {
		if (pl.id == id) {
			return pl;
		}
	}
	return std::nullopt;
}

LocalPlaylist Store::createPlaylist(const QString &name) {
	ensureLoaded();
	auto pl = LocalPlaylist{
		.id = QUuid::createUuid().toString(QUuid::WithoutBraces),
		.name = name.trimmed().isEmpty() ? u"Playlist"_q : name.trimmed(),
		.createdAt = QDateTime::currentMSecsSinceEpoch(),
	};
	_playlists.push_back(pl);
	save();
	_changed.fire({});
	return pl;
}

void Store::addPlaylist(LocalPlaylist playlist) {
	ensureLoaded();
	if (playlist.id.isEmpty()) {
		playlist.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	}
	std::erase_if(_playlists, [&](const LocalPlaylist &existing) {
		return existing.id == playlist.id;
	});
	_playlists.push_back(std::move(playlist));
	save();
	_changed.fire({});
}

void Store::deletePlaylist(const QString &id) {
	ensureLoaded();
	if (Queue().playlistId == id) {
		ClearActiveQueue();
	}
	const auto removed = std::erase_if(
		_playlists,
		[&](const LocalPlaylist &pl) { return pl.id == id; });
	if (removed > 0) {
		save();
		_changed.fire({});
	}
}

void Store::renamePlaylist(const QString &id, const QString &newName) {
	ensureLoaded();
	const auto trimmed = newName.trimmed();
	if (trimmed.isEmpty()) {
		return;
	}
	for (auto &pl : _playlists) {
		if (pl.id == id) {
			pl.name = trimmed;
			save();
			_changed.fire({});
			break;
		}
	}
}

bool Store::addTrack(const QString &playlistId, const PlaylistItem &item) {
	ensureLoaded();
	for (auto &pl : _playlists) {
		if (pl.id != playlistId) {
			continue;
		}
		for (const auto &existing : pl.tracks) {
			if (!item.externalId.isEmpty()
				&& item.externalId == existing.externalId) {
				return false;
			}
			if (!item.streamUrl.isEmpty()
				&& item.streamUrl == existing.streamUrl) {
				return false;
			}
			if (item.documentId != 0
				&& item.documentId == existing.documentId) {
				return false;
			}
			if (item.messageId != 0
				&& item.dialogId != 0
				&& item.messageId == existing.messageId
				&& item.dialogId == existing.dialogId) {
				return false;
			}
		}
		pl.tracks.push_back(item);
		save();
		_changed.fire({});
		return true;
	}
	return false;
}

bool Store::addTrack(
		const QString &playlistId,
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	return addTrack(playlistId, PlaylistItem::FromDocument(document, contextId));
}

void Store::removeTrack(const QString &playlistId, int index) {
	ensureLoaded();
	for (auto &pl : _playlists) {
		if (pl.id == playlistId) {
			if (index >= 0 && index < int(pl.tracks.size())) {
				pl.tracks.erase(pl.tracks.begin() + index);
				save();
				_changed.fire({});
			}
			break;
		}
	}
}

rpl::producer<> Store::changed() const {
	return _changed.events();
}

bool ExportJson(const LocalPlaylist &playlist, const QString &filePath) {
	auto file = QSaveFile(filePath);
	if (!file.open(QIODevice::WriteOnly)) {
		return false;
	}
	const auto data = QJsonDocument(playlist.toJson()).toJson(
		QJsonDocument::Indented);
	file.write(data);
	return file.commit();
}

bool ExportM3U8(const LocalPlaylist &playlist, const QString &filePath) {
	auto file = QSaveFile(filePath);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
		return false;
	}
	auto out = QTextStream(&file);
	out.setEncoding(QStringConverter::Utf8);
	out << u"#EXTM3U\n"_q;
	out << u"#EXT-X-PLAYLIST-NAME:"_q << playlist.name << u"\n"_q;
	for (const auto &track : playlist.tracks) {
		const auto artist = track.performer.isEmpty()
			? u"Unknown"_q
			: track.performer;
		const auto title = track.title.isEmpty()
			? u"Audio"_q
			: track.title;
		out << u"#EXTINF:"_q << track.duration << u","_q
			<< artist << u" - "_q << title << u"\n"_q;
		const auto meta = QString::fromUtf8(
			QJsonDocument(track.toJson()).toJson(QJsonDocument::Compact));
		out << u"#EXT-X-TG-META:"_q << meta << u"\n"_q;
		if (!track.attachPath.isEmpty()) {
			out << track.attachPath << u"\n"_q;
		} else if (!track.streamUrl.isEmpty()) {
			out << track.streamUrl << u"\n"_q;
		} else if (track.source.compare(u"youtube"_q, Qt::CaseInsensitive) == 0
			&& !track.externalId.isEmpty()) {
			out << u"https://music.youtube.com/watch?v="_q
				<< track.externalId << u"\n"_q;
		} else {
			out << u"https://t.me/c/"_q
				<< std::llabs(track.dialogId) << u"/"_q
				<< track.messageId << u"\n"_q;
		}
	}
	out.flush();
	return file.commit();
}

std::optional<LocalPlaylist> ImportFile(const QString &filePath) {
	auto file = QFile(filePath);
	if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
		return std::nullopt;
	}
	const auto info = QFileInfo(filePath);
	if (filePath.endsWith(u".m3u8"_q, Qt::CaseInsensitive)) {
		auto pl = LocalPlaylist{
			.id = QUuid::createUuid().toString(QUuid::WithoutBraces),
			.name = info.completeBaseName().isEmpty()
				? u"Playlist"_q
				: info.completeBaseName(),
			.createdAt = QDateTime::currentMSecsSinceEpoch(),
		};
		auto in = QTextStream(&file);
		in.setEncoding(QStringConverter::Utf8);
		auto current = std::optional<PlaylistItem>();
		while (!in.atEnd()) {
			const auto line = in.readLine().trimmed();
			if (line.startsWith(u"#EXT-X-PLAYLIST-NAME:"_q)) {
				const auto name = line.mid(21).trimmed();
				if (!name.isEmpty()) {
					pl.name = name;
				}
			} else if (line.startsWith(u"#EXTINF:"_q)) {
				auto item = PlaylistItem();
				const auto comma = line.indexOf(QChar(','));
				if (comma > 0) {
					item.duration = line.mid(8, comma - 8).trimmed().toInt();
					const auto artistTitle = line.mid(comma + 1).trimmed();
					const auto dash = artistTitle.indexOf(u" - "_q);
					if (dash > 0) {
						item.performer = artistTitle.mid(0, dash).trimmed();
						item.title = artistTitle.mid(dash + 3).trimmed();
					} else {
						item.title = artistTitle;
					}
				}
				current = std::move(item);
			} else if (line.startsWith(u"#EXT-X-TG-META:"_q)) {
				const auto jsonStr = line.mid(15).trimmed();
				const auto doc = QJsonDocument::fromJson(jsonStr.toUtf8());
				if (doc.isObject()) {
					current = PlaylistItem::FromJson(doc.object());
				}
			} else if (!line.startsWith(QChar('#')) && !line.isEmpty()) {
				if (current) {
					if (IsLocalPath(line)) {
						current->attachPath = line;
					} else if ((line.startsWith(u"http://"_q, Qt::CaseInsensitive)
						|| line.startsWith(u"https://"_q, Qt::CaseInsensitive))
						&& !line.contains(u"t.me/"_q, Qt::CaseInsensitive)
						&& current->streamUrl.isEmpty()) {
						current->streamUrl = line;
					}
					pl.tracks.push_back(std::move(*current));
					current.reset();
				}
			}
		}
		if (current) {
			pl.tracks.push_back(std::move(*current));
		}
		Store::Instance().addPlaylist(pl);
		return pl;
	}

	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isObject()) {
		return std::nullopt;
	}
	auto pl = LocalPlaylist::FromJson(doc.object());
	Store::Instance().addPlaylist(pl);
	return pl;
}

bool IsPlaylistFile(const QString &path) {
	return path.endsWith(u".mplaylist"_q, Qt::CaseInsensitive)
		|| path.endsWith(u".m3u8"_q, Qt::CaseInsensitive);
}

void PlayPlaylist(
		not_null<Window::SessionController*> controller,
		const QString &playlistId,
		int startIndex) {
	const auto pl = Store::Instance().playlist(playlistId);
	if (!pl || pl->tracks.empty()) {
		controller->showToast(u"Playlist is empty"_q);
		return;
	}

	auto resolved = std::vector<AudioMsgId>();
	auto mappedStart = -1;
	resolved.reserve(pl->tracks.size());
	for (auto i = 0; i != int(pl->tracks.size()); ++i) {
		const auto audioId = ResolveTrackAudioId(
			&controller->session(),
			pl->tracks[i]);
		if (audioId.audio()) {
			if (i == startIndex || (mappedStart < 0 && i >= startIndex)) {
				mappedStart = int(resolved.size());
			}
			resolved.push_back(audioId);
		}
	}

	if (resolved.empty()) {
		controller->showToast(u"No playable tracks in this playlist"_q);
		return;
	}
	if (mappedStart < 0 || mappedStart >= int(resolved.size())) {
		mappedStart = 0;
	}

	auto &queue = Queue();
	queue.session = base::make_weak(&controller->session());
	queue.playlistId = pl->id;
	queue.tracks = std::move(resolved);
	InstallQueueHooks();
	PlayQueueIndex(mappedStart);
}

bool HasActiveQueue() {
	auto &queue = Queue();
	if (!queue.session) {
		ClearActiveQueue();
		return false;
	}
	return !queue.tracks.empty() && queue.index >= 0;
}

bool PlayNext() {
	auto &queue = Queue();
	if (!queue.session) {
		ClearActiveQueue();
		return false;
	}
	if (queue.tracks.empty() || queue.index < 0) {
		return false;
	}
	const auto &settings = Core::App().settings();
	if (settings.playerOrderMode() == Media::OrderMode::Shuffle
		&& queue.tracks.size() > 1) {
		auto next = queue.index;
		for (auto attempt = 0; attempt < 8 && next == queue.index; ++attempt) {
			next = base::RandomIndex(int(queue.tracks.size()));
		}
		return PlayQueueIndex(next);
	}
	const auto next = (queue.index + 1) % int(queue.tracks.size());
	return PlayQueueIndex(next);
}

bool PlayPrevious() {
	auto &queue = Queue();
	if (!queue.session) {
		ClearActiveQueue();
		return false;
	}
	if (queue.tracks.empty() || queue.index < 0) {
		return false;
	}
	const auto prev = (queue.index - 1 + int(queue.tracks.size()))
		% int(queue.tracks.size());
	return PlayQueueIndex(prev);
}

void ShowNewPlaylistBox(
		not_null<Window::SessionController*> controller,
		Fn<void(const LocalPlaylist&)> onCreated) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"New Playlist"_q));
		const auto field = box->addRow(object_ptr<Ui::InputField>(
			box,
			st::defaultInputField,
			rpl::single(u"Playlist name"_q)));
		box->setFocusCallback([=] {
			field->setFocusFast();
		});
		const auto submit = [=] {
			auto name = field->getLastText().trimmed();
			if (name.isEmpty()) {
				name = u"Playlist"_q;
			}
			const auto created = Store::Instance().createPlaylist(name);
			controller->showToast(u"Playlist created"_q);
			box->closeBox();
			if (onCreated) {
				onCreated(created);
			}
		};
		field->submits() | rpl::on_next(submit, field->lifetime());
		box->addButton(tr::lng_create_group_create(), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowAddToPlaylistBox(
		not_null<Window::SessionController*> controller,
		not_null<DocumentData*> document,
		FullMsgId contextId) {
	const auto item = PlaylistItem::FromDocument(document, contextId);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Add to Playlist"_q));
		box->setWidth(st::boxWidth);

		const auto container = box->verticalLayout();
		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"+ New Playlist"_q),
			st::settingsButton,
			{ &st::menuIconAddToFolder }
		)->setClickedCallback([=] {
			box->closeBox();
			ShowNewPlaylistBox(controller, [=](const LocalPlaylist &pl) {
				Store::Instance().addTrack(pl.id, item);
				controller->showToast(u"Added to "_q + pl.name);
			});
		});

		Ui::AddDivider(container);

		const auto list = Store::Instance().playlists();
		if (list.empty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					rpl::single(u"No local playlists yet."_q),
					st::boxLabel),
				st::boxPadding);
		} else {
			for (const auto &pl : list) {
				const auto plId = pl.id;
				const auto plName = pl.name;
				const auto label = u"%1 (%2)"_q
					.arg(pl.name)
					.arg(pl.tracks.size());
				Settings::AddButtonWithIcon(
					container,
					rpl::single(label),
					st::settingsButton,
					{ &st::menuIconFave }
				)->setClickedCallback([=] {
					const auto added = Store::Instance().addTrack(plId, item);
					controller->showToast(added
						? (u"Added to "_q + plName)
						: u"Already in playlist"_q);
					box->closeBox();
				});
			}
		}

		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void ShowPlaylistDetailsBox(
		not_null<Window::SessionController*> controller,
		const QString &playlistId) {
	const auto maybe = Store::Instance().playlist(playlistId);
	if (!maybe) {
		return;
	}
	const auto pl = *maybe;
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(pl.name));
		box->setWidth(st::boxWideWidth);

		const auto container = box->verticalLayout();

		if (!pl.tracks.empty()) {
			Settings::AddButtonWithIcon(
				container,
				rpl::single(u"Play All (%1)"_q.arg(pl.tracks.size())),
				st::settingsButton,
				{ &st::menuIconFave }
			)->setClickedCallback([=] {
				PlayPlaylist(controller, playlistId, 0);
				box->closeBox();
			});
		}

		const auto weakBox = base::make_weak(box.get());
		const auto weakController = base::make_weak(controller);
		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"Export .mplaylist (JSON)"_q),
			st::settingsButton,
			{ &st::menuIconSaveImage }
		)->setClickedCallback([=] {
			const auto current = Store::Instance().playlist(playlistId);
			if (!current) {
				return;
			}
			FileDialog::GetWritePath(
				box.get(),
				u"Export Playlist (.mplaylist)"_q,
				u"MoltenGram Playlist (*.mplaylist)"_q,
				SanitizeFileName(current->name) + u".mplaylist"_q,
				[=](QString &&result) {
					if (result.isEmpty()) {
						return;
					}
					if (!result.endsWith(u".mplaylist"_q, Qt::CaseInsensitive)) {
						result += u".mplaylist"_q;
					}
					const auto ok = ExportJson(*current, result);
					if (const auto strong = weakController.get()) {
						strong->showToast(ok
							? u"Playlist exported"_q
							: u"Failed to export playlist"_q);
					}
				});
		});

		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"Export .m3u8"_q),
			st::settingsButton,
			{ &st::menuIconShare }
		)->setClickedCallback([=] {
			const auto current = Store::Instance().playlist(playlistId);
			if (!current) {
				return;
			}
			FileDialog::GetWritePath(
				box.get(),
				u"Export Playlist (.m3u8)"_q,
				u"M3U8 Playlist (*.m3u8)"_q,
				SanitizeFileName(current->name) + u".m3u8"_q,
				[=](QString &&result) {
					if (result.isEmpty()) {
						return;
					}
					if (!result.endsWith(u".m3u8"_q, Qt::CaseInsensitive)) {
						result += u".m3u8"_q;
					}
					const auto ok = ExportM3U8(*current, result);
					if (const auto strong = weakController.get()) {
						strong->showToast(ok
							? u"Playlist exported"_q
							: u"Failed to export playlist"_q);
					}
				});
		});

		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"Rename Playlist"_q),
			st::settingsButton,
			{ &st::menuIconEdit }
		)->setClickedCallback([=] {
			const auto current = Store::Instance().playlist(playlistId);
			if (!current) {
				return;
			}
			box->closeBox();
			ShowRenamePlaylistBox(controller, *current, [=] {
				if (const auto strong = weakController.get()) {
					ShowPlaylistDetailsBox(strong, playlistId);
				}
			});
		});

		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"Delete Playlist"_q),
			st::settingsButton,
			{ &st::menuIconDelete }
		)->setClickedCallback([=] {
			controller->show(Ui::MakeConfirmBox({
				.text = u"Delete playlist \"%1\"?"_q.arg(pl.name),
				.confirmed = [=](Fn<void()> close) {
					Store::Instance().deletePlaylist(playlistId);
					if (const auto strong = weakController.get()) {
						strong->showToast(u"Playlist deleted"_q);
					}
					close();
					if (weakBox) {
						weakBox->closeBox();
					}
				},
				.confirmText = tr::lng_box_delete(),
				.confirmStyle = &st::attentionBoxButton,
			}));
		});

		Ui::AddDivider(container);

		if (pl.tracks.empty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					rpl::single(u"No tracks in this playlist yet."_q),
					st::boxLabel),
				st::boxPadding);
		} else {
			for (auto i = 0; i != int(pl.tracks.size()); ++i) {
				const auto index = i;
				const auto &track = pl.tracks[i];
				const auto text = u"%1. %2 (%3)"_q
					.arg(i + 1)
					.arg(TrackDisplayTitle(track))
					.arg(FormatDuration(track.duration));
				const auto btn = Settings::AddButtonWithIcon(
					container,
					rpl::single(text),
					st::settingsButton);
				btn->setClickedCallback([=] {
					PlayPlaylist(controller, playlistId, index);
					box->closeBox();
				});
				btn->setAcceptBoth();
				btn->addClickHandler([=](Qt::MouseButton button) {
					if (button != Qt::RightButton) {
						return;
					}
					const auto menu = Ui::CreateChild<Ui::PopupMenu>(
						box.get(),
						st::popupMenuWithIcons);
					menu->addAction(u"Play"_q, [=] {
						PlayPlaylist(controller, playlistId, index);
						box->closeBox();
					}, &st::menuIconFave);
					menu->addAction(u"Remove from Playlist"_q, [=] {
						Store::Instance().removeTrack(playlistId, index);
						box->closeBox();
						ShowPlaylistDetailsBox(controller, playlistId);
					}, &st::menuIconDelete);
					menu->popup(QCursor::pos());
				});
			}
		}

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowPlaylistsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"My Playlists"_q));
		box->setWidth(st::boxWidth);

		const auto container = box->verticalLayout();

		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"+ New Playlist"_q),
			st::settingsButton,
			{ &st::menuIconAddToFolder }
		)->setClickedCallback([=] {
			box->closeBox();
			ShowNewPlaylistBox(controller, [=](const LocalPlaylist &) {
				ShowPlaylistsBox(controller);
			});
		});

		const auto weakBox = base::make_weak(box.get());
		const auto weakController = base::make_weak(controller);
		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"Import Playlist (.mplaylist / .m3u8)"_q),
			st::settingsButton,
			{ &st::menuIconDownload }
		)->setClickedCallback([=] {
			FileDialog::GetOpenPath(
				box.get(),
				u"Import Playlist"_q,
				u"Playlists (*.mplaylist *.m3u8)"_q,
				[=](FileDialog::OpenResult &&result) {
					if (result.paths.isEmpty()) {
						return;
					}
					const auto imported = ImportFile(result.paths.front());
					const auto strongController = weakController.get();
					if (imported) {
						if (strongController) {
							strongController->showToast(
								u"Imported \"%1\" (%2 tracks)"_q
									.arg(imported->name)
									.arg(imported->tracks.size()));
						}
						if (weakBox) {
							weakBox->closeBox();
						}
						if (strongController) {
							ShowPlaylistDetailsBox(strongController, imported->id);
						}
					} else if (strongController) {
						strongController->showToast(u"Failed to import playlist"_q);
					}
				});
		});

		Ui::AddDivider(container);

		const auto list = Store::Instance().playlists();
		if (list.empty()) {
			box->addRow(
				object_ptr<Ui::FlatLabel>(
					box,
					rpl::single(u"No local playlists yet."_q),
					st::boxLabel),
				st::boxPadding);
		} else {
			for (const auto &pl : list) {
				const auto plId = pl.id;
				const auto label = u"%1 (%2)"_q
					.arg(pl.name)
					.arg(pl.tracks.size());
				Settings::AddButtonWithIcon(
					container,
					rpl::single(label),
					st::settingsButton,
					{ &st::menuIconFave }
				)->setClickedCallback([=] {
					box->closeBox();
					ShowPlaylistDetailsBox(controller, plId);
				});
			}
		}

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void HandlePlaylistFileClick(
		not_null<Window::SessionController*> controller,
		const QString &path,
		const QString &fileName) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(
			fileName.isEmpty() ? QFileInfo(path).fileName() : fileName));
		box->setWidth(st::boxWidth);

		const auto container = box->verticalLayout();
		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"Import Playlist"_q),
			st::settingsButton,
			{ &st::menuIconDownload }
		)->setClickedCallback([=] {
			const auto pl = ImportFile(path);
			if (pl) {
				controller->showToast(
					u"Imported \"%1\" (%2 tracks)"_q
						.arg(pl->name)
						.arg(pl->tracks.size()));
			} else {
				controller->showToast(u"Failed to import playlist"_q);
			}
			box->closeBox();
		});

		Settings::AddButtonWithIcon(
			container,
			rpl::single(u"Import & Play"_q),
			st::settingsButton,
			{ &st::menuIconFave }
		)->setClickedCallback([=] {
			const auto pl = ImportFile(path);
			if (pl) {
				controller->showToast(
					u"Imported \"%1\" (%2 tracks)"_q
						.arg(pl->name)
						.arg(pl->tracks.size()));
				PlayPlaylist(controller, pl->id, 0);
			} else {
				controller->showToast(u"Failed to import playlist"_q);
			}
			box->closeBox();
		});

		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Extera::Playlists
