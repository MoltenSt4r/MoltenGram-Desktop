/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_media_carousel.h"

#include "extera/extera_config.h"
#include "ui/painter.h"
#include "ui/style/style_core.h"
#include "styles/style_basic.h"

#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPen>

#include <algorithm>
#include <cmath>

namespace Extera::MediaCarousel {

bool Enabled() {
	return Config::Instance().boolValue(u"md3_media_carousel"_q);
}

int OptimalHeight(int width, int itemCount) {
	if (width <= 0 || itemCount < 2) {
		return 0;
	}
	const auto minH = style::ConvertScale(180);
	const auto maxH = style::ConvertScale(290);
	const auto ratio = (itemCount == 2) ? 0.72 : 0.68;
	return std::clamp(int(std::lround(width * ratio)), minH, maxH);
}

Layout ComputeLayout(int width, int height, int itemCount, int activeIndex) {
	Layout result;
	result.size = QSize(width, height);
	result.cornerRadius = style::ConvertScale(20);
	if (width <= 0 || height <= 0 || itemCount <= 0) {
		return result;
	}

	const auto active = std::clamp(activeIndex, 0, itemCount - 1);
	const auto pad = style::ConvertScale(4);
	const auto gap = style::ConvertScale(6);
	const auto innerW = std::max(1, width - pad * 2);
	const auto innerH = std::max(1, height - pad * 2);
	const auto top = pad;

	if (itemCount == 1) {
		result.cards.push_back({
			.index = 0,
			.rect = QRect(pad, top, innerW, innerH),
			.focused = true,
			.dimOpacity = 0.,
		});
		return result;
	}

	if (itemCount == 2) {
		// Matches TwoPhotoCarouselStrategy (78% hero / 18% peek).
		const auto avail = std::max(2, innerW - gap);
		const auto heroW = std::max(1, int(std::lround(avail * 0.78)));
		const auto peekW = std::max(1, avail - heroW);
		if (active == 0) {
			result.cards.push_back({
				.index = 0,
				.rect = QRect(pad, top, heroW, innerH),
				.focused = true,
				.dimOpacity = 0.,
			});
			result.cards.push_back({
				.index = 1,
				.rect = QRect(pad + heroW + gap, top, peekW, innerH),
				.focused = false,
				.dimOpacity = 0.22,
			});
		} else {
			result.cards.push_back({
				.index = 0,
				.rect = QRect(pad, top, peekW, innerH),
				.focused = false,
				.dimOpacity = 0.22,
			});
			result.cards.push_back({
				.index = 1,
				.rect = QRect(pad + peekW + gap, top, heroW, innerH),
				.focused = true,
				.dimOpacity = 0.,
			});
		}
	} else {
		// MultiBrowseCarouselStrategy: hero + adjacent peek(s).
		const auto hasPrev = (active > 0);
		const auto hasNext = (active + 1 < itemCount);
		const auto hasNext2 = (active + 2 < itemCount);

		if (!hasPrev) {
			// Start of carousel: hero (70%), next (20%), small peek (10%).
			const auto gapsTotal = hasNext2 ? (gap * 2) : gap;
			const auto avail = std::max(3, innerW - gapsTotal);
			const auto heroW = std::max(1, int(std::lround(avail * 0.70)));
			const auto nextW = hasNext2
				? std::max(1, int(std::lround(avail * 0.20)))
				: std::max(1, avail - heroW);
			auto x = pad;
			result.cards.push_back({
				.index = active,
				.rect = QRect(x, top, heroW, innerH),
				.focused = true,
				.dimOpacity = 0.,
			});
			x += heroW + gap;
			result.cards.push_back({
				.index = active + 1,
				.rect = QRect(x, top, nextW, innerH),
				.focused = false,
				.dimOpacity = 0.20,
			});
			if (hasNext2) {
				x += nextW + gap;
				const auto remW = std::max(1, pad + innerW - x);
				result.cards.push_back({
					.index = active + 2,
					.rect = QRect(x, top, remW, innerH),
					.focused = false,
					.dimOpacity = 0.34,
				});
			}
		} else if (!hasNext) {
			// End of carousel: small peek (10%), prev (20%), hero (70%).
			const auto hasPrev2 = (active - 2 >= 0);
			const auto gapsTotal = hasPrev2 ? (gap * 2) : gap;
			const auto avail = std::max(3, innerW - gapsTotal);
			const auto heroW = std::max(1, int(std::lround(avail * 0.70)));
			const auto prevW = hasPrev2
				? std::max(1, int(std::lround(avail * 0.20)))
				: std::max(1, avail - heroW);
			auto x = pad;
			if (hasPrev2) {
				const auto prev2W = std::max(1, avail - heroW - prevW);
				result.cards.push_back({
					.index = active - 2,
					.rect = QRect(x, top, prev2W, innerH),
					.focused = false,
					.dimOpacity = 0.34,
				});
				x += prev2W + gap;
			}
			result.cards.push_back({
				.index = active - 1,
				.rect = QRect(x, top, prevW, innerH),
				.focused = false,
				.dimOpacity = 0.20,
			});
			x += prevW + gap;
			const auto remHeroW = std::max(1, pad + innerW - x);
			result.cards.push_back({
				.index = active,
				.rect = QRect(x, top, remHeroW, innerH),
				.focused = true,
				.dimOpacity = 0.,
			});
		} else {
			// Middle of carousel: prev peek (15%), hero (70%), next peek (15%).
			const auto avail = std::max(3, innerW - gap * 2);
			const auto prevW = std::max(1, int(std::lround(avail * 0.15)));
			const auto nextW = std::max(1, int(std::lround(avail * 0.15)));
			const auto heroW = std::max(1, avail - prevW - nextW);
			auto x = pad;
			result.cards.push_back({
				.index = active - 1,
				.rect = QRect(x, top, prevW, innerH),
				.focused = false,
				.dimOpacity = 0.22,
			});
			x += prevW + gap;
			result.cards.push_back({
				.index = active,
				.rect = QRect(x, top, heroW, innerH),
				.focused = true,
				.dimOpacity = 0.,
			});
			x += heroW + gap;
			result.cards.push_back({
				.index = active + 1,
				.rect = QRect(x, top, nextW, innerH),
				.focused = false,
				.dimOpacity = 0.22,
			});
		}
	}

	// Navigation buttons and counter pill.
	const auto btnSize = style::ConvertScale(28);
	const auto btnMargin = style::ConvertScale(10);
	const auto btnY = (height - btnSize) / 2;
	if (active > 0) {
		result.prevButtonRect = QRect(pad + btnMargin, btnY, btnSize, btnSize);
	}
	if (active + 1 < itemCount) {
		result.nextButtonRect = QRect(
			width - pad - btnMargin - btnSize,
			btnY,
			btnSize,
			btnSize);
	}

	const auto pillW = style::ConvertScale(42);
	const auto pillH = style::ConvertScale(20);
	result.counterPillRect = QRect(
		width - pad - style::ConvertScale(10) - pillW,
		pad + style::ConvertScale(10),
		pillW,
		pillH);

	return result;
}

void PaintCardOverlay(
		QPainter &p,
		const CardLayout &card,
		int cornerRadius,
		bool dark) {
	auto hq = PainterHighQualityEnabler(p);
	QPainterPath path;
	path.addRoundedRect(QRectF(card.rect), cornerRadius, cornerRadius);

	if (!card.focused && card.dimOpacity > 0.) {
		auto dim = QColor(0, 0, 0);
		dim.setAlphaF(card.dimOpacity);
		p.fillPath(path, dim);
	}

	// Subtle MD3 card outline.
	p.setBrush(Qt::NoBrush);
	auto border = dark
		? QColor(255, 255, 255, card.focused ? 44 : 24)
		: QColor(0, 0, 0, card.focused ? 26 : 16);
	p.setPen(QPen(border, style::ConvertScale(1.)));
	p.drawPath(path);
}

void PaintChrome(
		QPainter &p,
		const Layout &layout,
		int activeIndex,
		int itemCount,
		bool dark,
		bool hovered) {
	if (itemCount < 2) {
		return;
	}
	auto hq = PainterHighQualityEnabler(p);

	// Counter pill ("1 / 5") at top-right.
	if (!layout.counterPillRect.isEmpty()) {
		const auto r = QRectF(layout.counterPillRect);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, dark ? 148 : 132));
		p.drawRoundedRect(r, r.height() * 0.5, r.height() * 0.5);

		p.setPen(Qt::white);
		p.setFont(st::semiboldFont);
		const auto text = QString::number(activeIndex + 1)
			+ u"/"_q
			+ QString::number(itemCount);
		p.drawText(layout.counterPillRect, Qt::AlignCenter, text);
	}

	// Prev / Next chevron pills.
	const auto alpha = hovered ? 168 : 112;
	const auto drawChevron = [&](const QRect &rect, bool left) {
		if (rect.isEmpty()) {
			return;
		}
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, alpha));
		p.drawEllipse(QRectF(rect));

		const auto cx = rect.center().x() + (left ? -0.5 : 0.5);
		const auto cy = float64(rect.center().y());
		const auto arm = style::ConvertScale(4.2);
		QPainterPath chevron;
		if (left) {
			chevron.moveTo(cx + arm * 0.5, cy - arm);
			chevron.lineTo(cx - arm * 0.6, cy);
			chevron.lineTo(cx + arm * 0.5, cy + arm);
		} else {
			chevron.moveTo(cx - arm * 0.5, cy - arm);
			chevron.lineTo(cx + arm * 0.6, cy);
			chevron.lineTo(cx - arm * 0.5, cy + arm);
		}
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(
			Qt::white,
			style::ConvertScale(1.8),
			Qt::SolidLine,
			Qt::RoundCap,
			Qt::RoundJoin));
		p.drawPath(chevron);
	};

	drawChevron(layout.prevButtonRect, true);
	drawChevron(layout.nextButtonRect, false);
}

} // namespace Extera::MediaCarousel
