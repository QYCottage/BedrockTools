#pragma once

#include "../Module.hpp"
#include <atomic>
#include <cstdint>

// A native version of the Hit Range ground-circle indicator: a ring at melee
// reach around your feet and, optionally, around nearby players and mobs.
// Green follows the game's own crosshair verdict, so it only lights up when an
// attack would actually land; red means out of range, blocked, or not aimed.
class HitRangeModule final : public Module {
public:
    HitRangeModule();
    ~HitRangeModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Auto Reach uses 3 blocks in survival and 5 in creative, learned from the
    // game's own mode events. Reach is the manual radius used when it is off.
    float effectiveReach() const;
    void setCreativeMode(bool creative);

    float reach = 3.0f;
    float lineWidth = 0.10f;
    int style = 0;
    std::uint32_t inRangeColor = 0xFF35D07Fu;
    std::uint32_t outOfRangeColor = 0xFFFF453Au;
    bool autoReach = true;
    bool showSelf = true;
    bool showTargets = true;
    bool showPlayers = true;
    bool showMobs = false;
    bool seeThrough = false;

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
    std::atomic<bool> m_creative{false};
};
