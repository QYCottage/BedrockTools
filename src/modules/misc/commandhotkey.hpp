#pragma once

#include "../Module.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

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
        // Launcher keycap text color.
        std::uint32_t textColor = 0x373737;
        std::string label;
    };

    CommandHotkeyModule();
    ~CommandHotkeyModule() override;

    void onInit() override;
    void onFrame() override;
    bool onKeyEvent(int key, bool isDown) override;
    bool onTouchEvent(float x, float y, bool isDown) override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    static CommandHotkeyModule* instance();

private:
    std::array<Binding, MaxCommands> m_commands{};
    // Launcher on-screen button look ("keycap" preset used by the launcher's
    // own overlay buttons): light gray face, dark 2px border, tiny corner
    // radius and 0.85 alpha.
    float m_buttonOpacity = 0.85f;
    float m_buttonRadius = 2.0f;
    std::uint32_t m_buttonColor = 0x8B8B8B;
    std::uint32_t m_buttonBorderColor = 0x373737;
    float m_buttonBorderWidth = 2.0f;

    // HUD editor integration - group position
    float hudPosX = 0.0f;
    float hudPosY = 0.0f;
    bool isHudModule = true;

    // per-command drag via HUD edit mode (inside mod)
    bool m_hudEditMode = false;
    int m_draggingIndex = -1;
    // Short pressed highlight, mirroring the launcher's active button state.
    int m_pressedIndex = -1;
    std::chrono::steady_clock::time_point m_pressedTime{};
    float m_dragOffsetX = 0.0f;
    float m_dragOffsetY = 0.0f;

    void execute(std::size_t index);
    bool isPressed(std::size_t index) const;
    void applyDefaultBindings();
    void normalizeBindings();

    static std::string normalizeCommand(std::string command);
    static std::string defaultLabel(const Binding& binding, std::size_t index);
    bool inside(const Binding& binding, float x, float y) const;
    static void sendCommandPacket(const std::string& command);
};
