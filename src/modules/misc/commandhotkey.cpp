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
#include <string>
#include <string_view>
#include <vector>

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

void CommandHotkeyModule::execute(std::size_t index) {
    if (!enabled || index >= MaxCommands) return;
    auto& binding = m_commands[index];
    if (!binding.enabled) return;

    const auto command = normalizeCommand(binding.command);
    if (command.empty()) return;

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

bool CommandHotkeyModule::onTouchEvent(float x, float y, bool isDown) {
    if (!enabled || ModuleRegistry::get().keybindBlocked()) return false;

    // HUD edit mode: drag individual buttons instead of executing
    if (m_hudEditMode) {
        if (isDown) {
            // Touch down -> try to grab a button
            for (std::size_t i = 0; i < MaxCommands; ++i) {
                const auto& binding = m_commands[i];
                if (!binding.enabled || !binding.screen) continue;
                if (!inside(binding, x, y)) continue;
                m_draggingIndex = static_cast<int>(i);
                m_dragOffsetX = x - (binding.x + hudPosX);
                m_dragOffsetY = y - (binding.y + hudPosY);
                return true;
            }
            m_draggingIndex = -1;
            return false;
        } else {
            // Move / Up -> if dragging, update position
            if (m_draggingIndex >= 0 && m_draggingIndex < static_cast<int>(MaxCommands)) {
                auto& b = m_commands[m_draggingIndex];
                if (!b.enabled) {
                    m_draggingIndex = -1;
                    return false;
                }
                float newAbsX = x - m_dragOffsetX;
                float newAbsY = y - m_dragOffsetY;
                // Clamp absolute position 0..10000 then convert to relative
                float clampedAbsX = std::clamp(newAbsX, 0.0f, 10000.0f);
                float clampedAbsY = std::clamp(newAbsY, 0.0f, 10000.0f);
                b.x = clampedAbsX - hudPosX;
                b.y = clampedAbsY - hudPosY;
                // also clamp relative to keep inside reasonable range
                b.x = std::clamp(b.x, -5000.0f, 10000.0f);
                b.y = std::clamp(b.y, -5000.0f, 10000.0f);
                normalizeBindings();
                bedrocktools::config::ConfigManager::get().save();
                return true;
            }
            return false;
        }
    }

    // Normal mode: execute on tap down
    if (!isDown) return false;

    for (std::size_t i = 0; i < MaxCommands; ++i) {
        const auto& binding = m_commands[i];
        if (!binding.enabled || !binding.screen) continue;
        if (!inside(binding, x, y)) continue;
        execute(i);
        return true;
    }
    return false;
}

void CommandHotkeyModule::onFrame() {
    if (!enabled) {
        std::vector<PLModMenu_DrawCommand> empty;
        submitDrawCommands(moduleId, empty);
        return;
    }

    std::vector<PLModMenu_DrawCommand> commands;
    commands.reserve(MaxCommands * 3 + 1);
    std::vector<std::string> labels;
    labels.reserve(MaxCommands);

    // HUD editor group hitbox: compute union of all screen buttons and add transparent bg so gaps are draggable
    {
        float minRelX = std::numeric_limits<float>::max();
        float minRelY = std::numeric_limits<float>::max();
        float maxRelX = std::numeric_limits<float>::lowest();
        float maxRelY = std::numeric_limits<float>::lowest();
        bool hasAny = false;
        for (std::size_t j = 0; j < MaxCommands; ++j) {
            const auto& b = m_commands[j];
            if (!b.enabled || !b.screen) continue;
            minRelX = std::min(minRelX, b.x);
            minRelY = std::min(minRelY, b.y);
            maxRelX = std::max(maxRelX, b.x + b.width);
            maxRelY = std::max(maxRelY, b.y + b.height);
            hasAny = true;
        }
        if (hasAny) {
            PLModMenu_DrawCommand groupBg{};
            groupBg.type = PL_DRAW_RECT_FILLED;
            groupBg.x = hudPosX + minRelX;
            groupBg.y = hudPosY + minRelY;
            groupBg.w = maxRelX - minRelX;
            groupBg.h = maxRelY - minRelY;
            groupBg.x3 = m_buttonRadius;
            groupBg.color = 0x02000000; // nearly transparent, makes whole group draggable in HUD editor
            commands.push_back(groupBg);
        }
    }

    for (std::size_t i = 0; i < MaxCommands; ++i) {
        const auto& binding = m_commands[i];
        if (!binding.enabled || !binding.screen) continue;

        float absX = binding.x + hudPosX;
        float absY = binding.y + hudPosY;

        PLModMenu_DrawCommand rect{};
        rect.type = PL_DRAW_RECT_FILLED;
        rect.x = absX;
        rect.y = absY;
        rect.w = binding.width;
        rect.h = binding.height;
        rect.x3 = m_buttonRadius;
        // Highlight dragging button in HUD edit mode
        if (m_hudEditMode && m_draggingIndex == static_cast<int>(i)) {
            rect.color = colorWithAlpha(0x4AE0A0, 0.95f);
        } else {
            rect.color = colorWithAlpha(m_buttonColor, m_buttonOpacity);
        }
        commands.push_back(rect);

        // In HUD edit mode draw an outline to indicate draggable
        if (m_hudEditMode) {
            PLModMenu_DrawCommand outline{};
            outline.type = PL_DRAW_RECT;
            outline.x = absX;
            outline.y = absY;
            outline.w = binding.width;
            outline.h = binding.height;
            outline.x3 = m_buttonRadius;
            outline.color = 0xFF4AE0A0;
            outline.size = 1.0f;
            commands.push_back(outline);
        }

        labels.push_back(defaultLabel(binding, i));
        PLModMenu_DrawCommand text{};
        text.type = PL_DRAW_TEXT;
        text.x = absX;
        text.y = absY;
        text.w = binding.width;
        text.h = binding.height;
        // The overlay forwards this value to Android Paint.setTextSize(), so it
        // must be a usable pixel size rather than the old 0.5–4 scale value.
        text.size = binding.textSize;
        text.color = 0xFF000000u | (binding.textColor & 0x00FFFFFFu);
        text.text = labels.back().c_str();
        commands.push_back(text);
    }

    submitDrawCommands(moduleId, commands);
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
}

void CommandHotkeyModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

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
