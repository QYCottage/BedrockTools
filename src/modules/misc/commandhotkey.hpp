#pragma once

#include "../Module.hpp"

#include <array>
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
        float width = 110.0f;
        float height = 40.0f;
        std::uint32_t textColor = 0x373737;
        std::string label;
    };

    CommandHotkeyModule();
    ~CommandHotkeyModule() override;

    void onInit() override;
    bool onKeyEvent(int key, bool isDown) override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    static CommandHotkeyModule* instance();

private:
    std::array<Binding, MaxCommands> m_commands{};

    // Uniform multiplier for the Zoom-style command button frame.
    // Width/Height on each Binding remain available for per-button sizing.
    float m_buttonScale = 1.0f;

    // Launcher styling values are kept in the config for compatibility with
    // older profiles and with launchers that use the fallback preset.
    float m_buttonOpacity = 0.85f;
    std::uint32_t m_buttonColor = 0x8B8B8B;
    std::uint32_t m_buttonBorderColor = 0x373737;

    void execute(std::size_t index);
    void applyDefaultBindings();
    void normalizeBindings();
    void syncOverlayButtons();
    void unregisterOverlayButtons();

    static std::string normalizeCommand(std::string command);
    static std::string defaultLabel(const Binding& binding, std::size_t index);
    static void sendCommandPacket(const std::string& command);
};
