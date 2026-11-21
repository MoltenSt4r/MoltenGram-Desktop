/*
This file is part of MoltenGram Desktop,
based on Telegram Desktop and exteraGram.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "extera/molten_chat_extras.h"

#include "apiwrap.h"
#include "api/api_common.h"
#include "boxes/translate_box.h"
#include "data/data_message_reaction_id.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_types.h"
#include "data/data_user.h"
#include "extera/extera_config.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/history_item_components.h"
#include "history/history_item_helpers.h"
#include "history/view/history_view_element.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "styles/style_boxes.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/effects/reaction_fly_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_entity.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <algorithm>
#include <map>

namespace Extera::ChatExtras {
namespace {

constexpr auto kDoubleTapEnabledKey = "molten_doubletap_enabled";
constexpr auto kDoubleTapIncomingKey = "molten_doubletap_action";
constexpr auto kDoubleTapOutgoingKey = "molten_doubletap_action_out";
constexpr auto kDoubleTapReactionKey = "molten_doubletap_reaction";
constexpr auto kDeletedReplyQuoteKey = "molten_deleted_reply_quote";
constexpr auto kReplyToDeletedAsQuoteKey = "molten_reply_to_deleted_as_quote";

constexpr auto kMaxRetainedQuotesMemory = 3000;

class QuoteStore final {
public:
	static QuoteStore &Instance() {
		static QuoteStore store;
		return store;
	}

	void retain(FullMsgId id, const QString &authorName, const QString &text, TimeId date) {
		if (!id.msg || (authorName.trimmed().isEmpty() && text.trimmed().isEmpty())) {
			return;
		}
		ensureLoaded();
		auto quote = RetainedQuote{
			.fullId = id,
			.authorName = authorName.trimmed(),
			.text = text.trimmed(),
			.date = date,
		};
		_quotes[id] = std::move(quote);
		if (_quotes.size() > kMaxRetainedQuotesMemory) {
			_quotes.erase(_quotes.begin());
		}
		_dirty = true;
		_saveTimer.callOnce(1000);
	}

	[[nodiscard]] std::optional<RetainedQuote> find(FullMsgId id) {
		ensureLoaded();
		const auto it = _quotes.find(id);
		if (it != _quotes.end()) {
			return it->second;
		}
		return std::nullopt;
	}

	[[nodiscard]] std::optional<RetainedQuote> find(PeerId peerId, MsgId msgId) {
		return find(FullMsgId(peerId, msgId));
	}

	void clear() {
		_quotes.clear();
		_dirty = true;
		saveNow();
	}

	[[nodiscard]] size_t count() {
		ensureLoaded();
		return _quotes.size();
	}

	void saveNow() {
		if (!_dirty) {
			return;
		}
		_dirty = false;
		_saveTimer.cancel();
		saveToFile();
	}

private:
	QuoteStore() {
		_saveTimer.setCallback([this] { saveToFile(); });
	}

	void ensureLoaded() {
		if (_loaded) {
			return;
		}
		_loaded = true;
		loadFromFile();
	}

	void loadFromFile() {
		QFile file(storagePath());
		if (!file.open(QIODevice::ReadOnly)) {
			return;
		}
		const auto doc = QJsonDocument::fromJson(file.readAll());
		if (!doc.isArray()) {
			return;
		}
		for (const auto &item : doc.array()) {
			if (!item.isObject()) {
				continue;
			}
			const auto obj = item.toObject();
			const auto peerId = PeerId(obj.value(u"peerId"_q).toString().toULongLong());
			const auto msgId = MsgId(obj.value(u"msgId"_q).toInt());
			const auto author = obj.value(u"author"_q).toString();
			const auto text = obj.value(u"text"_q).toString();
			const auto date = TimeId(obj.value(u"date"_q).toInt());
			if (msgId) {
				const auto id = FullMsgId(peerId, msgId);
				_quotes[id] = RetainedQuote{
					.fullId = id,
					.authorName = author,
					.text = text,
					.date = date,
				};
			}
		}
	}

	void saveToFile() {
		_dirty = false;
		QJsonArray arr;
		for (const auto &[id, quote] : _quotes) {
			QJsonObject obj;
			obj[u"peerId"_q] = QString::number(id.peer.value);
			obj[u"msgId"_q] = static_cast<qint64>(id.msg.bare);
			obj[u"author"_q] = quote.authorName;
			obj[u"text"_q] = quote.text;
			obj[u"date"_q] = static_cast<qint64>(quote.date);
			arr.append(obj);
		}
		QDir().mkpath(QFileInfo(storagePath()).absolutePath());
		QFile file(storagePath());
		if (file.open(QIODevice::WriteOnly)) {
			file.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
		}
	}

	[[nodiscard]] QString storagePath() const {
		return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
			+ u"/molten_retained_quotes.json"_q;
	}

	std::map<FullMsgId, RetainedQuote> _quotes;
	base::Timer _saveTimer;
	bool _loaded = false;
	bool _dirty = false;
};

} // namespace

bool DoubleTapEnabled() {
	const auto val = Config::Instance().valueOf(kDoubleTapEnabledKey);
	return val.isUndefined() ? true : val.toBool();
}

void SetDoubleTapEnabled(bool enabled) {
	Config::Instance().setBool(kDoubleTapEnabledKey, enabled);
}

DoubleTapAction IncomingAction() {
	const auto val = Config::Instance().valueOf(kDoubleTapIncomingKey);
	if (val.isUndefined()) {
		return DoubleTapAction::Reaction;
	}
	if (val.isString()) {
		return ActionFromId(val.toString());
	}
	return ActionFromInt(val.toInt());
}

void SetIncomingAction(DoubleTapAction action) {
	Config::Instance().setValue(kDoubleTapIncomingKey, ActionId(action));
}

DoubleTapAction OutgoingAction() {
	const auto val = Config::Instance().valueOf(kDoubleTapOutgoingKey);
	if (val.isUndefined()) {
		return DoubleTapAction::Reply;
	}
	if (val.isString()) {
		return ActionFromId(val.toString());
	}
	return ActionFromInt(val.toInt());
}

void SetOutgoingAction(DoubleTapAction action) {
	Config::Instance().setValue(kDoubleTapOutgoingKey, ActionId(action));
}

QString DefaultReactionEmoji() {
	const auto str = Config::Instance().stringValue(kDoubleTapReactionKey);
	return str.trimmed().isEmpty() ? u"👍"_q : str.trimmed();
}

void SetDefaultReactionEmoji(const QString &emoji) {
	Config::Instance().setString(kDoubleTapReactionKey, emoji.trimmed());
}

QString ActionName(DoubleTapAction action) {
	switch (action) {
	case DoubleTapAction::None: return u"None"_q;
	case DoubleTapAction::Reaction: return u"Quick Reaction"_q;
	case DoubleTapAction::Reply: return u"Reply"_q;
	case DoubleTapAction::Translate: return u"Translate"_q;
	case DoubleTapAction::Copy: return u"Copy Text"_q;
	case DoubleTapAction::ForwardToSaved: return u"Forward to Saved Messages"_q;
	}
	return u"None"_q;
}

QString ActionId(DoubleTapAction action) {
	switch (action) {
	case DoubleTapAction::None: return u"none"_q;
	case DoubleTapAction::Reaction: return u"reaction"_q;
	case DoubleTapAction::Reply: return u"reply"_q;
	case DoubleTapAction::Translate: return u"translate"_q;
	case DoubleTapAction::Copy: return u"copy"_q;
	case DoubleTapAction::ForwardToSaved: return u"forward"_q;
	}
	return u"none"_q;
}

DoubleTapAction ActionFromId(const QString &id) {
	if (id == u"reaction"_q) return DoubleTapAction::Reaction;
	if (id == u"reply"_q) return DoubleTapAction::Reply;
	if (id == u"translate"_q) return DoubleTapAction::Translate;
	if (id == u"copy"_q) return DoubleTapAction::Copy;
	if (id == u"forward"_q || id == u"save"_q) return DoubleTapAction::ForwardToSaved;
	return DoubleTapAction::None;
}

DoubleTapAction ActionFromInt(int val) {
	switch (val) {
	case 1: return DoubleTapAction::Reaction;
	case 2: return DoubleTapAction::Reply;
	case 3: return DoubleTapAction::Translate;
	case 4: return DoubleTapAction::Copy;
	case 5: return DoubleTapAction::ForwardToSaved;
	default: return DoubleTapAction::None;
	}
}

int ActionToInt(DoubleTapAction action) {
	return static_cast<int>(action);
}

const std::vector<DoubleTapAction> &AllActions() {
	static const auto list = std::vector<DoubleTapAction>{
		DoubleTapAction::Reaction,
		DoubleTapAction::Reply,
		DoubleTapAction::Translate,
		DoubleTapAction::Copy,
		DoubleTapAction::ForwardToSaved,
		DoubleTapAction::None,
	};
	return list;
}

bool HandleDoubleTap(
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item,
		HistoryView::Element *view) {
	if (!DoubleTapEnabled()) {
		return false;
	}
	const auto action = item->out() ? OutgoingAction() : IncomingAction();
	return HandleDoubleTapAction(action, controller, item, view);
}

bool HandleDoubleTapAction(
		DoubleTapAction action,
		not_null<Window::SessionController*> controller,
		not_null<HistoryItem*> item,
		HistoryView::Element *view) {
	if (!view) {
		view = item->mainView();
	}
	switch (action) {
	case DoubleTapAction::None:
		return false;

	case DoubleTapAction::Reaction: {
		if (!item->canReact()) {
			return false;
		}
		const auto emoji = DefaultReactionEmoji();
		const auto reactionId = emoji.startsWith(u"animated_"_q)
			? Data::ReactionId{ emoji.mid(9).toULongLong() }
			: Data::ReactionId{ emoji.isEmpty() ? u"👍"_q : emoji };
		if (view) {
			view->animateReaction(Ui::ReactionFlyAnimationArgs{ .id = reactionId });
		}
		item->toggleReaction(reactionId, HistoryReactionSource::Quick);
		return true;
	}

	case DoubleTapAction::Reply: {
		if (view && view->delegate()) {
			view->delegate()->elementReplyTo(FullReplyTo{ .messageId = item->fullId() });
			return true;
		}
		return false;
	}

	case DoubleTapAction::Translate: {
		const auto original = item->originalText();
		if (original.text.trimmed().isEmpty()) {
			controller->showToast(u"No text to translate."_q);
			return false;
		}
		controller->show(Box(
			Ui::TranslateBox,
			item->history()->peer,
			item->fullId().msg,
			original,
			item->forbidsForward()));
		return true;
	}

	case DoubleTapAction::Copy: {
		const auto text = item->originalText().text;
		if (text.trimmed().isEmpty()) {
			controller->showToast(u"No text to copy."_q);
			return false;
		}
		QGuiApplication::clipboard()->setText(text);
		controller->showToast(u"Message text copied to clipboard."_q);
		return true;
	}

	case DoubleTapAction::ForwardToSaved: {
		if (!item->allowsForward()) {
			controller->showToast(u"Forwarding is restricted for this message."_q);
			return false;
		}
		auto &session = item->history()->session();
		const auto savedHistory = session.data().history(session.user());
		session.api().forwardMessages(
			Data::ResolvedForwardDraft{
				.items = { item },
				.options = Data::ForwardOptions::PreserveInfo,
			},
			Api::SendAction(savedHistory));
		controller->showToast(u"Saved to Saved Messages."_q);
		return true;
	}
	}
	return false;
}

bool DeletedReplyQuoteEnabled() {
	const auto val = Config::Instance().valueOf(kDeletedReplyQuoteKey);
	return val.isUndefined() ? true : val.toBool();
}

void SetDeletedReplyQuoteEnabled(bool enabled) {
	Config::Instance().setBool(kDeletedReplyQuoteKey, enabled);
}

bool ReplyToDeletedAsQuoteEnabled() {
	const auto val = Config::Instance().valueOf(kReplyToDeletedAsQuoteKey);
	return val.isUndefined() ? true : val.toBool();
}

void SetReplyToDeletedAsQuoteEnabled(bool enabled) {
	Config::Instance().setBool(kReplyToDeletedAsQuoteKey, enabled);
}

void RetainMessage(not_null<const HistoryItem*> item) {
	if (!DeletedReplyQuoteEnabled()) {
		return;
	}
	const auto fullId = item->fullId();
	const auto author = item->author();
	const auto authorName = author
		? author->name()
		: (item->originalHiddenSenderInfo()
			? item->originalHiddenSenderInfo()->name
			: QString());
	const auto text = item->originalText().text;
	QuoteStore::Instance().retain(fullId, authorName, text, item->date());
}

void RetainReply(not_null<const HistoryItem*> replyingItem) {
	if (!DeletedReplyQuoteEnabled()) {
		return;
	}
	const auto reply = replyingItem->Get<HistoryMessageReply>();
	if (!reply) {
		return;
	}
	if (const auto original = reply->resolvedMessage.get()) {
		RetainMessage(original);
		return;
	}
	const auto &fields = reply->fields();
	if (fields.messageId) {
		const auto peerId = fields.externalPeerId
			? fields.externalPeerId
			: replyingItem->history()->peer->id;
		if (const auto orig = replyingItem->history()->owner().message(peerId, fields.messageId)) {
			RetainMessage(orig);
		} else if (!fields.displayQuote().text.isEmpty()) {
			RetainQuote(
				peerId,
				fields.messageId,
				fields.externalSenderName,
				fields.displayQuote().text);
		}
	}
}

void RetainQuote(
		FullMsgId id,
		const QString &authorName,
		const QString &text,
		TimeId date) {
	QuoteStore::Instance().retain(id, authorName, text, date);
}

void RetainQuote(
		PeerId peerId,
		MsgId msgId,
		const QString &authorName,
		const QString &text,
		TimeId date) {
	QuoteStore::Instance().retain(FullMsgId(peerId, msgId), authorName, text, date);
}

std::optional<RetainedQuote> FindRetainedQuote(FullMsgId id) {
	return QuoteStore::Instance().find(id);
}

std::optional<RetainedQuote> FindRetainedQuote(PeerId peerId, MsgId msgId) {
	return QuoteStore::Instance().find(peerId, msgId);
}

bool HasRetainedQuote(PeerId peerId, MsgId msgId) {
	return FindRetainedQuote(peerId, msgId).has_value();
}

QString FormatPreservedSnippet(const RetainedQuote &quote, int maxLen) {
	auto result = QString();
	if (!quote.authorName.isEmpty()) {
		result += quote.authorName;
	}
	if (!quote.text.isEmpty()) {
		if (!result.isEmpty()) {
			result += u": "_q;
		}
		result += quote.text;
	}
	if (result.size() > maxLen) {
		result = result.left(std::max(0, maxLen - 3)) + u"..."_q;
	}
	return result;
}

QString PreservedQuoteForDisplay(
		PeerId peerId,
		MsgId msgId,
		const QString &fallback) {
	if (!DeletedReplyQuoteEnabled()) {
		return fallback;
	}
	if (const auto quote = FindRetainedQuote(peerId, msgId)) {
		const auto formatted = FormatPreservedSnippet(*quote);
		if (!formatted.isEmpty()) {
			return u"["_q + tr::lng_deleted_message(tr::now) + u"] "_q + formatted;
		}
	}
	return fallback;
}

QString PreservedAuthorForDisplay(
		PeerId peerId,
		MsgId msgId,
		const QString &fallback) {
	if (!DeletedReplyQuoteEnabled()) {
		return fallback;
	}
	if (const auto quote = FindRetainedQuote(peerId, msgId)) {
		if (!quote->authorName.isEmpty()) {
			return quote->authorName;
		}
	}
	return fallback;
}

bool ResolveDeletedReplyState(
		PeerId peerId,
		MsgId msgId,
		QString &stateTextOut) {
	if (!DeletedReplyQuoteEnabled()) {
		return false;
	}
	const auto quote = FindRetainedQuote(peerId, msgId);
	if (!quote || quote->empty()) {
		return false;
	}
	const auto snippet = FormatPreservedSnippet(*quote, 100);
	if (snippet.isEmpty()) {
		return false;
	}
	stateTextOut = u"["_q + tr::lng_deleted_message(tr::now) + u"] "_q + snippet;
	return true;
}

bool ShouldRewriteReplyToQuote(not_null<const HistoryItem*> targetItem) {
	if (!ReplyToDeletedAsQuoteEnabled()) {
		return false;
	}
	return targetItem->isEmpty();
}

TextWithEntities BuildQuotePrefix(const RetainedQuote &quote) {
	auto result = TextWithEntities();
	const auto author = quote.authorName.isEmpty()
		? tr::lng_deleted_message(tr::now)
		: quote.authorName;
	const auto header = author + u"\n"_q;
	const auto body = quote.text;
	const auto totalQuote = header + body;

	result.text = totalQuote + u"\n\n"_q;
	result.entities.push_back({
		EntityType::Blockquote,
		0,
		int(totalQuote.size()),
	});
	result.entities.push_back({
		EntityType::Bold,
		0,
		int(author.size()),
	});
	return result;
}

TextWithEntities RewriteReplyToQuote(
		const RetainedQuote &quote,
		const TextWithEntities &replyText) {
	auto prefix = BuildQuotePrefix(quote);
	const auto shift = prefix.text.size();
	auto combined = prefix;
	combined.text += replyText.text;
	for (const auto &ent : replyText.entities) {
		combined.entities.push_back(EntityInText(
			ent.type(),
			ent.offset() + shift,
			ent.length(),
			ent.data()));
	}
	return combined;
}

void ClearRetainedQuotes() {
	QuoteStore::Instance().clear();
}

size_t RetainedQuotesCount() {
	return QuoteStore::Instance().count();
}

void AddDoubleTapSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSubsectionTitle(container, rpl::single(u"Double-Tap Message Actions"_q));

	const auto enableCheckbox = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			u"Enable Double-Tap Actions on Messages"_q,
			DoubleTapEnabled(),
			st::defaultCheckbox));

	const auto optionsWrap = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container)));

	const auto options = optionsWrap->entity();

	// Incoming Action Selection
	Ui::AddSkip(options, 6);
	options->add(
		object_ptr<Ui::FlatLabel>(
			options,
			rpl::single(u"Action for Incoming Messages:"_q),
			st::defaultFlatLabel));

	const auto incomingRadioGroup = std::make_shared<Ui::RadiobuttonGroup>(
		ActionToInt(IncomingAction()));

	for (const auto action : AllActions()) {
		options->add(
			object_ptr<Ui::Radiobutton>(
				options,
				incomingRadioGroup,
				ActionToInt(action),
				ActionName(action),
				st::defaultRadio));
	}

	incomingRadioGroup->setChangedCallback([=](int val) {
		SetIncomingAction(ActionFromInt(val));
	});

	// Outgoing Action Selection
	Ui::AddSkip(options, 10);
	options->add(
		object_ptr<Ui::FlatLabel>(
			options,
			rpl::single(u"Action for Outgoing Messages:"_q),
			st::defaultFlatLabel));

	const auto outgoingRadioGroup = std::make_shared<Ui::RadiobuttonGroup>(
		ActionToInt(OutgoingAction()));

	for (const auto action : AllActions()) {
		options->add(
			object_ptr<Ui::Radiobutton>(
				options,
				outgoingRadioGroup,
				ActionToInt(action),
				ActionName(action),
				st::defaultRadio));
	}

	outgoingRadioGroup->setChangedCallback([=](int val) {
		SetOutgoingAction(ActionFromInt(val));
	});

	// Quick Reaction Emoji
	Ui::AddSkip(options, 10);
	options->add(
		object_ptr<Ui::FlatLabel>(
			options,
			rpl::single(u"Quick Reaction Emoji:"_q),
			st::defaultFlatLabel));

	const auto reactionField = options->add(
		object_ptr<Ui::InputField>(
			options,
			st::defaultInputField,
			rpl::single(u"Emoji (default: 👍)"_q),
			DefaultReactionEmoji()));

	reactionField->changes(
	) | rpl::on_next([=] {
		const auto text = reactionField->getLastText().trimmed();
		if (!text.isEmpty()) {
			SetDefaultReactionEmoji(text);
		}
	}, reactionField->lifetime());

	// Quick preset buttons: 👍, ❤️, 🔥, 🎉, 👏, 😂
	const auto emojis = std::vector<QString>{
		u"👍"_q, u"❤️"_q, u"🔥"_q, u"🎉"_q, u"👏"_q, u"😂"_q, u"⚡"_q
	};
	const auto quickEmojiButtons = options->add(
		object_ptr<Ui::SettingsButton>(
			options,
			rpl::single(u"Cycle Preset Reaction: 👍  ❤️  🔥  🎉  👏  😂  ⚡"_q),
			st::settingsButtonNoIcon));
	quickEmojiButtons->setClickedCallback([=] {
		static int nextIdx = 0;
		nextIdx = (nextIdx + 1) % emojis.size();
		SetDefaultReactionEmoji(emojis[nextIdx]);
		reactionField->setText(emojis[nextIdx]);
		controller->showToast(u"Selected quick reaction: "_q + emojis[nextIdx]);
	});

	optionsWrap->toggle(DoubleTapEnabled(), anim::type::instant);
	enableCheckbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		SetDoubleTapEnabled(checked);
		optionsWrap->toggle(checked, anim::type::normal);
	}, enableCheckbox->lifetime());
}

void AddDeletedReplyQuoteSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSubsectionTitle(container, rpl::single(u"Deleted Reply Quotes Retention"_q));

	const auto retainCheckbox = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			u"Preserve Deleted Reply Quotes"_q,
			DeletedReplyQuoteEnabled(),
			st::defaultCheckbox));

	retainCheckbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		SetDeletedReplyQuoteEnabled(checked);
	}, retainCheckbox->lifetime());

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(u"When an author deletes their message, replies still display the retained quote snippet and sender name instead of 'Deleted message'."_q),
			st::boxDividerLabel));

	Ui::AddSkip(container, 8);

	const auto rewriteCheckbox = container->add(
		object_ptr<Ui::Checkbox>(
			container,
			u"Quote Deleted Messages in New Replies"_q,
			ReplyToDeletedAsQuoteEnabled(),
			st::defaultCheckbox));

	rewriteCheckbox->checkedChanges(
	) | rpl::on_next([=](bool checked) {
		SetReplyToDeletedAsQuoteEnabled(checked);
	}, rewriteCheckbox->lifetime());

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(u"Format replies to deleted messages with a preserved quote blockquote header and original sender name."_q),
			st::boxDividerLabel));

	Ui::AddSkip(container, 10);

	const auto cacheLabel = container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			rpl::single(QString(u"Preserved quotes in cache: %1"_q).arg(RetainedQuotesCount())),
			st::defaultFlatLabel));

	const auto clearButton = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Clear Preserved Quotes Cache"_q),
			st::settingsButtonNoIcon));

	clearButton->setClickedCallback([=] {
		ClearRetainedQuotes();
		cacheLabel->setText(u"Preserved quotes in cache: 0"_q);
		controller->showToast(u"Preserved quotes cache cleared."_q);
	});
}

void SetupSettings(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSubsectionTitle(container, rpl::single(u"Chat Extras & Gestures"_q));

	const auto button = container->add(
		object_ptr<Ui::SettingsButton>(
			container,
			rpl::single(u"Double-Tap Actions & Deleted Quotes"_q),
			st::settingsButtonNoIcon));

	button->setClickedCallback([=] {
		ShowSettingsBox(controller);
	});
}

void ShowSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"Chat Extras & Gestures"_q));
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

		const auto container = box->verticalLayout();

		AddDoubleTapSettings(container, controller);
		Ui::AddSkip(container, 16);
		AddDeletedReplyQuoteSettings(container, controller);

		box->addButton(tr::lng_close(), [=] {
			box->closeBox();
		});
	}));
}

} // namespace Extera::ChatExtras
