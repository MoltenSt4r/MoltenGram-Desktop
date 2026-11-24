#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>

class History;
class HistoryItem;
struct TextWithEntities;

namespace Extera {

void ConvertPremiumEmojiForSending(
	TextWithEntities &text,
	not_null<History*> history);
void ApplyLocalPremiumEmoji(
	not_null<const HistoryItem*> item,
	TextWithEntities &text);
void ForgetLocalPremiumEmoji(not_null<const HistoryItem*> item);
void ReapplyLocalPremiumEmoji(not_null<HistoryItem*> item);

} // namespace Extera

namespace Extera::NotoEmoji {

[[nodiscard]] bool Enabled();
[[nodiscard]] QString EmojiToCodePoints(const QString &emoji);
[[nodiscard]] QString WebpUrl(const QString &emoji);
[[nodiscard]] QString LottieUrl(const QString &emoji);

} // namespace Extera::NotoEmoji
