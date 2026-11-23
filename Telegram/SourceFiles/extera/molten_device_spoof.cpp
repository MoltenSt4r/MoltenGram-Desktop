/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_device_spoof.h"

#include "api/api_authorizations.h"
#include "apiwrap.h"
#include "extera/extera_config.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "mtproto/mtp_instance.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
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
		// 1. Futuristic Flagship: MoltenStar / MoltenGram branding
		{
			.id = u"moltenstar"_q,
			.title = u"MoltenStar Flagship (MoltenGram)"_q,
			.deviceModel = u"MoltenStar MoltenGram"_q,
			.systemVersion = u"MoltenOS 2.0 (SDK 36)"_q,
			.appVersion = u"MoltenGram Desktop 7.4.4"_q,
			.group = u"MoltenStar"_q,
			.platform = u"MoltenStar"_q,
			.isCustom = false,
		},

		// 2. Google Pixel Lineup (from Pixel 11 down to original Pixel, matching MoltenGram Android)
		{
			.id = u"pixel_11_pro_fold"_q,
			.title = u"Google Pixel 11 Pro Fold"_q,
			.deviceModel = u"Google Pixel 11 Pro Fold"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 12.0.0 (6100)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_11_pro_xl"_q,
			.title = u"Google Pixel 11 Pro XL"_q,
			.deviceModel = u"Google Pixel 11 Pro XL"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 12.0.0 (6100)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_11_pro"_q,
			.title = u"Google Pixel 11 Pro"_q,
			.deviceModel = u"Google Pixel 11 Pro"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 12.0.0 (6100)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_11"_q,
			.title = u"Google Pixel 11"_q,
			.deviceModel = u"Google Pixel 11"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 12.0.0 (6100)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_10_pro_fold"_q,
			.title = u"Google Pixel 10 Pro Fold"_q,
			.deviceModel = u"Google Pixel 10 Pro Fold"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 11.9.0 (5900)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_10_pro_xl"_q,
			.title = u"Google Pixel 10 Pro XL"_q,
			.deviceModel = u"Google Pixel 10 Pro XL"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 11.9.0 (5900)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_10_pro"_q,
			.title = u"Google Pixel 10 Pro"_q,
			.deviceModel = u"Google Pixel 10 Pro"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 11.9.0 (5900)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_10"_q,
			.title = u"Google Pixel 10"_q,
			.deviceModel = u"Google Pixel 10"_q,
			.systemVersion = u"Android 16 (SDK 36)"_q,
			.appVersion = u"Telegram Android 11.9.0 (5900)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_9a"_q,
			.title = u"Google Pixel 9a"_q,
			.deviceModel = u"Google Pixel 9a"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.8.0 (5800)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_9_pro_fold"_q,
			.title = u"Google Pixel 9 Pro Fold"_q,
			.deviceModel = u"Google Pixel 9 Pro Fold"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_9_pro_xl"_q,
			.title = u"Google Pixel 9 Pro XL"_q,
			.deviceModel = u"Google Pixel 9 Pro XL"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_9_pro"_q,
			.title = u"Google Pixel 9 Pro"_q,
			.deviceModel = u"Google Pixel 9 Pro"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_9"_q,
			.title = u"Google Pixel 9"_q,
			.deviceModel = u"Google Pixel 9"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_8a"_q,
			.title = u"Google Pixel 8a"_q,
			.deviceModel = u"Google Pixel 8a"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_8_pro"_q,
			.title = u"Google Pixel 8 Pro"_q,
			.deviceModel = u"Google Pixel 8 Pro"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_8"_q,
			.title = u"Google Pixel 8"_q,
			.deviceModel = u"Google Pixel 8"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_fold"_q,
			.title = u"Google Pixel Fold"_q,
			.deviceModel = u"Google Pixel Fold"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_7a"_q,
			.title = u"Google Pixel 7a"_q,
			.deviceModel = u"Google Pixel 7a"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_7_pro"_q,
			.title = u"Google Pixel 7 Pro"_q,
			.deviceModel = u"Google Pixel 7 Pro"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_7"_q,
			.title = u"Google Pixel 7"_q,
			.deviceModel = u"Google Pixel 7"_q,
			.systemVersion = u"Android 15 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_6a"_q,
			.title = u"Google Pixel 6a"_q,
			.deviceModel = u"Google Pixel 6a"_q,
			.systemVersion = u"Android 14 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_6_pro"_q,
			.title = u"Google Pixel 6 Pro"_q,
			.deviceModel = u"Google Pixel 6 Pro"_q,
			.systemVersion = u"Android 14 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_6"_q,
			.title = u"Google Pixel 6"_q,
			.deviceModel = u"Google Pixel 6"_q,
			.systemVersion = u"Android 14 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_5a"_q,
			.title = u"Google Pixel 5a"_q,
			.deviceModel = u"Google Pixel 5a"_q,
			.systemVersion = u"Android 14 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_5"_q,
			.title = u"Google Pixel 5"_q,
			.deviceModel = u"Google Pixel 5"_q,
			.systemVersion = u"Android 14 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_4a_5g"_q,
			.title = u"Google Pixel 4a (5G)"_q,
			.deviceModel = u"Google Pixel 4a (5G)"_q,
			.systemVersion = u"Android 13 (SDK 33)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_4a"_q,
			.title = u"Google Pixel 4a"_q,
			.deviceModel = u"Google Pixel 4a"_q,
			.systemVersion = u"Android 13 (SDK 33)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_4_xl"_q,
			.title = u"Google Pixel 4 XL"_q,
			.deviceModel = u"Google Pixel 4 XL"_q,
			.systemVersion = u"Android 13 (SDK 33)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_4"_q,
			.title = u"Google Pixel 4"_q,
			.deviceModel = u"Google Pixel 4"_q,
			.systemVersion = u"Android 13 (SDK 33)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_3a_xl"_q,
			.title = u"Google Pixel 3a XL"_q,
			.deviceModel = u"Google Pixel 3a XL"_q,
			.systemVersion = u"Android 12L (SDK 32)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_3a"_q,
			.title = u"Google Pixel 3a"_q,
			.deviceModel = u"Google Pixel 3a"_q,
			.systemVersion = u"Android 12L (SDK 32)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_3_xl"_q,
			.title = u"Google Pixel 3 XL"_q,
			.deviceModel = u"Google Pixel 3 XL"_q,
			.systemVersion = u"Android 12 (SDK 31)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_3"_q,
			.title = u"Google Pixel 3"_q,
			.deviceModel = u"Google Pixel 3"_q,
			.systemVersion = u"Android 12 (SDK 31)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_2_xl"_q,
			.title = u"Google Pixel 2 XL"_q,
			.deviceModel = u"Google Pixel 2 XL"_q,
			.systemVersion = u"Android 11 (SDK 30)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_2"_q,
			.title = u"Google Pixel 2"_q,
			.deviceModel = u"Google Pixel 2"_q,
			.systemVersion = u"Android 11 (SDK 30)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_xl"_q,
			.title = u"Google Pixel XL"_q,
			.deviceModel = u"Google Pixel XL"_q,
			.systemVersion = u"Android 10 (SDK 29)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"pixel_1"_q,
			.title = u"Google Pixel"_q,
			.deviceModel = u"Google Pixel"_q,
			.systemVersion = u"Android 10 (SDK 29)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Google Pixel"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},

		// 3. Nothing & CMF Lineup (matching MoltenGram Android: 4b, 4a Pro, 4a, 3, 3a Pro, 3a Plus, 3a, 3a Lite, 2a Plus, 2a, 2, 1, CMF Phone 2 Pro, CMF Phone 1)
		{
			.id = u"nothing_phone_4b"_q,
			.title = u"Nothing Phone (4b)"_q,
			.deviceModel = u"Nothing Phone (4b)"_q,
			.systemVersion = u"Nothing OS 3.5 (SDK 36)"_q,
			.appVersion = u"Telegram Android 12.0.0 (6100)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_4a_pro"_q,
			.title = u"Nothing Phone (4a Pro)"_q,
			.deviceModel = u"Nothing Phone (4a Pro)"_q,
			.systemVersion = u"Nothing OS 3.5 (SDK 36)"_q,
			.appVersion = u"Telegram Android 12.0.0 (6100)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_4a"_q,
			.title = u"Nothing Phone (4a)"_q,
			.deviceModel = u"Nothing Phone (4a)"_q,
			.systemVersion = u"Nothing OS 3.5 (SDK 36)"_q,
			.appVersion = u"Telegram Android 12.0.0 (6100)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_3"_q,
			.title = u"Nothing Phone (3)"_q,
			.deviceModel = u"Nothing Phone (3)"_q,
			.systemVersion = u"Nothing OS 3.0 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_3a_pro"_q,
			.title = u"Nothing Phone (3a Pro)"_q,
			.deviceModel = u"Nothing Phone (3a Pro)"_q,
			.systemVersion = u"Nothing OS 3.0 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_3a_plus"_q,
			.title = u"Nothing Phone (3a) Plus"_q,
			.deviceModel = u"Nothing Phone (3a) Plus"_q,
			.systemVersion = u"Nothing OS 3.0 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_3a"_q,
			.title = u"Nothing Phone (3a)"_q,
			.deviceModel = u"Nothing Phone (3a)"_q,
			.systemVersion = u"Nothing OS 3.0 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_3a_lite"_q,
			.title = u"Nothing Phone (3a) Lite"_q,
			.deviceModel = u"Nothing Phone (3a) Lite"_q,
			.systemVersion = u"Nothing OS 3.0 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_2a_plus"_q,
			.title = u"Nothing Phone (2a) Plus"_q,
			.deviceModel = u"Nothing Phone (2a) Plus"_q,
			.systemVersion = u"Nothing OS 2.6 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_2a"_q,
			.title = u"Nothing Phone (2a)"_q,
			.deviceModel = u"Nothing Phone (2a)"_q,
			.systemVersion = u"Nothing OS 2.6 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_2"_q,
			.title = u"Nothing Phone (2)"_q,
			.deviceModel = u"Nothing Phone (2)"_q,
			.systemVersion = u"Nothing OS 2.6 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_phone_1"_q,
			.title = u"Nothing Phone (1)"_q,
			.deviceModel = u"Nothing Phone (1)"_q,
			.systemVersion = u"Nothing OS 2.6 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_cmf_phone_2_pro"_q,
			.title = u"Nothing CMF Phone 2 Pro"_q,
			.deviceModel = u"CMF Phone 2 Pro"_q,
			.systemVersion = u"Nothing OS 3.0 (SDK 35)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing / CMF"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"nothing_cmf_phone_1"_q,
			.title = u"Nothing CMF Phone 1"_q,
			.deviceModel = u"CMF Phone 1"_q,
			.systemVersion = u"Nothing OS 2.6 (SDK 34)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Nothing / CMF"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},

		// 4. Apple iOS / iPadOS
		{
			.id = u"iphone_16_pro_max"_q,
			.title = u"Apple iPhone 16 Pro Max"_q,
			.deviceModel = u"iPhone 16 Pro Max"_q,
			.systemVersion = u"iOS 18.3"_q,
			.appVersion = u"Telegram iOS 11.7.0"_q,
			.group = u"Apple iOS"_q,
			.platform = u"iOS"_q,
			.isCustom = false,
		},
		{
			.id = u"iphone_16_pro"_q,
			.title = u"Apple iPhone 16 Pro"_q,
			.deviceModel = u"iPhone 16 Pro"_q,
			.systemVersion = u"iOS 18.3"_q,
			.appVersion = u"Telegram iOS 11.7.0"_q,
			.group = u"Apple iOS"_q,
			.platform = u"iOS"_q,
			.isCustom = false,
		},
		{
			.id = u"iphone_16"_q,
			.title = u"Apple iPhone 16"_q,
			.deviceModel = u"iPhone 16"_q,
			.systemVersion = u"iOS 18.3"_q,
			.appVersion = u"Telegram iOS 11.7.0"_q,
			.group = u"Apple iOS"_q,
			.platform = u"iOS"_q,
			.isCustom = false,
		},
		{
			.id = u"iphone_15_pro"_q,
			.title = u"Apple iPhone 15 Pro"_q,
			.deviceModel = u"iPhone 15 Pro"_q,
			.systemVersion = u"iOS 18.2"_q,
			.appVersion = u"Telegram iOS 11.7.0"_q,
			.group = u"Apple iOS"_q,
			.platform = u"iOS"_q,
			.isCustom = false,
		},
		{
			.id = u"iphone_14_pro"_q,
			.title = u"Apple iPhone 14 Pro"_q,
			.deviceModel = u"iPhone 14 Pro"_q,
			.systemVersion = u"iOS 17.5"_q,
			.appVersion = u"Telegram iOS 11.6.0"_q,
			.group = u"Apple iOS"_q,
			.platform = u"iOS"_q,
			.isCustom = false,
		},
		{
			.id = u"ipad_pro_13"_q,
			.title = u"Apple iPad Pro 13\" (M4)"_q,
			.deviceModel = u"iPad Pro 13\""_q,
			.systemVersion = u"iPadOS 18.3"_q,
			.appVersion = u"Telegram iOS 11.7.0"_q,
			.group = u"Apple iOS"_q,
			.platform = u"iOS"_q,
			.isCustom = false,
		},

		// 5. Samsung Galaxy
		{
			.id = u"samsung_s25_ultra"_q,
			.title = u"Samsung Galaxy S25 Ultra"_q,
			.deviceModel = u"Samsung Galaxy S25 Ultra"_q,
			.systemVersion = u"Android 15 (One UI 7.0)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Samsung Galaxy"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"samsung_s25"_q,
			.title = u"Samsung Galaxy S25"_q,
			.deviceModel = u"Samsung Galaxy S25"_q,
			.systemVersion = u"Android 15 (One UI 7.0)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Samsung Galaxy"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"samsung_s24_ultra"_q,
			.title = u"Samsung Galaxy S24 Ultra"_q,
			.deviceModel = u"Samsung Galaxy S24 Ultra"_q,
			.systemVersion = u"Android 15 (One UI 7.0)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Samsung Galaxy"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},
		{
			.id = u"samsung_z_fold_6"_q,
			.title = u"Samsung Galaxy Z Fold6"_q,
			.deviceModel = u"Samsung Galaxy Z Fold6"_q,
			.systemVersion = u"Android 14 (One UI 6.1.1)"_q,
			.appVersion = u"Telegram Android 11.7.1 (5678)"_q,
			.group = u"Samsung Galaxy"_q,
			.platform = u"Android"_q,
			.isCustom = false,
		},

		// 6. macOS
		{
			.id = u"macbook_pro_16"_q,
			.title = u"Apple MacBook Pro 16\" (macOS)"_q,
			.deviceModel = u"MacBook Pro 16\""_q,
			.systemVersion = u"macOS Sequoia 15.3"_q,
			.appVersion = u"Telegram macOS 11.7.1"_q,
			.group = u"macOS"_q,
			.platform = u"macOS"_q,
			.isCustom = false,
		},
		{
			.id = u"telegram_macos"_q,
			.title = u"Apple MacBook Pro 16\" (macOS) [Legacy]"_q,
			.deviceModel = u"MacBook Pro 16\""_q,
			.systemVersion = u"macOS Sequoia 15.3"_q,
			.appVersion = u"Telegram macOS 11.7.1"_q,
			.group = u"macOS"_q,
			.platform = u"macOS"_q,
			.isCustom = false,
		},
		{
			.id = u"macbook_air_m3"_q,
			.title = u"Apple MacBook Air M3 (macOS)"_q,
			.deviceModel = u"MacBook Air M3"_q,
			.systemVersion = u"macOS Sequoia 15.3"_q,
			.appVersion = u"Telegram macOS 11.7.1"_q,
			.group = u"macOS"_q,
			.platform = u"macOS"_q,
			.isCustom = false,
		},
		{
			.id = u"mac_studio_m2"_q,
			.title = u"Apple Mac Studio M2 (macOS)"_q,
			.deviceModel = u"Mac Studio M2"_q,
			.systemVersion = u"macOS Sonoma 14.6"_q,
			.appVersion = u"Telegram macOS 11.7.1"_q,
			.group = u"macOS"_q,
			.platform = u"macOS"_q,
			.isCustom = false,
		},

		// 7. Desktop (PC / Linux)
		{
			.id = u"tdesktop_official"_q,
			.title = u"Telegram Desktop (Windows 11 PC)"_q,
			.deviceModel = u"PC 64bit"_q,
			.systemVersion = u"Windows 11"_q,
			.appVersion = u"Telegram Desktop 5.12.1"_q,
			.group = u"Desktop PC"_q,
			.platform = u"Windows"_q,
			.isCustom = false,
		},
		{
			.id = u"tdesktop_win10"_q,
			.title = u"Telegram Desktop (Windows 10 PC)"_q,
			.deviceModel = u"PC 64bit"_q,
			.systemVersion = u"Windows 10"_q,
			.appVersion = u"Telegram Desktop 5.12.1"_q,
			.group = u"Desktop PC"_q,
			.platform = u"Windows"_q,
			.isCustom = false,
		},
		{
			.id = u"tdesktop_linux"_q,
			.title = u"Telegram Desktop (Ubuntu Linux PC)"_q,
			.deviceModel = u"PC 64bit"_q,
			.systemVersion = u"Ubuntu 24.04 LTS"_q,
			.appVersion = u"Telegram Desktop 5.12.1"_q,
			.group = u"Desktop PC"_q,
			.platform = u"Linux"_q,
			.isCustom = false,
		},

		// 8. Custom
		{
			.id = u"custom"_q,
			.title = u"Custom Device Name & App Version"_q,
			.deviceModel = u"Custom Device"_q,
			.systemVersion = u"Custom OS 1.0"_q,
			.appVersion = u"MoltenGram 7.4.4"_q,
			.group = u"Custom"_q,
			.platform = u"Custom"_q,
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
	if (id == u"macbook_pro_16"_q) {
		return FindPreset(u"telegram_macos"_q);
	} else if (id == u"telegram_macos"_q) {
		return FindPreset(u"macbook_pro_16"_q);
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

QString ApplyPlatform(const QString &original) {
	if (!Enabled()) {
		return original;
	}
	const auto preset = ActivePreset();
	return preset.platform.isEmpty() ? original : preset.platform;
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
		box->setWidth(st::boxWideWidth);
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

		const auto container = box->verticalLayout();

		Ui::AddSubsectionTitle(
			container,
			rpl::single(u"MTProto Device Information"_q));

		Ui::AddSkip(container, 8);

		const auto aboutLabel = container->add(
			object_ptr<Ui::FlatLabel>(
				container,
				rpl::single(u"Spoofs your outgoing device model, OS version, and client version in MTProto initConnection. This changes how active sessions appear on other devices and on Telegram servers."_q),
				st::boxLabel));

		Ui::AddSkip(container, 16);

		const auto enableCheckbox = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				u"Enable Device Spoofing"_q,
				Enabled(),
				st::defaultCheckbox));

		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Device Presets"_q));
		Ui::AddSkip(container, 8);

		const auto &presets = AllPresets();
		const auto currentId = SelectedPresetId();
		int initialIndex = 0;
		for (int i = 0; i < int(presets.size()); ++i) {
			if (presets[i].id == currentId) {
				initialIndex = i;
				break;
			}
		}

		const auto radioGroup = std::make_shared<Ui::RadiobuttonGroup>(
			initialIndex);

		struct CustomFields {
			Ui::InputField *modelField = nullptr;
			Ui::InputField *sysVerField = nullptr;
			Ui::InputField *appVerField = nullptr;
			QWidget *wrap = nullptr;
		};
		auto customFields = std::make_shared<CustomFields>();

		QString lastGroup;
		for (int i = 0; i < int(presets.size()); ++i) {
			const auto &preset = presets[i];
			if (!preset.group.isEmpty() && preset.group != lastGroup) {
				lastGroup = preset.group;
				Ui::AddSkip(container, 16);
				Ui::AddSubsectionTitle(container, rpl::single(preset.group));
				Ui::AddSkip(container, 8);
			}
			const auto text = preset.title + u"\n"_q + preset.formattedSummary();
			const auto btn = container->add(
				object_ptr<Ui::Radiobutton>(
					container,
					radioGroup,
					i,
					text));
			btn->setCheckAlignment(style::al_topleft);
			Ui::AddSkip(container, 6);
		}

		// Custom fields wrapper
		const auto customWrap = container->add(
			object_ptr<Ui::VerticalLayout>(container));
		customFields->wrap = customWrap;

		Ui::AddSkip(customWrap, 16);
		Ui::AddSubsectionTitle(customWrap, rpl::single(u"Custom Device Details"_q));
		Ui::AddSkip(customWrap, 8);

		const auto customModel = customWrap->add(
			object_ptr<Ui::InputField>(
				customWrap,
				st::defaultInputField,
				rpl::single(u"Device Model (e.g. Pixel 11 / Mac Pro)"_q),
				CustomDeviceModel()));
		customFields->modelField = customModel;

		Ui::AddSkip(customWrap, 8);
		const auto customSysVer = customWrap->add(
			object_ptr<Ui::InputField>(
				customWrap,
				st::defaultInputField,
				rpl::single(u"OS / System Version (e.g. Android 16 / macOS 15.3)"_q),
				CustomSystemVersion()));
		customFields->sysVerField = customSysVer;

		Ui::AddSkip(customWrap, 8);
		const auto customAppVer = customWrap->add(
			object_ptr<Ui::InputField>(
				customWrap,
				st::defaultInputField,
				rpl::single(u"App Version (e.g. MoltenGram 7.4.4)"_q),
				CustomAppVersion()));
		customFields->appVerField = customAppVer;

		const auto selectedPresetIdFor = [=](int idx) -> QString {
			const auto &list = AllPresets();
			if (idx >= 0 && idx < int(list.size())) {
				return list[idx].id;
			}
			return kDefaultPresetId;
		};

		const auto updateCustomVisibility = [=] {
			const auto customSelected = (selectedPresetIdFor(radioGroup->current()) == u"custom"_q);
			customFields->wrap->setVisible(customSelected);
		};
		updateCustomVisibility();

		radioGroup->setChangedCallback([=](int) {
			updateCustomVisibility();
		});

		box->addButton(rpl::single(u"Save & Apply"_q), [=] {
			const auto enabled = enableCheckbox->checked();
			const auto selectedId = selectedPresetIdFor(radioGroup->current());
			SetEnabled(enabled);
			SetSelectedPresetId(selectedId);
			if (selectedId == u"custom"_q) {
				SetCustomDeviceModel(customFields->modelField->getLastText().trimmed());
				SetCustomSystemVersion(customFields->sysVerField->getLastText().trimmed());
				SetCustomAppVersion(customFields->appVerField->getLastText().trimmed());
			}
			controller->session().mtp().reInitConnection(controller->session().mtp().mainDcId());
			controller->session().api().authorizations().reload();
			controller->showToast(
				enabled
					? u"Device spoofing applied. Active connection reinitialized."_q
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
