#pragma once

#include "../Module.hpp"

#include <atomic>

// Keeps the vanilla HUD paper doll (HudPlayerRenderer) visible at all times
// so players can always see their skin and armor while playing.
class PaperDollModule final : public Module {
public:
    PaperDollModule();
    ~PaperDollModule() override;

    void onInit() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool alwaysVisible() const { return m_alwaysVisible.load(std::memory_order_relaxed); }
    void setAlwaysVisible(bool value) { m_alwaysVisible.store(value, std::memory_order_relaxed); }

    // Called from the HudPlayerRenderer::render detour right before invoking
    // the original renderer.
    void beforeRender(void* renderer) const;

private:
    std::atomic_bool m_alwaysVisible{true};
};
