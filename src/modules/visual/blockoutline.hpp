#pragma once

#include "../Module.hpp"
#include <cstdint>
#include <cstddef>

class BlockOutlineModule : public Module {
public:
    BlockOutlineModule();
    ~BlockOutlineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Stored as AARRGGBB.
    uint32_t color = 0xFFFFFFFF;
    bool rgb = false;
    float rgbSpeed = 1.0f;
    float lineThickness = 1.0f;
    float range = 20.0f;

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};
