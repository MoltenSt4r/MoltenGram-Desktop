#pragma once

#include "base/basic_types.h"

#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Extera::Md3 {

void ApplyBeforeStyles();

[[nodiscard]] bool Enabled();
[[nodiscard]] bool RoundedSelection();
[[nodiscard]] bool FloatingField();
[[nodiscard]] bool DrawerCorners();
[[nodiscard]] bool Expressive();
[[nodiscard]] bool ProgressBars();
[[nodiscard]] bool MonetDynamic();
[[nodiscard]] QString MonetColor();

struct MonetPreset {
	QString name;
	QColor color;
};

[[nodiscard]] const std::vector<MonetPreset> &MonetPresets();
void SetMonetColor(const QString &hex);

void ShowSettingsBox(not_null<Window::SessionController*> controller);
void ShowMonetColorBox(not_null<Window::SessionController*> controller);
void ShowIconPacksBox(not_null<Window::SessionController*> controller);
[[nodiscard]] bool ShowIntroIfNeeded(
	not_null<Window::SessionController*> controller);

// --- Material Design 3 Shape Tokens (Official Corner Radius Scale) ---
enum class ShapeScale {
	None,       // 0dp
	ExtraSmall, // 4dp (Badges, snackbars)
	Small,      // 8dp (Chips, tooltips, tags)
	Medium,     // 12dp (Small cards, dialog list rows, menus)
	Large,      // 16dp (Standard cards, drawers)
	ExtraLarge, // 28dp (Dialog containers, large sheets)
	Full,       // 9999dp / Pill
};

inline constexpr double kShapeRadiusNone = 0.;
inline constexpr double kShapeRadiusExtraSmall = 4.;
inline constexpr double kShapeRadiusSmall = 8.;
inline constexpr double kShapeRadiusMedium = 12.;
inline constexpr double kShapeRadiusLarge = 16.;
inline constexpr double kShapeRadiusExtraLarge = 28.;
inline constexpr double kShapeRadiusFull = 9999.;

[[nodiscard]] double ShapeRadius(ShapeScale scale);

// Continuous-curvature squircle generator (smooth G2 cubic Bezier)
[[nodiscard]] QPainterPath SquirclePath(
	const QRectF &rect,
	double radius,
	double smoothing = 0.65);

void PaintSquircle(
	QPainter &p,
	const QRectF &rect,
	const QBrush &brush,
	double radius,
	double smoothing = 0.65);

// --- WCAG AA Relative Luminance & Contrast Calculation ---
// WCAG AA requirement: >= 4.5:1 for body text, >= 3.0:1 for large/bold text (>= 18pt or >= 14pt bold).
[[nodiscard]] double RelativeLuminance(const QColor &color);
[[nodiscard]] double ContrastRatio(const QColor &c1, const QColor &c2);
[[nodiscard]] bool MeetsWcagAaBody(const QColor &text, const QColor &bg);
[[nodiscard]] bool MeetsWcagAaLarge(const QColor &text, const QColor &bg);
[[nodiscard]] QColor EnsureWcagAaBody(const QColor &text, const QColor &bg);
[[nodiscard]] QColor EnsureWcagAaLarge(const QColor &text, const QColor &bg);

// --- Material Design 3 Surface Tokens (Tonal Elevation) ---
struct SurfaceTokens {
	QColor surface;
	QColor surfaceDim;
	QColor surfaceBright;
	QColor surfaceContainerLowest;
	QColor surfaceContainerLow;
	QColor surfaceContainer; // Default
	QColor surfaceContainerHigh;
	QColor surfaceContainerHighest;
	QColor onSurface;
	QColor onSurfaceVariant;
	QColor outline;
	QColor outlineVariant;
	QColor primary;
	QColor onPrimary;
	QColor primaryContainer;
	QColor onPrimaryContainer;
	QColor secondaryContainer;
	QColor onSecondaryContainer;
};

[[nodiscard]] SurfaceTokens ComputeSurfaceTokens(
	bool dark,
	const QColor &bg,
	const QColor &fg,
	const QColor &accent);

[[nodiscard]] SurfaceTokens CurrentSurfaceTokens();

[[nodiscard]] QColor SurfaceContainerLowest();
[[nodiscard]] QColor SurfaceContainerLow();
[[nodiscard]] QColor SurfaceContainerDefault();
[[nodiscard]] QColor SurfaceContainerHigh();
[[nodiscard]] QColor SurfaceContainerHighest();

// --- Material Design 3 State Layers (Hover, Focus, Pressed, Dragged) ---
enum class StateLayer {
	Hover,   // 8% opacity
	Focus,   // 10% opacity
	Pressed, // 10% standard, 12% expressive
	Dragged, // 16% opacity
};

[[nodiscard]] double StateLayerOpacity(StateLayer state, bool expressive = true);
[[nodiscard]] QColor StateLayerColor(
	const QColor &contentColor,
	StateLayer state,
	bool expressive = true);

void PaintStateLayer(
	QPainter &p,
	const QRectF &rect,
	const QColor &contentColor,
	StateLayer state,
	double radius = 0.,
	bool expressive = true);

// --- Material Design 3 Component Specifications ---
struct SwitchSpec {
	double trackWidth = 52.;
	double trackHeight = 32.;
	double trackRadius = 16.;
	double thumbUnselectedSize = 16.;
	double thumbSelectedSize = 24.;
	double thumbPressedSize = 28.;
};

[[nodiscard]] SwitchSpec GetSwitchSpec();

struct SliderSpec {
	double trackHeightStandard = 4.;
	double trackHeightExpressive = 16.;
	double thumbRadiusStandard = 10.;
	double thumbWidthExpressive = 4.;
	double thumbHeightExpressive = 44.;
	double stopIndicatorGap = 4.;
};

[[nodiscard]] SliderSpec GetSliderSpec(bool expressive = true);

struct ProgressBarSpec {
	double trackHeightStandard = 4.;
	double trackHeightExpressive = 8.;
	double stopIndicatorSize = 4.;
};

[[nodiscard]] ProgressBarSpec GetProgressBarSpec(bool expressive = true);

void PaintSwitch(
	QPainter &p,
	const QRectF &rect,
	bool checked,
	bool hovered,
	bool pressed,
	const SurfaceTokens &tokens);

void PaintSlider(
	QPainter &p,
	const QRectF &rect,
	double progress,
	bool hovered,
	bool pressed,
	bool expressive,
	const SurfaceTokens &tokens);

void PaintLinearProgressBar(
	QPainter &p,
	const QRectF &rect,
	double progress,
	bool expressive,
	const SurfaceTokens &tokens);

void PaintCircularProgressIndicator(
	QPainter &p,
	const QRectF &rect,
	double progress,
	double penWidth,
	const SurfaceTokens &tokens);

} // namespace Extera::Md3
