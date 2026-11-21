/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QRect>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <vector>

class QPainter;

namespace Window {
class SessionController;
} // namespace Window

namespace Extera::QrGenerator {

enum class DotStyle : int {
	Squircles = 0,
	Dots = 1,
	RoundedSquares = 2,
	Squares = 3,
	Diamond = 4,
	Md3Shapes = 5,
};

struct Matrix final {
	int size = 0;
	std::vector<uint8_t> modules; // Row-major: modules[y * size + x] != 0 -> dark

	[[nodiscard]] bool valid() const {
		return (size > 0) && (modules.size() == size_t(size * size));
	}

	[[nodiscard]] bool isDark(int x, int y) const {
		if (x >= 0 && x < size && y >= 0 && y < size) {
			return modules[y * size + x] != 0;
		}
		return false;
	}

	[[nodiscard]] bool isFinder(int x, int y) const {
		// Top-left
		if (x < 7 && y < 7) {
			return true;
		}
		// Top-right
		if (x >= size - 7 && y < 7) {
			return true;
		}
		// Bottom-left
		if (x < 7 && y >= size - 7) {
			return true;
		}
		return false;
	}

	[[nodiscard]] bool isFinderCenter(int x, int y) const {
		if ((x >= 2 && x <= 4 && y >= 2 && y <= 4)
			|| (x >= size - 5 && x <= size - 3 && y >= 2 && y <= 4)
			|| (x >= 2 && x <= 4 && y >= size - 5 && y <= size - 3)) {
			return true;
		}
		return false;
	}
};

struct RenderOptions final {
	int cardWidth = 340;
	int cardHeight = 430;
	int cardRadius = 24; // MD3 large surface corner radius
	int qrSize = 250;
	DotStyle dotStyle = DotStyle::Squircles;
	QColor cardColor = QColor(0xFF, 0xFF, 0xFF);
	QColor foregroundColor = QColor(0x1B, 0x1B, 0x1F); // MD3 on-surface
	QColor accentColor = QColor(0xFF, 0x57, 0x22);     // Molten flame accent
	bool drawCard = true;
	bool drawShadow = true;
	bool drawLogo = true;
	int logoCutoutRadius = 0; // 0 = automatic (~14.5% of qrSize)
	QString title;
	QString subtitle;
};

// Generates QR matrix from text/link using lib_qr or embedded qrcodegen
[[nodiscard]] Matrix GenerateMatrix(const QString &text);

// Renders complete MD3 styled QR card into a QImage
[[nodiscard]] QImage RenderCard(
	const QString &link,
	const RenderOptions &options = RenderOptions());

// Paints the MD3 styled QR card onto a given painter rect
void PaintCard(
	QPainter &p,
	const QRect &targetRect,
	const Matrix &matrix,
	const RenderOptions &options = RenderOptions());

// Paints only the QR code matrix with custom dot styles, finders, and logo cutout
void PaintQr(
	QPainter &p,
	const QRect &qrRect,
	const Matrix &matrix,
	const RenderOptions &options = RenderOptions());

// Paints the center MoltenGram flame/star logo with gradient
void PaintLogo(
	QPainter &p,
	const QRectF &logoRect,
	const QColor &accentColor = QColor(0xFF, 0x57, 0x22));

// Configuration helpers
[[nodiscard]] DotStyle CurrentDotStyle();
void SetCurrentDotStyle(DotStyle style);
[[nodiscard]] QString DotStyleName(DotStyle style);
[[nodiscard]] const std::vector<DotStyle> &AvailableDotStyles();

// Interactive modal QR Code Share dialog box
void ShowQrShareBox(
	not_null<Window::SessionController*> controller,
	const QString &link,
	const QString &title = QString());

void ShowQrShareBox(
	Window::SessionController *controller,
	const QString &link,
	const QString &title = QString());

} // namespace Extera::QrGenerator
