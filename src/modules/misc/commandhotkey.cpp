#include "commandhotkey.hpp"

#include "../ModuleRegistry.hpp"
#include "core/GameHooks.hpp"
#include "core/memory/Hooks.hpp"
#include "config/ConfigManager.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <pl/ModMenu.hpp>

namespace {

CommandHotkeyModule* g_instance = nullptr;

using SendToServerFn = void* (*)(void*, void*);
using GetPacketSenderFn = void* (*)(void*);
using CreatePacketFn = std::shared_ptr<void> (*)(int);

SendToServerFn g_sendToServer = nullptr;
GetPacketSenderFn g_getPacketSender = nullptr;
CreatePacketFn g_createPacket = nullptr;

bool resolvePacketFunctions() {
    if (!g_sendToServer) {
        g_sendToServer = reinterpret_cast<SendToServerFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::LoopbackPacketSenderSendToServer));
    }
    if (!g_getPacketSender) {
        g_getPacketSender = reinterpret_cast<GetPacketSenderFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::ClientInstanceGetPacketSender));
    }
    if (!g_createPacket) {
        g_createPacket = reinterpret_cast<CreatePacketFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::MinecraftPacketsCreatePacket));
    }
    return g_sendToServer && g_getPacketSender && g_createPacket;
}

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

std::string keyName(int key) {
    // Android KEYCODE_* values. Unknown values are still usable and are shown numerically.
    switch (key) {
        case 4: return "BACK";
        case 19: return "UP";
        case 20: return "DOWN";
        case 21: return "LEFT";
        case 22: return "RIGHT";
        case 23: return "ENTER";
        case 24: return "VOL+";
        case 25: return "VOL-";
        case 61: return "TAB";
        case 62: return "SPACE";
        case 66: return "ENTER";
        case 67: return "BACKSPACE";
        case 82: return "MENU";
        case 111: return "ESC";
        case 113: return "CTRL_L";
        case 114: return "CTRL_R";
        case 115: return "CAPS";
        case 116: return "SCROLL";
        case 117: return "META_L";
        case 118: return "META_R";
        case 119: return "FUNCTION";
        case 57: return "ALT_L";
        case 58: return "ALT_R";
        case 59: return "SHIFT_L";
        case 60: return "SHIFT_R";
        case 131: return "F1";
        case 132: return "F2";
        case 133: return "F3";
        case 134: return "F4";
        case 135: return "F5";
        case 136: return "F6";
        case 137: return "F7";
        case 138: return "F8";
        case 139: return "F9";
        case 140: return "F10";
        case 141: return "F11";
        case 142: return "F12";
        default:
            if (key >= 7 && key <= 16) return std::string(1, static_cast<char>('0' + (key - 7)));
            if (key >= 29 && key <= 54) return std::string(1, static_cast<char>('A' + (key - 29)));
            return "KEY " + std::to_string(key);
    }
}

}

CommandHotkeyModule::CommandHotkeyModule()
    : Module("Command Hotkey", "Run custom commands from keyboard keys or on-screen mobile buttons.") {
    g_instance = this;
    showInMenu = true;
    hideInHudEditor = false;
    isHudModule = true;
    hudPosX = 0.0f;
    hudPosY = 0.0f;
    // All command slots exist from the start (no "Add Command" button needed).
    applyDefaultBindings();
}

CommandHotkeyModule::~CommandHotkeyModule() {
    if (g_instance == this) g_instance = nullptr;
}

CommandHotkeyModule* CommandHotkeyModule::instance() {
    return g_instance;
}

void CommandHotkeyModule::onInit() {
    // Input hooks are installed once by Runtime. This module only consumes the events.
}

void CommandHotkeyModule::onEnable() {
    m_nativeButtonsDirty = true;
    m_nativeButtonRetries = 0;
    syncNativeButtons();
}

void CommandHotkeyModule::onDisable() {
    unregisterAllNativeButtons();
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingCommands.clear();
}

void CommandHotkeyModule::onLauncherRegistered() {
    // The module is now visible to the launcher's Mod Menu bridge, so this is
    // the earliest safe point to register the native overlay buttons (before
    // the launcher's first button refresh).
    m_nativeButtonsDirty = true;
    m_nativeButtonRetries = 0;
    syncNativeButtons();
}

void CommandHotkeyModule::unregisterAllNativeButtons() {
    for (std::size_t i = 0; i < MaxCommands; ++i) {
        if (!m_nativeButtonRegistered[i]) continue;
        pl::modmenu::unregisterButton(moduleId + ".command" + std::to_string(i));
        m_nativeButtonRegistered[i] = false;
        m_nativeButtonLabel[i].clear();
    }
}

void CommandHotkeyModule::syncNativeButtons() {
    // The launcher's native overlay buttons are rendered and hit-tested by the
    // launcher itself (real Android views), so they respond while the game is
    // running and the player is in a world, independent of the game's input
    // pipeline.
    struct SlotState {
        bool want = false;
        std::string label;
        std::uint32_t textColor = 0xFFFFFF;
        float width = 110.0f;
        float height = 40.0f;
    };
    std::array<SlotState, MaxCommands> states{};

    for (std::size_t i = 0; i < MaxCommands; ++i) {
        const auto& binding = m_commands[i];
        states[i].want = enabled && nativeButtonsEnabled && binding.enabled &&
                         binding.screen && !binding.command.empty();
        states[i].label = truncateUtf8(defaultLabel(binding, i), 32);
        states[i].textColor = binding.textColor & 0x00FFFFFFu;
        states[i].width = binding.width;
        states[i].height = binding.height;
    }

    bool allSettled = true;
    for (std::size_t i = 0; i < MaxCommands; ++i) {
        const std::string id = moduleId + ".command" + std::to_string(i);

        if (!states[i].want) {
            if (m_nativeButtonRegistered[i]) {
                pl::modmenu::unregisterButton(id);
                m_nativeButtonRegistered[i] = false;
                m_nativeButtonLabel[i].clear();
            }
            continue;
        }

        if (m_nativeButtonRegistered[i]) {
            if (m_nativeButtonLabel[i] == states[i].label) continue;
            // Label changed -> re-register so the launcher shows the new text.
            pl::modmenu::unregisterButton(id);
            m_nativeButtonRegistered[i] = false;
        }

        pl::modmenu::ButtonBuilder builder(id, "Command " + std::to_string(i + 1));
        builder.moduleId(moduleId)
            .label(states[i].label)
            .behavior(pl::modmenu::ButtonBehavior::Click)
            .defaultVisible(true)
            .styleColors(colorWithAlpha(m_buttonColor, m_buttonOpacity),
                         colorWithAlpha(0x4AE0A0, 0.95f))
            .textColor(0xFF000000u | states[i].textColor)
            .activeTextColor(0xFF000000u)
            .sizeScale(std::clamp(states[i].width / 110.0f, 0.6f, 4.0f),
                       std::clamp(states[i].height / 40.0f, 0.6f, 2.0f))
            .onEvent([this, i](std::string_view, pl::modmenu::ButtonEvent event,
                               float) {
                if (event == pl::modmenu::ButtonEvent::Click) execute(i);
            });
        if (builder.registerButton()) {
            m_nativeButtonRegistered[i] = true;
            m_nativeButtonLabel[i] = states[i].label;
        } else {
            // Not ready yet (e.g. the module is not registered with the
            // launcher yet right after startup); retry on the next frame.
            allSettled = false;
        }
    }

    if (!allSettled && ++m_nativeButtonRetries < 10) {
        m_nativeButtonsDirty = true;
        return;
    }
    m_nativeButtonsDirty = false;
}

void CommandHotkeyModule::execute(std::size_t index) {
    if (!enabled || index >= MaxCommands) return;
    auto& binding = m_commands[index];
    if (!binding.enabled) return;

    const auto command = normalizeCommand(binding.command);
    if (command.empty()) return;

    // Queue for the game thread. Native overlay clicks arrive on the
    // launcher UI thread; building CommandRequestPacket there is ignored
    // or races the client instance.
    std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingCommands.push_back(command);
}

void CommandHotkeyModule::flushPendingCommands() {
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(m_pendingMutex);
        pending.swap(m_pendingCommands);
    }
    for (const auto& command : pending)
        sendCommandPacket(command);
}

bool CommandHotkeyModule::onKeyEvent(int key, bool isDown) {
    if (!enabled || !isDown || ModuleRegistry::get().keybindBlocked()) return false;

    for (std::size_t i = 0; i < MaxCommands; ++i) {
        const auto& binding = m_commands[i];
        if (!binding.enabled || binding.key <= 0 || binding.key != key) continue;
        execute(i);
        return true;
    }
    return false;
}

bool CommandHotkeyModule::onTouchEvent(float, float, bool) {
    // Overlay buttons are launcher-native views; the game no longer owns
    // the old drawn hitboxes.
    return false;
}

void CommandHotkeyModule::onFrame() {
    flushPendingCommands();

    if (m_nativeButtonsDirty)
        syncNativeButtons();

    // Never draw the old in-mod rectangles; the launcher owns the buttons.
    std::vector<PLModMenu_DrawCommand> empty;
    submitDrawCommands(moduleId, empty);
}

void CommandHotkeyModule::applyDefaultBindings() {
    for (std::size_t i = 0; i < MaxCommands; ++i) {
        m_commands[i] = Binding{};
        m_commands[i].enabled = true;
        m_commands[i].screen = true;
        // positions are relative to hudPos
        m_commands[i].x = 24.0f;
        m_commands[i].y = 80.0f + static_cast<float>(i) * 52.0f;
        m_commands[i].width = 110.0f;
        m_commands[i].height = 40.0f;
        m_commands[i].label = "Command " + std::to_string(i + 1);
    }
    // Invalidate dragging
    m_draggingIndex = -1;
}

void CommandHotkeyModule::normalizeBindings() {
    // Clamp HUD position
    hudPosX = std::clamp(hudPosX, -5000.0f, 10000.0f);
    hudPosY = std::clamp(hudPosY, -5000.0f, 10000.0f);
    for (std::size_t i = 0; i < MaxCommands; ++i) {
        auto& b = m_commands[i];
        if (!b.enabled) continue;
        if (b.command.size() > 256) b.command.resize(256);
        if (b.label.size() > 64) b.label.resize(64);
        b.width = std::clamp(b.width, 40.0f, 600.0f);
        b.height = std::clamp(b.height, 24.0f, 160.0f);
        b.textSize = std::clamp(b.textSize, 8.0f, 100.0f);
        b.textColor &= 0x00FFFFFFu;
        // keep relative x/y within reasonable range, absolute will be clamped on drag
        b.x = std::clamp(b.x, -5000.0f, 10000.0f);
        b.y = std::clamp(b.y, -5000.0f, 10000.0f);
    }
}

std::string CommandHotkeyModule::normalizeCommand(std::string command) {
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front()))) command.erase(command.begin());
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back()))) command.pop_back();
    if (!command.empty() && command.front() != '/') command.insert(command.begin(), '/');
    return command;
}

std::string CommandHotkeyModule::defaultLabel(const Binding& binding, std::size_t index) {
    if (!binding.label.empty()) return binding.label;
    if (!binding.command.empty()) {
        std::string value = binding.command;
        if (!value.empty() && value.front() == '/') value.erase(value.begin());
        return value.empty() ? "Command " + std::to_string(index + 1) : value;
    }
    return "Command " + std::to_string(index + 1);
}

bool CommandHotkeyModule::inside(const Binding& binding, float x, float y) const {
    float absX = binding.x + hudPosX;
    float absY = binding.y + hudPosY;
    return x >= absX && x <= absX + binding.width &&
           y >= absY && y <= absY + binding.height;
}

void CommandHotkeyModule::sendCommandPacket(const std::string& command) {
    if (!resolvePacketFunctions()) return;

    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return;

    std::shared_ptr<void> packet = g_createPacket(77);
    if (!packet) return;

    auto* raw = packet.get();
    const std::uintptr_t payload = reinterpret_cast<std::uintptr_t>(raw) +
                                   bedrocktools::sdk::offsets::Packet::Size;

    *reinterpret_cast<std::string*>(payload +
        bedrocktools::sdk::offsets::CommandRequestPacketPayload::mCommand) = command;

    *reinterpret_cast<std::uint8_t*>(payload +
        bedrocktools::sdk::offsets::CommandRequestPacketPayload::mOrigin +
        bedrocktools::sdk::offsets::CommandOriginData::mType) = 0;

    *reinterpret_cast<bool*>(payload +
        bedrocktools::sdk::offsets::CommandRequestPacketPayload::mInternalSource) = true;

    void* sender = g_getPacketSender(client);
    if (sender) g_sendToServer(sender, raw);
}

void CommandHotkeyModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    // Start from the built-in defaults (all slots exist directly) and then
    // override each slot with whatever is stored in the config.
    applyDefaultBindings();

    if (j.contains("nativeButtons") && j["nativeButtons"].is_boolean())
        nativeButtonsEnabled = j["nativeButtons"].get<bool>();
    if (j.contains("m_buttonOpacity")) m_buttonOpacity = std::clamp(j["m_buttonOpacity"].get<float>(), 0.05f, 1.0f);
    if (j.contains("m_buttonRadius")) m_buttonRadius = std::clamp(j["m_buttonRadius"].get<float>(), 0.0f, 40.0f);
    if (j.contains("m_buttonColor")) {
        const auto& v = j["m_buttonColor"];
        if (v.is_string()) {
            std::string hexStr = v.get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { m_buttonColor = std::stoul(hexStr.substr(1), nullptr, 16) & 0x00FFFFFFu; } catch (...) {}
            }
        } else if (v.is_number()) {
            m_buttonColor = static_cast<std::uint32_t>(v.get<std::uint64_t>()) & 0x00FFFFFFu;
        }
    }
    // HUD editor fields
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    else if (j.contains("m_hudPosX")) hudPosX = j["m_hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    else if (j.contains("m_hudPosY")) hudPosY = j["m_hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_isHudModule")) isHudModule = j["m_isHudModule"].get<bool>();
    if (j.contains("m_hudEditMode")) m_hudEditMode = j["m_hudEditMode"].get<bool>();
    else if (j.contains("hudEditMode")) m_hudEditMode = j["hudEditMode"].get<bool>();

    for (std::size_t i = 0; i < MaxCommands; ++i) {
        const std::string p = "m_command" + std::to_string(i + 1);
        auto& b = m_commands[i];

        // Slot missing from the config -> keep the built-in default (enabled).
        if (!j.contains(p) || !j[p].is_boolean()) continue;

        // Slot explicitly disabled in the config -> no binding.
        if (!j[p].get<bool>()) {
            b = Binding{};
            continue;
        }

        b.enabled = true;
        if (j.contains(p + "Command")) b.command = j[p + "Command"].get<std::string>();
        if (j.contains(p + "Keybind")) b.key = j[p + "Keybind"].get<int>();
        if (j.contains(p + "Screen")) b.screen = j[p + "Screen"].get<bool>();
        if (j.contains(p + "X")) b.x = j[p + "X"].get<float>();
        if (j.contains(p + "Y")) b.y = j[p + "Y"].get<float>();
        if (j.contains(p + "Width")) b.width = j[p + "Width"].get<float>();
        if (j.contains(p + "Height")) b.height = j[p + "Height"].get<float>();
        if (j.contains(p + "TextSize")) b.textSize = j[p + "TextSize"].get<float>();
        if (j.contains(p + "TextColor")) {
            const auto& v = j[p + "TextColor"];
            if (v.is_string()) {
                std::string hexStr = v.get<std::string>();
                if (!hexStr.empty() && hexStr[0] == '#') {
                    try { b.textColor = std::stoul(hexStr.substr(1), nullptr, 16) & 0x00FFFFFFu; } catch (...) {}
                }
            } else if (v.is_number()) {
                b.textColor = static_cast<std::uint32_t>(v.get<std::uint64_t>()) & 0x00FFFFFFu;
            }
        }
        if (j.contains(p + "Label")) b.label = j[p + "Label"].get<std::string>();
    }

    // Migration: old versions stored absolute positions with hudPos == 0.
    // Convert to relative (hudPos + offset) so HUD editor group drag works correctly.
    // After migration the group's bounding box left/top becomes hudPos and relative coords start at 0.
    if (hudPosX == 0.0f && hudPosY == 0.0f) {
        float minX = std::numeric_limits<float>::max();
        float minY = std::numeric_limits<float>::max();
        bool has = false;
        for (auto &b : m_commands) if (b.enabled) {
            minX = std::min(minX, b.x);
            minY = std::min(minY, b.y);
            has = true;
        }
        if (has && (minX != 0.0f || minY != 0.0f)) {
            // Only migrate if min is not already 0 (avoid moving already-relative configs)
            // Check that at least one binding's position equals absolute expected (e.g., 24,80)
            // For safety, migrate only when hudPos was default 0 and we have absolute positions.
            hudPosX = minX;
            hudPosY = minY;
            for (auto &b : m_commands) if (b.enabled) {
                b.x -= minX;
                b.y -= minY;
            }
        }
    }

    normalizeBindings();

    // Keep the launcher-native overlay buttons in sync with the loaded slots.
    m_nativeButtonsDirty = true;
    m_nativeButtonRetries = 0;
    syncNativeButtons();
}

void CommandHotkeyModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["nativeButtons"] = nativeButtonsEnabled;
    j["m_buttonOpacity"] = m_buttonOpacity;
    j["m_buttonRadius"] = m_buttonRadius;

    char hexBuf[10];
    std::snprintf(hexBuf, sizeof(hexBuf), "#%06X", m_buttonColor & 0x00FFFFFFu);
    j["m_buttonColor"] = std::string(hexBuf);

    // HUD editor integration
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_hudEditMode"] = m_hudEditMode;

    // Always emit all command slots so ModMenu registers them at startup.
    // All slots exist directly in the menu (no Add Command / Remove buttons),
    // and toggling a slot off disables it.
    for (std::size_t i = 0; i < MaxCommands; ++i) {
        const std::string p = "m_command" + std::to_string(i + 1);
        const auto& b = m_commands[i];
        const bool enabled = b.enabled;

        j[p] = enabled;
        // Always emit child keys so they are registered and can be hidden via dependsOn
        // For disabled slots, emit defaults so toggle can be switched on and show fields immediately.
        if (enabled) {
            j[p + "Command"] = b.command;
            j[p + "Keybind"] = b.key;
            j[p + "Screen"] = b.screen;
            j[p + "X"] = b.x;
            j[p + "Y"] = b.y;
            j[p + "Width"] = b.width;
            j[p + "Height"] = b.height;
            j[p + "TextSize"] = b.textSize;
            char commandTextColor[10];
            std::snprintf(commandTextColor, sizeof(commandTextColor), "#%06X", b.textColor & 0x00FFFFFFu);
            j[p + "TextColor"] = std::string(commandTextColor);
            j[p + "Label"] = b.label;
        } else {
            // Emit defaults for disabled slots - ensures registration and instant visibility when enabled
            // Use per-index default position so each slot has distinct place when later enabled
            j[p + "Command"] = "";
            j[p + "Keybind"] = 0;
            j[p + "Screen"] = false;
            j[p + "X"] = 24.0f;
            j[p + "Y"] = 80.0f + static_cast<float>(i) * 52.0f;
            j[p + "Width"] = 110.0f;
            j[p + "Height"] = 40.0f;
            j[p + "TextSize"] = 20.0f;
            j[p + "TextColor"] = "#FFFFFF";
            j[p + "Label"] = "";
        }
    }
}
