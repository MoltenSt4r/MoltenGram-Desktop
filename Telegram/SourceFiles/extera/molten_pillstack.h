/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/timer.h"
#include "ui/rp_widget.h"

#include <QtCore/QString>
#include <QtCore/QRect>
#include <QtGui/QColor>
#include <vector>

namespace Window {
class SessionController;
} // namespace Window

namespace Main {
class Session;
} // namespace Main

namespace Extera::PillStack {

enum class PillType : int {
	DcPing = 0,
	NetSpeed = 1,
	Ram = 2,
	Ghost = 3,
	Proxy = 4,
	Weather = 5,
	Rates = 6,
};

enum class NetSpeedDisplayMode : int {
	Dominant = 0,
	Download = 1,
	Upload = 2,
	Combined = 3,
};

enum class RamDisplayMode : int {
	SystemPercent = 0,
	AppRss = 1,
};

struct PillInfo {
	PillType type = PillType::DcPing;
	QString id;
	QString text;
	QString tooltip;
	QColor accentColor;
	bool active = true;
	QRect rect;
};

// Global feature toggles
[[nodiscard]] bool Enabled();
[[nodiscard]] bool PillEnabled(PillType type);
void SetPillEnabled(PillType type, bool enabled);

// Formatting helpers
[[nodiscard]] QString FormatSpeed(int64 bytesPerSec);
[[nodiscard]] QString FormatBytes(int64 bytes);

// Live query methods
[[nodiscard]] int CurrentDcPingMs();
[[nodiscard]] QString CurrentNetSpeedText();
[[nodiscard]] QString CurrentRamText();
[[nodiscard]] QString CurrentWeatherText();
[[nodiscard]] QString CurrentRatesText();

// Actions & Refreshers
void TriggerDcPingProbe(Window::SessionController *controller = nullptr);
void RefreshWeather(bool force = false);
void RefreshRates(bool force = false);
void CycleRatesPair();
void CycleNetSpeedMode();
void CycleRamMode();

// Standalone paint helper (for custom bars or headers)
void PaintPills(
	QPainter &p,
	const QRect &geometry,
	const std::vector<PillInfo> &pills,
	int hoveredIndex = -1,
	int pressedIndex = -1);

// UI Widget for top bar / chat header integration
class Widget final : public Ui::RpWidget {
public:
	Widget(QWidget *parent, Window::SessionController *controller = nullptr);
	~Widget();

	[[nodiscard]] int naturalWidth() const;
	void updateData();

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEventHook(QEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void contextMenuEvent(QContextMenuEvent *e) override;

private:
	void rebuildPills();
	void paintPill(QPainter &p, const PillInfo &pill, bool hovered, bool pressed);
	[[nodiscard]] int pillAt(const QPoint &pos) const;
	void onPillClicked(int index);

	Window::SessionController *_controller = nullptr;
	std::vector<PillInfo> _pills;
	int _hoveredIndex = -1;
	int _pressedIndex = -1;
	base::Timer _refreshTimer;
};

// Settings Box
void ShowSettingsBox(not_null<Window::SessionController*> controller);

} // namespace Extera::PillStack
