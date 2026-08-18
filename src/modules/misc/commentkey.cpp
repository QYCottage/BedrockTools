#include "commentkey.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include "core/GameHooks.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <limits>
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

// Launcher overlay button palette ("keycap" preset used by the launcher's own
// on-screen buttons).
constexpr std::uint32_t kKeycapActiveBg = 0xC6C6C6;
constexpr std::uint32_t kKeycapActiveText = 0xFF1F1F1Fu;
constexpr int kPressedHighlightMs = 130;

std::uint32_t colorWithAlpha(std::uint32_t rgb, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return (static_cast<std::uint32_t>(alpha * 255.0f) << 24) | (rgb & 0x00FFFFFFu);
}

} // namespace

CommentKey::CommentKey(std::function<void(const std::string&)> sendFunc)
    : Module("CommentKey",
             "Configure comments and send them with keyboard keys or on-screen buttons.") {
    mKeyDown.resize(512, false);
    showInMenu = true;
    hideInHudEditor = false;
    isHudModule = true;
    hudPosX = 0.0f;
    hudPosY = 0.0f;
    applyDefaultComments();

    if (sendFunc) {
        mSendFunc = std::move(sendFunc);
    } else {
        mSendFunc = [this](const std::string& text) { sendTextPacket(text); };
    }
}

void CommentKey::applyDefaultComments() {
    // Callers either hold mMutex (loadConfig) or run single-threaded (constructor).
    mComments.assign(MaxComments, Comment{});
    for (std::size_t i = 0; i < MaxComments; ++i) {
        auto& comment = mComments[i];
        comment.screen = true;
        // Staggered default column so enabled slots do not stack on each other.
        comment.x = 140.0f;
        comment.y = 80.0f + static_cast<float>(i) * 52.0f;
    }
    mDraggingIndex = -1;
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

void CommentKey::onFrame() {
    std::vector<PLModMenu_DrawCommand> commands;
    std::vector<std::string> labels;

    {
        std::lock_guard<std::mutex> lock(mMutex);

        if (enabled) {
            commands.reserve(MaxComments * 3 + 1);
            labels.reserve(MaxComments);

            // HUD editor group hitbox: union of all on-screen buttons with a
            // nearly transparent background so gaps stay draggable.
            float minRelX = std::numeric_limits<float>::max();
            float minRelY = std::numeric_limits<float>::max();
            float maxRelX = std::numeric_limits<float>::lowest();
            float maxRelY = std::numeric_limits<float>::lowest();
            bool hasAny = false;
            for (std::size_t j = 0; j < MaxComments; ++j) {
                const auto& c = mComments[j];
                if (!c.enabled || !c.screen)
                    continue;
                minRelX = std::min(minRelX, c.x);
                minRelY = std::min(minRelY, c.y);
                maxRelX = std::max(maxRelX, c.x + c.width);
                maxRelY = std::max(maxRelY, c.y + c.height);
                hasAny = true;
            }
            if (hasAny) {
                PLModMenu_DrawCommand groupBg{};
                groupBg.type = PL_DRAW_RECT_FILLED;
                groupBg.x = hudPosX + minRelX;
                groupBg.y = hudPosY + minRelY;
                groupBg.w = maxRelX - minRelX;
                groupBg.h = maxRelY - minRelY;
                groupBg.x3 = mButtonRadius;
                groupBg.color = 0x02000000;
                commands.push_back(groupBg);
            }

            for (std::size_t i = 0; i < MaxComments; ++i) {
                const auto& comment = mComments[i];
                if (!comment.enabled || !comment.screen)
                    continue;

                const float absX = comment.x + hudPosX;
                const float absY = comment.y + hudPosY;

                const bool dragging =
                    mHudEditMode && mDraggingIndex == static_cast<int>(i);
                const bool pressed = isPressed(i);

                // Face: launcher keycap style (light gray face, 0.85 alpha,
                // tiny corner radius).
                PLModMenu_DrawCommand rect{};
                rect.type = PL_DRAW_RECT_FILLED;
                rect.x = absX;
                rect.y = absY;
                rect.w = comment.width;
                rect.h = comment.height;
                rect.x3 = mButtonRadius;
                if (dragging) {
                    rect.color = colorWithAlpha(0x4AE0A0, 0.95f);
                } else if (pressed) {
                    rect.color = colorWithAlpha(kKeycapActiveBg, mButtonOpacity);
                } else {
                    rect.color = colorWithAlpha(mButtonColor, mButtonOpacity);
                }
                commands.push_back(rect);

                // Border: the same 2px dark stroke the launcher draws around
                // its own overlay buttons (also marks drag targets in the HUD
                // editor).
                PLModMenu_DrawCommand border{};
                border.type = PL_DRAW_RECT;
                border.x = absX;
                border.y = absY;
                border.w = comment.width;
                border.h = comment.height;
                border.x3 = mButtonRadius;
                border.size = std::max(1.0f, mButtonBorderWidth);
                border.color = dragging
                                   ? 0xFF4AE0A0
                                   : colorWithAlpha(mButtonBorderColor, mButtonOpacity);
                commands.push_back(border);

                labels.push_back(comment.text.empty()
                                     ? ("Comment " + std::to_string(i + 1))
                                     : comment.text);
                PLModMenu_DrawCommand text{};
                text.type = PL_DRAW_TEXT;
                text.x = absX;
                text.y = absY;
                text.w = comment.width;
                text.h = comment.height;
                // Forwarded to Android Paint.setTextSize(), so it is a pixel size.
                text.size = comment.textSize;
                text.color = pressed ? kKeycapActiveText
                                     : (0xFF000000u | (comment.textColor & 0x00FFFFFFu));
                text.text = labels.back().c_str();
                commands.push_back(text);
            }
        }
    }

    // When disabled (or with no buttons on screen) submit an empty batch so
    // any previously drawn buttons disappear.
    submitDrawCommands(moduleId, commands);
}

bool CommentKey::onTouchEvent(float x, float y, bool isDown) {
    if (!enabled || ModuleRegistry::get().keybindBlocked())
        return false;

    // HUD edit mode: drag individual buttons instead of sending comments.
    if (mHudEditMode) {
        if (isDown) {
            std::lock_guard<std::mutex> lock(mMutex);
            for (std::size_t i = 0; i < MaxComments; ++i) {
                const auto& comment = mComments[i];
                if (!comment.enabled || !comment.screen)
                    continue;
                if (!inside(comment, x, y))
                    continue;
                mDraggingIndex = static_cast<int>(i);
                mDragOffsetX = x - (comment.x + hudPosX);
                mDragOffsetY = y - (comment.y + hudPosY);
                return true;
            }
            mDraggingIndex = -1;
            return false;
        }

        bool moved = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            if (mDraggingIndex >= 0 && mDraggingIndex < static_cast<int>(MaxComments)) {
                auto& comment = mComments[static_cast<std::size_t>(mDraggingIndex)];
                if (!comment.enabled) {
                    mDraggingIndex = -1;
                } else {
                    const float clampedAbsX = std::clamp(x - mDragOffsetX, 0.0f, 10000.0f);
                    const float clampedAbsY = std::clamp(y - mDragOffsetY, 0.0f, 10000.0f);
                    comment.x = std::clamp(clampedAbsX - hudPosX, -5000.0f, 10000.0f);
                    comment.y = std::clamp(clampedAbsY - hudPosY, -5000.0f, 10000.0f);
                    normalizeComments();
                    moved = true;
                }
            }
        }
        // Save after releasing mMutex: ConfigManager::save() calls saveConfig(),
        // which takes the same mutex.
        if (moved)
            bedrocktools::config::ConfigManager::get().save();
        return moved;
    }

    // Normal mode: tap down on a button sends its comment.
    if (!isDown)
        return false;

    std::size_t hit = MaxComments;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (std::size_t i = 0; i < MaxComments; ++i) {
            const auto& comment = mComments[i];
            if (!comment.enabled || !comment.screen)
                continue;
            if (!inside(comment, x, y))
                continue;
            hit = i;
            break;
        }
    }
    if (hit >= MaxComments)
        return false;

    sendComment(hit);
    return true;
}

bool CommentKey::isPressed(std::size_t index) const {
    if (mPressedIndex != static_cast<int>(index))
        return false;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - mPressedTime)
                             .count();
    return elapsed >= 0 && elapsed < kPressedHighlightMs;
}

bool CommentKey::inside(const Comment& comment, float x, float y) const {
    const float absX = comment.x + hudPosX;
    const float absY = comment.y + hudPosY;
    return x >= absX && x <= absX + comment.width &&
           y >= absY && y <= absY + comment.height;
}

void CommentKey::normalizeComments() {
    hudPosX = std::clamp(hudPosX, -5000.0f, 10000.0f);
    hudPosY = std::clamp(hudPosY, -5000.0f, 10000.0f);
    for (auto& comment : mComments) {
        if (!comment.enabled)
            continue;
        if (comment.text.size() > 256)
            comment.text.resize(256);
        comment.width = std::clamp(comment.width, 40.0f, 600.0f);
        comment.height = std::clamp(comment.height, 24.0f, 160.0f);
        comment.textSize = std::clamp(comment.textSize, 8.0f, 100.0f);
        comment.textColor &= 0x00FFFFFFu;
        comment.x = std::clamp(comment.x, -5000.0f, 10000.0f);
        comment.y = std::clamp(comment.y, -5000.0f, 10000.0f);
    }
}

void CommentKey::addComment(std::string text, int keyCode) {
    std::lock_guard<std::mutex> lock(mMutex);
    const auto slot = std::find_if(mComments.begin(), mComments.end(),
                                   [](const Comment& comment) { return !comment.enabled; });
    if (slot != mComments.end()) {
        slot->text = std::move(text);
        slot->keyCode = keyCode;
        slot->enabled = true;
        if (!slot->screen) {
            // Programmatically added comments get a visible on-screen button,
            // mirroring the old touch-button behavior.
            slot->screen = true;
            const auto index =
                static_cast<std::size_t>(std::distance(mComments.begin(), slot));
            slot->x = 140.0f;
            slot->y = 80.0f + static_cast<float>(index) * 52.0f;
            slot->width = 110.0f;
            slot->height = 40.0f;
            slot->textSize = 20.0f;
            slot->textColor = 0x373737;
        }
    }
}

void CommentKey::removeComment(std::size_t index) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (index < mComments.size())
        mComments[index] = Comment{};
}

void CommentKey::updateComment(std::size_t index, std::string text, int keyCode, bool enabledValue) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (index < mComments.size()) {
        mComments[index].text = std::move(text);
        mComments[index].keyCode = keyCode;
        mComments[index].enabled = enabledValue;
    }
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
        mPressedIndex = static_cast<int>(index);
        mPressedTime = std::chrono::steady_clock::now();
    }
    if (sendFunc)
        sendFunc(text);
}

void CommentKey::loadConfig(const nlohmann::json& j) {
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

    std::uint32_t loadedButtonColor = mButtonColor;
    std::uint32_t loadedBorderColor = mButtonBorderColor;
    float loadedOpacity = mButtonOpacity;
    float loadedRadius = mButtonRadius;
    float loadedBorderWidth = mButtonBorderWidth;
    if (!legacyStyle) {
        if (j.contains("m_buttonOpacity") && j["m_buttonOpacity"].is_number())
            loadedOpacity = std::clamp(j["m_buttonOpacity"].get<float>(), 0.05f, 1.0f);
        if (j.contains("m_buttonRadius") && j["m_buttonRadius"].is_number())
            loadedRadius = std::clamp(j["m_buttonRadius"].get<float>(), 0.0f, 40.0f);
        if (j.contains("m_buttonBorderWidth") && j["m_buttonBorderWidth"].is_number())
            loadedBorderWidth = std::clamp(j["m_buttonBorderWidth"].get<float>(), 0.0f, 8.0f);
        if (j.contains("m_buttonColor")) readColor(j["m_buttonColor"], loadedButtonColor);
        readColor(j["m_buttonBorderColor"], loadedBorderColor);
    }

    int loadedCooldown = cooldownTime;
    if (j.contains("cooldownTime") && j["cooldownTime"].is_number_integer())
        loadedCooldown = std::clamp(j["cooldownTime"].get<int>(), 0, 60000);

    // HUD editor fields (same keys the launcher HUD editor writes).
    if (j.contains("hudPosX") && j["hudPosX"].is_number())
        hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY") && j["hudPosY"].is_number())
        hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule") && j["isHudModule"].is_boolean())
        isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_hudEditMode") && j["m_hudEditMode"].is_boolean())
        mHudEditMode = j["m_hudEditMode"].get<bool>();
    else if (j.contains("hudEditMode") && j["hudEditMode"].is_boolean())
        mHudEditMode = j["hudEditMode"].get<bool>();

    {
        std::lock_guard<std::mutex> lock(mMutex);
        cooldownTime = loadedCooldown;
        mButtonOpacity = loadedOpacity;
        mButtonRadius = loadedRadius;
        mButtonBorderWidth = loadedBorderWidth;
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
                // Enabled slots default to showing their on-screen button unless
                // the stored config says otherwise.
                comment.screen = true;

                if (j.contains(prefix + "Text") && j[prefix + "Text"].is_string())
                    comment.text = j[prefix + "Text"].get<std::string>();
                if (j.contains(prefix + "Keybind") && j[prefix + "Keybind"].is_number())
                    comment.keyCode = j[prefix + "Keybind"].get<int>();
                if (j.contains(prefix + "Screen") && j[prefix + "Screen"].is_boolean())
                    comment.screen = j[prefix + "Screen"].get<bool>();
                if (j.contains(prefix + "X") && j[prefix + "X"].is_number())
                    comment.x = j[prefix + "X"].get<float>();
                if (j.contains(prefix + "Y") && j[prefix + "Y"].is_number())
                    comment.y = j[prefix + "Y"].get<float>();
                if (j.contains(prefix + "Width") && j[prefix + "Width"].is_number())
                    comment.width = j[prefix + "Width"].get<float>();
                if (j.contains(prefix + "Height") && j[prefix + "Height"].is_number())
                    comment.height = j[prefix + "Height"].get<float>();
                if (j.contains(prefix + "TextSize") && j[prefix + "TextSize"].is_number())
                    comment.textSize = j[prefix + "TextSize"].get<float>();
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
                // Migrate configs written before the on-screen button settings.
                // Such comments used to have an always-visible touch button, so
                // keep it on screen.
                const bool wasEnabled = j.value(legacyPrefix + "_enabled", false);
                if (wasEnabled) {
                    comment.enabled = true;
                    comment.screen = true;
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
        mDraggingIndex = -1;
    }
}

void CommentKey::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    std::lock_guard<std::mutex> lock(mMutex);
    j["cooldownTime"] = cooldownTime;
    j["m_buttonOpacity"] = mButtonOpacity;
    j["m_buttonRadius"] = mButtonRadius;
    j["m_buttonBorderWidth"] = mButtonBorderWidth;

    char buttonColorHex[10];
    std::snprintf(buttonColorHex, sizeof(buttonColorHex), "#%06X", mButtonColor & 0x00FFFFFFu);
    j["m_buttonColor"] = std::string(buttonColorHex);

    char borderColorHex[10];
    std::snprintf(borderColorHex, sizeof(borderColorHex), "#%06X",
                  mButtonBorderColor & 0x00FFFFFFu);
    j["m_buttonBorderColor"] = std::string(borderColorHex);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_hudEditMode"] = mHudEditMode;

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
            j[prefix + "Screen"] = comment.screen;
            j[prefix + "X"] = comment.x;
            j[prefix + "Y"] = comment.y;
            j[prefix + "Width"] = comment.width;
            j[prefix + "Height"] = comment.height;
            j[prefix + "TextSize"] = comment.textSize;
            char textColor[10];
            std::snprintf(textColor, sizeof(textColor), "#%06X",
                          comment.textColor & 0x00FFFFFFu);
            j[prefix + "TextColor"] = std::string(textColor);
        } else {
            j[prefix + "Text"] = "";
            j[prefix + "Keybind"] = 0;
            j[prefix + "Screen"] = false;
            j[prefix + "X"] = 140.0f;
            j[prefix + "Y"] = 80.0f + static_cast<float>(i) * 52.0f;
            j[prefix + "Width"] = 110.0f;
            j[prefix + "Height"] = 40.0f;
            j[prefix + "TextSize"] = 20.0f;
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
