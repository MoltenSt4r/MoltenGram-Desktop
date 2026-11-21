#include "extera/molten_profile_metadata.h"

#include "apiwrap.h"
#include "base/flat_map.h"
#include "data/data_changes.h"
#include "data/data_peer.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/stickers/data_custom_emoji.h"
#include "extera/extera_config.h"
#include "extera/molten_bot_sync.h"
#include "main/main_session.h"
#include "mtproto/mtproto_config.h"
#include "ui/chat/chat_style.h"

#include <QtCore/QRegularExpression>
#include <algorithm>
#include <limits>
#include <vector>

namespace Extera::MoltenProfile {
namespace {

constexpr auto kV3KindMetadata = 0x3;
constexpr auto kV3KindBotRef = 0x4;

constexpr auto kV2Delimiter = char16_t(0xFE0E);
constexpr auto kV2KeyStatus = char16_t(0xFE00);
constexpr auto kV2KeyNameColor = char16_t(0xFE01);
constexpr auto kV2KeyNameEmoji = char16_t(0xFE02);
constexpr auto kV2KeyProfileColor = char16_t(0xFE03);
constexpr auto kV2KeyProfileEmoji = char16_t(0xFE04);
constexpr auto kV2KeyQrBackdrop = char16_t(0xFE05);
constexpr auto kV2KeyQrEmoji = char16_t(0xFE06);
constexpr auto kV2KeyCollectibleStatus = char16_t(0xFE07);

constexpr auto kV1Delimiter = char16_t(0x200B);
constexpr auto kV1KeyStatus = char16_t(0x206A);
constexpr auto kV1KeyNameColor = char16_t(0x206B);
constexpr auto kV1KeyNameEmoji = char16_t(0x206C);
constexpr auto kV1KeyProfileColor = char16_t(0x206D);
constexpr auto kV1KeyProfileEmoji = char16_t(0x206E);
constexpr auto kV1KeyQrBackdrop = char16_t(0x2062);
constexpr auto kV1KeyQrEmoji = char16_t(0x2063);

[[nodiscard]] const QString &V3StartTag() {
	static const auto k = QString::fromUtf16(u"\uFE0B\uFE0C");
	return k;
}

[[nodiscard]] const QString &V3EndTag() {
	static const auto k = QString::fromUtf16(u"\uFE0C\uFE0B");
	return k;
}

[[nodiscard]] const QRegularExpression &V3TagRegex() {
	static const auto re = QRegularExpression(
		u"\uFE0B\uFE0C([\uFE00-\uFE0F]{3,}?)\uFE0C\uFE0B"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &V3OrphanRegex() {
	static const auto re = QRegularExpression(
		u"\uFE0B\uFE0C[\uFE00-\uFE0F]{3,}"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &V2TagRegex() {
	static const auto re = QRegularExpression(
		u"\uFE0F\uFE0E([\uFE00-\uFE0E]+?)\uFE0E\uFE0F"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &V2OrphanRegex() {
	static const auto re = QRegularExpression(
		u"\uFE0F\uFE0E[\uFE00-\uFE0E]{3,}"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &V1TagRegex() {
	static const auto re = QRegularExpression(
		u"\u200D\u2060([\uFE00-\uFE0F\u200B\u2061-\u206E]+?)\u2060\u200D"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &V1OrphanRegex() {
	static const auto re = QRegularExpression(
		u"\u200D\u2060[\uFE00-\uFE0F\u200B-\u200D\u2060-\u206E]{3,}"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &FramedBase32Regex() {
	static const auto re = QRegularExpression(
		u"\u200D\u2060([\uFE00-\uFE0F\u200B-\u200D\u2060\uFEFF\u200E\u200F\u2061-\u2064\u206A-\u206E]+?)\u2060\u200D"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &LegacyVsRegex() {
	static const auto re = QRegularExpression(u"[\uFE00-\uFE0F]{8,}"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &TgEmojiRegex() {
	static const auto re = QRegularExpression(u"tg://emoji\\?id=(\\d+)"_q);
	return re;
}

[[nodiscard]] const QRegularExpression &InvisibleCleanupRegex() {
	static const auto re = QRegularExpression(
		u"[\u2060\u200B-\u200F\uFEFF\u2061-\u2064\u206A-\u206E]{2,}"_q);
	return re;
}

[[nodiscard]] QChar NibbleToChar(int n) {
	return QChar(ushort(0xFE00 + (n & 0xF)));
}

[[nodiscard]] int CharToNibble(QChar c) {
	const auto code = c.unicode();
	return (code >= 0xFE00 && code <= 0xFE0F) ? int(code - 0xFE00) : -1;
}

[[nodiscard]] int ComputeCrc4(const std::vector<int> &nibbles, size_t count) {
	auto h = 0x5;
	for (size_t i = 0; i < count; ++i) {
		h = ((h * 5) + nibbles[i] + 3) & 0xF;
	}
	return h;
}

void AppendIntSmallOrExt(std::vector<int> &out, int value) {
	const auto v = std::max(0, value);
	if (v < 0xFF) {
		out.push_back((v >> 4) & 0xF);
		out.push_back(v & 0xF);
	} else {
		out.push_back(0xF);
		out.push_back(0xF);
		const auto clamped = std::min(v, 0xFFFF);
		out.push_back((clamped >> 12) & 0xF);
		out.push_back((clamped >> 8) & 0xF);
		out.push_back((clamped >> 4) & 0xF);
		out.push_back(clamped & 0xF);
	}
}

[[nodiscard]] std::optional<int> ReadIntSmallOrExt(
		const std::vector<int> &nibbles,
		size_t &idx) {
	if (idx + 2 > nibbles.size()) {
		return std::nullopt;
	}
	const auto b = (nibbles[idx] << 4) | nibbles[idx + 1];
	if (b < 0xFF) {
		idx += 2;
		return b;
	}
	if (idx + 6 > nibbles.size()) {
		return std::nullopt;
	}
	const auto ext = (nibbles[idx + 2] << 12)
		| (nibbles[idx + 3] << 8)
		| (nibbles[idx + 4] << 4)
		| nibbles[idx + 5];
	idx += 6;
	return ext;
}

void AppendU64Nibbles(std::vector<int> &out, uint64 value) {
	for (int shift = 60; shift >= 0; shift -= 4) {
		out.push_back(int((value >> shift) & 0xFULL));
	}
}

[[nodiscard]] std::optional<uint64> ReadU64Nibbles(
		const std::vector<int> &nibbles,
		size_t &idx) {
	if (idx + 16 > nibbles.size()) {
		return std::nullopt;
	}
	auto value = uint64(0);
	for (size_t k = 0; k < 16; ++k) {
		value = (value << 4) | uint64(nibbles[idx + k]);
	}
	idx += 16;
	return value;
}

[[nodiscard]] uint64 DecodeBase14(const QString &str) {
	if (str.isEmpty()) {
		return 0;
	}
	auto value = uint64(0);
	for (const auto ch : str) {
		const auto code = ch.unicode();
		if (code >= 0xFE00 && code <= 0xFE0D) {
			value = value * 14ULL + uint64(code - 0xFE00);
		} else {
			return 0;
		}
	}
	return value;
}

[[nodiscard]] uint64 DecodeV1Hex(const QString &str) {
	if (str.isEmpty()) {
		return 0;
	}
	auto value = uint64(0);
	for (const auto ch : str) {
		const auto n = CharToNibble(ch);
		if (n < 0) {
			return 0;
		}
		value = (value << 4) | uint64(n);
	}
	return value;
}

[[nodiscard]] int CharToBase32Index(QChar ch) {
	const auto code = ch.unicode();
	if (code >= 0xFE00 && code <= 0xFE0F) {
		return int(code - 0xFE00);
	}
	switch (code) {
	case 0x200B: return 16;
	case 0x200C: return 17;
	case 0x200D: return 18;
	case 0x2060: return 19;
	case 0xFEFF: return 20;
	case 0x200E: return 21;
	case 0x200F: return 22;
	case 0x2061: return 23;
	case 0x2062: return 24;
	case 0x2063: return 25;
	case 0x2064: return 26;
	case 0x206A: return 27;
	case 0x206B: return 28;
	case 0x206C: return 29;
	case 0x206D: return 30;
	case 0x206E: return 31;
	default: return -1;
	}
}

[[nodiscard]] std::optional<Metadata> DecodeV3Payload(const QString &raw) {
	if (raw.size() < 4) {
		return std::nullopt;
	}
	auto nibbles = std::vector<int>();
	nibbles.reserve(size_t(raw.size()));
	for (const auto ch : raw) {
		const auto n = CharToNibble(ch);
		if (n < 0) {
			return std::nullopt;
		}
		nibbles.push_back(n);
	}
	if (nibbles[0] != kV3KindMetadata) {
		return std::nullopt;
	}
	const auto crcIdx = nibbles.size() - 1;
	if (nibbles[crcIdx] != ComputeCrc4(nibbles, crcIdx)) {
		return std::nullopt;
	}
	const auto mask = (nibbles[1] << 4) | nibbles[2];
	auto data = std::vector<int>(nibbles.begin(), nibbles.begin() + ptrdiff_t(crcIdx));
	size_t idx = 3;
	auto meta = Metadata();

	if (mask & 0x01) {
		const auto v = ReadU64Nibbles(data, idx);
		if (!v) return std::nullopt;
		meta.emojiStatusDocId = DocumentId(*v);
		meta.emojiStatusCollectible = ((mask & 0x02) != 0) && (meta.emojiStatusDocId != 0);
	}
	if (mask & 0x04) {
		const auto v = ReadIntSmallOrExt(data, idx);
		if (!v) return std::nullopt;
		meta.nameColorId = *v;
	}
	if (mask & 0x08) {
		const auto v = ReadU64Nibbles(data, idx);
		if (!v) return std::nullopt;
		meta.nameEmojiId = DocumentId(*v);
	}
	if (mask & 0x10) {
		const auto v = ReadIntSmallOrExt(data, idx);
		if (!v) return std::nullopt;
		meta.profileColorId = *v;
	}
	if (mask & 0x20) {
		const auto v = ReadU64Nibbles(data, idx);
		if (!v) return std::nullopt;
		meta.profileEmojiId = DocumentId(*v);
	}
	if (mask & 0x40) {
		const auto v = ReadIntSmallOrExt(data, idx);
		if (!v) return std::nullopt;
		meta.qrBackdropId = *v;
	}
	if (mask & 0x80) {
		const auto v = ReadU64Nibbles(data, idx);
		if (!v) return std::nullopt;
		meta.qrEmojiId = DocumentId(*v);
	}

	if (idx != data.size() || meta.isEmpty()) {
		return std::nullopt;
	}
	return meta;
}

[[nodiscard]] QString TrimEnd(QString value) {
	while (!value.isEmpty() && value.back().isSpace()) {
		value.chop(1);
	}
	return value;
}

[[nodiscard]] QString TrimStart(QString value) {
	qsizetype pos = 0;
	while (pos < value.size() && value[pos].isSpace()) {
		++pos;
	}
	return (pos > 0) ? value.mid(pos) : value;
}

[[nodiscard]] bool ConfigBool(const QString &key, bool fallback) {
	const auto val = Extera::Config::Instance().valueOf(key);
	return val.isBool() ? val.toBool() : fallback;
}

[[nodiscard]] QString SelfMetaKey(UserId userId) {
	return u"profile_invisible_meta_self_"_q + QString::number(userId.bare);
}

struct CachedDirectMeta {
	QString rawLastName;
	std::optional<Metadata> meta;
};

struct AppliedColor {
	int colorId = -1;
	DocumentId emojiId = 0;
};

[[nodiscard]] auto &RawLastNames() {
	static auto map = base::flat_map<UserId, QString>();
	return map;
}

[[nodiscard]] auto &DirectMetaCache() {
	static auto map = base::flat_map<UserId, CachedDirectMeta>();
	return map;
}

[[nodiscard]] auto &AppliedFakeStatus() {
	static auto map = base::flat_map<UserId, DocumentId>();
	return map;
}

[[nodiscard]] auto &AppliedFakeNameColor() {
	static auto map = base::flat_map<UserId, AppliedColor>();
	return map;
}

[[nodiscard]] auto &AppliedFakeProfileColor() {
	static auto map = base::flat_map<UserId, AppliedColor>();
	return map;
}

[[nodiscard]] Metadata LoadSelfMetadata(not_null<UserData*> self) {
	const auto raw = Extera::Config::Instance().stringValue(
		SelfMetaKey(peerToUser(self->id)));
	if (!raw.isEmpty()) {
		if (const auto decoded = Decode(raw)) {
			return *decoded;
		}
	}
	if (const auto decoded = Decode(RawLastName(self))) {
		return *decoded;
	}
	return Metadata();
}

void PersistSelfMetadata(UserId userId, const Metadata &meta) {
	if (meta.isEmpty()) {
		Extera::Config::Instance().setString(SelfMetaKey(userId), QString());
	} else {
		Extera::Config::Instance().setString(
			SelfMetaKey(userId),
			EncodeV3Direct(meta));
	}
}

[[nodiscard]] QString ComputeDesiredLastNameTag(
		not_null<UserData*> self,
		const Metadata &meta) {
	if (meta.isEmpty()) {
		return QString();
	}
	if (BotProfileSyncEnabled()) {
		const auto savedSeed = MoltenBotSync::GetSavedSelfBotSeed(
			peerToUser(self->id).bare);
		if (savedSeed.has_value()) {
			return EncodeBotRef(*savedSeed);
		}
	}
	return EncodeWithLimit(meta, kMaxLastNameLength);
}

void ApplyMetaToUserInternal(
		not_null<UserData*> user,
		const std::optional<Metadata> &meta,
		bool hasNativeEmojiStatus,
		bool hasNativeNameColor,
		bool hasNativeProfileColor,
		bool nativeFlagsKnown) {
	const auto uid = peerToUser(user->id);
	using UpdateFlag = Data::PeerUpdate::Flag;
	auto updateFlags = UpdateFlag::None | UpdateFlag::None;

	if (CustomEmojiStatusEnabled()) {
		const auto currentDocId = user->emojiStatusId().documentId;
		const auto prevIt = AppliedFakeStatus().find(uid);
		const auto prevFakeId = (prevIt != AppliedFakeStatus().end())
			? prevIt->second
			: DocumentId(0);
		const auto isSyntheticOrEmpty = nativeFlagsKnown
			? !hasNativeEmojiStatus
			: (!currentDocId || (prevFakeId && currentDocId == prevFakeId));
		if (isSyntheticOrEmpty) {
			const auto metaDocId = meta ? meta->emojiStatusDocId : DocumentId(0);
			if (metaDocId) {
				user->setEmojiStatus(EmojiStatusId{ metaDocId });
				AppliedFakeStatus()[uid] = metaDocId;
			} else if (prevFakeId && currentDocId == prevFakeId) {
				user->setEmojiStatus(EmojiStatusId());
				AppliedFakeStatus().remove(uid);
			}
		} else if (currentDocId) {
			AppliedFakeStatus().remove(uid);
		}
	}

	if (FreeProfileAppearanceEnabled()) {
		const auto prevIt = AppliedFakeNameColor().find(uid);
		const auto hasPrev = (prevIt != AppliedFakeNameColor().end());
		const auto isSyntheticOrEmpty = nativeFlagsKnown
			? !hasNativeNameColor
			: (hasPrev
				&& int(user->colorIndex()) == (prevIt->second.colorId % int(Ui::kColorIndexCount))
				&& user->backgroundEmojiId() == prevIt->second.emojiId)
				|| !hasPrev;
		if (isSyntheticOrEmpty && (!nativeFlagsKnown || !hasNativeNameColor)) {
			if (meta && meta->nameColorId >= 0) {
				const auto c1 = user->changeColorIndex(uint8(meta->nameColorId));
				const auto c2 = user->changeBackgroundEmojiId(meta->nameEmojiId);
				AppliedFakeNameColor()[uid] = AppliedColor{
					meta->nameColorId,
					meta->nameEmojiId,
				};
				if (c1 || c2) {
					updateFlags |= UpdateFlag::Color;
				}
			} else if (hasPrev) {
				const auto c1 = user->clearColorIndex();
				const auto c2 = user->changeBackgroundEmojiId(0);
				AppliedFakeNameColor().remove(uid);
				if (c1 || c2) {
					updateFlags |= UpdateFlag::Color;
				}
			}
		} else if (nativeFlagsKnown && hasNativeNameColor) {
			AppliedFakeNameColor().remove(uid);
		}
	}

	if (FreeProfileAppearanceEnabled()) {
		const auto prevIt = AppliedFakeProfileColor().find(uid);
		const auto hasPrev = (prevIt != AppliedFakeProfileColor().end());
		const auto currentProfile = user->colorProfileIndex();
		const auto isSyntheticOrEmpty = nativeFlagsKnown
			? !hasNativeProfileColor
			: (!currentProfile.has_value()
				|| (hasPrev
					&& int(*currentProfile) == (prevIt->second.colorId % int(Ui::kColorIndexCount))
					&& user->profileBackgroundEmojiId() == prevIt->second.emojiId));
		if (isSyntheticOrEmpty) {
			if (meta && meta->profileColorId >= 0) {
				const auto c1 = user->changeColorProfileIndex(uint8(meta->profileColorId));
				const auto c2 = user->changeProfileBackgroundEmojiId(meta->profileEmojiId);
				AppliedFakeProfileColor()[uid] = AppliedColor{
					meta->profileColorId,
					meta->profileEmojiId,
				};
				if (c1 || c2) {
					updateFlags |= UpdateFlag::ColorProfile;
				}
			} else if (hasPrev) {
				const auto c1 = user->clearColorProfileIndex();
				const auto c2 = user->changeProfileBackgroundEmojiId(0);
				AppliedFakeProfileColor().remove(uid);
				if (c1 || c2) {
					updateFlags |= UpdateFlag::ColorProfile;
				}
			}
		} else if (nativeFlagsKnown && hasNativeProfileColor) {
			AppliedFakeProfileColor().remove(uid);
		}
	}

	if (updateFlags != UpdateFlag::None) {
		user->session().changes().peerUpdated(user, updateFlags);
	}
}

void CommitProfileTag(
		not_null<Main::Session*> session,
		const QString &tag,
		Fn<void(bool)> callback) {
	const auto self = session->user();
	const auto oldFirst = self->firstName;
	const auto oldLastRaw = RawLastName(self);
	const auto [newFirst, newLast] = PackProfileNamesWithTag(
		oldFirst,
		oldLastRaw,
		tag);

	InvalidateUser(peerToUser(self->id));

	if (newFirst == oldFirst && newLast == oldLastRaw) {
		ApplyMetadata(self);
		if (callback) {
			callback(true);
		}
		return;
	}

	using Flag = MTPaccount_UpdateProfile::Flag;
	auto flags = Flag::f_last_name | Flag::f_last_name;
	if (newFirst != oldFirst) {
		flags |= Flag::f_first_name;
	}

	RecordRawLastName(self, newLast);
	self->setName(newFirst, newLast, QString(), self->editableUsername());
	ApplyMetadata(self);

	session->api().request(MTPaccount_UpdateProfile(
		MTP_flags(flags),
		MTP_string(newFirst),
		MTP_string(newLast),
		MTPstring()
	)).done([=](const MTPUser &result) {
		session->data().processUser(result);
		if (callback) {
			callback(true);
		}
	}).fail([=](const MTP::Error &) {
		if (callback) {
			callback(false);
		}
	}).send();
}

void SaveSelfMetadata(
		not_null<Main::Session*> session,
		const Metadata &meta,
		Fn<void(bool)> callback) {
	const auto self = session->user();
	const auto uid = peerToUser(self->id);
	PersistSelfMetadata(uid, meta);

	// Optimistic local visuals for non-premium users.
	if (!self->isPremiumReal()) {
		ApplyMetaToUserInternal(self, meta, false, false, false, false);
	}

	const auto usingBotSync = BotProfileSyncEnabled();
	const auto savedBotSeed = usingBotSync
		? MoltenBotSync::GetSavedSelfBotSeed(uid.bare)
		: std::optional<int>();

	if (usingBotSync && !savedBotSeed.has_value()) {
		MoltenBotSync::EnsureBotReady(
			session,
			false,
			[=](MoltenBotSync::BotSetupResult res) {
				if (res.success) {
					MoltenBotSync::SyncMetadataToBot(session, meta, nullptr);
					CommitProfileTag(
						session,
						meta.isEmpty() ? QString() : EncodeBotRef(res.seed),
						callback);
				} else {
					CommitProfileTag(
						session,
						EncodeWithLimit(meta, kMaxLastNameLength),
						callback);
				}
			});
		return;
	}

	QString tag;
	if (usingBotSync && savedBotSeed.has_value()) {
		MoltenBotSync::SyncMetadataToBot(session, meta, nullptr);
		tag = meta.isEmpty() ? QString() : EncodeBotRef(*savedBotSeed);
	} else {
		tag = EncodeWithLimit(meta, kMaxLastNameLength);
	}

	CommitProfileTag(session, tag, std::move(callback));
}

} // namespace

bool CustomEmojiStatusEnabled() {
	return ConfigBool(u"molten_custom_emoji_status"_q, true);
}

bool FreeProfileAppearanceEnabled() {
	return ConfigBool(u"molten_free_profile_appearance"_q, true);
}

bool BotProfileSyncEnabled() {
	return ConfigBool(u"molten_bot_profile_sync"_q, false);
}

bool CleanBioDisplayEnabled() {
	return ConfigBool(u"molten_clean_bio_display"_q, true);
}

QString EncodeBotRef(int seed) {
	const auto s = seed & 0xFFFF;
	const auto data = std::vector<int>{
		kV3KindBotRef,
		(s >> 12) & 0xF,
		(s >> 8) & 0xF,
		(s >> 4) & 0xF,
		s & 0xF,
	};
	const auto crc = ComputeCrc4(data, data.size());
	QString out;
	out.reserve(10);
	out.append(V3StartTag());
	for (const auto n : data) {
		out.append(NibbleToChar(n));
	}
	out.append(NibbleToChar(crc));
	out.append(V3EndTag());
	return out;
}

std::optional<int> DecodeBotRef(const QString &text) {
	if (text.isEmpty()) {
		return std::nullopt;
	}
	auto it = V3TagRegex().globalMatch(text);
	while (it.hasNext()) {
		const auto match = it.next();
		const auto raw = match.captured(1);
		if (raw.size() != 6) {
			continue;
		}
		auto nibbles = std::vector<int>();
		nibbles.reserve(6);
		auto valid = true;
		for (const auto ch : raw) {
			const auto n = CharToNibble(ch);
			if (n < 0) {
				valid = false;
				break;
			}
			nibbles.push_back(n);
		}
		if (!valid || nibbles[0] != kV3KindBotRef) {
			continue;
		}
		if (nibbles[5] != ComputeCrc4(nibbles, 5)) {
			continue;
		}
		return (nibbles[1] << 12)
			| (nibbles[2] << 8)
			| (nibbles[3] << 4)
			| nibbles[4];
	}
	return std::nullopt;
}

QString EncodeV3Direct(const Metadata &meta) {
	if (meta.isEmpty()) {
		return QString();
	}
	const auto mask = (meta.emojiStatusDocId != 0 ? 0x01 : 0)
		| ((meta.emojiStatusDocId != 0 && meta.emojiStatusCollectible) ? 0x02 : 0)
		| (meta.nameColorId >= 0 ? 0x04 : 0)
		| (meta.nameEmojiId != 0 ? 0x08 : 0)
		| (meta.profileColorId >= 0 ? 0x10 : 0)
		| (meta.profileEmojiId != 0 ? 0x20 : 0)
		| (meta.qrBackdropId >= 0 ? 0x40 : 0)
		| (meta.qrEmojiId != 0 ? 0x80 : 0);
	if (mask == 0) {
		return QString();
	}

	auto data = std::vector<int>();
	data.reserve(74);
	data.push_back(kV3KindMetadata);
	data.push_back((mask >> 4) & 0xF);
	data.push_back(mask & 0xF);
	if (mask & 0x01) AppendU64Nibbles(data, uint64(meta.emojiStatusDocId));
	if (mask & 0x04) AppendIntSmallOrExt(data, meta.nameColorId);
	if (mask & 0x08) AppendU64Nibbles(data, uint64(meta.nameEmojiId));
	if (mask & 0x10) AppendIntSmallOrExt(data, meta.profileColorId);
	if (mask & 0x20) AppendU64Nibbles(data, uint64(meta.profileEmojiId));
	if (mask & 0x40) AppendIntSmallOrExt(data, meta.qrBackdropId);
	if (mask & 0x80) AppendU64Nibbles(data, uint64(meta.qrEmojiId));
	const auto crc = ComputeCrc4(data, data.size());

	QString out;
	out.reserve(qsizetype(data.size()) + 5);
	out.append(V3StartTag());
	for (const auto n : data) {
		out.append(NibbleToChar(n));
	}
	out.append(NibbleToChar(crc));
	out.append(V3EndTag());
	return out;
}

QString EncodeWithLimit(const Metadata &meta, int maxLen) {
	if (meta.isEmpty() || maxLen < 10) {
		return QString();
	}
	const auto full = EncodeV3Direct(meta);
	if (full.size() <= maxLen) {
		return full;
	}

	auto subset = Metadata();
	const auto tryAdd = [&](auto applyField, auto revertField) {
		applyField();
		if (EncodeV3Direct(subset).size() > maxLen) {
			revertField();
		}
	};

	if (meta.emojiStatusDocId != 0) {
		const auto prevDoc = subset.emojiStatusDocId;
		const auto prevColl = subset.emojiStatusCollectible;
		tryAdd(
			[&] {
				subset.emojiStatusDocId = meta.emojiStatusDocId;
				subset.emojiStatusCollectible = meta.emojiStatusCollectible;
			},
			[&] {
				subset.emojiStatusDocId = prevDoc;
				subset.emojiStatusCollectible = prevColl;
			});
	}
	if (meta.nameColorId >= 0) {
		const auto prev = subset.nameColorId;
		tryAdd(
			[&] { subset.nameColorId = meta.nameColorId; },
			[&] { subset.nameColorId = prev; });
	}
	if (meta.profileColorId >= 0) {
		const auto prev = subset.profileColorId;
		tryAdd(
			[&] { subset.profileColorId = meta.profileColorId; },
			[&] { subset.profileColorId = prev; });
	}
	if (meta.qrBackdropId >= 0) {
		const auto prev = subset.qrBackdropId;
		tryAdd(
			[&] { subset.qrBackdropId = meta.qrBackdropId; },
			[&] { subset.qrBackdropId = prev; });
	}
	if (meta.nameEmojiId != 0) {
		const auto prev = subset.nameEmojiId;
		tryAdd(
			[&] { subset.nameEmojiId = meta.nameEmojiId; },
			[&] { subset.nameEmojiId = prev; });
	}
	if (meta.profileEmojiId != 0) {
		const auto prev = subset.profileEmojiId;
		tryAdd(
			[&] { subset.profileEmojiId = meta.profileEmojiId; },
			[&] { subset.profileEmojiId = prev; });
	}
	if (meta.qrEmojiId != 0) {
		const auto prev = subset.qrEmojiId;
		tryAdd(
			[&] { subset.qrEmojiId = meta.qrEmojiId; },
			[&] { subset.qrEmojiId = prev; });
	}

	return subset.isEmpty() ? QString() : EncodeV3Direct(subset);
}

QString Encode(const Metadata &meta) {
	return EncodeWithLimit(meta, std::numeric_limits<int>::max());
}

QString EncodeEmojiStatusTag(DocumentId documentId, bool isCollectible) {
	if (!documentId) {
		return QString();
	}
	auto meta = Metadata();
	meta.emojiStatusDocId = documentId;
	meta.emojiStatusCollectible = isCollectible;
	return EncodeV3Direct(meta);
}

std::optional<Metadata> Decode(const QString &text) {
	if (text.isEmpty()) {
		return std::nullopt;
	}

	// 1. V3 binary tag
	{
		auto it = V3TagRegex().globalMatch(text);
		while (it.hasNext()) {
			const auto match = it.next();
			if (auto decoded = DecodeV3Payload(match.captured(1))) {
				return decoded;
			}
		}
	}

	// 2. Legacy V2 base-14 tag
	{
		const auto match = V2TagRegex().match(text);
		if (match.hasMatch()) {
			const auto payload = match.captured(1);
			auto meta = Metadata();
			const auto tokens = payload.split(QChar(kV2Delimiter));
			for (const auto &tok : tokens) {
				if (tok.size() < 2) {
					continue;
				}
				const auto type = tok[0].unicode();
				const auto num = DecodeBase14(tok.mid(1));
				switch (type) {
				case kV2KeyStatus:
					meta.emojiStatusDocId = DocumentId(num);
					meta.emojiStatusCollectible = false;
					break;
				case kV2KeyCollectibleStatus:
					meta.emojiStatusDocId = DocumentId(num);
					meta.emojiStatusCollectible = (num != 0);
					break;
				case kV2KeyNameColor:
					meta.nameColorId = int(int64(num) - 1);
					break;
				case kV2KeyNameEmoji:
					meta.nameEmojiId = DocumentId(num);
					break;
				case kV2KeyProfileColor:
					meta.profileColorId = int(int64(num) - 1);
					break;
				case kV2KeyProfileEmoji:
					meta.profileEmojiId = DocumentId(num);
					break;
				case kV2KeyQrBackdrop:
					meta.qrBackdropId = int(int64(num) - 1);
					break;
				case kV2KeyQrEmoji:
					meta.qrEmojiId = DocumentId(num);
					break;
				default:
					break;
				}
			}
			if (!meta.isEmpty()) {
				return meta;
			}
		}
	}

	// 3. Legacy V1 hex tag
	{
		const auto match = V1TagRegex().match(text);
		if (!match.hasMatch()) {
			return std::nullopt;
		}
		const auto payload = match.captured(1);
		auto meta = Metadata();
		const auto tokens = payload.split(QChar(kV1Delimiter));
		for (const auto &tok : tokens) {
			if (tok.size() < 2) {
				continue;
			}
			const auto type = tok[0].unicode();
			const auto num = DecodeV1Hex(tok.mid(1));
			switch (type) {
			case kV1KeyStatus:
				meta.emojiStatusDocId = DocumentId(num);
				break;
			case kV1KeyNameColor:
				meta.nameColorId = int(int64(num) - 1);
				break;
			case kV1KeyNameEmoji:
				meta.nameEmojiId = DocumentId(num);
				break;
			case kV1KeyProfileColor:
				meta.profileColorId = int(int64(num) - 1);
				break;
			case kV1KeyProfileEmoji:
				meta.profileEmojiId = DocumentId(num);
				break;
			case kV1KeyQrBackdrop:
				meta.qrBackdropId = int(int64(num) - 1);
				break;
			case kV1KeyQrEmoji:
				meta.qrEmojiId = DocumentId(num);
				break;
			default:
				break;
			}
		}
		if (!meta.isEmpty()) {
			return meta;
		}
	}

	return std::nullopt;
}

DocumentId DecodeEmojiId(const QString &text) {
	if (text.isEmpty()) {
		return 0;
	}
	if (const auto meta = Decode(text)) {
		if (meta->emojiStatusDocId) {
			return meta->emojiStatusDocId;
		}
	}
	// 1. Legacy Base-32 framed tag
	{
		auto it = FramedBase32Regex().globalMatch(text);
		while (it.hasNext()) {
			const auto payload = it.next().captured(1);
			auto value = uint64(0);
			auto valid = true;
			for (const auto ch : payload) {
				const auto idx = CharToBase32Index(ch);
				if (idx < 0) {
					valid = false;
					break;
				}
				value = value * 32ULL + uint64(idx);
			}
			if (valid && value != 0) {
				return DocumentId(value);
			}
		}
	}
	// 2. Legacy Base-16 VS sequence (length >= 8)
	{
		const auto match = LegacyVsRegex().match(text);
		if (match.hasMatch()) {
			const auto value = DecodeV1Hex(match.captured(0));
			if (value != 0) {
				return DocumentId(value);
			}
		}
	}
	// 3. tg://emoji?id=...
	{
		const auto match = TgEmojiRegex().match(text);
		if (match.hasMatch()) {
			auto ok = false;
			const auto value = match.captured(1).toULongLong(&ok);
			if (ok && value != 0) {
				return DocumentId(value);
			}
		}
	}
	return 0;
}

QString StripAllTags(const QString &text) {
	if (text.isEmpty()) {
		return QString();
	}
	auto cleaned = text;
	cleaned.remove(V3TagRegex());
	cleaned.remove(V2TagRegex());
	cleaned.remove(V1TagRegex());
	cleaned.remove(V3OrphanRegex());
	cleaned.remove(V2OrphanRegex());
	cleaned.remove(V1OrphanRegex());
	return cleaned;
}

QString CleanLastName(const QString &lastName) {
	if (lastName.isEmpty()) {
		return QString();
	}
	return TrimEnd(StripAllTags(lastName));
}

QString CleanBio(const QString &text) {
	if (text.isEmpty()) {
		return QString();
	}
	auto cleaned = StripAllTags(text);
	cleaned.remove(FramedBase32Regex());
	cleaned.remove(LegacyVsRegex());
	cleaned.remove(TgEmojiRegex());
	cleaned.remove(InvisibleCleanupRegex());
	return cleaned.trimmed();
}

QString CleanBioDisplay(const QString &text) {
	if (!CustomEmojiStatusEnabled() || !CleanBioDisplayEnabled()) {
		return text;
	}
	return CleanBio(text);
}

QString FormatLastName(
		const QString &visibleLastName,
		const std::optional<Metadata> &meta) {
	const auto base = CleanLastName(visibleLastName).left(kMaxLastNameLength);
	if (!meta || meta->isEmpty()) {
		return base;
	}
	const auto remaining = std::max(0, kMaxLastNameLength - int(base.size()));
	const auto tag = EncodeWithLimit(*meta, remaining);
	if (tag.isEmpty()) {
		return base;
	}
	return base.isEmpty() ? tag : (base + tag);
}

std::pair<QString, QString> PackProfileNamesWithTag(
		const QString &firstName,
		const QString &visibleLastName,
		const QString &tag) {
	const auto cleanFirst = firstName.trimmed().left(kMaxFirstNameLength);
	const auto cleanLast = CleanLastName(visibleLastName).left(kMaxLastNameLength);
	if (tag.isEmpty()) {
		return { cleanFirst, cleanLast };
	}
	const auto clippedTag = tag.left(kMaxLastNameLength);
	const auto maxLastVisible = std::max(
		0,
		kMaxLastNameLength - int(clippedTag.size()));

	if (cleanLast.size() <= maxLastVisible) {
		return {
			cleanFirst,
			cleanLast.isEmpty() ? clippedTag : (cleanLast + clippedTag),
		};
	}

	const auto combinedFirst = cleanFirst.isEmpty()
		? cleanLast
		: (cleanFirst + u' ' + cleanLast);
	if (combinedFirst.size() <= kMaxFirstNameLength) {
		return { combinedFirst, clippedTag };
	}

	const auto newFirst = TrimEnd(combinedFirst.left(kMaxFirstNameLength));
	const auto overflowLast = TrimStart(
		combinedFirst.mid(newFirst.size())).left(maxLastVisible);
	return {
		newFirst,
		overflowLast.isEmpty() ? clippedTag : (overflowLast + clippedTag),
	};
}

std::pair<QString, QString> PackProfileNamesForEdit(
		not_null<UserData*> user,
		const QString &newFirst,
		const QString &newLast) {
	const auto selfMeta = GetSelfMetadataSnapshot(user);
	const auto tag = ComputeDesiredLastNameTag(user, selfMeta);
	return PackProfileNamesWithTag(newFirst, newLast, tag);
}

void RecordRawLastName(not_null<UserData*> user, const QString &rawLastName) {
	const auto uid = peerToUser(user->id);
	const auto it = RawLastNames().find(uid);
	if (it != RawLastNames().end() && it->second == rawLastName) {
		return;
	}
	RawLastNames()[uid] = rawLastName;
	DirectMetaCache().remove(uid);
}

QString RawLastName(not_null<const UserData*> user) {
	const auto uid = peerToUser(user->id);
	const auto it = RawLastNames().find(uid);
	return (it != RawLastNames().end()) ? it->second : user->lastName;
}

void InvalidateUser(UserId userId) {
	DirectMetaCache().remove(userId);
}

std::optional<Metadata> GetMetadata(not_null<UserData*> user) {
	const auto uid = peerToUser(user->id);
	const auto rawLast = RawLastName(user);
	const auto botSeed = DecodeBotRef(rawLast);

	if (botSeed.has_value()) {
		std::optional<Metadata> cachedBotMeta;
		if (user->isSelf()) {
			const auto selfMeta = LoadSelfMetadata(user);
			if (!selfMeta.isEmpty()) {
				cachedBotMeta = selfMeta;
			}
		} else {
			cachedBotMeta = MoltenBotSync::GetCachedPeerBotMetadata(
				uid.bare,
				*botSeed);
			MoltenBotSync::FetchPeerBotMetadataIfNeeded(
				&user->session(),
				uid,
				*botSeed);
		}
		return cachedBotMeta;
	}

	const auto it = DirectMetaCache().find(uid);
	if (it != DirectMetaCache().end() && it->second.rawLastName == rawLast) {
		return it->second.meta;
	}
	const auto decoded = Decode(rawLast);
	DirectMetaCache()[uid] = CachedDirectMeta{ rawLast, decoded };
	return decoded;
}

void ApplyMetadata(
		not_null<UserData*> user,
		bool hasNativeEmojiStatus,
		bool hasNativeNameColor,
		bool hasNativeProfileColor,
		bool nativeFlagsKnown) {
	if (!CustomEmojiStatusEnabled() && !FreeProfileAppearanceEnabled()) {
		return;
	}
	const auto meta = GetMetadata(user);
	ApplyMetaToUserInternal(
		user,
		meta,
		hasNativeEmojiStatus,
		hasNativeNameColor,
		hasNativeProfileColor,
		nativeFlagsKnown);
}

Metadata GetSelfMetadataSnapshot(not_null<UserData*> self) {
	return LoadSelfMetadata(self);
}

void UpdateSelfEmojiStatus(
		not_null<Main::Session*> session,
		DocumentId documentId,
		bool isCollectible,
		Fn<void(bool)> callback) {
	const auto self = session->user();
	if (self->isPremiumReal()) {
		if (callback) {
			callback(false);
		}
		return;
	}
	auto currentMeta = LoadSelfMetadata(self);
	currentMeta.emojiStatusDocId = documentId;
	currentMeta.emojiStatusCollectible = (documentId != 0) && isCollectible;
	SaveSelfMetadata(session, currentMeta, std::move(callback));
}

void ClearSelfEmojiStatus(
		not_null<Main::Session*> session,
		Fn<void(bool)> callback) {
	UpdateSelfEmojiStatus(session, 0, false, std::move(callback));
}

void UpdateSelfColors(
		not_null<Main::Session*> session,
		int nameColorId,
		DocumentId nameEmojiId,
		int profileColorId,
		DocumentId profileEmojiId,
		Fn<void(bool)> callback) {
	const auto self = session->user();
	auto currentMeta = LoadSelfMetadata(self);
	currentMeta.nameColorId = nameColorId;
	currentMeta.nameEmojiId = nameEmojiId;
	currentMeta.profileColorId = profileColorId;
	currentMeta.profileEmojiId = profileEmojiId;
	SaveSelfMetadata(session, currentMeta, std::move(callback));
}

void UpdateSelfMetadata(
		not_null<Main::Session*> session,
		const Metadata &meta,
		Fn<void(bool)> callback) {
	SaveSelfMetadata(session, meta, std::move(callback));
}

void ResyncSelfMetadata(
		not_null<Main::Session*> session,
		Fn<void(bool)> callback) {
	const auto self = session->user();
	const auto currentMeta = LoadSelfMetadata(self);
	SaveSelfMetadata(session, currentMeta, std::move(callback));
}

} // namespace Extera::MoltenProfile
