#include "extera/molten_custom_commands.h"

#include "extera/extera_config.h"
#include "lang/lang_keys.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>

namespace Extera::MoltenCommands {
namespace {

struct Store {
	bool loaded = false;
	std::vector<CustomCommand> commands;
};

[[nodiscard]] Store &State() {
	static auto instance = Store();
	return instance;
}

[[nodiscard]] QString StoragePath() {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		+ u"/molten_custom_commands.json"_q;
}

void EnsureLoaded() {
	auto &state = State();
	if (state.loaded) {
		return;
	}
	state.loaded = true;
	state.commands.clear();

	auto file = QFile(StoragePath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	const auto doc = QJsonDocument::fromJson(file.readAll());
	if (!doc.isArray()) {
		return;
	}
	for (const auto &entry : doc.array()) {
		if (!entry.isObject()) {
			continue;
		}
		const auto obj = entry.toObject();
		auto prefix = obj.value(u"prefix"_q).toString().trimmed();
		const auto name = obj.value(u"name"_q).toString().trimmed();
		const auto desc = obj.value(u"description"_q).toString().trimmed();
		if (name.isEmpty()) {
			continue;
		}
		if (prefix.isEmpty()) {
			prefix = u"."_q;
		}
		state.commands.push_back({
			.prefix = prefix.left(1),
			.name = name,
			.description = desc,
		});
	}
}

void Save() {
	const auto &state = State();
	auto arr = QJsonArray();
	for (const auto &cmd : state.commands) {
		auto obj = QJsonObject();
		obj.insert(u"prefix"_q, cmd.prefix.isEmpty() ? u"."_q : cmd.prefix.left(1));
		obj.insert(u"name"_q, cmd.name);
		obj.insert(u"description"_q, cmd.description);
		arr.append(obj);
	}
	const auto path = StoragePath();
	QDir().mkpath(QFileInfo(path).absolutePath());
	auto file = QFile(path);
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
	}
}

void ShowEditCommandBox(
		not_null<Window::SessionController*> controller,
		int editIndex,
		Fn<void()> onSaved) {
	EnsureLoaded();
	const auto existing = (editIndex >= 0
		&& editIndex < int(State().commands.size()))
		? std::make_optional(State().commands[editIndex])
		: std::nullopt;

	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(existing
			? u"Edit custom command"_q
			: u"Add custom command"_q));

		const auto prefixField = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"Prefix (., /, !, $, ?)"_q),
				existing ? existing->prefix : u"."_q));
		prefixField->setMaxLength(1);

		const auto nameField = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"Command name (e.g. id, tagall)"_q),
				existing ? existing->name : QString()));

		const auto descField = box->addRow(
			object_ptr<Ui::InputField>(
				box,
				st::defaultInputField,
				rpl::single(u"Description (optional)"_q),
				existing ? existing->description : QString()));

		const auto submit = [=] {
			auto prefix = prefixField->getLastText().trimmed();
			auto name = nameField->getLastText().trimmed();
			const auto desc = descField->getLastText().trimmed();
			if (!name.isEmpty() && IsPrefix(name.left(1))) {
				if (prefix.isEmpty()) {
					prefix = name.left(1);
				}
				name.remove(0, 1);
				name = name.trimmed();
			}
			if (prefix.isEmpty()) {
				prefix = u"."_q;
			}
			if (name.isEmpty()) {
				nameField->showError();
				return;
			}
			const auto cmd = CustomCommand{
				.prefix = prefix.left(1),
				.name = name,
				.description = desc,
			};
			if (existing) {
				Update(editIndex, cmd);
			} else {
				Add(cmd);
			}
			box->closeBox();
			if (onSaved) {
				onSaved();
			}
		};

		box->addButton(tr::lng_settings_save(), submit);
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
		box->setFocusCallback([=] { nameField->setFocusFast(); });
	}));
}

} // namespace

bool Enabled() {
	return Extera::Bool(u"custom_commands_enabled"_q);
}

QStringList Prefixes() {
	auto raw = Extera::String(u"custom_command_prefixes"_q).trimmed();
	if (raw.isEmpty()) {
		raw = u".,!,$,?"_q;
	}
	auto result = QStringList();
	for (const auto &part : raw.split(',', Qt::SkipEmptyParts)) {
		const auto trimmed = part.trimmed();
		if (!trimmed.isEmpty()) {
			const auto ch = trimmed.left(1);
			if (!result.contains(ch)) {
				result.push_back(ch);
			}
		}
	}
	if (result.isEmpty()) {
		result = { u"."_q, u"!"_q, u"$"_q, u"?"_q };
	}
	return result;
}

bool IsPrefix(const QString &ch) {
	if (ch.size() != 1) {
		return false;
	}
	return Prefixes().contains(ch);
}

std::vector<CustomCommand> List() {
	EnsureLoaded();
	return State().commands;
}

void Add(const CustomCommand &command) {
	EnsureLoaded();
	if (command.name.trimmed().isEmpty()) {
		return;
	}
	State().commands.push_back({
		.prefix = command.prefix.isEmpty() ? u"."_q : command.prefix.left(1),
		.name = command.name.trimmed(),
		.description = command.description.trimmed(),
	});
	Save();
}

void Update(int index, const CustomCommand &command) {
	EnsureLoaded();
	if (index < 0 || index >= int(State().commands.size())) {
		return;
	}
	if (command.name.trimmed().isEmpty()) {
		return;
	}
	State().commands[index] = {
		.prefix = command.prefix.isEmpty() ? u"."_q : command.prefix.left(1),
		.name = command.name.trimmed(),
		.description = command.description.trimmed(),
	};
	Save();
}

void Remove(int index) {
	EnsureLoaded();
	if (index < 0 || index >= int(State().commands.size())) {
		return;
	}
	State().commands.erase(State().commands.begin() + index);
	Save();
}

std::vector<CustomCommand> Match(
		const QString &prefix,
		const QString &filter) {
	EnsureLoaded();
	auto result = std::vector<CustomCommand>();
	const auto needle = filter.trimmed();
	for (const auto &cmd : State().commands) {
		const auto cmdPrefix = cmd.prefix.isEmpty() ? u"."_q : cmd.prefix;
		if (cmdPrefix != prefix) {
			continue;
		}
		if (needle.isEmpty()
			|| cmd.name.startsWith(needle, Qt::CaseInsensitive)
			|| cmd.description.contains(needle, Qt::CaseInsensitive)) {
			result.push_back(cmd);
		}
	}
	return result;
}

ParsedQuery ParseQuery(const QString &text, int cursorPosition) {
	if (!Enabled() || text.isEmpty() || cursorPosition <= 0) {
		return {};
	}
	const auto pos = std::min(cursorPosition, int(text.size()));
	auto start = pos;
	while (start > 0 && !text[start - 1].isSpace()) {
		--start;
	}
	if (start >= pos) {
		return {};
	}
	const auto token = text.mid(start, pos - start);
	if (token.isEmpty() || !IsPrefix(token.left(1))) {
		return {};
	}
	return {
		.query = token,
		.fromStart = (start == 0),
	};
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	EnsureLoaded();
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Custom Autocomplete Commands"_q));
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

		const auto listWrap = box->verticalLayout()->add(
			object_ptr<Ui::VerticalLayout>(box));

		const auto rebuild = std::make_shared<Fn<void()>>();
		*rebuild = [=] {
			while (listWrap->count() > 0) {
				delete listWrap->widgetAt(0);
			}
			const auto commands = List();
			if (commands.empty()) {
				listWrap->add(
					object_ptr<Ui::FlatLabel>(
						listWrap,
						rpl::single(u"No custom commands yet. Add commands like .id or .tagall for instant autocomplete."_q),
						st::boxDividerLabel),
					st::boxRowPadding);
			} else {
				for (auto i = 0; i != int(commands.size()); ++i) {
					const auto &cmd = commands[i];
					const auto label = cmd.description.isEmpty()
						? cmd.fullCommand()
						: (cmd.fullCommand() + u" — "_q + cmd.description);
					const auto btn = listWrap->add(
						object_ptr<Ui::SettingsButton>(
							listWrap,
							rpl::single(label),
							st::settingsButtonNoIcon));
					btn->setClickedCallback([=] {
						controller->show(Box([=](not_null<Ui::GenericBox*> actionBox) {
							actionBox->setTitle(rpl::single(cmd.fullCommand()));
							actionBox->addButton(rpl::single(u"Edit"_q), [=] {
								actionBox->closeBox();
								ShowEditCommandBox(controller, i, *rebuild);
							});
							actionBox->addButton(tr::lng_box_delete(), [=] {
								actionBox->closeBox();
								Remove(i);
								(*rebuild)();
							}, st::attentionBoxButton);
							actionBox->addLeftButton(tr::lng_cancel(), [=] {
								actionBox->closeBox();
							});
						}));
					});
				}
			}
			listWrap->resizeToWidth(box->width());
		};
		(*rebuild)();

		box->addButton(rpl::single(u"Add command"_q), [=] {
			ShowEditCommandBox(controller, -1, *rebuild);
		});
		box->addButton(tr::lng_close(), [=] { box->closeBox(); });
	}));
}

} // namespace Extera::MoltenCommands
