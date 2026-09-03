#pragma once

#include "../Module.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <string>
#include <chrono>

class TimeChangerModule;

class DebugMenuModule : public Module {
public:
    DebugMenuModule();
    ~DebugMenuModule() override;

    void onInit()     override;
    void onEnable()   override;
    void onDisable()  override;
    void onFrame()    override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j)       override;
    
    void updateFrameTiming(std::chrono::steady_clock::time_point now);
    void updateData(float yaw, float pitch, const bedrocktools::sdk::Vec3& pos);

    void* m_level = nullptr;
    bool  m_levelInitHooked = false;
    bool  m_levelDtorHooked = false;
    int   m_entityCount = -1;
    int   m_worldTime = 0;
    bool  m_worldTimeValid = false;

public:
    bedrocktools::sdk::Vec3  m_lastPos = {0.f, 0.f, 0.f};
    bool  m_firstTick = true;
    float m_speed = 0.f;
    float m_frameTimeMs = 0.f;
    int m_fps = -1;
    unsigned int m_frameCount = 0;
    std::chrono::steady_clock::time_point m_sampleStart{};
    std::chrono::steady_clock::time_point m_lastFrameTime{};
    bool m_hasFrameTime = false;

    float m_yaw = 0.f;
    float m_pitch = 0.f;
    bedrocktools::sdk::Vec3  m_pos = {0.f, 0.f, 0.f};
    bedrocktools::sdk::Vec3  m_velocity = {0.f, 0.f, 0.f};
    float hudPosX = 50.f;
    float hudPosY = 50.f;
    bool isHudModule = true;

    bool  m_showCrosshair = true;
    float m_lineLength = 41.f;
    float m_lineThick = 2.6f;
    float m_outlineThick = 6.6f;
    float m_lerpSpeed = 10.f;
    bool  m_showTipDot = false;

    bool  m_showOverlay = true;
    float m_textScale = 1.72f;
    
public:
    std::string m_worldName = "N/A";
    std::string m_biomeName = "Unknown";
    
    float m_lerpYaw = 0.f;
    float m_lerpPitch = 0.f;

    void* m_clientInstance = nullptr;
    bool m_cursorHooked = false;
    void* m_cursorPatchTarget = nullptr;

    bool m_cacheInit = false;
    std::string m_cachedDeviceName;
    std::string m_cachedCpuName;
    std::string m_cachedAbi;
    unsigned long m_totalMemMb = 0;
    unsigned long m_usedMemMb = 0;
    TimeChangerModule* m_timeChanger = nullptr;
};
