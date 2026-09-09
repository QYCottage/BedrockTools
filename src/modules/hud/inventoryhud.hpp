#pragma once

#include "../Module.hpp"
#include "inventoryhud_layout.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

// Shows the player's main inventory (container slots 9-35, the 9x3 grid of
// the inventory screen) as item icons on the HUD, so the contents are visible
// without opening the inventory.
//
// Armor and the offhand are handled by the separate Armor module: this module
// owns nothing but the inventory grid.
//
// It shares its plumbing with the Armor module and Hotbar Slots (see
// huditems.hpp): the stacks come straight from the player's FillingContainer
// and the icons are painted by the game's ItemRenderer from the
// HudCameraRenderer hook. Stack counts and durability bars use launcher
// overlay draw commands, like the other HUD modules' text.
class InventoryHudModule final : public Module {
public:
    static constexpr std::size_t GridSlotCount = bedrocktools::inventoryhud::GridSlotCount;

    InventoryHudModule();
    ~InventoryHudModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the shared HudCameraRenderer detour.
    void renderNative(void* context, void* client);

    // Icons are hidden while the real inventory / container screen is open;
    // the counter mirrors the ScreenStateEvent container depth.
    bool hiddenByScreen() const;

private:
    struct SlotRuntime {
        std::atomic_bool hasItem{false};
        std::atomic<std::uint8_t> count{0};
        std::atomic_int damage{0};
        std::atomic_int maxDamage{0};
    };

    struct ConfigSnapshot {
        bedrocktools::inventoryhud::GridLayout grid{};
        bool stackCount = true;
        bool durability = true;
        bool hideInContainer = true;
        bool slotBackground = true;
        std::uint32_t slotBgColor = 0x73000000u; // "#000000" at 45%
        float countTextSize = 12.0f;
        std::uint32_t countColor = 0xFFFFFFFFu;
        float gridSize = 16.0f;
        float gridGap = 4.0f;
        float snapThreshold = 12.0f;
        std::uint32_t snapFlags = 0;
    };

    ConfigSnapshot snapshotConfig() const;
    // Expects m_configMutex to be held.
    bedrocktools::inventoryhud::GridLayout gridLayout() const;
    void clearRuntime();
    void storeRuntime(SlotRuntime& runtime, void* stack, void* item, bool wantDurability);

    mutable std::mutex m_configMutex;
    std::array<SlotRuntime, GridSlotCount> m_grid;
    std::atomic_int m_containerDepth{0};

    float hudPosX = 24.0f;
    float hudPosY = 200.0f;
    int m_columns = static_cast<int>(bedrocktools::inventoryhud::DefaultColumns);
    float m_slotSize = 32.0f;
    float m_slotGap = 4.0f;
    bool m_showStackCount = true;
    bool m_showDurability = true;
    bool m_hideInContainer = true;
    bool m_slotBackground = true;
    float m_slotBgOpacity = 0.45f;
    std::string m_slotBgColor = "#000000";
    float m_countTextSize = 12.0f;
    std::string m_countColor = "#FFFFFF";

    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;
};
