#pragma once

#include <QtGui/QColor>

#include <memory>
#include <optional>

namespace Ui {
class Show;
} // namespace Ui

struct TextWithEntities;

namespace Extera::UrlCleaner {

[[nodiscard]] bool Enabled();
[[nodiscard]] QString Clean(const QString &url);
[[nodiscard]] QString CleanIfEnabled(const QString &url);

} // namespace Extera::UrlCleaner

namespace Extera::ColoredText {

[[nodiscard]] bool Enabled();
[[nodiscard]] bool IsColorUrl(const QString &url);
[[nodiscard]] std::optional<QColor> ParseColor(const QString &url);
[[nodiscard]] int ParseLength(const QString &url);
[[nodiscard]] QString BuildColorUrl(const QColor &color, int length = 0);
[[nodiscard]] TextWithEntities WrapColored(
	const QString &text,
	const QColor &color);
void HandleColorUrlClick(
	const QString &url,
	std::shared_ptr<Ui::Show> show);

} // namespace Extera::ColoredText
