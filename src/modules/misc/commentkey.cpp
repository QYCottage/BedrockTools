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
#include <string_view>
#include <utility>
#include <vector>

namespace {

using SendToServerFn = void* (*)(void*, void*);
using GetPacketSenderFn = void* (*)(void*);
using CreatePacketFn = std::shared_ptr<void> (*)(int);

SendToServerFn gSendToServer = nullptr;
GetPacketSenderFn gGetPacketSender = nullptr;
CreatePacketFn gCreatePacket = nullptr;

std::uint32_t colorWithAlpha(std::uint32_t rgb, float alpha) {
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return (static_cast<std::uint32_t>(alpha * 255.0f) << 24) | (rgb & 0x00FFFFFFu);
}

// Truncates text to fit the launcher's button label byte limit without
// splitting a UTF-8 sequence.
std::string truncateUtf8(const std::string& text, std::size_t maxBytes) {
    if (text.size() <= maxBytes) return text;
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        --cut;
    return text.substr(0, cut);
}

} // namespace

CommentKey::CommentKey(std::function<void(const std::string&)> sendFunc)
    : Module("CommentKey",
             "Configure comments and send them with keyboard keys or on-screen buttons.") {
    mKeyDown.resize(512, false);
    mNativeButtonRegistered.resize(MaxComments, false);
    mNativeButtonLabel.resize(MaxComments);
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

void CommentKey::onEnable() {
    mNativeButtonsDirty = true;
    mNativeButtonRetries = 0;
    syncNativeButtons();
}

void CommentKey::onLauncherRegistered() {
    // The module is now visible to the launcher's Mod Menu bridge, so this is
    // the earliest safe point to register the native overlay buttons (before
    // the launcher's first button refresh).
    mNativeButtonsDirty = true;
    mNativeButtonRetries = 0;
    syncNativeButtons();
}

void CommentKey::syncNativeButtons() {
    // The launcher's native overlay buttons are rendered and hit-tested by the
    // launcher itself (real Android views), so they respond while the game is
    // running and the player is in a world, independent of the game's input
    // pipeline. Slots that fail to register keep the classic drawn buttons.
    struct SlotState {
        bool want = false;
        std::string label;
        std::uint32_t textColor = 0xFFFFFF;
        float width = 110.0f;
        float height = 40.0f;
    };
    std::vector<SlotState> states(MaxComments);

    // The launcher never invokes our button callbacks while registering, so
    // holding mMutex across the bridge calls is safe and keeps the registered
    // state synchronized with the reads in onFrame/onTouchEvent.
    std::lock_guard<std::mutex> lock(mMutex);

    for (std::size_t i = 0; i < MaxComments; ++i) {
        const auto& comment = mComments[i];
        states[i].want = nativeButtonsEnabled && comment.enabled &&
                         comment.screen && !comment.text.empty();
        states[i].label = truncateUtf8(comment.text, 32);
        states[i].textColor = comment.textColor & 0x00FFFFFFu;
        states[i].width = comment.width;
        states[i].height = comment.height;
    }

    bool allSettled = true;
    for (std::size_t i = 0; i < MaxComments; ++i) {
        const std::string id = moduleId + ".comment" + std::to_string(i);

        if (!states[i].want) {
            if (mNativeButtonRegistered[i]) {
                pl::modmenu::unregisterButton(id);
                mNativeButtonRegistered[i] = false;
                mNativeButtonLabel[i].clear();
            }
            continue;
        }

        if (mNativeButtonRegistered[i]) {
            if (mNativeButtonLabel[i] == states[i].label) continue;
            // Label changed -> re-register so the launcher shows the new text.
            pl::modmenu::unregisterButton(id);
            mNativeButtonRegistered[i] = false;
        }

        pl::modmenu::ButtonBuilder builder(id, "Comment " + std::to_string(i + 1));
        builder.moduleId(moduleId)
            .label(states[i].label)
            .behavior(pl::modmenu::ButtonBehavior::Click)
            .defaultVisible(true)
            .styleColors(colorWithAlpha(mButtonColor, mButtonOpacity),
                         colorWithAlpha(0x4AE0A0, 0.95f))
            .textColor(0xFF000000u | states[i].textColor)
            .activeTextColor(0xFF000000u)
            .sizeScale(std::clamp(states[i].width / 110.0f, 0.6f, 4.0f),
                       std::clamp(states[i].height / 40.0f, 0.6f, 2.0f))
            .onEvent([this, i](std::string_view, pl::modmenu::ButtonEvent event,
                               float) {
                if (event == pl::modmenu::ButtonEvent::Click) sendComment(i);
            });
        if (builder.registerButton()) {
            mNativeButtonRegistered[i] = true;
            mNativeButtonLabel[i] = states[i].label;
        } else {
            // Not ready yet (e.g. the module is not registered with the
            // launcher yet right after startup); retry on the next frame.
            allSettled = false;
        }
    }

    if (!allSettled && ++mNativeButtonRetries < 10) {
        mNativeButtonsDirty = true;
        return;
    }
    mNativeButtonsDirty = false;
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
    if (mNativeButtonsDirty)
        syncNativeButtons();

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
                if (!c.enabled || !c.screen || mNativeButtonRegistered[j])
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
                if (!comment.enabled || !comment.screen || mNativeButtonRegistered[i])
                    continue;

                const float absX = comment.x + hudPosX;
                const float absY = comment.y + hudPosY;

                PLModMenu_DrawCommand rect{};
                rect.type = PL_DRAW_RECT_FILLED;
                rect.x = absX;
                rect.y = absY;
                rect.w = comment.width;
                rect.h = comment.height;
                rect.x3 = mButtonRadius;
                // Highlight the dragged button while in HUD edit mode.
                if (mHudEditMode && mDraggingIndex == static_cast<int>(i)) {
                    rect.color = colorWithAlpha(0x4AE0A0, 0.95f);
                } else {
                    rect.color = colorWithAlpha(mButtonColor, mButtonOpacity);
                }
                commands.push_back(rect);

                // Outline in HUD edit mode to hint the button is draggable.
                if (mHudEditMode) {
                    PLModMenu_DrawCommand outline{};
                    outline.type = PL_DRAW_RECT;
                    outline.x = absX;
                    outline.y = absY;
                    outline.w = comment.width;
                    outline.h = comment.height;
                    outline.x3 = mButtonRadius;
                    outline.color = 0xFF4AE0A0;
                    outline.size = 1.0f;
                    commands.push_back(outline);
                }

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
                text.color = 0xFF000000u | (comment.textColor & 0x00FFFFFFu);
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
    if (!enabled)
        return false;

    // HUD edit mode: drag individual buttons instead of sending comments.
    if (mHudEditMode) {
        if (isDown) {
            std::lock_guard<std::mutex> lock(mMutex);
            for (std::size_t i = 0; i < MaxComments; ++i) {
                const auto& comment = mComments[i];
                if (!comment.enabled || !comment.screen || mNativeButtonRegistered[i])
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
            if (!comment.enabled || !comment.screen || mNativeButtonRegistered[i])
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
            slot->textColor = 0xFFFFFF;
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
    }
    if (sendFunc)
        sendFunc(text);
}

void CommentKey::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    int loadedCooldown = cooldownTime;
    if (j.contains("cooldownTime") && j["cooldownTime"].is_number_integer())
        loadedCooldown = std::clamp(j["cooldownTime"].get<int>(), 0, 60000);

    if (j.contains("nativeButtons") && j["nativeButtons"].is_boolean())
        nativeButtonsEnabled = j["nativeButtons"].get<bool>();

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
                if (j.contains(prefix + "TextColor")) {
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

    // Keep the launcher-native overlay buttons in sync with the loaded slots.
    mNativeButtonsDirty = true;
    mNativeButtonRetries = 0;
    syncNativeButtons();
}

void CommentKey::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    std::lock_guard<std::mutex> lock(mMutex);
    j["nativeButtons"] = nativeButtonsEnabled;
    j["cooldownTime"] = cooldownTime;
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
            j[prefix + "TextColor"] = "#FFFFFF";
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
