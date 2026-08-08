#pragma once

// Shulker preview module — thanks to Kashifro
// GitHub: https://github.com/Kashifro

#include "../Module.hpp"

class ShulkerPreviewModule : public Module {
public:
    float m_tintIntensity = 2.0f;
    float m_positionX = 8.0f;
    float m_positionY = 8.0f;
    bool m_followSelectedShulker = false;

    ShulkerPreviewModule();
    ~ShulkerPreviewModule() override;

    void onInit() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    bool m_hooksInstalled = false;
};

void ShulkerPreviewHandleContainerDestroyed(void* controller);
