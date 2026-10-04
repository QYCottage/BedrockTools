#pragma once

#include "../Module.hpp"
#include <atomic>

class FreelookModule : public Module {
public:
    FreelookModule();
    ~FreelookModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool requested() const;
    void resetInput();

private:
    void updateButton();

    std::atomic_bool m_available{false};
    std::atomic_bool m_buttonActive{false};
    std::atomic_bool m_keyActive{false};
    bool m_overlayToggle = true;
    bool m_buttonRegistered = false;
};
