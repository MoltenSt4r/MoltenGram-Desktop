/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <optional>
#include <vector>

class HistoryItem;

namespace HistoryView {
class Element;
} // namespace HistoryView

namespace Window {
class SessionController;
} // namespace Window

namespace Ui {
class VerticalLayout;
} // namespace Ui

struct TextWithEntities;

namespace Extera::ChatExtras {

enum class DoubleTapAction : int {
	None = 0,
	Reaction = 1,
	Reply = 2,
	Translate = 3,
	Copy = 4,
	ForwardToSaved = 5,
};

struct RetainedQuote {
	FullMsgId fullId;
	QString authorName;
	QString text;
	TimeId date = 0;

	[[nodiscard]] bool empty() const {
		return text.isEmpty() && authorName.isEmpty();
	}
};

// --- Configuration: Double-Tap Action ---
[[nodiscard]] bool DoubleTapEnabled();
void SetDoubleTapEnabled(bool enabled);

[[nodiscard]] DoubleTapAction IncomingAction();
void SetIncomingAction(DoubleTapAction action);

[[nodiscard]] DoubleTapAction OutgoingAction();
void SetOutgoingAction(DoubleTapAction action);

[[nodiscard]] QString DefaultReactionEmoji();
void SetDefaultReactionEmoji(const QString &emoji);

[[nodiscard]] QString ActionName(DoubleTapAction action);
[[nodiscard]] QString ActionId(DoubleTapAction action);
[[nodiscard]] DoubleTapAction ActionFromId(const QString &id);
[[nodiscard]] DoubleTapAction ActionFromInt(int val);
[[nodiscard]] int ActionToInt(DoubleTapAction action);
[[nodiscard]] const std::vector<DoubleTapAction> &AllActions();

// Double-Tap Action Dispatcher & Execution
bool HandleDoubleTap(
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item,
	HistoryView::Element *view = nullptr);

bool HandleDoubleTapAction(
	DoubleTapAction action,
	not_null<Window::SessionController*> controller,
	not_null<HistoryItem*> item,
	HistoryView::Element *view = nullptr);

// --- Configuration: Deleted Reply Quote Retention ---
[[nodiscard]] bool DeletedReplyQuoteEnabled();
void SetDeletedReplyQuoteEnabled(bool enabled);

[[nodiscard]] bool ReplyToDeletedAsQuoteEnabled();
void SetReplyToDeletedAsQuoteEnabled(bool enabled);

// Retention Store API
void RetainMessage(not_null<const HistoryItem*> item);
void RetainReply(not_null<const HistoryItem*> replyingItem);
void RetainQuote(
	FullMsgId id,
	const QString &authorName,
	const QString &text,
	TimeId date = 0);
void RetainQuote(
	PeerId peerId,
	MsgId msgId,
	const QString &authorName,
	const QString &text,
	TimeId date = 0);

[[nodiscard]] std::optional<RetainedQuote> FindRetainedQuote(FullMsgId id);
[[nodiscard]] std::optional<RetainedQuote> FindRetainedQuote(
	PeerId peerId,
	MsgId msgId);
[[nodiscard]] bool HasRetainedQuote(PeerId peerId, MsgId msgId);

// Display & Formatting Helpers
[[nodiscard]] QString FormatPreservedSnippet(
	const RetainedQuote &quote,
	int maxLen = 120);

[[nodiscard]] QString PreservedQuoteForDisplay(
	PeerId peerId,
	MsgId msgId,
	const QString &fallback = QString());

[[nodiscard]] QString PreservedAuthorForDisplay(
	PeerId peerId,
	MsgId msgId,
	const QString &fallback = QString());

bool ResolveDeletedReplyState(
	PeerId peerId,
	MsgId msgId,
	QString &stateTextOut);

// Quote Rewrite for outgoing replies to deleted messages
[[nodiscard]] bool ShouldRewriteReplyToQuote(
	not_null<const HistoryItem*> targetItem);
[[nodiscard]] TextWithEntities BuildQuotePrefix(
	const RetainedQuote &quote);
[[nodiscard]] TextWithEntities RewriteReplyToQuote(
	const RetainedQuote &quote,
	const TextWithEntities &replyText);

void ClearRetainedQuotes();
[[nodiscard]] size_t RetainedQuotesCount();

// --- Settings UI ---
void ShowSettingsBox(not_null<Window::SessionController*> controller);
void SetupSettings(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);
void AddDoubleTapSettings(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);
void AddDeletedReplyQuoteSettings(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> controller);

} // namespace Extera::ChatExtras
