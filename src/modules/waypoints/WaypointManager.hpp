#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

struct Waypoint {
    std::string name;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    bool enabled = true;
    int dimensionId = 0; // 0: Overworld, 1: Nether, 2: End
};

class WaypointManager {
public:
    static WaypointManager& get() {
        static WaypointManager instance;
        return instance;
    }

    std::vector<Waypoint>& getWaypoints() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_waypoints;
    }

    void setWaypoints(const std::vector<Waypoint>& wps) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waypoints = wps;
        saveToFile();
    }

    void addWaypoint(const Waypoint& wp) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waypoints.push_back(wp);
        saveToFile();
    }

    void saveToFile() {
        std::string dir = "/sdcard/games/BedrockTools";
        std::string path = dir + "/waypoints.json";
        try {
            std::filesystem::create_directories(dir);
            nlohmann::json j = nlohmann::json::array();
            for (const auto& wp : m_waypoints) {
                nlohmann::json item;
                item["name"] = wp.name;
                item["x"] = wp.x;
                item["y"] = wp.y;
                item["z"] = wp.z;
                item["r"] = wp.r;
                item["g"] = wp.g;
                item["b"] = wp.b;
                item["enabled"] = wp.enabled;
                item["dimensionId"] = wp.dimensionId;
                j.push_back(item);
            }
            std::ofstream outFile(path);
            if (outFile.is_open()) {
                outFile << j.dump(4);
            }
        } catch (...) {}
    }

    void loadFromFile() {
        std::string path = "/sdcard/games/BedrockTools/waypoints.json";
        if (!std::filesystem::exists(path)) return;
        try {
            std::ifstream inFile(path);
            if (!inFile.is_open()) return;
            nlohmann::json j;
            inFile >> j;
            if (j.is_array()) {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_waypoints.clear();
                for (const auto& item : j) {
                    Waypoint wp;
                    wp.name = item.value("name", "Waypoint");
                    wp.x = item.value("x", 0.0f);
                    wp.y = item.value("y", 0.0f);
                    wp.z = item.value("z", 0.0f);
                    wp.r = item.value("r", 1.0f);
                    wp.g = item.value("g", 1.0f);
                    wp.b = item.value("b", 1.0f);
                    wp.enabled = item.value("enabled", true);
                    wp.dimensionId = item.value("dimensionId", 0);
                    m_waypoints.push_back(wp);
                }
            }
        } catch (...) {}
    }

private:
    WaypointManager() {
        loadFromFile();
    }
    ~WaypointManager() = default;

    std::vector<Waypoint> m_waypoints;
    std::mutex m_mutex;
};
