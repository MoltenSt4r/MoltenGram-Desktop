/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QRect>
#include <QtCore/QSize>

#include <vector>

class QPainter;

namespace Extera::MediaCarousel {

struct CardLayout {
	int index = 0;
	QRect rect;
	bool focused = false;
	float64 dimOpacity = 0.;
};

struct Layout {
	QSize size;
	std::vector<CardLayout> cards;
	QRect prevButtonRect;
	QRect nextButtonRect;
	QRect counterPillRect;
	int cornerRadius = 20;
};

struct State {
	int activeIndex = 0;
};

[[nodiscard]] bool Enabled();

[[nodiscard]] int OptimalHeight(int width, int itemCount);

[[nodiscard]] Layout ComputeLayout(
	int width,
	int height,
	int itemCount,
	int activeIndex);

void PaintCardOverlay(
	QPainter &p,
	const CardLayout &card,
	int cornerRadius,
	bool dark);

void PaintChrome(
	QPainter &p,
	const Layout &layout,
	int activeIndex,
	int itemCount,
	bool dark,
	bool hovered);

} // namespace Extera::MediaCarousel
