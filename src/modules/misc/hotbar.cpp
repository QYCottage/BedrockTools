#include "hotbar.hpp"

#include "../ModuleRegistry.hpp"
#include "core/GameHooks.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace {

HotbarModule* g_instance = nullptr;

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
constexpr std::uint32_t kKeycapActiveBg = 0xC6C6C6;

std::string hotbarButtonId(std::size_t index) {
    return "bedrocktools.Hotbar.Button" + std::to_string(index + 1);
}

std::string launcherLabel(std::string value) {
    constexpr std::size_t maxBytes = 32;
    if (value.size() <= maxBytes) return value;
    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0u) == 0x80u) {
        --end;
    }
    value.resize(end);
    return value;
}

} // namespace

HotbarModule::HotbarModule()
    : Module("Hotbar", "Run commands or send chat when you select a hotbar slot (1-9) or press overlay buttons. Like Comment/Command hotkey but tied to hotbar.") {
    g_instance = this;
    showInMenu = true;
    hideInHudEditor = true;
    applyDefaultBindings();
}

HotbarModule::~HotbarModule() {
    unregisterOverlayButtons();
    if (g_instance == this) g_instance = nullptr;
}

HotbarModule* HotbarModule::instance() {
    return g_instance;
}

void HotbarModule::onInit() {
    syncOverlayButtons();
    // Subscribe to LocalPlayer tick for hotbar slot detection
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_instance) g_instance->onTick(event.player);
        });
}

void HotbarModule::applyDefaultBindings() {
    for (std::size_t i = 0; i < MaxSlots; ++i) {
        auto& b = m_bindings[i];
        b = Binding{};
        b.enabled = false;
        b.screen = false;
        b.width = 44.0f;
        b.height = 44.0f;
        b.textColor = 0xFFFFFF;
        b.label = "";
        b.type = ActionType::Command;
        b.text = "";
    }
    // Provide helpful defaults for first few slots (disabled until enabled)
    // Keep them disabled by default to not spam.
}

void HotbarModule::normalizeBindings() {
    for (auto& b : m_bindings) {
        if (!b.enabled) continue;
        if (b.text.size() > 256) b.text.resize(256);
        if (b.label.size() > 64) b.label.resize(64);
        b.width = std::clamp(b.width, 24.0f, 600.0f);
        b.height = std::clamp(b.height, 24.0f, 600.0f);
        b.textColor &= 0x00FFFFFFu;
    }
}

void HotbarModule::unregisterOverlayButtons() {
    for (std::size_t i = 0; i < MaxSlots; ++i)
        pl::modmenu::unregisterButton(hotbarButtonId(i));
}

void HotbarModule::syncOverlayButtons() {
    std::array<Binding, MaxSlots> bindings;
    float opacity;
    std::uint32_t faceColor, borderColor;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bindings = m_bindings;
        opacity = m_buttonOpacity;
        faceColor = m_buttonColor;
        borderColor = m_buttonBorderColor;
    }
    unregisterOverlayButtons();
    for (std::size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        if (!b.enabled || !b.screen) continue;
        const std::string label = launcherLabel(defaultLabel(b, i));
        const std::string idName = "Hotbar " + std::to_string(i + 1);
        pl::modmenu::ButtonBuilder builder(hotbarButtonId(i), idName);
        builder.moduleId(moduleId)
            .label(label)
            .behavior(pl::modmenu::ButtonBehavior::Click)
            .defaultVisible(true)
            .stylePreset(pl::modmenu::ButtonStylePreset::Keycap)
            .styleColors(colorWithAlpha(faceColor, opacity),
                         colorWithAlpha(kKeycapActiveBg, opacity),
                         colorWithAlpha(borderColor, opacity))
            .textColor(0xFF000000u | (b.textColor & 0x00FFFFFFu))
            .activeTextColor(0xFF1F1F1Fu)
            .sizeScale(b.width / kLauncherButtonBaseSize,
                       b.height / kLauncherButtonBaseSize)
            .onEvent([this, i](std::string_view, pl::modmenu::ButtonEvent event, float) {
                if (event == pl::modmenu::ButtonEvent::Click)
                    execute(i);
            });
        (void)builder.registerButton();
    }
}

std::string HotbarModule::normalizeCommand(std::string command) {
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.front()))) command.erase(command.begin());
    while (!command.empty() && std::isspace(static_cast<unsigned char>(command.back()))) command.pop_back();
    if (!command.empty() && command.front() != '/') command.insert(command.begin(), '/');
    return command;
}

std::string HotbarModule::defaultLabel(const Binding& binding, std::size_t index) {
    if (!binding.label.empty()) return binding.label;
    if (!binding.text.empty()) {
        std::string v = binding.text;
        // Strip leading slash for display
        if (!v.empty() && v.front() == '/') v.erase(v.begin());
        // Trim long
        if (v.size() > 16) v.resize(16);
        if (!v.empty()) return v;
    }
    return std::to_string(index + 1);
}

int HotbarModule::hotbarSlotFromKey(int key) {
    // GLFW / generic keyboard: 49-57 = '1'-'9'
    if (key >= 49 && key <= 57) return key - 49;
    // Android KeyEvent: KEYCODE_1=8 .. KEYCODE_9=16
    if (key >= 8 && key <= 16) return key - 8;
    // Numpad 1-9 often 257-265 in some mappings
    if (key >= 257 && key <= 265) return key - 257;
    if (key >= 320 && key <= 328) return key - 320; // alternative numpad
    // F1-F9 (290-298) as alternative hotbar for some launchers
    if (key >= 290 && key <= 298) return key - 290;
    return -1;
}

bool HotbarModule::canSendNow() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSendTime).count();
    return elapsed >= m_cooldownMs;
}

void HotbarModule::markSent() {
    m_lastSendTime = std::chrono::steady_clock::now();
}

void HotbarModule::execute(std::size_t slotIndex) {
    Binding binding;
    bool shouldSend = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!enabled) return;
        if (slotIndex >= MaxSlots) return;
        const auto& b = m_bindings[slotIndex];
        if (!b.enabled) return;
        if (b.text.empty()) return;
        if (!canSendNow()) return;
        // Prevent spam when quickly scrolling if enabled
        // We already have cooldown; additional scroll guard is optional
        binding = b;
        shouldSend = true;
        markSent();
    }
    if (!shouldSend) return;

    if (binding.type == ActionType::Command) {
        std::string cmd = normalizeCommand(binding.text);
        if (!cmd.empty()) sendCommand(cmd);
    } else {
        sendChat(binding.text);
    }
}

bool HotbarModule::onKeyEvent(int key, bool isDown) {
    if (!enabled || !isDown) return false;
    if (ModuleRegistry::get().keybindBlocked()) return false;

    bool triggerOnKeyCopy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        triggerOnKeyCopy = m_triggerOnKey;
    }
    if (!triggerOnKeyCopy) return false;

    int slot = hotbarSlotFromKey(key);
    if (slot < 0 || slot >= (int)MaxSlots) return false;

    // Update discovery: we expect player slot to become this slot shortly
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_configuredOffset == -1 && m_discoveredOffset == -1) {
            m_pendingExpectedSlot = slot;
            m_pendingTicks = 20; // ~1 second at 20tps
        }
    }

    // Check if this slot has an enabled binding
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (slot < 0 || slot >= (int)MaxSlots) return false;
        const auto& b = m_bindings[slot];
        if (!b.enabled) return false;
        if (b.text.empty()) return false;
        // Avoid double-trigger with onTick: mark this slot as already handled
        m_lastSlot = slot;
    }

    execute((std::size_t)slot);
    // Do not consume the key so the game still switches the visual hotbar slot.
    return false;
}

void HotbarModule::onTick(void* player) {
    if (!player || !enabled) return;

    // Handle configured offset override
    int configured;
    bool triggerOnSelectCopy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        configured = m_configuredOffset;
        triggerOnSelectCopy = m_triggerOnSelect;
    }

    // If triggerOnSelect is off, no need to poll slot
    if (!triggerOnSelectCopy) {
        // Still need to manage discovery if pending?
        if (configured == -1 && m_pendingExpectedSlot != -1) {
            // try to discover even if not triggering on select
            (void)getSelectedSlot(player);
        }
        return;
    }

    int slot = getSelectedSlot(player);
    if (slot < 0 || slot >= (int)MaxSlots) return;

    int last;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        last = m_lastSlot;
        if (slot == last) return;
        m_lastSlot = slot;
    }

    // Debounce rapid scroll: if preventSpamOnScroll and slot changed quickly multiple times,
    // we still execute each slot's command, but cooldown will throttle.
    // Also we want to avoid triggering when inventory is open? Could check screen state but skip for now.
    execute((std::size_t)slot);
}

int HotbarModule::getSelectedSlot(void* player) {
    if (!player) return -1;
    std::lock_guard<std::mutex> lock(m_mutex);

    // Manual offset override
    if (m_configuredOffset >= 0) {
        // Treat as byte/int offset: try int first
        // To be safe, read as int and as byte
        int valInt = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + m_configuredOffset);
        if (valInt >= 0 && valInt < 9) return valInt;
        // try byte
        std::uint8_t valByte = *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(player) + m_configuredOffset);
        if (valByte < 9) return (int)valByte;
        return -1;
    }
    if (m_configuredOffset == -2) {
        // disabled
        return -1;
    }

    // If already discovered, use it
    if (m_discoveredOffset >= 0) {
        int val = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + m_discoveredOffset);
        if (val >= 0 && val < 9) return val;
        // try byte reinterpret
        std::uint8_t b = *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(player) + m_discoveredOffset);
        if (b < 9) return (int)b;
        // discovered offset invalid now (maybe version mismatch), reset
        m_discoveredOffset = -1;
        m_candidates.clear();
        m_pendingExpectedSlot = -1;
        m_lastSlot = -1;
        return -1;
    }

    // Auto-discovery: if we have pending expected slot, filter candidates
    if (m_pendingExpectedSlot != -1) {
        if (m_candidates.empty()) {
            // Need to initialize candidates now (first tick after key press)
            tryFullScan(player);
            if (m_candidates.empty()) {
                // no candidates found, give up pending
                m_pendingTicks--;
                if (m_pendingTicks <= 0) m_pendingExpectedSlot = -1;
                return -1;
            }
        }
        // Filter candidates that match expected slot
        std::vector<int> filtered;
        filtered.reserve(m_candidates.size());
        for (int off : m_candidates) {
            // Check int
            int vInt = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + off);
            if (vInt == m_pendingExpectedSlot) {
                filtered.push_back(off);
                continue;
            }
            // Also check byte (lower byte of int)
            std::uint8_t vByte = *reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(player) + off);
            if ((int)vByte == m_pendingExpectedSlot) {
                // Ensure the surrounding 3 bytes are zero for int case? Not needed.
                // If byte matches but int doesn't, it's still candidate.
                // But we prefer int matching, so if byte matches alone, keep.
                filtered.push_back(off);
            }
        }
        if (filtered.size() == 1) {
            m_discoveredOffset = filtered[0];
            m_candidates.clear();
            m_pendingExpectedSlot = -1;
            m_pendingTicks = 0;
            // Immediately return the discovered value (should be expected)
            int val = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + m_discoveredOffset);
            if (val >=0 && val <9) return val;
            return m_pendingExpectedSlot;
        } else if (filtered.empty()) {
            m_pendingTicks--;
            if (m_pendingTicks <= 0) {
                m_pendingExpectedSlot = -1;
                // keep candidates for next expected?
            }
            return -1;
        } else {
            // Multiple candidates, narrow
            m_candidates = std::move(filtered);
            m_pendingTicks--;
            if (m_pendingTicks <= 0) {
                // If still multiple after timeout, pick most plausible? Keep first that looks like inventory region.
                // For now, keep pending but wait for next slot change to further filter.
                // Instead of clearing, keep candidates and reset pending to allow next key press to filter further.
                // If we have many candidates, we need another expected slot to distinguish.
                // So clear pending to wait for next hotbar press
                m_pendingExpectedSlot = -1;
            }
            return -1;
        }
    }

    // No pending, no discovered: we could still attempt to detect slot changes by heuristic:
    // Look for int that was previously different and now changed to something that looks like slot and is stable?
    // For now, do periodic scan if not discovered and no pending but we want to try to infer slot without key press
    // Use scan cooldown to avoid spamming
    if (m_candidates.empty() && m_scanCooldown <= 0) {
        // Do a passive scan: find all offsets with 0-8, keep them for future
        // But without expected slot we can't know which is correct, so just prepare candidates
        // Initialize candidates for next expected slot
        tryFullScan(player);
        m_scanCooldown = 100; // wait 100 ticks before rescanning
        return -1;
    }
    if (m_scanCooldown > 0) m_scanCooldown--;

    return -1;
}

void HotbarModule::tryFullScan(void* player) {
    m_candidates.clear();
    if (!player) return;
    // Scan range 0..4096 or 8192? Use 8192 to be safe but limit candidates
    constexpr int MaxScan = 8192;
    constexpr int MaxCandidates = 512;
    for (int off = 0; off < MaxScan; off += 4) {
        // Avoid reading unmapped? Player object is at least 4096, but 8192 may go out.
        // Use try/catch? In C++ we just read; if unmapped would crash.
        // We assume safe. Add guard: only scan up to 4096 for safety first, then expand if needed.
        // We'll scan 0..4096.
        if (off >= 4096) break;
        int v = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + off);
        if (v >= 0 && v < 9) {
            // Heuristic: the value should be stable for at least a few ticks and should not be common like 0 everywhere.
            // For now accept.
            m_candidates.push_back(off);
            if ((int)m_candidates.size() >= MaxCandidates) break;
        }
    }
    // If too many candidates (e.g., many zeros), filter zeros that are too common?
    // Count zeros: if >200 zeros, likely not correct, remove zeros?
    if (m_candidates.size() > 200) {
        std::vector<int> nonZero;
        for (int off : m_candidates) {
            int v = *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(player) + off);
            if (v != 0) nonZero.push_back(off);
        }
        if (!nonZero.empty() && nonZero.size() < m_candidates.size()) {
            m_candidates = nonZero;
        }
        if (m_candidates.size() > 300) m_candidates.resize(300);
    }
}

void HotbarModule::updateDiscovery(void* player, int expectedSlot) {
    // Not used directly; getSelectedSlot handles
}

void HotbarModule::sendChat(const std::string& text) {
    namespace Packet = bedrocktools::sdk::offsets::Packet;
    namespace TextPacketPayload = bedrocktools::sdk::offsets::TextPacketPayload;
    if (!resolvePacketFunctions()) return;
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return;
    std::shared_ptr<void> pktSp = g_createPacket(9);
    void* pkt = pktSp.get();
    if (!pkt) return;
    uintptr_t payload = reinterpret_cast<uintptr_t>(pkt) + Packet::Size;
    std::string* messageOnly = reinterpret_cast<std::string*>(payload + TextPacketPayload::MessageOnly::mMessage);
    messageOnly->~basic_string();
    *reinterpret_cast<uint32_t*>(payload + TextPacketPayload::mVariantIndex) = 1;
    *reinterpret_cast<uint8_t*>(payload + TextPacketPayload::AuthorAndMessage::mType) = 1;
    new (reinterpret_cast<void*>(payload + TextPacketPayload::AuthorAndMessage::mAuthor)) std::string();
    new (reinterpret_cast<void*>(payload + TextPacketPayload::AuthorAndMessage::mMessage)) std::string(text);
    void* sender = g_getPacketSender(client);
    if (sender) g_sendToServer(sender, pkt);
}

void HotbarModule::sendCommand(const std::string& command) {
    if (!resolvePacketFunctions()) return;
    void* client = bedrocktools::core::gamehooks::clientInstance();
    if (!client) return;
    std::shared_ptr<void> packet = g_createPacket(77);
    if (!packet) return;
    auto* raw = packet.get();
    const std::uintptr_t payload = reinterpret_cast<std::uintptr_t>(raw) + bedrocktools::sdk::offsets::Packet::Size;
    *reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::CommandRequestPacketPayload::mCommand) = command;
    *reinterpret_cast<std::uint8_t*>(payload + bedrocktools::sdk::offsets::CommandRequestPacketPayload::mOrigin + bedrocktools::sdk::offsets::CommandOriginData::mType) = 0;
    *reinterpret_cast<bool*>(payload + bedrocktools::sdk::offsets::CommandRequestPacketPayload::mInternalSource) = true;
    void* sender = g_getPacketSender(client);
    if (sender) g_sendToServer(sender, raw);
}

void HotbarModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    const bool legacyStyle = !j.contains("m_hotbarButtonBorderColor");
    auto readColor = [](const nlohmann::json& v, std::uint32_t& out) {
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            if (!s.empty() && s[0] == '#') {
                try { out = static_cast<std::uint32_t>(std::stoul(s.substr(1), nullptr, 16)) & 0x00FFFFFFu; } catch (...) {}
            }
        } else if (v.is_number()) {
            out = static_cast<std::uint32_t>(v.get<std::uint64_t>()) & 0x00FFFFFFu;
        }
    };

    float loadedOpacity = m_buttonOpacity;
    std::uint32_t loadedColor = m_buttonColor;
    std::uint32_t loadedBorder = m_buttonBorderColor;
    if (!legacyStyle) {
        if (j.contains("m_hotbarButtonOpacity") && j["m_hotbarButtonOpacity"].is_number())
            loadedOpacity = std::clamp(j["m_hotbarButtonOpacity"].get<float>(), 0.05f, 1.0f);
        if (j.contains("m_hotbarButtonColor")) readColor(j["m_hotbarButtonColor"], loadedColor);
        if (j.contains("m_hotbarButtonBorderColor")) readColor(j["m_hotbarButtonBorderColor"], loadedBorder);
    }

    int loadedCooldown = m_cooldownMs;
    if (j.contains("m_hotbarCooldown") && j["m_hotbarCooldown"].is_number_integer())
        loadedCooldown = std::clamp(j["m_hotbarCooldown"].get<int>(), 0, 60000);
    bool loadedTriggerOnSelect = m_triggerOnSelect;
    bool loadedTriggerOnKey = m_triggerOnKey;
    if (j.contains("m_hotbarTriggerOnSelect") && j["m_hotbarTriggerOnSelect"].is_boolean())
        loadedTriggerOnSelect = j["m_hotbarTriggerOnSelect"].get<bool>();
    if (j.contains("m_hotbarTriggerOnKey") && j["m_hotbarTriggerOnKey"].is_boolean())
        loadedTriggerOnKey = j["m_hotbarTriggerOnKey"].get<bool>();
    int loadedOffset = m_configuredOffset;
    if (j.contains("m_hotbarSelectedOffset") && j["m_hotbarSelectedOffset"].is_number_integer())
        loadedOffset = j["m_hotbarSelectedOffset"].get<int>();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_buttonOpacity = loadedOpacity;
        m_buttonColor = loadedColor;
        m_buttonBorderColor = loadedBorder;
        m_cooldownMs = loadedCooldown;
        m_triggerOnSelect = loadedTriggerOnSelect;
        m_triggerOnKey = loadedTriggerOnKey;
        m_configuredOffset = loadedOffset;
        if (m_configuredOffset >= 0) {
            m_discoveredOffset = m_configuredOffset;
            m_candidates.clear();
        } else if (m_configuredOffset == -2) {
            m_discoveredOffset = -1;
            m_candidates.clear();
        } else {
            // auto: keep discovered if already found, else reset
            if (m_discoveredOffset != -1 && m_configuredOffset == -1) {
                // keep
            } else {
                m_discoveredOffset = -1;
            }
        }

        applyDefaultBindings();
        for (std::size_t i = 0; i < MaxSlots; ++i) {
            const std::string p = "m_hotbar" + std::to_string(i + 1);
            auto& b = m_bindings[i];
            if (!j.contains(p) || !j[p].is_boolean()) continue;
            if (!j[p].get<bool>()) {
                b = Binding{};
                continue;
            }
            b.enabled = true;
            b.screen = true;
            if (j.contains(p + "Text") && j[p + "Text"].is_string())
                b.text = j[p + "Text"].get<std::string>();
            if (j.contains(p + "Type") && j[p + "Type"].is_number_integer())
                b.type = (j[p + "Type"].get<int>() == 0 ? ActionType::Chat : ActionType::Command);
            else if (j.contains(p + "IsCommand") && j[p + "IsCommand"].is_boolean())
                b.type = j[p + "IsCommand"].get<bool>() ? ActionType::Command : ActionType::Chat;
            if (j.contains(p + "Screen") && j[p + "Screen"].is_boolean())
                b.screen = j[p + "Screen"].get<bool>();
            if (j.contains(p + "Width") && j[p + "Width"].is_number())
                b.width = j[p + "Width"].get<float>();
            if (j.contains(p + "Height") && j[p + "Height"].is_number())
                b.height = j[p + "Height"].get<float>();
            if (!legacyStyle && j.contains(p + "TextColor")) {
                const auto& v = j[p + "TextColor"];
                if (v.is_string()) {
                    std::string s = v.get<std::string>();
                    if (!s.empty() && s[0] == '#') {
                        try { b.textColor = std::stoul(s.substr(1), nullptr, 16) & 0x00FFFFFFu; } catch (...) {}
                    }
                } else if (v.is_number()) {
                    b.textColor = static_cast<std::uint32_t>(v.get<std::uint64_t>()) & 0x00FFFFFFu;
                }
            }
            if (j.contains(p + "Label") && j[p + "Label"].is_string())
                b.label = j[p + "Label"].get<std::string>();
        }
        normalizeBindings();
        m_lastSlot = -1;
        // keep discovery but reset pending
        m_pendingExpectedSlot = -1;
        m_pendingTicks = 0;
    }
    syncOverlayButtons();
}

void HotbarModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard<std::mutex> lock(m_mutex);
    j["m_hotbarButtonOpacity"] = m_buttonOpacity;
    char hexBuf[10];
    std::snprintf(hexBuf, sizeof(hexBuf), "#%06X", m_buttonColor & 0x00FFFFFFu);
    j["m_hotbarButtonColor"] = std::string(hexBuf);
    char hexBorder[10];
    std::snprintf(hexBorder, sizeof(hexBorder), "#%06X", m_buttonBorderColor & 0x00FFFFFFu);
    j["m_hotbarButtonBorderColor"] = std::string(hexBorder);
    j["m_hotbarCooldown"] = m_cooldownMs;
    j["m_hotbarTriggerOnSelect"] = m_triggerOnSelect;
    j["m_hotbarTriggerOnKey"] = m_triggerOnKey;
    j["m_hotbarSelectedOffset"] = m_configuredOffset;

    for (std::size_t i = 0; i < MaxSlots; ++i) {
        const std::string p = "m_hotbar" + std::to_string(i + 1);
        const auto& b = m_bindings[i];
        j[p] = b.enabled;
        if (b.enabled) {
            j[p + "Text"] = b.text;
            j[p + "Type"] = (b.type == ActionType::Chat ? 0 : 1);
            j[p + "Screen"] = b.screen;
            j[p + "Width"] = b.width;
            j[p + "Height"] = b.height;
            char tc[10];
            std::snprintf(tc, sizeof(tc), "#%06X", b.textColor & 0x00FFFFFFu);
            j[p + "TextColor"] = std::string(tc);
            j[p + "Label"] = b.label;
        } else {
            j[p + "Text"] = "";
            j[p + "Type"] = 1;
            j[p + "Screen"] = false;
            j[p + "Width"] = 44.0f;
            j[p + "Height"] = 44.0f;
            j[p + "TextColor"] = "#FFFFFF";
            j[p + "Label"] = "";
        }
    }
}
