#include "lightoverlay.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>

struct BlockPos {
    int x, y, z;
};


typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);


typedef void* (*BlockSource_getBlock_t)(void* region, const BlockPos& pos);
typedef float (*BlockSource_getBrightness_t)(void* region, const BlockPos& pos);
typedef bool (*BlockSource_isSolidBlockingBlock_t)(void* region, const BlockPos& pos);

static BlockSource_getBlock_t s_getBlock = nullptr;
static BlockSource_getBrightness_t s_getBrightness = nullptr;
static BlockSource_isSolidBlockingBlock_t s_isSolidBlockingBlock = nullptr;

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

static LightOverlayModule* g_lightOverlayMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr s_matSelection;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};
static void* g_localPlayer = nullptr;

static void s_lightOverlayTickCallback(void* _this) {
    if (!g_lightOverlayMod || !g_lightOverlayMod->enabled) return;
    g_localPlayer = _this;
    uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (svc != 0) {
        g_playerPos = *(bedrocktools::sdk::Vec3*)svc;
    }
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup) return {};
    HashedString hs(name);
    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial]) return {};
    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterials() {
    if (s_matSelection) return;
    if (!s_renderMaterialGroup) return;
    if (!s_matSelection) s_matSelection = getMaterial("selection_box");
}

static void drawDigit(void* tessellator, int digit, bedrocktools::sdk::Vec3 center, bedrocktools::sdk::Vec3 right, bedrocktools::sdk::Vec3 up, float scale) {
    auto emitLine = [&](float x1, float y1, float x2, float y2) {
        bedrocktools::sdk::Vec3 p1 = {center.x + (right.x * x1 * scale) + (up.x * y1 * scale),
                   center.y + (right.y * x1 * scale) + (up.y * y1 * scale),
                   center.z + (right.z * x1 * scale) + (up.z * y1 * scale)};
        bedrocktools::sdk::Vec3 p2 = {center.x + (right.x * x2 * scale) + (up.x * y2 * scale),
                   center.y + (right.y * x2 * scale) + (up.y * y2 * scale),
                   center.z + (right.z * x2 * scale) + (up.z * y2 * scale)};
        s_tessVertex(tessellator, p1.x, p1.y, p1.z);
        s_tessVertex(tessellator, p2.x, p2.y, p2.z);
    };

    float hw = 0.3f; 
    float hh = 0.4f; 

    switch (digit) {
        case 0:
            emitLine(-hw, -hh,  hw, -hh);
            emitLine( hw, -hh,  hw,  hh);
            emitLine( hw,  hh, -hw,  hh);
            emitLine(-hw,  hh, -hw, -hh);
            emitLine(-hw, -hh,  hw,  hh); 
            break;
        case 1:
            emitLine( 0.0f, -hh,  0.0f,  hh);         
            emitLine(-hw, -hh,  hw, -hh);             
            emitLine( 0.0f,  hh, -hw, 0.5f * hh);     
            break;
        case 2:
            emitLine(-hw,  hh,  hw,  hh);
            emitLine( hw,  hh,  hw, 0.0f);
            emitLine( hw, 0.0f, -hw, 0.0f);
            emitLine(-hw, 0.0f, -hw, -hh);
            emitLine(-hw, -hh,  hw, -hh);
            break;
        case 3:
            emitLine(-hw,  hh,  hw,  hh);
            emitLine( hw,  hh,  hw, -hh);
            emitLine(-hw, -hh,  hw, -hh);
            emitLine(-hw, 0.0f,  hw, 0.0f);
            break;
        case 4:
            emitLine(-hw,  hh, -hw, 0.0f);
            emitLine(-hw, 0.0f,  hw, 0.0f);
            emitLine( hw,  hh,  hw, -hh);
            break;
        case 5:
            emitLine( hw,  hh, -hw,  hh);
            emitLine(-hw,  hh, -hw, 0.0f);
            emitLine(-hw, 0.0f,  hw, 0.0f);
            emitLine( hw, 0.0f,  hw, -hh);
            emitLine( hw, -hh, -hw, -hh);
            break;
        case 6:
            emitLine( hw,  hh, -hw,  hh);
            emitLine(-hw,  hh, -hw, -hh);
            emitLine(-hw, -hh,  hw, -hh);
            emitLine( hw, -hh,  hw, 0.0f);
            emitLine( hw, 0.0f, -hw, 0.0f);
            break;
        case 7:
            emitLine(-hw,  hh,  hw,  hh);
            emitLine( hw,  hh, -hw, -hh); 
            break;
        case 8:
            emitLine(-hw, -hh,  hw, -hh);
            emitLine( hw, -hh,  hw,  hh);
            emitLine( hw,  hh, -hw,  hh);
            emitLine(-hw,  hh, -hw, -hh);
            emitLine(-hw, 0.0f,  hw, 0.0f);
            break;
        case 9:
            emitLine( hw, -hh,  hw,  hh);
            emitLine( hw,  hh, -hw,  hh);
            emitLine(-hw,  hh, -hw, 0.0f);
            emitLine(-hw, 0.0f,  hw, 0.0f);
            emitLine(-hw, -hh,  hw, -hh);
            break;
    }
}

static void drawNumber(void* tessellator, int number, bedrocktools::sdk::Vec3 center, bedrocktools::sdk::Vec3 right, bedrocktools::sdk::Vec3 up, float scale) {
    if (number < 0) number = 0;
    if (number > 99) number = 99;

    if (number < 10) {
        drawDigit(tessellator, number, center, right, up, scale);
    } else {
        int tens = number / 10;
        int ones = number % 10;
        bedrocktools::sdk::Vec3 offsetLeft = {center.x - right.x * 0.45f * scale, center.y - right.y * 0.45f * scale, center.z - right.z * 0.45f * scale};
        bedrocktools::sdk::Vec3 offsetRight = {center.x + right.x * 0.45f * scale, center.y + right.y * 0.45f * scale, center.z + right.z * 0.45f * scale};
        drawDigit(tessellator, tens, offsetLeft, right, up, scale);
        drawDigit(tessellator, ones, offsetRight, right, up, scale);
    }
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (!g_lightOverlayMod || !g_lightOverlayMod->enabled) {
        if (_renderLevel_orig) _renderLevel_orig(_this, screenContext, a3);
        return;
    }

    if (!g_localPlayer) {
        
    } else if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) {
        
    } else if (!s_getBrightness || !s_isSolidBlockingBlock) {
        
    } else {
        uintptr_t tessellatorPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
        if (tessellatorPtr && tessellatorPtr >= 0x1000) {
            void* tessellator = (void*)tessellatorPtr;

            uintptr_t lrpPtr = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
            if (lrpPtr && lrpPtr >= 0x1000) {
                float camX = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
                float camY = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 4);
                float camZ = *(float*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos + 8);

                ensureMaterials();

                void* matOutline = s_matSelection ? (void*)&s_matSelection : (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
                
                uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
                if (colorHolderPtr && colorHolderPtr >= 0x1000) {
                    float* colorHolder = (float*)colorHolderPtr;
                    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
                    colorHolder[0] = 1.0f; colorHolder[1] = 1.0f; colorHolder[2] = 1.0f; colorHolder[3] = 1.0f;

                    uintptr_t dimension = *(uintptr_t*)((uintptr_t)g_localPlayer + bedrocktools::sdk::offsets::Actor::mDimension);
                    if (dimension) {
                        uintptr_t blockSource = *(uintptr_t*)(dimension + bedrocktools::sdk::offsets::Dimension::mBlockSource);
                        if (blockSource) {
                            void* region = (void*)blockSource;

                            int rx = g_lightOverlayMod->radiusHorizontal;
                            int ry = g_lightOverlayMod->radiusVertical;
                            int rz = g_lightOverlayMod->radiusHorizontal;
                            
                            BlockPos playerPos = {(int)std::floor(g_playerPos.x), (int)std::floor(g_playerPos.y), (int)std::floor(g_playerPos.z)};

                            s_tessBegin(tessellator, nullptr, 4 , 0, 0);

                            for (int x = -rx; x <= rx; ++x) {
                                for (int y = -ry; y <= ry; ++y) {
                                    for (int z = -rz; z <= rz; ++z) {
                                        BlockPos bp = {playerPos.x + x, playerPos.y + y, playerPos.z + z};
                                        static const void* s_airBlock = nullptr;
                                        if (!s_airBlock) {
                                            s_airBlock = s_getBlock(region, {0, 32767, 0});
                                        }

                                        if (g_lightOverlayMod->onlySolidBlocks) {
                                            if (!s_isSolidBlockingBlock(region, bp)) continue;
                                        } else {
                                            if (s_getBlock(region, bp) == s_airBlock) continue;
                                        }

                                        struct FaceData {
                                            BlockPos offset;
                                            bedrocktools::sdk::Vec3 centerOffset;
                                            bedrocktools::sdk::Vec3 right;
                                            bedrocktools::sdk::Vec3 up;
                                            bool shouldCheck;
                                        };

                                        FaceData faces[6] = {
                                            { { 0,  1,  0}, { 0.5f, 1.05f, 0.5f}, { 1, 0, 0}, {0, 0, -1}, true }, 
                                            { { 0, -1,  0}, { 0.5f,-0.05f, 0.5f}, { 1, 0, 0}, {0, 0, 1}, !g_lightOverlayMod->onlyTopFace }, 
                                            { { 1,  0,  0}, { 1.05f, 0.5f, 0.5f}, { 0, 0,-1}, {0, 1, 0}, !g_lightOverlayMod->onlyTopFace }, 
                                            { {-1,  0,  0}, {-0.05f, 0.5f, 0.5f}, { 0, 0, 1}, {0, 1, 0}, !g_lightOverlayMod->onlyTopFace }, 
                                            { { 0,  0,  1}, { 0.5f, 0.5f, 1.05f}, { 1, 0, 0}, {0, 1, 0}, !g_lightOverlayMod->onlyTopFace }, 
                                            { { 0,  0, -1}, { 0.5f, 0.5f,-0.05f}, {-1, 0, 0}, {0, 1, 0}, !g_lightOverlayMod->onlyTopFace }  
                                        };

                                        for (int f = 0; f < 6; ++f) {
                                            if (!faces[f].shouldCheck) continue;

                                            BlockPos np = {bp.x + faces[f].offset.x, bp.y + faces[f].offset.y, bp.z + faces[f].offset.z};
                                            if (!s_isSolidBlockingBlock(region, np)) {
                                                int lightLevel = std::round(s_getBrightness(region, np) * 15.0f);
                                                
                                                uint32_t col = (lightLevel <= g_lightOverlayMod->dangerThreshold) 
                                                    ? g_lightOverlayMod->dangerColor 
                                                    : g_lightOverlayMod->safeColor;

                                                float r = ((col >> 16) & 0xFF) / 255.0f;
                                                float g = ((col >>  8) & 0xFF) / 255.0f;
                                                float b = ((col      ) & 0xFF) / 255.0f;
                                                float a = ((col >> 24) & 0xFF) / 255.0f;
                                                
                                                s_tessColor(tessellator, r, g, b, a);

                                                bedrocktools::sdk::Vec3 centerOrigin = {bp.x + faces[f].centerOffset.x, bp.y + faces[f].centerOffset.y, bp.z + faces[f].centerOffset.z};
                                                centerOrigin.x -= camX;
                                                centerOrigin.y -= camY;
                                                centerOrigin.z -= camZ;

                                                drawNumber(tessellator, lightLevel, centerOrigin, faces[f].right, faces[f].up, 0.4f);
                                            }
                                        }
                                    }
                                }
                            }
                            char pad[0x58];
                            memset(pad, 0, sizeof(pad));
                            s_renderMesh(screenContext, tessellator, matOutline, pad);
                        }
                    }
                    
                    colorHolder[0] = savedColor[0];
                    colorHolder[1] = savedColor[1];
                    colorHolder[2] = savedColor[2];
                    colorHolder[3] = savedColor[3];
                }
            }
        }
    }

    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }
}

LightOverlayModule::LightOverlayModule()
    : Module("Light Overlay", "Displays the light level of blocks on their faces.") {
    
    showInMenu = true;
    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMeshAddr = nullptr;
    m_renderMesh2Addr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_lightOverlayMod = this;
}

LightOverlayModule::~LightOverlayModule() {
    if (g_lightOverlayMod == this) g_lightOverlayMod = nullptr;
}

void LightOverlayModule::onInit() {
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

    uintptr_t gb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBlock);
    if (gb) s_getBlock = (BlockSource_getBlock_t)gb;

    uintptr_t gbr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceGetBrightness);
    if (gbr) s_getBrightness = (BlockSource_getBrightness_t)gbr;

    uintptr_t isb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
    if (isb) s_isSolidBlockingBlock = (BlockSource_isSolidBlockingBlock_t)isb;

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_lightOverlayTickCallback(event.player); });
}

void LightOverlayModule::applyPatch() {
    if (m_patched) return;
    if (!m_patchTarget) {
        return;
    }
    bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = true;
}

void LightOverlayModule::onEnable() {
    applyPatch();
}

void LightOverlayModule::onDisable() {
}

void LightOverlayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    radiusHorizontal = j.value("radiusHorizontal", radiusHorizontal);
    radiusVertical = j.value("radiusVertical", radiusVertical);
    onlyTopFace = j.value("onlyTopFace", onlyTopFace);
    onlySolidBlocks = j.value("onlySolidBlocks", onlySolidBlocks);
    dangerThreshold = j.value("dangerThreshold", dangerThreshold);
    
    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (j.contains(key)) {
            std::string hexStr = j[key].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { outColor = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
            }
        }
    };

    parseColor("safeColor", safeColor);
    parseColor("dangerColor", dangerColor);
}

void LightOverlayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["radiusHorizontal"] = radiusHorizontal;
    j["radiusVertical"] = radiusVertical;
    j["onlyTopFace"] = onlyTopFace;
    j["onlySolidBlocks"] = onlySolidBlocks;
    j["dangerThreshold"] = dangerThreshold;

    char hexS[12], hexD[12];
    snprintf(hexS, sizeof(hexS), "#%08X", safeColor);
    snprintf(hexD, sizeof(hexD), "#%08X", dangerColor);

    j["safeColor"] = std::string(hexS);
    j["dangerColor"] = std::string(hexD);
}
