/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <vector>

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class VerticalLayout;
} // namespace Ui

namespace Extera::DeviceSpoof {

struct Preset {
	QString id;
	QString title;
	QString deviceModel;
	QString systemVersion;
	QString appVersion;
	QString group;
	QString platform;
	bool isCustom = false;

	[[nodiscard]] QString formattedSummary() const;
};

// Global toggle for device spoofing
[[nodiscard]] bool Enabled();
void SetEnabled(bool enabled);

// Selected preset ID
[[nodiscard]] QString SelectedPresetId();
void SetSelectedPresetId(const QString &id);

// Active preset based on current configuration
[[nodiscard]] Preset ActivePreset();

// Complete list of built-in & custom presets
[[nodiscard]] const std::vector<Preset> &AllPresets();
[[nodiscard]] const Preset *FindPreset(const QString &id);

// Custom values configured by user
[[nodiscard]] QString CustomDeviceModel();
void SetCustomDeviceModel(const QString &model);

[[nodiscard]] QString CustomSystemVersion();
void SetCustomSystemVersion(const QString &sysVer);

[[nodiscard]] QString CustomAppVersion();
void SetCustomAppVersion(const QString &appVer);

// Hooks for MTProto MTPinitConnection & session connection initialization
[[nodiscard]] QString ApplyDeviceModel(const QString &original = QString());
[[nodiscard]] QString ApplySystemVersion(const QString &original = QString());
[[nodiscard]] QString ApplyAppVersion(const QString &original = QString());
[[nodiscard]] QString ApplyPlatform(const QString &original = QString());

// Overrides all three parameters at once if spoofing is enabled
void ApplyConnectionParameters(
	QString &deviceModel,
	QString &systemVersion,
	QString &appVersion);

// UI settings box and embedded settings section
void ShowSettingsBox(not_null<Window::SessionController*> controller);
void SetupSettings(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Extera::DeviceSpoof
