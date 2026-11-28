#pragma once

#include "../Module.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

// Controls the vanilla HUD paper doll (HudPlayerRenderer):
//  1. Position offset (X / Y)
//  2. Scale (size multiplier)
//  3. Rotation / auto-spin
//  4. Hide in containers / chat screens
//  5. Smart visibility modes (Always, Combat/Action, Vanilla)
class PaperDollModule final : public Module {
public:
    enum class VisibilityMode : int {
        Always = 0,
        CombatOnly = 1,
        Vanilla = 2,
    };

    struct ControlState {
        bool modifiedPos = false;
        bool modifiedSize = false;
        float origPosX = 0.0f;
        float origPosY = 0.0f;
        float origSizeX = 0.0f;
        float origSizeY = 0.0f;
    };

    PaperDollModule();
    ~PaperDollModule() override;

    void onInit() override;
    void onFrame() override;
    void onDisable() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // HUD Editor position
    float hudPosX() const { return m_hudPosX.load(std::memory_order_relaxed); }
    float hudPosY() const { return m_hudPosY.load(std::memory_order_relaxed); }
    void setHudPos(float x, float y);

    // Visibility mode
    VisibilityMode mode() const { return static_cast<VisibilityMode>(m_mode.load(std::memory_order_relaxed)); }
    void setMode(VisibilityMode m) { m_mode.store(static_cast<int>(m), std::memory_order_relaxed); }

    bool alwaysVisible() const { return mode() == VisibilityMode::Always; }
    void setAlwaysVisible(bool value) { setMode(value ? VisibilityMode::Always : VisibilityMode::Vanilla); }

    // Position offset
    float offsetX() const { return m_offsetX.load(std::memory_order_relaxed); }
    float offsetY() const { return m_offsetY.load(std::memory_order_relaxed); }
    void setOffset(float x, float y);

    // Scale
    float scale() const { return m_scale.load(std::memory_order_relaxed); }
    void setScale(float s);

    // Rotation
    float rotation() const { return m_rotation.load(std::memory_order_relaxed); }
    void setRotation(float deg);
    bool autoSpin() const { return m_autoSpin.load(std::memory_order_relaxed); }
    void setAutoSpin(bool value) { m_autoSpin.store(value, std::memory_order_relaxed); }

    // Hide in containers
    bool hideInContainer() const { return m_hideInContainer.load(std::memory_order_relaxed); }
    void setHideInContainer(bool value) { m_hideInContainer.store(value, std::memory_order_relaxed); }
    bool hiddenByScreen() const { return hideInContainer() && m_screenDepth.load(std::memory_order_relaxed) > 0; }

    // Combat trigger (called from AttackEvent or tests)
    void notifyCombatActivity();
    void onScreenStateChanged(bool opened);

    // Hook callbacks around HudPlayerRenderer::render
    // Returns false if rendering should be skipped completely.
    bool beforeRender(void* renderer, void* control, ControlState& state) const;
    void afterRender(void* control, const ControlState& state) const;

    // Convenience overload for tests / callers without a UIControl
    void beforeRender(void* renderer) const;

private:
    bool isCombatActive() const;

    std::atomic_int m_mode{static_cast<int>(VisibilityMode::Always)};
    std::atomic<float> m_hudPosX{16.0f};
    std::atomic<float> m_hudPosY{16.0f};
    std::atomic_bool m_useHudPos{false};
    std::atomic<float> m_offsetX{0.0f};
    std::atomic<float> m_offsetY{0.0f};
    std::atomic<float> m_scale{1.0f};
    std::atomic<float> m_rotation{0.0f};
    std::atomic_bool m_autoSpin{false};
    std::atomic_bool m_hideInContainer{false};
    std::atomic_int m_screenDepth{0};
    std::atomic<std::int64_t> m_lastCombatMs{0};
};
