#include "extera/extera_full_player.h"

#include "api/api_common.h"
#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/timer.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_origin.h"
#include "data/data_saved_music.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "extera/extera_config.h"
#include "extera/extera_plugins.h"
#include "extera/molten_playlists.h"
#include "extera/molten_songrec.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "media/audio/media_audio.h"
#include "media/media_common.h"
#include "media/player/media_player_instance.h"
#include "settings.h"
#include "styles/style_layers.h"
#include "styles/style_media_player.h"
#include "styles/style_menu_icons.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"
#include "ui/effects/animations.h"
#include "ui/image/image.h"
#include "ui/layers/box_content.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/style/style_core.h"
#include "ui/widgets/popup_menu.h"
#include "window/window_session_controller.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <cmath>
#include <numbers>

namespace Extera::FullPlayer {
namespace {

using Type = AudioMsgId::Type;

[[nodiscard]] int S(int value) {
	return style::ConvertScale(value);
}

struct Line {
	crl::time time = 0;
	QString text;
	int top = 0;
	int height = 0;
};

enum class Lyrics {
	Loading,
	Synced,
	Plain,
	None,
};

enum class Button {
	None,
	Previous,
	Play,
	Next,
	Slider,
	Line,
	Shuffle,
	Repeat,
	Playlists,
	ShowMessage,
	CopyLyrics,
	Heart,
};

constexpr auto kLongPressMs = crl::time(500);
constexpr auto kWaveTau = 2.0 * std::numbers::pi;

[[nodiscard]] bool ConfigBoolDefaultTrue(const QString &key) {
	const auto value = Extera::Config::Instance().valueOf(key);
	return value.isBool() ? value.toBool() : true;
}

[[nodiscard]] bool WavySeekbarEnabled() {
	return ConfigBoolDefaultTrue(u"player_wavy_seekbar"_q);
}

[[nodiscard]] bool OnlineCoverEnabled() {
	return ConfigBoolDefaultTrue(u"player_online_cover"_q);
}

[[nodiscard]] QString CoverCacheDir() {
	const auto base = cWorkingDir().isEmpty()
		? QStandardPaths::writableLocation(
			QStandardPaths::CacheLocation)
		: (cWorkingDir() + u"tdata"_q);
	return base + u"/molten_covers"_q;
}

[[nodiscard]] QString CoverCachePath(const QString &key) {
	const auto hash = QCryptographicHash::hash(
		key.toUtf8(),
		QCryptographicHash::Md5).toHex();
	return CoverCacheDir() + u"/"_q + QString::fromLatin1(hash) + u".jpg"_q;
}

void PaintHeartIcon(
		QPainter &p,
		QPoint center,
		float64 radius,
		const QColor &color,
		bool filled) {
	const auto cx = float64(center.x());
	const auto cy = float64(center.y());
	const auto s = radius;
	auto path = QPainterPath();
	path.moveTo(cx, cy + s * 0.78);
	path.cubicTo(
		cx - s * 1.15, cy + s * 0.05,
		cx - s * 1.05, cy - s * 0.95,
		cx, cy - s * 0.32);
	path.cubicTo(
		cx + s * 1.05, cy - s * 0.95,
		cx + s * 1.15, cy + s * 0.05,
		cx, cy + s * 0.78);
	path.closeSubpath();

	p.save();
	if (filled) {
		p.setPen(Qt::NoPen);
		p.setBrush(color);
		p.drawPath(path);
	} else {
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(color, std::max(1.5, radius * 0.19), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.drawPath(path);
	}
	p.restore();
}

[[nodiscard]] std::vector<Line> ParseSynced(const QString &text) {
	static const auto re = QRegularExpression(
		u"^\\[(\\d+):(\\d+(?:\\.\\d+)?)\\]\\s*(.*)$"_q);
	auto result = std::vector<Line>();
	for (const auto &line : text.split('\n')) {
		const auto match = re.match(line.trimmed());
		if (!match.hasMatch()) {
			continue;
		}
		result.push_back({
			.time = match.captured(1).toInt() * crl::time(60000)
				+ crl::time(match.captured(2).toDouble() * 1000),
			.text = match.captured(3).trimmed(),
		});
	}
	return result;
}

[[nodiscard]] QString CleanTitle(QString text) {
	static const auto brackets = QRegularExpression(
		u"\\s*[\\(\\[\\{][^\\)\\]\\}]*[\\)\\]\\}]"_q);
	static const auto feat = QRegularExpression(
		u"\\s+(feat\\.?|ft\\.?|featuring)\\s+.*$"_q,
		QRegularExpression::CaseInsensitiveOption);
	static const auto extra = QRegularExpression(
		u"\\s*-\\s*(official|lyrics?|audio|video|remaster).*$"_q,
		QRegularExpression::CaseInsensitiveOption);
	text.replace(brackets, QString());
	text.replace(feat, QString());
	text.replace(extra, QString());
	text.replace('_', ' ');
	return text.simplified();
}

[[nodiscard]] QString FirstArtist(const QString &artist) {
	static const auto split = QRegularExpression(
		u"\\s*(,|&|\\bx\\b|\\band\\b|;)\\s*"_q,
		QRegularExpression::CaseInsensitiveOption);
	const auto parts = artist.split(split, Qt::SkipEmptyParts);
	return parts.isEmpty() ? artist : parts.front().trimmed();
}

[[nodiscard]] QString FormatTime(crl::time ms) {
	const auto seconds = std::max(ms, crl::time(0)) / 1000;
	return u"%1:%2"_q
		.arg(seconds / 60)
		.arg(seconds % 60, 2, 10, QChar('0'));
}

[[nodiscard]] QColor Mix(QColor a, QColor b, float64 progress) {
	return QColor(
		anim::interpolate(a.red(), b.red(), progress),
		anim::interpolate(a.green(), b.green(), progress),
		anim::interpolate(a.blue(), b.blue(), progress));
}

struct ExdColors {
	bool md3 = false;
	QColor bg;
	QColor fg;
	QColor sub;
	QColor accent;
	QColor onAccent;
	QColor card;
	QColor raised;
	QColor tonal;
};

[[nodiscard]] ExdColors ExdPalette() {
	auto result = ExdColors();
	result.md3 = style::Md3Enabled();
	if (!result.md3) {
		return result;
	}
	const auto mix = [](QColor a, QColor b, float64 k) {
		return QColor(
			anim::interpolate(a.red(), b.red(), k),
			anim::interpolate(a.green(), b.green(), k),
			anim::interpolate(a.blue(), b.blue(), k));
	};
	result.bg = st::windowBg->c;
	result.fg = st::windowFg->c;
	result.sub = st::windowSubTextFg->c;
	result.accent = st::windowBgActive->c;
	result.onAccent = st::windowBg->c;
	result.card = mix(result.bg, result.fg, 0.055);
	result.raised = mix(result.bg, result.fg, 0.08);
	result.tonal = mix(result.bg, result.accent, 0.22);
	return result;
}

[[nodiscard]] QColor Darken(QColor color, float64 amount) {
	return QColor(
		int(color.red() * amount),
		int(color.green() * amount),
		int(color.blue() * amount));
}

class View final : public Ui::RpWidget {
public:
	View(
		QWidget *parent,
		not_null<Window::SessionController*> controller);

protected:
	int resizeGetHeight(int newWidth) override;
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	void loadTrack();
	base::unique_qptr<Ui::PopupMenu> _pluginMenu;
	void checkCover();
	void applyCover(QImage image);
	void fetchOnlineCover();
	void fetchDeezerCover(int requestId, const QString &term, const QString &cachePath);
	void downloadOnlineCover(int requestId, const QUrl &url, const QString &cachePath);
	void toggleProfileMusic();
	void saveToSavedMessages();
	void updateWaveTimer();
	[[nodiscard]] bool isSavedToProfile() const;
	void requestLyrics();
	void tryLyrics(
		int requestId,
		std::vector<std::pair<QUrl, bool>> urls,
		int index,
		int attempt = 0);
	void tryNetEase(int requestId);
	void loadNetEaseLyric(int requestId, qint64 id);
	bool applyLyrics(const QJsonObject &object);
	void layoutLines();
	void updateCurrentLine();
	void seekTo(float64 progress);
	void paintControls(QPainter &p);
	void paintControlsMd3(QPainter &p);
	void paintLyrics(QPainter &p);
	[[nodiscard]] Button buttonAt(QPoint point, int *line = nullptr) const;
	[[nodiscard]] crl::time position() const;
	[[nodiscard]] crl::time length() const;
	[[nodiscard]] QRect coverRect() const;
	[[nodiscard]] QRect sliderRect() const;
	[[nodiscard]] QPoint buttonCenter(Button button) const;
	[[nodiscard]] QRect lyricsRect() const;
	[[nodiscard]] QRect lyricsCardRect() const;

	const not_null<Window::SessionController*> _controller;
	const int _height = 0;
	const style::font _titleFont;
	const style::font _subtitleFont;
	const style::font _timeFont;
	const style::font _lineFont;
	const style::font _currentFont;

	DocumentData *_document = nullptr;
	FullMsgId _context;
	std::shared_ptr<Data::DocumentMedia> _media;
	QString _title;
	QString _performer;
	QImage _cover;
	QImage _backdrop;
	QColor _top;
	QColor _bottom;
	QColor _wasTop;
	QColor _wasBottom;
	Ui::Animations::Simple _colors;

	Media::Player::TrackState _state;
	bool _seeking = false;
	float64 _seekValue = 0.;
	Button _over = Button::None;
	Button _pressedButton = Button::None;
	bool _longPressTriggered = false;
	base::Timer _longPressTimer;

	QTimer _waveTimer;
	QElapsedTimer _waveClock;
	float64 _waveAmplitude = 0.;
	float64 _wavePhase = 0.;

	QNetworkAccessManager *_network = nullptr;
	int _requestId = 0;
	int _coverRequestId = 0;
	bool _onlineCoverRequested = false;
	Lyrics _lyrics = Lyrics::Loading;
	std::vector<Line> _lines;
	int _linesWidth = 0;
	int _linesHeight = 0;
	int _current = -1;
	float64 _scroll = 0.;
	float64 _scrollFrom = 0.;
	float64 _scrollTo = 0.;
	Ui::Animations::Simple _scrollAnimation;

};

View::View(
	QWidget *parent,
	not_null<Window::SessionController*> controller)
: RpWidget(parent)
, _controller(controller)
, _height(S(560))
, _titleFont(S(20), style::FontFlag::Bold, 0)
, _subtitleFont(S(15), style::FontFlags(), 0)
, _timeFont(S(12), style::FontFlags(), 0)
, _lineFont(
	S(17),
	style::Md3Enabled()
		? style::FontFlags()
		: style::FontFlags(style::FontFlag::Bold),
	0)
, _currentFont(S(style::Md3Enabled() ? 21 : 19), style::FontFlag::Bold, 0)
, _top(QColor(40, 40, 48))
, _bottom(QColor(18, 18, 22))
, _wasTop(_top)
, _wasBottom(_bottom)
, _longPressTimer([=] {
	if (_pressedButton == Button::Heart) {
		_longPressTriggered = true;
		saveToSavedMessages();
	} else if (_pressedButton == Button::Playlists) {
		_longPressTriggered = true;
		Extera::Playlists::ShowPlaylistsBox(_controller);
	}
})
, _network(new QNetworkAccessManager(this)) {
	setMouseTracking(true);

	_waveTimer.setInterval(16);
	connect(&_waveTimer, &QTimer::timeout, this, [=] {
		const auto dt = _waveClock.isValid()
			? std::min(0.05, _waveClock.restart() / 1000.0)
			: 0.016;
		const auto playing = Media::Player::ShowPauseIcon(_state.state);
		const auto isWavy = style::Md3Enabled() && WavySeekbarEnabled();
		const auto maxAmp = float64(S(4));
		const auto targetAmp = (playing && isWavy) ? maxAmp : 0.0;
		_waveAmplitude += (targetAmp - _waveAmplitude)
			* (1.0 - std::pow(0.85, dt / 0.04));
		if (std::abs(_waveAmplitude - targetAmp) < 0.05) {
			_waveAmplitude = targetAmp;
		}
		if (_waveAmplitude > 0.0) {
			_wavePhase = std::fmod(_wavePhase + 6.0 * dt, kWaveTau);
		}
		update(sliderRect().marginsAdded(QMargins(S(8), S(16), S(8), S(16))));
		if (_waveAmplitude == 0.0 && targetAmp == 0.0) {
			_waveTimer.stop();
			_waveClock.invalidate();
		}
	});

	const auto instance = Media::Player::instance();
	instance->updatedNotifier(
	) | rpl::filter([](const Media::Player::TrackState &state) {
		return state.id.type() == Type::Song;
	}) | rpl::on_next([=](const Media::Player::TrackState &state) {
		_state = state;
		if (state.id.audio() != _document) {
			loadTrack();
		}
		updateWaveTimer();
		updateCurrentLine();
		update();
	}, lifetime());

	instance->trackChanged(
	) | rpl::filter(
		rpl::mappers::_1 == Type::Song
	) | rpl::on_next([=] {
		loadTrack();
	}, lifetime());

	controller->session().downloaderTaskFinished(
	) | rpl::on_next([=] {
		checkCover();
	}, lifetime());

	auto &savedMusic = controller->session().data().savedMusic();
	savedMusic.loadIds();
	savedMusic.changed(
	) | rpl::on_next([=] {
		update();
	}, lifetime());

	_state = instance->getState(Type::Song);
	loadTrack();
	updateWaveTimer();
}

int View::resizeGetHeight(int newWidth) {
	_linesWidth = 0;
	return _height;
}

crl::time View::length() const {
	if (_state.length > 0 && _state.frequency > 0) {
		return _state.length * 1000 / _state.frequency;
	}
	return _document ? _document->duration() : 0;
}

crl::time View::position() const {
	if (_seeking) {
		return crl::time(_seekValue * length());
	}
	return (_state.frequency > 0)
		? (_state.position * 1000 / _state.frequency)
		: 0;
}

void View::loadTrack() {
	const auto current = Media::Player::instance()->current(Type::Song);
	const auto document = current.audio();
	if (document == _document && current.contextId() == _context) {
		return;
	}
	_document = document;
	_context = current.contextId();
	_cover = QImage();
	_backdrop = QImage();
	_media = nullptr;
	_title = QString();
	_performer = QString();
	if (const auto song = document ? document->song() : nullptr) {
		_title = song->title;
		_performer = song->performer;
	}
	if (document && _title.isEmpty()) {
		_title = document->filename();
		const auto dot = _title.lastIndexOf('.');
		if (dot > 0) {
			_title = _title.mid(0, dot);
		}
	}
	if (_performer.isEmpty()) {
		const auto split = _title.indexOf(u" - "_q);
		if (split > 0) {
			_performer = _title.mid(0, split).trimmed();
			_title = _title.mid(split + 3).trimmed();
		}
	}
	if (document) {
		_media = document->createMediaView();
		_media->thumbnailWanted(Data::FileOriginMessage(_context));
	}
	++_coverRequestId;
	_onlineCoverRequested = false;
	applyCover(QImage());
	checkCover();
	if (_cover.isNull()
		&& (!document || !document->hasThumbnail())) {
		fetchOnlineCover();
	}
	requestLyrics();
	updateWaveTimer();
	update();
}

void View::checkCover() {
	if (!_media || !_cover.isNull()) {
		return;
	}
	if (const auto image = _media->thumbnail()) {
		applyCover(image->original());
	} else if (_document
		&& !_document->thumbnailLoading()
		&& !_onlineCoverRequested) {
		fetchOnlineCover();
	}
}

void View::fetchOnlineCover() {
	if (_onlineCoverRequested || !OnlineCoverEnabled() || _title.isEmpty()) {
		return;
	}
	_onlineCoverRequested = true;
	const auto artist = CleanTitle(_performer);
	const auto title = CleanTitle(_title);
	const auto term = (artist.isEmpty() ? title : (artist + u" "_q + title)).trimmed();
	if (term.isEmpty()) {
		return;
	}
	const auto cachePath = CoverCachePath(term.toLower());
	if (QFile::exists(cachePath)) {
		if (auto cached = QImage(cachePath); !cached.isNull()) {
			applyCover(std::move(cached));
			return;
		}
	}

	const auto requestId = ++_coverRequestId;
	auto query = QUrlQuery();
	query.addQueryItem(u"term"_q, term);
	query.addQueryItem(u"media"_q, u"music"_q);
	query.addQueryItem(u"entity"_q, u"song"_q);
	query.addQueryItem(u"limit"_q, u"1"_q);
	auto url = QUrl(u"https://itunes.apple.com/search"_q);
	url.setQuery(query);
	auto request = QNetworkRequest(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, u"Mozilla/5.0"_q);
	const auto reply = _network->get(request);
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (requestId != _coverRequestId || !_cover.isNull()) {
			return;
		}
		const auto results = QJsonDocument::fromJson(reply->readAll()).object()
			.value(u"results"_q).toArray();
		if (!results.isEmpty()) {
			auto art = results.first().toObject()
				.value(u"artworkUrl100"_q).toString();
			if (!art.isEmpty()) {
				art.replace(u"100x100bb"_q, u"600x600bb"_q);
				downloadOnlineCover(requestId, QUrl(art), cachePath);
				return;
			}
		}
		fetchDeezerCover(requestId, term, cachePath);
	});
}

void View::fetchDeezerCover(
		int requestId,
		const QString &term,
		const QString &cachePath) {
	auto query = QUrlQuery();
	query.addQueryItem(u"q"_q, term);
	query.addQueryItem(u"limit"_q, u"1"_q);
	auto url = QUrl(u"https://api.deezer.com/search"_q);
	url.setQuery(query);
	auto request = QNetworkRequest(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, u"Mozilla/5.0"_q);
	const auto reply = _network->get(request);
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (requestId != _coverRequestId || !_cover.isNull()) {
			return;
		}
		const auto data = QJsonDocument::fromJson(reply->readAll()).object()
			.value(u"data"_q).toArray();
		if (!data.isEmpty()) {
			const auto art = data.first().toObject()
				.value(u"album"_q).toObject()
				.value(u"cover_xl"_q).toString();
			if (!art.isEmpty()) {
				downloadOnlineCover(requestId, QUrl(art), cachePath);
			}
		}
	});
}

void View::downloadOnlineCover(
		int requestId,
		const QUrl &url,
		const QString &cachePath) {
	if (!url.isValid()) {
		return;
	}
	auto request = QNetworkRequest(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, u"Mozilla/5.0"_q);
	const auto reply = _network->get(request);
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (requestId != _coverRequestId
			|| !_cover.isNull()
			|| reply->error() != QNetworkReply::NoError) {
			return;
		}
		const auto bytes = reply->readAll();
		auto image = QImage();
		if (!image.loadFromData(bytes) || image.isNull()) {
			return;
		}
		QDir().mkpath(CoverCacheDir());
		auto save = QSaveFile(cachePath);
		if (save.open(QIODevice::WriteOnly)) {
			save.write(bytes);
			save.commit();
		}
		applyCover(std::move(image));
	});
}

bool View::isSavedToProfile() const {
	return _document
		&& _controller->session().data().savedMusic().has(_document);
}

void View::toggleProfileMusic() {
	if (!_document) {
		return;
	}
	auto &savedMusic = _controller->session().data().savedMusic();
	if (savedMusic.has(_document)) {
		savedMusic.remove(_document);
		_controller->showToast(u"Removed from profile music"_q);
	} else {
		savedMusic.save(_document, Data::FileOriginMessage(_context));
		_controller->showToast(u"Saved to profile music"_q);
	}
	update();
}

void View::saveToSavedMessages() {
	auto &session = _controller->session();
	const auto item = session.data().message(_context);
	if (!item) {
		_controller->showToast(u"Cannot save this track to Saved Messages"_q);
		return;
	}
	if (item->forbidsForward()) {
		_controller->showToast(u"Forwarding is restricted for this track"_q);
		return;
	}
	const auto savedHistory = session.data().history(session.user());
	session.api().forwardMessages(
		Data::ResolvedForwardDraft{
			.items = { item },
			.options = Data::ForwardOptions::PreserveInfo,
		},
		Api::SendAction(savedHistory));
	_controller->showToast(u"Saved to Saved Messages"_q);
}

void View::updateWaveTimer() {
	const auto playing = Media::Player::ShowPauseIcon(_state.state);
	const auto isWavy = style::Md3Enabled() && WavySeekbarEnabled();
	if ((playing && isWavy) || _waveAmplitude > 0.0) {
		if (!_waveTimer.isActive()) {
			_waveClock.start();
			_waveTimer.start();
		}
	}
}

void View::applyCover(QImage image) {
	_wasTop = _top;
	_wasBottom = _bottom;
	if (image.isNull()) {
		_top = QColor(52, 52, 64);
		_bottom = QColor(18, 18, 24);
	} else {
		_cover = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
		const auto tiny = _cover.scaled(
			2,
			2,
			Qt::IgnoreAspectRatio,
			Qt::SmoothTransformation);
		_top = Darken(tiny.pixelColor(0, 0), 0.72);
		_bottom = Darken(tiny.pixelColor(1, 1), 0.38);
		_backdrop = _cover.scaled(
			12,
			12,
			Qt::KeepAspectRatioByExpanding,
			Qt::SmoothTransformation);
	}
	_colors.stop();
	_colors.start([=] { update(); }, 0., 1., 450, anim::easeOutCirc);
}

void View::requestLyrics() {
	const auto requestId = ++_requestId;
	_lyrics = Lyrics::Loading;
	_lines.clear();
	_linesWidth = 0;
	_current = -1;
	_scroll = _scrollFrom = _scrollTo = 0.;
	if (!ConfigBoolDefaultTrue(u"player_lrclib_lyrics"_q) || _title.isEmpty()) {
		_lyrics = Lyrics::None;
		return;
	}
	const auto title = CleanTitle(_title);
	const auto artist = CleanTitle(_performer);
	const auto firstArtist = FirstArtist(artist);
	const auto duration = int(length() / 1000);
	auto urls = std::vector<std::pair<QUrl, bool>>();
	const auto add = [&](
			const QString &path,
			std::vector<std::pair<QString, QString>> items,
			bool single) {
		auto query = QUrlQuery();
		for (const auto &[key, value] : items) {
			if (!value.isEmpty()) {
				query.addQueryItem(key, value);
			}
		}
		auto url = QUrl(u"https://lrclib.net/api/"_q + path);
		url.setQuery(query);
		urls.emplace_back(url, single);
	};
	if (!artist.isEmpty()) {
		add(u"get"_q, {
			{ u"track_name"_q, title },
			{ u"artist_name"_q, artist },
			{ u"duration"_q, duration ? QString::number(duration) : QString() },
		}, true);
		add(u"search"_q, {
			{ u"track_name"_q, title },
			{ u"artist_name"_q, firstArtist },
		}, false);
		add(u"search"_q, { { u"q"_q, firstArtist + ' ' + title } }, false);
	}
	add(u"search"_q, { { u"track_name"_q, title } }, false);
	add(u"search"_q, { { u"q"_q, _title } }, false);
	tryLyrics(requestId, std::move(urls), 0);
}

void View::tryLyrics(
		int requestId,
		std::vector<std::pair<QUrl, bool>> urls,
		int index,
		int attempt) {
	if (index >= int(urls.size())) {
		tryNetEase(requestId);
		return;
	}
	auto request = QNetworkRequest(urls[index].first);
	request.setHeader(QNetworkRequest::UserAgentHeader, u"MoltenGram-Desktop"_q);
	const auto single = urls[index].second;
	const auto reply = _network->get(request);
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (requestId != _requestId) {
			return;
		}
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto failed = (status == 503 || status == 429 || status == 0)
			&& reply->error() != QNetworkReply::NoError;
		if (failed && attempt < 4) {
			base::call_delayed(1500 * (attempt + 1), this, [=] {
				if (requestId == _requestId) {
					tryLyrics(requestId, urls, index, attempt + 1);
				}
			});
			return;
		}
		const auto document = QJsonDocument::fromJson(reply->readAll());
		if (single) {
			if (applyLyrics(document.object())) {
				return;
			}
		} else {
			const auto duration = length() / 1000;
			auto best = QJsonObject();
			auto bestScore = -1;
			for (const auto &entry : document.array()) {
				const auto object = entry.toObject();
				const auto synced = !object.value(
					u"syncedLyrics"_q).toString().isEmpty();
				const auto plain = !object.value(
					u"plainLyrics"_q).toString().isEmpty();
				if (!synced && !plain) {
					continue;
				}
				const auto diff = duration
					? std::abs(object.value(u"duration"_q).toDouble()
						- duration)
					: 0.;
				if (duration && diff > 15.) {
					continue;
				}
				const auto score = (synced ? 100 : 0)
					+ (diff <= 3. ? 50 : diff <= 8. ? 20 : 0);
				if (score > bestScore) {
					bestScore = score;
					best = object;
				}
			}
			if (bestScore >= 0 && applyLyrics(best)) {
				return;
			}
		}
		tryLyrics(requestId, urls, index + 1);
	});
}

void View::tryNetEase(int requestId) {
	auto query = QUrlQuery();
	query.addQueryItem(
		u"s"_q,
		(CleanTitle(_performer) + ' ' + CleanTitle(_title)).trimmed());
	query.addQueryItem(u"type"_q, u"1"_q);
	query.addQueryItem(u"limit"_q, u"8"_q);
	auto url = QUrl(u"https://music.163.com/api/search/get"_q);
	url.setQuery(query);
	auto request = QNetworkRequest(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, u"Mozilla/5.0"_q);
	const auto reply = _network->get(request);
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (requestId != _requestId) {
			return;
		}
		const auto songs = QJsonDocument::fromJson(reply->readAll()).object(
		).value(u"result"_q).toObject().value(u"songs"_q).toArray();
		const auto duration = length();
		auto best = qint64(0);
		auto bestDiff = crl::time(-1);
		for (const auto &entry : songs) {
			const auto song = entry.toObject();
			const auto diff = duration
				? std::abs(crl::time(song.value(u"duration"_q).toDouble())
					- duration)
				: crl::time(0);
			if (duration && diff > 15000) {
				continue;
			}
			if (bestDiff < 0 || diff < bestDiff) {
				bestDiff = diff;
				best = qint64(song.value(u"id"_q).toDouble());
			}
		}
		if (best) {
			loadNetEaseLyric(requestId, best);
		} else {
			_lyrics = Lyrics::None;
			update();
		}
	});
}

void View::loadNetEaseLyric(int requestId, qint64 id) {
	auto url = QUrl(u"https://music.163.com/api/song/lyric"_q);
	auto query = QUrlQuery();
	query.addQueryItem(u"id"_q, QString::number(id));
	query.addQueryItem(u"lv"_q, u"1"_q);
	url.setQuery(query);
	auto request = QNetworkRequest(url);
	request.setHeader(QNetworkRequest::UserAgentHeader, u"Mozilla/5.0"_q);
	const auto reply = _network->get(request);
	connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (requestId != _requestId) {
			return;
		}
		const auto lyric = QJsonDocument::fromJson(reply->readAll()).object(
		).value(u"lrc"_q).toObject().value(u"lyric"_q).toString();
		auto object = QJsonObject();
		object.insert(u"syncedLyrics"_q, lyric);
		if (lyric.isEmpty() || !applyLyrics(object)) {
			_lyrics = Lyrics::None;
			update();
		}
	});
}

bool View::applyLyrics(const QJsonObject &object) {
	const auto synced = object.value(u"syncedLyrics"_q).toString();
	const auto plain = object.value(u"plainLyrics"_q).toString();
	if (!synced.isEmpty()) {
		_lines = ParseSynced(synced);
		if (!_lines.empty()) {
			_lyrics = Lyrics::Synced;
		}
	}
	if (_lines.empty() && !plain.isEmpty()) {
		for (const auto &text : plain.split('\n')) {
			_lines.push_back({ .time = -1, .text = text.trimmed() });
		}
		_lyrics = Lyrics::Plain;
	}
	if (_lines.empty()) {
		return false;
	}
	_linesWidth = 0;
	_current = -1;
	updateCurrentLine();
	update();
	return true;
}

QRect View::coverRect() const {
	const auto size = S(272);
	return QRect(S(40), S(36), size, size);
}

QRect View::sliderRect() const {
	const auto cover = coverRect();
	if (style::Md3Enabled()) {
		return QRect(cover.x(), S(389), cover.width(), S(10));
	}
	return QRect(cover.x(), S(392), cover.width(), S(4));
}

QPoint View::buttonCenter(Button button) const {
	const auto cover = coverRect();
	const auto x = cover.x() + cover.width() / 2;
	const auto y = S(460);
	const auto row = y + S(56);
	switch (button) {
	case Button::Previous: return QPoint(x - S(76), y);
	case Button::Next: return QPoint(x + S(76), y);
	case Button::Shuffle: return QPoint(x - S(116), row);
	case Button::Repeat: return QPoint(x - S(58), row);
	case Button::Playlists: return QPoint(x, row);
	case Button::ShowMessage: return QPoint(x + S(58), row);
	case Button::CopyLyrics: return QPoint(x + S(116), row);
	case Button::Heart: return QPoint(cover.right() - S(16), S(348));
	default: return QPoint(x, y);
	}
}

QRect View::lyricsCardRect() const {
	const auto left = coverRect().x() + coverRect().width() + S(40);
	return QRect(left, S(36), width() - left - S(32), _height - S(64));
}

QRect View::lyricsRect() const {
	if (style::Md3Enabled()) {
		return lyricsCardRect().marginsRemoved(
			QMargins(S(24), S(22), S(24), S(22)));
	}
	const auto left = coverRect().right() + S(48);
	return QRect(left, S(24), width() - left - S(32), _height - S(48));
}

void View::layoutLines() {
	const auto rect = lyricsRect();
	if (_linesWidth == rect.width()) {
		return;
	}
	_linesWidth = rect.width();
	const auto md3 = style::Md3Enabled();
	auto top = 0;
	for (auto i = 0; i != int(_lines.size()); ++i) {
		auto &line = _lines[i];
		const auto text = line.text.isEmpty() ? u"..."_q : line.text;
		const auto &font = (!md3 || i == _current) ? _currentFont : _lineFont;
		line.top = top;
		line.height = QFontMetrics(font->f).boundingRect(
			QRect(0, 0, rect.width(), 10000),
			Qt::TextWordWrap,
			text).height();
		top += line.height + (md3 ? S(10) : S(14));
	}
	_linesHeight = top;
}

void View::updateCurrentLine() {
	if (_lyrics != Lyrics::Synced || _lines.empty()) {
		return;
	}
	layoutLines();
	const auto now = position();
	auto current = -1;
	for (auto i = 0; i != int(_lines.size()); ++i) {
		if (_lines[i].time <= now) {
			current = i;
		} else {
			break;
		}
	}
	if (current == _current) {
		return;
	}
	_current = current;
	if (style::Md3Enabled()) {
		_linesWidth = 0;
		layoutLines();
	}
	const auto rect = lyricsRect();
	const auto &line = _lines[std::max(current, 0)];
	const auto target = std::clamp(
		float64(line.top + line.height / 2 - rect.height() / 3),
		0.,
		float64(std::max(_linesHeight - rect.height() / 2, 0)));
	_scrollFrom = _scroll;
	_scrollTo = target;
	_scrollAnimation.stop();
	_scrollAnimation.start([=] {
		_scroll = _scrollFrom
			+ (_scrollTo - _scrollFrom) * _scrollAnimation.value(1.);
		update();
	}, 0., 1., 420, anim::easeOutCirc);
}

Button View::buttonAt(QPoint point, int *line) const {
	const auto radius = S(26);
	for (const auto button : { Button::Previous, Button::Play, Button::Next }) {
		const auto center = buttonCenter(button);
		if ((point - center).manhattanLength() <= radius * 1.4) {
			return button;
		}
	}
	for (const auto button : {
			Button::Shuffle,
			Button::Repeat,
			Button::Playlists,
			Button::ShowMessage,
			Button::CopyLyrics,
			Button::Heart }) {
		const auto center = buttonCenter(button);
		if ((point - center).manhattanLength() <= S(22)) {
			return button;
		}
	}
	if (sliderRect().marginsAdded({ S(6), S(12), S(6), S(12) }).contains(point)) {
		return Button::Slider;
	}
	const auto rect = lyricsRect();
	if (_lyrics == Lyrics::Synced && rect.contains(point)) {
		const auto y = point.y() - rect.y() + int(_scroll);
		for (auto i = 0; i != int(_lines.size()); ++i) {
			const auto &entry = _lines[i];
			if (y >= entry.top && y < entry.top + entry.height) {
				if (line) {
					*line = i;
				}
				return Button::Line;
			}
		}
	}
	return Button::None;
}

void View::seekTo(float64 progress) {
	const auto instance = Media::Player::instance();
	instance->startSeeking(Type::Song);
	instance->finishSeeking(Type::Song, std::clamp(progress, 0., 1.));
}

void View::mouseMoveEvent(QMouseEvent *e) {
	if (_seeking) {
		const auto rect = sliderRect();
		_seekValue = std::clamp(
			float64(e->pos().x() - rect.x()) / rect.width(),
			0.,
			1.);
		update();
		return;
	}
	const auto over = buttonAt(e->pos());
	if (_over != over) {
		_over = over;
		setCursor((over == Button::None)
			? style::cur_default
			: style::cur_pointer);
		update();
	}
}

void View::leaveEventHook(QEvent *e) {
	if (!_seeking && _over != Button::None) {
		_over = Button::None;
		setCursor(style::cur_default);
		update();
	}
}

void View::mousePressEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	const auto button = buttonAt(e->pos());
	_pressedButton = button;
	_longPressTriggered = false;
	_longPressTimer.cancel();
	if (button == Button::Heart || button == Button::Playlists) {
		_longPressTimer.callOnce(kLongPressMs);
	}
	if (button == Button::Slider && length() > 0) {
		_seeking = true;
		Media::Player::instance()->startSeeking(Type::Song);
		mouseMoveEvent(e);
	}
}

void View::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() != Qt::LeftButton) {
		return;
	}
	_longPressTimer.cancel();
	const auto wasLongPress = std::exchange(_longPressTriggered, false);
	_pressedButton = Button::None;
	if (_seeking) {
		_seeking = false;
		Media::Player::instance()->finishSeeking(Type::Song, _seekValue);
		update();
		return;
	}
	if (wasLongPress) {
		return;
	}
	auto line = -1;
	const auto instance = Media::Player::instance();
	switch (buttonAt(e->pos(), &line)) {
	case Button::Previous:
		if (!Extera::Playlists::HasActiveQueue() || !Extera::Playlists::PlayPrevious()) {
			instance->previous(Type::Song);
		}
		break;
	case Button::Next:
		if (!Extera::Playlists::HasActiveQueue() || !Extera::Playlists::PlayNext()) {
			instance->next(Type::Song);
		}
		break;
	case Button::Play: instance->playPause(Type::Song); break;
	case Button::Heart: toggleProfileMusic(); break;
	case Button::Playlists:
		if (_document) {
			Extera::Playlists::ShowAddToPlaylistBox(_controller, _document, _context);
		} else {
			Extera::Playlists::ShowPlaylistsBox(_controller);
		}
		break;
	case Button::Shuffle: {
		auto &settings = Core::App().settings();
		settings.setPlayerOrderMode(
			(settings.playerOrderMode() == Media::OrderMode::Shuffle)
				? Media::OrderMode::Default
				: Media::OrderMode::Shuffle);
		Core::App().saveSettingsDelayed();
		update();
	} break;
	case Button::Repeat: {
		auto &settings = Core::App().settings();
		const auto mode = settings.playerRepeatMode();
		settings.setPlayerRepeatMode((mode == Media::RepeatMode::None)
			? Media::RepeatMode::All
			: (mode == Media::RepeatMode::All)
			? Media::RepeatMode::One
			: Media::RepeatMode::None);
		Core::App().saveSettingsDelayed();
		update();
	} break;
	case Button::ShowMessage:
		if (const auto item = _controller->session().data().message(
				_context)) {
			_controller->showMessage(item);
			for (auto w = parentWidget(); w; w = w->parentWidget()) {
				if (const auto box = dynamic_cast<Ui::BoxContent*>(w)) {
					box->closeBox();
					break;
				}
			}
		}
		break;
	case Button::CopyLyrics:
		if (!_lines.empty()) {
			auto text = QStringList();
			for (const auto &entry : _lines) {
				text.push_back(entry.text);
			}
			QGuiApplication::clipboard()->setText(text.join('\n'));
			_controller->showToast(tr::lng_exd_player_lyrics_copied(tr::now));
		}
		break;
	case Button::Line:
		if (line >= 0 && length() > 0) {
			seekTo(float64(_lines[line].time) / length());
		}
		break;
	default: break;
	}
}

void View::wheelEvent(QWheelEvent *e) {
	if (_lyrics != Lyrics::Plain && _lyrics != Lyrics::Synced) {
		return;
	}
	layoutLines();
	const auto rect = lyricsRect();
	_scrollAnimation.stop();
	_scroll = std::clamp(
		_scroll - e->angleDelta().y() / 2.,
		0.,
		float64(std::max(_linesHeight - rect.height() / 2, 0)));
	update();
}

void View::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	auto hq = PainterHighQualityEnabler(p);

	const auto progress = _colors.value(1.);
	const auto colors = ExdPalette();
	auto gradient = QLinearGradient(0, 0, width() / 3, height());
	gradient.setColorAt(0., Mix(_wasTop, _top, progress));
	gradient.setColorAt(1., Mix(_wasBottom, _bottom, progress));
	const auto radius = st::boxRadius;
	auto clip = QPainterPath();
	clip.addRoundedRect(rect(), radius, radius);
	p.setClipPath(clip);
	if (colors.md3) {
		p.fillRect(rect(), colors.bg);
		auto card = QPainterPath();
		card.addRoundedRect(lyricsCardRect(), S(20), S(20));
		p.fillPath(card, colors.card);
	} else {
		p.fillRect(rect(), gradient);
	}
	if (!colors.md3 && !_backdrop.isNull()) {
		p.setOpacity(0.32 * progress);
		p.drawImage(rect(), _backdrop);
		p.setOpacity(1.);
		auto shade = QLinearGradient(0, 0, 0, height());
		shade.setColorAt(0., QColor(0, 0, 0, 40));
		shade.setColorAt(1., QColor(0, 0, 0, 140));
		p.fillRect(rect(), shade);
	}

	const auto cover = coverRect();
	auto path = QPainterPath();
	path.addRoundedRect(
		cover,
		colors.md3 ? S(20) : S(16),
		colors.md3 ? S(20) : S(16));
	if (!_cover.isNull()) {
		p.save();
		p.setClipPath(path);
		const auto scaled = _cover.size().scaled(
			cover.size(),
			Qt::KeepAspectRatioByExpanding);
		p.drawImage(
			QRect(
				cover.x() - (scaled.width() - cover.width()) / 2,
				cover.y() - (scaled.height() - cover.height()) / 2,
				scaled.width(),
				scaled.height()),
			_cover);
		p.restore();
	} else {
		p.fillPath(path, colors.md3 ? colors.raised : QColor(255, 255, 255, 28));
		p.setPen(colors.md3 ? colors.sub : QColor(255, 255, 255, 150));
		p.setFont(style::font(S(72), style::FontFlags(), 0)->f);
		p.drawText(cover, Qt::AlignCenter, QString(QChar(0x266B)));
	}

	paintControls(p);
	paintLyrics(p);
}

void View::paintControlsMd3(QPainter &p) {
	const auto colors = ExdPalette();
	const auto cover = coverRect();
	const auto textWidth = cover.width() - S(42);
	p.setPen(colors.fg);
	p.setFont(_titleFont->f);
	p.drawText(
		QRect(cover.x(), S(326), textWidth, _titleFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		_titleFont->elided(_title, textWidth));
	p.setPen(colors.sub);
	p.setFont(_subtitleFont->f);
	p.drawText(
		QRect(cover.x(), S(352), textWidth, _subtitleFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		_subtitleFont->elided(_performer, textWidth));

	const auto heartCenter = buttonCenter(Button::Heart);
	const auto saved = isSavedToProfile();
	if (saved || _over == Button::Heart) {
		p.setPen(Qt::NoPen);
		p.setBrush(saved ? colors.tonal : colors.raised);
		p.drawEllipse(QPointF(heartCenter), S(18), S(18));
	}
	PaintHeartIcon(
		p,
		heartCenter,
		S(9),
		saved ? colors.accent : (_over == Button::Heart) ? colors.fg : colors.sub,
		saved);

	const auto slider = sliderRect();
	const auto total = length();
	const auto now = position();
	const auto value = (total > 0)
		? std::clamp(float64(now) / total, 0., 1.)
		: 0.;
	const auto cy = slider.y() + slider.height() / 2.0;
	const auto strokeW = float64(S(4));
	const auto handleWidth = float64(_seeking ? S(4) : S(5));
	const auto handleHeight = float64((_over == Button::Slider || _seeking)
		? S(24)
		: S(20));
	const auto left = float64(slider.x()) + S(2);
	const auto right = float64(slider.x() + slider.width()) - S(2);
	const auto handleCenterX = left + (right - left) * value;
	const auto visualGap = float64(S(4)) + strokeW * 0.35;
	const auto capHalf = strokeW / 2.0;
	const auto waveEnd = handleCenterX - handleWidth / 2.0 - visualGap - capHalf;
	const auto stopX = right - capHalf;
	const auto restX = std::min(
		stopX,
		handleCenterX + handleWidth / 2.0 + visualGap + capHalf);

	if (restX < stopX) {
		auto trackPen = QPen(colors.tonal, strokeW, Qt::SolidLine, Qt::RoundCap);
		p.setPen(trackPen);
		p.setBrush(Qt::NoBrush);
		p.drawLine(QPointF(restX, cy), QPointF(stopX, cy));
	}
	if (waveEnd > left) {
		auto wavePen = QPen(
			colors.accent,
			strokeW,
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin);
		p.setPen(wavePen);
		p.setBrush(Qt::NoBrush);
		if (_waveAmplitude > 0.0 && WavySeekbarEnabled()) {
			const auto lambda = float64(S(34));
			const auto step = std::max(1.0, float64(S(2)));
			auto wavePath = QPainterPath();
			auto first = true;
			for (auto px = left; ; px += step) {
				const auto last = (px >= waveEnd);
				const auto cx = last ? waveEnd : px;
				const auto y = cy
					+ _waveAmplitude * std::sin(kWaveTau * cx / lambda - _wavePhase);
				if (first) {
					wavePath.moveTo(cx, y);
					first = false;
				} else {
					wavePath.lineTo(cx, y);
				}
				if (last) {
					break;
				}
			}
			p.drawPath(wavePath);
		} else {
			p.drawLine(QPointF(left, cy), QPointF(waveEnd, cy));
		}
	}
	p.setPen(Qt::NoPen);
	p.setBrush(colors.accent);
	p.drawRoundedRect(
		QRectF(
			handleCenterX - handleWidth / 2.0,
			cy - handleHeight / 2.0,
			handleWidth,
			handleHeight),
		handleWidth / 2.0,
		handleWidth / 2.0);
	if (stopX > restX + S(1)) {
		p.drawEllipse(QPointF(stopX, cy), float64(S(2)), float64(S(2)));
	}

	p.setFont(_timeFont->f);
	p.setPen(colors.sub);
	const auto timesTop = slider.y() + slider.height() + S(10);
	p.drawText(
		QRect(slider.x(), timesTop, slider.width(), _timeFont->height),
		Qt::AlignLeft,
		FormatTime(now));
	p.drawText(
		QRect(slider.x(), timesTop, slider.width(), _timeFont->height),
		Qt::AlignRight,
		u"-"_q + FormatTime(total - now));

	const auto paintSkip = [&](QPoint center, bool forward, bool over) {
		p.setPen(Qt::NoPen);
		p.setBrush(over ? colors.tonal : colors.raised);
		p.drawEllipse(QPointF(center), S(26), S(26));
		p.setBrush(colors.fg);
		const auto h = S(7);
		const auto w = S(9);
		const auto dir = forward ? 1 : -1;
		const auto start = center.x() - dir * (w / 2 + S(2));
		auto triangle = QPolygonF();
		triangle << QPointF(start, center.y() - h)
			<< QPointF(start + dir * w, center.y())
			<< QPointF(start, center.y() + h);
		p.drawPolygon(triangle);
		const auto barX = forward
			? (start + w)
			: (start - w - S(2));
		p.drawRoundedRect(
			QRectF(barX, center.y() - h, S(2), 2 * h),
			S(1),
			S(1));
	};
	paintSkip(
		buttonCenter(Button::Previous),
		false,
		_over == Button::Previous);
	paintSkip(buttonCenter(Button::Next), true, _over == Button::Next);

	const auto &settings = Core::App().settings();
	const auto shuffle = (settings.playerOrderMode()
		== Media::OrderMode::Shuffle);
	const auto repeat = settings.playerRepeatMode();
	const auto paintIcon = [&](
			Button button,
			const style::icon &icon,
			bool active) {
		const auto center = buttonCenter(button);
		const auto size = S(44);
		const auto rect = QRect(
			center.x() - size / 2,
			center.y() - size / 2,
			size,
			size);
		if (active || _over == button) {
			p.setPen(Qt::NoPen);
			p.setBrush(active ? colors.tonal : colors.raised);
			p.drawEllipse(rect);
		}
		icon.paintInCenter(
			p,
			rect,
			active ? colors.accent : (_over == button) ? colors.fg : colors.sub);
	};
	paintIcon(Button::Shuffle, st::mediaPlayerShuffleIcon, shuffle);
	paintIcon(
		Button::Repeat,
		(repeat == Media::RepeatMode::One)
			? st::mediaPlayerRepeatOneIcon
			: st::mediaPlayerRepeatButton.icon,
		repeat != Media::RepeatMode::None);
	paintIcon(
		Button::Playlists,
		st::menuIconAddToFolder,
		Extera::Playlists::HasActiveQueue());
	paintIcon(Button::ShowMessage, st::menuIconShowInChat, false);
	paintIcon(Button::CopyLyrics, st::menuIconCopy, false);

	const auto play = buttonCenter(Button::Play);
	const auto playRadius = S(32);
	p.setPen(Qt::NoPen);
	auto playBg = colors.accent;
	if (_over == Button::Play) {
		playBg = QColor(
			anim::interpolate(playBg.red(), colors.fg.red(), 0.12),
			anim::interpolate(playBg.green(), colors.fg.green(), 0.12),
			anim::interpolate(playBg.blue(), colors.fg.blue(), 0.12));
	}
	p.setBrush(playBg);
	p.drawEllipse(QPointF(play), playRadius, playRadius);
	p.setBrush(colors.onAccent);
	if (Media::Player::ShowPauseIcon(_state.state)) {
		const auto h = S(10);
		p.drawRoundedRect(
			QRectF(play.x() - S(8), play.y() - h, S(5), 2 * h),
			S(2),
			S(2));
		p.drawRoundedRect(
			QRectF(play.x() + S(3), play.y() - h, S(5), 2 * h),
			S(2),
			S(2));
	} else {
		const auto h = S(11);
		auto triangle = QPolygonF();
		triangle << QPointF(play.x() - S(6), play.y() - h)
			<< QPointF(play.x() + S(12), play.y())
			<< QPointF(play.x() - S(6), play.y() + h);
		p.drawPolygon(triangle);
	}
}

void View::paintControls(QPainter &p) {
	if (style::Md3Enabled()) {
		paintControlsMd3(p);
		return;
	}
	const auto cover = coverRect();
	const auto textWidth = cover.width() - S(42);
	p.setPen(Qt::white);
	p.setFont(_titleFont->f);
	p.drawText(
		QRect(cover.x(), S(328), textWidth, _titleFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		_titleFont->elided(_title, textWidth));
	p.setPen(QColor(255, 255, 255, 170));
	p.setFont(_subtitleFont->f);
	p.drawText(
		QRect(cover.x(), S(356), textWidth, _subtitleFont->height),
		Qt::AlignLeft | Qt::AlignVCenter,
		_subtitleFont->elided(_performer, textWidth));

	const auto heartCenter = buttonCenter(Button::Heart);
	const auto saved = isSavedToProfile();
	if (saved || _over == Button::Heart) {
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, saved ? 46 : 24));
		p.drawEllipse(QPointF(heartCenter), S(18), S(18));
	}
	PaintHeartIcon(
		p,
		heartCenter,
		S(9),
		saved
			? QColor(255, 107, 129)
			: QColor(255, 255, 255, (_over == Button::Heart) ? 255 : 185),
		saved);

	const auto slider = sliderRect();
	const auto total = length();
	const auto now = position();
	const auto value = (total > 0)
		? std::clamp(float64(now) / total, 0., 1.)
		: 0.;
	const auto sliderRadius = slider.height() / 2.;
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 60));
	p.drawRoundedRect(slider, sliderRadius, sliderRadius);
	p.setBrush(Qt::white);
	const auto filled = int(slider.width() * value);
	p.drawRoundedRect(
		QRect(slider.x(), slider.y(), filled, slider.height()),
		sliderRadius,
		sliderRadius);
	if (_over == Button::Slider || _seeking) {
		p.drawEllipse(
			QPointF(slider.x() + filled, slider.y() + sliderRadius),
			S(7),
			S(7));
	}
	p.setFont(_timeFont->f);
	p.setPen(QColor(255, 255, 255, 150));
	const auto timesTop = slider.bottom() + S(8);
	p.drawText(
		QRect(slider.x(), timesTop, slider.width(), _timeFont->height),
		Qt::AlignLeft,
		FormatTime(now));
	p.drawText(
		QRect(slider.x(), timesTop, slider.width(), _timeFont->height),
		Qt::AlignRight,
		u"-"_q + FormatTime(total - now));

	const auto paintSkip = [&](QPoint center, bool forward, bool over) {
		p.setBrush(QColor(255, 255, 255, over ? 255 : 210));
		const auto h = S(9);
		const auto w = S(11);
		const auto dir = forward ? 1 : -1;
		for (auto i = 0; i != 2; ++i) {
			const auto start = center.x() - dir * w + dir * i * w;
			auto triangle = QPolygonF();
			triangle << QPointF(start, center.y() - h)
				<< QPointF(start + dir * w, center.y())
				<< QPointF(start, center.y() + h);
			p.drawPolygon(triangle);
		}
		const auto barX = forward
			? (center.x() + w)
			: (center.x() - w - S(3));
		p.drawRect(QRectF(barX, center.y() - h, S(3), 2 * h));
	};
	paintSkip(
		buttonCenter(Button::Previous),
		false,
		_over == Button::Previous);
	paintSkip(buttonCenter(Button::Next), true, _over == Button::Next);

	const auto &settings = Core::App().settings();
	const auto shuffle = (settings.playerOrderMode()
		== Media::OrderMode::Shuffle);
	const auto repeat = settings.playerRepeatMode();
	const auto paintIcon = [&](
			Button button,
			const style::icon &icon,
			bool active) {
		const auto center = buttonCenter(button);
		const auto size = S(36);
		const auto rect = QRect(
			center.x() - size / 2,
			center.y() - size / 2,
			size,
			size);
		if (active || _over == button) {
			p.setPen(Qt::NoPen);
			p.setBrush(QColor(255, 255, 255, active ? 46 : 24));
			p.drawRoundedRect(rect, size / 2., size / 2.);
		}
		icon.paintInCenter(
			p,
			rect,
			QColor(255, 255, 255, (active || _over == button) ? 255 : 170));
	};
	paintIcon(Button::Shuffle, st::mediaPlayerShuffleIcon, shuffle);
	paintIcon(
		Button::Repeat,
		(repeat == Media::RepeatMode::One)
			? st::mediaPlayerRepeatOneIcon
			: st::mediaPlayerRepeatButton.icon,
		repeat != Media::RepeatMode::None);
	paintIcon(
		Button::Playlists,
		st::menuIconAddToFolder,
		Extera::Playlists::HasActiveQueue());
	paintIcon(Button::ShowMessage, st::menuIconShowInChat, false);
	paintIcon(Button::CopyLyrics, st::menuIconCopy, false);

	const auto play = buttonCenter(Button::Play);
	const auto playRadius = S(30);
	p.setBrush(QColor(255, 255, 255, (_over == Button::Play) ? 255 : 235));
	p.drawEllipse(QPointF(play), playRadius, playRadius);
	p.setBrush(Mix(_wasBottom, _bottom, _colors.value(1.)));
	if (Media::Player::ShowPauseIcon(_state.state)) {
		const auto h = S(11);
		p.drawRoundedRect(
			QRectF(play.x() - S(9), play.y() - h, S(6), 2 * h),
			S(2),
			S(2));
		p.drawRoundedRect(
			QRectF(play.x() + S(3), play.y() - h, S(6), 2 * h),
			S(2),
			S(2));
	} else {
		const auto h = S(12);
		auto triangle = QPolygonF();
		triangle << QPointF(play.x() - S(7), play.y() - h)
			<< QPointF(play.x() + S(13), play.y())
			<< QPointF(play.x() - S(7), play.y() + h);
		p.drawPolygon(triangle);
	}
}

void View::paintLyrics(QPainter &p) {
	const auto rect = lyricsRect();
	const auto colors = ExdPalette();
	if (_lyrics == Lyrics::Loading || _lyrics == Lyrics::None) {
		p.setPen(colors.md3 ? colors.sub : QColor(255, 255, 255, 140));
		p.setFont(_subtitleFont->f);
		p.drawText(
			rect,
			Qt::AlignCenter | Qt::TextWordWrap,
			(_lyrics == Lyrics::Loading)
				? tr::lng_exd_player_lyrics_loading(tr::now)
				: tr::lng_exd_player_lyrics_none(tr::now));
		return;
	}
	layoutLines();
	p.save();
	p.setClipRect(rect);
	const auto synced = (_lyrics == Lyrics::Synced);
	for (auto i = 0; i != int(_lines.size()); ++i) {
		const auto &line = _lines[i];
		const auto top = rect.y() + line.top - int(_scroll);
		if (top + line.height < rect.y() || top > rect.bottom()) {
			continue;
		}
		const auto current = synced && (i == _current);
		const auto passed = synced && (i < _current);
		const auto edge = std::min(
			top + line.height - rect.y(),
			rect.bottom() - top);
		const auto edgeFade = std::clamp(edge / float64(S(48)), 0., 1.);
		const auto alpha = current
			? 255
			: !synced
			? 220
			: passed
			? 90
			: 120;
		if (colors.md3) {
			auto color = (current || !synced) ? colors.fg : colors.sub;
			color.setAlphaF(color.alphaF()
				* (current ? 1. : passed ? 0.6 : 1.)
				* edgeFade);
			p.setPen(color);
		} else {
			p.setPen(QColor(255, 255, 255, int(alpha * edgeFade)));
		}
		p.setFont((current ? _currentFont : _lineFont)->f);
		p.drawText(
			QRect(rect.x(), top, rect.width(), line.height),
			Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
			line.text.isEmpty() ? QString(QChar(0x266A)) : line.text);
	}
	p.restore();
}

} // namespace

void View::contextMenuEvent(QContextMenuEvent *e) {
	const auto button = buttonAt(e->pos());
	if (button == Button::Heart) {
		saveToSavedMessages();
		return;
	}
	if (button == Button::Playlists) {
		Extera::Playlists::ShowPlaylistsBox(_controller);
		return;
	}
	_pluginMenu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	if (_document) {
		_pluginMenu->addAction(u"Save to Saved Messages"_q, [=] {
			saveToSavedMessages();
		}, &st::menuIconForward);
		_pluginMenu->addAction(u"Add to Playlist"_q, [=] {
			if (_document) {
				Extera::Playlists::ShowAddToPlaylistBox(
					_controller,
					_document,
					_context);
			}
		}, &st::menuIconAddToFolder);
	}
	_pluginMenu->addAction(u"My Playlists"_q, [=] {
		Extera::Playlists::ShowPlaylistsBox(_controller);
	}, &st::menuIconFave);
	_pluginMenu->addAction(u"Identify Music (SongRec)"_q, [=] {
		Extera::SongRec::RecognizeCurrentPlayback(_controller);
	}, &st::menuIconSoundOn);

	const auto items = Extera::Plugins::PlayerMenuItems();
	if (!items.empty()) {
		_pluginMenu->addSeparator();
		for (const auto &[id, text] : items) {
			const auto itemId = id;
			_pluginMenu->addAction(text, [=] {
				Extera::Plugins::PlayerMenuClick(itemId);
			});
		}
	}
	_pluginMenu->popup(e->globalPos());
}

class PlayerBox final : public Ui::BoxContent {
public:
	PlayerBox(QWidget*, not_null<Window::SessionController*> controller)
	: _controller(controller) {
		Extera::Plugins::NotifyPlayerShown(true);
	}

	~PlayerBox() {
		Extera::Plugins::NotifyPlayerShown(false);
	}

protected:
	void prepare() override {
		const auto view = Ui::CreateChild<View>(this, _controller);
		view->resizeToWidth(S(760));
		view->moveToLeft(0, 0);
		view->show();
		setStyle(st::exteraPlayerBox);
		setNoContentMargin(true);
		setDimensions(S(760), view->height());
	}

private:
	const not_null<Window::SessionController*> _controller;

};

void Show(not_null<Window::SessionController*> controller) {
	controller->show(Box<PlayerBox>(controller));
}

} // namespace Extera::FullPlayer
