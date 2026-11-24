#include "ui/widgets/scroll_area.h"
#include "extera/exd_md3.h"

#include "core/application.h"
#include "extera/extera_ai_chat.h"
#include "extera/extera_feed.h"
#include "extera/extera_full_player.h"
#include "extera/extera_pdf_viewer.h"
#include "window/window_controller.h"
#include "core/core_settings.h"
#include "extera/extera_config.h"
#include "lang/lang_keys.h"
#include "settings/settings_common.h"
#include "ui/boxes/confirm_box.h"
#include "ui/effects/animation_value.h"
#include "ui/boxes/single_choice_box.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/style/style_core.h"
#include "ui/style/style_core_font.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_chat_helpers.h"
#include "styles/style_dialogs.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtGui/QPainterPath>
#include <QtWidgets/QApplication>
#include <cmath>
#include <algorithm>
#include <utility>
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

namespace Extera::Md3 {
namespace {

constexpr auto kGoogleSans = "Google Sans";

struct PreviewState {
	bool enabled = true;
	bool grouped = true;
	bool rounded = true;
	bool floating = true;
};

[[nodiscard]] PreviewState ReadState() {
	return {
		.enabled = Enabled(),
		.grouped = Bool(u"md3_grouped"_q),
		.rounded = Bool(u"md3_rounded_selection"_q),
		.floating = Bool(u"md3_floating_field"_q),
	};
}

[[nodiscard]] QColor Mix(QColor a, QColor b, float64 amount) {
	return QColor(
		int(a.red() + (b.red() - a.red()) * amount),
		int(a.green() + (b.green() - a.green()) * amount),
		int(a.blue() + (b.blue() - a.blue()) * amount));
}

void RequestRestart(not_null<Window::SessionController*> controller) {
	controller->show(Ui::MakeConfirmBox({
		.text = tr::lng_exd_md3_restart(),
		.confirmed = [] {
			Config::Instance().saveNow();
			Core::Restart();
		},
		.confirmText = tr::lng_extera_restart(),
	}));
}

void RepaintAll() {
	for (const auto widget : QApplication::topLevelWidgets()) {
		widget->update();
	}
}

void PaintBar(QPainter &p, QRectF rect, const QColor &color) {
	p.setBrush(color);
	p.drawRoundedRect(rect, rect.height() / 2., rect.height() / 2.);
}

} // namespace

double ShapeRadius(ShapeScale scale) {
	switch (scale) {
	case ShapeScale::None: return kShapeRadiusNone;
	case ShapeScale::ExtraSmall: return kShapeRadiusExtraSmall;
	case ShapeScale::Small: return kShapeRadiusSmall;
	case ShapeScale::Medium: return kShapeRadiusMedium;
	case ShapeScale::Large: return kShapeRadiusLarge;
	case ShapeScale::ExtraLarge: return kShapeRadiusExtraLarge;
	case ShapeScale::Full: return kShapeRadiusFull;
	}
	return kShapeRadiusNone;
}

QPainterPath SquirclePath(
		const QRectF &rect,
		double radius,
		double smoothing) {
	QPainterPath path;
	if (rect.isEmpty()) {
		return path;
	}
	const auto w = rect.width();
	const auto h = rect.height();
	const auto maxR = std::min(w, h) * 0.5;
	const auto r = std::clamp(radius, 0., maxR);
	if (r <= 0.001) {
		path.addRect(rect);
		return path;
	}
	const auto s = std::clamp(smoothing, 0., 1.);
	const auto d = std::min(maxR, r * (1. + 0.28 * s));
	const auto kCircleK = (4. / 3.) * (std::sqrt(2.) - 1.);
	const auto handle = std::min(d, kCircleK * r * (1. + 0.55 * s));

	const auto left = rect.left();
	const auto top = rect.top();
	const auto right = rect.right();
	const auto bottom = rect.bottom();

	path.moveTo(left + d, top);
	path.lineTo(right - d, top);
	path.cubicTo(right - d + handle, top, right, top + d - handle, right, top + d);

	path.lineTo(right, bottom - d);
	path.cubicTo(right, bottom - d + handle, right - d + handle, bottom, right - d, bottom);

	path.lineTo(left + d, bottom);
	path.cubicTo(left + d - handle, bottom, left, bottom - d + handle, left, bottom - d);

	path.lineTo(left, top + d);
	path.cubicTo(left, top + d - handle, left + d - handle, top, left + d, top);

	path.closeSubpath();
	return path;
}

void PaintSquircle(
		QPainter &p,
		const QRectF &rect,
		const QBrush &brush,
		double radius,
		double smoothing) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.fillPath(SquirclePath(rect, radius, smoothing), brush);
}

namespace {
static double SrgbToLinear(double val) {
	return (val <= 0.04045)
		? (val / 12.92)
		: std::pow((val + 0.055) / 1.055, 2.4);
}
} // namespace

double RelativeLuminance(const QColor &color) {
	const auto r = SrgbToLinear(color.redF());
	const auto g = SrgbToLinear(color.greenF());
	const auto b = SrgbToLinear(color.blueF());
	return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

double ContrastRatio(const QColor &c1, const QColor &c2) {
	auto l1 = RelativeLuminance(c1);
	auto l2 = RelativeLuminance(c2);
	if (l1 < l2) {
		std::swap(l1, l2);
	}
	return (l1 + 0.05) / (l2 + 0.05);
}

bool MeetsWcagAaBody(const QColor &text, const QColor &bg) {
	return ContrastRatio(text, bg) >= 4.5;
}

bool MeetsWcagAaLarge(const QColor &text, const QColor &bg) {
	return ContrastRatio(text, bg) >= 3.0;
}

static QColor EnsureWcagAaInternal(const QColor &text, const QColor &bg, double targetRatio) {
	if (ContrastRatio(text, bg) >= targetRatio) {
		return text;
	}
	const auto bgLum = RelativeLuminance(bg);
	const auto lighten = (bgLum < 0.5);
	auto r = text.red();
	auto g = text.green();
	auto b = text.blue();
	for (auto step = 0; step < 50; ++step) {
		if (ContrastRatio(QColor(r, g, b), bg) >= targetRatio) {
			return QColor(r, g, b);
		}
		if (lighten) {
			r = std::min(255, int(r * 1.05 + 2));
			g = std::min(255, int(g * 1.05 + 2));
			b = std::min(255, int(b * 1.05 + 2));
			if (r == 255 && g == 255 && b == 255) break;
		} else {
			r = std::max(0, int(r * 0.95 - 2));
			g = std::max(0, int(g * 0.95 - 2));
			b = std::max(0, int(b * 0.95 - 2));
			if (r == 0 && g == 0 && b == 0) break;
		}
	}
	return lighten ? QColor(255, 255, 255) : QColor(0, 0, 0);
}

QColor EnsureWcagAaBody(const QColor &text, const QColor &bg) {
	return EnsureWcagAaInternal(text, bg, 4.5);
}

QColor EnsureWcagAaLarge(const QColor &text, const QColor &bg) {
	return EnsureWcagAaInternal(text, bg, 3.0);
}

SurfaceTokens ComputeSurfaceTokens(
		bool dark,
		const QColor &bg,
		const QColor &fg,
		const QColor &accent) {
	SurfaceTokens tokens;
	tokens.surface = bg;
	tokens.primary = accent;
	tokens.onPrimary = (RelativeLuminance(accent) > 0.4)
		? QColor(0, 0, 0)
		: QColor(255, 255, 255);

	if (dark) {
		tokens.surfaceDim = Mix(bg, QColor(0, 0, 0), 0.15);
		tokens.surfaceBright = Mix(bg, fg, 0.10);
		tokens.surfaceContainerLowest = Mix(bg, QColor(0, 0, 0), 0.20);
		tokens.surfaceContainerLow = Mix(bg, fg, 0.04);
		tokens.surfaceContainer = Mix(bg, fg, 0.08);
		tokens.surfaceContainerHigh = Mix(bg, fg, 0.12);
		tokens.surfaceContainerHighest = Mix(bg, fg, 0.16);

		tokens.onSurface = EnsureWcagAaBody(Mix(fg, QColor(255, 255, 255), 0.10), tokens.surfaceContainer);
		tokens.onSurfaceVariant = EnsureWcagAaBody(Mix(fg, bg, 0.30), tokens.surfaceContainerHighest);
		tokens.outline = Mix(fg, bg, 0.45);
		tokens.outlineVariant = Mix(fg, bg, 0.70);

		tokens.primaryContainer = Mix(bg, accent, 0.25);
		tokens.onPrimaryContainer = EnsureWcagAaBody(Mix(accent, QColor(255, 255, 255), 0.40), tokens.primaryContainer);
		tokens.secondaryContainer = Mix(bg, fg, 0.15);
		tokens.onSecondaryContainer = EnsureWcagAaBody(tokens.onSurface, tokens.secondaryContainer);
	} else {
		tokens.surfaceDim = Mix(bg, fg, 0.05);
		tokens.surfaceBright = QColor(255, 255, 255);
		tokens.surfaceContainerLowest = QColor(255, 255, 255);
		tokens.surfaceContainerLow = Mix(bg, fg, 0.03);
		tokens.surfaceContainer = Mix(bg, fg, 0.06);
		tokens.surfaceContainerHigh = Mix(bg, fg, 0.09);
		tokens.surfaceContainerHighest = Mix(bg, fg, 0.12);

		tokens.onSurface = EnsureWcagAaBody(fg, tokens.surfaceContainer);
		tokens.onSurfaceVariant = EnsureWcagAaBody(Mix(fg, bg, 0.35), tokens.surfaceContainerHighest);
		tokens.outline = Mix(fg, bg, 0.50);
		tokens.outlineVariant = Mix(fg, bg, 0.80);

		tokens.primaryContainer = Mix(bg, accent, 0.15);
		tokens.onPrimaryContainer = EnsureWcagAaBody(Mix(accent, QColor(0, 0, 0), 0.30), tokens.primaryContainer);
		tokens.secondaryContainer = Mix(bg, fg, 0.10);
		tokens.onSecondaryContainer = EnsureWcagAaBody(tokens.onSurface, tokens.secondaryContainer);
	}
	return tokens;
}

SurfaceTokens CurrentSurfaceTokens() {
	const auto bg = st::windowBg->c;
	const auto fg = st::windowFg->c;
	const auto accent = st::windowBgActive->c;
	const auto dark = (RelativeLuminance(bg) < 0.35);
	return ComputeSurfaceTokens(dark, bg, fg, accent);
}

QColor SurfaceContainerLowest() { return CurrentSurfaceTokens().surfaceContainerLowest; }
QColor SurfaceContainerLow() { return CurrentSurfaceTokens().surfaceContainerLow; }
QColor SurfaceContainerDefault() { return CurrentSurfaceTokens().surfaceContainer; }
QColor SurfaceContainerHigh() { return CurrentSurfaceTokens().surfaceContainerHigh; }
QColor SurfaceContainerHighest() { return CurrentSurfaceTokens().surfaceContainerHighest; }

double StateLayerOpacity(StateLayer state, bool expressive) {
	switch (state) {
	case StateLayer::Hover: return 0.08;
	case StateLayer::Focus: return 0.10;
	case StateLayer::Pressed: return expressive ? 0.12 : 0.10;
	case StateLayer::Dragged: return 0.16;
	}
	return 0.;
}

QColor StateLayerColor(const QColor &contentColor, StateLayer state, bool expressive) {
	auto c = contentColor;
	c.setAlphaF(StateLayerOpacity(state, expressive));
	return c;
}

void PaintStateLayer(
		QPainter &p,
		const QRectF &rect,
		const QColor &contentColor,
		StateLayer state,
		double radius,
		bool expressive) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	p.setBrush(StateLayerColor(contentColor, state, expressive));
	if (radius <= 0.) {
		p.drawRect(rect);
	} else {
		p.drawRoundedRect(rect, radius, radius);
	}
}

SwitchSpec GetSwitchSpec() {
	return SwitchSpec{};
}

SliderSpec GetSliderSpec(bool expressive) {
	SliderSpec spec;
	return spec;
}

ProgressBarSpec GetProgressBarSpec(bool expressive) {
	ProgressBarSpec spec;
	return spec;
}

void PaintSwitch(
		QPainter &p,
		const QRectF &rect,
		bool checked,
		bool hovered,
		bool pressed,
		const SurfaceTokens &tokens) {
	auto hq = PainterHighQualityEnabler(p);
	const auto spec = GetSwitchSpec();
	const auto r = QRectF(rect.topLeft(), QSizeF(spec.trackWidth, spec.trackHeight));
	const auto radius = spec.trackRadius;

	if (checked) {
		p.setPen(Qt::NoPen);
		p.setBrush(tokens.primary);
		p.drawRoundedRect(r, radius, radius);
	} else {
		p.setPen(QPen(tokens.outline, 2.));
		p.setBrush(tokens.surfaceContainerHighest);
		p.drawRoundedRect(r.adjusted(1., 1., -1., -1.), radius - 1., radius - 1.);
	}

	const auto thumbSize = pressed
		? spec.thumbPressedSize
		: (checked ? spec.thumbSelectedSize : spec.thumbUnselectedSize);
	const auto trackInner = spec.trackWidth - thumbSize - 8.;
	const auto thumbX = checked ? (r.left() + 4. + trackInner) : (r.left() + 4.);
	const auto thumbY = r.top() + (spec.trackHeight - thumbSize) / 2.;
	const auto thumbRect = QRectF(thumbX, thumbY, thumbSize, thumbSize);

	p.setPen(Qt::NoPen);
	p.setBrush(checked ? tokens.onPrimary : tokens.outline);
	p.drawEllipse(thumbRect);

	if (hovered) {
		PaintStateLayer(
			p,
			thumbRect.adjusted(-6., -6., 6., 6.),
			checked ? tokens.primary : tokens.onSurface,
			StateLayer::Hover,
			(thumbSize + 12.) / 2.);
	}
}

void PaintSlider(
		QPainter &p,
		const QRectF &rect,
		double progress,
		bool hovered,
		bool pressed,
		bool expressive,
		const SurfaceTokens &tokens) {
	auto hq = PainterHighQualityEnabler(p);
	const auto spec = GetSliderSpec(expressive);
	const auto trackH = expressive ? spec.trackHeightExpressive : spec.trackHeightStandard;
	const auto cy = rect.center().y();
	const auto trackRect = QRectF(rect.left(), cy - trackH / 2., rect.width(), trackH);
	const auto clampedProg = std::clamp(progress, 0., 1.);
	const auto activeW = trackRect.width() * clampedProg;
	const auto activeRect = QRectF(trackRect.left(), trackRect.top(), activeW, trackH);
	const auto inactiveLeft = activeRect.right() + (expressive ? spec.stopIndicatorGap : 0.);
	const auto inactiveW = std::max(0., trackRect.right() - inactiveLeft);
	const auto inactiveRect = QRectF(inactiveLeft, trackRect.top(), inactiveW, trackH);

	p.setPen(Qt::NoPen);
	p.setBrush(tokens.primary);
	p.drawRoundedRect(activeRect, trackH / 2., trackH / 2.);

	if (inactiveW > 0.) {
		p.setBrush(tokens.surfaceContainerHighest);
		p.drawRoundedRect(inactiveRect, trackH / 2., trackH / 2.);
	}

	if (expressive) {
		const auto thumbW = spec.thumbWidthExpressive;
		const auto thumbH = spec.thumbHeightExpressive;
		const auto thumbRect = QRectF(activeRect.right() - thumbW / 2., cy - thumbH / 2., thumbW, thumbH);
		p.setBrush(tokens.onPrimary);
		p.drawRoundedRect(thumbRect, thumbW / 2., thumbW / 2.);
	} else {
		const auto thumbR = spec.thumbRadiusStandard;
		const auto thumbRect = QRectF(activeRect.right() - thumbR, cy - thumbR, thumbR * 2., thumbR * 2.);
		p.setBrush(tokens.primary);
		p.drawEllipse(thumbRect);
	}
}

void PaintLinearProgressBar(
		QPainter &p,
		const QRectF &rect,
		double progress,
		bool expressive,
		const SurfaceTokens &tokens) {
	auto hq = PainterHighQualityEnabler(p);
	const auto spec = GetProgressBarSpec(expressive);
	const auto trackH = expressive ? spec.trackHeightExpressive : spec.trackHeightStandard;
	const auto cy = rect.center().y();
	const auto trackRect = QRectF(rect.left(), cy - trackH / 2., rect.width(), trackH);
	const auto clampedProg = std::clamp(progress, 0., 1.);

	p.setPen(Qt::NoPen);
	p.setBrush(tokens.surfaceContainerHighest);
	p.drawRoundedRect(trackRect, trackH / 2., trackH / 2.);

	const auto activeW = trackRect.width() * clampedProg;
	if (activeW > 0.) {
		p.setBrush(tokens.primary);
		p.drawRoundedRect(QRectF(trackRect.left(), trackRect.top(), activeW, trackH), trackH / 2., trackH / 2.);
	}

	if (clampedProg < 0.98) {
		const auto dotSize = spec.stopIndicatorSize;
		p.setBrush(tokens.primary);
		p.drawEllipse(QRectF(trackRect.right() - dotSize - 2., cy - dotSize / 2., dotSize, dotSize));
	}
}

void PaintCircularProgressIndicator(
		QPainter &p,
		const QRectF &rect,
		double progress,
		double penWidth,
		const SurfaceTokens &tokens) {
	auto hq = PainterHighQualityEnabler(p);
	const auto r = rect.adjusted(penWidth / 2., penWidth / 2., -penWidth / 2., -penWidth / 2.);
	QPen trackPen(tokens.surfaceContainerHighest, penWidth, Qt::SolidLine, Qt::RoundCap);
	p.setBrush(Qt::NoBrush);
	p.setPen(trackPen);
	p.drawEllipse(r);

	QPen activePen(tokens.primary, penWidth, Qt::SolidLine, Qt::RoundCap);
	p.setPen(activePen);
	const auto spanAngle = int(std::clamp(progress, 0., 1.) * 360. * 16.);
	p.drawArc(r, 90 * 16, -spanAngle);
}

namespace {

void PaintPreview(QPainter &p, QRect rect, const PreviewState &state) {
	auto hq = PainterHighQualityEnabler(p);
	p.setPen(Qt::NoPen);
	const auto md3 = state.enabled;
	const auto tokens = CurrentSurfaceTokens();
	const auto outer = md3 ? ShapeRadius(ShapeScale::Large) : 6.;
	const auto frame = md3 ? tokens.surfaceContainerLowest : Mix(st::windowBg->c, st::windowFg->c, 0.03);
	const auto card = md3 ? tokens.surfaceContainerHigh : Mix(st::windowBg->c, st::windowFg->c, 0.09);
	const auto bg = md3 ? tokens.surfaceContainer : st::windowBg->c;
	const auto fg = tokens.onSurface;
	const auto sub = tokens.onSurfaceVariant;

	p.setBrush(frame);
	if (md3) {
		PaintSquircle(p, QRectF(rect), frame, outer, 0.65);
	} else {
		p.drawRoundedRect(rect, outer, outer);
	}

	const auto pad = 12;
	const auto inner = rect.marginsRemoved({ pad, pad, pad, pad });
	const auto listWidth = inner.width() * 4 / 10;
	const auto list = QRect(inner.x(), inner.y(), listWidth, inner.height());
	const auto chat = QRect(
		list.x() + list.width() + pad,
		inner.y(),
		inner.width() - listWidth - pad,
		inner.height());

	const auto containerRadius = md3 ? ShapeRadius(ShapeScale::Medium) : 3.;
	if (md3) {
		PaintSquircle(p, QRectF(list), bg, containerRadius, 0.65);
		PaintSquircle(p, QRectF(chat), bg, containerRadius, 0.65);
	} else {
		p.setBrush(bg);
		p.drawRoundedRect(list, containerRadius, containerRadius);
		p.drawRoundedRect(chat, containerRadius, containerRadius);
	}

	const auto rowHeight = list.height() / 4;
	const auto colors = std::array<QColor, 4>{
		QColor(0xF4, 0x8F, 0xB1),
		QColor(0x81, 0xC7, 0x84),
		QColor(0x64, 0xB5, 0xF6),
		QColor(0xFF, 0xB7, 0x4D),
	};
	for (auto i = 0; i != 4; ++i) {
		const auto row = QRect(
			list.x(),
			list.y() + i * rowHeight,
			list.width(),
			rowHeight);
		const auto active = (i == 1);
		if (active) {
			if (md3 && state.rounded) {
				const auto activeRect = QRectF(row).marginsRemoved({ 4., 2., 4., 2. });
				PaintSquircle(p, activeRect, tokens.primaryContainer, ShapeRadius(ShapeScale::Medium), 0.65);
			} else {
				p.setBrush(st::dialogsBgActive->c);
				p.drawRect(row);
			}
		}
		const auto size = rowHeight - 12;
		const auto avatarRect = QRectF(
			row.x() + 8,
			row.y() + (rowHeight - size) / 2,
			size,
			size);
		if (md3) {
			PaintSquircle(p, avatarRect, colors[i], size * 0.35, 0.70);
		} else {
			p.setBrush(colors[i]);
			p.drawEllipse(avatarRect);
		}
		const auto textLeft = row.x() + 16 + size;
		const auto textWidth = row.width() - (textLeft - row.x()) - 10;
		PaintBar(p, QRectF(
			textLeft,
			row.y() + rowHeight * 0.3,
			textWidth * 0.7,
			6), active ? tokens.onPrimaryContainer : fg);
		PaintBar(p, QRectF(
			textLeft,
			row.y() + rowHeight * 0.58,
			textWidth,
			5), active ? tokens.onPrimaryContainer : sub);
	}

	const auto groupLeft = chat.x() + 10;
	const auto groupWidth = chat.width() - 20;
	const auto groupTop = chat.y() + 10;
	const auto itemHeight = 26;
	for (auto i = 0; i != 3; ++i) {
		const auto item = QRectF(
			groupLeft,
			groupTop + i * itemHeight,
			groupWidth,
			itemHeight);
		if (md3 && state.grouped) {
			const auto shape = item.marginsRemoved({
				0.,
				(i > 0) ? 1. : 0.,
				0.,
				(i < 2) ? 1. : 0. });
			const auto big = ShapeRadius(ShapeScale::Medium);
			const auto small = ShapeRadius(ShapeScale::ExtraSmall);
			const auto topR = (i == 0) ? big : small;
			const auto bottomR = (i == 2) ? big : small;
			auto path = QPainterPath();
			path.moveTo(shape.left() + topR, shape.top());
			path.lineTo(shape.right() - topR, shape.top());
			path.quadTo(shape.right(), shape.top(), shape.right(), shape.top() + topR);
			path.lineTo(shape.right(), shape.bottom() - bottomR);
			path.quadTo(shape.right(), shape.bottom(), shape.right() - bottomR, shape.bottom());
			path.lineTo(shape.left() + bottomR, shape.bottom());
			path.quadTo(shape.left(), shape.bottom(), shape.left(), shape.bottom() - bottomR);
			path.lineTo(shape.left(), shape.top() + topR);
			path.quadTo(shape.left(), shape.top(), shape.left() + topR, shape.top());
			p.fillPath(path, card);
		} else if (i > 0) {
			p.setBrush(tokens.outlineVariant);
			p.drawRect(QRectF(item.left(), item.top(), item.width(), 1));
		}
		PaintBar(p, QRectF(
			item.left() + 12,
			item.top() + itemHeight / 2. - 3,
			item.width() * 0.45,
			6), fg);
		p.setBrush(md3 ? tokens.primary : sub);
		const auto toggle = QRectF(
			item.right() - 34,
			item.top() + itemHeight / 2. - 7,
			24,
			14);
		p.drawRoundedRect(toggle, 7., 7.);
	}

	const auto fieldHeight = 30.;
	const auto fieldTop = chat.bottom() - fieldHeight - 8.;
	const auto sendSize = fieldHeight;
	if (md3 && state.floating) {
		const auto field = QRectF(
			chat.x() + 8,
			fieldTop,
			chat.width() - 24 - sendSize,
			fieldHeight);
		PaintSquircle(p, field, card, fieldHeight / 2, 0.65);
		PaintSquircle(p, QRectF(field.right() + 8, fieldTop, sendSize, sendSize), tokens.primary, sendSize / 2, 0.65);
		PaintBar(p, QRectF(field.left() + 14, fieldTop + 12, field.width() * 0.4, 6), sub);
	} else {
		p.setBrush(card);
		const auto bar = QRectF(
			chat.x(),
			fieldTop - 4,
			chat.width(),
			chat.bottom() - fieldTop + 5);
		p.drawRect(bar);
		PaintBar(p, QRectF(bar.left() + 14, fieldTop + 12, bar.width() * 0.4, 6), sub);
	}
}

not_null<Ui::SettingsButton*> AddOption(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> title,
		const QString &key,
		Fn<void(bool)> changed) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		std::move(title),
		st::settingsButtonNoIcon));
	button->toggleOn(rpl::single(Bool(key)));
	button->toggledChanges(
	) | rpl::filter([=](bool value) {
		return value != Bool(key);
	}) | rpl::on_next([=](bool value) {
		Config::Instance().setBool(key, value);
		changed(value);
	}, button->lifetime());
	return button;
}

} // namespace

void ApplyBeforeStyles() {
	const auto on = Enabled();
	style::SetMd3(on, on ? 1 : 0, false);
	style::SetMd3Grouped(on && Bool(u"md3_grouped"_q));
	style::SetMd3Icons(Bool(u"md3_icons"_q));
	Ui::SetSmoothScroll(Bool(u"smooth_scroll"_q));
	static auto smoothLifetime = rpl::lifetime();
	if (!smoothLifetime) {
		Config::Instance().boolChanges(
			u"smooth_scroll"_q
		) | rpl::on_next([](bool enabled) {
			Ui::SetSmoothScroll(enabled);
		}, smoothLifetime);
	}
	style::SetMd3Floating(on && Bool(u"md3_floating_field"_q));
	anim::SetExpressive(on && Bool(u"md3_expressive"_q));
	if (on
		&& Bool(u"md3_google_sans"_q)
		&& Core::App().settings().customFontFamily().isEmpty()) {
		style::SetCustomFont(QString::fromLatin1(kGoogleSans));
	}
}

bool Enabled() {
	return Bool(u"md3"_q);
}

bool RoundedSelection() {
	return Enabled() && Bool(u"md3_rounded_selection"_q);
}

bool FloatingField() {
	return Enabled() && Bool(u"md3_floating_field"_q);
}

bool Expressive() {
	return Enabled() && Bool(u"md3_expressive"_q);
}

bool DrawerCorners() {
	return Enabled() && Bool(u"md3_drawer_corners"_q);
}

bool ProgressBars() {
	return Enabled() && Bool(u"md3_progress"_q);
}

bool MonetDynamic() {
	return Bool(u"md3_monet_dynamic"_q);
}

QString MonetColor() {
	return String(u"md3_monet_color"_q);
}

const std::vector<MonetPreset> &MonetPresets() {
	static const auto presets = std::vector<MonetPreset>{
		{ u"Pixel Coral"_q, QColor(0xFF, 0x84, 0x5E) },
		{ u"Material Orange"_q, QColor(0xFE, 0xBB, 0x5B) },
		{ u"Material Violet"_q, QColor(0xB6, 0x94, 0xF9) },
		{ u"Material Green"_q, QColor(0x9A, 0xD1, 0x64) },
		{ u"Material Cyan"_q, QColor(0x5B, 0xCB, 0xE3) },
		{ u"Material Blue"_q, QColor(0x5C, 0xAF, 0xFA) },
		{ u"Material Pink"_q, QColor(0xFF, 0x8A, 0xAC) },
		{ u"Molten Flame"_q, QColor(0xE9, 0x59, 0x33) },
		{ u"Deep Ocean"_q, QColor(0x3B, 0x78, 0xE7) },
		{ u"Forest Jade"_q, QColor(0x2E, 0x7D, 0x32) },
		{ u"Lavender Mist"_q, QColor(0x9C, 0x7A, 0x97) },
		{ u"Monochrome"_q, QColor(0x75, 0x75, 0x75) },
	};
	return presets;
}

void SetMonetColor(const QString &hex) {
	Config::Instance().setString(u"md3_monet_color"_q, hex);
	Config::Instance().setBool(u"md3_monet_dynamic"_q, true);
	RepaintAll();
}

void ShowMonetColorBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_exd_monet_color());
		box->setWidth(style::ConvertScale(440));

		const auto container = box->verticalLayout();
		const auto &presets = MonetPresets();
		const auto count = int(presets.size());
		const auto currentHex = MonetColor().toUpper();

		const auto selected = box->lifetime().make_state<rpl::variable<QString>>(currentHex);

		constexpr auto kCols = 4;
		constexpr auto kRows = 3;
		const auto cellW = style::ConvertScale(94);
		const auto cellH = style::ConvertScale(80);
		const auto gap = style::ConvertScale(8);
		const auto padX = style::ConvertScale(16);
		const auto padY = style::ConvertScale(12);

		const auto grid = container->add(object_ptr<Ui::RpWidget>(container));
		const auto totalH = padY * 2 + kRows * cellH + (kRows - 1) * gap;
		grid->resize(padX * 2 + kCols * cellW + (kCols - 1) * gap, totalH);
		grid->setMinimumHeight(totalH);

		for (auto i = 0; i < count; ++i) {
			const auto row = i / kCols;
			const auto col = i % kCols;
			const auto btn = Ui::CreateChild<Ui::AbstractButton>(grid);
			btn->setGeometry(
				padX + col * (cellW + gap),
				padY + row * (cellH + gap),
				cellW,
				cellH);
			const auto &preset = presets[i];
			const auto hex = preset.color.name().toUpper();

			btn->paintRequest() | rpl::on_next([=] {
				auto p = QPainter(btn);
				auto hq = PainterHighQualityEnabler(p);
				const auto active = (selected->current() == hex);
				const auto r = QRectF(btn->rect()).adjusted(1., 1., -1., -1.);

				if (active) {
					auto fill = preset.color;
					fill.setAlphaF(0.2);
					p.setBrush(fill);
					p.setPen(QPen(preset.color, style::ConvertScale(2.)));
				} else {
					auto fill = st::windowFg->c;
					fill.setAlphaF(btn->isOver() ? 0.08 : 0.03);
					p.setBrush(fill);
					p.setPen(Qt::NoPen);
				}
				p.drawRoundedRect(r, style::ConvertScale(12.), style::ConvertScale(12.));

				const auto circleSize = style::ConvertScale(32.);
				const auto circleRect = QRectF(
					(btn->width() - circleSize) * 0.5,
					style::ConvertScale(10.),
					circleSize,
					circleSize);
				p.setPen(Qt::NoPen);
				p.setBrush(preset.color);
				p.drawEllipse(circleRect);

				p.setFont(st::normalFont);
				p.setPen(active ? preset.color : st::windowFg->c);
				const auto textRect = QRect(
					style::ConvertScale(2),
					int(circleRect.bottom()) + style::ConvertScale(4),
					btn->width() - style::ConvertScale(4),
					btn->height() - int(circleRect.bottom()) - style::ConvertScale(5));
				p.drawText(
					textRect,
					Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
					preset.name);
			}, btn->lifetime());

			selected->changes() | rpl::on_next([=] {
				btn->update();
			}, btn->lifetime());

			btn->setClickedCallback([=] {
				*selected = hex;
				SetMonetColor(hex);
			});
		}

		Ui::AddSkip(container);
		Ui::AddDividerText(
			container,
			tr::lng_exd_monet_color_about());

		box->addLeftButton(rpl::single(u"System Accent"_q), [=] {
			Config::Instance().setString(u"md3_monet_color"_q, QString());
			Config::Instance().setBool(u"md3_monet_dynamic"_q, true);
			RepaintAll();
			box->closeBox();
		});

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowIconPacksBox(not_null<Window::SessionController*> controller) {
	const auto options = std::vector<QString>{
		tr::lng_exd_icon_pack_default(tr::now),
		tr::lng_exd_icon_pack_solar(tr::now),
		tr::lng_exd_icon_pack_remix(tr::now),
		tr::lng_exd_icon_pack_material(tr::now),
		tr::lng_exd_icon_pack_halloween(tr::now),
	};
	const auto current = Config::Instance().valueOf(u"icon_pack"_q).toInt(0);
	controller->show(Box([=](not_null<Ui::GenericBox*> choice) {
		SingleChoiceBox(choice, {
			.title = tr::lng_exd_icon_packs(),
			.options = options,
			.initialSelection = std::clamp(current, 0, int(options.size()) - 1),
			.callback = [=](int index) {
				Config::Instance().setInt(u"icon_pack"_q, index);
				Config::Instance().setBool(u"md3_icons"_q, index != 0);
				style::SetMd3Icons(index != 0);
				RepaintAll();
			},
		});
	}));
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(tr::lng_exd_md3_title());
		box->setWidth(st::boxWideWidth);
		const auto content = box->verticalLayout();
		const auto restart = box->lifetime().make_state<bool>(false);
		const auto state = box->lifetime().make_state<PreviewState>(
			ReadState());

		Ui::AddSkip(content);
		const auto preview = content->add(
			object_ptr<Ui::FixedHeightWidget>(
				content,
				style::ConvertScale(150)),
			style::Md3Grouped()
				? style::margins(
					style::ConvertScale(10),
					0,
					style::ConvertScale(10),
					0)
				: st::boxRowPadding);
		preview->paintRequest(
		) | rpl::on_next([=] {
			auto p = QPainter(preview);
			PaintPreview(p, preview->rect(), *state);
		}, preview->lifetime());
		const auto refresh = [=] {
			*state = ReadState();
			preview->update();
		};

		Ui::AddSkip(content);
		AddOption(content, tr::lng_exd_md3_title(), u"md3"_q, [=](bool) {
			*restart = true;
			refresh();
		});
		Ui::AddSkip(content);
		Ui::AddDividerText(content, tr::lng_exd_md3_about());

		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::lng_exd_md3_view());
		const auto fontName = [] {
			return Bool(u"md3_google_sans"_q)
				? QString::fromLatin1(kGoogleSans)
				: tr::lng_exd_md3_font_telegram(tr::now);
		};
		const auto fontLabel = content->lifetime().make_state<
			rpl::variable<QString>>(fontName());
		Settings::AddButtonWithLabel(
			content,
			tr::lng_exd_md3_font(),
			fontLabel->value(),
			st::settingsButtonNoIcon
		)->setClickedCallback([=] {
			const auto options = std::vector<QString>{
				QString::fromLatin1(kGoogleSans),
				tr::lng_exd_md3_font_telegram(tr::now),
			};
			controller->show(Box([=](not_null<Ui::GenericBox*> choice) {
				SingleChoiceBox(choice, {
					.title = tr::lng_exd_md3_font(),
					.options = options,
					.initialSelection = Bool(u"md3_google_sans"_q) ? 0 : 1,
					.callback = [=](int index) {
						const auto google = (index == 0);
						if (google != Bool(u"md3_google_sans"_q)) {
							Config::Instance().setBool(
								u"md3_google_sans"_q,
								google);
							*fontLabel = fontName();
							*restart = true;
						}
					},
				});
			}));
		});
		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::lng_exd_md3_icons());
		const auto cardHeight = st::exteraIconsCardHeight;
		const auto labelSkip = style::ConvertScale(8);
		const auto labelHeight = st::semiboldFont->height;
		const auto picker = content->add(
			object_ptr<Ui::FixedHeightWidget>(
				content,
				cardHeight + labelSkip + labelHeight + style::ConvertScale(6)),
			st::boxRowPadding);
		const auto selected = picker->lifetime().make_state<
			rpl::variable<bool>>(Bool(u"md3_icons"_q));
		for (auto i = 0; i != 2; ++i) {
			const auto md3 = (i == 1);
			const auto button = Ui::CreateChild<Ui::AbstractButton>(picker);
			picker->sizeValue(
			) | rpl::on_next([=](QSize size) {
				const auto gap = style::ConvertScale(12);
				const auto width = (size.width() - gap) / 2;
				button->setGeometry(i * (width + gap), 0, width, size.height());
			}, button->lifetime());
			button->paintRequest(
			) | rpl::on_next([=] {
				auto p = QPainter(button);
				PainterHighQualityEnabler hq(p);
				const auto active = (selected->current() == md3);
				const auto accent = st::windowActiveTextFg->c;
				const auto border = style::ConvertScale(2);
				const auto card = QRectF(button->rect().left(), 0, button->width(), cardHeight)
					.marginsRemoved(QMarginsF(border, border, border, border) / 2.);
				const auto radius = card.height() / 2.;
				p.setPen(active ? QPen(accent, border) : Qt::NoPen);
				p.setBrush(active ? Mix(st::windowBgOver->c, accent, 0.12) : st::windowBgOver->c);
				p.drawRoundedRect(card, radius, radius);
				const auto icons = md3
					? std::array{
						&st::exteraIconsNoMd3MgFolder,
						&st::exteraIconsNoMd3MgChats,
						&st::exteraIconsNoMd3MgSend }
					: std::array{
						&st::exteraIconsNoMd3StdFolder,
						&st::exteraIconsNoMd3StdChats,
						&st::exteraIconsNoMd3StdSend };
				const auto gap = style::ConvertScale(16);
				auto total = gap * 2;
				for (const auto icon : icons) {
					total += icon->width();
				}
				auto x = int(card.center().x()) - total / 2;
				const auto color = active ? accent : st::windowSubTextFg->c;
				for (const auto icon : icons) {
					icon->paint(
						p,
						x,
						int(card.center().y()) - icon->height() / 2,
						button->width(),
						color);
					x += icon->width() + gap;
				}
				p.setFont(st::semiboldFont);
				p.setPen(active ? accent : st::windowFg->c);
				p.drawText(
					QRect(0, cardHeight + labelSkip, button->width(), labelHeight),
					md3
						? tr::lng_exd_md3_icons_material(tr::now)
						: tr::lng_exd_md3_icons_standard(tr::now),
					style::al_top);
			}, button->lifetime());
			selected->changes(
			) | rpl::on_next([=] {
				button->update();
			}, button->lifetime());
			button->setClickedCallback([=] {
				if (selected->current() == md3) {
					return;
				}
				*selected = md3;
				Config::Instance().setBool(u"md3_icons"_q, md3);
				*restart = true;
			});
		}
		Ui::AddSkip(content);
		AddOption(
			content,
			tr::lng_exd_md3_grouped(),
			u"md3_grouped"_q,
			[=](bool value) {
				style::SetMd3Grouped(Enabled() && value);
				refresh();
				RepaintAll();
			});
		AddOption(
			content,
			tr::lng_exd_md3_rounded_selection(),
			u"md3_rounded_selection"_q,
			[=](bool) {
				refresh();
				RepaintAll();
			});
		AddOption(
			content,
			tr::lng_exd_md3_floating_field(),
			u"md3_floating_field"_q,
			[=](bool value) {
				style::SetMd3Floating(Enabled() && value);
				refresh();
				RepaintAll();
			});
		AddOption(
			content,
			tr::lng_exd_md3_drawer_corners(),
			u"md3_drawer_corners"_q,
			[=](bool) { refresh(); });
		AddOption(
			content,
			tr::lng_exd_md3_expressive(),
			u"md3_expressive"_q,
			[=](bool value) { anim::SetExpressive(Enabled() && value); });
		Ui::AddSkip(content);
		Ui::AddDividerText(content, tr::lng_exd_md3_view_about());

		Ui::AddSkip(content);
		Ui::AddSubsectionTitle(content, tr::lng_exd_md3_controls());
		AddOption(
			content,
			tr::lng_extera_md3_switches(),
			u"md3_switches"_q,
			[=](bool) { RepaintAll(); });
		AddOption(
			content,
			tr::lng_exd_md3_switches_muted(),
			u"md3_switches_muted"_q,
			[=](bool) { RepaintAll(); });
		AddOption(
			content,
			tr::lng_extera_md3_sliders(),
			u"md3_sliders"_q,
			[=](bool) { RepaintAll(); });
		AddOption(
			content,
			tr::lng_exd_md3_progress(),
			u"md3_progress"_q,
			[=](bool) { RepaintAll(); });
		Ui::AddSkip(content);

		Ui::AddSubsectionTitle(content, tr::lng_exd_monet_dynamic());
		AddOption(
			content,
			tr::lng_exd_monet_dynamic(),
			u"md3_monet_dynamic"_q,
			[=](bool) { RepaintAll(); });
		Settings::AddButtonWithLabel(
			content,
			tr::lng_exd_monet_color(),
			Config::Instance().stringChanges(
				u"md3_monet_color"_q
			) | rpl::map([](const QString &c) {
				return c.isEmpty() ? u"System Accent"_q : c;
			}),
			st::settingsButtonNoIcon
		)->setClickedCallback([=] {
			ShowMonetColorBox(controller);
		});
		Settings::AddButtonWithLabel(
			content,
			tr::lng_exd_icon_packs(),
			Config::Instance().intChanges(
				u"icon_pack"_q
			) | rpl::map([](int pack) {
				switch (pack) {
				case 1: return tr::lng_exd_icon_pack_solar(tr::now);
				case 2: return tr::lng_exd_icon_pack_remix(tr::now);
				case 3: return tr::lng_exd_icon_pack_material(tr::now);
				case 4: return tr::lng_exd_icon_pack_halloween(tr::now);
				default: return tr::lng_exd_icon_pack_default(tr::now);
				}
			}),
			st::settingsButtonNoIcon
		)->setClickedCallback([=] {
			ShowIconPacksBox(controller);
		});
		Ui::AddSkip(content);

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
		box->boxClosing() | rpl::on_next([=] {
			if (*restart) {
				RequestRestart(controller);
			}
		}, box->lifetime());
	}));
}

bool ShowIntroIfNeeded(not_null<Window::SessionController*> controller) {
	if (Bool(u"md3_intro_shown"_q)) {
		return false;
	}
	Config::Instance().setBool(u"md3_intro_shown"_q, true);
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		Ui::ConfirmBox(box, {
			.text = tr::lng_exd_md3_intro_text(),
			.confirmed = [](Fn<void()> close) { close(); },
			.cancelled = [](Fn<void()> close) {
				close();
				if (Enabled()) {
					Config::Instance().setBool(u"md3"_q, false);
					Config::Instance().saveNow();
					Core::Restart();
				}
			},
			.confirmText = tr::lng_exd_md3_intro_keep(),
			.cancelText = tr::lng_exd_md3_intro_revert(),
			.title = tr::lng_exd_md3_intro_title(),
			.strictCancel = true,
		});
		const auto &button = st::defaultBoxButton;
		const auto text = [&](const QString &value) {
			return button.style.font->width(value) - std::min(button.width, 0);
		};
		const auto padding = st::defaultBox.buttonPadding;
		const auto buttons = text(tr::lng_exd_md3_intro_keep(tr::now))
			+ text(tr::lng_exd_md3_intro_revert(tr::now))
			+ padding.left()
			+ padding.right()
			+ st::defaultBox.buttonPadding.left() * 2
			+ style::ConvertScale(24);
		box->setWidth(std::max(st::boxWidth, buttons));
	}));
	return true;
}

} // namespace Extera::Md3
