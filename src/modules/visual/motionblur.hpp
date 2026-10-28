#pragma once

#include "../Module.hpp"
#include <atomic>
#include <memory>

class MotionBlurModule : public Module {
public:
    MotionBlurModule();
    ~MotionBlurModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;
    void captureCamera(void* camera, void* player);
    void captureWorld(void* renderer);
    void beforeClear(unsigned int mask);

private:
    struct State;
    std::unique_ptr<State> m_state;
    std::atomic_bool m_active{false};
    std::atomic<float> m_intensity{0.90f};
    std::atomic<float> m_opacity{0.40f};
    std::atomic<float> m_resolutionScale{0.5f};
    std::atomic<int> m_samples{6};
    std::atomic_bool m_depthBlur{true};
};

void MotionBlurCaptureCamera(void* camera, void* player);
