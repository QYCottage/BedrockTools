#pragma once

#include "../Module.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class ArmorHudModule : public Module {
public:
    ArmorHudModule();
    ~ArmorHudModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void renderNative(void* context, void* client);

private:
    struct SlotConfig {
        bool enabled;
        bool durability;
        float x;
        float y;
        float size;
    };

    struct SlotRuntime {
        std::atomic_bool hasItem{false};
        std::atomic_int damage{0};
        std::atomic_int maxDamage{0};
    };

    struct ConfigSnapshot {
        std::array<SlotConfig, 6> slots;
        bool showDamage;
        bool showRemaining;
        bool showMaxDurability;
        bool showPercentage;
        float durabilityTextSize;
        int durabilityTextPosition;
        float durabilityTextGap;
        std::uint32_t textColor;
        float gridSize;
        float gridGap;
        float snapThreshold;
        std::uint32_t snapFlags;
    };

    ConfigSnapshot snapshotConfig() const;
    void clearRuntime();
    void submitEditorElements(const ConfigSnapshot& config);

    mutable std::mutex m_configMutex;
    std::array<SlotRuntime, 6> m_runtime;

    bool m_helmet = true;
    bool m_helmetDurability = true;
    float hudHelmetPosX = 24.0f;
    float hudHelmetPosY = 80.0f;
    float m_helmetSize = 48.0f;

    bool m_chestplate = true;
    bool m_chestplateDurability = true;
    float hudChestplatePosX = 24.0f;
    float hudChestplatePosY = 152.0f;
    float m_chestplateSize = 48.0f;

    bool m_leggings = true;
    bool m_leggingsDurability = true;
    float hudLeggingsPosX = 24.0f;
    float hudLeggingsPosY = 224.0f;
    float m_leggingsSize = 48.0f;

    bool m_boots = true;
    bool m_bootsDurability = true;
    float hudBootsPosX = 24.0f;
    float hudBootsPosY = 296.0f;
    float m_bootsSize = 48.0f;

    bool m_offhand = true;
    bool m_offhandDurability = true;
    float hudOffhandPosX = 24.0f;
    float hudOffhandPosY = 368.0f;
    float m_offhandSize = 48.0f;

    bool m_mainhand = true;
    bool m_mainhandDurability = true;
    float hudMainhandPosX = 24.0f;
    float hudMainhandPosY = 440.0f;
    float m_mainhandSize = 48.0f;

    bool m_showDamage = true;
    bool m_showRemaining = true;
    bool m_showMaxDurability = true;
    bool m_showPercentage = true;
    float m_durabilityTextSize = 14.0f;
    int m_durabilityTextPosition = 0;
    float m_durabilityTextGap = 4.0f;
    std::string m_textColor = "#FFFFFF";
    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;
};
