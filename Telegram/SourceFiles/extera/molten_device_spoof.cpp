/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_device_spoof.h"

#include "extera/extera_config.h"
#include "lang/lang_keys.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/toast/toast.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

namespace Extera::DeviceSpoof {
namespace {

const auto kKeyEnabled = u"spoof_device_enabled"_q;
const auto kKeyPreset = u"spoof_device_preset"_q;
const auto kKeyCustomModel = u"spoof_custom_model"_q;
const auto kKeyCustomSysVer = u"spoof_custom_sys_ver"_q;
const auto kKeyCustomAppVer = u"spoof_custom_app_ver"_q;

const auto kDefaultPresetId = u"moltenstar"_q;

const std::vector<Preset> &PresetsList() {
	static const auto kList = std::vector<Preset>{
		{
			.id = u"moltenstar"_q,
			.title = u"MoltenStar Flagship (MoltenGram)"_q,
			.deviceModel = u"MoltenStar MoltenGram"_q,
			.systemVersion = u"MoltenOS 2.0 (SDK 36)"_q,
			.appVersion = u"MoltenGram Desktop 7.4.4"_q,
			.group = u"MoltenStar"_q,
			.isCustom = false,
		},
		{
			.id = u"tdesktop_official"_q,
			.title = u"Telegram Desktop Official (PC)"_q,
			.deviceModel = u"PC 64bit"_q,
			.systemVersion = u"Windows 11"_q,
			.appVersion = u"Telegram Desktop 5.12.1"_q,
			.group = u"Official Desktop"_q,
			.isCustom = false,
		},
		{
			.id = u"telegram_macos"_q,
			.title = u"Telegram macOS (Native)"_q,
			.deviceModel = u"MacBook Pro 16\""_q,
			.systemVersion = u"macOS Sequoia 15.3"_q,
			.appVersion = u"Telegram macOS 11.7.1"_q,
			.group = u"macOS"_q,
			.isCustom = false,
		},
		{
			.id = u"telegram_ios"_q,
			.title = u"Telegram iOS (iPhone 16 Pro)"_q,
			.deviceModel = u"iPhone 16 Pro"_q,
			.systemVersion = u"iOS 18.3"_q,
			.appVersion = u"Telegram iOS 11.7.0"_q,
			.group = u"Apple iOS"_q,
			.isCustom = false,
		},
		{
			.id = u"telegram_ipad"_q,
			.title = u"Telegram iPad (iPad Pro 13\")"_q,
			.deviceModel = u"iPad Pro 13\""_q,
			.systemVersion = u"iPadOS 18.3"_q,
			.appVersion = u"Telegram iOS 11.7.0"_q,
			.group = u"Apple iOS"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_9_pro"_q,
			.title = u"Telegram Android (Google Pixel 9 Pro)"_q,
			.deviceModel = u"Google Pixel 9 Pro"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_11_pro"_q,
			.title = u"Telegram Android (Google Pixel 11 Pro Fold)"_q,
			.deviceModel = u"Google Pixel 11 Pro Fold"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 12.0.0 (6100)"_q,
			.group = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"samsung_s25"_q,
			.title = u"Telegram Android (Samsung Galaxy S25 Ultra)"_q,
			.deviceModel = u"Samsung Galaxy S25 Ultra"_q,
			.systemVersion = u"Android 15 (One UI 7.0)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_3"_q,
			.title = u"Telegram Android (Nothing Phone 3)"_q,
			.deviceModel = u"Nothing Phone (3)"_q,
			.systemVersion = u"Nothing OS 3.0 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"custom"_q,
			.title = u"Custom Device Name & App Version"_q,
			.deviceModel = u"Custom Device"_q,
			.systemVersion = u"Custom OS 1.0"_q,
			.appVersion = u"MoltenGram 7.4.4"_q,
			.group = u"Custom"_q,
			.isCustom = true,
		},
	};
	return kList;
}

} // namespace

QString Preset::formattedSummary() const {
	return deviceModel + u" | "_q + systemVersion + u" | "_q + appVersion;
}

bool Enabled() {
	return Config::Instance().boolValue(kKeyEnabled);
}

void SetEnabled(bool enabled) {
	Config::Instance().setBool(kKeyEnabled, enabled);
	Config::Instance().saveNow();
}

QString SelectedPresetId() {
	const auto val = Config::Instance().stringValue(kKeyPreset);
	return val.isEmpty() ? kDefaultPresetId : val;
}

void SetSelectedPresetId(const QString &id) {
	Config::Instance().setString(kKeyPreset, id);
	Config::Instance().saveNow();
}

Preset ActivePreset() {
	const auto id = SelectedPresetId();
	if (const auto preset = FindPreset(id)) {
		if (preset->isCustom) {
			auto custom = *preset;
			const auto m = CustomDeviceModel();
			const auto s = CustomSystemVersion();
			const auto a = CustomAppVersion();
			if (!m.isEmpty()) custom.deviceModel = m;
			if (!s.isEmpty()) custom.systemVersion = s;
			if (!a.isEmpty()) custom.appVersion = a;
			return custom;
		}
		return *preset;
	}
	return PresetsList().front();
}

const std::vector<Preset> &AllPresets() {
	return PresetsList();
}

const Preset *FindPreset(const QString &id) {
	for (const auto &preset : PresetsList()) {
		if (preset.id == id) {
			return &preset;
		}
	}
	return nullptr;
}

QString CustomDeviceModel() {
	return Config::Instance().stringValue(kKeyCustomModel);
}

void SetCustomDeviceModel(const QString &model) {
	Config::Instance().setString(kKeyCustomModel, model);
	Config::Instance().saveNow();
}

QString CustomSystemVersion() {
	return Config::Instance().stringValue(kKeyCustomSysVer);
}

void SetCustomSystemVersion(const QString &sysVer) {
	Config::Instance().setString(kKeyCustomSysVer, sysVer);
	Config::Instance().saveNow();
}

QString CustomAppVersion() {
	return Config::Instance().stringValue(kKeyCustomAppVer);
}

void SetCustomAppVersion(const QString &appVer) {
	Config::Instance().setString(kKeyCustomAppVer, appVer);
	Config::Instance().saveNow();
}

QString ApplyDeviceModel(const QString &original) {
	if (!Enabled()) {
		return original;
	}
	const auto preset = ActivePreset();
	return preset.deviceModel.isEmpty() ? original : preset.deviceModel;
}

QString ApplySystemVersion(const QString &original) {
	if (!Enabled()) {
		return original;
	}
	const auto preset = ActivePreset();
	return preset.systemVersion.isEmpty() ? original : preset.systemVersion;
}

QString ApplyAppVersion(const QString &original) {
	if (!Enabled()) {
		return original;
	}
	const auto preset = ActivePreset();
	return preset.appVersion.isEmpty() ? original : preset.appVersion;
}

void ApplyConnectionParameters(
		QString &deviceModel,
		QString &systemVersion,
		QString &appVersion) {
	if (!Enabled()) {
		return;
	}
	const auto preset = ActivePreset();
	if (!preset.deviceModel.isEmpty()) {
		deviceModel = preset.deviceModel;
	}
	if (!preset.systemVersion.isEmpty()) {
		systemVersion = preset.systemVersion;
	}
	if (!preset.appVersion.isEmpty()) {
		appVersion = preset.appVersion;
	}
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Device & Session Spoofing"_q));
		box->setWidth(st::boxWidth);

		const auto container = box->verticalLayout();

		Ui::AddSubsectionTitle(
			container,
			rpl::single(u"MTProto Device Information"_q));

		Ui::AddSkip(container, st::settingsCheckboxesSkip);

		const auto aboutLabel = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(u"Spoofs your outgoing device model, OS version, and client version in MTProto initConnection. This changes how active sessions appear on other devices and on Telegram servers."_q),
				st::boxLabel));

		Ui::AddSkip(container, st::settingsCheckboxesSkip);

		const auto enableCheckbox = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				u"Enable Device Spoofing"_q,
				Enabled(),
				st::defaultCheckbox));

		Ui::AddSkip(container, st::settingsCheckboxesSkip);
		Ui::AddSubsectionTitle(container, rpl::single(u"Device Presets"_q));

		const auto radioGroup = std::make_shared<Ui::RadiobuttonGroup>(
			SelectedPresetId());

		struct CustomFields {
			Ui::InputField *modelField = nullptr;
			Ui::InputField *sysVerField = nullptr;
			Ui::InputField *appVerField = nullptr;
			QWidget *wrap = nullptr;
		};
		auto customFields = std::make_shared<CustomFields>();

		for (const auto &preset : AllPresets()) {
			const auto isCustom = preset.isCustom;
			const auto text = preset.title + u"\n"_q + preset.formattedSummary();
			const auto btn = container->add(
				object_ptr<Ui::Radiobutton>(
					container,
					radioGroup,
					preset.id,
					text,
					st::defaultRadio));
			btn->setCheckAlignment(style::al_topleft);
			Ui::AddSkip(container, 4);
		}

		// Custom fields wrapper
		const auto customWrap = container->add(
			object_ptr<Ui::VerticalLayout>(container));
		customFields->wrap = customWrap;

		Ui::AddSubsectionTitle(customWrap, rpl::single(u"Custom Device Details"_q));

		const auto customModel = customWrap->add(
			object_ptr<Ui::InputField>(
				customWrap,
				st::defaultInputField,
				rpl::single(u"Device Model (e.g. Pixel 11 / Mac Pro)"_q),
				CustomDeviceModel()));
		customFields->modelField = customModel;

		const auto customSysVer = customWrap->add(
			object_ptr<Ui::InputField>(
				customWrap,
				st::defaultInputField,
				rpl::single(u"OS / System Version (e.g. Android 16 / macOS 15.3)"_q),
				CustomSystemVersion()));
		customFields->sysVerField = customSysVer;

		const auto customAppVer = customWrap->add(
			object_ptr<Ui::InputField>(
				customWrap,
				st::defaultInputField,
				rpl::single(u"App Version (e.g. MoltenGram 7.4.4)"_q),
				CustomAppVersion()));
		customFields->appVerField = customAppVer;

		const auto updateCustomVisibility = [=] {
			const auto customSelected = (radioGroup->value() == u"custom"_q);
			customFields->wrap->setVisible(customSelected);
		};
		updateCustomVisibility();

		radioGroup->setChangedCallback([=](const QString &) {
			updateCustomVisibility();
		});

		box->addButton(rpl::single(u"Save & Apply"_q), [=] {
			const auto enabled = enableCheckbox->checked();
			const auto selectedId = radioGroup->value();
			SetEnabled(enabled);
			SetSelectedPresetId(selectedId);
			if (selectedId == u"custom"_q) {
				SetCustomDeviceModel(customFields->modelField->getLastText().trimmed());
				SetCustomSystemVersion(customFields->sysVerField->getLastText().trimmed());
				SetCustomAppVersion(customFields->appVerField->getLastText().trimmed());
			}
			controller->showToast(
				enabled
					? u"Device spoofing applied. Changes active on next connection."_q
					: u"Device spoofing disabled."_q);
			box->closeBox();
		});

		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
}

void SetupSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSubsectionTitle(container, rpl::single(u"Device & Session Spoofing"_q));

	const auto button = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Device Spoofing & Presets"_q),
			st::settingsButtonNoIcon));

	button->setClickedCallback([=] {
		ShowSettingsBox(controller);
	});
}

} // namespace Extera::DeviceSpoof
