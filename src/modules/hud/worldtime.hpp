#pragma once

#include "../Module.hpp"
#include <atomic>
#include <cstdint>
#include <mutex>

class TimeChangerModule;

class WorldTimeModule : public Module {
public:
    WorldTimeModule();
    ~WorldTimeModule() override;

    void onInit() override;
    void onDisable() override;
    void onFrame() override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

private:
    TimeChangerModule* m_timeChanger = nullptr;
    std::uint64_t m_subscription = 0;
    std::atomic<int> m_dayTicks{-1};
    std::mutex m_configMutex;
    float hudPosX = 20.0f;
    float hudPosY = 220.0f;
    float m_size = 40.0f;
    bool m_use24HourClock = true;
    bool m_background = false;
    float m_backgroundOpacity = 0.5f;
};
