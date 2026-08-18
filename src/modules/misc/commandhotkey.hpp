#pragma once

#include "../Module.hpp"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class CommandHotkeyModule final : public Module {
public:
    static constexpr std::size_t MaxCommands = 8;

    struct Binding {
        bool enabled = false;
        std::string command;
        int key = 0;
        bool screen = false;
        float x = 24.0f;
        float y = 80.0f;
        float width = 110.0f;
        float height = 40.0f;
        float textSize = 20.0f;
        std::uint32_t textColor = 0xFFFFFF;
        std::string label;
    };

    CommandHotkeyModule();
    ~CommandHotkeyModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onLauncherRegistered() override;
    void onFrame() override;
    bool onKeyEvent(int key, bool isDown) override;
    bool onTouchEvent(float x, float y, bool isDown) override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    static CommandHotkeyModule* instance();

    // On-screen slots are always launcher-native overlay buttons (same
    // Android views as Zoom / other launcher modules). There is no fallback
    // to the old in-mod drawn rectangles.
    bool nativeButtonsEnabled = true;

private:
    std::array<Binding, MaxCommands> m_commands{};
    float m_buttonOpacity = 0.78f;
    float m_buttonRadius = 7.0f;
    std::uint32_t m_buttonColor = 0x202020;

    // Launcher-native button state, one entry per command slot.
    std::array<bool, MaxCommands> m_nativeButtonRegistered{};
    std::array<std::string, MaxCommands> m_nativeButtonLabel;
    bool m_nativeButtonsDirty = true;
    int m_nativeButtonRetries = 0;

    // HUD editor integration - group position
    float hudPosX = 0.0f;
    float hudPosY = 0.0f;
    bool isHudModule = true;

    // per-command drag via HUD edit mode (inside mod)
    bool m_hudEditMode = false;
    int m_draggingIndex = -1;
    float m_dragOffsetX = 0.0f;
    float m_dragOffsetY = 0.0f;

    void execute(std::size_t index);
    void applyDefaultBindings();
    void normalizeBindings();
    void flushPendingCommands();
    void unregisterAllNativeButtons();

    // Registers/unregisters the launcher-native buttons for on-screen command
    // slots. Same threading model as the rest of this module (called from the
    // render thread via onFrame and from loadConfig).
    void syncNativeButtons();

    std::mutex m_pendingMutex;
    std::vector<std::string> m_pendingCommands;

    static std::string normalizeCommand(std::string command);
    static std::string defaultLabel(const Binding& binding, std::size_t index);
    bool inside(const Binding& binding, float x, float y) const;
    static void sendCommandPacket(const std::string& command);
};
