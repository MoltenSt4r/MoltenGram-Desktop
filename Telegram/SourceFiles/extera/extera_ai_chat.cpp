#include "extera/extera_ai_chat.h"

#include "base/unixtime.h"
#include "extera/exd_ai_chat_section.h"
#include "extera/extera_config.h"
#include "lang/lang_keys.h"
#include "settings.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "ui/layers/generic_box.h"
#include "ui/toast/toast.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/scroll_area.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Extera {
namespace {

const auto kGeminiModels = std::array{
	u"gemini-2.5-flash"_q,
	u"gemini-2.5-pro"_q,
	u"gemini-2.5-flash-lite"_q,
};

struct Message {
	bool user = false;
	bool error = false;
	QString text;
	TimeId date = 0;
};

[[nodiscard]] QString DefaultBaseUrl(int provider) {
	switch (provider) {
	case 1: return u"https://api.openai.com/v1"_q;
	case 2: return u"https://openrouter.ai/api/v1"_q;
	case 3: return u"https://api.deepseek.com/v1"_q;
	case 4: return u"http://localhost:11434/v1"_q;
	case 5: return u"https://api.openai.com/v1"_q;
	default: return u"https://generativelanguage.googleapis.com"_q;
	}
}

[[nodiscard]] QString EffectiveBaseUrl() {
	const auto p = Int(u"ai_chat_provider"_q);
	const auto custom = String(u"ai_chat_base_url"_q).trimmed();
	if (!custom.isEmpty()) {
		auto url = custom;
		while (url.endsWith('/')) {
			url.chop(1);
		}
		return url;
	}
	return DefaultBaseUrl(p);
}

[[nodiscard]] QString ModelName() {
	const auto custom = String(u"ai_chat_custom_model"_q).trimmed();
	if (!custom.isEmpty()) {
		return custom;
	}
	const auto provider = Int(u"ai_chat_provider"_q);
	switch (provider) {
	case 1: return u"gpt-4o"_q;
	case 2: return u"openrouter/auto"_q;
	case 3: return u"deepseek-chat"_q;
	case 4: return u"llama3.3"_q;
	case 5: return u"gpt-4o"_q;
	default: {
		const auto index = std::clamp(
			Int(u"ai_chat_model"_q),
			0,
			int(kGeminiModels.size()) - 1);
		return kGeminiModels[index];
	}
	}
}

[[nodiscard]] QString RolePrompt() {
	switch (Int(u"ai_chat_role"_q)) {
	case 1: return u"You are a translator. Translate the user's text into Russian if it is in another language, otherwise into English. Reply with the translation only."_q;
	case 2: return u"You are an editor. Fix spelling, grammar and style of the user's text and reply with the corrected text only."_q;
	case 3: return u"You summarize the user's text in a few short sentences."_q;
	}
	return u"You are a helpful assistant."_q;
}

[[nodiscard]] QString EffectiveSystemPrompt() {
	const auto customPrompt = String(u"ai_chat_system_prompt"_q).trimmed();
	const auto persona = Int(u"ai_chat_persona"_q);
	switch (persona) {
	case 1:
		return u"You are an expert multilingual translator. Translate the user's input accurately, fluently, and naturally. If input is not in English, translate to English; if in English, translate to Russian. Reply with the translation only without any conversational padding."_q;
	case 2:
		return u"You are a master copy editor and software engineer. Review the user's text or code for grammar, style, bugs, and performance. Output the corrected text or code first, followed by a concise bulleted explanation of improvements."_q;
	case 3:
		return u"You are an executive summarizer. Condense the user's input into key bullet points and a one-sentence takeaway."_q;
	case 4:
		return u"You are a Principal Software Architect and Senior Systems Engineer. Write clean, idiomatic, and highly performant code with thoughtful handling of edge cases and architecture decisions."_q;
	case 5:
		return u"You are an imaginative and eloquent creative writer. Compose vivid, engaging, and evocative responses with rich vocabulary."_q;
	case 6:
		return customPrompt.isEmpty() ? u"You are a helpful assistant."_q : customPrompt;
	default:
		if (!customPrompt.isEmpty()) {
			return customPrompt;
		}
		return RolePrompt();
	}
}

[[nodiscard]] QString ReasoningEffortString() {
	switch (Int(u"ai_chat_reasoning_effort"_q)) {
	case 1: return u"minimal"_q;
	case 2: return u"low"_q;
	case 3: return u"medium"_q;
	case 4: return u"high"_q;
	default: return QString();
	}
}

[[nodiscard]] int HistoryLimit() {
	switch (Int(u"ai_chat_history"_q)) {
	case 0: return 1;
	case 1: return 10;
	case 2: return 30;
	}
	return 1000;
}

[[nodiscard]] QString ErrorFrom(const QByteArray &data) {
	const auto document = QJsonDocument::fromJson(data);
	const auto object = document.isArray()
		? document.array().first().toObject()
		: document.object();
	const auto errorObj = object.value(u"error"_q);
	if (errorObj.isObject()) {
		const auto message = errorObj.toObject().value(u"message"_q).toString();
		if (!message.isEmpty()) {
			return message;
		}
	} else if (errorObj.isString()) {
		const auto msg = errorObj.toString();
		if (!msg.isEmpty()) {
			return msg;
		}
	}
	const auto message = object.value(u"message"_q).toString();
	if (!message.isEmpty()) {
		return message;
	}
	const auto blocked = object.value(u"promptFeedback"_q).toObject()
		.value(u"blockReason"_q).toString();
	if (!blocked.isEmpty()) {
		return u"Blocked: "_q + blocked;
	}
	const auto candidates = object.value(u"candidates"_q).toArray();
	if (!candidates.isEmpty()) {
		const auto reason = candidates[0].toObject()
			.value(u"finishReason"_q).toString();
		if (!reason.isEmpty() && reason != u"STOP"_q) {
			return u"Finish reason: "_q + reason;
		}
	}
	return QString();
}

struct State {
	std::vector<Message> history;
	QNetworkAccessManager *manager = nullptr;
	QString partial;
	bool waiting = false;
};

State &Chat() {
	static auto result = State();
	return result;
}

void RequestGemini(
		Fn<void(QString answer, bool ok)> done,
		Fn<void(QString partial)> progress) {
	auto &chat = Chat();
	auto contents = QJsonArray();
	auto messages = std::vector<Message>();
	for (const auto &message : chat.history) {
		if (!message.error) {
			messages.push_back(message);
		}
	}
	const auto limit = HistoryLimit();
	auto skip = std::max(int(messages.size()) - limit, 0);
	while (skip < int(messages.size()) && !messages[skip].user) {
		++skip;
	}
	auto index = 0;
	for (const auto &message : messages) {
		if (index++ < skip) {
			continue;
		}
		contents.push_back(QJsonObject{
			{ u"role"_q, message.user ? u"user"_q : u"model"_q },
			{ u"parts"_q, QJsonArray{ QJsonObject{
				{ u"text"_q, message.text },
			} } },
		});
	}
	const auto streaming = Bool(u"ai_chat_streaming"_q);
	auto request = QNetworkRequest(QUrl(
		u"https://generativelanguage.googleapis.com/v1beta/models/"_q
		+ ModelName()
		+ (streaming
			? u":streamGenerateContent?alt=sse"_q
			: u":generateContent"_q)));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setRawHeader(
		"x-goog-api-key",
		String(u"ai_chat_key"_q).trimmed().toUtf8());

	auto genConfig = QJsonObject{
		{ u"temperature"_q, Int(u"ai_chat_temperature"_q) / 10. },
	};
	const auto effort = ReasoningEffortString();
	if (!effort.isEmpty()) {
		genConfig.insert(
			u"thinkingConfig"_q,
			QJsonObject{ { u"thinkingBudget"_q, (effort == u"high"_q ? 1024 : 256) } });
	}

	const auto body = QJsonDocument(QJsonObject{
		{ u"contents"_q, contents },
		{ u"systemInstruction"_q, QJsonObject{
			{ u"parts"_q, QJsonArray{ QJsonObject{
				{ u"text"_q, EffectiveSystemPrompt() },
			} } },
		} },
		{ u"generationConfig"_q, genConfig },
	}).toJson(QJsonDocument::Compact);

	const auto reply = chat.manager->post(request, body);
	if (streaming) {
		const auto buffer = std::make_shared<QByteArray>();
		const auto text = std::make_shared<QString>();
		const auto error = std::make_shared<QString>();
		const auto parse = [=] {
			while (true) {
				const auto end = buffer->indexOf('\n');
				if (end < 0) {
					break;
				}
				const auto line = buffer->left(end).trimmed();
				buffer->remove(0, end + 1);
				if (!line.startsWith("data:")) {
					continue;
				}
				const auto chunk = line.mid(5).trimmed();
				if (chunk == "[DONE]") {
					break;
				}
				const auto object = QJsonDocument::fromJson(chunk).object();
				const auto message = ErrorFrom(chunk);
				if (!message.isEmpty()) {
					*error = message;
				}
				const auto candidates = object.value(
					u"candidates"_q).toArray();
				if (candidates.isEmpty()) {
					continue;
				}
				const auto parts = candidates[0].toObject()
					.value(u"content"_q).toObject()
					.value(u"parts"_q).toArray();
				for (const auto &part : parts) {
					*text += part.toObject().value(u"text"_q).toString();
				}
				progress(*text);
			}
		};
		QObject::connect(reply, &QNetworkReply::readyRead, [=] {
			buffer->append(reply->readAll());
			parse();
		});
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			reply->deleteLater();
			buffer->append(reply->readAll());
			buffer->append('\n');
			parse();
			if (!text->isEmpty()) {
				done(text->trimmed(), true);
			} else if (!error->isEmpty()) {
				done(*error, false);
			} else {
				const auto message = ErrorFrom(*buffer);
				done(message.isEmpty() ? reply->errorString() : message, false);
			}
		});
		return;
	}
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto data = reply->readAll();
		const auto object = QJsonDocument::fromJson(data).object();
		const auto candidates = object.value(u"candidates"_q).toArray();
		auto text = QString();
		if (!candidates.isEmpty()) {
			const auto parts = candidates[0].toObject()
				.value(u"content"_q).toObject()
				.value(u"parts"_q).toArray();
			for (const auto &part : parts) {
				text += part.toObject().value(u"text"_q).toString();
			}
		}
		if (!text.isEmpty()) {
			done(text.trimmed(), true);
			return;
		}
		const auto error = ErrorFrom(data);
		done(error.isEmpty() ? reply->errorString() : error, false);
	});
}

void RequestOpenAICompat(
		Fn<void(QString answer, bool ok)> done,
		Fn<void(QString partial)> progress) {
	auto &chat = Chat();
	const auto baseUrl = EffectiveBaseUrl();
	const auto targetUrl = baseUrl.endsWith(u"/chat/completions"_q)
		? baseUrl
		: (baseUrl + u"/chat/completions"_q);

	auto request = QNetworkRequest(QUrl(targetUrl));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);

	const auto key = String(u"ai_chat_key"_q).trimmed();
	if (!key.isEmpty()) {
		request.setRawHeader("Authorization", ("Bearer " + key).toUtf8());
	}
	if (baseUrl.contains(u"openrouter.ai"_q)) {
		request.setRawHeader("HTTP-Referer", "https://github.com/MoltenSt4r/MoltenGram-Desktop");
		request.setRawHeader("X-Title", "MoltenGram Desktop");
	}

	auto messagesJson = QJsonArray();
	const auto systemPrompt = EffectiveSystemPrompt();
	if (!systemPrompt.isEmpty()) {
		messagesJson.push_back(QJsonObject{
			{ u"role"_q, u"system"_q },
			{ u"content"_q, systemPrompt },
		});
	}

	auto historyMessages = std::vector<Message>();
	for (const auto &message : chat.history) {
		if (!message.error) {
			historyMessages.push_back(message);
		}
	}
	const auto limit = HistoryLimit();
	auto skip = std::max(int(historyMessages.size()) - limit, 0);
	while (skip < int(historyMessages.size()) && !historyMessages[skip].user) {
		++skip;
	}
	auto index = 0;
	for (const auto &message : historyMessages) {
		if (index++ < skip) {
			continue;
		}
		messagesJson.push_back(QJsonObject{
			{ u"role"_q, message.user ? u"user"_q : u"assistant"_q },
			{ u"content"_q, message.text },
		});
	}

	const auto streaming = Bool(u"ai_chat_streaming"_q);
	auto root = QJsonObject{
		{ u"model"_q, ModelName() },
		{ u"messages"_q, messagesJson },
		{ u"temperature"_q, Int(u"ai_chat_temperature"_q) / 10. },
		{ u"stream"_q, streaming },
	};

	const auto effort = ReasoningEffortString();
	if (!effort.isEmpty()) {
		root.insert(u"reasoning_effort"_q, effort);
		if (baseUrl.contains(u"openrouter.ai"_q)) {
			root.insert(u"reasoning"_q, QJsonObject{ { u"effort"_q, effort } });
		}
	}

	const auto body = QJsonDocument(root).toJson(QJsonDocument::Compact);
	const auto reply = chat.manager->post(request, body);

	if (streaming) {
		const auto buffer = std::make_shared<QByteArray>();
		const auto text = std::make_shared<QString>();
		const auto error = std::make_shared<QString>();
		const auto parse = [=] {
			while (true) {
				const auto end = buffer->indexOf('\n');
				if (end < 0) {
					break;
				}
				const auto line = buffer->left(end).trimmed();
				buffer->remove(0, end + 1);
				if (!line.startsWith("data:")) {
					continue;
				}
				const auto chunk = line.mid(5).trimmed();
				if (chunk == "[DONE]") {
					break;
				}
				const auto doc = QJsonDocument::fromJson(chunk);
				if (!doc.isObject()) {
					continue;
				}
				const auto obj = doc.object();
				const auto errMsg = ErrorFrom(chunk);
				if (!errMsg.isEmpty()) {
					*error = errMsg;
				}
				const auto choices = obj.value(u"choices"_q).toArray();
				if (choices.isEmpty()) {
					continue;
				}
				const auto choice0 = choices[0].toObject();
				const auto delta = choice0.value(u"delta"_q).toObject();
				auto content = delta.value(u"content"_q).toString();
				if (content.isEmpty()) {
					content = delta.value(u"reasoning_content"_q).toString();
				}
				if (content.isEmpty()) {
					content = choice0.value(u"text"_q).toString();
				}
				if (!content.isEmpty()) {
					*text += content;
					progress(*text);
				}
			}
		};
		QObject::connect(reply, &QNetworkReply::readyRead, [=] {
			buffer->append(reply->readAll());
			parse();
		});
		QObject::connect(reply, &QNetworkReply::finished, [=] {
			reply->deleteLater();
			buffer->append(reply->readAll());
			buffer->append('\n');
			parse();
			if (!text->isEmpty()) {
				done(text->trimmed(), true);
			} else if (!error->isEmpty()) {
				done(*error, false);
			} else {
				const auto message = ErrorFrom(*buffer);
				done(message.isEmpty() ? reply->errorString() : message, false);
			}
		});
		return;
	}

	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto bytes = reply->readAll();
		const auto doc = QJsonDocument::fromJson(bytes);
		if (doc.isObject()) {
			const auto obj = doc.object();
			const auto choices = obj.value(u"choices"_q).toArray();
			if (!choices.isEmpty()) {
				const auto msg = choices[0].toObject().value(u"message"_q).toObject();
				auto content = msg.value(u"content"_q).toString().trimmed();
				if (content.isEmpty()) {
					content = msg.value(u"reasoning_content"_q).toString().trimmed();
				}
				if (!content.isEmpty()) {
					done(content, true);
					return;
				}
			}
		}
		const auto err = ErrorFrom(bytes);
		done(err.isEmpty() ? reply->errorString() : err, false);
	});
}

void Request(
		Fn<void(QString answer, bool ok)> done,
		Fn<void(QString partial)> progress) {
	auto &chat = Chat();
	if (!chat.manager) {
		chat.manager = new QNetworkAccessManager(qApp);
	}
	const auto provider = Int(u"ai_chat_provider"_q);
	const auto baseUrl = EffectiveBaseUrl();
	if (provider == 0 && baseUrl.contains(u"generativelanguage.googleapis.com"_q) && !baseUrl.contains(u"/openai"_q)) {
		RequestGemini(std::move(done), std::move(progress));
	} else {
		RequestOpenAICompat(std::move(done), std::move(progress));
	}
}

[[nodiscard]] rpl::event_stream<> &Updates() {
	static auto result = rpl::event_stream<>();
	return result;
}

[[nodiscard]] QString HistoryPath() {
	return cWorkingDir() + u"tdata/exd_ai_chat.json"_q;
}

void SaveHistory() {
	auto list = QJsonArray();
	for (const auto &message : Chat().history) {
		list.push_back(QJsonObject{
			{ u"user"_q, message.user },
			{ u"error"_q, message.error },
			{ u"text"_q, message.text },
			{ u"date"_q, qint64(message.date) },
		});
	}
	auto file = QFile(HistoryPath());
	if (file.open(QIODevice::WriteOnly)) {
		file.write(QJsonDocument(list).toJson(QJsonDocument::Compact));
	}
}

void LoadHistory() {
	static auto loaded = false;
	if (std::exchange(loaded, true)) {
		return;
	}
	auto file = QFile(HistoryPath());
	if (!file.open(QIODevice::ReadOnly)) {
		return;
	}
	for (const auto &entry : QJsonDocument::fromJson(file.readAll()).array()) {
		const auto object = entry.toObject();
		Chat().history.push_back({
			.user = object.value(u"user"_q).toBool(),
			.error = object.value(u"error"_q).toBool(),
			.text = object.value(u"text"_q).toString(),
			.date = TimeId(object.value(u"date"_q).toInteger()),
		});
	}
}

} // namespace

void ShowAiSettingsBox(not_null<Window::SessionController*> controller) {
	controller->show(Box([=](not_null<Ui::GenericBox*> box) {
		box->setTitle(rpl::single(u"AI Assistant Settings"_q));
		box->setWidth(st::boxWideWidth);
		box->addTopButton(st::boxTitleClose, [=] { box->closeBox(); });

		const auto container = box->verticalLayout();

		Ui::AddSubsectionTitle(container, rpl::single(u"Provider & Endpoint"_q));
		Ui::AddSkip(container, 8);

		const auto providerNames = std::vector<QString>{
			u"Google Gemini (AI Studio)"_q,
			u"OpenAI (api.openai.com)"_q,
			u"OpenRouter (openrouter.ai)"_q,
			u"DeepSeek (api.deepseek.com)"_q,
			u"Ollama / Local LLM (localhost:11434)"_q,
			u"Custom OpenAI-Compatible Endpoint"_q,
		};
		const auto currentProvider = std::clamp(Int(u"ai_chat_provider"_q), 0, int(providerNames.size()) - 1);
		const auto providerGroup = std::make_shared<Ui::RadiobuttonGroup>(currentProvider);

		for (int i = 0; i < int(providerNames.size()); ++i) {
			container->add(object_ptr<Ui::Radiobutton>(
				container,
				providerGroup,
				i,
				providerNames[i]));
			Ui::AddSkip(container, 6);
		}

		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Authentication & URL"_q));
		Ui::AddSkip(container, 8);

		const auto keyField = container->add(object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			rpl::single(u"API Key (Optional for Ollama)"_q),
			String(u"ai_chat_key"_q)));

		Ui::AddSkip(container, 8);
		const auto urlField = container->add(object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			rpl::single(u"Base URL (leave empty for provider default)"_q),
			String(u"ai_chat_base_url"_q)));

		Ui::AddSkip(container, 8);
		const auto modelField = container->add(object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			rpl::single(u"Model ID (e.g. gpt-4o / deepseek-chat / llama3.3)"_q),
			String(u"ai_chat_custom_model"_q)));

		providerGroup->setChangedCallback([=](int val) {
			if (urlField->getLastText().trimmed().isEmpty()) {
				urlField->setPlaceholder(rpl::single(DefaultBaseUrl(val)));
			}
		});

		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Persona & System Prompt"_q));
		Ui::AddSkip(container, 8);

		const auto personaNames = std::vector<QString>{
			u"Helpful Assistant (General)"_q,
			u"Multilingual Translator"_q,
			u"Code & Grammar Editor"_q,
			u"Executive Summarizer"_q,
			u"Senior Software Engineer"_q,
			u"Creative Writer"_q,
			u"Custom Persona / Instructions"_q,
		};
		const auto currentPersona = std::clamp(Int(u"ai_chat_persona"_q), 0, int(personaNames.size()) - 1);
		const auto personaGroup = std::make_shared<Ui::RadiobuttonGroup>(currentPersona);

		for (int i = 0; i < int(personaNames.size()); ++i) {
			container->add(object_ptr<Ui::Radiobutton>(
				container,
				personaGroup,
				i,
				personaNames[i]));
			Ui::AddSkip(container, 6);
		}

		Ui::AddSkip(container, 8);
		const auto sysPromptField = container->add(object_ptr<Ui::InputField>(
			container,
			st::defaultInputField,
			Ui::InputField::Mode::MultiLine,
			rpl::single(u"Custom System Prompt / Instructions"_q),
			String(u"ai_chat_system_prompt"_q)));

		Ui::AddSkip(container, 16);
		Ui::AddSubsectionTitle(container, rpl::single(u"Reasoning & Parameters"_q));
		Ui::AddSkip(container, 8);

		const auto effortNames = std::vector<QString>{
			u"Default / None"_q,
			u"Minimal Reasoning"_q,
			u"Low Reasoning"_q,
			u"Medium Reasoning"_q,
			u"High Reasoning"_q,
		};
		const auto currentEffort = std::clamp(Int(u"ai_chat_reasoning_effort"_q), 0, int(effortNames.size()) - 1);
		const auto effortGroup = std::make_shared<Ui::RadiobuttonGroup>(currentEffort);

		for (int i = 0; i < int(effortNames.size()); ++i) {
			container->add(object_ptr<Ui::Radiobutton>(
				container,
				effortGroup,
				i,
				effortNames[i]));
			Ui::AddSkip(container, 6);
		}

		Ui::AddSkip(container, 12);
		const auto streamCheckbox = container->add(object_ptr<Ui::Checkbox>(
			container,
			u"Enable Streaming Responses"_q,
			Bool(u"ai_chat_streaming"_q),
			st::defaultCheckbox));

		box->addButton(rpl::single(u"Save & Apply"_q), [=] {
			Config::Instance().setInt(u"ai_chat_provider"_q, providerGroup->current());
			Config::Instance().setString(u"ai_chat_key"_q, keyField->getLastText().trimmed());
			Config::Instance().setString(u"ai_chat_base_url"_q, urlField->getLastText().trimmed());
			Config::Instance().setString(u"ai_chat_custom_model"_q, modelField->getLastText().trimmed());
			Config::Instance().setInt(u"ai_chat_persona"_q, personaGroup->current());
			Config::Instance().setString(u"ai_chat_system_prompt"_q, sysPromptField->getLastText().trimmed());
			Config::Instance().setInt(u"ai_chat_reasoning_effort"_q, effortGroup->current());
			Config::Instance().setBool(u"ai_chat_streaming"_q, streamCheckbox->checked());
			Config::Instance().saveNow();
			Updates().fire({});
			controller->showToast(u"AI settings saved."_q);
			box->closeBox();
		});

		box->addButton(tr::lng_cancel(), [=] {
			box->closeBox();
		});
	}));
}

void ShowKeyBox(not_null<Window::SessionController*> controller) {
	ShowAiSettingsBox(controller);
}

bool CanTranscribe() {
	return (Int(u"transcribe_provider"_q) == 1)
		&& !String(u"ai_chat_key"_q).trimmed().isEmpty();
}

void Transcribe(
		const QByteArray &data,
		const QString &mime,
		Fn<void(QString text, bool ok)> done) {
	auto &chat = Chat();
	if (!chat.manager) {
		chat.manager = new QNetworkAccessManager(qApp);
	}
	auto request = QNetworkRequest(QUrl(
		u"https://generativelanguage.googleapis.com/v1beta/models/"_q
		+ ModelName()
		+ u":generateContent"_q));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setRawHeader(
		"x-goog-api-key",
		String(u"ai_chat_key"_q).trimmed().toUtf8());
	const auto body = QJsonDocument(QJsonObject{
		{ u"contents"_q, QJsonArray{ QJsonObject{
			{ u"role"_q, u"user"_q },
			{ u"parts"_q, QJsonArray{
				QJsonObject{
					{ u"inline_data"_q, QJsonObject{
						{ u"mime_type"_q, mime },
						{ u"data"_q, QString::fromLatin1(data.toBase64()) },
					} },
				},
				QJsonObject{
					{ u"text"_q, u"Transcribe the speech in this recording "
						"verbatim in its original language. Return only the "
						"transcription text without comments."_q },
				},
			} },
		} } },
		{ u"generationConfig"_q, QJsonObject{
			{ u"temperature"_q, 0. },
		} },
	}).toJson(QJsonDocument::Compact);
	const auto reply = chat.manager->post(request, body);
	QObject::connect(reply, &QNetworkReply::finished, [=] {
		reply->deleteLater();
		const auto bytes = reply->readAll();
		const auto object = QJsonDocument::fromJson(bytes).object();
		const auto candidates = object.value(u"candidates"_q).toArray();
		auto text = QString();
		if (!candidates.isEmpty()) {
			const auto parts = candidates[0].toObject()
				.value(u"content"_q).toObject()
				.value(u"parts"_q).toArray();
			for (const auto &part : parts) {
				text += part.toObject().value(u"text"_q).toString();
			}
		}
		if (!text.trimmed().isEmpty()) {
			done(text.trimmed(), true);
		} else {
			const auto error = ErrorFrom(bytes);
			done(error.isEmpty() ? reply->errorString() : error, false);
		}
	});
}

std::vector<AiChatMessage> AiChatHistory() {
	LoadHistory();
	auto result = std::vector<AiChatMessage>();
	for (const auto &message : Chat().history) {
		result.push_back({
			message.user,
			message.error,
			message.text,
			message.date,
		});
	}
	return result;
}

QString AiChatPartial() {
	return Chat().partial;
}

bool AiChatWaiting() {
	return Chat().waiting;
}

QString AiChatModel() {
	return ModelName();
}

rpl::producer<> AiChatUpdates() {
	return Updates().events();
}

void AiChatClear() {
	LoadHistory();
	if (Chat().waiting) {
		return;
	}
	Chat().history.clear();
	SaveHistory();
	Updates().fire({});
}

bool AiChatSend(
		not_null<Window::SessionController*> controller,
		const QString &text) {
	LoadHistory();
	if (text.trimmed().isEmpty() || Chat().waiting) {
		return false;
	} else if (String(u"ai_chat_key"_q).trimmed().isEmpty()
		&& Int(u"ai_chat_provider"_q) != 4) {
		ShowAiSettingsBox(controller);
		return false;
	}
	Chat().history.push_back({
		.user = true,
		.text = text.trimmed(),
		.date = base::unixtime::now(),
	});
	Chat().waiting = true;
	Chat().partial = QString();
	SaveHistory();
	Updates().fire({});
	Request([=](QString answer, bool ok) {
		Chat().waiting = false;
		Chat().partial = QString();
		Chat().history.push_back({
			.error = !ok,
			.text = answer,
			.date = base::unixtime::now(),
		});
		SaveHistory();
		Updates().fire({});
	}, [=](QString partial) {
		Chat().partial = partial;
		Updates().fire({});
	});
	return true;
}

void ShowAiChat(not_null<Window::SessionController*> controller) {
	Exd::ShowAiChatSection(controller);
}

} // namespace Extera
