#pragma once

#include "base/basic_types.h"
#include "data/data_types.h"

#include <QtCore/QString>
#include <optional>
#include <utility>

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Extera::MoltenProfile {

inline constexpr auto kMaxLastNameLength = 64;
inline constexpr auto kMaxFirstNameLength = 64;

struct Metadata {
	DocumentId emojiStatusDocId = 0;
	int nameColorId = -1;
	DocumentId nameEmojiId = 0;
	int profileColorId = -1;
	DocumentId profileEmojiId = 0;
	int qrBackdropId = -1;
	DocumentId qrEmojiId = 0;
	bool emojiStatusCollectible = false;

	[[nodiscard]] bool isEmpty() const {
		return emojiStatusDocId == 0
			&& nameColorId < 0
			&& nameEmojiId == 0
			&& profileColorId < 0
			&& profileEmojiId == 0
			&& qrBackdropId < 0
			&& qrEmojiId == 0;
	}

	friend inline bool operator==(
		const Metadata &,
		const Metadata &) = default;
};

// Feature config accessors (safe defaults even before extera_config glue).
[[nodiscard]] bool CustomEmojiStatusEnabled();
[[nodiscard]] bool FreeProfileAppearanceEnabled();
[[nodiscard]] bool BotProfileSyncEnabled();
[[nodiscard]] bool CleanBioDisplayEnabled();

// Wire-compatible V3 / V2 / V1 Variation Selectors codec (100% compatible with Android MoltenGram).
[[nodiscard]] QString EncodeBotRef(int seed);
[[nodiscard]] std::optional<int> DecodeBotRef(const QString &text);

[[nodiscard]] QString EncodeV3Direct(const Metadata &meta);
[[nodiscard]] QString EncodeWithLimit(const Metadata &meta, int maxLen);
[[nodiscard]] QString Encode(const Metadata &meta);
[[nodiscard]] QString EncodeEmojiStatusTag(
	DocumentId documentId,
	bool isCollectible = false);

[[nodiscard]] std::optional<Metadata> Decode(const QString &text);
[[nodiscard]] DocumentId DecodeEmojiId(const QString &text);

[[nodiscard]] QString StripAllTags(const QString &text);
[[nodiscard]] QString CleanLastName(const QString &lastName);
[[nodiscard]] QString CleanBio(const QString &text);
[[nodiscard]] QString CleanBioDisplay(const QString &text);

[[nodiscard]] QString FormatLastName(
	const QString &visibleLastName,
	const std::optional<Metadata> &meta);

[[nodiscard]] std::pair<QString, QString> PackProfileNamesWithTag(
	const QString &firstName,
	const QString &visibleLastName,
	const QString &tag);

[[nodiscard]] std::pair<QString, QString> PackProfileNamesForEdit(
	not_null<UserData*> user,
	const QString &newFirst,
	const QString &newLast);

// Raw last_name tracking (called from UserData::setName before stripping).
void RecordRawLastName(not_null<UserData*> user, const QString &rawLastName);
[[nodiscard]] QString RawLastName(not_null<const UserData*> user);
void InvalidateUser(UserId userId);

// Decode + trigger bot resolution if needed + apply to UserData when native fields are empty.
[[nodiscard]] std::optional<Metadata> GetMetadata(not_null<UserData*> user);
void ApplyMetadata(
	not_null<UserData*> user,
	bool hasNativeEmojiStatus = false,
	bool hasNativeNameColor = false,
	bool hasNativeProfileColor = false,
	bool nativeFlagsKnown = false);

[[nodiscard]] Metadata GetSelfMetadataSnapshot(not_null<UserData*> self);

void UpdateSelfEmojiStatus(
	not_null<Main::Session*> session,
	DocumentId documentId,
	bool isCollectible = false,
	Fn<void(bool)> callback = nullptr);

void ClearSelfEmojiStatus(
	not_null<Main::Session*> session,
	Fn<void(bool)> callback = nullptr);

void UpdateSelfColors(
	not_null<Main::Session*> session,
	int nameColorId,
	DocumentId nameEmojiId,
	int profileColorId,
	DocumentId profileEmojiId,
	Fn<void(bool)> callback = nullptr);

void UpdateSelfMetadata(
	not_null<Main::Session*> session,
	const Metadata &meta,
	Fn<void(bool)> callback = nullptr);

void ResyncSelfMetadata(
	not_null<Main::Session*> session,
	Fn<void(bool)> callback = nullptr);

} // namespace Extera::MoltenProfile
