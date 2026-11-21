/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/unique_qptr.h"

#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QPainterPath>

#include <optional>

class QPainter;

namespace Ui {
class VerticalLayout;
struct SimpleBubble;
} // namespace Ui

namespace Window {
class SessionController;
} // namespace Window

namespace Extera::AvatarShapes {

enum class PeerType : int {
	User = 0,
	Bot = 1,
	Group = 2,
	Channel = 3,
	Self = 4,
};

inline constexpr int kShapeCircle = 0;
inline constexpr int kShapeSquare = 1;
inline constexpr int kShapeSlanted = 2;
inline constexpr int kShapeArch = 3;
inline constexpr int kShapeSemicircle = 4;
inline constexpr int kShapeOval = 5;
inline constexpr int kShapePill = 6;
inline constexpr int kShapeTriangle = 7;
inline constexpr int kShapeArrow = 8;
inline constexpr int kShapeFan = 9;
inline constexpr int kShapeDiamond = 10;
inline constexpr int kShapeClamshell = 11;
inline constexpr int kShapePentagon = 12;
inline constexpr int kShapeGem = 13;
inline constexpr int kShapeSunny = 14;
inline constexpr int kShapeVerySunny = 15;
inline constexpr int kShapeCookie4 = 16;
inline constexpr int kShapeCookie6 = 17;
inline constexpr int kShapeCookie7 = 18;
inline constexpr int kShapeCookie9 = 19;
inline constexpr int kShapeCookie12 = 20;
inline constexpr int kShapeClover4 = 21;
inline constexpr int kShapeClover8 = 22;
inline constexpr int kShapeBurst = 23;
inline constexpr int kShapeSoftBurst = 24;
inline constexpr int kShapeBoom = 25;
inline constexpr int kShapeSoftBoom = 26;
inline constexpr int kShapeFlower = 27;
inline constexpr int kShapePuffy = 28;
inline constexpr int kShapePuffyDiamond = 29;
inline constexpr int kShapeGhostish = 30;
inline constexpr int kShapePixelCircle = 31;
inline constexpr int kShapePixelTriangle = 32;
inline constexpr int kShapeBun = 33;
inline constexpr int kShapeHeart = 34;
inline constexpr int kShapeStar4 = 35;
inline constexpr int kShapeStar8 = 36;
inline constexpr int kShapeHexagon = 37;
inline constexpr int kShapeCloud = 38;
inline constexpr int kShapeScallop = 39;
inline constexpr int kShapeOctagon = 40;
inline constexpr int kShapeShield = 41;
inline constexpr int kShapeFlower12 = 42;
inline constexpr int kShapeClover3 = 43;

inline constexpr int kShapesCount = 44;

[[nodiscard]] QString ShapeName(int shapeId);
[[nodiscard]] QString PeerTypeName(PeerType type);
[[nodiscard]] QString ConfigKeyForPeerType(PeerType type);

[[nodiscard]] const QPainterPath &BasePath(int shapeId);
[[nodiscard]] QPainterPath ShapePath(int shapeId, QRectF rect);

[[nodiscard]] int ShapeForPeerType(PeerType type);
void SetShapeForPeerType(PeerType type, int shapeId);
[[nodiscard]] int ActiveShapeId();

void PaintStoryRing(
	QPainter &p,
	QRectF rect,
	int shapeId,
	const QBrush &brush,
	float64 penWidth);

void ShowShapePickerBox(
	not_null<Window::SessionController*> controller,
	PeerType type,
	Fn<void(int)> onPicked = nullptr);

void ShowSettingsBox(not_null<Window::SessionController*> controller);

void AddSettingsRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Extera::AvatarShapes

namespace Extera::GlassBubbles {

[[nodiscard]] bool Enabled();
[[nodiscard]] int Opacity();
[[nodiscard]] bool StrokeEnabled();

void PaintGlassBubble(
	QPainter &p,
	const Ui::SimpleBubble &simple,
	const std::optional<QColor> &backgroundAverage);

void ShowSettingsBox(not_null<Window::SessionController*> controller);

} // namespace Extera::GlassBubbles
