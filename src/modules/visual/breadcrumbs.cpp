#include "breadcrumbs.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>
#include <utility>
#include <mutex>

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

static BreadcrumbsModule* g_breadcrumbsMod = nullptr;
static std::mutex s_pointsMutex;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr s_matSelection;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

struct AABB {
    bedrocktools::sdk::Vec3 min;
    bedrocktools::sdk::Vec3 max;
};

static AABB getActorAABB(void* actor) {
    AABB aabb = {{0,0,0},{0,0,0}};
    uintptr_t actorAddr = (uintptr_t)actor;

    uintptr_t builtInPtr = *(uintptr_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (builtInPtr) {
        uintptr_t aabbComponentPtr = *(uintptr_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mStateVectorComponent + bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent);
        if (aabbComponentPtr) {
            aabb = *(AABB*)(aabbComponentPtr + bedrocktools::sdk::offsets::AABBShapeComponent::mAABB);
        }
    }

    return aabb;
}

static void s_breadcrumbsTickCallback(void* _this) {
    if (!g_breadcrumbsMod || !g_breadcrumbsMod->enabled) return;
    
    g_breadcrumbsMod->tickCounter++;
    if (g_breadcrumbsMod->tickCounter >= g_breadcrumbsMod->tickInterval) {
        g_breadcrumbsMod->tickCounter = 0;
        
        uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
        if (svc != 0) {
            bedrocktools::sdk::Vec3 pos = *(bedrocktools::sdk::Vec3*)svc;
            AABB aabb = getActorAABB(_this);
            pos.y = aabb.min.y;

            std::lock_guard<std::mutex> lock(s_pointsMutex);
            
            bool shouldAdd = true;
            if (!g_breadcrumbsMod->points.empty()) {
                bedrocktools::sdk::Vec3 lastPos = g_breadcrumbsMod->points.back();
                int bx1 = std::floor(pos.x);
                int bz1 = std::floor(pos.z);
                int bx2 = std::floor(lastPos.x);
                int bz2 = std::floor(lastPos.z);
                
                if (bx1 == bx2 && bz1 == bz2) {
                    if (std::abs(pos.y - lastPos.y) < 1.0f) {
                        shouldAdd = false;
                    }
                }
            }
            
            if (shouldAdd) {
                g_breadcrumbsMod->points.push_back(pos);
                size_t pointLimit = g_breadcrumbsMod->maxPoints > 0
                    ? static_cast<size_t>(g_breadcrumbsMod->maxPoints)
                    : 1;
                if (g_breadcrumbsMod->points.size() > pointLimit) {
                    size_t excess = g_breadcrumbsMod->points.size() - pointLimit;
                    g_breadcrumbsMod->points.erase(
                        g_breadcrumbsMod->points.begin(),
                        g_breadcrumbsMod->points.begin() + excess);
                }
            }
        }
    }
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

    if (!g_breadcrumbsMod || !g_breadcrumbsMod->enabled) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || (uintptr_t)screenContext < 0x1000) return;

    std::vector<bedrocktools::sdk::Vec3> points;
    {
        std::lock_guard<std::mutex> lock(s_pointsMutex);
        points = g_breadcrumbsMod->points;
    }
    if (points.empty()) return;

    uintptr_t tessellatorPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    if (!tessellatorPtr || tessellatorPtr < 0x1000) return;
    void* tessellator = (void*)tessellatorPtr;

    uintptr_t lrpPtr = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (!lrpPtr || lrpPtr < 0x1000) return;

    float camX = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    float camY = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
    float camZ = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

    ensureMaterials();

    void* matOutline = s_matSelection ? (void*)&s_matSelection : (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f; colorHolder[1] = 1.0f; colorHolder[2] = 1.0f; colorHolder[3] = 1.0f;

    if (!points.empty()) {
        uint32_t color = g_breadcrumbsMod->trailColor;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float g = ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color      ) & 0xFF) / 255.0f;
        float baseA = ((color >> 24) & 0xFF) / 255.0f;

        char pad[0x58];

        int lineCount = points.size() * 4; 
        if (points.size() > 1) {
            lineCount += (points.size() - 1); 
        }
        
        s_tessBegin(tessellator, nullptr, 4, lineCount * 2, 0);
        
        auto emitLine = [&](bedrocktools::sdk::Vec3 p1, bedrocktools::sdk::Vec3 p2) {
            p1.x -= camX; p1.y -= camY; p1.z -= camZ;
            p2.x -= camX; p2.y -= camY; p2.z -= camZ;
            s_tessVertex(tessellator, p1.x, p1.y, p1.z);
            s_tessVertex(tessellator, p2.x, p2.y, p2.z);
        };

        for (size_t i = 0; i < points.size(); ++i) {
            bedrocktools::sdk::Vec3 p = points[i];
            float bx = std::floor(p.x);
            float by = p.y + 0.05f; 
            float bz = std::floor(p.z);
            
            float alphaFactor = 0.1f + 0.9f * ((float)(i + 1) / points.size());
            s_tessColor(tessellator, r, g, b, baseA * alphaFactor);
            
            bedrocktools::sdk::Vec3 c1 = {bx, by, bz};
            bedrocktools::sdk::Vec3 c2 = {bx + 1.0f, by, bz};
            bedrocktools::sdk::Vec3 c3 = {bx + 1.0f, by, bz + 1.0f};
            bedrocktools::sdk::Vec3 c4 = {bx, by, bz + 1.0f};
            
            emitLine(c1, c2);
            emitLine(c2, c3);
            emitLine(c3, c4);
            emitLine(c4, c1);
            
            if (i > 0) {
                bedrocktools::sdk::Vec3 prevP = points[i - 1];
                float pbx = std::floor(prevP.x);
                float pby = prevP.y + 0.05f;
                float pbz = std::floor(prevP.z);
                
                bedrocktools::sdk::Vec3 centerPrev = {pbx + 0.5f, pby, pbz + 0.5f};
                bedrocktools::sdk::Vec3 centerCurr = {bx + 0.5f, by, bz + 0.5f};
                
                emitLine(centerPrev, centerCurr);
            }
        }
        memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matOutline, pad);

        
        if (points.size() > 1) {
            s_tessBegin(tessellator, nullptr, 4, (points.size() - 1) * 4, 0);
            
            for (size_t i = 1; i < points.size(); ++i) {
                float alphaFactor = 0.1f + 0.9f * ((float)(i + 1) / points.size());
                s_tessColor(tessellator, 1.0f, 1.0f, 1.0f, baseA * alphaFactor);

                bedrocktools::sdk::Vec3 p = points[i];
                float bx = std::floor(p.x);
                float by = p.y + 0.05f;
                float bz = std::floor(p.z);
                bedrocktools::sdk::Vec3 centerCurr = {bx + 0.5f, by, bz + 0.5f};

                bedrocktools::sdk::Vec3 prevP = points[i - 1];
                float pbx = std::floor(prevP.x);
                float pby = prevP.y + 0.05f;
                float pbz = std::floor(prevP.z);
                bedrocktools::sdk::Vec3 centerPrev = {pbx + 0.5f, pby, pbz + 0.5f};

                bedrocktools::sdk::Vec3 dir = {centerCurr.x - centerPrev.x, centerCurr.y - centerPrev.y, centerCurr.z - centerPrev.z};
                float len = std::sqrt(dir.x*dir.x + dir.y*dir.y + dir.z*dir.z);
                if (len > 0.001f) {
                    dir.x /= len; dir.y /= len; dir.z /= len;
                    bedrocktools::sdk::Vec3 right = {dir.z, 0.0f, -dir.x}; 
                    
                    float arrowSize = 0.45f;
                    bedrocktools::sdk::Vec3 arrowTip = {centerCurr.x - dir.x * 0.05f, centerCurr.y - dir.y * 0.05f, centerCurr.z - dir.z * 0.05f}; 
                    bedrocktools::sdk::Vec3 w1 = {arrowTip.x - dir.x * arrowSize + right.x * (arrowSize * 0.8f), 
                               arrowTip.y - dir.y * arrowSize + right.y * (arrowSize * 0.8f), 
                               arrowTip.z - dir.z * arrowSize + right.z * (arrowSize * 0.8f)};
                    bedrocktools::sdk::Vec3 w2 = {arrowTip.x - dir.x * arrowSize - right.x * (arrowSize * 0.8f), 
                               arrowTip.y - dir.y * arrowSize - right.y * (arrowSize * 0.8f), 
                               arrowTip.z - dir.z * arrowSize - right.z * (arrowSize * 0.8f)};
                    
                    emitLine(arrowTip, w1);
                    emitLine(arrowTip, w2);
                }
            }
            memset(pad, 0, sizeof(pad));
            s_renderMesh(screenContext, tessellator, matOutline, pad);
        }
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

BreadcrumbsModule::BreadcrumbsModule()
    : Module("Breadcrumbs", "Draws a trail behind you as you walk.") {
    
    showInMenu = true;

    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMeshAddr = nullptr;
    m_renderMesh2Addr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_breadcrumbsMod = this;
}

BreadcrumbsModule::~BreadcrumbsModule() {
    if (g_breadcrumbsMod == this) g_breadcrumbsMod = nullptr;
}

void BreadcrumbsModule::onInit() {
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
        m_renderMesh2Addr = (void*)rm;
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

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_breadcrumbsTickCallback(event.player); });
}

void BreadcrumbsModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = true;
}

void BreadcrumbsModule::onEnable() {
    applyPatch();
}

void BreadcrumbsModule::onDisable() {
}

void BreadcrumbsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    tickInterval = j.value("tickInterval", tickInterval);
    maxPoints = j.value("maxPoints", maxPoints);
    
    if (j.contains("trailColor")) {
        std::string hexStr = j["trailColor"].get<std::string>();
        if (!hexStr.empty() && hexStr[0] == '#') {
            try { trailColor = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }

    
    if (j.contains("clearTrailButton") && j["clearTrailButton"].is_boolean()) {
        if (j["clearTrailButton"].get<bool>()) {
            clearTrail();
        }
    }
}

void BreadcrumbsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["tickInterval"] = tickInterval;
    j["maxPoints"] = maxPoints;

    char hexC[12];
    snprintf(hexC, sizeof(hexC), "#%08X", trailColor);
    j["trailColor"] = std::string(hexC);
    
    
    j["clearTrailButton"] = false;
}

void BreadcrumbsModule::clearTrail() {
    std::lock_guard<std::mutex> lock(s_pointsMutex);
    points.clear();
}
