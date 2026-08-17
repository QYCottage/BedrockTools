#include "hitbox.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <cstring>
#include <vector>
#include <utility>

typedef void (*Tessellator_begin_t)(void* tessellator, void* debugCallback, int primitiveMode, int vertexCount, int noIndices);
typedef void (*Tessellator_color_t)(void* tessellator, float r, float g, float b, float a);
typedef void (*Tessellator_vertex_t)(void* tessellator, float x, float y, float z);
typedef void (*MeshHelpers_renderMeshImmediately_t)(void* screenContext, void* tessellator, void* material, char* pad);

typedef void* (*HitResult_getEntity_t)(void* hitResult);
typedef void* (*Level_getHitResult_t)(void* level);
typedef bool (*Actor_isPlayer_t)(void* actor);
typedef bool (*Actor_isInvisible_t)(void* actor);
typedef bool (*BlockSource_isSolidBlockingBlock_t)(void* region, const bedrocktools::sdk::BlockPos& pos);
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
    void* sharedPtrData[2];

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
static std::atomic_bool g_crosshairCanHit{false};

// True while the Crosshair Indicator owns the cursor: the vanilla white
// crosshair must not be drawn because the module paints a red one instead.
bool shouldHideVanillaCrosshair() {
    return g_hitboxMod && g_hitboxMod->enabled &&
           g_hitboxMod->crosshairIndicator &&
           g_crosshairCanHit.load(std::memory_order_relaxed);
}

static Tessellator_begin_t                s_tessBegin = nullptr;
static Tessellator_color_t                s_tessColor = nullptr;
static Tessellator_vertex_t               s_tessVertex = nullptr;
static MeshHelpers_renderMeshImmediately_t s_renderMesh = nullptr;

static HitResult_getEntity_t              s_hitResultGetEntity = nullptr;
static Level_getHitResult_t               s_getHitResult = nullptr;
static Actor_isPlayer_t                   s_actorIsPlayer = nullptr;
static Actor_isInvisible_t                s_actorIsInvisible = nullptr;
static Actor_fetchNearbyActorsSorted_t    s_actorFetchNearby = nullptr;
static BlockSource_isSolidBlockingBlock_t s_isSolidBlockingBlock = nullptr;

static MaterialPtr s_matSelection;
static MaterialPtr s_matOpaqueFill;
static uintptr_t    s_renderMaterialGroup = 0;

// Fixed values for settings that were removed from the module menu:
// the look line keeps its classic length / blue color, and the Hitbox
// Indicator uses the vanilla Bedrock melee reach (3 blocks) plus a small
// margin, matching the previous defaults.
static constexpr float    kLookLineLength       = 2.0f;
static constexpr uint32_t kLookLineColor        = 0xFF0000FF;
static constexpr float    kHitboxIndicatorRange = 3.0f;

static void (*_renderLevel_orig)(void* _this, void* screenContext, void* a3);

static bedrocktools::sdk::Vec3 g_playerPos = {0.f, 0.f, 0.f};
static void* g_localPlayerPtr = nullptr;

struct AABB {
    bedrocktools::sdk::Vec3 min;
    bedrocktools::sdk::Vec3 max;
};

static void s_hitboxTickCallback(void* _this) {
    // Always keep the local player pointer fresh so the first frame after
    // enabling the module can already draw. Position is only needed while
    // the overlay is on.
    g_localPlayerPtr = _this;
    if (!g_hitboxMod || !g_hitboxMod->enabled) return;
    uintptr_t svc = *(uintptr_t*)((uintptr_t)_this + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
    if (svc != 0) {
        g_playerPos = *(bedrocktools::sdk::Vec3*)svc;
    }
}

static MaterialPtr getMaterial(const char* name) {
    if (!s_renderMaterialGroup || !name) return {};

    HashedString hs(name);

    void** vtable = *reinterpret_cast<void***>(s_renderMaterialGroup);
    const auto getMatIndex =
        bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial;
    if (!vtable || !vtable[getMatIndex]) return {};

    using getMat_t = MaterialPtr(*)(void*, const HashedString*);
    return reinterpret_cast<getMat_t>(vtable[getMatIndex])((void*)s_renderMaterialGroup, &hs);
}

static void ensureMaterials() {
    if (!s_renderMaterialGroup) return;

    // selection_box is the proven untextured line material used by Chunk
    // Border / Breadcrumbs / Block Outline. Entity materials
    // (entity_alphatest / entity_alphablend / entity) require a bound
    // texture and UVs, so Tessellator_vertex-only geometry is discarded
    // and the hitbox never appears.
    if (!s_matSelection) {
        s_matSelection = getMaterial("selection_box");
    }

    // Vertex-color fill for the thickened outline. Prefer ui_fill_color
    // because it does not sample a texture.
    if (!s_matOpaqueFill) {
        static const char* kFill[] = {
            "ui_fill_color",
            "selection_box",
            "ui_textured_and_glcolor"
        };
        for (const char* name : kFill) {
            s_matOpaqueFill = getMaterial(name);
            if (s_matOpaqueFill) break;
        }
    }
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

// 3D DDA from the camera to a world point. Returns false when a solid
// blocking block sits between the two points. The start cell (camera) and
// the destination cell (inside the entity AABB) are not treated as walls,
// so standing inside a block or aiming at a mob in the same cell still
// shows the hitbox. Fail-open when BlockSource is unavailable so the
// overlay never silently disappears.
static bool hasLineOfSight(void* region, float x0, float y0, float z0,
                           float x1, float y1, float z1) {
    if (!region || !s_isSolidBlockingBlock) return true;

    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float dz = z1 - z0;
    const float lenSq = dx * dx + dy * dy + dz * dz;
    if (lenSq < 1e-8f) return true;

    int x = static_cast<int>(std::floor(x0));
    int y = static_cast<int>(std::floor(y0));
    int z = static_cast<int>(std::floor(z0));
    const int xEnd = static_cast<int>(std::floor(x1));
    const int yEnd = static_cast<int>(std::floor(y1));
    const int zEnd = static_cast<int>(std::floor(z1));
    if (x == xEnd && y == yEnd && z == zEnd) return true;

    const float len = std::sqrt(lenSq);
    const float invLen = 1.0f / len;
    const float dirX = dx * invLen;
    const float dirY = dy * invLen;
    const float dirZ = dz * invLen;

    const int stepX = (dirX > 0.0f) ? 1 : (dirX < 0.0f ? -1 : 0);
    const int stepY = (dirY > 0.0f) ? 1 : (dirY < 0.0f ? -1 : 0);
    const int stepZ = (dirZ > 0.0f) ? 1 : (dirZ < 0.0f ? -1 : 0);

    auto firstBoundary = [](float origin, float dir, int cell, int step) -> float {
        if (step == 0) return 1e30f;
        const float boundary = (step > 0)
            ? static_cast<float>(cell + 1)
            : static_cast<float>(cell);
        return (boundary - origin) / dir;
    };

    float tMaxX = firstBoundary(x0, dirX, x, stepX);
    float tMaxY = firstBoundary(y0, dirY, y, stepY);
    float tMaxZ = firstBoundary(z0, dirZ, z, stepZ);
    const float tDeltaX = (stepX != 0) ? std::abs(1.0f / dirX) : 1e30f;
    const float tDeltaY = (stepY != 0) ? std::abs(1.0f / dirY) : 1e30f;
    const float tDeltaZ = (stepZ != 0) ? std::abs(1.0f / dirZ) : 1e30f;

    int guard = 0;
    while (guard++ < 256) {
        if (tMaxX < tMaxY) {
            if (tMaxX < tMaxZ) {
                if (tMaxX > len) break;
                x += stepX;
                tMaxX += tDeltaX;
            } else {
                if (tMaxZ > len) break;
                z += stepZ;
                tMaxZ += tDeltaZ;
            }
        } else if (tMaxY < tMaxZ) {
            if (tMaxY > len) break;
            y += stepY;
            tMaxY += tDeltaY;
        } else {
            if (tMaxZ > len) break;
            z += stepZ;
            tMaxZ += tDeltaZ;
        }

        if (x == xEnd && y == yEnd && z == zEnd) return true;

        bedrocktools::sdk::BlockPos bp{x, y, z};
        if (s_isSolidBlockingBlock(region, bp)) return false;
    }
    return true;
}

static bool aabbVisibleFromCamera(void* region, const AABB& aabb,
                                  float camX, float camY, float camZ) {
    if (!region || !s_isSolidBlockingBlock) return true;

    if (camX >= aabb.min.x && camX <= aabb.max.x &&
        camY >= aabb.min.y && camY <= aabb.max.y &&
        camZ >= aabb.min.z && camZ <= aabb.max.z) {
        return true;
    }

    // Inset so rays land inside the volume instead of grazing the floor
    // the entity is standing on (which would look like a wall).
    const float eps = 0.08f;
    float minX = aabb.min.x + eps, maxX = aabb.max.x - eps;
    float minY = aabb.min.y + eps, maxY = aabb.max.y - eps;
    float minZ = aabb.min.z + eps, maxZ = aabb.max.z - eps;
    if (minX > maxX) minX = maxX = (aabb.min.x + aabb.max.x) * 0.5f;
    if (minY > maxY) minY = maxY = (aabb.min.y + aabb.max.y) * 0.5f;
    if (minZ > maxZ) minZ = maxZ = (aabb.min.z + aabb.max.z) * 0.5f;

    const float midX = (minX + maxX) * 0.5f;
    const float midY = (minY + maxY) * 0.5f;
    const float midZ = (minZ + maxZ) * 0.5f;

    const bedrocktools::sdk::Vec3 samples[] = {
        {midX, midY, midZ},
        {minX, minY, minZ}, {maxX, minY, minZ},
        {minX, minY, maxZ}, {maxX, minY, maxZ},
        {minX, maxY, minZ}, {maxX, maxY, minZ},
        {minX, maxY, maxZ}, {maxX, maxY, maxZ},
        {midX, midY, minZ}, {midX, midY, maxZ},
        {minX, midY, midZ}, {maxX, midY, midZ},
        {midX, maxY, midZ}
    };

    for (const auto& sample : samples) {
        if (hasLineOfSight(region, camX, camY, camZ, sample.x, sample.y, sample.z))
            return true;
    }
    return false;
}

static void* getLocalBlockSource() {
    if (!g_localPlayerPtr) return nullptr;
    uintptr_t dimension = *(uintptr_t*)((uintptr_t)g_localPlayerPtr +
                                        bedrocktools::sdk::offsets::Actor::mDimension);
    if (!dimension || dimension < 0x1000) return nullptr;
    uintptr_t blockSource = *(uintptr_t*)(dimension +
                                          bedrocktools::sdk::offsets::Dimension::mBlockSource);
    if (!blockSource || blockSource < 0x1000) return nullptr;
    return (void*)blockSource;
}

static void unpackOpaqueColor(uint32_t color, float& r, float& g, float& b, float& a) {
    r = ((color >> 16) & 0xFF) / 255.0f;
    g = ((color >>  8) & 0xFF) / 255.0f;
    b = ((color      ) & 0xFF) / 255.0f;
    a = 1.0f;
}


struct PickRay {
    float ox = 0.f, oy = 0.f, oz = 0.f;
    float dx = 0.f, dy = 0.f, dz = 1.f;
    bool valid = false;
};

static bedrocktools::sdk::Vec3 lookDirFromRotation(const bedrocktools::sdk::Vec2& rot) {
    static constexpr float kDeg2Rad = 3.14159265f / 180.0f;
    const float yawR = rot.y * kDeg2Rad;
    const float pitchR = rot.x * kDeg2Rad;
    const float cp = cosf(pitchR);
    return {-sinf(yawR) * cp, -sinf(pitchR), cosf(yawR) * cp};
}

// Slab-method ray vs AABB. `pad` inflates the box a few centimetres so a
// crosshair that is just grazing the hitbox still counts as a melee aim.
static bool rayHitsAABB(const PickRay& ray, const AABB& aabb, float maxDist, float pad = 0.08f) {
    if (!ray.valid || maxDist <= 0.0f) return false;

    float tmin = 0.0f;
    float tmax = maxDist;
    bool missed = false;

    auto intersectSlab = [&](float origin, float dir, float bmin, float bmax) {
        if (missed) return;
        bmin -= pad;
        bmax += pad;
        if (std::abs(dir) < 1e-8f) {
            if (origin < bmin || origin > bmax) missed = true;
            return;
        }
        float invD = 1.0f / dir;
        float t1 = (bmin - origin) * invD;
        float t2 = (bmax - origin) * invD;
        if (t1 > t2) std::swap(t1, t2);
        tmin = std::max(tmin, t1);
        tmax = std::min(tmax, t2);
        if (tmin > tmax) missed = true;
    };

    intersectSlab(ray.ox, ray.dx, aabb.min.x, aabb.max.x);
    intersectSlab(ray.oy, ray.dy, aabb.min.y, aabb.max.y);
    intersectSlab(ray.oz, ray.dz, aabb.min.z, aabb.max.z);
    return !missed && tmin <= maxDist;
}

static float distanceToAABB(float x, float y, float z, const AABB& aabb) {
    const float dx = std::max(std::max(aabb.min.x - x, 0.0f), x - aabb.max.x);
    const float dy = std::max(std::max(aabb.min.y - y, 0.0f), y - aabb.max.y);
    const float dz = std::max(std::max(aabb.min.z - z, 0.0f), z - aabb.max.z);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// Prefer Level::getHitResult() (same path Block Outline / Reach Counter use).
// The raw mHitResultWrapper offset is only a fallback — it does not always
// point at the live pick used for melee.
static void* resolveHitResult(void* level) {
    if (!level || reinterpret_cast<uintptr_t>(level) < 0x1000) return nullptr;
    if (s_getHitResult) {
        void* hit = s_getHitResult(level);
        if (hit && reinterpret_cast<uintptr_t>(hit) >= 0x1000) return hit;
    }
    uintptr_t wrapper = reinterpret_cast<uintptr_t>(level) +
                        bedrocktools::sdk::offsets::Level::mHitResultWrapper;
    void* hit = reinterpret_cast<void*>(
        wrapper + bedrocktools::sdk::offsets::HitResultWrapper::mHitResult);
    if (hit && reinterpret_cast<uintptr_t>(hit) >= 0x1000) return hit;
    return nullptr;
}

static PickRay pickRayFromHitResult(void* hitResult) {
    PickRay ray;
    if (!hitResult) return ray;
    const auto start = *reinterpret_cast<bedrocktools::sdk::Vec3*>(
        reinterpret_cast<uintptr_t>(hitResult) +
        bedrocktools::sdk::offsets::HitResult::mStartPos);
    const auto dir = *reinterpret_cast<bedrocktools::sdk::Vec3*>(
        reinterpret_cast<uintptr_t>(hitResult) +
        bedrocktools::sdk::offsets::HitResult::mRayDir);
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-5f) return ray;
    ray.ox = start.x;
    ray.oy = start.y;
    ray.oz = start.z;
    ray.dx = dir.x / len;
    ray.dy = dir.y / len;
    ray.dz = dir.z / len;
    ray.valid = true;
    return ray;
}

static PickRay pickRayFromActor(void* actor, const AABB& localAabb,
                                float camX, float camY, float camZ,
                                bool isThirdPerson,
                                const bedrocktools::sdk::Vec3& localPos) {
    PickRay ray;
    if (!isThirdPerson) {
        ray.ox = camX;
        ray.oy = camY;
        ray.oz = camZ;
    } else if (localAabb.max.y > localAabb.min.y) {
        ray.ox = (localAabb.min.x + localAabb.max.x) * 0.5f;
        ray.oy = localAabb.min.y + (localAabb.max.y - localAabb.min.y) * 0.9f;
        ray.oz = (localAabb.min.z + localAabb.max.z) * 0.5f;
    } else {
        ray.ox = localPos.x;
        ray.oy = localPos.y + 1.62f;
        ray.oz = localPos.z;
    }
    const auto dir = lookDirFromRotation(getActorRotation(actor));
    ray.dx = dir.x;
    ray.dy = dir.y;
    ray.dz = dir.z;
    ray.valid = true;
    return ray;
}

static void _renderLevel_hook(void* _this, void* screenContext, void* a3) {
    if (_renderLevel_orig) {
        _renderLevel_orig(_this, screenContext, a3);
    }

    // The HUD is rendered after the world. Clear the previous frame first so
    // the cursor immediately returns to white when the target leaves reach,
    // becomes occluded, or the player looks away.
    g_crosshairCanHit.store(false, std::memory_order_relaxed);

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

    // Same fallback as Chunk Border / Breadcrumbs / Block Outline: if the
    // named materials did not resolve, the embedded selection overlay on
    // LevelRendererPlayer still draws untextured colored lines/quads.
    void* overlayMaterial = (void*)(lrpPtr + bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);
    void* matInner = s_matSelection ? (void*)&s_matSelection : overlayMaterial;
    void* matFill = s_matOpaqueFill ? (void*)&s_matOpaqueFill : overlayMaterial;
    if (!matInner && !matFill) return;

    uintptr_t colorHolderPtr = *(uintptr_t*)((uintptr_t)screenContext + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (!colorHolderPtr || colorHolderPtr < 0x1000) return;
    float* colorHolder = (float*)colorHolderPtr;

    float savedColor[4] = { colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3] };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    // World-space line width. Menu units match Block Outline:
    // thickness * 0.01 blocks. 0 keeps the classic single-pixel GL_LINES look.
    // Thickness is applied only to the hitbox outline, never to eye / look lines.
    const float lineThickness =
        std::min(std::max(g_hitboxMod->thickness, 0.0f) * 0.01f, 0.45f);

    struct ThickQuad {
        bedrocktools::sdk::Vec3 v[4];
    };

    auto cameraFacingSide = [&](const bedrocktools::sdk::Vec3& a,
                                const bedrocktools::sdk::Vec3& b,
                                bedrocktools::sdk::Vec3& outSide) -> bool {
        bedrocktools::sdk::Vec3 dir = {b.x - a.x, b.y - a.y, b.z - a.z};
        float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len < 1e-5f) return false;
        dir.x /= len; dir.y /= len; dir.z /= len;

        bedrocktools::sdk::Vec3 mid = {
            (a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, (a.z + b.z) * 0.5f
        };
        bedrocktools::sdk::Vec3 toCam = {camX - mid.x, camY - mid.y, camZ - mid.z};
        outSide = {
            dir.y * toCam.z - dir.z * toCam.y,
            dir.z * toCam.x - dir.x * toCam.z,
            dir.x * toCam.y - dir.y * toCam.x
        };
        float sideLen = std::sqrt(outSide.x * outSide.x + outSide.y * outSide.y + outSide.z * outSide.z);
        if (sideLen < 1e-4f) {
            bedrocktools::sdk::Vec3 up = {0.f, 1.f, 0.f};
            outSide = {
                dir.y * up.z - dir.z * up.y,
                dir.z * up.x - dir.x * up.z,
                dir.x * up.y - dir.y * up.x
            };
            sideLen = std::sqrt(outSide.x * outSide.x + outSide.y * outSide.y + outSide.z * outSide.z);
            if (sideLen < 1e-4f) {
                up = {1.f, 0.f, 0.f};
                outSide = {
                    dir.y * up.z - dir.z * up.y,
                    dir.z * up.x - dir.x * up.z,
                    dir.x * up.y - dir.y * up.x
                };
                sideLen = std::sqrt(outSide.x * outSide.x + outSide.y * outSide.y + outSide.z * outSide.z);
                if (sideLen < 1e-4f) return false;
            }
        }
        outSide.x /= sideLen; outSide.y /= sideLen; outSide.z /= sideLen;
        return true;
    };

    auto addCameraFacingRibbon = [&](std::vector<ThickQuad>& quads,
                                     const bedrocktools::sdk::Vec3& a,
                                     const bedrocktools::sdk::Vec3& b,
                                     float t) {
        if (t <= 0.0f) return;
        bedrocktools::sdk::Vec3 side;
        if (!cameraFacingSide(a, b, side)) return;
        const float h = t * 0.5f;
        const bedrocktools::sdk::Vec3 o = {side.x * h, side.y * h, side.z * h};
        quads.push_back({{
            {a.x + o.x, a.y + o.y, a.z + o.z},
            {a.x - o.x, a.y - o.y, a.z - o.z},
            {b.x - o.x, b.y - o.y, b.z - o.z},
            {b.x + o.x, b.y + o.y, b.z + o.z}
        }});
    };

    auto drawQuads = [&](const std::vector<ThickQuad>& quads, uint32_t color) {
        if (quads.empty() || !matFill) return;
        float r, g, b, a;
        unpackOpaqueColor(color, r, g, b, a);

        // Quad list; each quad is emitted twice (both windings) so it is
        // visible from either side.
        s_tessBegin(tessellator, nullptr, 1, static_cast<int>(quads.size() * 8), 0);
        s_tessColor(tessellator, r, g, b, a);

        for (const auto& quad : quads) {
            for (int i = 0; i < 4; ++i) {
                s_tessVertex(tessellator,
                             quad.v[i].x - camX,
                             quad.v[i].y - camY,
                             quad.v[i].z - camZ);
            }
            for (int i = 3; i >= 0; --i) {
                s_tessVertex(tessellator,
                             quad.v[i].x - camX,
                             quad.v[i].y - camY,
                             quad.v[i].z - camZ);
            }
        }

        char pad[0x58];
        memset(pad, 0, sizeof(pad));
        s_renderMesh(screenContext, tessellator, matFill, pad);
    };

    auto drawLines = [&](const std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>>& lines, uint32_t color) {
        if (lines.empty()) return;
        float r, g, b, a;
        unpackOpaqueColor(color, r, g, b, a);

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

        // Only the hitbox outline grows with Thickness. Camera-facing opaque
        // ribbons plus a few parallel selection_box lines keep the result
        // solid white instead of a translucent overlay frame.
        if (lineThickness > 0.0f) {
            std::vector<ThickQuad> quads;
            quads.reserve(lines.size());
            for (const auto& line : lines) {
                addCameraFacingRibbon(quads, line.first, line.second, lineThickness);
            }
            drawQuads(quads, color);

            const int steps = std::clamp(
                static_cast<int>(std::lround(g_hitboxMod->thickness)), 1, 8);
            std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> bundle;
            bundle.reserve(lines.size() * static_cast<size_t>(2 * steps + 1));
            for (const auto& line : lines) {
                const bedrocktools::sdk::Vec3& a = line.first;
                const bedrocktools::sdk::Vec3& b = line.second;
                bedrocktools::sdk::Vec3 side;
                if (!cameraFacingSide(a, b, side)) {
                    bundle.push_back(line);
                    continue;
                }
                const float h = lineThickness * 0.5f;
                for (int i = -steps; i <= steps; ++i) {
                    const float f = (static_cast<float>(i) / static_cast<float>(steps)) * h;
                    const bedrocktools::sdk::Vec3 o = {side.x * f, side.y * f, side.z * f};
                    bundle.push_back({
                        {a.x + o.x, a.y + o.y, a.z + o.z},
                        {b.x + o.x, b.y + o.y, b.z + o.z}
                    });
                }
            }
            drawLines(bundle, color);
        } else {
            drawLines(lines, color);
        }
    };

    bedrocktools::sdk::Vec3 localPos = g_playerPos;
    float dx = camX - localPos.x;
    float dy = camY - (localPos.y + 1.62f);
    float dz = camZ - localPos.z;
    bool isThirdPerson = (dx*dx + dy*dy + dz*dz) > 0.05f;

    // Build the same pick ray the game uses for melee / block targeting.
    // The previous indicator reconstructed the ray from ActorRotationComponent,
    // which on Bedrock (especially mobile) often lags or disagrees with the
    // camera, so the box never turned red even when the crosshair was on a
    // mob. Level::getHitResult() is the live pick Block Outline already uses.
    void* selectedEntity = nullptr;
    PickRay pick;
    uintptr_t levelPtr = *(uintptr_t*)((uintptr_t)g_localPlayerPtr + bedrocktools::sdk::offsets::Actor::mLevel);
    void* hitResult = resolveHitResult(levelPtr ? reinterpret_cast<void*>(levelPtr) : nullptr);
    if (hitResult) {
        pick = pickRayFromHitResult(hitResult);
        // Reject a garbage pick (wrong wrapper offset) whose origin is
        // nowhere near the camera or the player's eyes.
        if (pick.valid) {
            const float dCam = (pick.ox - camX) * (pick.ox - camX) +
                               (pick.oy - camY) * (pick.oy - camY) +
                               (pick.oz - camZ) * (pick.oz - camZ);
            const float eyeY = localPos.y + 1.62f;
            const float dEye = (pick.ox - localPos.x) * (pick.ox - localPos.x) +
                               (pick.oy - eyeY) * (pick.oy - eyeY) +
                               (pick.oz - localPos.z) * (pick.oz - localPos.z);
            if (dCam > 64.0f && dEye > 64.0f) pick.valid = false;
        }
        const int hitType = *reinterpret_cast<int*>(
            reinterpret_cast<uintptr_t>(hitResult) +
            bedrocktools::sdk::offsets::HitResult::mType);
        if (s_hitResultGetEntity &&
            (hitType == bedrocktools::sdk::offsets::HitResult::TypeEntity ||
             hitType == bedrocktools::sdk::offsets::HitResult::TypeEntityOutOfRange)) {
            selectedEntity = s_hitResultGetEntity(hitResult);
        }
    }
    if (!pick.valid) {
        pick = pickRayFromActor(g_localPlayerPtr, getActorAABB(g_localPlayerPtr),
                                camX, camY, camZ, isThirdPerson, localPos);
    }

    // Keep selection_box / ui_fill_color so the overlay still draws. Those
    // materials ignore the depth buffer, so occlusion is done in software:
    // skip entities that have no line of sight from the camera. That hides
    // player/mob hitboxes behind walls without turning the module off.
    void* region = getLocalBlockSource();

    auto renderActor = [&](void* ent) {
        AABB aabb = getActorAABB(ent);
        if (aabb.min.x == 0.f && aabb.min.y == 0.f && aabb.min.z == 0.f &&
            aabb.max.x == 0.f && aabb.max.y == 0.f && aabb.max.z == 0.f) return;

        if (!aabbVisibleFromCamera(region, aabb, camX, camY, camZ)) {
            return;
        }

        // Hitbox Indicator: recolor the box when this entity is the one
        // the local player can currently melee. The game's own pick
        // (HitResult entity + pick ray) is the source of truth — that is
        // the same ray SurvivalMode uses for attack. Distance to the AABB
        // is only a range gate so an EntityOutOfRange pick does not light
        // up a mob 10 blocks away. Standing above / beside an entity
        // without aiming at it does not trigger.
        uint32_t boxColor = g_hitboxMod->hitboxColor;
        if (g_hitboxMod->hitboxIndicator && ent != g_localPlayerPtr) {
            const float range = kHitboxIndicatorRange;
            const bool closeEnough =
                distanceToAABB(pick.ox, pick.oy, pick.oz, aabb) <= range;
            const bool lookingAt =
                (selectedEntity != nullptr && ent == selectedEntity) ||
                rayHitsAABB(pick, aabb, range);
            if (closeEnough && lookingAt) {
                boxColor = g_hitboxMod->hitboxIndicatorColor;
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
            const auto dir = lookDirFromRotation(getActorRotation(ent));
            const float dirX = dir.x;
            const float dirY = dir.y;
            const float dirZ = dir.z;

            float entityHeight = aabb.max.y - aabb.min.y;
            float eyeHeight = aabb.min.y + entityHeight * 0.85f;
            float centerX = (aabb.min.x + aabb.max.x) * 0.5f;
            float centerZ = (aabb.min.z + aabb.max.z) * 0.5f;

            bedrocktools::sdk::Vec3 start = {centerX, eyeHeight, centerZ};
            bedrocktools::sdk::Vec3 end = {start.x + dirX * kLookLineLength, start.y + dirY * kLookLineLength, start.z + dirZ * kLookLineLength};

            std::vector<std::pair<bedrocktools::sdk::Vec3, bedrocktools::sdk::Vec3>> lookLines;
            lookLines.push_back({start, end});
            drawLines(lookLines, kLookLineColor);
        }
    };

    // Crosshair Indicator: runs on its own, independent of the hitbox
    // Players/Entities toggles. It evaluates every melee-able target (player
    // or mob) even when the matching hitbox category is hidden, so turning
    // off "Players" or "Entities" no longer also disables the red crosshair.
    auto evaluateCrosshair = [&](void* ent) {
        AABB aabb = getActorAABB(ent);
        if (aabb.min.x == 0.f && aabb.min.y == 0.f && aabb.min.z == 0.f &&
            aabb.max.x == 0.f && aabb.max.y == 0.f && aabb.max.z == 0.f) return;

        if (!aabbVisibleFromCamera(region, aabb, camX, camY, camZ)) return;

        const float range = kHitboxIndicatorRange;
        const bool closeEnough =
            distanceToAABB(pick.ox, pick.oy, pick.oz, aabb) <= range;
        const bool lookingAt =
            (selectedEntity != nullptr && ent == selectedEntity) ||
            rayHitsAABB(pick, aabb, range);
        if (closeEnough && lookingAt) {
            g_crosshairCanHit.store(true, std::memory_order_relaxed);
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
                const bool isMob = !isPlayer && hasCategory(ent, 2);

                if (s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

                if (g_hitboxMod->crosshairIndicator && (isPlayer || isMob)) {
                    evaluateCrosshair(ent);
                }

                if (isPlayer && !g_hitboxMod->showPlayers) continue;
                if (isMob && !g_hitboxMod->showEntities) continue;
                if (!isPlayer && !isMob) continue;

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
    // The Crosshair Indicator is pinned to the centre of the screen, so there
    // is nothing for the user to drag in the HUD editor.
    hideInHudEditor = true;

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

    uintptr_t hrge = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::HitResultGetEntity);
    if (hrge) s_hitResultGetEntity = (HitResult_getEntity_t)hrge;

    uintptr_t ghr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelGetHitResult);
    if (ghr) s_getHitResult = (Level_getHitResult_t)ghr;

    uintptr_t aip = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer);
    if (aip) s_actorIsPlayer = (Actor_isPlayer_t)aip;

    uintptr_t aii = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible);
    if (aii) s_actorIsInvisible = (Actor_isInvisible_t)aii;

    uintptr_t afn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted);
    if (afn) s_actorFetchNearby = (Actor_fetchNearbyActorsSorted_t)afn;

    uintptr_t isb = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock);
    if (isb) s_isSolidBlockingBlock = (BlockSource_isSolidBlockingBlock_t)isb;

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
    g_crosshairCanHit.store(false, std::memory_order_relaxed);
    // Drop any crosshair geometry still queued for this module so the vanilla
    // cursor is the only thing left on screen.
    submitDrawCommands(moduleId, {});
}

// Draws the replacement crosshair. The vanilla cursor is suppressed by the
// HudCursor hook (see shouldHideVanillaCrosshair), so exactly one crosshair is
// visible at any time: white when out of reach, red when a mob / player is
// aimed at within melee range.
void HitboxModule::onFrame() {
    std::vector<PLModMenu_DrawCommand> cmds;

    if (crosshairIndicator && g_crosshairCanHit.load(std::memory_order_relaxed)) {
        // -20000 is the launcher's screen-centre sentinel (same convention the
        // Debug Menu crosshair and the Tablist use).
        const float cx = -20000.f;
        const float cy = -20000.f;
        const float half = std::max(crosshairIndicatorSize, 1.0f);
        const float thick = std::max(crosshairIndicatorThickness, 1.0f);

        auto addLine = [&](float dx, float dy, float size, uint32_t color) {
            PLModMenu_DrawCommand line = {};
            line.type = PL_DRAW_LINE;
            line.x = cx - dx * 0.5f;
            line.y = cy - dy * 0.5f;
            line.w = dx;
            line.h = dy;
            line.size = size;
            line.color = color;
            cmds.push_back(line);
        };

        // Thin dark outline first so the red stays readable on bright blocks.
        const uint32_t outline = 0xA0000000u;
        addLine(half * 2.f, 0.f, thick + 2.f, outline);
        addLine(0.f, half * 2.f, thick + 2.f, outline);

        addLine(half * 2.f, 0.f, thick, crosshairIndicatorColor);
        addLine(0.f, half * 2.f, thick, crosshairIndicatorColor);
    }

    // Submitted unconditionally: an empty list clears last frame's crosshair
    // as soon as the target leaves reach.
    submitDrawCommands(moduleId, cmds);
}

void HitboxModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    showEntities = j.value("showEntities", showEntities);
    showPlayers = j.value("showPlayers", showPlayers);
    showSelf = j.value("showSelf", showSelf);
    showEyeLine = j.value("showEyeLine", showEyeLine);
    showLookLine = j.value("showLookLine", showLookLine);
    hitboxIndicator = j.value("hitboxIndicator", hitboxIndicator);
    crosshairIndicator = j.value("crosshairIndicator", crosshairIndicator);
    if (j.contains("crosshairIndicatorSize"))
        crosshairIndicatorSize = j["crosshairIndicatorSize"].get<float>();
    if (j.contains("crosshairIndicatorThickness"))
        crosshairIndicatorThickness = j["crosshairIndicatorThickness"].get<float>();

    if (j.contains("thickness")) thickness = j["thickness"].get<float>();
    else if (j.contains("lineWidth")) thickness = j["lineWidth"].get<float>();
    else if (j.contains("m_thickness")) thickness = j["m_thickness"].get<float>();

    auto parseColor = [&](const std::string& key, uint32_t& outColor) {
        if (j.contains(key)) {
            std::string hexStr = j[key].get<std::string>();
            if (!hexStr.empty() && hexStr[0] == '#') {
                try {
                    std::string hex = hexStr.substr(1);
                    uint32_t parsed = std::stoul(hex, nullptr, 16);
                    // Menu color pickers often emit #RRGGBB (no alpha). Treat
                    // missing / zero alpha as fully opaque so white stays white.
                    if (hex.size() <= 6 || (parsed & 0xFF000000u) == 0) {
                        parsed |= 0xFF000000u;
                    }
                    outColor = parsed;
                } catch (...) {}
            }
        }
    };

    parseColor("hitboxColor", hitboxColor);
    parseColor("eyeLineColor", eyeLineColor);
    parseColor("hitboxIndicatorColor", hitboxIndicatorColor);
    parseColor("crosshairIndicatorColor", crosshairIndicatorColor);

    if (!crosshairIndicator || !enabled) {
        g_crosshairCanHit.store(false, std::memory_order_relaxed);
    }
}

void HitboxModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["showEntities"] = showEntities;
    j["showPlayers"] = showPlayers;
    j["showSelf"] = showSelf;
    j["showEyeLine"] = showEyeLine;
    j["showLookLine"] = showLookLine;
    j["hitboxIndicator"] = hitboxIndicator;
    j["crosshairIndicator"] = crosshairIndicator;
    j["crosshairIndicatorSize"] = crosshairIndicatorSize;
    j["crosshairIndicatorThickness"] = crosshairIndicatorThickness;
    j["thickness"] = thickness;

    char hexH[12], hexE[12], hexI[12], hexC[12];
    snprintf(hexH, sizeof(hexH), "#%08X", hitboxColor);
    snprintf(hexE, sizeof(hexE), "#%08X", eyeLineColor);
    snprintf(hexI, sizeof(hexI), "#%08X", hitboxIndicatorColor);
    snprintf(hexC, sizeof(hexC), "#%08X", crosshairIndicatorColor);

    j["hitboxColor"] = std::string(hexH);
    j["eyeLineColor"] = std::string(hexE);
    j["hitboxIndicatorColor"] = std::string(hexI);
    j["crosshairIndicatorColor"] = std::string(hexC);
}
