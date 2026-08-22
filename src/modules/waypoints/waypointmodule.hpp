#pragma once
#include "../Module.hpp"
#include "WaypointManager.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <vector>

class WaypointModule final : public Module {
public:
    WaypointModule();
    ~WaypointModule() override;
    void onInit() override;
    void onFrame() override;
    void onEnable() override;
    void loadConfig(const nlohmann::json&) override;
    void saveConfig(nlohmann::json&) override;

    std::string mode = "Multi Block,Single Block";
    std::string waypointName = "Trap";
    std::string color = "#FF0000";
    bool fill = true;
    bool outline = true;
    float fillOpacity = .20f;
    float outlineOpacity = .95f;
    float outlineThickness = 1.f;
    float maxRenderDistance = 200.f;
    bool showLabel = true;
    bool showDistance = true;
    float hudPosX = 100.f;
    float hudPosY = 120.f;
    bool createWaypointButton = false;
    bool startSelectionButton = false;
    bool addBlockButton = false;
    bool removeBlockButton = false;
    bool clearBlocksButton = false;
    bool saveButton = false;
    bool deleteButton = false;

private:
    void installRenderHook();
    void handleActions();
    static BlockWaypoint makeWaypoint(const std::string&, const std::vector<BlockPosition>&, const std::string&, bool, bool, float, float, float, int);
    bool m_hookInstalled = false;
    std::vector<BlockPosition> m_selection;
};
