#include "waypoints.hpp"
#include "WaypointManager.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>
#include <utility>
#include <mutex>
#include <algorithm>
#include <cstdio>

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

struct HashedString {
    uint64_t mStrHash;
    std::string mStr;
    mutable const HashedString* mLastMatch;

    HashedString() : mStrHash(0), mStr(), mLastMatch(nullptr) {}

    explicit HashedString(const char* str) : mLastMatch(nullptr) {
        mStr = str ? str : "";
        mStrHash = computeHash(mStr);
    }

private:
    static uint64_t computeHash(const std::string& str) {
        if (str.empty()) return 0;
        constexpr uint64_t kOffset = 0xCBF29CE484222325ULL;
        constexpr uint64_t kPrime = 0x100000001B3ULL;
        uint64_t hash = kOffset;
        for (char ch : str)
            hash = static_cast<uint64_t>(static_cast<unsigned char>(ch)) ^ (kPrime * hash);
        return hash;
    }
};

struct MaterialPtr {
    void* sharedPtrData[2]{nullptr, nullptr};

    MaterialPtr() = default;
    MaterialPtr(const MaterialPtr&) = delete;
    MaterialPtr& operator=(const MaterialPtr&) = delete;

    MaterialPtr(MaterialPtr&& other) noexcept
        : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
        other.sharedPtrData[0] = nullptr;
        other.sharedPtrData[1] = nullptr;
    }

    MaterialPtr& operator=(MaterialPtr&& other) noexcept {
        if (this != &other) {
            sharedPtrData[0] = other.sharedPtrData[0];
            sharedPtrData[1] = other.sharedPtrData[1];
            other.sharedPtrData[0] = nullptr;
            other.sharedPtrData[1] = nullptr;
        }
        return *this;
    }

    ~MaterialPtr() {}

    explicit operator bool() const {
        return sharedPtrData[0] != nullptr;
    }
};

static uintptr_t resolveADRP(uint32_t* insns, size_t count, uint32_t targetReg) {
    for (size_t i = 0; i < count; i++) {
        uint32_t insn = insns[i];
        if ((insn & 0x1F) != targetReg) continue;

        if ((insn & 0x9F000000) == 0x90000000) {
            uintptr_t page = ((uintptr_t)&insns[i] & ~0xFFFULL)
                           + ((int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29) & 3) << 43) >> 31);

            for (size_t j = i + 1; j < count; j++) {
                uint32_t add = insns[j];
                if ((add & 0xFF000000) == 0x91000000 &&
                    ((add >> 5) & 0x1F) == targetReg &&
                    (add & 0x1F) == targetReg) {
                    uint32_t imm12 = (add >> 10) & 0xFFF;
                    if (add & 0x400000) imm12 <<= 12;
                    return page + imm12;
                }
                if ((add & 0x1F) == targetReg) break;
            }
        }
        if ((insn & 0x9F000000) == 0x10000000) {
            int64_t imm = (int64_t)((uint64_t)((insn >> 3) & 0x1FFFFC | (insn >> 29)) << 43) >> 43;
            return (uintptr_t)&insns[i] + imm;
        }
    }
    return 0;
}

static WaypointsModule* g_waypointsMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr s_matSelection;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3) = nullptr;

static bedrocktools::sdk::Player* g_localPlayer = nullptr;
static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};
static float g_playerYaw = 0.f;
static int g_playerDimensionId = 0;

static void s_waypointsTickCallback(bedrocktools::sdk::Player* player) {
    if (!g_waypointsMod || !g_waypointsMod->enabled || !player) return;
    g_localPlayer = player;
    g_playerPos = player->position();
    g_playerYaw = player->rotation().y;

    if (auto* dim = player->dimension()) {
        if (auto* bs = dim->blockSource()) {
            g_playerDimensionId = bs->dimensionId();
        }
    }
}

static std::string floatToHex(float r, float g, float b) {
    int ri = std::clamp((int)(r * 255.f), 0, 255);
    int gi = std::clamp((int)(g * 255.f), 0, 255);
    int bi = std::clamp((int)(b * 255.f), 0, 255);
    char buf[12];
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", ri, gi, bi);
    return std::string(buf);
}

static void hexToFloat(const std::string& hex, float& r, float& g, float& b) {
    if (hex.empty() || hex[0] != '#') return;
    try {
        unsigned long val = std::stoul(hex.substr(1), nullptr, 16);
        r = ((val >> 16) & 0xFF) / 255.0f;
        g = ((val >> 8) & 0xFF) / 255.0f;
        b = (val & 0xFF) / 255.0f;
    } catch (...) {}
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[2]) return {};

    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[2])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterials() {
    if (s_matSelection) return;
    if (!s_renderMaterialGroup) return;

    if (!s_matSelection) s_matSelection = getMaterial("selection_box");
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    if (!g_waypointsMod || !g_waypointsMod->enabled || !g_waypointsMod->m_showWorldMarker) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || (uintptr_t)screenContext < 0x1000) return;

    auto& wps = WaypointManager::get().getWaypoints();
    if (wps.empty()) return;

    uintptr_t tessellatorPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) return;
    void* tessellator = (void*)tessellatorPtr;

    uintptr_t lrpPtr = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    float camX = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    ensureMaterials();

    void* matInner = s_matSelection ? (void*)&s_matSelection
                                    : (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    for (const auto& wp : wps) {
        if (!wp.enabled) continue;
        if (wp.dimensionId != g_playerDimensionId) continue;

        float dx = wp.x - g_playerPos.x;
        float dy = wp.y - g_playerPos.y;
        float dz = wp.z - g_playerPos.z;
        float distSq = dx*dx + dy*dy + dz*dz;
        float maxDist = g_waypointsMod->m_maxRenderDistance;
        if (distSq > maxDist * maxDist) continue;

        float wx = wp.x - camX;
        float wz = wp.z - camZ;
        float markerSize = g_waypointsMod->m_markerSize;

        // Render Vertical Beam
        s_tessBegin(tessellator, nullptr, 4, 2, 0);
        s_tessColor(tessellator, wp.r, wp.g, wp.b, 0.7f);
        s_tessVertex(tessellator, wx, -64.0f - camY, wz);
        s_tessVertex(tessellator, wx, 320.0f - camY, wz);

        char pad[0x58];
        memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matInner, pad);

        // Render Ground Square Marker
        float hw = markerSize * 0.5f;
        s_tessBegin(tessellator, nullptr, 4, 8, 0);
        s_tessColor(tessellator, wp.r, wp.g, wp.b, 0.9f);

        float sy = wp.y + 0.05f - camY;
        float x1 = wx - hw, x2 = wx + hw;
        float z1 = wz - hw, z2 = wz + hw;

        s_tessVertex(tessellator, x1, sy, z1);
        s_tessVertex(tessellator, x2, sy, z1);

        s_tessVertex(tessellator, x2, sy, z1);
        s_tessVertex(tessellator, x2, sy, z2);

        s_tessVertex(tessellator, x2, sy, z2);
        s_tessVertex(tessellator, x1, sy, z2);

        s_tessVertex(tessellator, x1, sy, z2);
        s_tessVertex(tessellator, x1, sy, z1);

        memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matInner, pad);
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

WaypointsModule::WaypointsModule()
    : Module("Waypoints", "Mark important world coordinates and navigate back using HUD pointers.") {
    showInMenu = true;
    g_waypointsMod = this;
}

WaypointsModule::~WaypointsModule() {
    if (g_waypointsMod == this) g_waypointsMod = nullptr;
}

void WaypointsModule::onInit() {
    uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderLevel);
    if (addr != 0) {
        m_patchTarget = (void*)addr;
    }

    uintptr_t tb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorBegin);
    if (tb) { m_tessBeginAddr = (void*)tb; s_tessBegin = (Tessellator_begin_t)tb; }

    uintptr_t tc = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorColor);
    if (tc) { m_tessColorAddr = (void*)tc; s_tessColor = (Tessellator_color_t)tc; }

    uintptr_t tv = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::TessellatorVertex);
    if (tv) { m_tessVertexAddr = (void*)tv; s_tessVertex = (Tessellator_vertex_t)tv; }

    uintptr_t rm = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
    if (rm) {
        s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm;
    } else {
        uintptr_t rm5 = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately);
        if (rm5) s_renderMesh = (MeshHelpers_renderMeshImmediately_t)rm5;
    }

    uintptr_t rmg = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
    if (rmg) {
        m_renderMaterialGroupAddr = (void*)rmg;
        uintptr_t groupAddr = resolveADRP(reinterpret_cast<uint32_t*>(rmg), 2, 0);
        if (groupAddr) {
            s_renderMaterialGroup = groupAddr + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
        }
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_waypointsTickCallback(event.player); });
    syncManagerToSlots();
}

void WaypointsModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = true;
}

void WaypointsModule::onEnable() {
    applyPatch();
}

void WaypointsModule::onDisable() {}

void WaypointsModule::onFrame() {
    if (!enabled || !m_showHUD) return;

    auto& wps = WaypointManager::get().getWaypoints();
    if (wps.empty()) return;

    int closestIndex = -1;
    float closestDist = 999999.0f;

    for (int i = 0; i < (int)wps.size(); ++i) {
        const auto& wp = wps[i];
        if (!wp.enabled) continue;
        if (wp.dimensionId != g_playerDimensionId) continue;

        float dx = wp.x - g_playerPos.x;
        float dy = wp.y - g_playerPos.y;
        float dz = wp.z - g_playerPos.z;
        float dist = std::sqrt(dx*dx + dy*dy + dz*dz);

        if (dist < closestDist) {
            closestDist = dist;
            closestIndex = i;
        }
    }

    if (closestIndex == -1) return;

    const auto& wp = wps[closestIndex];

    float toWpX = wp.x - g_playerPos.x;
    float toWpZ = wp.z - g_playerPos.z;

    float yawRad = g_playerYaw * (M_PI / 180.0f);
    float lookX = -std::sin(yawRad);
    float lookZ = std::cos(yawRad);

    float rightX = std::cos(yawRad);
    float rightZ = std::sin(yawRad);

    float dotForward = toWpX * lookX + toWpZ * lookZ;
    float dotRight = toWpX * rightX + toWpZ * rightZ;

    float relativeAngle = std::atan2(dotRight, dotForward) * (180.0f / M_PI);

    std::string arrow = "↑";
    if (relativeAngle >= 45.0f && relativeAngle <= 135.0f) {
        arrow = "→";
    } else if (relativeAngle <= -45.0f && relativeAngle >= -135.0f) {
        arrow = "←";
    } else if (std::abs(relativeAngle) > 135.0f) {
        arrow = "↓";
    }

    float angleToWp = std::atan2(toWpX, toWpZ) * (180.f / M_PI);
    if (angleToWp < 0) angleToWp += 360.f;

    std::string cardDir = "S";
    if (angleToWp >= 337.5f || angleToWp < 22.5f) cardDir = "S";
    else if (angleToWp >= 22.5f && angleToWp < 67.5f) cardDir = "SE";
    else if (angleToWp >= 67.5f && angleToWp < 112.5f) cardDir = "E";
    else if (angleToWp >= 112.5f && angleToWp < 157.5f) cardDir = "NE";
    else if (angleToWp >= 157.5f && angleToWp < 202.5f) cardDir = "N";
    else if (angleToWp >= 202.5f && angleToWp < 247.5f) cardDir = "NW";
    else if (angleToWp >= 247.5f && angleToWp < 292.5f) cardDir = "W";
    else if (angleToWp >= 292.5f && angleToWp < 337.5f) cardDir = "SW";

    int distM = (int)std::round(closestDist);

    std::vector<PLModMenu_DrawCommand> cmds;
    std::string line1 = m_showDirection ? (arrow + " " + cardDir) : arrow;
    std::string line2 = m_showName ? wp.name : "Waypoint";
    std::string line3 = m_showDistance ? (std::to_string(distM) + "m") : "";

    float boxW = 120.0f;
    float boxH = m_textSize * 4.5f;
    float boxX = hudPosX;
    float boxY = hudPosY;

    PLModMenu_DrawCommand bgCmd = {};
    bgCmd.type = PL_DRAW_RECT_FILLED;
    bgCmd.x = boxX;
    bgCmd.y = boxY;
    bgCmd.w = boxW;
    bgCmd.h = boxH;
    bgCmd.color = 0x88000000;
    cmds.push_back(bgCmd);

    uint32_t wpCol = (0xFF << 24) | ((uint32_t)(wp.r * 255) << 16) | ((uint32_t)(wp.g * 255) << 8) | (uint32_t)(wp.b * 255);

    auto renderCenteredText = [&](const std::string& str, float offsetMultiplier, uint32_t col) {
        if (str.empty()) return;
        PLModMenu_DrawCommand txtCmd = {};
        txtCmd.type = PL_DRAW_TEXT;
        txtCmd.x = boxX;
        txtCmd.y = boxY + m_textSize * offsetMultiplier;
        txtCmd.w = boxW;
        txtCmd.h = m_textSize * 1.5f;
        txtCmd.color = col;
        txtCmd.size = m_textSize;
        txtCmd.text = str.c_str();
        cmds.push_back(txtCmd);
    };

    renderCenteredText(line1, 0.4f, wpCol);
    renderCenteredText(line2, 1.5f, 0xFFFFFFFF);
    renderCenteredText(line3, 2.6f, 0xFFCCCCCC);

    ::submitDrawCommands(moduleId, cmds);
}

void WaypointsModule::syncSlotsToManager() {
    std::vector<Waypoint> wps;
    auto addSlot = [&](bool enabledVal, const std::string& nameVal, float xVal, float yVal, float zVal, const std::string& colorVal) {
        if (!enabledVal) return;
        Waypoint wp;
        wp.name = nameVal;
        wp.x = xVal;
        wp.y = yVal;
        wp.z = zVal;
        wp.enabled = true;
        wp.dimensionId = g_playerDimensionId;
        hexToFloat(colorVal, wp.r, wp.g, wp.b);
        wps.push_back(wp);
    };

    addSlot(m_wp1_enabled, m_wp1_name, m_wp1_x, m_wp1_y, m_wp1_z, m_wp1_color);
    addSlot(m_wp2_enabled, m_wp2_name, m_wp2_x, m_wp2_y, m_wp2_z, m_wp2_color);
    addSlot(m_wp3_enabled, m_wp3_name, m_wp3_x, m_wp3_y, m_wp3_z, m_wp3_color);
    addSlot(m_wp4_enabled, m_wp4_name, m_wp4_x, m_wp4_y, m_wp4_z, m_wp4_color);
    addSlot(m_wp5_enabled, m_wp5_name, m_wp5_x, m_wp5_y, m_wp5_z, m_wp5_color);

    WaypointManager::get().setWaypoints(wps);
}

void WaypointsModule::syncManagerToSlots() {
    auto& wps = WaypointManager::get().getWaypoints();

    m_wp1_enabled = false;
    m_wp2_enabled = false;
    m_wp3_enabled = false;
    m_wp4_enabled = false;
    m_wp5_enabled = false;

    if (wps.size() >= 1) {
        m_wp1_enabled = wps[0].enabled; m_wp1_name = wps[0].name;
        m_wp1_x = wps[0].x; m_wp1_y = wps[0].y; m_wp1_z = wps[0].z;
        m_wp1_color = floatToHex(wps[0].r, wps[0].g, wps[0].b);
    }
    if (wps.size() >= 2) {
        m_wp2_enabled = wps[1].enabled; m_wp2_name = wps[1].name;
        m_wp2_x = wps[1].x; m_wp2_y = wps[1].y; m_wp2_z = wps[1].z;
        m_wp2_color = floatToHex(wps[1].r, wps[1].g, wps[1].b);
    }
    if (wps.size() >= 3) {
        m_wp3_enabled = wps[2].enabled; m_wp3_name = wps[2].name;
        m_wp3_x = wps[2].x; m_wp3_y = wps[2].y; m_wp3_z = wps[2].z;
        m_wp3_color = floatToHex(wps[2].r, wps[2].g, wps[2].b);
    }
    if (wps.size() >= 4) {
        m_wp4_enabled = wps[3].enabled; m_wp4_name = wps[3].name;
        m_wp4_x = wps[3].x; m_wp4_y = wps[3].y; m_wp4_z = wps[3].z;
        m_wp4_color = floatToHex(wps[3].r, wps[3].g, wps[3].b);
    }
    if (wps.size() >= 5) {
        m_wp5_enabled = wps[4].enabled; m_wp5_name = wps[4].name;
        m_wp5_x = wps[4].x; m_wp5_y = wps[4].y; m_wp5_z = wps[4].z;
        m_wp5_color = floatToHex(wps[4].r, wps[4].g, wps[4].b);
    }
}

void WaypointsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();

    if (j.contains("m_showWorldMarker")) m_showWorldMarker = j["m_showWorldMarker"].get<bool>();
    if (j.contains("m_showName")) m_showName = j["m_showName"].get<bool>();
    if (j.contains("m_showDistance")) m_showDistance = j["m_showDistance"].get<bool>();
    if (j.contains("m_showDirection")) m_showDirection = j["m_showDirection"].get<bool>();
    if (j.contains("m_showHUD")) m_showHUD = j["m_showHUD"].get<bool>();
    if (j.contains("m_showOnlySelected")) m_showOnlySelected = j["m_showOnlySelected"].get<bool>();
    if (j.contains("m_maxRenderDistance")) m_maxRenderDistance = j["m_maxRenderDistance"].get<float>();
    if (j.contains("m_textSize")) m_textSize = j["m_textSize"].get<float>();
    if (j.contains("m_markerSize")) m_markerSize = j["m_markerSize"].get<float>();

    // Load Slots
    if (j.contains("m_wp1_enabled")) m_wp1_enabled = j["m_wp1_enabled"].get<bool>();
    if (j.contains("m_wp1_name")) m_wp1_name = j["m_wp1_name"].get<std::string>();
    if (j.contains("m_wp1_x")) m_wp1_x = j["m_wp1_x"].get<float>();
    if (j.contains("m_wp1_y")) m_wp1_y = j["m_wp1_y"].get<float>();
    if (j.contains("m_wp1_z")) m_wp1_z = j["m_wp1_z"].get<float>();
    if (j.contains("m_wp1_color")) m_wp1_color = j["m_wp1_color"].get<std::string>();

    if (j.contains("m_wp2_enabled")) m_wp2_enabled = j["m_wp2_enabled"].get<bool>();
    if (j.contains("m_wp2_name")) m_wp2_name = j["m_wp2_name"].get<std::string>();
    if (j.contains("m_wp2_x")) m_wp2_x = j["m_wp2_x"].get<float>();
    if (j.contains("m_wp2_y")) m_wp2_y = j["m_wp2_y"].get<float>();
    if (j.contains("m_wp2_z")) m_wp2_z = j["m_wp2_z"].get<float>();
    if (j.contains("m_wp2_color")) m_wp2_color = j["m_wp2_color"].get<std::string>();

    if (j.contains("m_wp3_enabled")) m_wp3_enabled = j["m_wp3_enabled"].get<bool>();
    if (j.contains("m_wp3_name")) m_wp3_name = j["m_wp3_name"].get<std::string>();
    if (j.contains("m_wp3_x")) m_wp3_x = j["m_wp3_x"].get<float>();
    if (j.contains("m_wp3_y")) m_wp3_y = j["m_wp3_y"].get<float>();
    if (j.contains("m_wp3_z")) m_wp3_z = j["m_wp3_z"].get<float>();
    if (j.contains("m_wp3_color")) m_wp3_color = j["m_wp3_color"].get<std::string>();

    if (j.contains("m_wp4_enabled")) m_wp4_enabled = j["m_wp4_enabled"].get<bool>();
    if (j.contains("m_wp4_name")) m_wp4_name = j["m_wp4_name"].get<std::string>();
    if (j.contains("m_wp4_x")) m_wp4_x = j["m_wp4_x"].get<float>();
    if (j.contains("m_wp4_y")) m_wp4_y = j["m_wp4_y"].get<float>();
    if (j.contains("m_wp4_z")) m_wp4_z = j["m_wp4_z"].get<float>();
    if (j.contains("m_wp4_color")) m_wp4_color = j["m_wp4_color"].get<std::string>();

    if (j.contains("m_wp5_enabled")) m_wp5_enabled = j["m_wp5_enabled"].get<bool>();
    if (j.contains("m_wp5_name")) m_wp5_name = j["m_wp5_name"].get<std::string>();
    if (j.contains("m_wp5_x")) m_wp5_x = j["m_wp5_x"].get<float>();
    if (j.contains("m_wp5_y")) m_wp5_y = j["m_wp5_y"].get<float>();
    if (j.contains("m_wp5_z")) m_wp5_z = j["m_wp5_z"].get<float>();
    if (j.contains("m_wp5_color")) m_wp5_color = j["m_wp5_color"].get<std::string>();

    // Check delete buttons
    if (j.contains("m_wp1_deleteButton") && j["m_wp1_deleteButton"].get<bool>()) {
        m_wp1_enabled = false;
        m_wp1_name = "Home";
        m_wp1_x = 0.0f; m_wp1_y = 64.0f; m_wp1_z = 0.0f;
        m_wp1_color = "#FF0000";
    }
    if (j.contains("m_wp2_deleteButton") && j["m_wp2_deleteButton"].get<bool>()) {
        m_wp2_enabled = false;
        m_wp2_name = "Village";
        m_wp2_x = 0.0f; m_wp2_y = 64.0f; m_wp2_z = 0.0f;
        m_wp2_color = "#00FF00";
    }
    if (j.contains("m_wp3_deleteButton") && j["m_wp3_deleteButton"].get<bool>()) {
        m_wp3_enabled = false;
        m_wp3_name = "Mine";
        m_wp3_x = 0.0f; m_wp3_y = 64.0f; m_wp3_z = 0.0f;
        m_wp3_color = "#0000FF";
    }
    if (j.contains("m_wp4_deleteButton") && j["m_wp4_deleteButton"].get<bool>()) {
        m_wp4_enabled = false;
        m_wp4_name = "Nether Portal";
        m_wp4_x = 0.0f; m_wp4_y = 64.0f; m_wp4_z = 0.0f;
        m_wp4_color = "#FFFF00";
    }
    if (j.contains("m_wp5_deleteButton") && j["m_wp5_deleteButton"].get<bool>()) {
        m_wp5_enabled = false;
        m_wp5_name = "Stronghold";
        m_wp5_x = 0.0f; m_wp5_y = 64.0f; m_wp5_z = 0.0f;
        m_wp5_color = "#FF00FF";
    }

    // Check add waypoint button
    if (j.contains("m_addWaypointButton") && j["m_addWaypointButton"].get<bool>()) {
        if (!m_wp1_enabled) {
            m_wp1_enabled = true; m_wp1_name = "Waypoint 1";
            m_wp1_x = g_playerPos.x; m_wp1_y = g_playerPos.y; m_wp1_z = g_playerPos.z;
        } else if (!m_wp2_enabled) {
            m_wp2_enabled = true; m_wp2_name = "Waypoint 2";
            m_wp2_x = g_playerPos.x; m_wp2_y = g_playerPos.y; m_wp2_z = g_playerPos.z;
        } else if (!m_wp3_enabled) {
            m_wp3_enabled = true; m_wp3_name = "Waypoint 3";
            m_wp3_x = g_playerPos.x; m_wp3_y = g_playerPos.y; m_wp3_z = g_playerPos.z;
        } else if (!m_wp4_enabled) {
            m_wp4_enabled = true; m_wp4_name = "Waypoint 4";
            m_wp4_x = g_playerPos.x; m_wp4_y = g_playerPos.y; m_wp4_z = g_playerPos.z;
        } else if (!m_wp5_enabled) {
            m_wp5_enabled = true; m_wp5_name = "Waypoint 5";
            m_wp5_x = g_playerPos.x; m_wp5_y = g_playerPos.y; m_wp5_z = g_playerPos.z;
        }
    }

    syncSlotsToManager();
}

void WaypointsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;

    j["m_showWorldMarker"] = m_showWorldMarker;
    j["m_showName"] = m_showName;
    j["m_showDistance"] = m_showDistance;
    j["m_showDirection"] = m_showDirection;
    j["m_showHUD"] = m_showHUD;
    j["m_showOnlySelected"] = m_showOnlySelected;
    j["m_maxRenderDistance"] = m_maxRenderDistance;
    j["m_textSize"] = m_textSize;
    j["m_markerSize"] = m_markerSize;

    j["m_addWaypointButton"] = false;

    j["m_wp1_enabled"] = m_wp1_enabled;
    j["m_wp1_name"] = m_wp1_name;
    j["m_wp1_x"] = m_wp1_x;
    j["m_wp1_y"] = m_wp1_y;
    j["m_wp1_z"] = m_wp1_z;
    j["m_wp1_color"] = m_wp1_color;
    j["m_wp1_deleteButton"] = false;

    j["m_wp2_enabled"] = m_wp2_enabled;
    j["m_wp2_name"] = m_wp2_name;
    j["m_wp2_x"] = m_wp2_x;
    j["m_wp2_y"] = m_wp2_y;
    j["m_wp2_z"] = m_wp2_z;
    j["m_wp2_color"] = m_wp2_color;
    j["m_wp2_deleteButton"] = false;

    j["m_wp3_enabled"] = m_wp3_enabled;
    j["m_wp3_name"] = m_wp3_name;
    j["m_wp3_x"] = m_wp3_x;
    j["m_wp3_y"] = m_wp3_y;
    j["m_wp3_z"] = m_wp3_z;
    j["m_wp3_color"] = m_wp3_color;
    j["m_wp3_deleteButton"] = false;

    j["m_wp4_enabled"] = m_wp4_enabled;
    j["m_wp4_name"] = m_wp4_name;
    j["m_wp4_x"] = m_wp4_x;
    j["m_wp4_y"] = m_wp4_y;
    j["m_wp4_z"] = m_wp4_z;
    j["m_wp4_color"] = m_wp4_color;
    j["m_wp4_deleteButton"] = false;

    j["m_wp5_enabled"] = m_wp5_enabled;
    j["m_wp5_name"] = m_wp5_name;
    j["m_wp5_x"] = m_wp5_x;
    j["m_wp5_y"] = m_wp5_y;
    j["m_wp5_z"] = m_wp5_z;
    j["m_wp5_color"] = m_wp5_color;
    j["m_wp5_deleteButton"] = false;
}
