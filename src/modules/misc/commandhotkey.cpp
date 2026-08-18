#include "commandhotkey.hpp"

#include "../ModuleRegistry.hpp"
#include "core/GameHooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

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

constexpr float kLauncherButtonBaseSize = 52.0f;

// LeviLauncher derives an independent persisted HUD position from each stable
// button ID ("external_button:<id>").
std::string commandButtonId(std::size_t index) {
    return "bedrocktools.CommandHotkey.Button" + std::to_string(index + 1);
}

std::string launcherLabel(std::string value) {
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

// Launcher overlay button palette (keycap preset).
constexpr std::uint32_t kKeycapActiveBg = 0xC6C6C6;

} // namespace

CommandHotkeyModule::CommandHotkeyModule()
    : Module("Command Hotkey", "Run custom commands from keyboard keys or on-screen mobile buttons.") {
    g_instance = this;
    showInMenu = true;
    // On-screen commands are launcher overlay buttons. The parent module has
    // no custom draw surface of its own in the HUD editor.
    hideInHudEditor = true;
    // All command slots exist from the start (no "Add Command" button needed).
    applyDefaultBindings();
}

CommandHotkeyModule::~CommandHotkeyModule() {
    unregisterOverlayButtons();
    if (g_instance == this) g_instance = nullptr;
}

CommandHotkeyModule* CommandHotkeyModule::instance() {
    return g_instance;
}

void CommandHotkeyModule::onInit() {
    // Input hooks are installed once by Runtime. The launcher owns each
    // on-screen button and its independent HUD-editor position.
    syncOverlayButtons();
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

void CommandHotkeyModule::applyDefaultBindings() {
    for (std::size_t i = 0; i < MaxCommands; ++i) {
        m_commands[i] = Binding{};
        m_commands[i].enabled = true;
        m_commands[i].screen = true;
        m_commands[i].width = 110.0f;
        m_commands[i].height = 40.0f;
        m_commands[i].label = "Command " + std::to_string(i + 1);
        m_commands[i].textColor = 0x373737;
    }
}

void CommandHotkeyModule::normalizeBindings() {
    for (auto& binding : m_commands) {
        if (!binding.enabled) continue;
        if (binding.command.size() > 256) binding.command.resize(256);
        if (binding.label.size() > 64) binding.label.resize(64);
        binding.width = std::clamp(binding.width, 40.0f, 600.0f);
        binding.height = std::clamp(binding.height, 24.0f, 160.0f);
        binding.textColor &= 0x00FFFFFFu;
    }
}

void CommandHotkeyModule::unregisterOverlayButtons() {
    for (std::size_t i = 0; i < MaxCommands; ++i)
        pl::modmenu::unregisterButton(commandButtonId(i));
}

void CommandHotkeyModule::syncOverlayButtons() {
    unregisterOverlayButtons();
    for (std::size_t i = 0; i < MaxCommands; ++i) {
        const auto& binding = m_commands[i];
        if (!binding.enabled || !binding.screen)
            continue;

        const std::string label = launcherLabel(defaultLabel(binding, i));
        const std::string displayName = "Command " + std::to_string(i + 1);
        pl::modmenu::ButtonBuilder builder(commandButtonId(i), displayName);
        builder.moduleId(moduleId)
            .label(label)
            .behavior(pl::modmenu::ButtonBehavior::Click)
            .defaultVisible(true)
            .stylePreset(pl::modmenu::ButtonStylePreset::Keycap)
            .styleColors(colorWithAlpha(m_buttonColor, m_buttonOpacity),
                         colorWithAlpha(kKeycapActiveBg, m_buttonOpacity),
                         colorWithAlpha(m_buttonBorderColor, m_buttonOpacity))
            .textColor(0xFF000000u | (binding.textColor & 0x00FFFFFFu))
            .activeTextColor(0xFF1F1F1Fu)
            .sizeScale(binding.width / kLauncherButtonBaseSize,
                       binding.height / kLauncherButtonBaseSize)
            .onEvent([this, i](std::string_view, pl::modmenu::ButtonEvent event, float) {
                if (event == pl::modmenu::ButtonEvent::Click)
                    execute(i);
            });
        (void)builder.registerButton();
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

    // Configs written before the launcher-style button look have no border
    // entry. For those the stored face color / radius / opacity (and the old
    // white label color) are ignored so the buttons pick up the launcher look.
    const bool legacyStyle = !j.contains("m_buttonBorderColor");

    if (!legacyStyle && j.contains("m_buttonOpacity")) m_buttonOpacity = std::clamp(j["m_buttonOpacity"].get<float>(), 0.05f, 1.0f);
    if (j.contains("m_buttonBorderColor")) {
        const auto& v = j["m_buttonBorderColor"];
        if (v.is_string()) {
            std::string hexStr = v.get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { m_buttonBorderColor = std::stoul(hexStr.substr(1), nullptr, 16) & 0x00FFFFFFu; } catch (...) {}
            }
        } else if (v.is_number()) {
            m_buttonBorderColor = static_cast<std::uint32_t>(v.get<std::uint64_t>()) & 0x00FFFFFFu;
        }
    }
    if (!legacyStyle && j.contains("m_buttonColor")) {
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
        if (j.contains(p + "Width")) b.width = j[p + "Width"].get<float>();
        if (j.contains(p + "Height")) b.height = j[p + "Height"].get<float>();
        if (!legacyStyle && j.contains(p + "TextColor")) {
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

    normalizeBindings();
    syncOverlayButtons();
}

void CommandHotkeyModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["m_buttonOpacity"] = m_buttonOpacity;

    char borderHexBuf[10];
    std::snprintf(borderHexBuf, sizeof(borderHexBuf), "#%06X", m_buttonBorderColor & 0x00FFFFFFu);
    j["m_buttonBorderColor"] = std::string(borderHexBuf);

    char hexBuf[10];
    std::snprintf(hexBuf, sizeof(hexBuf), "#%06X", m_buttonColor & 0x00FFFFFFu);
    j["m_buttonColor"] = std::string(hexBuf);

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
            j[p + "Width"] = b.width;
            j[p + "Height"] = b.height;
            char commandTextColor[10];
            std::snprintf(commandTextColor, sizeof(commandTextColor), "#%06X", b.textColor & 0x00FFFFFFu);
            j[p + "TextColor"] = std::string(commandTextColor);
            j[p + "Label"] = b.label;
        } else {
            // Emit defaults for disabled slots so their settings are available
            // immediately when the slot is enabled.
            j[p + "Command"] = "";
            j[p + "Keybind"] = 0;
            j[p + "Screen"] = false;
            j[p + "Width"] = 110.0f;
            j[p + "Height"] = 40.0f;
            j[p + "TextColor"] = "#373737";
            j[p + "Label"] = "";
        }
    }
}
