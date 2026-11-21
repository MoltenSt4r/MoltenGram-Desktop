#pragma once

#include "base/basic_types.h"
#include "data/data_types.h"
#include "extera/molten_profile_metadata.h"

#include <QtCore/QString>
#include <optional>

class UserData;

namespace Main {
class Session;
} // namespace Main

namespace Window {
class SessionController;
} // namespace Window

namespace Extera::MoltenBotSync {

enum class ErrorType {
	None,
	NotAuthorized,
	AlreadyInProgress,
	BotLimitExceeded,
	SpamBlock,
	BotFatherBlocked,
	FloodWait,
	BotFatherRejected,
	Timeout,
	NetworkError,
};

struct BotSetupResult {
	bool success = false;
	UserData *botUser = nullptr;
	QString botUsername;
	int seed = 0;
	ErrorType errorType = ErrorType::None;
	QString rawDetail;

	[[nodiscard]] QString formatUserMessage() const;
};

[[nodiscard]] QString BuildBotUsername(uint64 ownerUserId, int seed);
[[nodiscard]] std::optional<int> ParseSeedFromUsername(
	uint64 ownerUserId,
	const QString &username);
[[nodiscard]] QString BuildBotBioPayload(
	const std::optional<MoltenProfile::Metadata> &meta);

[[nodiscard]] std::optional<int> GetSavedSelfBotSeed(uint64 ownerUserId);
[[nodiscard]] QString GetSavedSelfBotUsername(uint64 ownerUserId);
[[nodiscard]] uint64 GetSavedSelfBotId(uint64 ownerUserId);

[[nodiscard]] std::optional<MoltenProfile::Metadata> GetCachedPeerBotMetadata(
	uint64 ownerUserId,
	int expectedSeed);

void FetchPeerBotMetadataIfNeeded(
	not_null<Main::Session*> session,
	UserId ownerUserId,
	int seed);

void SyncMetadataToBot(
	not_null<Main::Session*> session,
	const MoltenProfile::Metadata &meta,
	Fn<void(bool)> callback = nullptr);

void EnsureBotReady(
	not_null<Main::Session*> session,
	bool allowInteractiveBotFather,
	Fn<void(BotSetupResult)> callback);

// Settings dialog for Bot-Based Profile Styling & Free Custom Emoji Status.
void ShowProfileStylingBox(not_null<Window::SessionController*> controller);

} // namespace Extera::MoltenBotSync
