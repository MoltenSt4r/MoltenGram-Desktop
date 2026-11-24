/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_pillstack.h"

#include "extera/extera_config.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/core_settings_proxy.h"
#include "main/main_session.h"
#include "main/main_account.h"
#include "window/window_session_controller.h"
#include "lang/lang_keys.h"
#include "ui/painter.h"
#include "ui/style/style_core.h"
#include "ui/vertical_list.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "styles/style_basic.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QPointer>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QPen>
#include <QtGui/QBrush>
#include <QtGui/QColor>
#include <QtGui/QLinearGradient>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtGui/QContextMenuEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMenu>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QTcpSocket>

#include <cmath>
#include <algorithm>
#include <memory>

#if defined(Q_OS_LINUX)
#include <unistd.h>
#elif defined(Q_OS_WIN)
#include "platform/win/windows_dlls.h"
#elif defined(Q_OS_MAC)
#include <mach/mach.h>
#endif

namespace Extera::PillStack {
namespace {

constexpr double kPi = 3.14159265358979323846;

const auto kPillStackEnabledKey = u"pillstack_enabled"_q;
const auto kDcPingKey = u"pillstack_dc_ping"_q;
const auto kNetSpeedKey = u"pillstack_net_speed"_q;
const auto kRamKey = u"pillstack_ram"_q;
const auto kGhostKey = u"pillstack_ghost"_q;
const auto kProxyKey = u"pillstack_proxy"_q;
const auto kWeatherKey = u"pillstack_weather"_q;
const auto kRatesKey = u"pillstack_rates"_q;
const auto kWeatherCityKey = u"pillstack_location_city"_q;
const auto kWeatherLatKey = u"pillstack_location_lat"_q;
const auto kWeatherLonKey = u"pillstack_location_lon"_q;
const auto kWeatherUnitKey = u"pillstack_weather_unit"_q;
const auto kRatePairKey = u"pillstack_rate_pair"_q;
const auto kRateTickersKey = u"pillstack_rate_tickers"_q;
const auto kNetSpeedModeKey = u"pillstack_speed_mode"_q;
const auto kRamModeKey = u"pillstack_ram_mode"_q;

[[nodiscard]] bool ConfigBool(const QString &key, bool defaultValue = true) {
	const auto val = Config::Instance().valueOf(key);
	if (val.isUndefined() || val.isNull()) {
		return defaultValue;
	}
	return val.toBool(defaultValue);
}

void SetConfigBool(const QString &key, bool value) {
	Config::Instance().setBool(key, value);
}

[[nodiscard]] int ConfigInt(const QString &key, int defaultValue = 0) {
	const auto val = Config::Instance().valueOf(key);
	if (val.isUndefined() || val.isNull()) {
		return defaultValue;
	}
	return val.toInt(defaultValue);
}

void SetConfigInt(const QString &key, int value) {
	Config::Instance().setInt(key, value);
}

[[nodiscard]] QString ConfigString(const QString &key, const QString &defaultValue = QString()) {
	const auto val = Config::Instance().valueOf(key);
	if (val.isUndefined() || val.isNull() || !val.isString()) {
		return defaultValue;
	}
	return val.toString(defaultValue);
}

void SetConfigString(const QString &key, const QString &value) {
	Config::Instance().setString(key, value);
}

[[nodiscard]] QNetworkAccessManager *NetworkManager() {
	static const auto manager = new QNetworkAccessManager(QCoreApplication::instance());
	return manager;
}

// ---------------------------------------------------------------------------
// DC Ping State & Probe
// ---------------------------------------------------------------------------

struct DcPingState {
	int pingMs = 38;
	int dcId = 2;
	qint64 lastProbeTime = 0;
	bool inFlight = false;
};

[[nodiscard]] DcPingState &GetDcPingState() {
	static auto state = DcPingState();
	return state;
}

[[nodiscard]] QString GetDcHost(int dcId) {
	switch (dcId) {
	case 1: return u"149.154.175.53"_q;
	case 2: return u"149.154.167.51"_q;
	case 3: return u"149.154.175.100"_q;
	case 4: return u"149.154.167.91"_q;
	case 5: return u"91.108.56.130"_q;
	default: return u"149.154.167.51"_q;
	}
}

// ---------------------------------------------------------------------------
// Network Speed State & Sampler
// ---------------------------------------------------------------------------

struct NetSpeedState {
	uint64 lastRx = 0;
	uint64 lastTx = 0;
	qint64 lastTimestamp = 0;
	int64 speedRx = 0;
	int64 speedTx = 0;
	NetSpeedDisplayMode mode = NetSpeedDisplayMode::Dominant;
};

[[nodiscard]] NetSpeedState &GetNetSpeedState() {
	static auto state = NetSpeedState();
	return state;
}

void SampleNetworkTraffic() {
	auto &state = GetNetSpeedState();
	const auto now = QDateTime::currentMSecsSinceEpoch();

#if defined(Q_OS_LINUX)
	QFile file(u"/proc/net/dev"_q);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
		return;
	}

	uint64 totalRx = 0;
	uint64 totalTx = 0;

	while (!file.atEnd()) {
		const auto line = QString::fromUtf8(file.readLine()).trimmed();
		if (!line.contains(':')) {
			continue;
		}
		const auto colonIndex = line.indexOf(':');
		const auto iface = line.left(colonIndex).trimmed();
		if (iface == u"lo"_q) {
			continue;
		}

		const auto tokens = line.mid(colonIndex + 1).split(' ', Qt::SkipEmptyParts);
		if (tokens.size() >= 9) {
			bool okRx = false;
			bool okTx = false;
			const auto rx = tokens[0].toULongLong(&okRx);
			const auto tx = tokens[8].toULongLong(&okTx);
			if (okRx) totalRx += rx;
			if (okTx) totalTx += tx;
		}
	}

	if (state.lastTimestamp > 0 && now > state.lastTimestamp) {
		const auto dt = now - state.lastTimestamp;
		if (dt > 0 && totalRx >= state.lastRx && totalTx >= state.lastTx) {
			state.speedRx = int64((totalRx - state.lastRx) * 1000 / dt);
			state.speedTx = int64((totalTx - state.lastTx) * 1000 / dt);
		}
	}

	state.lastRx = totalRx;
	state.lastTx = totalTx;
	state.lastTimestamp = now;
#else
	// Fallback platform telemetry
	state.lastTimestamp = now;
#endif
}

// ---------------------------------------------------------------------------
// RAM / Memory Usage State & Sampler
// ---------------------------------------------------------------------------

struct RamState {
	int systemPercent = 42;
	int processRssMb = 180;
	RamDisplayMode mode = RamDisplayMode::SystemPercent;
};

[[nodiscard]] RamState &GetRamState() {
	static auto state = RamState();
	return state;
}

void SampleMemoryUsage() {
	auto &state = GetRamState();

#if defined(Q_OS_LINUX)
	// 1. Read /proc/meminfo for system memory percent
	QFile meminfo(u"/proc/meminfo"_q);
	if (meminfo.open(QIODevice::ReadOnly | QIODevice::Text)) {
		uint64 totalKb = 0;
		uint64 availKb = 0;
		while (!meminfo.atEnd()) {
			const auto line = QString::fromUtf8(meminfo.readLine());
			if (line.startsWith(u"MemTotal:"_q)) {
				const auto parts = line.split(' ', Qt::SkipEmptyParts);
				if (parts.size() >= 2) totalKb = parts[1].toULongLong();
			} else if (line.startsWith(u"MemAvailable:"_q)) {
				const auto parts = line.split(' ', Qt::SkipEmptyParts);
				if (parts.size() >= 2) availKb = parts[1].toULongLong();
			}
		}
		if (totalKb > 0 && availKb <= totalKb) {
			const auto usedKb = totalKb - availKb;
			state.systemPercent = int((usedKb * 100) / totalKb);
		}
	}

	// 2. Read /proc/self/statm for app RSS memory
	QFile statm(u"/proc/self/statm"_q);
	if (statm.open(QIODevice::ReadOnly | QIODevice::Text)) {
		const auto parts = QString::fromUtf8(statm.readLine()).trimmed().split(' ', Qt::SkipEmptyParts);
		if (parts.size() >= 2) {
			const auto residentPages = parts[1].toULongLong();
			const auto pageSize = sysconf(_SC_PAGESIZE);
			if (pageSize > 0) {
				state.processRssMb = int((residentPages * pageSize) / (1024 * 1024));
			}
		}
	}
#elif defined(Q_OS_WIN)
	MEMORYSTATUSEX memInfo;
	memInfo.dwLength = sizeof(MEMORYSTATUSEX);
	if (GlobalMemoryStatusEx(&memInfo)) {
		state.systemPercent = int(memInfo.dwMemoryLoad);
	}
	PROCESS_MEMORY_COUNTERS pmc;
	if (Platform::Dlls::GetProcessMemoryInfo
		&& Platform::Dlls::GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
		state.processRssMb = int(pmc.WorkingSetSize / (1024 * 1024));
	}
#elif defined(Q_OS_MAC)
	mach_port_t host_port = mach_host_self();
	mach_msg_type_number_t host_size = sizeof(vm_statistics64_data_t) / sizeof(integer_t);
	vm_size_t pagesize;
	host_page_size(host_port, &pagesize);
	vm_statistics64_data_t vm_stat;
	if (host_statistics64(host_port, HOST_VM_INFO64, (host_info64_t)&vm_stat, &host_size) == KERN_SUCCESS) {
		const uint64 total = (vm_stat.free_count + vm_stat.active_count + vm_stat.inactive_count + vm_stat.wire_count) * pagesize;
		const uint64 used = (vm_stat.active_count + vm_stat.wire_count) * pagesize;
		if (total > 0) {
			state.systemPercent = int((used * 100) / total);
		}
	}
	mach_task_basic_info info;
	mach_msg_type_number_t size = MACH_TASK_BASIC_INFO_COUNT;
	if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &size) == KERN_SUCCESS) {
		state.processRssMb = int(info.resident_size / (1024 * 1024));
	}
#endif
}

// ---------------------------------------------------------------------------
// Weather State & Open-Meteo Query
// ---------------------------------------------------------------------------

struct WeatherState {
	qint64 lastFetch = 0;
	double temperature = 21.0;
	int weatherCode = 0;
	bool isFahrenheit = false;
	bool inFlight = false;
};

[[nodiscard]] WeatherState &GetWeatherState() {
	static auto state = WeatherState();
	return state;
}

[[nodiscard]] QString WeatherCodeToEmoji(int code) {
	if (code == 0) return QString::fromUtf8("☀️");
	if (code <= 3) return QString::fromUtf8("⛅");
	if (code == 45 || code == 48) return QString::fromUtf8("🌫️");
	if (code >= 51 && code <= 57) return QString::fromUtf8("🌧️");
	if (code >= 61 && code <= 67) return QString::fromUtf8("🌧️");
	if (code >= 71 && code <= 77) return QString::fromUtf8("❄️");
	if (code >= 80 && code <= 82) return QString::fromUtf8("🌦️");
	if (code >= 85 && code <= 86) return QString::fromUtf8("🌨️");
	if (code >= 95) return QString::fromUtf8("⛈️");
	return QString::fromUtf8("☀️");
}

// ---------------------------------------------------------------------------
// Exchange Rates State & CoinGecko / CBR Query
// ---------------------------------------------------------------------------

struct RatesState {
	qint64 lastFetch = 0;
	double btcUsd = 94250.0;
	double tonUsd = 5.45;
	double ethUsd = 3150.0;
	double solUsd = 185.0;
	double dogeUsd = 0.38;
	double notUsd = 0.0072;
	double usdRub = 98.5;
	double eurRub = 104.2;
	double eurUsd = 1.08;
	int activePairIndex = 0;
	bool inFlight = false;
};

[[nodiscard]] RatesState &GetRatesState() {
	static auto state = RatesState();
	return state;
}

[[nodiscard]] QString FormatTickerValue(const QString &rawTicker, const RatesState &state) {
	const auto ticker = rawTicker.trimmed().toUpper();
	if (ticker == u"BTC"_q || ticker == u"BTC/USD"_q || ticker == u"BITCOIN"_q) {
		return (state.btcUsd >= 1000.0)
			? QString("₿ $%1k").arg(state.btcUsd / 1000.0, 0, 'f', 1)
			: QString("₿ $%1").arg(qRound(state.btcUsd));
	} else if (ticker == u"TON"_q || ticker == u"TON/USD"_q || ticker == u"TONCOIN"_q) {
		return QString("💎 $%1").arg(state.tonUsd, 0, 'f', 2);
	} else if (ticker == u"ETH"_q || ticker == u"ETH/USD"_q || ticker == u"ETHEREUM"_q) {
		return (state.ethUsd >= 1000.0)
			? QString("Ξ $%1k").arg(state.ethUsd / 1000.0, 0, 'f', 1)
			: QString("Ξ $%1").arg(qRound(state.ethUsd));
	} else if (ticker == u"SOL"_q || ticker == u"SOL/USD"_q || ticker == u"SOLANA"_q) {
		return QString("◎ $%1").arg(state.solUsd, 0, 'f', 1);
	} else if (ticker == u"DOGE"_q || ticker == u"DOGE/USD"_q || ticker == u"DOGECOIN"_q) {
		return QString("🐶 $%1").arg(state.dogeUsd, 0, 'f', 3);
	} else if (ticker == u"NOT"_q || ticker == u"NOT/USD"_q || ticker == u"NOTCOIN"_q) {
		return QString("🦔 $%1").arg(state.notUsd, 0, 'f', 4);
	} else if (ticker == u"USD/RUB"_q || ticker == u"USDRUB"_q) {
		return QString("$ %1₽").arg(state.usdRub, 0, 'f', 1);
	} else if (ticker == u"EUR/RUB"_q || ticker == u"EURRUB"_q) {
		return QString("€ %1₽").arg(state.eurRub, 0, 'f', 1);
	} else if (ticker == u"EUR/USD"_q || ticker == u"EURUSD"_q) {
		return QString("€ $%1").arg(state.eurUsd, 0, 'f', 2);
	}
	return QString("%1: $%2").arg(ticker).arg(state.tonUsd, 0, 'f', 2);
}

// ---------------------------------------------------------------------------
// Icon Painter Helper
// ---------------------------------------------------------------------------

void PaintPillIcon(QPainter &p, PillType type, const QRect &r, const QColor &fg) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing, true);

	const auto cx = r.center().x();
	const auto cy = r.center().y();

	switch (type) {
	case PillType::DcPing: {
		p.setPen(Qt::NoPen);
		p.setBrush(fg);
		p.drawEllipse(QPointF(cx, cy + 2), 1.6, 1.6);

		p.setBrush(Qt::NoBrush);
		p.setPen(QPen(fg, 1.3));
		p.drawArc(QRectF(cx - 4, cy - 2, 8, 8), 45 * 16, 90 * 16);
		p.drawArc(QRectF(cx - 7, cy - 5, 14, 14), 45 * 16, 90 * 16);
		break;
	}
	case PillType::NetSpeed: {
		p.setPen(QPen(fg, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		p.drawLine(cx, cy - 4, cx, cy + 3);
		p.drawLine(cx - 3, cy, cx, cy + 3);
		p.drawLine(cx + 3, cy, cx, cy + 3);
		p.drawLine(cx - 4, cy + 5, cx + 4, cy + 5);
		break;
	}
	case PillType::Ram: {
		p.setPen(QPen(fg, 1.2));
		p.setBrush(Qt::NoBrush);
		p.drawRoundedRect(QRectF(cx - 4, cy - 4, 8, 8), 1.5, 1.5);
		p.drawLine(cx - 2, cy - 6, cx - 2, cy - 4);
		p.drawLine(cx + 2, cy - 6, cx + 2, cy - 4);
		p.drawLine(cx - 2, cy + 4, cx - 2, cy + 6);
		p.drawLine(cx + 2, cy + 4, cx + 2, cy + 6);
		p.drawLine(cx - 6, cy - 2, cx - 4, cy - 2);
		p.drawLine(cx - 6, cy + 2, cx - 4, cy + 2);
		p.drawLine(cx + 4, cy - 2, cx + 6, cy - 2);
		p.drawLine(cx + 4, cy + 2, cx + 6, cy + 2);
		break;
	}
	case PillType::Ghost: {
		QPainterPath gp;
		gp.moveTo(cx - 4, cy + 4);
		gp.lineTo(cx - 4, cy);
		gp.arcTo(QRectF(cx - 4, cy - 5, 8, 8), 180, -180);
		gp.lineTo(cx + 4, cy + 4);
		gp.lineTo(cx + 2, cy + 2.5);
		gp.lineTo(cx, cy + 4);
		gp.lineTo(cx - 2, cy + 2.5);
		gp.closeSubpath();
		p.setPen(QPen(fg, 1.1));
		p.setBrush(QColor(fg.red(), fg.green(), fg.blue(), 60));
		p.drawPath(gp);
		p.setPen(Qt::NoPen);
		p.setBrush(fg);
		p.drawEllipse(QPointF(cx - 1.5, cy - 1), 0.9, 0.9);
		p.drawEllipse(QPointF(cx + 1.5, cy - 1), 0.9, 0.9);
		break;
	}
	case PillType::Proxy: {
		QPainterPath sp;
		sp.moveTo(cx - 4.5, cy - 4.5);
		sp.lineTo(cx + 4.5, cy - 4.5);
		sp.lineTo(cx + 4.5, cy - 1.0);
		sp.quadTo(cx + 4.0, cy + 4.0, cx, cy + 5.5);
		sp.quadTo(cx - 4.0, cy + 4.0, cx - 4.5, cy - 1.0);
		sp.closeSubpath();
		p.setPen(QPen(fg, 1.2));
		p.setBrush(QColor(fg.red(), fg.green(), fg.blue(), 50));
		p.drawPath(sp);
		p.setPen(QPen(fg, 1.2, Qt::SolidLine, Qt::RoundCap));
		p.drawLine(cx - 2, cy, cx - 0.5, cy + 1.8);
		p.drawLine(cx - 0.5, cy + 1.8, cx + 2.2, cy - 1.5);
		break;
	}
	case PillType::Weather: {
		p.setPen(QPen(QColor(255, 185, 45), 1.2));
		p.setBrush(QColor(255, 185, 45, 140));
		p.drawEllipse(QPointF(cx, cy), 3.2, 3.2);
		for (int i = 0; i < 8; ++i) {
			const double angle = i * (kPi / 4.0);
			const double r1 = 4.2;
			const double r2 = 5.7;
			p.drawLine(
				QPointF(cx + std::cos(angle) * r1, cy + std::sin(angle) * r1),
				QPointF(cx + std::cos(angle) * r2, cy + std::sin(angle) * r2));
		}
		break;
	}
	case PillType::Rates: {
		p.setPen(QPen(QColor(247, 147, 26), 1.2));
		p.setBrush(QColor(247, 147, 26, 60));
		p.drawEllipse(QPointF(cx, cy), 4.8, 4.8);
		p.setFont(QFont(u"Arial"_q, 7, QFont::Bold));
		p.setPen(QColor(247, 147, 26));
		p.drawText(QRectF(cx - 4, cy - 4, 8, 8), Qt::AlignCenter, u"$"_q);
		break;
	}
	}

	p.restore();
}

} // namespace

// ---------------------------------------------------------------------------
// Global Settings Accessors
// ---------------------------------------------------------------------------

bool Enabled() {
	return ConfigBool(kPillStackEnabledKey, true);
}

bool PillEnabled(PillType type) {
	switch (type) {
	case PillType::DcPing: return ConfigBool(kDcPingKey, true);
	case PillType::NetSpeed: return ConfigBool(kNetSpeedKey, true);
	case PillType::Ram: return ConfigBool(kRamKey, true);
	case PillType::Ghost: return ConfigBool(kGhostKey, true);
	case PillType::Proxy: return ConfigBool(kProxyKey, true);
	case PillType::Weather: return ConfigBool(kWeatherKey, true);
	case PillType::Rates: return ConfigBool(kRatesKey, true);
	}
	return true;
}

void SetPillEnabled(PillType type, bool enabled) {
	switch (type) {
	case PillType::DcPing: SetConfigBool(kDcPingKey, enabled); break;
	case PillType::NetSpeed: SetConfigBool(kNetSpeedKey, enabled); break;
	case PillType::Ram: SetConfigBool(kRamKey, enabled); break;
	case PillType::Ghost: SetConfigBool(kGhostKey, enabled); break;
	case PillType::Proxy: SetConfigBool(kProxyKey, enabled); break;
	case PillType::Weather: SetConfigBool(kWeatherKey, enabled); break;
	case PillType::Rates: SetConfigBool(kRatesKey, enabled); break;
	}
}

// ---------------------------------------------------------------------------
// Weather & Rates Config Helpers
// ---------------------------------------------------------------------------

WeatherUnit CurrentWeatherUnit() {
	return (ConfigInt(kWeatherUnitKey, 0) == 1)
		? WeatherUnit::Fahrenheit
		: WeatherUnit::Celsius;
}

void SetWeatherUnit(WeatherUnit unit) {
	auto &wstate = GetWeatherState();
	wstate.isFahrenheit = (unit == WeatherUnit::Fahrenheit);
	SetConfigInt(kWeatherUnitKey, wstate.isFahrenheit ? 1 : 0);
	RefreshWeather(true);
}

void ToggleWeatherUnit() {
	auto &wstate = GetWeatherState();
	wstate.isFahrenheit = !wstate.isFahrenheit;
	SetConfigInt(kWeatherUnitKey, wstate.isFahrenheit ? 1 : 0);
	RefreshWeather(true);
}

QString CurrentWeatherCity() {
	return ConfigString(kWeatherCityKey, u"Berlin"_q);
}

void SetWeatherLocation(const QString &city, const QString &lat, const QString &lon) {
	SetConfigString(kWeatherCityKey, city.trimmed());
	SetConfigString(kWeatherLatKey, lat.trimmed());
	SetConfigString(kWeatherLonKey, lon.trimmed());
	RefreshWeather(true);
}

QString CurrentRatesTickers() {
	return ConfigString(kRateTickersKey, u"BTC,TON,ETH,SOL,USD/RUB,EUR/RUB"_q);
}

void SetRatesTickers(const QString &tickers) {
	SetConfigString(kRateTickersKey, tickers.trimmed());
	RefreshRates(true);
}

// ---------------------------------------------------------------------------
// Formatting Helpers
// ---------------------------------------------------------------------------

QString FormatSpeed(int64 bytesPerSec) {
	if (bytesPerSec < 0) bytesPerSec = 0;
	if (bytesPerSec < 1024) {
		return QString::number(bytesPerSec) + u" B/s"_q;
	} else if (bytesPerSec < 1024 * 1024) {
		return QString::number(bytesPerSec / 1024.0, 'f', 1) + u" KB/s"_q;
	} else if (bytesPerSec < 1024 * 1024 * 1024) {
		return QString::number(bytesPerSec / (1024.0 * 1024.0), 'f', 1) + u" MB/s"_q;
	} else {
		return QString::number(bytesPerSec / (1024.0 * 1024.0 * 1024.0), 'f', 2) + u" GB/s"_q;
	}
}

QString FormatBytes(int64 bytes) {
	if (bytes < 0) bytes = 0;
	if (bytes < 1024) {
		return QString::number(bytes) + u" B"_q;
	} else if (bytes < 1024 * 1024) {
		return QString::number(bytes / 1024.0, 'f', 1) + u" KB"_q;
	} else if (bytes < 1024 * 1024 * 1024) {
		return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + u" MB"_q;
	} else {
		return QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2) + u" GB"_q;
	}
}

// ---------------------------------------------------------------------------
// Telemetry & Live Queries
// ---------------------------------------------------------------------------

int CurrentDcPingMs() {
	return GetDcPingState().pingMs;
}

QString CurrentNetSpeedText() {
	const auto &state = GetNetSpeedState();
	switch (state.mode) {
	case NetSpeedDisplayMode::Dominant:
		return (state.speedRx >= state.speedTx)
			? (u"↓"_q + FormatSpeed(state.speedRx))
			: (u"↑"_q + FormatSpeed(state.speedTx));
	case NetSpeedDisplayMode::Download:
		return u"↓"_q + FormatSpeed(state.speedRx);
	case NetSpeedDisplayMode::Upload:
		return u"↑"_q + FormatSpeed(state.speedTx);
	case NetSpeedDisplayMode::Combined:
		return u"↓"_q + FormatSpeed(state.speedRx) + u" ↑"_q + FormatSpeed(state.speedTx);
	}
	return u"↓0 B/s"_q;
}

QString CurrentRamText() {
	const auto &state = GetRamState();
	if (state.mode == RamDisplayMode::AppRss) {
		return QString::number(state.processRssMb) + u" MB"_q;
	}
	return QString::number(state.systemPercent) + u"% RAM"_q;
}

QString CurrentWeatherText() {
	const auto &state = GetWeatherState();
	const auto emoji = WeatherCodeToEmoji(state.weatherCode);
	if (state.isFahrenheit) {
		const auto f = state.temperature * 9.0 / 5.0 + 32.0;
		return QString("%1 %2%3°F").arg(emoji).arg(f > 0 ? "+" : "").arg(qRound(f));
	}
	return QString("%1 %2%3°C").arg(emoji).arg(state.temperature > 0 ? "+" : "").arg(qRound(state.temperature));
}

QString CurrentRatesText() {
	const auto &state = GetRatesState();
	const auto tickers = CurrentRatesTickers().split(',', Qt::SkipEmptyParts);
	if (tickers.isEmpty()) {
		return (state.btcUsd >= 1000.0)
			? QString("₿ $%1k").arg(state.btcUsd / 1000.0, 0, 'f', 1)
			: QString("₿ $%1").arg(qRound(state.btcUsd));
	}
	const auto idx = std::abs(state.activePairIndex) % tickers.size();
	return FormatTickerValue(tickers[idx], state);
}

void TriggerDcPingProbe(Window::SessionController *controller) {
	auto &state = GetDcPingState();
	if (state.inFlight) {
		return;
	}

	if (controller) {
		const auto dc = int(controller->session().mainDcId());
		if (dc > 0 && dc <= 5) {
			state.dcId = dc;
		}
	}

	state.inFlight = true;
	const auto host = GetDcHost(state.dcId);

	auto socket = new QTcpSocket();
	auto timer = std::make_shared<QElapsedTimer>();
	auto finished = std::make_shared<bool>(false);
	const auto guardSocket = QPointer<QTcpSocket>(socket);
	timer->start();

	const auto finish = [guardSocket, finished, &state](bool connected, const std::shared_ptr<QElapsedTimer> &t) {
		if (*finished) {
			return;
		}
		*finished = true;
		if (connected) {
			state.pingMs = int(t->elapsed());
			state.lastProbeTime = QDateTime::currentMSecsSinceEpoch();
		}
		state.inFlight = false;
		if (guardSocket) {
			guardSocket->abort();
			guardSocket->deleteLater();
		}
	};

	QObject::connect(socket, &QTcpSocket::connected, [finish, timer] {
		finish(true, timer);
	});

	QObject::connect(socket, &QTcpSocket::errorOccurred, [finish, timer](QAbstractSocket::SocketError) {
		finish(false, timer);
	});

	socket->connectToHost(host, 443);

	// Timeout safeguard
	QTimer::singleShot(2500, socket, [finish, timer] {
		finish(false, timer);
	});
}

void RefreshWeather(bool force) {
	auto &state = GetWeatherState();
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (!force && state.lastFetch > 0 && (now - state.lastFetch < 15 * 60 * 1000)) {
		return;
	}
	if (state.inFlight) {
		return;
	}
	state.inFlight = true;

	const auto latStr = ConfigString(kWeatherLatKey, u"52.5200"_q);
	const auto lonStr = ConfigString(kWeatherLonKey, u"13.4050"_q);

	const auto urlStr = QString(
		"https://api.open-meteo.com/v1/forecast?latitude=%1&longitude=%2&current=temperature_2m,weather_code"
	).arg(latStr, lonStr);

	auto req = QNetworkRequest(QUrl(urlStr));
	req.setHeader(QNetworkRequest::UserAgentHeader, u"MoltenGram/2.0"_q);

	const auto reply = NetworkManager()->get(req);
	QObject::connect(reply, &QNetworkReply::finished, [=, &state] {
		reply->deleteLater();
		state.inFlight = false;
		if (reply->error() == QNetworkReply::NoError) {
			const auto doc = QJsonDocument::fromJson(reply->readAll());
			const auto obj = doc.object();
			const auto current = obj.value(u"current"_q).toObject();
			if (!current.isEmpty()) {
				state.temperature = current.value(u"temperature_2m"_q).toDouble(state.temperature);
				state.weatherCode = current.value(u"weather_code"_q).toInt(state.weatherCode);
				state.lastFetch = QDateTime::currentMSecsSinceEpoch();
			}
		}
	});
}

void RefreshRates(bool force) {
	auto &state = GetRatesState();
	const auto now = QDateTime::currentMSecsSinceEpoch();
	if (!force && state.lastFetch > 0 && (now - state.lastFetch < 10 * 60 * 1000)) {
		return;
	}
	if (state.inFlight) {
		return;
	}
	state.inFlight = true;

	// 1. CoinGecko API for BTC, TON, ETH, SOL, DOGE, NOT
	const auto cgUrl = QUrl(u"https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,the-open-network,ethereum,solana,dogecoin,notcoin&vs_currencies=usd,eur,rub"_q);
	auto cgReq = QNetworkRequest(cgUrl);
	cgReq.setHeader(QNetworkRequest::UserAgentHeader, u"MoltenGram/2.0"_q);

	const auto cgReply = NetworkManager()->get(cgReq);
	QObject::connect(cgReply, &QNetworkReply::finished, [=, &state] {
		cgReply->deleteLater();
		state.inFlight = false;
		if (cgReply->error() == QNetworkReply::NoError) {
			const auto doc = QJsonDocument::fromJson(cgReply->readAll());
			const auto obj = doc.object();

			const auto btc = obj.value(u"bitcoin"_q).toObject();
			if (!btc.isEmpty()) {
				state.btcUsd = btc.value(u"usd"_q).toDouble(state.btcUsd);
			}

			const auto ton = obj.value(u"the-open-network"_q).toObject();
			if (!ton.isEmpty()) {
				state.tonUsd = ton.value(u"usd"_q).toDouble(state.tonUsd);
			}

			const auto eth = obj.value(u"ethereum"_q).toObject();
			if (!eth.isEmpty()) {
				state.ethUsd = eth.value(u"usd"_q).toDouble(state.ethUsd);
			}

			const auto sol = obj.value(u"solana"_q).toObject();
			if (!sol.isEmpty()) {
				state.solUsd = sol.value(u"usd"_q).toDouble(state.solUsd);
			}

			const auto doge = obj.value(u"dogecoin"_q).toObject();
			if (!doge.isEmpty()) {
				state.dogeUsd = doge.value(u"usd"_q).toDouble(state.dogeUsd);
			}

			const auto notc = obj.value(u"notcoin"_q).toObject();
			if (!notc.isEmpty()) {
				state.notUsd = notc.value(u"usd"_q).toDouble(state.notUsd);
			}

			state.lastFetch = QDateTime::currentMSecsSinceEpoch();
		}
	});

	// 2. CBR API for USD/RUB and EUR/RUB
	const auto cbrUrl = QUrl(u"https://www.cbr-xml-daily.ru/daily_json.js"_q);
	auto cbrReq = QNetworkRequest(cbrUrl);
	cbrReq.setHeader(QNetworkRequest::UserAgentHeader, u"MoltenGram/2.0"_q);

	const auto cbrReply = NetworkManager()->get(cbrReq);
	QObject::connect(cbrReply, &QNetworkReply::finished, [=, &state] {
		cbrReply->deleteLater();
		if (cbrReply->error() == QNetworkReply::NoError) {
			const auto doc = QJsonDocument::fromJson(cbrReply->readAll());
			const auto valute = doc.object().value(u"Valute"_q).toObject();
			const auto usd = valute.value(u"USD"_q).toObject();
			const auto eur = valute.value(u"EUR"_q).toObject();
			if (!usd.isEmpty()) {
				state.usdRub = usd.value(u"Value"_q).toDouble(state.usdRub);
			}
			if (!eur.isEmpty()) {
				state.eurRub = eur.value(u"Value"_q).toDouble(state.eurRub);
			}
			if (state.usdRub > 0 && state.eurRub > 0) {
				state.eurUsd = state.eurRub / state.usdRub;
			}
		}
	});
}

void CycleRatesPair() {
	auto &state = GetRatesState();
	const auto tickers = CurrentRatesTickers().split(',', Qt::SkipEmptyParts);
	const int count = std::max(1, int(tickers.size()));
	state.activePairIndex = (state.activePairIndex + 1) % count;
	SetConfigInt(kRatePairKey, state.activePairIndex);
}

void CycleNetSpeedMode() {
	auto &state = GetNetSpeedState();
	state.mode = static_cast<NetSpeedDisplayMode>((int(state.mode) + 1) % 4);
	SetConfigInt(kNetSpeedModeKey, int(state.mode));
}

void CycleRamMode() {
	auto &state = GetRamState();
	state.mode = (state.mode == RamDisplayMode::SystemPercent)
		? RamDisplayMode::AppRss
		: RamDisplayMode::SystemPercent;
	SetConfigInt(kRamModeKey, int(state.mode));
}

// ---------------------------------------------------------------------------
// Standalone Badges Painter
// ---------------------------------------------------------------------------

void PaintPills(
		QPainter &p,
		const QRect &geometry,
		const std::vector<PillInfo> &pills,
		int hoveredIndex,
		int pressedIndex) {
	p.save();
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);

	const bool isDark = (st::dialogsBg->c.lightness() < 128);

	for (int i = 0; i < int(pills.size()); ++i) {
		const auto &pill = pills[i];
		const auto &rect = pill.rect;
		if (rect.isEmpty() || !geometry.intersects(rect)) {
			continue;
		}

		const auto hovered = (i == hoveredIndex);
		const auto pressed = (i == pressedIndex);

		const auto h = rect.height();
		const auto r = h / 2.0;

		QPainterPath path;
		path.addRoundedRect(QRectF(rect), r, r);

		QColor fill;
		QColor border;

		if (pill.type == PillType::Ghost && Extera::GhostActive()) {
			fill = QColor(160, 110, 240, hovered ? 75 : (pressed ? 95 : 55));
			border = QColor(160, 110, 240, 150);
		} else if (pill.type == PillType::Proxy && Core::App().settings().proxy().isEnabled()) {
			fill = QColor(52, 180, 90, hovered ? 75 : (pressed ? 95 : 55));
			border = QColor(52, 180, 90, 150);
		} else if (pill.type == PillType::DcPing) {
			const auto ping = CurrentDcPingMs();
			if (ping > 0 && ping < 70) {
				fill = isDark ? QColor(76, 175, 80, hovered ? 55 : 38) : QColor(76, 175, 80, hovered ? 45 : 30);
				border = QColor(76, 175, 80, 110);
			} else if (ping >= 70 && ping < 150) {
				fill = isDark ? QColor(255, 193, 7, hovered ? 55 : 38) : QColor(255, 193, 7, hovered ? 45 : 30);
				border = QColor(255, 193, 7, 110);
			} else {
				fill = isDark ? QColor(244, 67, 54, hovered ? 55 : 38) : QColor(244, 67, 54, hovered ? 45 : 30);
				border = QColor(244, 67, 54, 110);
			}
		} else {
			if (isDark) {
				fill = QColor(255, 255, 255, hovered ? 32 : (pressed ? 46 : 20));
				border = QColor(255, 255, 255, 34);
			} else {
				fill = QColor(0, 0, 0, hovered ? 26 : (pressed ? 40 : 15));
				border = QColor(0, 0, 0, 24);
			}
		}

		p.fillPath(path, fill);
		p.setPen(QPen(border, style::ConvertScale(1.)));
		p.drawPath(path);

		// Icon
		const auto iconSize = style::ConvertScale(13);
		const auto padLeft = style::ConvertScale(8);
		const auto iconRect = QRect(
			rect.left() + padLeft,
			rect.top() + (h - iconSize) / 2,
			iconSize,
			iconSize);

		const auto iconColor = (pill.type == PillType::Ghost && Extera::GhostActive())
			? QColor(200, 160, 255)
			: (pill.type == PillType::Proxy && Core::App().settings().proxy().isEnabled())
			? QColor(100, 220, 130)
			: st::windowFg->c;

		PaintPillIcon(p, pill.type, iconRect, iconColor);

		// Text
		const auto textLeft = iconRect.right() + style::ConvertScale(5);
		const auto textRight = rect.right() - style::ConvertScale(8);
		const auto textRect = QRect(textLeft, rect.top(), std::max(1, textRight - textLeft), h);

		p.setFont(st::semiboldFont);
		p.setPen(st::windowFg);
		p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, pill.text);
	}

	p.restore();
}

// ---------------------------------------------------------------------------
// UI Widget Implementation
// ---------------------------------------------------------------------------

Widget::Widget(QWidget *parent, Window::SessionController *controller)
: Ui::RpWidget(parent)
, _controller(controller)
, _refreshTimer([=] { updateData(); }) {
	setAttribute(Qt::WA_OpaquePaintEvent, false);
	setMouseTracking(true);

	// Load initial state modes from config
	GetNetSpeedState().mode = static_cast<NetSpeedDisplayMode>(ConfigInt(kNetSpeedModeKey, 0));
	GetRamState().mode = static_cast<RamDisplayMode>(ConfigInt(kRamModeKey, 0));
	GetRatesState().activePairIndex = ConfigInt(kRatePairKey, 0);
	GetWeatherState().isFahrenheit = (ConfigInt(kWeatherUnitKey, 0) == 1);

	updateData();
	_refreshTimer.callEach(2000);
}

Widget::~Widget() = default;

void Widget::updateData() {
	if (!Enabled()) {
		if (isVisible()) hide();
		return;
	} else if (!isVisible()) {
		show();
	}

	SampleNetworkTraffic();
	SampleMemoryUsage();

	// Periodically trigger weather and rates fetch
	RefreshWeather(false);
	RefreshRates(false);

	rebuildPills();
	update();
}

void Widget::rebuildPills() {
	_pills.clear();

	const auto addPill = [&](PillType type, const QString &id, const QString &text, const QString &tooltip, const QColor &accent) {
		if (PillEnabled(type)) {
			_pills.push_back({
				.type = type,
				.id = id,
				.text = text,
				.tooltip = tooltip,
				.accentColor = accent,
				.active = true,
			});
		}
	};

	addPill(
		PillType::DcPing,
		u"dc_ping"_q,
		QString("DC%1: %2ms").arg(GetDcPingState().dcId).arg(CurrentDcPingMs()),
		u"Telegram Data Center RTT (Click to re-ping socket)"_q,
		QColor(76, 175, 80));

	addPill(
		PillType::NetSpeed,
		u"net_speed"_q,
		CurrentNetSpeedText(),
		u"Network Throughput (Click to cycle display mode)"_q,
		QColor(33, 150, 243));

	addPill(
		PillType::Ram,
		u"ram"_q,
		CurrentRamText(),
		u"Memory Usage (Click to toggle System % / App Process RSS)"_q,
		QColor(156, 39, 176));

	addPill(
		PillType::Ghost,
		u"ghost"_q,
		Extera::GhostActive() ? u"Ghost: ON"_q : u"Ghost: OFF"_q,
		u"Ghost Mode (Click to toggle stealth ON/OFF)"_q,
		QColor(160, 110, 240));

	addPill(
		PillType::Proxy,
		u"proxy"_q,
		Core::App().settings().proxy().isEnabled() ? u"Proxy: ON"_q : u"Proxy: OFF"_q,
		u"Proxy Connection (Click to toggle ON/OFF)"_q,
		QColor(52, 180, 90));

	const auto cityName = CurrentWeatherCity();
	addPill(
		PillType::Weather,
		u"weather"_q,
		CurrentWeatherText(),
		QString("Weather in %1 (Click to toggle °C / °F)").arg(cityName),
		QColor(255, 152, 0));

	const auto tickers = CurrentRatesTickers().split(',', Qt::SkipEmptyParts);
	const auto &rstate = GetRatesState();
	const auto tickerIdx = (!tickers.isEmpty()) ? (std::abs(rstate.activePairIndex) % tickers.size()) : 0;
	const auto activeTicker = (!tickers.isEmpty()) ? tickers[tickerIdx].trimmed() : u"BTC"_q;
	addPill(
		PillType::Rates,
		u"rates"_q,
		CurrentRatesText(),
		QString("Exchange Rates (%1 - Click to cycle tickers)").arg(activeTicker),
		QColor(247, 147, 26));

	// Compute geometries adhering to MD3 8dp spatial grid
	const auto font = st::semiboldFont;
	const auto pillH = style::ConvertScale(24);
	const auto gap = style::ConvertScale(8);
	const auto padX = style::ConvertScale(8);
	const auto iconSize = style::ConvertScale(14);
	const auto textGap = style::ConvertScale(6);

	int curX = padX;
	const int curY = (height() > pillH) ? ((height() - pillH) / 2) : 0;

	for (auto &pill : _pills) {
		const auto textWidth = font->width(pill.text);
		const auto pillW = padX + iconSize + textGap + textWidth + padX;
		pill.rect = QRect(curX, curY, pillW, pillH);
		curX += pillW + gap;
	}

	const int totalW = curX + padX;
	_maxScrollX = std::max(0, totalW - width());
	_scrollX = std::clamp(_scrollX, 0, _maxScrollX);
}

int Widget::naturalWidth() const {
	if (_pills.empty()) {
		return 0;
	}
	return _pills.back().rect.right() + style::ConvertScale(8);
}

int Widget::pillAt(const QPoint &pos) const {
	const auto translatedPos = pos + QPoint(_scrollX, 0);
	for (int i = 0; i < int(_pills.size()); ++i) {
		if (_pills[i].rect.contains(translatedPos)) {
			return i;
		}
	}
	return -1;
}

void Widget::paintEvent(QPaintEvent *e) {
	Painter p(this);
	p.setClipRect(rect());

	p.save();
	p.translate(-_scrollX, 0);
	const auto visibleRect = rect().translated(_scrollX, 0);
	PaintPills(p, visibleRect, _pills, _hoveredIndex, _pressedIndex);
	p.restore();

	// Smooth fade edge gradients matching dialogs sidebar background
	if (_scrollX > 0) {
		const auto edgeW = style::ConvertScale(16);
		QLinearGradient lg(0, 0, edgeW, 0);
		lg.setColorAt(0, st::dialogsBg->c);
		lg.setColorAt(1, QColor(st::dialogsBg->c.red(), st::dialogsBg->c.green(), st::dialogsBg->c.blue(), 0));
		p.fillRect(0, 0, edgeW, height(), lg);
	}
	if (_scrollX < _maxScrollX) {
		const auto edgeW = style::ConvertScale(16);
		QLinearGradient rg(width() - edgeW, 0, width(), 0);
		rg.setColorAt(0, QColor(st::dialogsBg->c.red(), st::dialogsBg->c.green(), st::dialogsBg->c.blue(), 0));
		rg.setColorAt(1, st::dialogsBg->c);
		p.fillRect(width() - edgeW, 0, edgeW, height(), rg);
	}
}

void Widget::wheelEvent(QWheelEvent *e) {
	const auto delta = e->angleDelta();
	const auto d = (delta.x() != 0) ? delta.x() : delta.y();
	if (d != 0) {
		_scrollX = std::clamp(_scrollX - (d > 0 ? 35 : -35), 0, _maxScrollX);
		update();
		e->accept();
	}
}

void Widget::mouseMoveEvent(QMouseEvent *e) {
	if (e->buttons() & Qt::LeftButton) {
		const auto dx = e->pos().x() - _dragStartPos.x();
		if (std::abs(dx) > 3) {
			_dragging = true;
			_scrollX = std::clamp(_dragStartScrollX - dx, 0, _maxScrollX);
			_pressedIndex = -1;
			update();
			return;
		}
	}
	const auto idx = pillAt(e->pos());
	if (idx != _hoveredIndex) {
		_hoveredIndex = idx;
		setCursor((_hoveredIndex >= 0) ? Qt::PointingHandCursor : Qt::ArrowCursor);
		if (_hoveredIndex >= 0 && _hoveredIndex < int(_pills.size())) {
			setToolTip(_pills[_hoveredIndex].tooltip);
		} else {
			setToolTip(QString());
		}
		update();
	}
}

void Widget::leaveEventHook(QEvent *e) {
	if (_hoveredIndex != -1) {
		_hoveredIndex = -1;
		setToolTip(QString());
		update();
	}
}

void Widget::mousePressEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		_dragStartPos = e->pos();
		_dragStartScrollX = _scrollX;
		_dragging = false;
		_pressedIndex = pillAt(e->pos());
		update();
	}
}

void Widget::mouseReleaseEvent(QMouseEvent *e) {
	if (e->button() == Qt::LeftButton) {
		if (!_dragging) {
			const auto idx = pillAt(e->pos());
			if (idx >= 0 && idx == _pressedIndex) {
				onPillClicked(idx);
			}
		}
		_dragging = false;
		_pressedIndex = -1;
		update();
	}
}

void Widget::contextMenuEvent(QContextMenuEvent *e) {
	const auto idx = pillAt(e->pos());
	auto menu = std::make_unique<QMenu>(this);

	if (_controller) {
		menu->addAction(u"PillStack Settings..."_q, [=] {
			ShowSettingsBox(_controller);
		});
		menu->addSeparator();
	}

	menu->addAction(u"Refresh All Telemetry"_q, [=] {
		TriggerDcPingProbe(_controller);
		RefreshWeather(true);
		RefreshRates(true);
		updateData();
	});

	if (idx >= 0 && idx < int(_pills.size())) {
		const auto &pill = _pills[idx];
		menu->addSeparator();
		menu->addAction(QString("Hide '%1' Badge").arg(pill.id), [=] {
			SetPillEnabled(pill.type, false);
			updateData();
		});
	}

	menu->exec(e->globalPos());
}

void Widget::onPillClicked(int index) {
	if (index < 0 || index >= int(_pills.size())) {
		return;
	}
	const auto &pill = _pills[index];
	switch (pill.type) {
	case PillType::DcPing:
		TriggerDcPingProbe(_controller);
		updateData();
		break;
	case PillType::NetSpeed:
		CycleNetSpeedMode();
		updateData();
		break;
	case PillType::Ram:
		CycleRamMode();
		updateData();
		break;
	case PillType::Ghost:
		Extera::SetGhostAll(!Extera::GhostActive());
		updateData();
		for (const auto w : QApplication::topLevelWidgets()) {
			if (w) w->update();
		}
		break;
	case PillType::Proxy: {
		auto &settings = Core::App().settings().proxy();
		if (settings.isEnabled()) {
			settings.setSettings(MTP::ProxyData::Settings::Disabled);
		} else {
			settings.setSettings(MTP::ProxyData::Settings::Enabled);
		}
		settings.connectionTypeChangesNotify();
		Core::App().saveSettingsDelayed();
		updateData();
		break;
	}
	case PillType::Weather: {
		ToggleWeatherUnit();
		updateData();
		break;
	}
	case PillType::Rates:
		CycleRatesPair();
		updateData();
		break;
	}
}

// ---------------------------------------------------------------------------
// Settings Box Implementation
// ---------------------------------------------------------------------------

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setWidth(st::boxWideWidth);
		box->setTitle(rpl::single(u"PillStack Information Badges"_q));
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });
		const auto container = box->verticalLayout();

		Ui::AddSubsectionTitle(container, rpl::single(u"General"_q));
		Ui::AddSkip(container, 8);

		const auto masterToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Enable PillStack Bar in Sidebar Header"_q),
				st::settingsButtonNoIcon));
		masterToggle->toggleOn(rpl::single(Enabled()));
		masterToggle->toggledChanges() | rpl::on_next([](bool enabled) {
			SetConfigBool(kPillStackEnabledKey, enabled);
			for (const auto w : QApplication::topLevelWidgets()) {
				if (w) w->update();
			}
		}, masterToggle->lifetime());

		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Telemetry & Hardware Monitors"_q));
		Ui::AddSkip(container, 8);

		const auto pingToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"DC Ping & RTT Latency Badge"_q),
				st::settingsButtonNoIcon));
		pingToggle->toggleOn(rpl::single(PillEnabled(PillType::DcPing)));
		pingToggle->toggledChanges() | rpl::on_next([](bool val) {
			SetPillEnabled(PillType::DcPing, val);
		}, pingToggle->lifetime());

		Ui::AddSkip(container, 4);
		const auto pingBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(QString("Probe Active Data Center Now (DC%1: %2ms)").arg(GetDcPingState().dcId).arg(CurrentDcPingMs())),
				st::settingsButtonNoIcon));
		pingBtn->setClickedCallback([=] {
			TriggerDcPingProbe(controller);
		});

		Ui::AddSkip(container, 8);
		const auto speedToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Network Throughput Monitor Badge"_q),
				st::settingsButtonNoIcon));
		speedToggle->toggleOn(rpl::single(PillEnabled(PillType::NetSpeed)));
		speedToggle->toggledChanges() | rpl::on_next([](bool val) {
			SetPillEnabled(PillType::NetSpeed, val);
		}, speedToggle->lifetime());

		Ui::AddSkip(container, 4);
		const auto speedModeBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Cycle Network Speed Mode (Dominant / Download / Upload / Combined)"_q),
				st::settingsButtonNoIcon));
		speedModeBtn->setClickedCallback([=] {
			CycleNetSpeedMode();
		});

		Ui::AddSkip(container, 8);
		const auto ramToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"RAM / Memory Usage Badge"_q),
				st::settingsButtonNoIcon));
		ramToggle->toggleOn(rpl::single(PillEnabled(PillType::Ram)));
		ramToggle->toggledChanges() | rpl::on_next([](bool val) {
			SetPillEnabled(PillType::Ram, val);
		}, ramToggle->lifetime());

		Ui::AddSkip(container, 4);
		const auto ramModeBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Toggle RAM Mode (System Memory % / Telegram Process RSS)"_q),
				st::settingsButtonNoIcon));
		ramModeBtn->setClickedCallback([=] {
			CycleRamMode();
		});

		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Privacy & Network Badges"_q));
		Ui::AddSkip(container, 8);

		const auto ghostToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Ghost Mode Status Badge"_q),
				st::settingsButtonNoIcon));
		ghostToggle->toggleOn(rpl::single(PillEnabled(PillType::Ghost)));
		ghostToggle->toggledChanges() | rpl::on_next([](bool val) {
			SetPillEnabled(PillType::Ghost, val);
		}, ghostToggle->lifetime());

		Ui::AddSkip(container, 4);
		const auto proxyToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Proxy Status Badge"_q),
				st::settingsButtonNoIcon));
		proxyToggle->toggleOn(rpl::single(PillEnabled(PillType::Proxy)));
		proxyToggle->toggledChanges() | rpl::on_next([](bool val) {
			SetPillEnabled(PillType::Proxy, val);
		}, proxyToggle->lifetime());

		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Live Weather (Open-Meteo)"_q));
		Ui::AddSkip(container, 8);

		const auto weatherToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Open-Meteo Weather Badge"_q),
				st::settingsButtonNoIcon));
		weatherToggle->toggleOn(rpl::single(PillEnabled(PillType::Weather)));
		weatherToggle->toggledChanges() | rpl::on_next([](bool val) {
			SetPillEnabled(PillType::Weather, val);
		}, weatherToggle->lifetime());

		Ui::AddSkip(container, 4);
		const auto unitToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Use Fahrenheit (°F) instead of Celsius (°C)"_q),
				st::settingsButtonNoIcon));
		unitToggle->toggleOn(rpl::single(GetWeatherState().isFahrenheit));
		unitToggle->toggledChanges() | rpl::on_next([](bool val) {
			SetWeatherUnit(val ? WeatherUnit::Fahrenheit : WeatherUnit::Celsius);
		}, unitToggle->lifetime());

		Ui::AddSkip(container, 8);
		const auto cityField = container->add(
			object_ptr<Ui::InputField>(
				container,
				st::defaultInputField,
				rpl::single(u"City Name"_q),
				CurrentWeatherCity()));

		Ui::AddSkip(container, 8);
		const auto latField = container->add(
			object_ptr<Ui::InputField>(
				container,
				st::defaultInputField,
				rpl::single(u"Latitude (e.g. 52.5200)"_q),
				ConfigString(kWeatherLatKey, u"52.5200"_q)));

		Ui::AddSkip(container, 8);
		const auto lonField = container->add(
			object_ptr<Ui::InputField>(
				container,
				st::defaultInputField,
				rpl::single(u"Longitude (e.g. 13.4050)"_q),
				ConfigString(kWeatherLonKey, u"13.4050"_q)));

		Ui::AddSkip(container, 8);
		const auto saveWeatherBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Save Weather Location & Fetch"_q),
				st::settingsButtonNoIcon));
		saveWeatherBtn->setClickedCallback([=] {
			SetWeatherLocation(
				cityField->getLastText(),
				latField->getLastText(),
				lonField->getLastText());
		});

		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Crypto & Currency Exchange Rates"_q));
		Ui::AddSkip(container, 8);

		const auto ratesToggle = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Exchange Rates Badge"_q),
				st::settingsButtonNoIcon));
		ratesToggle->toggleOn(rpl::single(PillEnabled(PillType::Rates)));
		ratesToggle->toggledChanges() | rpl::on_next([](bool val) {
			SetPillEnabled(PillType::Rates, val);
		}, ratesToggle->lifetime());

		Ui::AddSkip(container, 8);
		const auto tickersField = container->add(
			object_ptr<Ui::InputField>(
				container,
				st::defaultInputField,
				rpl::single(u"Customizable Tickers (comma-separated, e.g. BTC, TON, ETH, SOL, USD/RUB, EUR/RUB)"_q),
				CurrentRatesTickers()));

		Ui::AddSkip(container, 8);
		const auto saveTickersBtn = container->add(
			object_ptr<Ui::SettingsButton>(
				container,
				rpl::single(u"Save Tickers & Refresh Rates"_q),
				st::settingsButtonNoIcon));
		saveTickersBtn->setClickedCallback([=] {
			SetRatesTickers(tickersField->getLastText());
		});

		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Extera::PillStack
