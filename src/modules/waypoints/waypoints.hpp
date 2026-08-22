#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>
#include <vector>
#include <mutex>

class WaypointsModule : public Module {
public:
    WaypointsModule();
    ~WaypointsModule() override;

    void onInit()     override;
    void onEnable()   override;
    void onDisable()  override;
    void onFrame()    override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j)       override;

    // HUD settings
    float hudPosX = 100.f;
    float hudPosY = 150.f;
    bool isHudModule = true;

    // Waypoint Module toggles
    bool m_showWorldMarker = true;
    bool m_showName = true;
    bool m_showDistance = true;
    bool m_showDirection = true;
    bool m_showHUD = true;
    bool m_showOnlySelected = false;
    float m_maxRenderDistance = 200.0f;
    float m_textSize = 10.0f;
    float m_markerSize = 1.0f;

    // Add Waypoint button trigger
    bool m_addWaypointButton = false;

    // Slot 1
    bool m_wp1_enabled = false;
    std::string m_wp1_name = "Home";
    float m_wp1_x = 0.0f;
    float m_wp1_y = 64.0f;
    float m_wp1_z = 0.0f;
    std::string m_wp1_color = "#FF0000";
    bool m_wp1_deleteButton = false;

    // Slot 2
    bool m_wp2_enabled = false;
    std::string m_wp2_name = "Village";
    float m_wp2_x = 0.0f;
    float m_wp2_y = 64.0f;
    float m_wp2_z = 0.0f;
    std::string m_wp2_color = "#00FF00";
    bool m_wp2_deleteButton = false;

    // Slot 3
    bool m_wp3_enabled = false;
    std::string m_wp3_name = "Mine";
    float m_wp3_x = 0.0f;
    float m_wp3_y = 64.0f;
    float m_wp3_z = 0.0f;
    std::string m_wp3_color = "#0000FF";
    bool m_wp3_deleteButton = false;

    // Slot 4
    bool m_wp4_enabled = false;
    std::string m_wp4_name = "Nether Portal";
    float m_wp4_x = 0.0f;
    float m_wp4_y = 64.0f;
    float m_wp4_z = 0.0f;
    std::string m_wp4_color = "#FFFF00";
    bool m_wp4_deleteButton = false;

    // Slot 5
    bool m_wp5_enabled = false;
    std::string m_wp5_name = "Stronghold";
    float m_wp5_x = 0.0f;
    float m_wp5_y = 64.0f;
    float m_wp5_z = 0.0f;
    std::string m_wp5_color = "#FF00FF";
    bool m_wp5_deleteButton = false;

    void applyPatch();

private:
    bool m_patched = false;
    void* m_patchTarget = nullptr;
    void* m_tessBeginAddr = nullptr;
    void* m_tessColorAddr = nullptr;
    void* m_tessVertexAddr = nullptr;
    void* m_renderMesh2Addr = nullptr;
    void* m_renderMaterialGroupAddr = nullptr;

    void syncSlotsToManager();
    void syncManagerToSlots();
};
