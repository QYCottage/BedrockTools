#include "debugmenu.hpp"
#include <bedrocktools/Version.hpp>
#include "modules/ModuleRegistry.hpp"
#include "modules/player/timechanger.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include "core/memory/Hooks.hpp"

#include <cmath>
#include <vector>
#include <list>
#include <cstring>
#include <chrono>
#include <sys/system_properties.h>
#include <sys/sysinfo.h>
#include <string>
#include <random>
#include <EGL/egl.h>
#include <atomic>
#include "core/Runtime.hpp"
#include <fstream>
#include <iterator>
static DebugMenuModule* g_debugMod = nullptr;

typedef void (*HudCursorRender_t)(void* _this, void* a1, void* a2, void* a3);
static HudCursorRender_t s_origCursorRender = nullptr;

static void s_cursorRenderHook(void* _this, void* a1, void* a2, void* a3) {
    if (g_debugMod) {
        g_debugMod->m_clientInstance = a2;
        if (g_debugMod->enabled && g_debugMod->m_showCrosshair) return;
    }
    if (s_origCursorRender) s_origCursorRender(_this, a1, a2, a3);
}

typedef void* (*BlockSource_getBiome_t)(void* blockSource, const void* blockPos);
static BlockSource_getBiome_t s_getBiome = nullptr;

struct BlockPos {
    int x, y, z;
    BlockPos(float _x, float _y, float _z) {
        x = (int)std::floor(_x);
        y = (int)std::floor(_y);
        z = (int)std::floor(_z);
    }
};

static void s_debugTickCallback(void* _this) {
    if (!g_debugMod || !g_debugMod->enabled || !_this) return;

    uintptr_t rotComp = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);

    float pitch = 0.f, yaw = 0.f;
    bedrocktools::sdk::Vec3 pos = {0.f, 0.f, 0.f};

    if (rotComp != 0) {
        pitch = *(float*)(rotComp + 0);
        yaw   = *(float*)(rotComp + 4);
    }
    if (svc != 0) {
        pos = *(bedrocktools::sdk::Vec3*)svc;
    }

    g_debugMod->updateData(yaw, pitch, pos);

    if (s_getBiome) {
        uintptr_t dimension = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mDimension);
        if (dimension) {
            uintptr_t blockSource = *(uintptr_t*)(dimension + bedrocktools::sdk::offsets::Dimension::mBlockSource);
            if (blockSource) {
                BlockPos bp(pos.x, pos.y, pos.z);
                void* biome = s_getBiome((void*)blockSource, &bp);
                if (biome) {
                    std::string* biomeNamePtr = (std::string*)((uintptr_t)biome + bedrocktools::sdk::offsets::Biome::mHash + 8);
                    if (biomeNamePtr && !biomeNamePtr->empty()) {
                        g_debugMod->m_biomeName = *biomeNamePtr;
                    } else {
                        g_debugMod->m_biomeName = "Unknown";
                    }
                }
            }
        }
    }
}

typedef std::vector<void*> (*GetRuntimeActorList_t)(void* actorManager);
static GetRuntimeActorList_t s_getRuntimeActorList = nullptr;

typedef bool (*LevelInitialize_t)(void*              _this,
                                  const std::string* levelName,
                                  void*              levelSettings,
                                  void*              experiments,
                                  const std::string* levelId,
                                  void*              biomeOptLo,
                                  void*              biomeOptHi);
static LevelInitialize_t s_origLevelInitialize = nullptr;

static bool s_levelInitializeHook(void*              _this,
                                  const std::string* levelName,
                                  void*              levelSettings,
                                  void*              experiments,
                                  const std::string* levelId,
                                  void*              biomeOptLo,
                                  void*              biomeOptHi) {
    if (g_debugMod) {
        if (levelName && !levelName->empty()) {
            g_debugMod->m_worldName = *levelName;
        }
        g_debugMod->m_level = _this;
    }
    if (s_origLevelInitialize)
        return s_origLevelInitialize(_this, levelName, levelSettings,
                                     experiments, levelId, biomeOptLo, biomeOptHi);
    return false;
}

typedef void (*LevelDtorVtable0_t)(void* _this);
static LevelDtorVtable0_t s_origLevelDtorVtable0 = nullptr;

static void s_levelDtorVtable0Hook(void* _this) {
    if (g_debugMod) {
        g_debugMod->m_worldName = "N/A";
        g_debugMod->m_level = nullptr;
        g_debugMod->m_entityCount = -1;
        g_debugMod->m_worldTime = 0;
        g_debugMod->m_worldTimeValid = false;
    }
    if (s_origLevelDtorVtable0) s_origLevelDtorVtable0(_this);
}

static void s_installLevelHooks(DebugMenuModule* mod) {
    if (!mod->m_levelInitHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelInit);
        if (addr) {
            bedrocktools::hooks::install((void*)addr,
                      (void*)s_levelInitializeHook,
                      (void**)&s_origLevelInitialize);
            mod->m_levelInitHooked = true;
        }
    }

    if (!mod->m_levelDtorHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelDtor);
        if (addr) {
            bedrocktools::hooks::install((void*)addr,
                      (void*)s_levelDtorVtable0Hook,
                      (void**)&s_origLevelDtorVtable0);
            mod->m_levelDtorHooked = true;
        }
    }
}

DebugMenuModule::DebugMenuModule() : Module("Debug Menu", "Displays advanced debug info on screen") {
    g_debugMod = this;
    hideInHudEditor = true;
    m_showOverlay = true;
    m_showCrosshair = true;
    isHudModule = false;
}

DebugMenuModule::~DebugMenuModule() {
    if (g_debugMod == this) g_debugMod = nullptr;
}

void DebugMenuModule::updateData(float yaw, float pitch, const bedrocktools::sdk::Vec3& pos) {
    m_yaw = yaw;
    m_pitch = pitch;
    m_pos = pos;

    if (m_firstTick) {
        m_lastPos = pos;
        m_firstTick = false;
        m_speed = 0.0f;
    } else {
        float dx = pos.x - m_lastPos.x;
        float dy = pos.y - m_lastPos.y;
        float dz = pos.z - m_lastPos.z;

        float distSq = (dx * dx) + (dy * dy) + (dz * dz);
        if (distSq <= 100.0f) {
            float instSpeed = std::sqrt(distSq) * 20.0f;
            m_speed = (m_speed * 0.8f) + (instSpeed * 0.2f);
        }
        m_velocity.x = dx * 20.f;
        m_velocity.y = dy * 20.f;
        m_velocity.z = dz * 20.f;
        
        m_lastPos = pos;
    }

    if (m_level && s_getRuntimeActorList) {
        void* actorManager = *(void**)((uintptr_t)m_level + bedrocktools::sdk::offsets::Level::mActorManager);
        if (actorManager) {
            std::vector<void*> list = s_getRuntimeActorList(actorManager);
            m_entityCount = (int)list.size();
        }
    }

    if (m_level && m_timeChanger) {
        m_worldTime = m_timeChanger->getRealTime(m_level);
        m_worldTimeValid = true;
    } else {
        m_worldTimeValid = false;
    }
}

void DebugMenuModule::onInit() {
    m_timeChanger = static_cast<TimeChangerModule*>(ModuleRegistry::get().find("bedrocktools.Time Changer"));
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_debugTickCallback(event.player); });

    if (!s_getRuntimeActorList) {
        uintptr_t addrActorList = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList);
        if (addrActorList) {
            s_getRuntimeActorList = (GetRuntimeActorList_t)addrActorList;
        }
    }

    if (!this->m_cursorHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::HudCursor);
        if (addr != 0) {
            this->m_cursorPatchTarget = (void*)addr;
            bedrocktools::hooks::install(
                    this->m_cursorPatchTarget,
                    (void*)s_cursorRenderHook,
                    (void**)&s_origCursorRender
            );
            this->m_cursorHooked = true;
        }
    }

    if (!s_getBiome) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBiome);
        if (addr != 0) {
            s_getBiome = (BlockSource_getBiome_t)addr;
        }
    }

    s_installLevelHooks(this);

}

void DebugMenuModule::onEnable() {
    m_firstTick = true;
    m_frameTimeMs = 0.0f;
    m_fps = -1;
    m_frameCount = 0;
    m_hasFrameTime = false;
}

void DebugMenuModule::onDisable() {
    m_firstTick = true;
    m_frameTimeMs = 0.0f;
    m_fps = -1;
    m_frameCount = 0;
    m_hasFrameTime = false;
}

void DebugMenuModule::updateFrameTiming(std::chrono::steady_clock::time_point now) {
    if (!m_hasFrameTime || now - m_lastFrameTime >= std::chrono::seconds(2)) {
        m_sampleStart = now;
        m_frameCount = 0;
        m_fps = -1;
        m_frameTimeMs = 0.0f;
    } else {
        ++m_frameCount;
        const double elapsed = std::chrono::duration<double>(now - m_sampleStart).count();
        if (elapsed >= 1.0) {
            m_fps = static_cast<int>(std::lround(m_frameCount / elapsed));
            m_frameTimeMs = static_cast<float>(elapsed * 1000.0 / m_frameCount);
            m_sampleStart = now;
            m_frameCount = 0;
        }
    }
    m_lastFrameTime = now;
    m_hasFrameTime = true;
}

void DebugMenuModule::onFrame() {
    if (!enabled) return;
    updateFrameTiming(std::chrono::steady_clock::now());

    std::vector<PLModMenu_DrawCommand> cmds;
    std::list<std::string> stringStore;

    if (!m_cacheInit) {
        const auto fontPath = bedrocktools::core::Runtime::get().resourceDirectory() / "minecraft.ttf";
        std::ifstream fontFile(fontPath, std::ios::binary);
        if (fontFile) {
            std::vector<unsigned char> font((std::istreambuf_iterator<char>(fontFile)), std::istreambuf_iterator<char>());
            if (!font.empty()) pl::modmenu::registerFont("minecraft", font);
        }
        
        char model[92] = {0};
        __system_property_get("ro.product.model", model);
        char brand[92] = {0};
        __system_property_get("ro.product.brand", brand);
        m_cachedDeviceName = std::string(brand) + " " + std::string(model);
        if (m_cachedDeviceName == " ") m_cachedDeviceName = "Unknown Android Device";

        char abi[92] = {0};
        __system_property_get("ro.product.cpu.abi", abi);
        m_cachedAbi = abi[0] ? abi : "arm64-v8a";

        char hardware[92] = {0};
        __system_property_get("ro.hardware", hardware);
        char board[92] = {0};
        __system_property_get("ro.board.platform", board);
        m_cachedCpuName = std::string(hardware);
        if (m_cachedCpuName.empty() || m_cachedCpuName == "qcom") {
            m_cachedCpuName = board;
        }
        if (m_cachedCpuName.empty()) m_cachedCpuName = "Unknown CPU";

        m_cacheInit = true;
    }

    static int frames = 0;
    frames++;
    if (frames > 60) {
        struct sysinfo info;
        if (sysinfo(&info) == 0) {
            m_totalMemMb = (info.totalram * info.mem_unit) / (1024 * 1024);
            unsigned long freeMem = (info.freeram * info.mem_unit) / (1024 * 1024);
            m_usedMemMb = m_totalMemMb > freeMem ? m_totalMemMb - freeMem : 0;
        }
        frames = 0;
    }

    if (m_showCrosshair) {
        float cx = -20000.f; 
        float cy = -20000.f; 

        float simDt = 1.f / 60.f;
        float t  = 1.f - std::exp(-m_lerpSpeed * simDt);

        float dyaw = m_yaw - m_lerpYaw;
        if (dyaw >  180.f) dyaw -= 360.f;
        if (dyaw < -180.f) dyaw += 360.f;
        m_lerpYaw   += dyaw               * t;
        m_lerpPitch += (m_pitch - m_lerpPitch) * t;

        const float PI       = 3.14159265f;
        const float yawRad   = (180.f + m_lerpYaw)  * (PI / 180.f);
        const float pitchRad = (-m_lerpPitch)        * (PI / 180.f);
        const float L        = m_lineLength;

        float redX = L * std::cos(yawRad);
        float redY = L * std::sin(yawRad) * std::sin(pitchRad);

        float greenX = 0.f;
        float greenY = -L * std::cos(pitchRad);

        float blueX = L * std::sin(yawRad);
        float blueY = -L * std::cos(yawRad) * std::sin(pitchRad);

        const uint32_t RED   = 0xFFDC3C3C;
        const uint32_t GREEN = 0xFF3CD23C;
        const uint32_t BLUE  = 0xFF3C64DC;
        const uint32_t BLACK = 0xC8000000;

        auto drawVector = [&](float dx, float dy, uint32_t col) {
            PLModMenu_DrawCommand lineOut = {};
            lineOut.type = PL_DRAW_LINE;
            lineOut.x = cx;
            lineOut.y = cy;
            lineOut.w = dx;
            lineOut.h = dy;
            lineOut.size = m_outlineThick;
            lineOut.color = BLACK;
            cmds.push_back(lineOut);
            
            PLModMenu_DrawCommand lineIn = {};
            lineIn.type = PL_DRAW_LINE;
            lineIn.x = cx;
            lineIn.y = cy;
            lineIn.w = dx;
            lineIn.h = dy;
            lineIn.size = m_lineThick;
            lineIn.color = col;
            cmds.push_back(lineIn);

            if (m_showTipDot) {
                PLModMenu_DrawCommand dotOut = {};
                dotOut.type = PL_DRAW_CIRCLE_FILLED;
                dotOut.x = cx + dx;
                dotOut.y = cy + dy;
                dotOut.size = m_lineThick * 1.4f;
                dotOut.color = BLACK;
                cmds.push_back(dotOut);
                
                PLModMenu_DrawCommand dotIn = {};
                dotIn.type = PL_DRAW_CIRCLE_FILLED;
                dotIn.x = cx + dx;
                dotIn.y = cy + dy;
                dotIn.size = m_lineThick * 0.9f;
                dotIn.color = col;
                cmds.push_back(dotIn);
            }
        };

        float absYaw = std::fabs(m_lerpYaw);
        struct Entry { float depth; float dx; float dy; uint32_t col; };
        Entry entries[3] = {
                { (m_lerpYaw < 0.f) ? 0.f : 2.f,  redX, redY, RED   },
                { 1.f,                              greenX, greenY, GREEN },
                { (absYaw    < 90.f)? 2.f : 0.f,  blueX, blueY, BLUE  },
        };
        for (int i = 1; i < 3; ++i) {
            Entry key = entries[i];
            int j = i - 1;
            while (j >= 0 && entries[j].depth > key.depth) {
                entries[j + 1] = entries[j]; --j;
            }
            entries[j + 1] = key;
        }

        for (auto& e : entries) drawVector(e.dx, e.dy, e.col);
    } 

    if (m_showOverlay) {
        float fontSz = 22.f * m_textScale; 
        
        struct TextLine {
            std::string text;
            uint32_t color;
        };

        std::vector<TextLine> leftLines;
        std::vector<TextLine> rightLines;

        auto addLeft = [&](const std::string& t, uint32_t c = 0) {
            leftLines.push_back({t, c == 0 ? 0xFFE6E6E6 : c});
        };
        
        auto addRight = [&](const std::string& t, uint32_t c = 0) {
            rightLines.push_back({t, c == 0 ? 0xFFE6E6E6 : c});
        };

        addLeft(std::string(bedrocktools::Name) + " v" + std::string(bedrocktools::Version), 0xFF55D2FF);
        addLeft("");
        
        char buf[128];

        if (m_entityCount >= 0) {
            snprintf(buf, sizeof(buf), "E: %d", m_entityCount);
            addLeft(buf);
            addLeft("");
        }

        snprintf(buf, sizeof(buf), "XYZ: %.1f / %.1f / %.1f", m_pos.x, m_pos.y, m_pos.z);
        addLeft(buf);

        int bX = (int)std::floor(m_pos.x);
        int bY = (int)std::floor(m_pos.y);
        int bZ = (int)std::floor(m_pos.z);
        snprintf(buf, sizeof(buf), "Block: %d %d %d", bX, bY, bZ);
        addLeft(buf);

        int cX = static_cast<int>(std::floor(m_pos.x / 16.0));
        int cZ = static_cast<int>(std::floor(m_pos.z / 16.0));
        int relX = bX % 16; if(relX < 0) relX += 16;
        int relY = bY % 16; if(relY < 0) relY += 16;
        int relZ = bZ % 16; if(relZ < 0) relZ += 16;
        snprintf(buf, sizeof(buf), "Chunk Relative: %d %d %d", relX, relY, relZ);
        addLeft(buf);
        
        snprintf(buf, sizeof(buf), "Chunk: %d %d", cX, cZ);
        addLeft(buf);

        float bYaw = fmodf(m_yaw + 180.f, 360.f);
        if (bYaw < 0.f) bYaw += 360.f;
        const char* facingName = "North";
        if (bYaw >= 45.f && bYaw < 135.f) facingName = "East";
        else if (bYaw >= 135.f && bYaw < 225.f) facingName = "South";
        else if (bYaw >= 225.f && bYaw < 315.f) facingName = "West";
        snprintf(buf, sizeof(buf), "Facing: %s (%.1f / %.1f)", facingName, m_yaw, m_pitch);
        addLeft(buf);
        
        uint32_t currentSeed = (cX * 0x1f1f1f1f) ^ cZ;
        std::mt19937 currMt(currentSeed);
        bool isSlime = (currMt() % 10 == 0);
        uint32_t slimeCol = isSlime ? 0xFF32FF32 : 0xFFFF5050;
        addLeft(std::string("Slime Chunk: ") + (isSlime ? "YES" : "NO"), slimeCol);
        addLeft("");

        snprintf(buf, sizeof(buf), "Speed: %.2f blocks/s", m_speed);
        addLeft(buf);
        snprintf(buf, sizeof(buf), "Velocity: %.2f / %.2f / %.2f blocks/s", m_velocity.x, m_velocity.y, m_velocity.z);
        addLeft(buf);
        addLeft("");

        snprintf(buf, sizeof(buf), "World Name: %s", m_worldName.c_str());
        addLeft(buf);
        snprintf(buf, sizeof(buf), "Biome: %s", m_biomeName.c_str());
        addLeft(buf);
        if (m_worldTimeValid) {
            int dayTicks = m_worldTime % 24000;
            if (dayTicks < 0) dayTicks += 24000;
            int totalMinutes = ((dayTicks + 6000) % 24000) * 1440 / 24000;
            int worldHour = totalMinutes / 60;
            int worldMinute = totalMinutes % 60;
            snprintf(buf, sizeof(buf), "World Time: %02d:%02d (%d ticks)", worldHour, worldMinute, dayTicks);
            addLeft(buf);
        }

        
        addRight(m_cachedDeviceName);
        addRight("CPU: " + m_cachedCpuName);
        addRight("ABI: " + m_cachedAbi);
        snprintf(buf, sizeof(buf), "Memory: %lu MB / %lu MB", m_usedMemMb, m_totalMemMb);
        addRight(buf);
        addRight("Display: {DISPLAY_SIZE}");
        addRight("Active Renderer: OpenGL ES");
        if (m_fps >= 0) {
            snprintf(buf, sizeof(buf), "FPS: %d", m_fps);
            addRight(buf);
            snprintf(buf, sizeof(buf), "Frame Time: %.2f ms", m_frameTimeMs);
            addRight(buf);
        } else {
            addRight("FPS: --");
            addRight("Frame Time: -- ms");
        }
        addRight("");

        time_t t = time(nullptr);
        struct tm* tm_info = localtime(&t);
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "Local Time: %I:%M %p", tm_info);
        addRight(timeBuf);
        
        static auto startupTime = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - startupTime).count();
        long upM = uptime / 60;
        long upS = uptime % 60;
        long upH = upM / 60;
        upM = upM % 60;
        snprintf(buf, sizeof(buf), "Minecraft Uptime: %02ld:%02ld:%02ld", upH, upM, upS);
        addRight(buf);

        float leftY = 25.f;
        float leftBaseX = 5.f;
        
        for (const auto& line : leftLines) {
            if (line.text.empty()) {
                leftY += fontSz / 2.f;
                continue;
            }
            float lineH = fontSz + 4.f;
            
            stringStore.push_back(line.text);
            PLModMenu_DrawCommand txtCmd = {};
            txtCmd.type = PL_DRAW_TEXT;
            txtCmd.x = leftBaseX;
            txtCmd.y = leftY + lineH - 6.f;
            txtCmd.w = 0.f; 
            txtCmd.h = 0.f;
            txtCmd.color = line.color;
            txtCmd.size = fontSz;
            txtCmd.fontId = "minecraft";
            txtCmd.text = stringStore.back().c_str(); 
            cmds.push_back(txtCmd);
            
            leftY += lineH;
        }

        float rightY = 25.f;
        for (const auto& line : rightLines) {
            if (line.text.empty()) {
                rightY += fontSz / 2.f;
                continue;
            }
            float lineH = fontSz + 4.f;
            
            stringStore.push_back(line.text);
            PLModMenu_DrawCommand txtCmd = {};
            txtCmd.type = PL_DRAW_TEXT;
            txtCmd.x = -10005.f; 
            txtCmd.y = rightY + lineH - 6.f;
            txtCmd.w = -1.f; 
            txtCmd.h = 0.f;
            txtCmd.color = line.color;
            txtCmd.size = fontSz;
            txtCmd.fontId = "minecraft";
            txtCmd.text = stringStore.back().c_str(); 
            cmds.push_back(txtCmd);
            
            rightY += lineH;
        }
    }

    submitDrawCommands(moduleId, cmds);
}

void DebugMenuModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("showCrosshair")) m_showCrosshair = j["showCrosshair"].get<bool>();
    if (j.contains("lineLength"))   m_lineLength   = j["lineLength"].get<float>();
    if (j.contains("lineThick"))    m_lineThick     = j["lineThick"].get<float>();
    if (j.contains("outlineThick")) m_outlineThick  = j["outlineThick"].get<float>();
    if (j.contains("lerpSpeed"))    m_lerpSpeed     = j["lerpSpeed"].get<float>();
    if (j.contains("showTipDot"))   m_showTipDot    = j["showTipDot"].get<bool>();

    if (j.contains("showOverlay"))   m_showOverlay  = j["showOverlay"].get<bool>();
    if (j.contains("textScale"))     m_textScale    = j["textScale"].get<float>();
}

void DebugMenuModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showCrosshair"] = m_showCrosshair;
    j["lineLength"]   = m_lineLength;
    j["lineThick"]    = m_lineThick;
    j["outlineThick"] = m_outlineThick;
    j["lerpSpeed"]    = m_lerpSpeed;
    j["showTipDot"]   = m_showTipDot;

    j["showOverlay"]  = m_showOverlay;
    j["textScale"]    = m_textScale;
}
