#pragma once

#include "../Module.hpp"

class ConnectedGlassModule final : public Module {
public:
    ConnectedGlassModule();
    ~ConnectedGlassModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool connectGlassBlocks = true;
    bool connectGlassPanes = true;
    bool connectDifferentColors = false;
    bool connectPanesToBlocks = false;
    bool removeFlatBorders = true;
    bool removeCornerBorders = true;
    bool removeOuterVerticalBorders = false;
    bool removeOuterHorizontalBorders = false;
    bool removeOuterTopBottomFaceBorders = false;
    bool affectSideFaces = true;
    bool affectTopFace = true;
    bool affectBottomFace = true;
    float borderWidth = 2.0f;

private:
    bool m_hooked = false;
    void installHooks();
    void applySettings();
};

void ConnectedGlassHandleClientInstanceUpdate(void* clientInstance);
void ConnectedGlassAdjustTessellatorVertex(void* tessellator, float x, float z);
