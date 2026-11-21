#include "extera/molten_url_cleaner.h"

#include "extera/extera_config.h"
#include "ui/layers/show.h"
#include "ui/text/text_entity.h"
#include "ui/toast/toast.h"

#include <QtCore/QUrl>
#include <QtCore/QUrlQuery>
#include <QtGui/QGuiApplication>
#include <QtGui/QClipboard>

namespace Extera::UrlCleaner {
namespace {

[[nodiscard]] bool HostMatches(const QString &host, QStringView domain) {
	return !host.compare(domain, Qt::CaseInsensitive)
		|| (host.size() > domain.size()
			&& host.endsWith(domain, Qt::CaseInsensitive)
			&& host[host.size() - domain.size() - 1] == '.');
}

[[nodiscard]] bool IsTrackingParam(
		const QString &rawName,
		const QString &host) {
	const auto name = rawName.toLower();
	if (name.isEmpty()) {
		return false;
	}
	if (name.startsWith(u"utm_"_q)
		|| name.startsWith(u"ga_"_q)
		|| name.startsWith(u"hsa_"_q)
		|| name.startsWith(u"mtm_"_q)
		|| name.startsWith(u"pk_"_q)
		|| name.startsWith(u"mc_"_q)
		|| name.startsWith(u"oly_"_q)
		|| name.startsWith(u"vero_"_q)
		|| name.startsWith(u"_hs"_q)
		|| name.startsWith(u"fb_"_q)) {
		return true;
	}

	static const auto kGlobalParams = std::vector<QString>{
		u"fbclid"_q,
		u"gclid"_q,
		u"gbraid"_q,
		u"wbraid"_q,
		u"dclid"_q,
		u"msclkid"_q,
		u"ttclid"_q,
		u"twclid"_q,
		u"li_fat_id"_q,
		u"igshid"_q,
		u"igsh"_q,
		u"si"_q,
		u"yclid"_q,
		u"ysclid"_q,
		u"_openstat"_q,
		u"ref_src"_q,
		u"ref_url"_q,
		u"mkt_tok"_q,
		u"trk"_q,
		u"trkinfo"_q,
		u"scid"_q,
		u"s_kwcid"_q,
		u"ef_id"_q,
		u"epik"_q,
		u"rdt_cid"_q,
		u"irclickid"_q,
		u"wickedid"_q,
		u"rb_clickid"_q,
		u"ndclid"_q,
		u"vmcid"_q,
		u"srsltid"_q,
		u"spm"_q,
		u"scm"_q,
		u"algo_exp_id"_q,
		u"btsid"_q,
		u"ws_ab_test"_q,
	};
	for (const auto &param : kGlobalParams) {
		if (name == param) {
			return true;
		}
	}

	if (HostMatches(host, u"twitter.com") || HostMatches(host, u"x.com")) {
		if (name == u"s"_q || name == u"t"_q) {
			return true;
		}
	}
	if (HostMatches(host, u"youtube.com") || HostMatches(host, u"youtu.be")) {
		if (name == u"feature"_q || name == u"pp"_q || name == u"kw"_q) {
			return true;
		}
	}
	if (HostMatches(host, u"instagram.com")) {
		if (name == u"img_index"_q) {
			return false;
		}
	}
	return false;
}

} // namespace

bool Enabled() {
	return Extera::Bool(u"strip_tracking_on_open"_q);
}

QString Clean(const QString &url) {
	const auto schemeEnd = url.indexOf(u"://"_q);
	if (schemeEnd <= 0) {
		return url;
	}
	const auto scheme = url.left(schemeEnd).toLower();
	if (scheme != u"http"_q && scheme != u"https"_q) {
		return url;
	}

	auto parsed = QUrl(url);
	if (!parsed.isValid() || !parsed.hasQuery()) {
		return url;
	}

	const auto host = parsed.host().toLower();
	const auto rawQuery = parsed.query(QUrl::FullyEncoded);
	if (rawQuery.isEmpty()) {
		return url;
	}

	auto kept = QStringList();
	auto removed = false;
	for (const auto &piece : rawQuery.split('&')) {
		if (piece.isEmpty()) {
			continue;
		}
		const auto eq = piece.indexOf('=');
		const auto keyEncoded = (eq < 0) ? piece : piece.left(eq);
		const auto keyDecoded = QUrl::fromPercentEncoding(keyEncoded.toUtf8());
		if (IsTrackingParam(keyDecoded, host)) {
			removed = true;
			continue;
		}
		kept.push_back(piece);
	}

	if (!removed) {
		return url;
	}

	parsed.setQuery(kept.isEmpty() ? QString() : kept.join('&'), QUrl::StrictMode);
	return QString::fromUtf8(parsed.toEncoded());
}

QString CleanIfEnabled(const QString &url) {
	return Enabled() ? Clean(url) : url;
}

} // namespace Extera::UrlCleaner

namespace Extera::ColoredText {

bool Enabled() {
	return Extera::Bool(u"colored_text"_q);
}

bool IsColorUrl(const QString &url) {
	return url.startsWith(u"tg://color"_q, Qt::CaseInsensitive)
		|| url.startsWith(u"color:"_q, Qt::CaseInsensitive);
}

std::optional<QColor> ParseColor(const QString &url) {
	if (!IsColorUrl(url)) {
		return std::nullopt;
	}
	const auto parsed = QUrl(url);
	const auto query = QUrlQuery(parsed);
	auto colorStr = query.queryItemValue(u"c"_q);
	if (colorStr.isEmpty()) {
		colorStr = query.queryItemValue(u"color"_q);
	}
	if (colorStr.isEmpty()) {
		const auto colon = url.indexOf(':');
		if (colon >= 0) {
			colorStr = url.mid(colon + 1);
			if (colorStr.startsWith(u"//"_q)) {
				colorStr.remove(0, 2);
			}
			if (colorStr.startsWith(u"color?"_q, Qt::CaseInsensitive)) {
				colorStr.remove(0, 6);
			}
		}
	}
	colorStr = colorStr.trimmed();
	if (colorStr.startsWith('#')) {
		colorStr.remove(0, 1);
	}
	if (colorStr.isEmpty()) {
		return std::nullopt;
	}

	auto ok = false;
	const auto value = colorStr.toUInt(&ok, 16);
	if (!ok) {
		return std::nullopt;
	}
	if (colorStr.size() <= 6) {
		return QColor(
			int((value >> 16) & 0xFF),
			int((value >> 8) & 0xFF),
			int(value & 0xFF));
	}
	return QColor::fromRgba(QRgb(value));
}

int ParseLength(const QString &url) {
	if (!IsColorUrl(url)) {
		return 0;
	}
	const auto query = QUrlQuery(QUrl(url));
	auto lenStr = query.queryItemValue(u"len"_q);
	if (lenStr.isEmpty()) {
		lenStr = query.queryItemValue(u"length"_q);
	}
	auto ok = false;
	const auto len = lenStr.toInt(&ok);
	return (ok && len > 0) ? len : 0;
}

QString BuildColorUrl(const QColor &color, int length) {
	const auto hex = u"%1%2%3"_q
		.arg(color.red(), 2, 16, QChar('0'))
		.arg(color.green(), 2, 16, QChar('0'))
		.arg(color.blue(), 2, 16, QChar('0'))
		.toUpper();
	return (length > 0)
		? u"tg://color?c=%1&len=%2"_q.arg(hex).arg(length)
		: u"tg://color?c=%1"_q.arg(hex);
}

TextWithEntities WrapColored(const QString &text, const QColor &color) {
	auto result = TextWithEntities{ .text = text };
	if (!text.isEmpty() && color.isValid()) {
		result.entities.push_back(EntityInText(
			EntityType::CustomUrl,
			0,
			int(text.size()),
			BuildColorUrl(color, int(text.size()))));
	}
	return result;
}

void HandleColorUrlClick(
		const QString &url,
		std::shared_ptr<Ui::Show> show) {
	const auto color = ParseColor(url);
	if (!color || !color->isValid()) {
		return;
	}
	const auto hex = u"#%1%2%3"_q
		.arg(color->red(), 2, 16, QChar('0'))
		.arg(color->green(), 2, 16, QChar('0'))
		.arg(color->blue(), 2, 16, QChar('0'))
		.toUpper();
	if (auto *clipboard = QGuiApplication::clipboard()) {
		clipboard->setText(hex);
	}
	if (show && show->valid()) {
		show->showToast(u"Color %1 copied"_q.arg(hex));
	}
}

} // namespace Extera::ColoredText
