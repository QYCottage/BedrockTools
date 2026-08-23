#include "commandhotkey.hpp"

#include "../ModuleRegistry.hpp"
#include "core/GameHooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
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

// The command buttons use the same Minecraft-style square frame as the
// Zoom overlay button. The label is kept visible so every command remains
// identifiable; the SVG supplies the frame while ButtonBuilder supplies the
// command text.
static const char* commandButtonSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
</svg>)svg";

static const char* commandButtonActiveSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <g transform="translate(32, 32) scale(0.85) translate(-32, -32)">
        <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
    </g>
</svg>)svg";

constexpr float kDefaultCommandButtonWidth = 64.0f;
constexpr float kDefaultCommandButtonHeight = 64.0f;

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
        // Command slots start disabled. Users can enable only the buttons they
        // need from the module settings.
        // Match the square Zoom button by default. Width and Height remain
        // per-command controls for users who want a custom button shape.
        m_commands[i].width = kDefaultCommandButtonWidth;
        m_commands[i].height = kDefaultCommandButtonHeight;
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
        if (!binding.enabled)
            continue;

        const std::string label = launcherLabel(defaultLabel(binding, i));
        const std::string displayName = "Command " + std::to_string(i + 1);
        pl::modmenu::ButtonBuilder builder(commandButtonId(i), displayName);
        builder.moduleId(moduleId)
            .label(label)
            .behavior(pl::modmenu::ButtonBehavior::Click)
            .defaultVisible(true)
            // Use the same custom Minecraft-style frame as Zoom. Keeping the
            // label visible makes the individual command buttons distinct.
            .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
            .styleColors(0x00000001, 0x00000001, 0x00000001)
            .svgIcon(commandButtonSvg, false)
            .activeSvgIcon(commandButtonActiveSvg)
            .textColor(0xFF000000u | (binding.textColor & 0x00FFFFFFu))
            .activeTextColor(0xFF1F1F1Fu)
            .sizeScale((binding.width / kLauncherButtonBaseSize) * m_buttonScale,
                       (binding.height / kLauncherButtonBaseSize) * m_buttonScale)
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
    // ModMenu edits text fields one character at a time. The native button
    // definitions are refreshed below so the launcher's overlay snapshot picks
    // up the new command/label text. The overlay view itself is then updated
    // in place by ExternalButtonRefresh (no hide/show), so there is no flicker
    // while the command/label fields are being typed.
    const auto previousBindings = m_commands;
    const float previousScale = m_buttonScale;
    Module::loadConfig(j);

    // Start from the built-in defaults (all slots exist directly but are
    // disabled) and then override each slot with whatever is stored in the config.
    applyDefaultBindings();

    // Configs written before the launcher-style button look have no border
    // entry. For those the stored face color / radius / opacity (and the old
    // white label color) are ignored so the buttons pick up the launcher look.
    const bool legacyStyle = !j.contains("m_buttonBorderColor");

    if (j.contains("m_buttonScale") && j["m_buttonScale"].is_number())
        m_buttonScale = std::clamp(j["m_buttonScale"].get<float>(), 0.5f, 2.0f);
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

        // Slot missing from the config -> keep the built-in default (disabled).
        if (!j.contains(p) || !j[p].is_boolean()) continue;

        // Slot explicitly disabled in the config -> no binding.
        if (!j[p].get<bool>()) {
            b = Binding{};
            continue;
        }

        b.enabled = true;
        if (j.contains(p + "Command")) b.command = j[p + "Command"].get<std::string>();
        if (j.contains(p + "Keybind")) b.key = j[p + "Keybind"].get<int>();
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

    // Re-register the native button definitions whenever anything shown on a
    // button changes. This includes the command and label text: the launcher
    // keeps its own snapshot of each ExternalButton, so the native definition
    // must be refreshed first, after which ExternalButtonRefresh re-applies the
    // new label in place. Registering a duplicate button id is rejected, so
    // syncOverlayButtons unregisters before re-registering.
    bool overlayChanged = previousScale != m_buttonScale;
    for (std::size_t i = 0; i < MaxCommands && !overlayChanged; ++i) {
        const auto& oldBinding = previousBindings[i];
        const auto& newBinding = m_commands[i];
        overlayChanged = oldBinding.enabled != newBinding.enabled ||
                         oldBinding.width != newBinding.width ||
                         oldBinding.height != newBinding.height ||
                         oldBinding.command != newBinding.command ||
                         oldBinding.label != newBinding.label ||
                         oldBinding.textColor != newBinding.textColor;
    }
    if (overlayChanged) syncOverlayButtons();
}

void CommandHotkeyModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    // Uniform multiplier for all command buttons. Individual Width/Height
    // values below still allow per-command fine tuning.
    j["m_buttonScale"] = m_buttonScale;
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
            j[p + "Width"] = kDefaultCommandButtonWidth;
            j[p + "Height"] = kDefaultCommandButtonHeight;
            j[p + "TextColor"] = "#373737";
            j[p + "Label"] = "";
        }
    }
}
