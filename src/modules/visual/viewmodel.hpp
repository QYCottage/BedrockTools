#pragma once

#include "../Module.hpp"

struct ViewModelHandTransform {
    float positionX = 0.0f;
    float positionY = 0.0f;
    float positionZ = 0.0f;
    float rotationX = 0.0f;
    float rotationY = 0.0f;
    float rotationZ = 0.0f;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    float scaleZ = 1.0f;
};

class ViewModelModule : public Module {
public:
    ViewModelModule();
    ~ViewModelModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    float getItemFov() const;

    ViewModelHandTransform m_mainHand;
    ViewModelHandTransform m_offHand;
    float m_itemFov = 70.0f;
    bool m_swapHands = false;
    bool m_applyThirdPerson = false;

private:
    void installHooks();
    void removeHooks();
    bool m_hooksInstalled = false;
    bool m_renderFirstPersonHooked = false;
    bool m_renderHandHooked = false;
    bool m_actorRenderDispatcherHooked = false;
    bool m_renderItemHooked = false;
    bool m_jsonTransformHooked = false;
    bool m_defaultTransformHooked = false;
    bool m_renderObjectHooked = false;
};

ViewModelModule* GetViewModelModule();
