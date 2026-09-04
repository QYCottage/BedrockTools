#pragma once

#include "../Module.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class PotionHudModule : public Module {
public:
    PotionHudModule();
    ~PotionHudModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool renderNative(void* context, void* client);

private:
    struct RuntimeEffect {
        std::uint32_t id;
        int duration;
        int amplifier;
        bool noCounter;
        bool nativeIcon;
    };

    struct ConfigSnapshot {
        float hudPosX;
        float hudPosY;
        float uiScale;
        float spacing;
        bool bottomUp;
        bool showText;
        bool showTitle;
        bool useRoman;
        bool useRomanFull;
        float textSize;
        float textOffsetX;
        int textSide;
        bool textShadow;
        float shadowOffset;
        std::uint32_t mainColor;
        std::uint32_t lowColor;
        std::uint32_t shadowColor;
        float gridSize;
        float gridGap;
        float snapThreshold;
        std::uint32_t snapFlags;
    };

    ConfigSnapshot snapshotConfig() const;
    std::vector<RuntimeEffect> snapshotRuntime(float& surfaceScale) const;
    void clearRuntime();
    void submitEditorElement(const ConfigSnapshot& config, const std::vector<RuntimeEffect>& effects, float surfaceScale);
    static float iconSurfaceSize(const ConfigSnapshot& config, float surfaceScale);
    static float rowSurfaceHeight(const ConfigSnapshot& config, float surfaceScale);
    static float textSurfaceWidth(const ConfigSnapshot& config, const std::vector<RuntimeEffect>& effects, float surfaceScale);
    static std::string titleForEffect(const RuntimeEffect& effect, const ConfigSnapshot& config);

    mutable std::mutex m_configMutex;
    mutable std::mutex m_runtimeMutex;
    std::vector<RuntimeEffect> m_runtimeEffects;
    float m_surfaceScale = 1.0f;
    std::atomic_bool m_runtimeValid{false};

    float hudPosX = 24.0f;
    float hudPosY = 80.0f;
    float m_uiScale = 1.0f;
    float m_spacing = 1.0f;
    bool m_bottomUp = false;
    bool m_showText = true;
    bool m_showTitle = true;
    bool m_useRoman = true;
    bool m_useRomanFull = true;
    float m_textSize = 8.0f;
    float m_textOffsetX = 2.0f;
    int m_textSide = 0;
    bool m_textShadow = true;
    float m_shadowOffset = 1.0f;
    std::string m_mainColor = "#FFFFFFFF";
    std::string m_lowColor = "#FFFF4040";
    std::string m_shadowColor = "#8C000000";
    float m_gridSize = 16.0f;
    float m_gridGap = 4.0f;
    float m_snapThreshold = 12.0f;
    bool m_snapToGrid = true;
    bool m_snapToElements = true;
    bool m_snapToScreenCenter = true;
};
