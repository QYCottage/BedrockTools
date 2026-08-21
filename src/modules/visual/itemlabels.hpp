#pragma once

#include "../Module.hpp"

#include <string>

// Item Labels — holographic 3D name tags above dropped items.
//
// Every dropped item within range gets a floating label showing the item
// name and stack count, rendered by the game's own name-tag system (the
// same client-side synced-data mechanism the TNT Timer module uses), so the
// text is real world-space 3D text: it scales with distance, stays
// billboarded, and works in multiplayer without touching player.json or
// sending anything to the server.
//
// Visibility is decided per frame on the client:
//   * ray-trace occlusion — items hidden behind solid blocks (or inside
//     grass) only get a label when they are actually visible, and
//   * two targeting modes — nearest N items inside a radius, or only the
//     single item currently under the crosshair.
class ItemLabelsModule : public Module {
public:
    // 0 = nearest items in range, 1 = crosshair target only.
    int m_mode = 0;
    // Maximum distance (blocks) at which labels are shown.
    float m_maxDistance = 32.0f;
    // How many nearest items get labels in "nearest" mode.
    int m_maxItems = 8;
    // Hide labels behind solid blocks (ray-cast occlusion).
    bool m_occlusion = true;

    bool m_showCount = true;
    bool m_multiline = false;
    bool m_bold = false;
    bool m_italic = false;
    std::string m_prefix;
    std::string m_suffix;

    // "#RRGGBB" color for the label text (mapped to the closest vanilla
    // name-tag color code). Empty string falls back to white.
    std::string m_textColor = "#FFFFFF";

    ItemLabelsModule();
    ~ItemLabelsModule() override;

    void onInit() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Runs once per client tick: gathers item actors in range, applies the
    // targeting mode and ray-trace culling, and updates the 3D name tags.
    void updateLabels(void* player);

    // Exposed for tests.
    static std::string buildLabel(const std::string& itemName, int count,
                                  bool showCount, bool multiline, bool bold,
                                  bool italic, const std::string& prefix,
                                  const std::string& suffix,
                                  const std::string& textColor);
};
