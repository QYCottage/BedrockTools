#include "hitbox.hpp"
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

typedef bool (*Actor_isPlayer_t)(void* actor);
typedef bool (*Actor_isInvisible_t)(void* actor);
struct DistanceSortedActor {
    void* mActor;
    float mDistance;
    float _pad;
};

struct ActorVec {
    DistanceSortedActor* begin;
    DistanceSortedActor* end;
    DistanceSortedActor* cap;
};

typedef ActorVec (*Actor_fetchNearbyActorsSorted_t)(void* actor, void* extent, int actorType);

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

static HitboxModule* g_hitboxMod = nullptr;

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static Actor_isPlayer_t                   s_actorIsPlayer = nullptr;
static Actor_isInvisible_t                s_actorIsInvisible = nullptr;
static Actor_fetchNearbyActorsSorted_t    s_actorFetchNearby = nullptr;

static MaterialPtr s_matSelection;
static uintptr_t    s_renderMaterialGroup = 0;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};
static void* g_localPlayerPtr = nullptr;

struct AABB {
    bedrocktools::sdk::Vec3 min;
    bedrocktools::sdk::Vec3 max;
};

static void s_hitboxTickCallback(void* _this) {
    if (!g_hitboxMod || !g_hitboxMod->enabled) return;
    g_localPlayerPtr = _this;
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

static bedrocktools::sdk::Vec2 getActorRotation(void* actor) {
    bedrocktools::sdk::Vec2 rot = {0.f, 0.f};
    uintptr_t actorAddr = (uintptr_t)actor;
    uintptr_t rotComp = *(uintptr_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (rotComp) {
        rot = *(bedrocktools::sdk::Vec2*)rotComp;
    }
    return rot;
}

static bool hasCategory(void* actor, uint32_t categoryBit) {
    uintptr_t actorAddr = (uintptr_t)actor;
    uint32_t categories = *(uint32_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mCategories);
    return (categories & categoryBit) != 0;
}



static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    if (!g_hitboxMod || !g_hitboxMod->enabled) return;
    if (!g_localPlayerPtr) return;
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

    // Filled geometry (thick lines) uses the embedded selection overlay
    // material, the same one Breadcrumbs / Block Outline use for quads.
    void* matFill = (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
    if (!matFill) matFill = matInner;

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    // Menu thickness slider -> world-space half width. 1.0 (or lower) keeps
    // the classic hairline box; anything above is drawn as real geometry,
    // because GL line width is ignored by nearly every mobile GLES driver.
    float thicknessSetting = g_hitboxMod->lineThickness;
    if (thicknessSetting < 1.0f) thicknessSetting = 1.0f;
    if (thicknessSetting > 20.0f) thicknessSetting = 20.0f;
    const bool thickLines = thicknessSetting > 1.05f;
    const float halfWidth = thicknessSetting * 0.01f * 0.5f;

    auto drawLines = [&](const std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& lines, uint32_t color) {
        if (lines.empty()) return;
        float r = ((color >> 16) & 0xFF) / 255.0f;
        float g = ((color >>  8) & 0xFF) / 255.0f;
        float b = ((color      ) & 0xFF) / 255.0f;
        float a = ((color >> 24) & 0xFF) / 255.0f;

        char pad[0x58];

        // Thick pass: every segment becomes a camera-facing quad, so the
        // apparent width follows the thickness setting from any angle.
        if (thickLines) {
            s_tessBegin(tessellator, nullptr, 1, static_cast<int>(lines.size() * 8), 0);
            s_tessColor(tessellator, r, g, b, a);

            for (const auto& line : lines) {
                bedrocktools::sdk::Vec3 p1 = line.first;
                bedrocktools::sdk::Vec3 p2 = line.second;
                p1.x -= camX; p1.y -= camY; p1.z -= camZ;
                p2.x -= camX; p2.y -= camY; p2.z -= camZ;

                float dx = p2.x - p1.x;
                float dy = p2.y - p1.y;
                float dz = p2.z - p1.z;
                float len = sqrtf(dx * dx + dy * dy + dz * dz);
                if (len < 1e-5f) continue;
                dx /= len; dy /= len; dz /= len;

                // The camera sits at the origin of this relative space, so
                // the vector to the segment midpoint is the view direction.
                float mx = (p1.x + p2.x) * 0.5f;
                float my = (p1.y + p2.y) * 0.5f;
                float mz = (p1.z + p2.z) * 0.5f;

                // side = dir x view, i.e. perpendicular to both the segment
                // and the eye ray => the quad always faces the player.
                float sx = dy * mz - dz * my;
                float sy = dz * mx - dx * mz;
                float sz = dx * my - dy * mx;
                float sLen = sqrtf(sx * sx + sy * sy + sz * sz);
                if (sLen < 1e-5f) {
                    // Looking straight down the segment: pick any perpendicular.
                    if (fabsf(dy) < 0.9f) { sx = -dz; sy = 0.0f; sz = dx; }
                    else { sx = 1.0f; sy = 0.0f; sz = 0.0f; }
                    sLen = sqrtf(sx * sx + sy * sy + sz * sz);
                    if (sLen < 1e-5f) continue;
                }
                sx = sx / sLen * halfWidth;
                sy = sy / sLen * halfWidth;
                sz = sz / sLen * halfWidth;

                // Overshoot both ends by half the width so corners stay solid.
                float ex = dx * halfWidth;
                float ey = dy * halfWidth;
                float ez = dz * halfWidth;

                bedrocktools::sdk::Vec3 quad[4] = {
                    {p1.x - ex - sx, p1.y - ey - sy, p1.z - ez - sz},
                    {p2.x + ex - sx, p2.y + ey - sy, p2.z + ez - sz},
                    {p2.x + ex + sx, p2.y + ey + sy, p2.z + ez + sz},
                    {p1.x - ex + sx, p1.y - ey + sy, p1.z - ez + sz}
                };

                // Emitted with both windings so back-face culling never
                // eats a segment.
                for (int i = 0; i < 4; ++i)
                    s_tessVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
                for (int i = 3; i >= 0; --i)
                    s_tessVertex(tessellator, quad[i].x, quad[i].y, quad[i].z);
            }

            memset(pad, 0, sizeof(pad));
            s_renderMesh(screenContext, tessellator, matFill, pad);
        }

        // Core hairline pass: keeps the edge crisp and visible even when the
        // quads shrink below a pixel at long range.
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

        memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matInner, pad);
    };

    auto drawBox = [&](const AABB& aabb, uint32_t color) {
        std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lines;
        bedrocktools::sdk::Vec3 mn = aabb.min;
        bedrocktools::sdk::Vec3 mx = aabb.max;

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}});

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}});

        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mn.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mn.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mx.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mx.x, mx.y, mx.z}});
        lines.push_back({bedrocktools::sdk::Vec3{mn.x, mn.y, mx.z}, bedrocktools::sdk::Vec3{mn.x, mx.y, mx.z}});

        drawLines(lines, color);
    };

    bedrocktools::sdk::Vec3 localPos = g_playerPos;
    float dx = camX - localPos.x;
    float dy = camY - (localPos.y + 1.62f);
    float dz = camZ - localPos.z;
    bool isThirdPerson = (dx*dx + dy*dy + dz*dz) > 0.05f;

    auto renderActor = [&](void* ent) {
        AABB aabb = getActorAABB(ent);
        if (aabb.min.x == 0.f && aabb.min.y == 0.f && aabb.min.z == 0.f &&
            aabb.max.x == 0.f && aabb.max.y == 0.f && aabb.max.z == 0.f) return;

        uint32_t boxColor = g_hitboxMod->hitboxColor;
        if (g_hitboxMod->hitboxIndicator) {
            // The indicator may only become active for the entity currently
            // under the crosshair. Nearby entities that the player is not
            // looking at must keep the default indicator color.
            boxColor = g_hitboxMod->indicatorDefaultColor;
            if (ent == selectedEntity) {
                // Closest-point distance from the local player to the entity's
                // AABB. Using a clamp (rather than face/center distance) means
                // tall or wide actors register as "in hitrange" the moment the
                // player is within reach of any part of them, which matches
                // how MCBE melee reach is measured.
                float cx = localPos.x;
                if (cx < aabb.min.x) cx = aabb.min.x;
                if (cx > aabb.max.x) cx = aabb.max.x;

                float cy = localPos.y;
                if (cy < aabb.min.y) cy = aabb.min.y;
                if (cy > aabb.max.y) cy = aabb.max.y;

                float cz = localPos.z;
                if (cz < aabb.min.z) cz = aabb.min.z;
                if (cz > aabb.max.z) cz = aabb.max.z;

                float dxA = localPos.x - cx;
                float dyA = localPos.y - cy;
                float dzA = localPos.z - cz;
                float distSq = dxA * dxA + dyA * dyA + dzA * dzA;

                const float range = g_hitboxMod->hitRange;
                if (distSq <= range * range) {
                    boxColor = g_hitboxMod->indicatorActiveColor;
                }
            }
        }

        drawBox(aabb, boxColor);

        if (g_hitboxMod->showEyeLine) {
            float minX = aabb.min.x;
            float maxX = aabb.max.x;
            float minZ = aabb.min.z;
            float maxZ = aabb.max.z;

            float entityHeight = aabb.max.y - aabb.min.y;
            float eyeHeight = aabb.min.y + entityHeight * 0.85f;

            std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> eyeLines;
            eyeLines.push_back({bedrocktools::sdk::Vec3{minX, eyeHeight, minZ}, bedrocktools::sdk::Vec3{maxX, eyeHeight, minZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{maxX, eyeHeight, minZ}, bedrocktools::sdk::Vec3{maxX, eyeHeight, maxZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{maxX, eyeHeight, maxZ}, bedrocktools::sdk::Vec3{minX, eyeHeight, maxZ}});
            eyeLines.push_back({bedrocktools::sdk::Vec3{minX, eyeHeight, maxZ}, bedrocktools::sdk::Vec3{minX, eyeHeight, minZ}});
            drawLines(eyeLines, g_hitboxMod->eyeLineColor);
        }

        if (g_hitboxMod->showLookLine) {
            bedrocktools::sdk::Vec2 rot = getActorRotation(ent);
            static constexpr float PI = 3.14159265f;
            static constexpr float DEG_TO_RAD = PI / 180.0f;

            float yawR = rot.y * DEG_TO_RAD;
            float pitchR = rot.x * DEG_TO_RAD;
            float dirX = -sinf(yawR) * cosf(pitchR);
            float dirY = -sinf(pitchR);
            float dirZ = cosf(yawR) * cosf(pitchR);

            float entityHeight = aabb.max.y - aabb.min.y;
            float eyeHeight = aabb.min.y + entityHeight * 0.85f;
            float centerX = (aabb.min.x + aabb.max.x) * 0.5f;
            float centerZ = (aabb.min.z + aabb.max.z) * 0.5f;

            bedrocktools::sdk::Vec3 start = {centerX, eyeHeight, centerZ};
            float lineLen = g_hitboxMod->lookLineLength;
            bedrocktools::sdk::Vec3 end = {start.x + dirX * lineLen, start.y + dirY * lineLen, start.z + dirZ * lineLen};

            std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lookLines;
            lookLines.push_back({start, end});
            drawLines(lookLines, g_hitboxMod->lookLineColor);
        }
    };

    if (g_hitboxMod->showSelf && isThirdPerson) {
        renderActor(g_localPlayerPtr);
    }

    if (s_actorFetchNearby) {
        bedrocktools::sdk::Vec3 extent = {30.0f, 30.0f, 30.0f};
        ActorVec actors = s_actorFetchNearby(g_localPlayerPtr, &extent, 1);

        if (actors.begin && actors.end) {
            for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
                void* ent = it->mActor;
                if (!ent || ent == g_localPlayerPtr) continue;

                bool isPlayer = false;
                if (s_actorIsPlayer) {
                    isPlayer = s_actorIsPlayer(ent);
                }

                if (isPlayer && !g_hitboxMod->showPlayers) continue;

                if (!isPlayer && (!g_hitboxMod->showEntities || !hasCategory(ent, 2))) continue;

                if (s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

                renderActor(ent);
            }
        }
    }

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

HitboxModule::HitboxModule()
    : Module("Hitbox", "Displays hitboxes of entities.") {

    showInMenu = true;

    m_patched = false;
    m_patchTarget = nullptr;
    m_tessBeginAddr = nullptr;
    m_tessColorAddr = nullptr;
    m_tessVertexAddr = nullptr;
    m_renderMaterialGroupAddr = nullptr;
    g_hitboxMod = this;
}

HitboxModule::~HitboxModule() {
    if (g_hitboxMod == this) g_hitboxMod = nullptr;
}

void HitboxModule::onInit() {
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

    uintptr_t aip = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer);
    if (aip) s_actorIsPlayer = (Actor_isPlayer_t)aip;

    uintptr_t aii = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible);
    if (aii) s_actorIsInvisible = (Actor_isInvisible_t)aii;

    uintptr_t afn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted);
    if (afn) s_actorFetchNearby = (Actor_fetchNearbyActorsSorted_t)afn;

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_hitboxTickCallback(event.player); });
}

void HitboxModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    bedrocktools::hooks::install(m_patchTarget, (void*)_renderLevel_hook, (void**)&_renderLevel_orig);
    m_patched = true;
}

void HitboxModule::onEnable() {
    applyPatch();
}

void HitboxModule::onDisable() {
}

void HitboxModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showEntities = j.value("showEntities", showEntities);
    showPlayers = j.value("showPlayers", showPlayers);
    showSelf = j.value("showSelf", showSelf);
    showEyeLine = j.value("showEyeLine", showEyeLine);
    showLookLine = j.value("showLookLine", showLookLine);
    lookLineLength = j.value("lookLineLength", lookLineLength);

    if (j.contains("lineThickness")) {
        try { lineThickness = j["lineThickness"].get<float>(); } catch (...) {}
    }
    if (lineThickness < 1.0f) lineThickness = 1.0f;
    if (lineThickness > 20.0f) lineThickness = 20.0f;

    if (j.contains("hitboxIndicator")) {
        hitboxIndicator = j["hitboxIndicator"].get<bool>();
    }
    if (j.contains("hitRange")) {
        try { hitRange = j["hitRange"].get<float>(); } catch (...) {}
    }

    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (j.contains(key)) {
            std::string hexStr = j[key].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try { outColor = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
            }
        }
    };

    parseColor("hitboxColor", hitboxColor);
    parseColor("eyeLineColor", eyeLineColor);
    parseColor("lookLineColor", lookLineColor);
    parseColor("indicatorDefaultColor", indicatorDefaultColor);
    parseColor("indicatorActiveColor", indicatorActiveColor);
}

void HitboxModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showEntities"] = showEntities;
    j["showPlayers"] = showPlayers;
    j["showSelf"] = showSelf;
    j["showEyeLine"] = showEyeLine;
    j["showLookLine"] = showLookLine;
    j["lookLineLength"] = lookLineLength;
    j["lineThickness"] = lineThickness;
    j["hitboxIndicator"] = hitboxIndicator;
    j["hitRange"] = hitRange;

    char hexH[12], hexE[12], hexL[12], hexD[12], hexA[12];
    snprintf(hexH, sizeof(hexH), "#%08X", hitboxColor);
    snprintf(hexE, sizeof(hexE), "#%08X", eyeLineColor);
    snprintf(hexL, sizeof(hexL), "#%08X", lookLineColor);
    snprintf(hexD, sizeof(hexD), "#%08X", indicatorDefaultColor);
    snprintf(hexA, sizeof(hexA), "#%08X", indicatorActiveColor);

    j["hitboxColor"] = std::string(hexH);
    j["eyeLineColor"] = std::string(hexE);
    j["lookLineColor"] = std::string(hexL);
    j["indicatorDefaultColor"] = std::string(hexD);
    j["indicatorActiveColor"] = std::string(hexA);
}
