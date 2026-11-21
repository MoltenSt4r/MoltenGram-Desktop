#include "extera/extera_app_icon.h"

#include "extera/extera_config.h"
#include "core/application.h"
#include "main/main_domain.h"
#include "tray.h"
#include "ui/painter.h"
#include "window/main_window.h"

#include <QtCore/QBuffer>
#include <QtCore/QHash>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtGui/QPainterPath>

#ifdef Q_OS_WIN
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace Extera::AppIcon {
namespace {

[[nodiscard]] QColor Lighter(QColor color, float64 amount) {
	return QColor(
		color.red() + int((255 - color.red()) * amount),
		color.green() + int((255 - color.green()) * amount),
		color.blue() + int((255 - color.blue()) * amount));
}

[[nodiscard]] QColor AccentColor() {
#ifdef Q_OS_WIN
	const auto settings = QSettings(
		u"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\DWM"_q,
		QSettings::NativeFormat);
	const auto value = settings.value(u"AccentColor"_q);
	if (value.isValid()) {
		const auto abgr = value.toUInt();
		return QColor(abgr & 0xFF, (abgr >> 8) & 0xFF, (abgr >> 16) & 0xFF);
	}
#endif
	return QColor(34, 135, 203);
}

[[nodiscard]] const QImage &Glyph() {
	static const auto result = QImage(u":/gui/art/app_icon_glyph.png"_q)
		.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	return result;
}

[[nodiscard]] QString IconPath() {
	return QStandardPaths::writableLocation(
		QStandardPaths::AppDataLocation) + u"/MoltenGram.ico"_q;
}

[[nodiscard]] QByteArray IconFile(const QString &id, bool square) {
	const auto sizes = std::vector<int>{ 16, 24, 32, 48, 64, 128, 256 };
	auto images = std::vector<QByteArray>();
	for (const auto size : sizes) {
		auto bytes = QByteArray();
		auto buffer = QBuffer(&bytes);
		buffer.open(QIODevice::WriteOnly);
		Render(size, id, square).save(&buffer, "PNG");
		images.push_back(bytes);
	}
	auto result = QByteArray();
	const auto push16 = [&](int value) {
		result.append(char(value & 0xFF));
		result.append(char((value >> 8) & 0xFF));
	};
	const auto push32 = [&](int value) {
		push16(value & 0xFFFF);
		push16((value >> 16) & 0xFFFF);
	};
	push16(0);
	push16(1);
	push16(int(sizes.size()));
	auto offset = 6 + 16 * int(sizes.size());
	for (auto i = 0; i != int(sizes.size()); ++i) {
		const auto size = sizes[i];
		result.append(char(size >= 256 ? 0 : size));
		result.append(char(size >= 256 ? 0 : size));
		result.append(char(0));
		result.append(char(0));
		push16(1);
		push16(32);
		push32(int(images[i].size()));
		push32(offset);
		offset += int(images[i].size());
	}
	for (const auto &image : images) {
		result.append(image);
	}
	return result;
}

#ifdef Q_OS_WIN
void SetShortcutIcon(const QString &link, const QString &icon) {
	if (!QFile::exists(link)) {
		return;
	}
	auto shellLink = (IShellLinkW*)nullptr;
	if (FAILED(CoCreateInstance(
			CLSID_ShellLink,
			nullptr,
			CLSCTX_INPROC_SERVER,
			IID_PPV_ARGS(&shellLink)))) {
		return;
	}
	auto file = (IPersistFile*)nullptr;
	if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&file)))) {
		const auto path = QDir::toNativeSeparators(link).toStdWString();
		if (SUCCEEDED(file->Load(path.c_str(), STGM_READWRITE))) {
			const auto location = QDir::toNativeSeparators(icon)
				.toStdWString();
			shellLink->SetIconLocation(location.c_str(), 0);
			file->Save(path.c_str(), TRUE);
		}
		file->Release();
	}
	shellLink->Release();
}
void UpdatePinned(const QString &icon) {
	const auto folder = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"))
		+ u"/Microsoft/Internet Explorer/Quick Launch/User Pinned/TaskBar"_q;
	const auto exe = QDir::toNativeSeparators(
		QCoreApplication::applicationFilePath()).toLower();
	const auto files = QDir(folder).entryInfoList(
		{ u"*.lnk"_q },
		QDir::Files);
	for (const auto &info : files) {
		auto shellLink = (IShellLinkW*)nullptr;
		if (FAILED(CoCreateInstance(
				CLSID_ShellLink,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&shellLink)))) {
			continue;
		}
		auto file = (IPersistFile*)nullptr;
		auto matches = false;
		if (SUCCEEDED(shellLink->QueryInterface(IID_PPV_ARGS(&file)))) {
			const auto path = QDir::toNativeSeparators(
				info.absoluteFilePath()).toStdWString();
			if (SUCCEEDED(file->Load(path.c_str(), STGM_READ))) {
				WCHAR target[MAX_PATH] = { 0 };
				if (SUCCEEDED(shellLink->GetPath(target, MAX_PATH, nullptr, 0))) {
					matches = (QString::fromWCharArray(target).toLower() == exe);
				}
			}
			file->Release();
		}
		shellLink->Release();
		if (matches) {
			SetShortcutIcon(info.absoluteFilePath(), icon);
		}
	}
}
#endif

} // namespace

std::vector<Variant> Variants() {
	const auto accent = AccentColor();
	return {
		{
			QString(),
			QColor(233, 89, 51),
			QColor(251, 123, 119),
			Qt::white,
			u"Classic"_q,
			u":/gui/art/molten_icons/classic.png"_q,
		},
		{
			u"accent"_q,
			accent,
			Lighter(accent, 0.35),
			Qt::white,
			u"Accent"_q,
			QString(),
		},
		{ u"proton"_q, {}, {}, {}, u"Proton"_q, u":/gui/art/molten_icons/proton.png"_q },
		{ u"sad"_q, {}, {}, {}, u"Sad"_q, u":/gui/art/molten_icons/sad.png"_q },
		{ u"schizophrenia"_q, {}, {}, {}, u"Schizophrenia"_q, u":/gui/art/molten_icons/schizophrenia.png"_q },
		{ u"burning"_q, {}, {}, {}, u"Burning"_q, u":/gui/art/molten_icons/burning.png"_q },
		{ u"anotherchance"_q, {}, {}, {}, u"Another Chance"_q, u":/gui/art/molten_icons/anotherchance.png"_q },
		{ u"cinema"_q, {}, {}, {}, u"Cinema"_q, u":/gui/art/molten_icons/cinema.png"_q },
		{ u"sunrise"_q, {}, {}, {}, u"Sunrise"_q, u":/gui/art/molten_icons/sunrise.png"_q },
		{ u"star"_q, {}, {}, {}, u"Star"_q, u":/gui/art/molten_icons/star.png"_q },
		{ u"coffee"_q, {}, {}, {}, u"Coffee"_q, u":/gui/art/molten_icons/coffee.png"_q },
		{ u"glacier"_q, {}, {}, {}, u"Glacier"_q, u":/gui/art/molten_icons/glacier.png"_q },
		{ u"google"_q, {}, {}, {}, u"Google"_q, u":/gui/art/molten_icons/google.png"_q },
		{ u"beer"_q, {}, {}, {}, u"Beer"_q, u":/gui/art/molten_icons/beer.png"_q },
		{ u"magma"_q, {}, {}, {}, u"Magma"_q, u":/gui/art/molten_icons/magma.png"_q },
		{ u"stilllife"_q, {}, {}, {}, u"Still Life"_q, u":/gui/art/molten_icons/stilllife.png"_q },
		{ u"toxic"_q, {}, {}, {}, u"Toxic"_q, u":/gui/art/molten_icons/toxic.png"_q },
		{ u"coral"_q, {}, {}, {}, u"Coral"_q, u":/gui/art/molten_icons/coral.png"_q },
		{ u"onyx"_q, {}, {}, {}, u"Onyx"_q, u":/gui/art/molten_icons/onyx.png"_q },
		{ u"candy"_q, {}, {}, {}, u"Candy"_q, u":/gui/art/molten_icons/candy.png"_q },
		{ u"exteragram"_q, {}, {}, {}, u"ExteraGram"_q, u":/gui/art/molten_icons/exteragram.png"_q },
		{ u"nothing"_q, {}, {}, {}, u"Nothing"_q, u":/gui/art/molten_icons/nothing.png"_q },
		{ u"pizza"_q, {}, {}, {}, u"Pizza"_q, u":/gui/art/molten_icons/pizza.png"_q },
		{ u"yinyang"_q, {}, {}, {}, u"Yin-Yang"_q, u":/gui/art/molten_icons/yinyang.png"_q },
		{ u"rage"_q, {}, {}, {}, u"Rage"_q, u":/gui/art/molten_icons/rage.png"_q },
		{ u"aero"_q, {}, {}, {}, u"Aero"_q, u":/gui/art/molten_icons/aero.png"_q },
		{ u"stone"_q, {}, {}, {}, u"Stone"_q, u":/gui/art/molten_icons/stone.png"_q },
	};
}

QImage Render(int size, const QString &id, bool square) {
	const auto variants = Variants();
	const auto i = ranges::find(variants, id, &Variant::id);
	const auto &variant = (i != end(variants)) ? *i : variants.front();

	auto result = QImage(
		QSize(size, size),
		QImage::Format_ARGB32_Premultiplied);
	result.fill(Qt::transparent);

	const auto rect = QRectF(0, 0, size, size);
	if (!variant.resourcePath.isEmpty()) {
		static auto cache = QHash<QString, QImage>();
		auto it = cache.find(variant.resourcePath);
		if (it == cache.end()) {
			auto loaded = QImage(variant.resourcePath);
			if (!loaded.isNull()) {
				loaded = loaded.convertToFormat(
					QImage::Format_ARGB32_Premultiplied);
			}
			it = cache.insert(variant.resourcePath, std::move(loaded));
		}
		if (!it->isNull()) {
			auto scaled = it->scaled(
				size,
				size,
				Qt::KeepAspectRatioByExpanding,
				Qt::SmoothTransformation);
			if (scaled.width() != size || scaled.height() != size) {
				const auto sx = (scaled.width() - size) / 2;
				const auto sy = (scaled.height() - size) / 2;
				scaled = scaled.copy(sx, sy, size, size);
			}
			auto p = QPainter(&result);
			auto hq = PainterHighQualityEnabler(p);
			auto clip = QPainterPath();
			if (square) {
				const auto radius = size * 0.23;
				clip.addRoundedRect(rect, radius, radius);
			} else {
				clip.addEllipse(rect);
			}
			p.setClipPath(clip);
			p.drawImage(0, 0, scaled);
			return result;
		}
	}

	auto glyph = Glyph().scaled(
		size,
		size,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	{
		auto p = QPainter(&glyph);
		p.setCompositionMode(QPainter::CompositionMode_SourceIn);
		p.fillRect(glyph.rect(), variant.fg);
	}

	auto p = QPainter(&result);
	auto hq = PainterHighQualityEnabler(p);
	auto gradient = QLinearGradient(0, size, size, 0);
	gradient.setColorAt(0., variant.from);
	gradient.setColorAt(1., variant.to);
	p.setPen(Qt::NoPen);
	p.setBrush(gradient);
	if (square) {
		const auto radius = size * 0.23;
		p.drawRoundedRect(rect, radius, radius);
	} else {
		p.drawEllipse(rect);
	}
	p.drawImage(0, 0, glyph);
	return result;
}

QImage Current(int size) {
	return Render(
		size,
		Extera::String(u"app_icon"_q),
		Extera::Bool(u"app_icon_square"_q));
}

void ApplyToShortcuts() {
#ifdef Q_OS_WIN
	const auto id = Extera::String(u"app_icon"_q);
	const auto square = Extera::Bool(u"app_icon_square"_q);
	auto icon = QCoreApplication::applicationFilePath();
	if (!id.isEmpty() || square) {
		const auto path = IconPath();
		QDir().mkpath(QFileInfo(path).absolutePath());
		auto file = QFile(path);
		if (file.open(QIODevice::WriteOnly)) {
			file.write(IconFile(id, square));
			file.close();
			icon = path;
		}
	}
	for (const auto location : {
			QStandardPaths::ApplicationsLocation,
			QStandardPaths::DesktopLocation }) {
		for (const auto &folder : QStandardPaths::standardLocations(
				location)) {
			SetShortcutIcon(folder + u"/MoltenGram Desktop.lnk"_q, icon);
			SetShortcutIcon(folder + u"/MoltenGram Desktop/MoltenGram Desktop.lnk"_q, icon);
		}
	}
	UpdatePinned(icon);
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}

void Apply() {
	ApplyToShortcuts();
	Window::RefreshLogo();
	Window::OverrideApplicationIcon(Window::Logo());
	Core::App().refreshApplicationIcon();
	Core::App().tray().updateIconCounters();
	Core::App().domain().notifyUnreadBadgeChanged();
}

void MakeDefaultClient() {
#ifdef Q_OS_WIN
	for (const auto protocol : { L"tg", L"tonsite", L"ton" }) {
		const auto path = std::wstring(
			L"Software\\Microsoft\\Windows\\Shell\\Associations\\"
			L"UrlAssociations\\") + protocol + L"\\UserChoice";
		RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
		RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
	}
#endif
	Core::Application::RegisterUrlScheme();
#ifdef Q_OS_WIN
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}

} // namespace Extera::AppIcon
