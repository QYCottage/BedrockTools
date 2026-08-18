#pragma once

#include "../Module.hpp"
#include "blockoutline_core.hpp"

#include <atomic>
#include <cstdint>

class BlockOutlineModule final : public Module {
public:
    BlockOutlineModule();
    ~BlockOutlineModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Settings exposed by the BedrockTools launcher.
    uint32_t outlineColor = 0xFFFFFFFFu;
    bool rgb = false;
    float rgbSpeed = 1.0f;
    float lineThickness = 2.0f;
    float maxDistance = 128.0f;
    bool threeD = false;

    // Pressing this button makes the module verify every runtime dependency
    // and stores the result in verificationStatus for the menu.
    bool verifyButton = false;
    const char* verificationStatus() const { return m_verificationStatus; }

public:
    void verifyRuntime();
    bool runtimeReady() const;
    void installRenderHook();

    bool m_initialized = false;
    bool m_hookInstalled = false;
    void* m_renderTarget = nullptr;
    const char* m_verificationStatus = "Not verified";

    // Render-side state is read atomically because the tick hook and the
    // RenderLevel hook can run on different threads.
    std::atomic<bool> m_hitValid{false};
    std::atomic<int> m_blockX{0};
    std::atomic<int> m_blockY{0};
    std::atomic<int> m_blockZ{0};
    std::atomic<float> m_hitX{0.0f};
    std::atomic<float> m_hitY{0.0f};
    std::atomic<float> m_hitZ{0.0f};
    std::atomic<float> m_playerX{0.0f};
    std::atomic<float> m_playerY{0.0f};
    std::atomic<float> m_playerZ{0.0f};

    float m_rgbHue = 0.0f;
};
