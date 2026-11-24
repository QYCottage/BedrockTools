#pragma once

#include "../Module.hpp"
#include <cstdint>

// A native version of the Hit Range ground-circle indicator: a ring at melee
// reach around your feet and, optionally, around nearby players and mobs.
// Green means a target can be hit; red means it cannot.
class HitRangeModule final : public Module {
public:
    HitRangeModule();
    ~HitRangeModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float reach = 3.0f;
    float lineWidth = 0.10f;
    int style = 0;
    std::uint32_t inRangeColor = 0xFF35D07Fu;
    std::uint32_t outOfRangeColor = 0xFFFF453Au;
    bool showSelf = true;
    bool showTargets = true;
    bool showPlayers = true;
    bool showMobs = false;
    bool seeThrough = false;

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
};
