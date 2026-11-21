/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_avatar_shapes.h"

#include "extera/extera_config.h"
#include "extera/molten_media_carousel.h"
#include "lang/lang_keys.h"
#include "ui/chat/chat_style.h"
#include "ui/chat/chat_style_radius.h"
#include "ui/chat/message_bubble.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/style/style_core.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/continuous_sliders.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/padding_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

#include <QtGui/QImage>
#include <QtGui/QLinearGradient>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QTransform>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace Extera::AvatarShapes {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
	double x = 0.;
	double y = 0.;

	[[nodiscard]] Vec2 operator+(const Vec2 &o) const {
		return { x + o.x, y + o.y };
	}
	[[nodiscard]] Vec2 operator-(const Vec2 &o) const {
		return { x - o.x, y - o.y };
	}
	[[nodiscard]] Vec2 operator*(double s) const {
		return { x * s, y * s };
	}
	[[nodiscard]] double length() const {
		return std::hypot(x, y);
	}
	[[nodiscard]] double angleDegrees() const {
		return std::atan2(y, x) * 180. / kPi;
	}
	[[nodiscard]] Vec2 rotatedDegrees(
			double angleDeg,
			Vec2 center = {}) const {
		const auto rad = angleDeg * kPi / 180.;
		const auto c = std::cos(rad);
		const auto s = std::sin(rad);
		const auto d = *this - center;
		return Vec2{ d.x * c - d.y * s, d.x * s + d.y * c } + center;
	}
};

struct CornerRounding {
	double radius = 0.;
	double smoothing = 0.;
};

constexpr auto kUnrounded = CornerRounding{ 0., 0. };
constexpr auto kRound15 = CornerRounding{ 0.15, 0. };
constexpr auto kRound20 = CornerRounding{ 0.20, 0. };
constexpr auto kRound30 = CornerRounding{ 0.30, 0. };
constexpr auto kRound50 = CornerRounding{ 0.50, 0. };
constexpr auto kRound100 = CornerRounding{ 1.00, 0. };

struct PointNRound {
	Vec2 o;
	CornerRounding r = kUnrounded;
};

struct PolySpec {
	std::vector<PointNRound> points;
	bool isCircle = false;
	double scaleX = 1.;
	double scaleY = 1.;
	double rotateDeg = 0.;
};

[[nodiscard]] std::vector<PointNRound> DoRepeat(
		const std::vector<PointNRound> &points,
		int reps,
		Vec2 center,
		bool mirroring) {
	std::vector<PointNRound> out;
	if (points.empty() || reps <= 0) {
		return out;
	}
	if (mirroring) {
		std::vector<double> angles;
		std::vector<double> distances;
		angles.reserve(points.size());
		distances.reserve(points.size());
		for (const auto &pt : points) {
			const auto rel = pt.o - center;
			angles.push_back(rel.angleDegrees());
			distances.push_back(rel.length());
		}
		const auto actualReps = reps * 2;
		const auto sectionAngle = 360. / actualReps;
		out.reserve(points.size() * actualReps);
		const auto lastIdx = int(points.size()) - 1;
		for (auto rep = 0; rep < actualReps; ++rep) {
			for (auto index = 0; index < int(points.size()); ++index) {
				const auto i = (rep % 2 == 0) ? index : (lastIdx - index);
				if (i > 0 || rep % 2 == 0) {
					const auto deg = sectionAngle * rep
						+ ((rep % 2 == 0)
							? angles[i]
							: (sectionAngle - angles[i] + 2. * angles[0]));
					const auto rad = deg * kPi / 180.;
					const auto finalPt = Vec2{
						std::cos(rad),
						std::sin(rad),
					} * distances[i] + center;
					out.push_back({ finalPt, points[i].r });
				}
			}
		}
	} else {
		const auto np = int(points.size());
		out.reserve(np * reps);
		for (auto it = 0; it < np * reps; ++it) {
			const auto idx = it % np;
			const auto step = (it / np) * (360. / reps);
			const auto pt = points[idx].o.rotatedDegrees(step, center);
			out.push_back({ pt, points[idx].r });
		}
	}
	return out;
}

[[nodiscard]] std::vector<PointNRound> Deduplicate(
		const std::vector<PointNRound> &raw) {
	std::vector<PointNRound> result;
	result.reserve(raw.size());
	for (const auto &pt : raw) {
		if (!result.empty()) {
			const auto dx = pt.o.x - result.back().o.x;
			const auto dy = pt.o.y - result.back().o.y;
			if (dx * dx + dy * dy < 1e-6) {
				continue;
			}
		}
		result.push_back(pt);
	}
	if (result.size() > 2) {
		const auto dx = result.front().o.x - result.back().o.x;
		const auto dy = result.front().o.y - result.back().o.y;
		if (dx * dx + dy * dy < 1e-6) {
			result.pop_back();
		}
	}
	return result;
}

[[nodiscard]] PolySpec CustomPolygon(
		const std::vector<PointNRound> &pnr,
		int reps,
		Vec2 center = { 0.5, 0.5 },
		bool mirroring = false) {
	PolySpec spec;
	spec.points = Deduplicate(DoRepeat(pnr, reps, center, mirroring));
	return spec;
}

[[nodiscard]] PolySpec RegularPolygon(
		int numVertices,
		CornerRounding rounding = kUnrounded,
		const std::vector<CornerRounding> &perVertex = {}) {
	PolySpec spec;
	spec.points.reserve(numVertices);
	for (auto i = 0; i < numVertices; ++i) {
		const auto angle = (2. * kPi * i) / numVertices;
		const auto r = (i < int(perVertex.size())) ? perVertex[i] : rounding;
		spec.points.push_back({ { std::cos(angle), std::sin(angle) }, r });
	}
	return spec;
}

[[nodiscard]] PolySpec RectanglePolygon(
		double width,
		double height,
		CornerRounding rounding = kUnrounded,
		const std::vector<CornerRounding> &perVertex = {}) {
	const auto hw = width * 0.5;
	const auto hh = height * 0.5;
	// AndroidX RoundedPolygon.rectangle vertex order:
	// (right, bottom), (left, bottom), (left, top), (right, top)
	const std::array<Vec2, 4> verts = {
		Vec2{ hw, hh },
		Vec2{ -hw, hh },
		Vec2{ -hw, -hh },
		Vec2{ hw, -hh },
	};
	PolySpec spec;
	spec.points.reserve(4);
	for (auto i = 0; i < 4; ++i) {
		const auto r = (i < int(perVertex.size())) ? perVertex[i] : rounding;
		spec.points.push_back({ verts[i], r });
	}
	return spec;
}

[[nodiscard]] PolySpec StarPolygon(
		int numVerticesPerRadius,
		double innerRadius,
		CornerRounding rounding = kUnrounded,
		CornerRounding innerRounding = kUnrounded) {
	PolySpec spec;
	spec.points.reserve(numVerticesPerRadius * 2);
	const auto step = kPi / numVerticesPerRadius;
	for (auto i = 0; i < numVerticesPerRadius * 2; ++i) {
		const auto angle = step * i;
		const auto isOuter = (i % 2 == 0);
		const auto rad = isOuter ? 1. : innerRadius;
		spec.points.push_back({
			{ rad * std::cos(angle), rad * std::sin(angle) },
			isOuter ? rounding : innerRounding,
		});
	}
	return spec;
}

[[nodiscard]] QPainterPath NormalizePath(const QPainterPath &raw) {
	const auto bounds = raw.boundingRect();
	if (bounds.width() <= 1e-6 || bounds.height() <= 1e-6) {
		QPainterPath fallback;
		fallback.addEllipse(QRectF(0., 0., 1., 1.));
		return fallback;
	}
	const auto scale = 1. / std::max(bounds.width(), bounds.height());
	const auto cx = bounds.center().x();
	const auto cy = bounds.center().y();
	QTransform t;
	t.translate(0.5, 0.5);
	t.scale(scale, scale);
	t.translate(-cx, -cy);
	return t.map(raw);
}

[[nodiscard]] QPainterPath BuildRoundedPolygonPath(const PolySpec &spec) {
	if (spec.isCircle) {
		QPainterPath circle;
		circle.addEllipse(QRectF(-1., -1., 2., 2.));
		QTransform t;
		if (spec.rotateDeg != 0.) {
			t.rotate(spec.rotateDeg);
		}
		if (spec.scaleX != 1. || spec.scaleY != 1.) {
			t.scale(spec.scaleX, spec.scaleY);
		}
		return NormalizePath(t.map(circle));
	}

	const auto n = int(spec.points.size());
	if (n < 3) {
		QPainterPath fallback;
		fallback.addEllipse(QRectF(0., 0., 1., 1.));
		return fallback;
	}

	// Precompute vertex geometry and expected cut distances along each edge.
	struct CornerGeom {
		Vec2 p;
		Vec2 u1; // unit vector toward prev
		Vec2 u2; // unit vector toward next
		double d1 = 0.;
		double d2 = 0.;
		double sinAlpha = 0.;
		double cosAlpha = 0.;
		double expectedCut = 0.;
		double effCut = 0.;
		double smoothing = 0.;
	};

	std::vector<CornerGeom> corners(n);
	for (auto i = 0; i < n; ++i) {
		const auto &prev = spec.points[(i + n - 1) % n].o;
		const auto &curr = spec.points[i].o;
		const auto &next = spec.points[(i + 1) % n].o;
		const auto v1 = prev - curr;
		const auto v2 = next - curr;
		const auto l1 = v1.length();
		const auto l2 = v2.length();
		auto &cg = corners[i];
		cg.p = curr;
		cg.d1 = l1;
		cg.d2 = l2;
		cg.smoothing = std::clamp(spec.points[i].r.smoothing, 0., 1.);
		if (l1 > 1e-6 && l2 > 1e-6) {
			cg.u1 = v1 * (1. / l1);
			cg.u2 = v2 * (1. / l2);
			const auto cos2a = std::clamp(
				cg.u1.x * cg.u2.x + cg.u1.y * cg.u2.y,
				-1.,
				1.);
			cg.sinAlpha = std::sqrt(std::max(0., (1. - cos2a) * 0.5));
			cg.cosAlpha = std::sqrt(std::max(0., (1. + cos2a) * 0.5));
			if (cg.sinAlpha > 1e-4 && spec.points[i].r.radius > 1e-5) {
				cg.expectedCut = spec.points[i].r.radius
					* (cg.cosAlpha / cg.sinAlpha);
			}
		}
	}

	// Clamp cuts per edge so adjacent rounded corners never overlap.
	std::vector<double> maxCut(n, 1e9);
	for (auto i = 0; i < n; ++i) {
		const auto nextIdx = (i + 1) % n;
		const auto edgeLen = corners[i].d2;
		const auto c1 = corners[i].expectedCut;
		const auto c2 = corners[nextIdx].expectedCut;
		const auto sum = c1 + c2;
		if (sum > edgeLen && sum > 1e-6) {
			const auto ratio = edgeLen / sum;
			maxCut[i] = std::min(maxCut[i], c1 * ratio);
			maxCut[nextIdx] = std::min(maxCut[nextIdx], c2 * ratio);
		} else {
			maxCut[i] = std::min(maxCut[i], c1);
			maxCut[nextIdx] = std::min(maxCut[nextIdx], c2);
		}
	}
	for (auto i = 0; i < n; ++i) {
		corners[i].effCut = std::min(
			corners[i].expectedCut,
			std::max(0., maxCut[i]));
	}

	QPainterPath path;
	for (auto i = 0; i < n; ++i) {
		const auto &cg = corners[i];
		if (cg.effCut <= 1e-5 || cg.sinAlpha <= 1e-4) {
			if (i == 0) {
				path.moveTo(cg.p.x, cg.p.y);
			} else {
				path.lineTo(cg.p.x, cg.p.y);
			}
		} else {
			const auto pStart = cg.p + cg.u1 * cg.effCut;
			const auto pEnd = cg.p + cg.u2 * cg.effCut;
			if (i == 0) {
				path.moveTo(pStart.x, pStart.y);
			} else {
				path.lineTo(pStart.x, pStart.y);
			}
			// Circular arc tangent to u1 at pStart and u2 at pEnd.
			// Sweep angle theta = pi - 2 * alpha.
			const auto alpha = std::asin(std::clamp(cg.sinAlpha, 0., 1.));
			const auto theta = kPi - 2. * alpha;
			const auto effRadius = cg.effCut * (cg.sinAlpha / std::max(1e-4, cg.cosAlpha));
			const auto arcHandle = (4. / 3.) * std::tan(theta * 0.25) * effRadius;
			// Smoothing blends the cubic handle slightly further toward vertex.
			const auto handle = std::min(
				cg.effCut,
				arcHandle * (1. + 0.18 * cg.smoothing));
			const auto c1 = pStart - cg.u1 * handle;
			const auto c2 = pEnd - cg.u2 * handle;
			path.cubicTo(c1.x, c1.y, c2.x, c2.y, pEnd.x, pEnd.y);
		}
	}
	path.closeSubpath();

	QTransform t;
	if (spec.rotateDeg != 0.) {
		t.rotate(spec.rotateDeg);
	}
	if (spec.scaleX != 1. || spec.scaleY != 1.) {
		t.scale(spec.scaleX, spec.scaleY);
	}
	return NormalizePath(t.map(path));
}

[[nodiscard]] PolySpec BuildPolySpec(int shapeId) {
	switch (shapeId) {
	case kShapeCircle: {
		PolySpec s;
		s.isCircle = true;
		return s;
	}
	case kShapeSquare:
		return RectanglePolygon(1., 1., kRound30);
	case kShapeSlanted:
		return CustomPolygon({
			{ { 0.926, 0.970 }, { 0.189, 0.811 } },
			{ { -0.021, 0.967 }, { 0.187, 0.057 } },
		}, 2);
	case kShapeArch: {
		auto s = RegularPolygon(
			4,
			kUnrounded,
			{ kRound100, kRound100, kRound20, kRound20 });
		s.rotateDeg = -135.;
		return s;
	}
	case kShapeSemicircle:
		return RectanglePolygon(
			1.6,
			1.0,
			kUnrounded,
			{ kRound20, kRound20, kRound100, kRound100 });
	case kShapeOval: {
		PolySpec s;
		s.isCircle = true;
		s.scaleX = 1.;
		s.scaleY = 0.64;
		s.rotateDeg = -45.;
		return s;
	}
	case kShapePill:
		return CustomPolygon({
			{ { 0.961, 0.039 }, { 0.426, 0. } },
			{ { 1.001, 0.428 }, kUnrounded },
			{ { 1.000, 0.609 }, { 1.000, 0. } },
		}, 2, { 0.5, 0.5 }, true);
	case kShapeTriangle: {
		auto s = RegularPolygon(3, kRound20);
		s.rotateDeg = -90.;
		return s;
	}
	case kShapeArrow:
		return CustomPolygon({
			{ { 0.500, 0.892 }, { 0.313, 0. } },
			{ { -0.216, 1.050 }, { 0.207, 0. } },
			{ { 0.499, -0.160 }, { 0.215, 1.000 } },
			{ { 1.225, 1.060 }, { 0.211, 0. } },
		}, 1);
	case kShapeFan:
		return CustomPolygon({
			{ { 1.004, 1.000 }, { 0.148, 0.417 } },
			{ { 0.000, 1.000 }, { 0.151, 0. } },
			{ { 0.000, -0.003 }, { 0.148, 0. } },
			{ { 0.978, 0.020 }, { 0.803, 0. } },
		}, 1);
	case kShapeDiamond:
		return CustomPolygon({
			{ { 0.500, 1.096 }, { 0.151, 0.524 } },
			{ { 0.040, 0.500 }, { 0.159, 0. } },
		}, 2);
	case kShapeClamshell:
		return CustomPolygon({
			{ { 0.171, 0.841 }, { 0.159, 0. } },
			{ { -0.020, 0.500 }, { 0.140, 0. } },
			{ { 0.170, 0.159 }, { 0.159, 0. } },
		}, 2);
	case kShapePentagon:
		return CustomPolygon({
			{ { 0.500, -0.009 }, { 0.172, 0. } },
			{ { 1.030, 0.365 }, { 0.164, 0. } },
			{ { 0.828, 0.970 }, { 0.169, 0. } },
		}, 1, { 0.5, 0.5 }, true);
	case kShapeGem:
		return CustomPolygon({
			{ { 0.499, 1.023 }, { 0.241, 0.778 } },
			{ { -0.005, 0.792 }, { 0.208, 0. } },
			{ { 0.073, 0.258 }, { 0.228, 0. } },
			{ { 0.433, 0.000 }, { 0.491, 0. } },
		}, 1, { 0.5, 0.5 }, true);
	case kShapeSunny:
		return StarPolygon(8, 0.8, kRound15, kRound15);
	case kShapeVerySunny:
		return CustomPolygon({
			{ { 0.500, 1.080 }, { 0.085, 0. } },
			{ { 0.358, 0.843 }, { 0.085, 0. } },
		}, 8);
	case kShapeCookie4:
		return CustomPolygon({
			{ { 1.237, 1.236 }, { 0.258, 0. } },
			{ { 0.500, 0.918 }, { 0.233, 0. } },
		}, 4);
	case kShapeCookie6:
		return CustomPolygon({
			{ { 0.723, 0.884 }, { 0.394, 0. } },
			{ { 0.500, 1.099 }, { 0.398, 0. } },
		}, 6);
	case kShapeCookie7: {
		auto s = StarPolygon(7, 0.75, kRound50, kRound50);
		s.rotateDeg = -90.;
		return s;
	}
	case kShapeCookie9: {
		auto s = StarPolygon(9, 0.8, kRound50, kRound50);
		s.rotateDeg = -90.;
		return s;
	}
	case kShapeCookie12: {
		auto s = StarPolygon(12, 0.8, kRound50, kRound50);
		s.rotateDeg = -90.;
		return s;
	}
	case kShapeClover4:
		return CustomPolygon({
			{ { 0.500, 0.074 }, kUnrounded },
			{ { 0.725, -0.099 }, { 0.476, 0. } },
		}, 4, { 0.5, 0.5 }, true);
	case kShapeClover8:
		return CustomPolygon({
			{ { 0.500, 0.036 }, kUnrounded },
			{ { 0.758, -0.101 }, { 0.209, 0. } },
		}, 8);
	case kShapeBurst:
		return CustomPolygon({
			{ { 0.500, -0.006 }, { 0.006, 0. } },
			{ { 0.592, 0.158 }, { 0.006, 0. } },
		}, 12);
	case kShapeSoftBurst:
		return CustomPolygon({
			{ { 0.193, 0.277 }, { 0.053, 0. } },
			{ { 0.176, 0.055 }, { 0.053, 0. } },
		}, 10);
	case kShapeBoom:
		return CustomPolygon({
			{ { 0.457, 0.296 }, { 0.007, 0. } },
			{ { 0.500, -0.051 }, { 0.007, 0. } },
		}, 15);
	case kShapeSoftBoom:
		return CustomPolygon({
			{ { 0.733, 0.454 }, kUnrounded },
			{ { 0.839, 0.437 }, { 0.532, 0. } },
			{ { 0.949, 0.449 }, { 0.439, 1.000 } },
			{ { 0.998, 0.478 }, { 0.174, 0. } },
		}, 16, { 0.5, 0.5 }, true);
	case kShapeFlower:
		return CustomPolygon({
			{ { 0.370, 0.187 }, kUnrounded },
			{ { 0.416, 0.049 }, { 0.381, 0. } },
			{ { 0.479, 0.001 }, { 0.095, 0. } },
		}, 8, { 0.5, 0.5 }, true);
	case kShapePuffy: {
		auto s = CustomPolygon({
			{ { 0.500, 0.053 }, kUnrounded },
			{ { 0.545, -0.040 }, { 0.405, 0. } },
			{ { 0.670, -0.035 }, { 0.426, 0. } },
			{ { 0.717, 0.066 }, { 0.574, 0. } },
			{ { 0.722, 0.128 }, kUnrounded },
			{ { 0.777, 0.002 }, { 0.360, 0. } },
			{ { 0.914, 0.149 }, { 0.660, 0. } },
			{ { 0.926, 0.289 }, { 0.660, 0. } },
			{ { 0.881, 0.346 }, kUnrounded },
			{ { 0.940, 0.344 }, { 0.126, 0. } },
			{ { 1.003, 0.437 }, { 0.255, 0. } },
		}, 2, { 0.5, 0.5 }, true);
		s.scaleY = 0.742;
		return s;
	}
	case kShapePuffyDiamond:
		return CustomPolygon({
			{ { 0.870, 0.130 }, { 0.146, 0. } },
			{ { 0.818, 0.357 }, kUnrounded },
			{ { 1.000, 0.332 }, { 0.853, 0. } },
		}, 4, { 0.5, 0.5 }, true);
	case kShapeGhostish:
		return CustomPolygon({
			{ { 0.500, 0.000 }, { 1.000, 0. } },
			{ { 1.000, 0.000 }, { 1.000, 0. } },
			{ { 1.000, 1.140 }, { 0.254, 0.106 } },
			{ { 0.575, 0.906 }, { 0.253, 0. } },
		}, 1, { 0.5, 0.5 }, true);
	case kShapePixelCircle:
		return CustomPolygon({
			{ { 0.500, 0.000 }, kUnrounded },
			{ { 0.704, 0.000 }, kUnrounded },
			{ { 0.704, 0.065 }, kUnrounded },
			{ { 0.843, 0.065 }, kUnrounded },
			{ { 0.843, 0.148 }, kUnrounded },
			{ { 0.926, 0.148 }, kUnrounded },
			{ { 0.926, 0.296 }, kUnrounded },
			{ { 1.000, 0.296 }, kUnrounded },
		}, 2, { 0.5, 0.5 }, true);
	case kShapePixelTriangle:
		return CustomPolygon({
			{ { 0.110, 0.500 }, kUnrounded },
			{ { 0.113, 0.000 }, kUnrounded },
			{ { 0.287, 0.000 }, kUnrounded },
			{ { 0.287, 0.087 }, kUnrounded },
			{ { 0.421, 0.087 }, kUnrounded },
			{ { 0.421, 0.170 }, kUnrounded },
			{ { 0.560, 0.170 }, kUnrounded },
			{ { 0.560, 0.265 }, kUnrounded },
			{ { 0.674, 0.265 }, kUnrounded },
			{ { 0.675, 0.344 }, kUnrounded },
			{ { 0.789, 0.344 }, kUnrounded },
			{ { 0.789, 0.439 }, kUnrounded },
			{ { 0.888, 0.439 }, kUnrounded },
		}, 1, { 0.5, 0.5 }, true);
	case kShapeBun:
		return CustomPolygon({
			{ { 0.796, 0.500 }, kUnrounded },
			{ { 0.853, 0.518 }, { 1.0, 0. } },
			{ { 0.992, 0.631 }, { 1.0, 0. } },
			{ { 0.968, 1.000 }, { 1.0, 0. } },
		}, 2, { 0.5, 0.5 }, true);
	case kShapeHeart:
		return CustomPolygon({
			{ { 0.500, 0.268 }, { 0.016, 0. } },
			{ { 0.792, -0.066 }, { 0.958, 0. } },
			{ { 1.064, 0.276 }, { 1.000, 0. } },
			{ { 0.501, 0.946 }, { 0.129, 0. } },
		}, 1, { 0.5, 0.5 }, true);
	case kShapeStar4:
		return StarPolygon(4, 0.45, kRound20, kRound15);
	case kShapeStar8:
		return StarPolygon(8, 0.55, kRound15, kRound15);
	case kShapeHexagon: {
		auto s = RegularPolygon(6, kRound20);
		s.rotateDeg = -90.;
		return s;
	}
	case kShapeCloud:
		return CustomPolygon({
			{ { 0.500, 0.100 }, { 0.800, 0. } },
			{ { 0.760, 0.200 }, { 0.700, 0. } },
			{ { 0.960, 0.520 }, { 0.650, 0. } },
			{ { 0.880, 0.850 }, { 0.550, 0. } },
			{ { 0.600, 0.880 }, { 0.450, 0. } },
			{ { 0.500, 0.880 }, { 0.300, 0. } },
		}, 1, { 0.5, 0.5 }, true);
	case kShapeScallop:
		return StarPolygon(12, 0.82, kRound50, kRound20);
	case kShapeOctagon:
		return RegularPolygon(8, kRound20);
	case kShapeShield:
		return CustomPolygon({
			{ { 0.500, 0.050 }, { 0.100, 0. } },
			{ { 0.950, 0.100 }, { 0.250, 0. } },
			{ { 0.920, 0.550 }, { 0.400, 0. } },
			{ { 0.500, 0.980 }, { 0.200, 0. } },
		}, 1, { 0.5, 0.5 }, true);
	case kShapeFlower12:
		return StarPolygon(12, 0.72, kRound50, kRound20);
	case kShapeClover3:
		return CustomPolygon({
			{ { 0.500, 0.120 }, kUnrounded },
			{ { 0.780, -0.080 }, { 0.450, 0. } },
		}, 3, { 0.5, 0.5 }, true);
	default: {
		PolySpec s;
		s.isCircle = true;
		return s;
	}
	}
}

struct ScaledPathCache {
	static constexpr std::size_t kCapacity = 96;
	std::mutex mutex;
	std::list<std::pair<uint64, QPainterPath>> lru;
	std::unordered_map<uint64, decltype(lru)::iterator> map;

	[[nodiscard]] QPainterPath get(int shapeId, double width, double height) {
		const auto id = std::clamp(shapeId, 0, kShapesCount - 1);
		const auto wInt = std::max(1, int(std::lround(width)));
		const auto hInt = std::max(1, int(std::lround(height)));
		const auto key = (uint64(id) << 32)
			| (uint64(wInt & 0xFFFF) << 16)
			| uint64(hInt & 0xFFFF);

		std::lock_guard lock(mutex);
		if (const auto it = map.find(key); it != map.end()) {
			lru.splice(lru.begin(), lru, it->second);
			return it->second->second;
		}
		QTransform t;
		t.scale(width, height);
		auto scaled = t.map(BasePath(id));
		lru.emplace_front(key, scaled);
		map[key] = lru.begin();
		if (lru.size() > kCapacity) {
			map.erase(lru.back().first);
			lru.pop_back();
		}
		return scaled;
	}
};

[[nodiscard]] ScaledPathCache &PathCache() {
	static ScaledPathCache cache;
	return cache;
}

void RepaintAllTopLevel() {
	for (const auto widget : QApplication::topLevelWidgets()) {
		if (widget) {
			widget->update();
		}
	}
}

} // namespace

QString ShapeName(int shapeId) {
	static const std::array<QString, kShapesCount> kNames = {
		u"Circle"_q,
		u"Square"_q,
		u"Slanted"_q,
		u"Arch"_q,
		u"Semicircle"_q,
		u"Oval"_q,
		u"Pill"_q,
		u"Triangle"_q,
		u"Arrow"_q,
		u"Fan"_q,
		u"Diamond"_q,
		u"Clamshell"_q,
		u"Pentagon"_q,
		u"Gem"_q,
		u"Sunny"_q,
		u"Very sunny"_q,
		u"4-sided cookie"_q,
		u"6-sided cookie"_q,
		u"7-sided cookie"_q,
		u"9-sided cookie"_q,
		u"12-sided cookie"_q,
		u"4-leaf clover"_q,
		u"8-leaf clover"_q,
		u"Burst"_q,
		u"Soft burst"_q,
		u"Boom"_q,
		u"Soft boom"_q,
		u"Flower"_q,
		u"Puffy"_q,
		u"Puffy diamond"_q,
		u"Ghost-ish"_q,
		u"Pixel circle"_q,
		u"Pixel triangle"_q,
		u"Bun"_q,
		u"Heart"_q,
		u"4-point star"_q,
		u"8-point star"_q,
		u"Hexagon"_q,
		u"Cloud"_q,
		u"Scallop"_q,
		u"Octagon"_q,
		u"Shield"_q,
		u"12-petal flower"_q,
		u"3-leaf clover"_q,
	};
	return kNames[std::clamp(shapeId, 0, kShapesCount - 1)];
}

QString PeerTypeName(PeerType type) {
	switch (type) {
	case PeerType::User: return u"Users"_q;
	case PeerType::Bot: return u"Bots"_q;
	case PeerType::Group: return u"Groups"_q;
	case PeerType::Channel: return u"Channels"_q;
	case PeerType::Self: return u"Saved / Self"_q;
	}
	return u"Users"_q;
}

QString ConfigKeyForPeerType(PeerType type) {
	switch (type) {
	case PeerType::User: return u"avatar_shape_users"_q;
	case PeerType::Bot: return u"avatar_shape_bots"_q;
	case PeerType::Group: return u"avatar_shape_groups"_q;
	case PeerType::Channel: return u"avatar_shape_channels"_q;
	case PeerType::Self: return u"avatar_shape_self"_q;
	}
	return u"avatar_shape_users"_q;
}

const QPainterPath &BasePath(int shapeId) {
	static std::mutex mutex;
	static std::array<QPainterPath, kShapesCount> paths;
	static std::array<bool, kShapesCount> ready{};
	const auto id = std::clamp(shapeId, 0, kShapesCount - 1);
	std::lock_guard lock(mutex);
	if (!ready[id]) {
		paths[id] = BuildRoundedPolygonPath(BuildPolySpec(id));
		ready[id] = true;
	}
	return paths[id];
}

QPainterPath ShapePath(int shapeId, QRectF rect) {
	auto path = PathCache().get(shapeId, rect.width(), rect.height());
	if (rect.left() != 0. || rect.top() != 0.) {
		path.translate(rect.topLeft());
	}
	return path;
}

int ShapeForPeerType(PeerType type) {
	const auto val = Config::Instance().valueOf(ConfigKeyForPeerType(type));
	if (val.isUndefined() || val.isNull()) {
		return kShapeCircle;
	}
	return std::clamp(val.toInt(0), 0, kShapesCount - 1);
}

void SetShapeForPeerType(PeerType type, int shapeId) {
	Config::Instance().setInt(
		ConfigKeyForPeerType(type),
		std::clamp(shapeId, 0, kShapesCount - 1));
	RepaintAllTopLevel();
}

int ActiveShapeId() {
	return ShapeForPeerType(PeerType::User);
}

void PaintStoryRing(
		QPainter &p,
		QRectF rect,
		int shapeId,
		const QBrush &brush,
		float64 penWidth) {
	auto hq = PainterHighQualityEnabler(p);
	p.setBrush(Qt::NoBrush);
	p.setPen(QPen(brush, penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	if (shapeId > 0) {
		p.drawPath(ShapePath(shapeId, rect));
	} else {
		p.drawEllipse(rect);
	}
}

void ShowShapePickerBox(
		not_null<Window::SessionController*> controller,
		PeerType type,
		Fn<void(int)> onPicked) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(
			u"Avatar shape — "_q + PeerTypeName(type)));
		box->setWidth(style::ConvertScale(400));

		const auto selected = box->lifetime().make_state<rpl::variable<int>>(
			ShapeForPeerType(type));
		const auto container = box->verticalLayout();

		constexpr auto kCols = 4;
		constexpr auto kRows = (kShapesCount + kCols - 1) / kCols;
		const auto cellW = style::ConvertScale(86);
		const auto cellH = style::ConvertScale(92);
		const auto gap = style::ConvertScale(6);
		const auto padX = style::ConvertScale(14);
		const auto padY = style::ConvertScale(8);

		const auto grid = container->add(
			object_ptr<Ui::RpWidget>(container));
		const auto totalH = padY * 2 + kRows * cellH + (kRows - 1) * gap;
		grid->resize(padX * 2 + kCols * cellW + (kCols - 1) * gap, totalH);
		grid->setMinimumHeight(totalH);

		for (auto i = 0; i < kShapesCount; ++i) {
			const auto row = i / kCols;
			const auto col = i % kCols;
			const auto btn = Ui::CreateChild<Ui::AbstractButton>(grid);
			btn->setGeometry(
				padX + col * (cellW + gap),
				padY + row * (cellH + gap),
				cellW,
				cellH);
			btn->paintRequest() | rpl::on_next([=] {
				auto p = QPainter(btn);
				auto hq = PainterHighQualityEnabler(p);
				const auto active = (selected->current() == i);
				const auto r = QRectF(btn->rect()).adjusted(1., 1., -1., -1.);
				const auto accent = st::windowBgActive->c;
				if (active) {
					auto fill = accent;
					fill.setAlphaF(0.16);
					p.setBrush(fill);
					p.setPen(QPen(accent, style::ConvertScale(2.)));
				} else {
					auto fill = st::windowFg->c;
					fill.setAlphaF(btn->isOver() ? 0.08 : 0.04);
					p.setBrush(fill);
					p.setPen(Qt::NoPen);
				}
				const auto cardRadius = style::ConvertScale(14.);
				p.drawRoundedRect(r, cardRadius, cardRadius);

				const auto shapeSide = style::ConvertScale(46.);
				const auto shapeRect = QRectF(
					(btn->width() - shapeSide) * 0.5,
					style::ConvertScale(10.),
					shapeSide,
					shapeSide);
				QLinearGradient grad(shapeRect.topLeft(), shapeRect.bottomRight());
				grad.setColorAt(0., accent.lighter(115));
				grad.setColorAt(1., accent.darker(115));
				p.setPen(Qt::NoPen);
				p.setBrush(grad);
				p.drawPath(ShapePath(i, shapeRect));

				p.setFont(st::normalFont);
				p.setPen(active ? accent : st::windowSubTextFg->c);
				const auto textRect = QRect(
					style::ConvertScale(2),
					int(shapeRect.bottom()) + style::ConvertScale(5),
					btn->width() - style::ConvertScale(4),
					btn->height() - int(shapeRect.bottom()) - style::ConvertScale(6));
				p.drawText(
					textRect,
					Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
					ShapeName(i));
			}, btn->lifetime());

			selected->changes() | rpl::on_next([=] {
				btn->update();
			}, btn->lifetime());

			btn->setClickedCallback([=] {
				*selected = i;
				SetShapeForPeerType(type, i);
				if (onPicked) {
					onPicked(i);
				}
			});
		}

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"MD3 Expressive Avatar Shapes"_q));
		const auto container = box->verticalLayout();

		Ui::AddSubsectionTitle(container, rpl::single(u"Shape per Peer Type"_q));
		const std::array<PeerType, 5> types = {
			PeerType::User,
			PeerType::Bot,
			PeerType::Group,
			PeerType::Channel,
			PeerType::Self,
		};
		for (const auto type : types) {
			const auto row = container->add(
				object_ptr<Ui::SettingsButton>(
					container,
					rpl::single(PeerTypeName(type)),
					st::settingsButtonNoIcon));
			const auto label = Ui::CreateChild<Ui::FlatLabel>(
				row,
				ShapeName(ShapeForPeerType(type)),
				st::defaultFlatLabel);
			label->setAttribute(Qt::WA_TransparentForMouseEvents);
			row->sizeValue() | rpl::on_next([=](QSize s) {
				label->moveToRight(
					st::settingsButtonNoIcon.padding.right(),
					(s.height() - label->height()) / 2,
					s.width());
			}, label->lifetime());

			row->setClickedCallback([=] {
				ShowShapePickerBox(controller, type, [=](int picked) {
					label->setText(ShapeName(picked));
				});
			});
		}

		Ui::AddSkip(container);
		Ui::AddDividerText(
			container,
			rpl::single(
				u"Choose from 44 Material Design 3 Expressive shapes. "
				"Stories rings automatically match the active shape."_q));

		box->addLeftButton(rpl::single(u"Reset"_q), [=] {
			for (const auto type : types) {
				SetShapeForPeerType(type, kShapeCircle);
			}
			box->closeBox();
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

void AddSettingsRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSubsectionTitle(container, rpl::single(u"MD3 Expressive Visuals"_q));

	const auto shapesBtn = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"MD3 Avatar Shapes (44 shapes)"_q),
			st::settingsButtonNoIcon));
	shapesBtn->setClickedCallback([=] {
		ShowSettingsBox(controller);
	});

	const auto carouselBtn = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"MD3 Media Carousel for Albums"_q),
			st::settingsButtonNoIcon));
	carouselBtn->toggleOn(rpl::single(MediaCarousel::Enabled()));
	carouselBtn->toggledChanges() | rpl::on_next([](bool enabled) {
		Config::Instance().setBool(u"md3_media_carousel"_q, enabled);
		RepaintAllTopLevel();
	}, carouselBtn->lifetime());

	const auto glassBtn = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Frosted / Liquid Glass Message Bubbles"_q),
			st::settingsButtonNoIcon));
	glassBtn->setClickedCallback([=] {
		GlassBubbles::ShowSettingsBox(controller);
	});
}

} // namespace Extera::AvatarShapes

namespace Extera::GlassBubbles {
namespace {

[[nodiscard]] QPainterPath BuildBubblePath(
		const QRect &rect,
		Ui::BubbleRounding rounding) {
	using Corner = Ui::BubbleCornerRounding;
	const auto smallR = float64(Ui::BubbleRadiusSmall());
	const auto largeR = float64(Ui::BubbleRadiusLarge());
	const auto cornerRadius = [&](Corner c) -> float64 {
		switch (c) {
		case Corner::Large: return largeR;
		case Corner::Small: return smallR;
		case Corner::Tail: return smallR * 0.5;
		case Corner::None: return 0.;
		}
		return 0.;
	};
	const auto maxR = std::min(rect.width(), rect.height()) * 0.5;
	const auto tl = std::min(maxR, cornerRadius(rounding.topLeft));
	const auto tr = std::min(maxR, cornerRadius(rounding.topRight));
	const auto br = std::min(maxR, cornerRadius(rounding.bottomRight));
	const auto bl = std::min(maxR, cornerRadius(rounding.bottomLeft));

	const auto x = float64(rect.x());
	const auto y = float64(rect.y());
	const auto w = float64(rect.width());
	const auto h = float64(rect.height());

	QPainterPath path;
	path.moveTo(x + tl, y);
	path.lineTo(x + w - tr, y);
	if (tr > 0.) {
		path.arcTo(x + w - 2. * tr, y, 2. * tr, 2. * tr, 90., -90.);
	}
	path.lineTo(x + w, y + h - br);
	if (br > 0.) {
		path.arcTo(x + w - 2. * br, y + h - 2. * br, 2. * br, 2. * br, 0., -90.);
	}
	path.lineTo(x + bl, y + h);
	if (bl > 0.) {
		path.arcTo(x, y + h - 2. * bl, 2. * bl, 2. * bl, 270., -90.);
	}
	path.lineTo(x, y + tl);
	if (tl > 0.) {
		path.arcTo(x, y, 2. * tl, 2. * tl, 180., -90.);
	}
	path.closeSubpath();
	return path;
}

[[nodiscard]] QImage BlurBackdropPixmap(const QPixmap &source) {
	if (source.isNull() || source.width() <= 0 || source.height() <= 0) {
		return {};
	}
	const auto img = source.toImage().convertToFormat(
		QImage::Format_ARGB32_Premultiplied);
	const auto smallW = std::max(8, img.width() / 8);
	const auto smallH = std::max(8, img.height() / 8);
	const auto down = img.scaled(
		smallW,
		smallH,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
	return down.scaled(
		img.size(),
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation);
}

struct BlurredPatternCache {
	qint64 cacheKey = 0;
	QImage blurred;
};

[[nodiscard]] const QImage &CachedBlurredPattern(const QPixmap &pixmap) {
	static BlurredPatternCache cache;
	if (cache.cacheKey != pixmap.cacheKey()) {
		cache.cacheKey = pixmap.cacheKey();
		cache.blurred = BlurBackdropPixmap(pixmap);
	}
	return cache.blurred;
}

} // namespace

bool Enabled() {
	return Config::Instance().boolValue(u"glass_messages"_q);
}

int Opacity() {
	const auto val = Config::Instance().valueOf(u"glass_messages_opacity"_q);
	if (val.isUndefined() || val.isNull()) {
		return 65;
	}
	return std::clamp(val.toInt(65), 10, 95);
}

bool StrokeEnabled() {
	const auto val = Config::Instance().valueOf(u"glass_messages_stroke"_q);
	if (val.isUndefined() || val.isNull()) {
		return true;
	}
	return val.toBool(true);
}

void PaintGlassBubble(
		QPainter &p,
		const Ui::SimpleBubble &simple,
		const std::optional<QColor> &backgroundAverage) {
	const auto &g = simple.geometry;
	if (g.isEmpty()) {
		return;
	}

	const auto path = BuildBubblePath(g, simple.rounding);
	const auto stm = simple.st->messageStyle(simple.outbg, simple.selected);
	const auto baseColor = stm->msgBg->c;
	const auto isDark = simple.st->dark();
	const auto opacityNorm = Opacity() / 100.;

	p.save();
	auto hq = PainterHighQualityEnabler(p);

	// Soft ambient drop shadow beneath glass bubble.
	if (simple.shadowed) {
		p.setPen(Qt::NoPen);
		auto shadowColor = QColor(0, 0, 0, isDark ? 42 : 24);
		p.setBrush(shadowColor);
		p.drawPath(path.translated(0., style::ConvertScale(1.5)));
	}

	p.setClipPath(path);

	// 1. Blurred wallpaper / pattern backdrop when available.
	if (simple.pattern
		&& !simple.pattern->pixmap.isNull()
		&& !simple.patternViewport.isEmpty()) {
		const auto &blurred = CachedBlurredPattern(simple.pattern->pixmap);
		if (!blurred.isNull()) {
			const auto vp = simple.patternViewport;
			p.drawImage(vp, blurred);
		}
	} else if (backgroundAverage.has_value()) {
		auto bgTint = *backgroundAverage;
		bgTint.setAlphaF(isDark ? 0.28 : 0.22);
		p.fillPath(path, bgTint);
	}

	// 2. Translucent frosted bubble fill using theme message tint.
	auto fillColor = baseColor;
	const auto effectiveAlpha = simple.selected
		? std::max(0.75, opacityNorm)
		: std::clamp(opacityNorm * (isDark ? 0.72 : 0.78), 0.16, 0.88);
	fillColor.setAlphaF(effectiveAlpha);
	p.fillPath(path, fillColor);

	// 3. Liquid glass specular sheen gradient (top highlight + subtle bottom depth).
	QLinearGradient sheen(g.topLeft(), g.bottomLeft());
	if (isDark) {
		sheen.setColorAt(0.00, QColor(255, 255, 255, 30));
		sheen.setColorAt(0.35, QColor(255, 255, 255, 8));
		sheen.setColorAt(0.80, QColor(255, 255, 255, 0));
		sheen.setColorAt(1.00, QColor(255, 255, 255, 16));
	} else {
		sheen.setColorAt(0.00, QColor(255, 255, 255, 86));
		sheen.setColorAt(0.40, QColor(255, 255, 255, 24));
		sheen.setColorAt(0.85, QColor(255, 255, 255, 0));
		sheen.setColorAt(1.00, QColor(255, 255, 255, 40));
	}
	p.fillPath(path, sheen);

	p.restore();

	// 4. Luminous frosted rim stroke.
	if (StrokeEnabled()) {
		p.save();
		auto hqStroke = PainterHighQualityEnabler(p);
		QLinearGradient rimGrad(g.topLeft(), g.bottomRight());
		if (isDark) {
			rimGrad.setColorAt(0.0, QColor(255, 255, 255, 76));
			rimGrad.setColorAt(0.5, QColor(255, 255, 255, 24));
			rimGrad.setColorAt(1.0, QColor(255, 255, 255, 48));
		} else {
			rimGrad.setColorAt(0.0, QColor(255, 255, 255, 165));
			rimGrad.setColorAt(0.5, QColor(255, 255, 255, 64));
			rimGrad.setColorAt(1.0, QColor(255, 255, 255, 115));
		}
		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(rimGrad, style::ConvertScale(1.)));
		p.drawPath(path);
		p.restore();
	}
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Frosted / Liquid Glass Bubbles"_q));
		const auto container = box->verticalLayout();

		const auto enableBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Enable Glass Message Bubbles"_q),
				st::settingsButtonNoIcon));
		enableBtn->toggleOn(rpl::single(Enabled()));
		enableBtn->toggledChanges() | rpl::on_next([](bool value) {
			Config::Instance().setBool(u"glass_messages"_q, value);
			for (const auto w : QApplication::topLevelWidgets()) {
				if (w) w->update();
			}
		}, enableBtn->lifetime());

		const auto strokeBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Glass Rim Highlight Stroke"_q),
				st::settingsButtonNoIcon));
		strokeBtn->toggleOn(rpl::single(StrokeEnabled()));
		strokeBtn->toggledChanges() | rpl::on_next([](bool value) {
			Config::Instance().setBool(u"glass_messages_stroke"_q, value);
			for (const auto w : QApplication::topLevelWidgets()) {
				if (w) w->update();
			}
		}, strokeBtn->lifetime());

		Ui::AddSkip(container);
		const auto opacityLabel = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				u"Bubble opacity: "_q + QString::number(Opacity()) + u"%"_q,
				st::defaultSubsectionTitle),
			st::defaultSubsectionTitlePadding);
		const auto slider = container->add(
			object_ptr<Ui::MediaSlider>(container, st::settingsScale),
			st::boxRowPadding);
		slider->resize(st::settingsScale.seekSize);
		slider->setPseudoDiscrete(
			18, // 10%..95% in 5% steps
			[](int index) { return 10 + index * 5; },
			Opacity(),
			[=](int value) {
				opacityLabel->setText(
					u"Bubble opacity: "_q + QString::number(value) + u"%"_q);
				Config::Instance().setInt(u"glass_messages_opacity"_q, value);
				for (const auto w : QApplication::topLevelWidgets()) {
					if (w) w->update();
				}
			});

		Ui::AddSkip(container);
		Ui::AddDividerText(
			container,
			rpl::single(
				u"Renders chat message bubbles with translucent frosted glass, "
				"specular sheen, and luminous rim highlights."_q));

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Extera::GlassBubbles
