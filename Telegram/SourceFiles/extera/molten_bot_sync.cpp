#include "extera/molten_bot_sync.h"

#include "apiwrap.h"
#include "base/call_delayed.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/random.h"
#include "crl/crl_time.h"
#include "data/data_changes.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "extera/extera_config.h"
#include "info/profile/info_profile_emoji_status_panel.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "window/window_session_controller.h"

#include <QtCore/QDateTime>
#include <QtGui/QMouseEvent>
#include <algorithm>
#include <array>

namespace Extera::MoltenBotSync {
namespace {

constexpr auto kCacheTtlMs = int64(180'000);
constexpr auto kNegativeTtlMs = int64(120'000);
constexpr auto kPollIntervalMs = crl::time(650);
constexpr auto kPollTimeoutMs = int64(10'000);
constexpr auto kMaxUsernameRetries = 3;
constexpr auto kMaxConcurrentFetches = 2;

[[nodiscard]] int64 NowMs() {
	return QDateTime::currentMSecsSinceEpoch();
}

[[nodiscard]] QString PrefBotIdKey(uint64 ownerUserId) {
	return u"bot_sync_id_"_q + QString::number(ownerUserId);
}

[[nodiscard]] QString PrefBotUserKey(uint64 ownerUserId) {
	return u"bot_sync_username_"_q + QString::number(ownerUserId);
}

[[nodiscard]] QString PrefBotSeedKey(uint64 ownerUserId) {
	return u"bot_sync_seed_"_q + QString::number(ownerUserId);
}

[[nodiscard]] QString PrefPeerBotIdKey(uint64 ownerUserId) {
	return u"bot_sync_peer_bot_id_"_q + QString::number(ownerUserId);
}

[[nodiscard]] QString PrefPeerMetaKey(uint64 ownerUserId) {
	return u"bot_sync_peer_meta_"_q + QString::number(ownerUserId);
}

[[nodiscard]] QString PrefPeerSeedKey(uint64 ownerUserId) {
	return u"bot_sync_peer_seed_"_q + QString::number(ownerUserId);
}

struct CachedPeerBotMeta {
	int seed = 0;
	std::optional<MoltenProfile::Metadata> meta;
	int64 fetchedAtMs = 0;
	bool isError = false;
};

[[nodiscard]] auto &PeerMetaCache() {
	static auto map = base::flat_map<uint64, CachedPeerBotMeta>();
	return map;
}

[[nodiscard]] auto &InFlightFetches() {
	static auto set = base::flat_set<uint64>();
	return set;
}

[[nodiscard]] auto &SetupInProgress() {
	static auto set = base::flat_set<uint64>();
	return set;
}

auto ActiveFetchCount = 0;
auto GlobalFloodUntilMs = int64(0);

void SaveSelfBot(uint64 ownerUserId, not_null<UserData*> botUser, int seed) {
	auto &cfg = Extera::Config::Instance();
	const auto username = botUser->username().isEmpty()
		? BuildBotUsername(ownerUserId, seed)
		: botUser->username();
	cfg.setString(
		PrefBotIdKey(ownerUserId),
		QString::number(peerToUser(botUser->id).bare));
	cfg.setString(PrefBotUserKey(ownerUserId), username);
	cfg.setInt(PrefBotSeedKey(ownerUserId), seed);
}

void ClearSavedSelfBot(uint64 ownerUserId) {
	auto &cfg = Extera::Config::Instance();
	cfg.setString(PrefBotIdKey(ownerUserId), QString());
	cfg.setString(PrefBotUserKey(ownerUserId), QString());
	cfg.setInt(PrefBotSeedKey(ownerUserId), -1);
}

void HandlePeerFetchError(
		const MTP::Error &error,
		uint64 ownerUserId,
		int seed) {
	const auto text = error.type();
	if (text.startsWith(u"FLOOD_WAIT_"_q)) {
		auto ok = false;
		const auto sec = text.mid(11).toLongLong(&ok);
		const auto waitSec = std::max(int64(5), ok ? sec : int64(30));
		GlobalFloodUntilMs = NowMs() + waitSec * 1000;
	}
	PeerMetaCache()[ownerUserId] = CachedPeerBotMeta{
		.seed = seed,
		.meta = std::nullopt,
		.fetchedAtMs = NowMs(),
		.isError = true,
	};
}

[[nodiscard]] BotSetupResult MapRpcErrorToResult(const MTP::Error &error) {
	const auto text = error.type();
	if (text.compare(u"PEER_FLOOD"_q, Qt::CaseInsensitive) == 0
		|| text.compare(u"CHAT_WRITE_FORBIDDEN"_q, Qt::CaseInsensitive) == 0) {
		return { .success = false, .errorType = ErrorType::SpamBlock, .rawDetail = text };
	}
	if (text.compare(u"USER_IS_BLOCKED"_q, Qt::CaseInsensitive) == 0
		|| text.compare(u"YOU_BLOCKED_USER"_q, Qt::CaseInsensitive) == 0) {
		return { .success = false, .errorType = ErrorType::BotFatherBlocked, .rawDetail = text };
	}
	if (text.compare(u"BOT_CREATE_LIMIT_EXCEEDED"_q, Qt::CaseInsensitive) == 0) {
		return { .success = false, .errorType = ErrorType::BotLimitExceeded, .rawDetail = text };
	}
	if (text.startsWith(u"FLOOD_WAIT_"_q, Qt::CaseInsensitive)) {
		return { .success = false, .errorType = ErrorType::FloodWait, .rawDetail = text };
	}
	return {
		.success = false,
		.errorType = ErrorType::NetworkError,
		.rawDetail = text.isEmpty()
			? (u"RPC_"_q + QString::number(error.code()))
			: text,
	};
}

[[nodiscard]] std::optional<BotSetupResult> DetectBotFatherTextError(
		const QString &lower) {
	if ((lower.contains(u"more than"_q) && lower.contains(u"bot"_q))
		|| lower.contains(u"too many bots"_q)
		|| (lower.contains(u"delete a bot"_q) && lower.contains(u"create"_q))) {
		return BotSetupResult{
			.success = false,
			.errorType = ErrorType::BotLimitExceeded,
			.rawDetail = u"BOTFATHER_20_LIMIT"_q,
		};
	}
	if (lower.contains(u"flood"_q)
		|| lower.contains(u"try again later"_q)
		|| lower.contains(u"too many attempts"_q)
		|| lower.contains(u"please wait"_q)) {
		return BotSetupResult{
			.success = false,
			.errorType = ErrorType::FloodWait,
			.rawDetail = u"BOTFATHER_RATE_LIMIT"_q,
		};
	}
	if (lower.contains(u"restricted"_q) || lower.contains(u"spam"_q)) {
		return BotSetupResult{
			.success = false,
			.errorType = ErrorType::SpamBlock,
			.rawDetail = u"BOTFATHER_SPAM_RESTRICTION"_q,
		};
	}
	return std::nullopt;
}

void LoadPeerBotFull(
		not_null<Main::Session*> session,
		UserId ownerUserId,
		int seed,
		not_null<UserData*> botUser,
		bool allowResolveFallback,
		Fn<void()> finish) {
	const auto ownerBare = ownerUserId.bare;
	session->api().request(MTPusers_GetFullUser(
		botUser->inputUser()
	)).done(crl::guard(session, [=](const MTPusers_UserFull &result) {
		QString aboutText;
		QString descText;
		result.match([&](const MTPDusers_userFull &data) {
			data.vfull_user().match([&](const MTPDuserFull &full) {
				aboutText = qs(full.vabout().value_or_empty());
				if (const auto info = full.vbot_info()) {
					if (info->type() == mtpc_botInfo) {
						descText = qs(info->c_botInfo().vdescription().value_or_empty());
					}
				}
			});
		});

		auto decoded = MoltenProfile::Decode(aboutText);
		if (!decoded) {
			decoded = MoltenProfile::Decode(descText);
		}
		const auto meta = decoded.value_or(MoltenProfile::Metadata());

		PeerMetaCache()[ownerBare] = CachedPeerBotMeta{
			.seed = seed,
			.meta = meta,
			.fetchedAtMs = NowMs(),
			.isError = false,
		};
		auto &cfg = Extera::Config::Instance();
		cfg.setInt(PrefPeerSeedKey(ownerBare), seed);
		cfg.setString(
			PrefPeerMetaKey(ownerBare),
			meta.isEmpty() ? QString() : MoltenProfile::EncodeV3Direct(meta));

		auto &emojiMgr = session->data().customEmojiManager();
		if (meta.emojiStatusDocId) {
			[[maybe_unused]] const auto r = emojiMgr.resolve(meta.emojiStatusDocId);
		}
		if (meta.nameEmojiId) {
			[[maybe_unused]] const auto r = emojiMgr.resolve(meta.nameEmojiId);
		}
		if (meta.profileEmojiId) {
			[[maybe_unused]] const auto r = emojiMgr.resolve(meta.profileEmojiId);
		}

		MoltenProfile::InvalidateUser(ownerUserId);
		if (const auto ownerUser = session->data().userLoaded(ownerUserId)) {
			MoltenProfile::ApplyMetadata(ownerUser);
		}
		finish();
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		if (allowResolveFallback) {
			Extera::Config::Instance().setString(
				PrefPeerBotIdKey(ownerBare),
				QString());
		}
		HandlePeerFetchError(error, ownerBare, seed);
		finish();
	})).send();
}

void SendSetBotInfo(
		not_null<Main::Session*> session,
		uint64 ownerUserId,
		not_null<UserData*> botUser,
		const MoltenProfile::Metadata &meta,
		Fn<void(bool)> callback) {
	const auto payload = BuildBotBioPayload(meta);
	using Flag = MTPbots_SetBotInfo::Flag;
	session->api().request(MTPbots_SetBotInfo(
		MTP_flags(Flag::f_bot | Flag::f_about),
		botUser->inputUser(),
		MTPstring(),
		MTPstring(),
		MTP_string(payload),
		MTPstring()
	)).done(crl::guard(session, [=](const MTPBool &result) {
		const auto ok = mtpIsTrue(result);
		if (ok) {
			const auto seed = ParseSeedFromUsername(ownerUserId, botUser->username())
				.value_or(GetSavedSelfBotSeed(ownerUserId).value_or(0));
			PeerMetaCache()[ownerUserId] = CachedPeerBotMeta{
				.seed = seed,
				.meta = meta,
				.fetchedAtMs = NowMs(),
				.isError = false,
			};
		}
		if (callback) {
			callback(ok);
		}
	})).fail(crl::guard(session, [=](const MTP::Error &) {
		if (callback) {
			callback(false);
		}
	})).send();
}

void FindOwnedSyncBot(
		not_null<Main::Session*> session,
		uint64 ownerUserId,
		Fn<void(UserData*, std::optional<MTP::Error>)> callback) {
	session->api().request(MTPbots_GetAdminedBots(
	)).done(crl::guard(session, [=](const MTPVector<MTPUser> &result) {
		UserData *matched = nullptr;
		for (const auto &rawUser : result.v) {
			const auto u = session->data().processUser(rawUser);
			if (u->isBot()
				&& ParseSeedFromUsername(ownerUserId, u->username()).has_value()) {
				matched = u;
				break;
			}
		}
		callback(matched, std::nullopt);
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		callback(nullptr, error);
	})).send();
}

void SendQuietCancel(not_null<Main::Session*> session, not_null<UserData*> botFather) {
	using Flag = MTPmessages_SendMessage::Flag;
	auto randomId = base::RandomValue<int64>();
	if (!randomId) {
		randomId = 2;
	}
	session->api().request(MTPmessages_SendMessage(
		MTP_flags(Flag::f_no_webpage | Flag::f_silent),
		botFather->input(),
		MTPInputReplyTo(),
		MTP_string(u"/cancel"_q),
		MTP_long(randomId),
		MTPReplyMarkup(),
		MTPVector<MTPMessageEntity>(),
		MTP_int(0),
		MTP_int(0),
		MTPInputPeer(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTP_long(0),
		MTPSuggestedPost(),
		MTPInputRichMessage()
	)).send();
}

struct IncomingMsg {
	int32 id = 0;
	QString text;
};

[[nodiscard]] std::optional<IncomingMsg> ExtractLatestIncoming(
		const MTPmessages_Messages &response,
		int32 baselineMsgId) {
	const QVector<MTPMessage> *list = nullptr;
	response.match([&](const MTPDmessages_messages &d) {
		list = &d.vmessages().v;
	}, [&](const MTPDmessages_messagesSlice &d) {
		list = &d.vmessages().v;
	}, [&](const MTPDmessages_channelMessages &d) {
		list = &d.vmessages().v;
	}, [&](const MTPDmessages_messagesNotModified &) {
	});
	if (!list) {
		return std::nullopt;
	}
	auto best = std::optional<IncomingMsg>();
	for (const auto &rawMsg : *list) {
		if (rawMsg.type() != mtpc_message) {
			continue;
		}
		const auto &m = rawMsg.c_message();
		if (m.is_out()) {
			continue;
		}
		const auto msgId = m.vid().v;
		const auto text = qs(m.vmessage());
		if (msgId > baselineMsgId && !text.isEmpty()) {
			if (!best || msgId > best->id) {
				best = IncomingMsg{ msgId, text };
			}
		}
	}
	return best;
}

[[nodiscard]] int32 ExtractTopMessageId(const MTPmessages_Messages &response) {
	const QVector<MTPMessage> *list = nullptr;
	response.match([&](const MTPDmessages_messages &d) {
		list = &d.vmessages().v;
	}, [&](const MTPDmessages_messagesSlice &d) {
		list = &d.vmessages().v;
	}, [&](const MTPDmessages_channelMessages &d) {
		list = &d.vmessages().v;
	}, [&](const MTPDmessages_messagesNotModified &) {
	});
	if (!list || list->isEmpty()) {
		return 0;
	}
	return list->front().match([](const auto &m) { return m.vid().v; });
}

void PollForBotFatherReply(
		not_null<Main::Session*> session,
		not_null<UserData*> botFather,
		int32 baselineMsgId,
		int64 startedAtMs,
		Fn<void(std::optional<IncomingMsg>, std::optional<BotSetupResult>)> callback) {
	if (NowMs() - startedAtMs > kPollTimeoutMs) {
		callback(std::nullopt, BotSetupResult{
			.success = false,
			.errorType = ErrorType::Timeout,
			.rawDetail = u"BOTFATHER_TIMEOUT"_q,
		});
		return;
	}
	base::call_delayed(kPollIntervalMs, session, [=] {
		session->api().request(MTPmessages_GetHistory(
			botFather->input(),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(6),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done(crl::guard(session, [=](const MTPmessages_Messages &result) {
			if (const auto incoming = ExtractLatestIncoming(result, baselineMsgId)) {
				callback(incoming, std::nullopt);
			} else {
				PollForBotFatherReply(
					session,
					botFather,
					baselineMsgId,
					startedAtMs,
					callback);
			}
		})).fail(crl::guard(session, [=](const MTP::Error &error) {
			callback(std::nullopt, MapRpcErrorToResult(error));
		})).send();
	});
}

void SendAndWaitBotFatherReply(
		not_null<Main::Session*> session,
		not_null<UserData*> botFather,
		const QString &text,
		int32 baselineMsgId,
		Fn<void(std::optional<IncomingMsg>, std::optional<BotSetupResult>)> callback) {
	using Flag = MTPmessages_SendMessage::Flag;
	auto randomId = base::RandomValue<int64>();
	if (!randomId) {
		randomId = 1;
	}
	session->api().request(MTPmessages_SendMessage(
		MTP_flags(Flag::f_no_webpage | Flag::f_silent),
		botFather->input(),
		MTPInputReplyTo(),
		MTP_string(text),
		MTP_long(randomId),
		MTPReplyMarkup(),
		MTPVector<MTPMessageEntity>(),
		MTP_int(0),
		MTP_int(0),
		MTPInputPeer(),
		MTPInputQuickReplyShortcut(),
		MTP_long(0),
		MTP_long(0),
		MTPSuggestedPost(),
		MTPInputRichMessage()
	)).done(crl::guard(session, [=](const MTPUpdates &) {
		PollForBotFatherReply(
			session,
			botFather,
			baselineMsgId,
			NowMs(),
			callback);
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		callback(std::nullopt, MapRpcErrorToResult(error));
	})).send();
}

void FinalizeCreatedBot(
		not_null<Main::Session*> session,
		uint64 ownerId,
		not_null<UserData*> botUser,
		int seed,
		Fn<void(BotSetupResult)> finish) {
	SaveSelfBot(ownerId, botUser, seed);
	const auto meta = MoltenProfile::GetSelfMetadataSnapshot(session->user());
	SendSetBotInfo(session, ownerId, botUser, meta, [=](bool ok) {
		if (ok) {
			finish(BotSetupResult{
				.success = true,
				.botUser = botUser,
				.botUsername = botUser->username().isEmpty()
					? BuildBotUsername(ownerId, seed)
					: botUser->username(),
				.seed = seed,
			});
		} else {
			ClearSavedSelfBot(ownerId);
			finish(BotSetupResult{
				.success = false,
				.errorType = ErrorType::BotFatherRejected,
				.rawDetail = u"SET_BOT_INFO_FAILED"_q,
			});
		}
	});
}

void TrySubmitBotUsername(
		not_null<Main::Session*> session,
		uint64 ownerId,
		not_null<UserData*> botFather,
		int32 afterMsgId,
		int attempt,
		Fn<void(BotSetupResult)> finish) {
	const auto seed = (attempt == 0)
		? 0
		: (1 + int(base::RandomValue<uint16>() % 0xFFFE));
	const auto candidateUsername = BuildBotUsername(ownerId, seed);

	SendAndWaitBotFatherReply(
		session,
		botFather,
		candidateUsername,
		afterMsgId,
		[=](std::optional<IncomingMsg> reply, std::optional<BotSetupResult> rpcErr) {
			if (rpcErr) {
				SendQuietCancel(session, botFather);
				finish(*rpcErr);
				return;
			}
			const auto text = reply ? reply->text : QString();
			const auto lower = text.toLower();
			if (const auto errResult = DetectBotFatherTextError(lower)) {
				SendQuietCancel(session, botFather);
				finish(*errResult);
				return;
			}

			if (lower.contains(u"done! congratulations"_q)
				|| lower.contains(u"token to access"_q)) {
				session->api().request(MTPcontacts_ResolveUsername(
					MTP_flags(0),
					MTP_string(candidateUsername),
					MTP_string()
				)).done(crl::guard(session, [=](const MTPcontacts_ResolvedPeer &res) {
					session->data().processUsers(res.data().vusers());
					session->data().processChats(res.data().vchats());
					const auto peer = session->data().peerLoaded(
						peerFromMTP(res.data().vpeer()));
					const auto botUser = peer ? peer->asUser() : nullptr;
					if (botUser && botUser->isBot()) {
						FinalizeCreatedBot(session, ownerId, botUser, seed, finish);
						return;
					}
					FindOwnedSyncBot(session, ownerId, [=](UserData *owned, std::optional<MTP::Error>) {
						if (owned) {
							const auto actualSeed = ParseSeedFromUsername(
								ownerId,
								owned->username()).value_or(seed);
							FinalizeCreatedBot(session, ownerId, owned, actualSeed, finish);
						} else {
							finish(BotSetupResult{
								.success = false,
								.errorType = ErrorType::NetworkError,
								.rawDetail = u"BOT_RESOLVE_FAILED"_q,
							});
						}
					});
				})).fail(crl::guard(session, [=](const MTP::Error &resErr) {
					FindOwnedSyncBot(session, ownerId, [=](UserData *owned, std::optional<MTP::Error>) {
						if (owned) {
							const auto actualSeed = ParseSeedFromUsername(
								ownerId,
								owned->username()).value_or(seed);
							FinalizeCreatedBot(session, ownerId, owned, actualSeed, finish);
						} else {
							finish(BotSetupResult{
								.success = false,
								.errorType = ErrorType::NetworkError,
								.rawDetail = resErr.type().isEmpty()
									? u"BOT_RESOLVE_FAILED"_q
									: resErr.type(),
							});
						}
					});
				})).send();
				return;
			}

			const auto usernameRejected = lower.contains(u"already taken"_q)
				|| lower.contains(u"username is invalid"_q)
				|| lower.contains(u"must end in"_q);
			if (usernameRejected && reply && (attempt + 1 < kMaxUsernameRetries)) {
				TrySubmitBotUsername(
					session,
					ownerId,
					botFather,
					reply->id,
					attempt + 1,
					finish);
				return;
			}

			SendQuietCancel(session, botFather);
			finish(BotSetupResult{
				.success = false,
				.errorType = ErrorType::BotFatherRejected,
				.rawDetail = text.left(80),
			});
		});
}

void StartBotFatherWizard(
		not_null<Main::Session*> session,
		uint64 ownerId,
		Fn<void(BotSetupResult)> finish) {
	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(u"BotFather"_q),
		MTP_string()
	)).done(crl::guard(session, [=](const MTPcontacts_ResolvedPeer &result) {
		session->data().processUsers(result.data().vusers());
		session->data().processChats(result.data().vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(result.data().vpeer()));
		const auto botFather = peer ? peer->asUser() : nullptr;
		if (!botFather || !botFather->isBot()) {
			finish(BotSetupResult{
				.success = false,
				.errorType = ErrorType::BotFatherRejected,
				.rawDetail = u"BOTFATHER_NOT_FOUND"_q,
			});
			return;
		}
		session->api().request(MTPmessages_GetHistory(
			botFather->input(),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(1),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)
		)).done(crl::guard(session, [=](const MTPmessages_Messages &hist) {
			const auto baselineMsgId = ExtractTopMessageId(hist);
			SendAndWaitBotFatherReply(
				session,
				botFather,
				u"/newbot"_q,
				baselineMsgId,
				[=](std::optional<IncomingMsg> reply, std::optional<BotSetupResult> rpcErr) {
					if (rpcErr) {
						finish(*rpcErr);
						return;
					}
					const auto replyText = reply ? reply->text : QString();
					const auto lower = replyText.toLower();
					if (const auto errResult = DetectBotFatherTextError(lower)) {
						SendQuietCancel(session, botFather);
						finish(*errResult);
						return;
					}
					if (!lower.contains(u"new bot"_q)
						&& !lower.contains(u"call it"_q)
						&& !lower.contains(u"choose a name"_q)) {
						SendQuietCancel(session, botFather);
						finish(BotSetupResult{
							.success = false,
							.errorType = ErrorType::BotFatherRejected,
							.rawDetail = replyText.left(80),
						});
						return;
					}
					SendAndWaitBotFatherReply(
						session,
						botFather,
						u"MoltenGram Sync"_q,
						reply->id,
						[=](std::optional<IncomingMsg> nameReply, std::optional<BotSetupResult> nameErr) {
							if (nameErr) {
								SendQuietCancel(session, botFather);
								finish(*nameErr);
								return;
							}
							const auto nameText = nameReply ? nameReply->text : QString();
							const auto nameLower = nameText.toLower();
							if (const auto errResult = DetectBotFatherTextError(nameLower)) {
								SendQuietCancel(session, botFather);
								finish(*errResult);
								return;
							}
							if (!nameLower.contains(u"username"_q)
								&& !nameLower.contains(u"good"_q)) {
								SendQuietCancel(session, botFather);
								finish(BotSetupResult{
									.success = false,
									.errorType = ErrorType::BotFatherRejected,
									.rawDetail = nameText.left(80),
								});
								return;
							}
							TrySubmitBotUsername(
								session,
								ownerId,
								botFather,
								nameReply->id,
								0,
								finish);
						});
				});
		})).fail(crl::guard(session, [=](const MTP::Error &histErr) {
			finish(MapRpcErrorToResult(histErr));
		})).send();
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		finish(MapRpcErrorToResult(error));
	})).send();
}

constexpr auto kSwatchColors = std::array<QColor, 8>{
	QColor(0xE1, 0x70, 0x76), // 0: Red
	QColor(0xFA, 0xA7, 0x74), // 1: Orange
	QColor(0xA6, 0x95, 0xE7), // 2: Violet
	QColor(0x7B, 0xC8, 0x62), // 3: Green
	QColor(0x6E, 0xC9, 0xCB), // 4: Cyan
	QColor(0x65, 0xAA, 0xDD), // 5: Blue
	QColor(0xEE, 0x7A, 0xAE), // 6: Pink
	QColor(0x95, 0xA5, 0xA6), // 7: Slate
};

not_null<Ui::RpWidget*> AddColorSwatchRow(
		not_null<Ui::GenericBox*> box,
		int count,
		int initialIndex,
		Fn<void(int)> onSelect) {
	const auto row = box->addRow(
		object_ptr<Ui::RpWidget>(box),
		st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, st::boxLittleSkip));
	const auto selected = row->lifetime().make_state<int>(initialIndex);
	const auto swatchSize = 28;
	const auto gap = 8;
	row->resize(row->width(), swatchSize + 8);
	row->setCursor(style::cur_pointer);

	row->paintRequest() | rpl::on_next([=] {
		auto p = QPainter(row);
		PainterHighQualityEnabler hq(p);
		const auto y = 4;
		// Index -1 = Default/Clear button at position 0.
		for (int i = -1; i < count; ++i) {
			const auto slot = i + 1;
			const auto x = slot * (swatchSize + gap);
			const auto rect = QRect(x, y, swatchSize, swatchSize);
			if (i < 0) {
				p.setPen(QPen(st::windowSubTextFg->c, 1.5));
				p.setBrush(Qt::NoBrush);
				p.drawEllipse(rect.adjusted(2, 2, -2, -2));
				p.drawLine(rect.topLeft() + QPoint(7, 7), rect.bottomRight() - QPoint(7, 7));
			} else {
				const auto &color = kSwatchColors[size_t(i) % kSwatchColors.size()];
				p.setPen(Qt::NoPen);
				p.setBrush(color);
				p.drawEllipse(rect.adjusted(2, 2, -2, -2));
			}
			if (*selected == i) {
				p.setPen(QPen(st::windowBgActive->c, 2.5));
				p.setBrush(Qt::NoBrush);
				p.drawEllipse(rect.adjusted(0, 0, 0, 0));
			}
		}
	}, row->lifetime());

	row->events() | rpl::on_next([=](not_null<QEvent*> e) {
		if (e->type() != QEvent::MouseButtonRelease) {
			return;
		}
		const auto me = static_cast<QMouseEvent*>(e.get());
		const auto slot = int(me->pos().x()) / (swatchSize + gap);
		const auto idx = slot - 1;
		if (idx >= -1 && idx < count) {
			*selected = idx;
			row->update();
			onSelect(idx);
		}
	}, row->lifetime());

	return row;
}

} // namespace

QString BotSetupResult::formatUserMessage() const {
	switch (errorType) {
	case ErrorType::BotLimitExceeded:
		return u"Bot limit reached (max 20 bots on @BotFather). Delete an unused bot and try again."_q;
	case ErrorType::SpamBlock:
		return u"Unable to message @BotFather due to account spam restrictions."_q;
	case ErrorType::BotFatherBlocked:
		return u"@BotFather is blocked. Unblock @BotFather and try again."_q;
	case ErrorType::FloodWait:
		return u"Rate limited by Telegram ("_q
			+ (rawDetail.isEmpty() ? u"FLOOD_WAIT"_q : rawDetail)
			+ u"). Please try again later."_q;
	case ErrorType::None:
		return QString();
	default:
		return u"Bot sync setup failed: "_q
			+ (rawDetail.isEmpty() ? u"UNKNOWN_ERROR"_q : rawDetail);
	}
}

QString BuildBotUsername(uint64 ownerUserId, int seed) {
	const auto uid36 = QString::number(ownerUserId, 36).toLower();
	const auto seed36 = QString::number(uint(seed & 0xFFFF), 36).toLower();
	return u"mgs"_q + uid36 + u"_"_q + seed36 + u"_bot"_q;
}

std::optional<int> ParseSeedFromUsername(
		uint64 ownerUserId,
		const QString &username) {
	if (!ownerUserId || username.trimmed().isEmpty()) {
		return std::nullopt;
	}
	const auto uid36 = QString::number(ownerUserId, 36).toLower();
	auto lower = username.trimmed().toLower();
	if (lower.startsWith(u'@')) {
		lower.remove(0, 1);
	}
	const auto prefix = u"mgs"_q + uid36 + u"_"_q;
	const auto suffix = u"_bot"_q;
	if (!lower.startsWith(prefix) || !lower.endsWith(suffix)) {
		return std::nullopt;
	}
	const auto midLen = lower.size() - prefix.size() - suffix.size();
	if (midLen <= 0) {
		return std::nullopt;
	}
	const auto middle = lower.mid(prefix.size(), midLen);
	auto ok = false;
	const auto parsed = middle.toInt(&ok, 36);
	if (!ok || parsed < 0 || parsed > 0xFFFF) {
		return std::nullopt;
	}
	return parsed;
}

QString BuildBotBioPayload(const std::optional<MoltenProfile::Metadata> &meta) {
	if (!meta || meta->isEmpty()) {
		return u"MoltenGram Profile Sync"_q;
	}
	const auto encoded = MoltenProfile::EncodeV3Direct(*meta);
	return encoded.isEmpty()
		? u"MoltenGram Profile Sync"_q
		: (u"MoltenGram Sync "_q + encoded);
}

std::optional<int> GetSavedSelfBotSeed(uint64 ownerUserId) {
	if (!ownerUserId) {
		return std::nullopt;
	}
	const auto val = Extera::Config::Instance().valueOf(PrefBotSeedKey(ownerUserId));
	if (!val.isDouble()) {
		return std::nullopt;
	}
	const auto seed = val.toInt(-1);
	return (seed >= 0 && seed <= 0xFFFF) ? std::optional<int>(seed) : std::nullopt;
}

QString GetSavedSelfBotUsername(uint64 ownerUserId) {
	if (!ownerUserId) {
		return QString();
	}
	return Extera::Config::Instance().stringValue(PrefBotUserKey(ownerUserId)).trimmed();
}

uint64 GetSavedSelfBotId(uint64 ownerUserId) {
	if (!ownerUserId) {
		return 0;
	}
	auto ok = false;
	const auto val = Extera::Config::Instance()
		.stringValue(PrefBotIdKey(ownerUserId))
		.toULongLong(&ok);
	return ok ? val : 0;
}

std::optional<MoltenProfile::Metadata> GetCachedPeerBotMetadata(
		uint64 ownerUserId,
		int expectedSeed) {
	if (!ownerUserId) {
		return std::nullopt;
	}
	const auto it = PeerMetaCache().find(ownerUserId);
	if (it != PeerMetaCache().end()
		&& it->second.seed == expectedSeed
		&& !it->second.isError) {
		return it->second.meta;
	}
	auto &cfg = Extera::Config::Instance();
	const auto seedVal = cfg.valueOf(PrefPeerSeedKey(ownerUserId));
	if (seedVal.isDouble() && seedVal.toInt(-1) == expectedSeed) {
		const auto raw = cfg.stringValue(PrefPeerMetaKey(ownerUserId));
		if (const auto decoded = MoltenProfile::Decode(raw)) {
			PeerMetaCache()[ownerUserId] = CachedPeerBotMeta{
				.seed = expectedSeed,
				.meta = decoded,
				.fetchedAtMs = NowMs() - (kCacheTtlMs / 2),
				.isError = false,
			};
			return decoded;
		}
	}
	return std::nullopt;
}

void FetchPeerBotMetadataIfNeeded(
		not_null<Main::Session*> session,
		UserId ownerUserId,
		int seed) {
	const auto ownerBare = ownerUserId.bare;
	if (!ownerBare || seed < 0 || seed > 0xFFFF) {
		return;
	}
	if (!MoltenProfile::CustomEmojiStatusEnabled()
		&& !MoltenProfile::FreeProfileAppearanceEnabled()) {
		return;
	}
	const auto now = NowMs();
	if (now < GlobalFloodUntilMs) {
		return;
	}
	const auto it = PeerMetaCache().find(ownerBare);
	if (it != PeerMetaCache().end() && it->second.seed == seed) {
		const auto ttl = it->second.isError ? kNegativeTtlMs : kCacheTtlMs;
		if (now - it->second.fetchedAtMs < ttl) {
			return;
		}
	}
	if (InFlightFetches().contains(ownerBare)) {
		return;
	}
	if (ActiveFetchCount >= kMaxConcurrentFetches) {
		return;
	}
	InFlightFetches().emplace(ownerBare);
	++ActiveFetchCount;

	const auto finish = [=] {
		--ActiveFetchCount;
		InFlightFetches().remove(ownerBare);
	};

	const auto targetUsername = BuildBotUsername(ownerBare, seed);
	auto ok = false;
	const auto cachedBotId = Extera::Config::Instance()
		.stringValue(PrefPeerBotIdKey(ownerBare))
		.toULongLong(&ok);
	if (ok && cachedBotId) {
		if (const auto botUser = session->data().userLoaded(UserId(cachedBotId))) {
			if (botUser->isBot()
				&& botUser->username().compare(targetUsername, Qt::CaseInsensitive) == 0) {
				LoadPeerBotFull(session, ownerUserId, seed, botUser, true, finish);
				return;
			}
		}
	}

	session->api().request(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string(targetUsername),
		MTP_string()
	)).done(crl::guard(session, [=](const MTPcontacts_ResolvedPeer &result) {
		session->data().processUsers(result.data().vusers());
		session->data().processChats(result.data().vchats());
		const auto peer = session->data().peerLoaded(
			peerFromMTP(result.data().vpeer()));
		const auto botUser = peer ? peer->asUser() : nullptr;
		if (!botUser || !botUser->isBot()) {
			PeerMetaCache()[ownerBare] = CachedPeerBotMeta{
				.seed = seed,
				.meta = std::nullopt,
				.fetchedAtMs = NowMs(),
				.isError = true,
			};
			finish();
			return;
		}
		Extera::Config::Instance().setString(
			PrefPeerBotIdKey(ownerBare),
			QString::number(peerToUser(botUser->id).bare));
		LoadPeerBotFull(session, ownerUserId, seed, botUser, false, finish);
	})).fail(crl::guard(session, [=](const MTP::Error &error) {
		HandlePeerFetchError(error, ownerBare, seed);
		finish();
	})).send();
}

void SyncMetadataToBot(
		not_null<Main::Session*> session,
		const MoltenProfile::Metadata &meta,
		Fn<void(bool)> callback) {
	const auto self = session->user();
	const auto ownerId = peerToUser(self->id).bare;
	const auto botId = GetSavedSelfBotId(ownerId);
	if (botId) {
		if (const auto cachedBot = session->data().userLoaded(UserId(botId))) {
			if (cachedBot->isBot()) {
				SendSetBotInfo(session, ownerId, cachedBot, meta, std::move(callback));
				return;
			}
		}
	}
	FindOwnedSyncBot(session, ownerId, [=](UserData *botUser, std::optional<MTP::Error>) {
		if (botUser) {
			const auto seed = ParseSeedFromUsername(ownerId, botUser->username())
				.value_or(0);
			SaveSelfBot(ownerId, botUser, seed);
			SendSetBotInfo(session, ownerId, botUser, meta, callback);
		} else if (callback) {
			callback(false);
		}
	});
}

void EnsureBotReady(
		not_null<Main::Session*> session,
		bool allowInteractiveBotFather,
		Fn<void(BotSetupResult)> callback) {
	const auto self = session->user();
	const auto ownerId = peerToUser(self->id).bare;
	if (!ownerId) {
		if (callback) {
			callback(BotSetupResult{
				.success = false,
				.errorType = ErrorType::NotAuthorized,
			});
		}
		return;
	}
	if (SetupInProgress().contains(ownerId)) {
		if (callback) {
			callback(BotSetupResult{
				.success = false,
				.errorType = ErrorType::AlreadyInProgress,
			});
		}
		return;
	}
	SetupInProgress().emplace(ownerId);

	const auto finish = [=](BotSetupResult result) {
		SetupInProgress().remove(ownerId);
		if (callback) {
			callback(std::move(result));
		}
	};

	FindOwnedSyncBot(session, ownerId, [=](UserData *existingBot, std::optional<MTP::Error> adminErr) {
		if (existingBot) {
			const auto seed = ParseSeedFromUsername(ownerId, existingBot->username())
				.value_or(0);
			SaveSelfBot(ownerId, existingBot, seed);
			const auto initialMeta = MoltenProfile::GetSelfMetadataSnapshot(self);
			SendSetBotInfo(session, ownerId, existingBot, initialMeta, [=](bool ok) {
				if (ok) {
					finish(BotSetupResult{
						.success = true,
						.botUser = existingBot,
						.botUsername = existingBot->username(),
						.seed = seed,
					});
				} else if (!allowInteractiveBotFather) {
					ClearSavedSelfBot(ownerId);
					finish(BotSetupResult{
						.success = false,
						.errorType = ErrorType::BotFatherRejected,
						.rawDetail = u"SET_BOT_INFO_FAILED"_q,
					});
				} else {
					ClearSavedSelfBot(ownerId);
					StartBotFatherWizard(session, ownerId, finish);
				}
			});
			return;
		}

		ClearSavedSelfBot(ownerId);
		if (!allowInteractiveBotFather) {
			finish(BotSetupResult{
				.success = false,
				.errorType = adminErr ? ErrorType::NetworkError : ErrorType::BotFatherRejected,
				.rawDetail = adminErr ? adminErr->type() : u"NO_EXISTING_BOT"_q,
			});
			return;
		}
		StartBotFatherWizard(session, ownerId, finish);
	});
}

void ShowProfileStylingBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		const auto session = &controller->session();
		const auto self = session->user();
		const auto ownerId = peerToUser(self->id).bare;

		box->setTitle(rpl::single(u"MoltenGram Profile Styling"_q));
		box->setWidth(st::boxWideWidth);

		struct State {
			MoltenProfile::Metadata meta;
			std::unique_ptr<Ui::Text::CustomEmoji> statusEmoji;
			Info::Profile::EmojiStatusPanel statusPanel;
		};
		const auto state = box->lifetime().make_state<State>();
		state->meta = MoltenProfile::GetSelfMetadataSnapshot(self);

		// 1. Companion Bot Sync status / setup button
		const auto botStatusText = [=] {
			const auto botUser = GetSavedSelfBotUsername(ownerId);
			if (MoltenProfile::BotProfileSyncEnabled() && !botUser.isEmpty()) {
				return u"Companion Bot: @"_q + botUser + u" (Synced)"_q;
			}
			return u"Set up Companion Bot Sync (@BotFather)"_q;
		};
		const auto botButtonText = box->lifetime().make_state<rpl::variable<QString>>(
			botStatusText());
		const auto botButton = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				botButtonText->value(),
				st::settingsButtonNoIcon),
			style::margins());
		botButton->setClickedCallback([=] {
			controller->showToast(u"Setting up MoltenGram Sync bot via @BotFather..."_q);
			EnsureBotReady(session, true, crl::guard(box, [=](BotSetupResult res) {
				if (res.success) {
					Extera::Config::Instance().setBool(u"molten_bot_profile_sync"_q, true);
					*botButtonText = botStatusText();
					MoltenProfile::ResyncSelfMetadata(session, nullptr);
					controller->showToast(
						u"Companion bot @"_q + res.botUsername + u" ready!"_q);
				} else if (res.errorType != ErrorType::AlreadyInProgress) {
					controller->showToast(res.formatUserMessage());
				}
			}));
		});

		// 2. Free Custom Emoji Status picker
		const auto statusButton = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(u"Custom Emoji Status (Free)"_q),
				st::settingsButtonNoIcon),
			style::margins());
		const auto preview = Ui::CreateChild<Ui::RpWidget>(statusButton);
		const auto previewSize = Data::FrameSizeFromTag(
			Data::CustomEmojiManager::SizeTag::Large
		) / style::DevicePixelRatio();
		preview->resize(previewSize, previewSize);
		preview->setAttribute(Qt::WA_TransparentForMouseEvents);

		const auto refreshStatusPreview = [=] {
			state->statusEmoji = state->meta.emojiStatusDocId
				? session->data().customEmojiManager().create(
					state->meta.emojiStatusDocId,
					[=] { preview->update(); },
					Data::CustomEmojiManager::SizeTag::Large)
				: nullptr;
			preview->update();
		};
		refreshStatusPreview();

		statusButton->sizeValue() | rpl::on_next([=](QSize outer) {
			preview->moveToRight(
				st::settingsButtonNoIcon.padding.right(),
				(outer.height() - preview->height()) / 2);
		}, preview->lifetime());

		preview->paintRequest() | rpl::on_next([=] {
			if (!state->statusEmoji) {
				return;
			}
			auto p = QPainter(preview);
			state->statusEmoji->paint(p, {
				.textColor = st::windowFg->c,
				.now = crl::now(),
				.position = QPoint(0, 0),
			});
		}, preview->lifetime());

		statusButton->setClickedCallback([=] {
			state->statusPanel.show({
				.controller = controller,
				.button = preview,
				.channelStatusMode = true,
			});
		});

		state->statusPanel.someCustomChosen(
		) | rpl::on_next([=](const Info::Profile::EmojiStatusPanel::CustomChosen &chosen) {
			state->meta.emojiStatusDocId = chosen.id.documentId;
			state->meta.emojiStatusCollectible = (chosen.id.collectible != nullptr);
			refreshStatusPreview();
		}, box->lifetime());

		const auto clearStatusBtn = box->addRow(
			object_ptr<Ui::SettingsButton>(
				box,
				rpl::single(u"Clear Custom Emoji Status"_q),
				st::settingsButtonNoIcon),
			style::margins());
		clearStatusBtn->setClickedCallback([=] {
			state->meta.emojiStatusDocId = 0;
			state->meta.emojiStatusCollectible = false;
			refreshStatusPreview();
		});

		// 3. Name Color picker
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Name & Reply Color"_q,
				st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
		AddColorSwatchRow(box, 7, state->meta.nameColorId, [=](int idx) {
			state->meta.nameColorId = idx;
		});

		// 4. Profile Color picker
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Profile Header Color"_q,
				st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));
		AddColorSwatchRow(box, 8, state->meta.profileColorId, [=](int idx) {
			state->meta.profileColorId = idx;
		});

		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box,
				u"Visible to all MoltenGram users without Telegram Premium. Uses an invisible V3 tag in your last name (or a 10-char pointer to your companion sync bot)."_q,
				st::boxDividerLabel),
			st::boxRowPadding + QMargins(0, st::boxLittleSkip, 0, 0));

		box->addButton(tr::lng_settings_save(), [=] {
			MoltenProfile::UpdateSelfMetadata(
				session,
				state->meta,
				crl::guard(session, [=](bool ok) {
					controller->showToast(ok
						? u"Profile styling updated!"_q
						: u"Saved locally (profile sync failed)."_q);
				}));
			box->closeBox();
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

} // namespace Extera::MoltenBotSync
