#include "commentkey.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include "core/GameHooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using SendToServerFn = void* (*)(void*, void*);
using GetPacketSenderFn = void* (*)(void*);
using CreatePacketFn = std::shared_ptr<void> (*)(int);

SendToServerFn gSendToServer = nullptr;
GetPacketSenderFn gGetPacketSender = nullptr;
CreatePacketFn gCreatePacket = nullptr;

constexpr float kLauncherButtonBaseSize = 52.0f;

// Keep Comment Key buttons visually consistent with the Zoom and Command
// Hotkey overlay buttons. The label is deliberately left visible because the
// comment text identifies what each button will send.
static const char* commentButtonSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
</svg>)svg";

static const char* commentButtonActiveSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <g transform="translate(32, 32) scale(0.85) translate(-32, -32)">
        <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
    </g>
</svg>)svg";

constexpr float kDefaultCommentButtonWidth = 64.0f;
constexpr float kDefaultCommentButtonHeight = 64.0f;

// LeviLauncher derives an independent persisted HUD position from each stable
// button ID ("external_button:<id>").
std::string commentButtonId(std::size_t index) {
    return "bedrocktools.CommentKey.Button" + std::to_string(index + 1);
}

std::string launcherLabel(std::string value, std::string fallback) {
    if (value.empty()) value = std::move(fallback);
    constexpr std::size_t maxBytes = 32;
    if (value.size() <= maxBytes) return value;

    std::size_t end = maxBytes;
    while (end > 0 &&
           (static_cast<unsigned char>(value[end]) & 0xC0u) == 0x80u) {
        --end;
    }
    value.resize(end);
    return value;
}

} // namespace

CommentKey::CommentKey(std::function<void(const std::string&)> sendFunc)
    : Module("CommentKey",
             "Configure comments and send them with keyboard keys or on-screen buttons.") {
    mKeyDown.resize(512, false);
    showInMenu = true;
    // On-screen comments are launcher overlay buttons. The parent module has
    // no custom draw surface of its own in the HUD editor.
    hideInHudEditor = true;
    applyDefaultComments();

    if (sendFunc) {
        mSendFunc = std::move(sendFunc);
    } else {
        mSendFunc = [this](const std::string& text) { sendTextPacket(text); };
    }
}

CommentKey::~CommentKey() {
    unregisterOverlayButtons();
}

void CommentKey::onInit() {
    syncOverlayButtons();
}

void CommentKey::applyDefaultComments() {
    // Callers either hold mMutex (loadConfig) or run single-threaded (constructor).
    mComments.assign(MaxComments, Comment{});
    for (std::size_t i = 0; i < MaxComments; ++i) {
        auto& comment = mComments[i];
        comment.width = kDefaultCommentButtonWidth;
        comment.height = kDefaultCommentButtonHeight;
    }
}

bool CommentKey::onKeyEvent(int keyCode, bool isDown) {
    if (!enabled || keyCode < 0)
        return false;

    std::function<void(const std::string&)> sendFunc;
    std::string text;
    bool matched = false;

    {
        std::lock_guard<std::mutex> lock(mMutex);

        if (static_cast<std::size_t>(keyCode) >= mKeyDown.size())
            mKeyDown.resize(static_cast<std::size_t>(keyCode) + 1, false);

        if (!isDown) {
            mKeyDown[static_cast<std::size_t>(keyCode)] = false;
            return false;
        }

        if (ModuleRegistry::get().keybindBlocked() ||
            mKeyDown[static_cast<std::size_t>(keyCode)]) {
            return false;
        }

        mKeyDown[static_cast<std::size_t>(keyCode)] = true;

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - mLastSendTime).count();
        const bool cooldownOk = elapsed >= cooldownTime;

        for (const auto& comment : mComments) {
            if (!comment.enabled || comment.text.empty() || comment.keyCode != keyCode)
                continue;

            matched = true;
            if (!cooldownOk)
                break;

            mLastSendTime = now;
            sendFunc = mSendFunc;
            text = comment.text;
            break;
        }
    }

    if (sendFunc)
        sendFunc(text);
    return matched;
}

void CommentKey::normalizeComments() {
    for (auto& comment : mComments) {
        if (!comment.enabled)
            continue;
        if (comment.text.size() > 256)
            comment.text.resize(256);
        comment.width = std::clamp(comment.width, 40.0f, 600.0f);
        comment.height = std::clamp(comment.height, 24.0f, 160.0f);
        comment.textColor &= 0x00FFFFFFu;
    }
}

void CommentKey::unregisterOverlayButtons() {
    for (std::size_t i = 0; i < MaxComments; ++i)
        pl::modmenu::unregisterButton(commentButtonId(i));
}

void CommentKey::syncOverlayButtons() {
    std::vector<Comment> comments;
    float buttonScale;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        comments = mComments;
        buttonScale = mButtonScale;
    }

    unregisterOverlayButtons();
    for (std::size_t i = 0; i < comments.size(); ++i) {
        const auto& comment = comments[i];
        if (!comment.enabled)
            continue;

        const std::string fallback = "Comment " + std::to_string(i + 1);
        const std::string label = launcherLabel(comment.text, fallback);
        pl::modmenu::ButtonBuilder builder(commentButtonId(i), fallback);
        builder.moduleId(moduleId)
            .label(label)
            .behavior(pl::modmenu::ButtonBehavior::Click)
            .defaultVisible(true)
            // Use the same custom Minecraft-style frame as Zoom and Command
            // Hotkey. The comment text remains visible on top of the frame.
            .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
            .styleColors(0x00000001, 0x00000001, 0x00000001)
            .svgIcon(commentButtonSvg, false)
            .activeSvgIcon(commentButtonActiveSvg)
            .textColor(0xFF000000u | (comment.textColor & 0x00FFFFFFu))
            .activeTextColor(0xFF1F1F1Fu)
            .sizeScale((comment.width / kLauncherButtonBaseSize) * buttonScale,
                       (comment.height / kLauncherButtonBaseSize) * buttonScale)
            .onEvent([this, i](std::string_view, pl::modmenu::ButtonEvent event, float) {
                if (event == pl::modmenu::ButtonEvent::Click)
                    sendComment(i);
            });
        (void)builder.registerButton();
    }
}

void CommentKey::addComment(std::string text, int keyCode) {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        const auto slot = std::find_if(mComments.begin(), mComments.end(),
                                       [](const Comment& comment) { return !comment.enabled; });
        if (slot == mComments.end())
            return;
        slot->text = std::move(text);
        slot->keyCode = keyCode;
        slot->enabled = true;
        normalizeComments();
    }
    syncOverlayButtons();
}

void CommentKey::removeComment(std::size_t index) {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index >= mComments.size())
            return;
        mComments[index] = Comment{};
    }
    syncOverlayButtons();
}

void CommentKey::updateComment(std::size_t index, std::string text, int keyCode,
                               bool enabledValue) {
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (index >= mComments.size())
            return;
        mComments[index].text = std::move(text);
        mComments[index].keyCode = keyCode;
        mComments[index].enabled = enabledValue;
        normalizeComments();
    }
    syncOverlayButtons();
}

void CommentKey::sendComment(std::size_t index) {
    std::function<void(const std::string&)> sendFunc;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!enabled || index >= mComments.size())
            return;
        const auto& comment = mComments[index];
        if (!comment.enabled || comment.text.empty())
            return;
        sendFunc = mSendFunc;
        text = comment.text;
    }
    if (sendFunc)
        sendFunc(text);
}

void CommentKey::loadConfig(const nlohmann::json& j) {
    // ModMenu invokes this callback for every character entered in a comment.
    // The native button definitions are refreshed below so the launcher's
    // overlay snapshot picks up the new text. ExternalButtonRefresh then
    // re-applies the label in place (no hide/show), so there is no flicker
    // while the comment text field is being typed.
    std::vector<Comment> previousComments;
    float previousButtonScale;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        previousComments = mComments;
        previousButtonScale = mButtonScale;
    }
    Module::loadConfig(j);

    // Configs written before the launcher-style button look have no border
    // entry. For those the stored face color / radius / opacity (and the old
    // white label color) are ignored so the buttons pick up the launcher look.
    const bool legacyStyle = !j.contains("m_buttonBorderColor");

    const auto readColor = [](const nlohmann::json& value, std::uint32_t& out) {
        if (value.is_string()) {
            const std::string hexStr = value.get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try {
                    out = static_cast<std::uint32_t>(std::stoul(hexStr.substr(1), nullptr, 16)) &
                          0x00FFFFFFu;
                } catch (...) {
                }
            }
        } else if (value.is_number()) {
            out = static_cast<std::uint32_t>(value.get<std::uint64_t>()) & 0x00FFFFFFu;
        }
    };

    float loadedButtonScale = mButtonScale;
    if (j.contains("m_buttonScale") && j["m_buttonScale"].is_number())
        loadedButtonScale = std::clamp(j["m_buttonScale"].get<float>(), 0.5f, 2.0f);

    std::uint32_t loadedButtonColor = mButtonColor;
    std::uint32_t loadedBorderColor = mButtonBorderColor;
    float loadedOpacity = mButtonOpacity;
    if (!legacyStyle) {
        if (j.contains("m_buttonOpacity") && j["m_buttonOpacity"].is_number())
            loadedOpacity = std::clamp(j["m_buttonOpacity"].get<float>(), 0.05f, 1.0f);
        if (j.contains("m_buttonColor")) readColor(j["m_buttonColor"], loadedButtonColor);
        readColor(j["m_buttonBorderColor"], loadedBorderColor);
    }

    int loadedCooldown = cooldownTime;
    if (j.contains("cooldownTime") && j["cooldownTime"].is_number_integer())
        loadedCooldown = std::clamp(j["cooldownTime"].get<int>(), 0, 60000);

    {
        std::lock_guard<std::mutex> lock(mMutex);
        cooldownTime = loadedCooldown;
        mButtonScale = loadedButtonScale;
        mButtonOpacity = loadedOpacity;
        mButtonColor = loadedButtonColor;
        mButtonBorderColor = loadedBorderColor;
        applyDefaultComments();

        for (std::size_t i = 0; i < MaxComments; ++i) {
            const std::string prefix = "m_comment" + std::to_string(i + 1);
            const std::string legacyPrefix = "comment_" + std::to_string(i);
            auto& comment = mComments[i];

            if (j.contains(prefix) && j[prefix].is_boolean()) {
                // Slot explicitly disabled in the config -> reset it.
                if (!j[prefix].get<bool>()) {
                    comment = Comment{};
                    continue;
                }

                comment.enabled = true;

                if (j.contains(prefix + "Text") && j[prefix + "Text"].is_string())
                    comment.text = j[prefix + "Text"].get<std::string>();
                if (j.contains(prefix + "Keybind") && j[prefix + "Keybind"].is_number())
                    comment.keyCode = j[prefix + "Keybind"].get<int>();
                if (j.contains(prefix + "Width") && j[prefix + "Width"].is_number())
                    comment.width = j[prefix + "Width"].get<float>();
                if (j.contains(prefix + "Height") && j[prefix + "Height"].is_number())
                    comment.height = j[prefix + "Height"].get<float>();
                if (!legacyStyle && j.contains(prefix + "TextColor")) {
                    const auto& v = j[prefix + "TextColor"];
                    if (v.is_string()) {
                        std::string hexStr = v.get<std::string>();
                        if (!hexStr.empty() && hexStr[0] == '#') {
                            try {
                                comment.textColor =
                                    std::stoul(hexStr.substr(1), nullptr, 16) & 0x00FFFFFFu;
                            } catch (...) {
                            }
                        }
                    } else if (v.is_number()) {
                        comment.textColor =
                            static_cast<std::uint32_t>(v.get<std::uint64_t>()) & 0x00FFFFFFu;
                    }
                }
            } else {
                // Migrate configs written before the fixed comment-slot settings.
                const bool wasEnabled = j.value(legacyPrefix + "_enabled", false);
                if (wasEnabled) {
                    comment.enabled = true;
                    comment.text = j.value(legacyPrefix + "_text", std::string());
                    if (j.contains(legacyPrefix + "_keybind") &&
                        j[legacyPrefix + "_keybind"].is_number())
                        comment.keyCode = j[legacyPrefix + "_keybind"].get<int>();
                } else {
                    comment = Comment{};
                }
            }
        }

        std::fill(mKeyDown.begin(), mKeyDown.end(), false);
        normalizeComments();
    }

    bool overlayChanged = previousButtonScale != mButtonScale;
    if (!overlayChanged && previousComments.size() == mComments.size()) {
        for (std::size_t i = 0; i < mComments.size(); ++i) {
            const auto& oldComment = previousComments[i];
            const auto& newComment = mComments[i];
            // Include text/textColor so the comment label shown on the button
            // is refreshed as it is typed; ExternalButtonRefresh re-applies it.
            if (oldComment.enabled != newComment.enabled ||
                oldComment.width != newComment.width ||
                oldComment.height != newComment.height ||
                oldComment.text != newComment.text ||
                oldComment.textColor != newComment.textColor) {
                overlayChanged = true;
                break;
            }
        }
    }
    // Rebuild only when something shown on a button changed (comment text,
    // width/height, text color, slot enable state, or the scale multiplier):
    // the launcher keeps a snapshot of each button definition, and
    // ExternalButtonRefresh re-applies the new text/size/colors in place once
    // the native definitions are refreshed. Updates that cannot affect a
    // button (for example the cooldown) skip the rebuild so unrelated edits
    // do not churn the launcher's button registry.
    if (overlayChanged) syncOverlayButtons();
}

void CommentKey::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    std::lock_guard<std::mutex> lock(mMutex);
    j["cooldownTime"] = cooldownTime;
    // Uniform multiplier for all comment buttons. Individual Width/Height
    // values below still allow per-comment fine tuning.
    j["m_buttonScale"] = mButtonScale;
    j["m_buttonOpacity"] = mButtonOpacity;

    char buttonColorHex[10];
    std::snprintf(buttonColorHex, sizeof(buttonColorHex), "#%06X", mButtonColor & 0x00FFFFFFu);
    j["m_buttonColor"] = std::string(buttonColorHex);

    char borderColorHex[10];
    std::snprintf(borderColorHex, sizeof(borderColorHex), "#%06X",
                  mButtonBorderColor & 0x00FFFFFFu);
    j["m_buttonBorderColor"] = std::string(borderColorHex);

    // Every slot always emits its full key set so the settings menu registers
    // them at startup; disabled slots emit defaults that are applied the moment
    // the slot toggle is switched on.
    for (std::size_t i = 0; i < MaxComments; ++i) {
        const std::string prefix = "m_comment" + std::to_string(i + 1);
        const Comment fallback{};
        const auto& comment = i < mComments.size() ? mComments[i] : fallback;

        j[prefix] = comment.enabled;
        if (comment.enabled) {
            j[prefix + "Text"] = comment.text;
            j[prefix + "Keybind"] = comment.keyCode;
            j[prefix + "Width"] = comment.width;
            j[prefix + "Height"] = comment.height;
            char textColor[10];
            std::snprintf(textColor, sizeof(textColor), "#%06X",
                          comment.textColor & 0x00FFFFFFu);
            j[prefix + "TextColor"] = std::string(textColor);
        } else {
            j[prefix + "Text"] = "";
            j[prefix + "Keybind"] = 0;
            j[prefix + "Width"] = kDefaultCommentButtonWidth;
            j[prefix + "Height"] = kDefaultCommentButtonHeight;
            j[prefix + "TextColor"] = "#373737";
        }
    }
}

void CommentKey::sendTextPacket(const std::string& text) {
    namespace Packet = bedrocktools::sdk::offsets::Packet;
    namespace TextPacketPayload = bedrocktools::sdk::offsets::TextPacketPayload;

    using bedrocktools::memory::SignatureId;

    if (!gSendToServer) {
        gSendToServer = reinterpret_cast<SendToServerFn>(
            bedrocktools::memory::resolve(SignatureId::LoopbackPacketSenderSendToServer));
    }
    if (!gGetPacketSender) {
        gGetPacketSender = reinterpret_cast<GetPacketSenderFn>(
            bedrocktools::memory::resolve(SignatureId::ClientInstanceGetPacketSender));
    }
    if (!gCreatePacket) {
        gCreatePacket = reinterpret_cast<CreatePacketFn>(
            bedrocktools::memory::resolve(SignatureId::MinecraftPacketsCreatePacket));
    }

    if (!gSendToServer || !gGetPacketSender || !gCreatePacket)
        return;

    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client)
        return;

    std::shared_ptr<void> pktSp = gCreatePacket(9); // TextPacket
    void* pkt = pktSp.get();
    if (!pkt)
        return;

    uintptr_t payload = reinterpret_cast<uintptr_t>(pkt) + Packet::Size;

    std::string* messageOnly =
        reinterpret_cast<std::string*>(payload + TextPacketPayload::MessageOnly::mMessage);
    messageOnly->~basic_string();

    *reinterpret_cast<uint32_t*>(payload + TextPacketPayload::mVariantIndex) = 1;
    *reinterpret_cast<uint8_t*>(payload + TextPacketPayload::AuthorAndMessage::mType) = 1;
    new (reinterpret_cast<void*>(payload + TextPacketPayload::AuthorAndMessage::mAuthor))
        std::string();
    new (reinterpret_cast<void*>(payload + TextPacketPayload::AuthorAndMessage::mMessage))
        std::string(text);

    void* sender = gGetPacketSender(client);
    if (sender)
        gSendToServer(sender, pkt);
}
