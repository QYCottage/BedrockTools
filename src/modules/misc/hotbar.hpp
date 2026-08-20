#pragma once

#include "../Module.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class HotbarModule final : public Module {
public:
    static constexpr std::size_t MaxSlots = 9;

    enum class ActionType : int {
        Chat = 0,
        Command = 1
    };

    struct Binding {
        bool enabled = false;
        ActionType type = ActionType::Command;
        std::string text; // command (with or without /) or chat message
        bool screen = false;
        float width = 44.0f;
        float height = 44.0f;
        std::uint32_t textColor = 0xFFFFFF;
        std::string label; // overlay button label, empty = auto
    };

    HotbarModule();
    ~HotbarModule() override;

    void onInit() override;
    bool onKeyEvent(int key, bool isDown) override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    static HotbarModule* instance();

    // Execute the binding for a given hotbar slot (0..8)
    void execute(std::size_t slotIndex);

    const std::array<Binding, MaxSlots>& bindings() const { return m_bindings; }

    // For manual testing: force discover offset
    int discoveredOffset() const { return m_discoveredOffset; }

private:
    std::array<Binding, MaxSlots> m_bindings{};

    // Launcher keycap styling forwarded to each overlay button
    float m_buttonOpacity = 0.85f;
    std::uint32_t m_buttonColor = 0x8B8B8B;
    std::uint32_t m_buttonBorderColor = 0x373737;

    int m_cooldownMs = 300;
    std::chrono::steady_clock::time_point m_lastSendTime{};

    // Hotbar detection
    int m_lastSlot = -1;
    bool m_triggerOnSelect = true;
    bool m_triggerOnKey = true;
    bool m_preventSpamOnScroll = true;

    // Auto-discovery of selected slot offset inside Player object.
    // -1 = auto (scan), >=0 = fixed offset, -2 = disabled
    int m_configuredOffset = -1;
    int m_discoveredOffset = -1;
    std::vector<int> m_candidates;
    int m_pendingExpectedSlot = -1;
    int m_pendingTicks = 0;
    int m_scanCooldown = 0;
    mutable std::mutex m_mutex;

    void applyDefaultBindings();
    void normalizeBindings();
    void syncOverlayButtons();
    void unregisterOverlayButtons();

    // Called from LocalPlayerTickEvent
    void onTick(void* player);
    int getSelectedSlot(void* player);
    void updateDiscovery(void* player, int expectedSlot);
    void tryFullScan(void* player);

    static std::string normalizeCommand(std::string command);
    static std::string defaultLabel(const Binding& binding, std::size_t index);
    static int hotbarSlotFromKey(int key);

    void sendChat(const std::string& text);
    void sendCommand(const std::string& command);
    bool canSendNow();
    void markSent();
};
