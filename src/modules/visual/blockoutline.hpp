#pragma once

#include "../Module.hpp"
#include <cstdint>

class BlockOutlineModule final : public Module {
public:
    BlockOutlineModule();
    ~BlockOutlineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& json) override;
    void saveConfig(nlohmann::json& json) override;

    std::uint32_t outlineColor = 0xFF00FFFFu;

private:
    void installRenderHook();

    bool m_hookInstalled = false;
    void* m_renderLevel = nullptr;
};
