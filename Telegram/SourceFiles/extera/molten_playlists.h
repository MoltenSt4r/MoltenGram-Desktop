#pragma once

#include "data/data_msg_id.h"

#include <QtCore/QByteArray>
#include <QtCore/QJsonObject>
#include <QtCore/QString>

#include <optional>
#include <vector>

class DocumentData;

namespace Window {
class SessionController;
} // namespace Window

namespace Extera::Playlists {

struct PlaylistItem {
	QString title;
	QString performer;
	int duration = 0;
	qint64 dialogId = 0;
	int messageId = 0;
	qint64 documentId = 0;
	qint64 accessHash = 0;
	int dcId = 0;
	qint64 size = 0;
	QString mimeType = u"audio/mpeg"_q;
	QString fileName;
	QString attachPath;
	QByteArray fileReference;

	QString source = u"telegram"_q;
	QString streamUrl;
	QString coverUrl;
	QString externalId;

	[[nodiscard]] bool isExternal() const;
	[[nodiscard]] bool isTelegram() const;
	[[nodiscard]] QJsonObject toJson() const;
	[[nodiscard]] static PlaylistItem FromJson(const QJsonObject &obj);
	[[nodiscard]] static PlaylistItem FromDocument(
		not_null<DocumentData*> document,
		FullMsgId contextId);
};

struct LocalPlaylist {
	QString id;
	QString name;
	qint64 createdAt = 0;
	std::vector<PlaylistItem> tracks;

	[[nodiscard]] QJsonObject toJson() const;
	[[nodiscard]] static LocalPlaylist FromJson(const QJsonObject &obj);
};

class Store final {
public:
	[[nodiscard]] static Store &Instance();

	[[nodiscard]] std::vector<LocalPlaylist> playlists();
	[[nodiscard]] std::optional<LocalPlaylist> playlist(const QString &id);

	LocalPlaylist createPlaylist(const QString &name);
	void addPlaylist(LocalPlaylist playlist);
	void deletePlaylist(const QString &id);
	void renamePlaylist(const QString &id, const QString &newName);

	bool addTrack(const QString &playlistId, const PlaylistItem &item);
	bool addTrack(
		const QString &playlistId,
		not_null<DocumentData*> document,
		FullMsgId contextId);
	void removeTrack(const QString &playlistId, int index);

	[[nodiscard]] rpl::producer<> changed() const;

	void reload();
	void save();

private:
	Store() = default;

	void ensureLoaded();
	[[nodiscard]] QString storagePath() const;

	std::vector<LocalPlaylist> _playlists;
	rpl::event_stream<> _changed;
	bool _loaded = false;
};

[[nodiscard]] bool ExportJson(
	const LocalPlaylist &playlist,
	const QString &filePath);
[[nodiscard]] bool ExportM3U8(
	const LocalPlaylist &playlist,
	const QString &filePath);
[[nodiscard]] std::optional<LocalPlaylist> ImportFile(const QString &filePath);

[[nodiscard]] bool IsPlaylistFile(const QString &path);

void PlayPlaylist(
	not_null<Window::SessionController*> controller,
	const QString &playlistId,
	int startIndex = 0);
[[nodiscard]] bool HasActiveQueue();
bool PlayNext();
bool PlayPrevious();

void ShowAddToPlaylistBox(
	not_null<Window::SessionController*> controller,
	not_null<DocumentData*> document,
	FullMsgId contextId);
void ShowPlaylistsBox(not_null<Window::SessionController*> controller);
void ShowPlaylistDetailsBox(
	not_null<Window::SessionController*> controller,
	const QString &playlistId);
void ShowNewPlaylistBox(
	not_null<Window::SessionController*> controller,
	Fn<void(const LocalPlaylist&)> onCreated = nullptr);
void HandlePlaylistFileClick(
	not_null<Window::SessionController*> controller,
	const QString &path,
	const QString &fileName);

} // namespace Extera::Playlists
