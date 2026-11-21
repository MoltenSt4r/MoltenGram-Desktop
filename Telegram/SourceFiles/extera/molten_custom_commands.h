#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <vector>

namespace Window {
class SessionController;
} // namespace Window

namespace Extera::MoltenCommands {

struct CustomCommand {
	QString prefix;
	QString name;
	QString description;

	[[nodiscard]] QString fullCommand() const {
		return (prefix.isEmpty() ? u"."_q : prefix) + name;
	}
};

struct ParsedQuery {
	QString query;
	bool fromStart = false;
};

[[nodiscard]] bool Enabled();
[[nodiscard]] QStringList Prefixes();
[[nodiscard]] bool IsPrefix(const QString &ch);
[[nodiscard]] std::vector<CustomCommand> List();
void Add(const CustomCommand &command);
void Update(int index, const CustomCommand &command);
void Remove(int index);
[[nodiscard]] std::vector<CustomCommand> Match(
	const QString &prefix,
	const QString &filter);
[[nodiscard]] ParsedQuery ParseQuery(const QString &text, int cursorPosition);
void ShowSettingsBox(not_null<Window::SessionController*> controller);

} // namespace Extera::MoltenCommands
