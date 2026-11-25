#pragma once

#include "../Module.hpp"
#include "hitrange_geometry.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>

// A native version of the Hit Range ground-circle indicator: a ring at melee
// reach around your feet and, optionally, around nearby players and mobs.
// The radius is always derived from the detected game mode (3 survival /
// 5 creative); there is no manual Reach setting. Green follows an in-range,
// frontal, unobstructed target rather than the exact crosshair, and attack
// frames cannot resize, flash or briefly drop the rings.
class HitRangeModule final : public Module {
public:
    HitRangeModule();
    ~HitRangeModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
    bool showInLegacyMenu(std::string_view key) const override;

    // Effective reach in blocks: survival 3, creative 5. The live Player's
    // validated game-mode vtable wins; weaker mode-event hints are debounced.
    float effectiveReach() const;
    void setGameMode(void* gameMode);
    void setCreativeMode(bool creative); // immediate, validated read
    void observeGameMode(bool creative, bool authoritative = false);

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
    std::atomic<void*> m_gameMode{nullptr};
    std::atomic<bool> m_creative{false};
    mutable std::mutex m_modeMutex;
    hitrange::StableMode m_mode{};
};
