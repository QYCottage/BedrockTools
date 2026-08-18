#include "chunkborder.hpp"
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

static ChunkBorderModule* g_chunkBorderMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static MaterialPtr s_matSelection;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};

static void s_chunkBorderTickCallback(void* _this) {
    if (!g_chunkBorderMod || !g_chunkBorderMod->enabled) return;
    uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (svc != 0) {
        g_playerPos = *(bedrocktools::sdk::Vec3*)svc;
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

    if (!g_chunkBorderMod || !g_chunkBorderMod->enabled) return;
    if (!s_tessBegin || !s_tessColor || !s_tessVertex || !s_renderMesh) return;
    if (!screenContext || (uintptr_t)screenContext < 0x1000) return;

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

    auto drawBatchedLines = [&](const std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& lines, uint32_t color) {
        if (lines.empty()) return;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float g = ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color      ) & 0xFF) / 255.0f;
        float a = ((color >> 24) & 0xFF) / 255.0f;

        s_tessBegin(tessellator, nullptr, 4, static_cast<int>(lines.size() * 2), 0);
        s_tessColor(tessellator, r, g, b, a);
        
        for (const auto& line : lines) {
            bedrocktools::sdk::Vec3 p1 = line.first;
            bedrocktools::sdk::Vec3 p2 = line.second;
            p1.x -= camX; p1.y -= camY; p1.z -= camZ;
            p2.x -= camX; p2.y -= camY; p2.z -= camZ;
            s_tessVertex(tessellator, p1.x, p1.y, p1.z);
            s_tessVertex(tessellator, p2.x, p2.y, p2.z);
        }
        
        char pad[0x58];
        memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matInner, pad);
    };

    const float bottom = -64.0f;
    const float top = 320.0f;
    const int chunkSize = 16;
    
    bedrocktools::sdk::Vec3 anchor = {
        std::floor(g_playerPos.x / 16.0f) * 16.0f,
        0.0f,
        std::floor(g_playerPos.z / 16.0f) * 16.0f
    };

    const float vls = g_chunkBorderMod->vertLineSpacing;
    const int hls = g_chunkBorderMod->horizLineSpacing;
    
    uint32_t cornerCol = g_chunkBorderMod->cornerColor;
    uint32_t midCol = g_chunkBorderMod->midColor;
    uint32_t adjCol = g_chunkBorderMod->adjColor;

    std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> cornerLines;
    std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> midLines;
    std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> adjLines;

    if (hls > 0) {
        for (int i = 0; i <= chunkSize / hls; ++i) {
            const float offset = static_cast<float>(i * hls);
            auto& targetList = (i % (chunkSize / hls) == 0) ? cornerLines : midLines;

            targetList.push_back({bedrocktools::sdk::Vec3(anchor.x + offset, bottom, anchor.z), bedrocktools::sdk::Vec3(anchor.x + offset, top, anchor.z)});
            targetList.push_back({bedrocktools::sdk::Vec3(anchor.x + offset, bottom, anchor.z + chunkSize), bedrocktools::sdk::Vec3(anchor.x + offset, top, anchor.z + chunkSize)});
            targetList.push_back({bedrocktools::sdk::Vec3(anchor.x, bottom, anchor.z + offset), bedrocktools::sdk::Vec3(anchor.x, top, anchor.z + offset)});
            targetList.push_back({bedrocktools::sdk::Vec3(anchor.x + static_cast<float>(chunkSize), bottom, anchor.z + offset), bedrocktools::sdk::Vec3(anchor.x + static_cast<float>(chunkSize), top, anchor.z + offset)});
        }
    }

    if (vls > 0.0f) {
        for (float y = bottom; y <= top; y += vls) {
            midLines.push_back({bedrocktools::sdk::Vec3(anchor.x, y, anchor.z), bedrocktools::sdk::Vec3(anchor.x + chunkSize, y, anchor.z)});
            midLines.push_back({bedrocktools::sdk::Vec3(anchor.x, y, anchor.z), bedrocktools::sdk::Vec3(anchor.x, y, anchor.z + chunkSize)});
            midLines.push_back({bedrocktools::sdk::Vec3(anchor.x + chunkSize, y, anchor.z), bedrocktools::sdk::Vec3(anchor.x + chunkSize, y, anchor.z + chunkSize)});
            midLines.push_back({bedrocktools::sdk::Vec3(anchor.x, y, anchor.z + chunkSize), bedrocktools::sdk::Vec3(anchor.x + chunkSize, y, anchor.z + chunkSize)});
        }
    }

    for (int x = -1; x <= 2; ++x) {
        for (int z = -1; z <= 2; ++z) {
            if (x >= 0 && x <= 1 && z >= 0 && z <= 1) continue;

            adjLines.push_back({
                bedrocktools::sdk::Vec3(anchor.x + x * chunkSize, bottom, anchor.z + z * chunkSize),
                bedrocktools::sdk::Vec3(anchor.x + x * chunkSize, top, anchor.z + z * chunkSize)
            });
        }
    }

    drawBatchedLines(cornerLines, cornerCol);
    drawBatchedLines(midLines, midCol);
    drawBatchedLines(adjLines, adjCol);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

ChunkBorderModule::ChunkBorderModule()
    : Module("Chunk Border", "Draws chunk borders.") {
    
    showInMenu = true;
    vertLineSpacing = 2.0f;
    horizLineSpacing = 2;
    cornerColor = 0xFF0000FF; 
    midColor = 0xFF00FFFF;    
    adjColor = 0xFFFF0000;    

    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_chunkBorderMod = this;
}

ChunkBorderModule::~ChunkBorderModule() {
    if (g_chunkBorderMod == this) g_chunkBorderMod = nullptr;
}

void ChunkBorderModule::onInit() {
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

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_chunkBorderTickCallback(event.player); });
}

void ChunkBorderModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = true;
}

void ChunkBorderModule::onEnable() {
    applyPatch();
}

void ChunkBorderModule::onDisable() {
}

void ChunkBorderModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    vertLineSpacing = j.value("vertLineSpacing", vertLineSpacing);
    horizLineSpacing = j.value("horizLineSpacing", horizLineSpacing);

    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (j.contains(key)) {
            std::string hexStr = j[key].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { outColor = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
            }
        }
    };

    parseColor("cornerColor", cornerColor);
    parseColor("midColor", midColor);
    parseColor("adjColor", adjColor);
}

void ChunkBorderModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["vertLineSpacing"] = vertLineSpacing;
    j["horizLineSpacing"] = horizLineSpacing;

    char hexC[12], hexM[12], hexA[12];
    snprintf(hexC, sizeof(hexC), "#%08X", cornerColor);
    snprintf(hexM, sizeof(hexM), "#%08X", midColor);
    snprintf(hexA, sizeof(hexA), "#%08X", adjColor);

    j["cornerColor"] = std::string(hexC);
    j["midColor"] = std::string(hexM);
    j["adjColor"] = std::string(hexA);
}
