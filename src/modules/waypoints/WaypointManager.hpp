#pragma once

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>
#include <utility>

struct BlockPosition {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const BlockPosition& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct BlockWaypoint {
    std::string name = "Waypoint";
    std::vector<BlockPosition> blocks;
    float r = 1.0f;
    float g = 0.0f;
    float b = 0.0f;
    bool enabled = true;
    int dimensionId = 0;
    bool fill = true;
    bool outline = true;
    float fillOpacity = 0.20f;
    float outlineOpacity = 0.95f;
    float outlineThickness = 1.0f;
};

class WaypointManager {
public:
    static WaypointManager& get() { static WaypointManager instance; return instance; }

    std::vector<BlockWaypoint> snapshot() const {
        std::lock_guard lock(m_mutex);
        return m_waypoints;
    }
    void replace(std::vector<BlockWaypoint> value) {
        std::lock_guard lock(m_mutex);
        m_waypoints = std::move(value);
    }
    void add(BlockWaypoint value) {
        std::lock_guard lock(m_mutex);
        m_waypoints.push_back(std::move(value));
    }
    void clear() {
        std::lock_guard lock(m_mutex);
        m_waypoints.clear();
    }

    static void toJson(const BlockWaypoint& wp, nlohmann::json& j) {
        j = { {"name", wp.name}, {"color", {wp.r, wp.g, wp.b}}, {"enabled", wp.enabled},
              {"dimensionId", wp.dimensionId}, {"fill", wp.fill}, {"outline", wp.outline},
              {"fillOpacity", wp.fillOpacity}, {"outlineOpacity", wp.outlineOpacity},
              {"outlineThickness", wp.outlineThickness}, {"blocks", nlohmann::json::array()} };
        for (const auto& p : wp.blocks) j["blocks"].push_back({{"x", p.x}, {"y", p.y}, {"z", p.z}});
    }

    static BlockWaypoint fromJson(const nlohmann::json& j) {
        BlockWaypoint wp;
        wp.name = j.value("name", "Waypoint");
        wp.enabled = j.value("enabled", true);
        wp.dimensionId = j.value("dimensionId", 0);
        wp.fill = j.value("fill", true);
        wp.outline = j.value("outline", true);
        wp.fillOpacity = std::clamp(j.value("fillOpacity", .20f), 0.f, 1.f);
        wp.outlineOpacity = std::clamp(j.value("outlineOpacity", .95f), 0.f, 1.f);
        wp.outlineThickness = std::clamp(j.value("outlineThickness", 1.f), 1.f, 10.f);
        if (j.contains("color") && j["color"].is_array() && j["color"].size() >= 3) {
            wp.r = j["color"][0].get<float>(); wp.g = j["color"][1].get<float>(); wp.b = j["color"][2].get<float>();
        } else { wp.r = j.value("r", 1.f); wp.g = j.value("g", 0.f); wp.b = j.value("b", 0.f); }
        if (j.contains("blocks") && j["blocks"].is_array()) for (const auto& p : j["blocks"])
            if (p.is_object()) wp.blocks.push_back({p.value("x", 0), p.value("y", 0), p.value("z", 0)});
        return wp;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<BlockWaypoint> m_waypoints;
};
