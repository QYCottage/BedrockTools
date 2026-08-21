#include "itemlabels.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Item Labels
//
// Shows a holographic 3D label (item name + stack count) above every dropped
// item using the game's own name-tag renderer — the same client-side
// synced-data mechanism the TNT Timer module uses. Everything happens on the
// client's local copy of the entity, so nothing is sent to the server, no
// player.json is touched, and the module stays compatible with other mods
// and with multiplayer.
//
// Version-sensitive memory layout is concentrated in a few documented spots:
//   * ItemActor::mItem           (offsets/World.hpp)
//   * ItemStackBase::mCustomName / mCount (this file, runtime-validated)
//   * Item description-id strings (this file, runtime-scanned + cached)
// Every read is validated before it is used, and a small scan is used as a
// fallback, so an outdated constant degrades to "no label" instead of a
// crash.
// ---------------------------------------------------------------------------

namespace {

using ActorGetNameTagFn = std::string (*)(void*);
using ActorSetNameTagFn = void (*)(void*, const std::string&);
using SynchedActorDataEnsureIndexFn = void (*)(void*, std::uint16_t);
using ActorSynchedDataUpdateAlwaysShowNameTagFn = void (*)(void*, const void*);

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

using ActorFetchNearbyActorsSortedFn = ActorVec (*)(void*, void*, int);
using BlockSourceIsSolidBlockingBlockFn = bool (*)(void*, const bedrocktools::sdk::BlockPos&);

ActorGetNameTagFn g_getNameTag = nullptr;
ActorSetNameTagFn g_setNameTag = nullptr;
SynchedActorDataEnsureIndexFn g_ensureIndex = nullptr;
ActorSynchedDataUpdateAlwaysShowNameTagFn g_updateAlwaysShowNameTag = nullptr;
ActorFetchNearbyActorsSortedFn g_fetchNearby = nullptr;
BlockSourceIsSolidBlockingBlockFn g_isSolidBlockingBlock = nullptr;

ItemLabelsModule* g_mod = nullptr;

// ActorCategory::Item — dropped item entities carry this category bit.
constexpr std::uint32_t kItemCategoryBit = 0x40;

// Sanity bounds for pointers read from the game.
constexpr std::uintptr_t kMinPtr = 0x10000;
constexpr std::uintptr_t kMaxPtr = 0x10000000000ULL;

struct CapturedState {
    std::string name;
    bool hadAlwaysShow = false;
    std::int8_t alwaysShowValue = 0;
    int unseenTicks = 0;
};

// Actors we currently show a label on, keyed by actor address. The menu
// toggle runs on the launcher thread while the tick callback runs on the
// game thread, so access is serialized with g_mutex.
std::unordered_map<void*, CapturedState> g_captured;
std::mutex g_mutex;
// Vtable of ItemActor — used to make sure an actor pointer is still a live
// item entity before restoring anything on it.
void* g_itemActorVtable = nullptr;
// Item* -> description id cache (Item objects are registry singletons, so
// the id only needs to be resolved once per item type).
std::unordered_map<void*, std::string> g_nameCache;
// Current level pointer, so a world switch clears all captured state.
std::uintptr_t g_level = 0;

bool isReadablePtr(std::uintptr_t p) {
    return p >= kMinPtr && p < kMaxPtr;
}

// ---------------------------------------------------------------------------
// Layout-agnostic std::string reader.
//
// The game's std::string is 32 bytes on this platform with the data pointer
// at +0; the size is either at +8 or +16 depending on the STL flavor the
// build shipped with. Both layouts are probed and the first sane reading is
// used, so this keeps working if a future build changes the internal order.
// ---------------------------------------------------------------------------
bool readGameString(const void* object, std::string& out) {
    if (!object) return false;
    const auto base = reinterpret_cast<const std::uint8_t*>(object);
    const auto dataPtr = *reinterpret_cast<const std::uintptr_t*>(base + 0x0);

    for (std::size_t sizeOff : {std::size_t{8}, std::size_t{16}}) {
        const auto size = *reinterpret_cast<const std::uintptr_t*>(base + sizeOff);
        if (size == 0 || size > 128) continue;

        const auto objStart = reinterpret_cast<std::uintptr_t>(object);
        const bool inlineData = dataPtr >= objStart && dataPtr < objStart + 32;
        if (!inlineData && !isReadablePtr(dataPtr)) continue;

        const char* src = reinterpret_cast<const char*>(dataPtr);
        out.assign(src, size);
        // Sanity: reject strings full of non-printable garbage.
        std::size_t printable = 0;
        for (char c : out) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 0x20 && u != 0x7F) ++printable;
        }
        if (printable * 2 < out.size()) {
            out.clear();
            continue;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ItemStackBase access (mirrors ShulkerPreview's helpers).
// ---------------------------------------------------------------------------
struct ItemStackBase {};
struct Item {};

Item* getStackItem(ItemStackBase* stack) {
    if (!stack) return nullptr;
    void* counter = *reinterpret_cast<void**>(
        reinterpret_cast<std::uint8_t*>(stack) +
        bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseItem);
    if (!isReadablePtr(reinterpret_cast<std::uintptr_t>(counter))) return nullptr;
    Item* item = *reinterpret_cast<Item**>(counter);
    if (!isReadablePtr(reinterpret_cast<std::uintptr_t>(item))) return nullptr;
    // The Item object must carry a valid vtable.
    if (!isReadablePtr(*reinterpret_cast<std::uintptr_t*>(item))) return nullptr;
    return item;
}

// The stack's custom (anvil-renamed) display name, if any.
bool readCustomName(ItemStackBase* stack, std::string& out) {
    if (!stack) return false;
    const auto* customName = reinterpret_cast<const std::uint8_t*>(stack) +
                             bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseCustomName;
    return readGameString(customName, out);
}

// ---------------------------------------------------------------------------
// Item display-name resolution.
// ---------------------------------------------------------------------------
bool isPlausibleIdChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == ':' || c == '.' || c == '-';
}

bool looksLikeItemId(const std::string& s) {
    if (s.size() < 2 || s.size() > 48) return false;
    for (char c : s) {
        if (!isPlausibleIdChar(c)) return false;
    }
    return true;
}

// Item keeps several string members in a row: mDescriptionId ("diamond"),
// mRawNameId ("diamond") and mNamespace ("minecraft") followed by the full
// name. Scanning for three consecutive valid strings with that shape is a
// distinctive signature that survives layout shifts between builds. The
// result is cached per Item* because Item objects are registry singletons.
bool findItemDescriptionId(Item* item, std::string& out) {
    if (!item) return false;

    const auto it = g_nameCache.find(item);
    if (it != g_nameCache.end()) {
        out = it->second;
        return !out.empty();
    }

    const auto base = reinterpret_cast<std::uintptr_t>(item);
    std::string found;
    // Probe both a 24-byte and a 32-byte string stride: which one applies
    // depends on the STL flavor of the build.
    for (std::size_t stride : {std::size_t{32}, std::size_t{24}}) {
        for (std::size_t off = 0x60; off + stride * 2 + 32 < 0x300; off += 8) {
            std::string s0, s1, s2;
            if (!readGameString(reinterpret_cast<void*>(base + off), s0)) continue;
            if (!readGameString(reinterpret_cast<void*>(base + off + stride), s1)) continue;
            if (!readGameString(reinterpret_cast<void*>(base + off + stride * 2), s2)) continue;
            if (!looksLikeItemId(s0) || !looksLikeItemId(s1) || !looksLikeItemId(s2)) continue;
            if (s0 != s1 && s2 != "minecraft") continue;
            // Reject the texture-atlas path which also sits near the start.
            if (s0.find('/') != std::string::npos) continue;
            found = s0;
            break;
        }
        if (!found.empty()) break;
    }

    g_nameCache[item] = found;
    out = found;
    return !out.empty();
}

// "iron_ingot" -> "Iron Ingot", "minecraft:diamond" -> "Diamond".
std::string prettifyItemId(const std::string& id) {
    std::string name = id;
    const std::string prefix = "minecraft:";
    if (name.compare(0, prefix.size(), prefix) == 0) name.erase(0, prefix.size());

    std::string out;
    bool capNext = true;
    for (char c : name) {
        if (c == '_' || c == '.' || c == '-') {
            capNext = true;
            out += ' ';
        } else {
            out += capNext ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            capNext = false;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Stack count extraction.
//
// The count byte lives near the start of ItemStackBase. Its exact offset has
// shifted between builds (0x3A on the 1.20.60-1.21.0x layouts, 0x28 on newer
// component-based layouts), so a small scored scan over the known candidates
// is used instead of trusting a single constant.
// ---------------------------------------------------------------------------
const std::size_t kCountCandidates[] = {
    0x3A, 0x39, 0x38, 0x28, 0x2A, 0x30, 0x32, 0x58, 0x5A, 0x59, 0x60, 0x62};

int readStackCount(ItemStackBase* stack) {
    if (!stack) return 1;
    const auto base = reinterpret_cast<const std::uint8_t*>(stack);

    int bestOffset = -1;
    int bestScore = -1;
    for (std::size_t off : kCountCandidates) {
        const int value = base[off];
        if (value < 1 || value > 99) continue;

        int score = 1;
        // A following bool flag and a preceding aux-value byte make a
        // candidate much more likely to be the real count field.
        if (base[off + 1] <= 1) score += 2;
        if (base[off - 1] <= 15) score += 1;
        if (base[off + 2] <= 1) score += 1;
        if (score > bestScore) {
            bestScore = score;
            bestOffset = static_cast<int>(off);
        }
    }
    if (bestOffset < 0) return 1;
    return base[bestOffset];
}

// ---------------------------------------------------------------------------
// Synched-actor-data always-show-name machinery (same approach as TNT Timer).
// ---------------------------------------------------------------------------
void* getEntityDataWrapper(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(actor) +
                                   bedrocktools::sdk::offsets::Actor::mEntityData);
}

void* getEntityContext(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(actor) +
                                   bedrocktools::sdk::offsets::Actor::mEntityContext);
}

void* getDataComponent(void* actor) {
    void* wrapper = getEntityDataWrapper(actor);
    if (!wrapper) return nullptr;
    return *reinterpret_cast<void**>(wrapper);
}

void** getItemsBegin(void* component) {
    if (!component) return nullptr;
    return *reinterpret_cast<void***>(component);
}

std::size_t getItemsSize(void* component) {
    if (!component) return 0;
    auto** begin = *reinterpret_cast<void***>(component);
    auto** end = *reinterpret_cast<void***>(reinterpret_cast<std::uintptr_t>(component) +
                                            sizeof(void*));
    if (!begin || !end || end < begin) return 0;
    return static_cast<std::size_t>(end - begin);
}

void markDataItemPresentAndDirty(void* component, std::size_t id) {
    if (!component || id >= 192) return;

    auto* dirty = reinterpret_cast<std::uint64_t*>(
        reinterpret_cast<std::uintptr_t>(component) + 0x18);
    auto* present = reinterpret_cast<std::uint64_t*>(
        reinterpret_cast<std::uintptr_t>(component) + 0x30);

    const std::size_t word = id / 64;
    const std::uint64_t bit = std::uint64_t{1} << (id % 64);
    dirty[word] |= bit;
    present[word] |= bit;
}

void* findSCharDataItemVtable(void* component) {
    auto** begin = getItemsBegin(component);
    const std::size_t size = getItemsSize(component);
    if (!begin) return nullptr;

    for (std::size_t i = 0; i < size; ++i) {
        void* item = begin[i];
        if (!item) continue;

        const auto address = reinterpret_cast<std::uintptr_t>(item);
        const auto type = *reinterpret_cast<const std::uint8_t*>(
            address + bedrocktools::sdk::offsets::DataItem::mType);
        if (type == 0) return *reinterpret_cast<void**>(item);
    }

    return nullptr;
}

bool readAlwaysShowItem(void* actor, std::int8_t& value) {
    void* component = getDataComponent(actor);
    if (!component) return false;

    constexpr std::size_t id = bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow;
    if (getItemsSize(component) <= id) return false;

    auto** begin = getItemsBegin(component);
    if (!begin) return false;

    void* item = begin[id];
    if (!item) return false;

    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mType);
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mId);
    if (type != 0 || itemId != id) return false;

    value = *reinterpret_cast<const std::int8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mValue);
    return true;
}

bool writeAlwaysShowItem(void* actor, std::int8_t value) {
    if (!actor || !g_ensureIndex || !g_updateAlwaysShowNameTag) return false;

    void* wrapper = getEntityDataWrapper(actor);
    void* context = getEntityContext(actor);
    void* component = getDataComponent(actor);
    if (!wrapper || !context || !component) return false;

    constexpr std::uint16_t id = static_cast<std::uint16_t>(
        bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow);

    g_ensureIndex(component, id);

    auto** begin = getItemsBegin(component);
    if (!begin || getItemsSize(component) <= id) return false;

    void* item = begin[id];
    if (!item) {
        void* vtable = findSCharDataItemVtable(component);
        if (!vtable) return false;

        item = ::operator new(16);
        std::memset(item, 0, 16);
        *reinterpret_cast<void**>(item) = vtable;
        *reinterpret_cast<std::uint8_t*>(
            reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mType) = 0;
        *reinterpret_cast<std::uint16_t*>(
            reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mId) = id;
        begin[id] = item;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mType);
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mId);
    if (type != 0 || itemId != id) return false;

    *reinterpret_cast<std::int8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mValue) = value;

    markDataItemPresentAndDirty(component, id);
    g_updateAlwaysShowNameTag(context, wrapper);
    return true;
}

// ---------------------------------------------------------------------------
// Actor helpers.
// ---------------------------------------------------------------------------
bool isLiveItemActor(void* actor) {
    if (!isReadablePtr(reinterpret_cast<std::uintptr_t>(actor))) return false;
    void* vtable = *reinterpret_cast<void**>(actor);
    if (!g_itemActorVtable) {
        // First contact: trust the category bit and validate the stack.
        if (!isReadablePtr(reinterpret_cast<std::uintptr_t>(vtable))) return false;
        const auto categories = *reinterpret_cast<const std::uint32_t*>(
            reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mCategories);
        if ((categories & kItemCategoryBit) == 0) return false;
        g_itemActorVtable = vtable;
        return true;
    }
    return vtable == g_itemActorVtable;
}

void* getActorStateVector(void* actor) {
    if (!actor) return nullptr;
    return *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mStateVectorComponent);
}

bedrocktools::sdk::Vec3 getActorPosition(void* actor) {
    bedrocktools::sdk::Vec3 pos{0.f, 0.f, 0.f};
    void* svc = getActorStateVector(actor);
    if (svc) pos = *reinterpret_cast<bedrocktools::sdk::Vec3*>(svc);
    return pos;
}

bedrocktools::sdk::Vec2 getActorRotation(void* actor) {
    bedrocktools::sdk::Vec2 rot{0.f, 0.f};
    void* rotComp = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mActorRotationComponent);
    if (rotComp) rot = *reinterpret_cast<bedrocktools::sdk::Vec2*>(rotComp);
    return rot;
}

bedrocktools::sdk::AABB getActorAABB(void* actor) {
    bedrocktools::sdk::AABB aabb{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    void* svc = getActorStateVector(actor);
    if (!svc) return aabb;
    void* aabbComp = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(svc) +
        bedrocktools::sdk::offsets::BuiltInActorComponents::mAABBShapeComponent);
    if (!aabbComp) return aabb;
    aabb = *reinterpret_cast<bedrocktools::sdk::AABB*>(
        reinterpret_cast<std::uintptr_t>(aabbComp) +
        bedrocktools::sdk::offsets::AABBShapeComponent::mAABB);
    return aabb;
}

// ---------------------------------------------------------------------------
// Item label data.
// ---------------------------------------------------------------------------
struct ItemLabelInfo {
    void* actor = nullptr;
    float distance = 0.f;
    std::string name;
    int count = 1;
};

// Extracts name + count from an item actor. Returns false when the actor is
// not (or no longer reads as) a dropped item.
bool readItemLabelInfo(void* actor, ItemLabelInfo& out) {
    if (!actor || !isLiveItemActor(actor)) return false;

    ItemStackBase* stack = reinterpret_cast<ItemStackBase*>(
        reinterpret_cast<std::uint8_t*>(actor) + bedrocktools::sdk::offsets::ItemActor::mItem);
    if (!stack) return false;

    // The stack must look real: a readable vtable and a valid item behind
    // the WeakPtr.
    if (!isReadablePtr(*reinterpret_cast<std::uintptr_t*>(stack))) return false;
    Item* item = getStackItem(stack);
    if (!item) return false;

    // Optional extra sanity: the item id (used by ShulkerPreview) should be
    // in a plausible range.
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(
        reinterpret_cast<std::uint8_t*>(item) +
        bedrocktools::sdk::offsets::ShulkerPreview::ItemId);
    if (itemId == 0 || itemId > 2000) return false;

    out.actor = actor;
    out.count = readStackCount(stack);

    std::string custom;
    if (readCustomName(stack, custom) && !custom.empty()) {
        out.name = custom;
    } else {
        std::string id;
        if (!findItemDescriptionId(item, id) || id.empty()) return false;
        out.name = prettifyItemId(id);
    }
    return !out.name.empty();
}

// ---------------------------------------------------------------------------
// Ray casting (mirrors the Hitbox module's helpers).
// ---------------------------------------------------------------------------
bool rayHitsAABB(float ox, float oy, float oz,
                 float dx, float dy, float dz,
                 const bedrocktools::sdk::AABB& aabb,
                 float maxDist,
                 float& outDist) {
    float tmin = 0.0f;
    float tmax = maxDist;

    auto slab = [&](float origin, float dir, float mn, float mx) -> bool {
        if (std::fabs(dir) < 1e-8f) {
            return origin >= mn && origin <= mx;
        }
        float inv = 1.0f / dir;
        float t1 = (mn - origin) * inv;
        float t2 = (mx - origin) * inv;
        if (t1 > t2) {
            float tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > tmin) tmin = t1;
        if (t2 < tmax) tmax = t2;
        return tmin <= tmax;
    };

    if (!slab(ox, dx, aabb.min.x, aabb.max.x)) return false;
    if (!slab(oy, dy, aabb.min.y, aabb.max.y)) return false;
    if (!slab(oz, dz, aabb.min.z, aabb.max.z)) return false;
    if (tmax < 0.0f) return false;
    outDist = tmin > 0.0f ? tmin : 0.0f;
    return true;
}

// Amanatides & Woo voxel traversal (same as the Hitbox module): walks every
// voxel the segment camera -> target passes through and returns true as soon
// as a solid blocking block is found. The camera's own voxel is never
// tested, so the check keeps working when the camera clips into geometry.
bool rayHitsSolid(void* region,
                  float ox, float oy, float oz,
                  float tx, float ty, float tz) {
    if (!region || !g_isSolidBlockingBlock) return false;

    const float dx = tx - ox;
    const float dy = ty - oy;
    const float dz = tz - oz;
    const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (dist < 0.01f) return false;

    int x = static_cast<int>(std::floor(ox));
    int y = static_cast<int>(std::floor(oy));
    int z = static_cast<int>(std::floor(oz));
    const int ex = static_cast<int>(std::floor(tx));
    const int ey = static_cast<int>(std::floor(ty));
    const int ez = static_cast<int>(std::floor(tz));
    if (x == ex && y == ey && z == ez) return false;

    int stepX, stepY, stepZ;
    float tMaxX, tMaxY, tMaxZ;
    float tDeltaX, tDeltaY, tDeltaZ;
    constexpr float kInf = 1e30f;

    if (dx > 0.0f)      { stepX = 1;  tDeltaX = 1.0f / dx;      tMaxX = (x + 1 - ox) * tDeltaX; }
    else if (dx < 0.0f) { stepX = -1; tDeltaX = -1.0f / dx;     tMaxX = (ox - x) * tDeltaX; }
    else                { stepX = 0;  tDeltaX = kInf;           tMaxX = kInf; }

    if (dy > 0.0f)      { stepY = 1;  tDeltaY = 1.0f / dy;      tMaxY = (y + 1 - oy) * tDeltaY; }
    else if (dy < 0.0f) { stepY = -1; tDeltaY = -1.0f / dy;     tMaxY = (oy - y) * tDeltaY; }
    else                { stepY = 0;  tDeltaY = kInf;           tMaxY = kInf; }

    if (dz > 0.0f)      { stepZ = 1;  tDeltaZ = 1.0f / dz;      tMaxZ = (z + 1 - oz) * tDeltaZ; }
    else if (dz < 0.0f) { stepZ = -1; tDeltaZ = -1.0f / dz;     tMaxZ = (oz - z) * tDeltaZ; }
    else                { stepZ = 0;  tDeltaZ = kInf;           tMaxZ = kInf; }

    while (true) {
        if (tMaxX < tMaxY && tMaxX < tMaxZ) {
            x += stepX;
            if (tMaxX > dist) break;
            tMaxX += tDeltaX;
        } else if (tMaxY < tMaxZ) {
            y += stepY;
            if (tMaxY > dist) break;
            tMaxY += tDeltaY;
        } else {
            z += stepZ;
            if (tMaxZ > dist) break;
            tMaxZ += tDeltaZ;
        }

        bedrocktools::sdk::BlockPos bp{x, y, z};
        if (g_isSolidBlockingBlock(region, bp)) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Name-tag capture / restore.
// ---------------------------------------------------------------------------
void captureItem(void* actor) {
    if (g_captured.contains(actor)) return;

    CapturedState state;
    if (g_getNameTag) state.name = g_getNameTag(actor);
    state.hadAlwaysShow = readAlwaysShowItem(actor, state.alwaysShowValue);
    g_captured.emplace(actor, std::move(state));
}

// Restores an actor's original name tag state. Does not erase the map entry;
// callers own the erasure. Actors that no longer read as live item entities
// are left untouched (the memory may have been reused after pick-up).
void restoreItem(void* actor) {
    const auto it = g_captured.find(actor);
    if (it == g_captured.end()) return;

    if (isLiveItemActor(actor)) {
        if (g_setNameTag) g_setNameTag(actor, it->second.name);
        writeAlwaysShowItem(actor, it->second.hadAlwaysShow ? it->second.alwaysShowValue : 0);
    }
}

void restoreAllItems() {
    for (auto it = g_captured.begin(); it != g_captured.end();) {
        void* actor = it->first;
        if (isLiveItemActor(actor)) {
            if (g_setNameTag) g_setNameTag(actor, it->second.name);
            writeAlwaysShowItem(actor, it->second.hadAlwaysShow ? it->second.alwaysShowValue : 0);
        }
        it = g_captured.erase(it);
    }
    g_itemActorVtable = nullptr;
}

// ---------------------------------------------------------------------------
// Color mapping (#RRGGBB -> nearest vanilla name-tag color code).
// ---------------------------------------------------------------------------
struct VanillaColor {
    const char code;
    const std::uint8_t r, g, b;
};

const VanillaColor kVanillaColors[] = {
    {'0', 0x00, 0x00, 0x00}, {'1', 0x00, 0x00, 0xAA}, {'2', 0x00, 0xAA, 0x00},
    {'3', 0x00, 0xAA, 0xAA}, {'4', 0xAA, 0x00, 0x00}, {'5', 0xAA, 0x00, 0xAA},
    {'6', 0xFF, 0xAA, 0x00}, {'7', 0xAA, 0xAA, 0xAA}, {'8', 0x55, 0x55, 0x55},
    {'9', 0x55, 0x55, 0xFF}, {'a', 0x55, 0xFF, 0x55}, {'b', 0x55, 0xFF, 0xFF},
    {'c', 0xFF, 0x55, 0x55}, {'d', 0xFF, 0x55, 0xFF}, {'e', 0xFF, 0xFF, 0x55},
    {'f', 0xFF, 0xFF, 0xFF},
};

char colorCodeFromHex(const std::string& hex) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() == 8) h = h.substr(2);  // #AARRGGBB -> RRGGBB
    if (h.size() != 6) return 'f';

    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    const int r = hexVal(h[0]) * 16 + hexVal(h[1]);
    const int g = hexVal(h[2]) * 16 + hexVal(h[3]);
    const int b = hexVal(h[4]) * 16 + hexVal(h[5]);

    char best = 'f';
    long bestDist = 1L << 60;
    for (const auto& c : kVanillaColors) {
        const long dr = r - c.r;
        const long dg = g - c.g;
        const long db = b - c.b;
        const long d = dr * dr + dg * dg + db * db;
        if (d < bestDist) {
            bestDist = d;
            best = c.code;
        }
    }
    return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// ItemLabelsModule
// ---------------------------------------------------------------------------

ItemLabelsModule::ItemLabelsModule()
    : Module("Item Labels", "Shows a holographic 3D label with the name and count above every dropped item. Ray-trace culling hides labels behind blocks; nearest-in-range and crosshair targeting modes are available.") {
    hideInHudEditor = true;  // World overlay, not a HUD element.
    g_mod = this;
}

ItemLabelsModule::~ItemLabelsModule() {
    if (g_mod == this) g_mod = nullptr;
}

void ItemLabelsModule::onInit() {
    if (!g_getNameTag) {
        g_getNameTag = reinterpret_cast<ActorGetNameTagFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    }
    if (!g_setNameTag) {
        g_setNameTag = reinterpret_cast<ActorSetNameTagFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSetNameTag));
    }
    if (!g_ensureIndex) {
        g_ensureIndex = reinterpret_cast<SynchedActorDataEnsureIndexFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SynchedActorDataEnsureIndex));
    }
    if (!g_updateAlwaysShowNameTag) {
        g_updateAlwaysShowNameTag = reinterpret_cast<ActorSynchedDataUpdateAlwaysShowNameTagFn>(
            bedrocktools::memory::resolve(
                bedrocktools::memory::SignatureId::ActorSynchedDataUpdateAlwaysShowNameTag));
    }
    if (!g_fetchNearby) {
        g_fetchNearby = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));
    }
    if (!g_isSolidBlockingBlock) {
        g_isSolidBlockingBlock = reinterpret_cast<BlockSourceIsSolidBlockingBlockFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BlockSourceIsSolidBlockingBlock));
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (!g_mod || !g_mod->enabled || !event.player) return;
            g_mod->updateLabels(event.player);
        });
}

void ItemLabelsModule::onDisable() {
    std::lock_guard<std::mutex> lock(g_mutex);
    restoreAllItems();
}

void ItemLabelsModule::updateLabels(void* player) {
    if (!player) return;
    std::lock_guard<std::mutex> lock(g_mutex);

    // World switch: everything we captured belongs to the old world.
    const std::uintptr_t level = *reinterpret_cast<std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(player) + bedrocktools::sdk::offsets::Actor::mLevel);
    if (level != g_level) {
        g_level = level;
        g_captured.clear();
        g_nameCache.clear();
        g_itemActorVtable = nullptr;
        if (level < kMinPtr) return;  // Not in a world yet.
    }

    const bedrocktools::sdk::Vec3 playerPos = getActorPosition(player);
    const float eyeY = playerPos.y + 1.62f;

    // Block source for occlusion ray casts.
    void* region = nullptr;
    if (g_isSolidBlockingBlock) {
        const std::uintptr_t dimension = *reinterpret_cast<std::uintptr_t*>(
            reinterpret_cast<std::uintptr_t>(player) + bedrocktools::sdk::offsets::Actor::mDimension);
        if (dimension >= kMinPtr) {
            const std::uintptr_t blockSource = *reinterpret_cast<std::uintptr_t*>(
                dimension + bedrocktools::sdk::offsets::Dimension::mBlockSource);
            if (blockSource >= kMinPtr) region = reinterpret_cast<void*>(blockSource);
        }
    }

    const float maxDist = std::max(4.0f, std::min(m_maxDistance, 128.0f));
    const float fetchRadius = std::min(std::max(maxDist + 8.0f, 32.0f), 64.0f);

    // Every actor returned by the fetch (whether it qualified for a label or
    // not) goes into this set, so captured entries can tell "still alive but
    // deselected" apart from "gone from the world".
    std::unordered_set<void*> fetchedThisTick;

    // Gather item actors inside range.
    std::vector<ItemLabelInfo> items;
    if (g_fetchNearby) {
        bedrocktools::sdk::Vec3 extent{fetchRadius, fetchRadius, fetchRadius};
        ActorVec actors = g_fetchNearby(player, &extent, 1);
        if (actors.begin && actors.end) {
            for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
                void* ent = it->mActor;
                if (!ent || ent == player) continue;
                fetchedThisTick.insert(ent);

                ItemLabelInfo info;
                if (!readItemLabelInfo(ent, info)) continue;

                const bedrocktools::sdk::Vec3 pos = getActorPosition(ent);
                const float dx = pos.x - playerPos.x;
                const float dy = pos.y - playerPos.y;
                const float dz = pos.z - playerPos.z;
                info.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (info.distance > maxDist) continue;

                // Ray-trace culling: hide labels behind solid blocks (walls,
                // ground overhangs...). Grass and other non-blocking blocks
                // do not hide labels, matching the "visible items only" rule.
                if (m_occlusion && region) {
                    const float tx = pos.x;
                    const float ty = pos.y + 0.35f;  // Label sits above the item.
                    const float tz = pos.z;
                    if (rayHitsSolid(region, playerPos.x, eyeY, playerPos.z, tx, ty, tz)) {
                        continue;
                    }
                }

                items.push_back(std::move(info));
            }
        }
    }

    // Select which items get labels.
    std::vector<ItemLabelInfo> selected;
    if (m_mode == 0) {
        // Nearest items in range.
        std::sort(items.begin(), items.end(),
                  [](const ItemLabelInfo& a, const ItemLabelInfo& b) {
                      return a.distance < b.distance;
                  });
        const int limit = std::max(1, std::min(m_maxItems, 64));
        for (int i = 0; i < limit && static_cast<std::size_t>(i) < items.size(); ++i) {
            selected.push_back(items[static_cast<std::size_t>(i)]);
        }
    } else {
        // Crosshair target only: ray-cast from the camera through the look
        // direction and pick the closest item whose AABB the ray touches.
        const bedrocktools::sdk::Vec2 rot = getActorRotation(player);
        static constexpr float kPi = 3.14159265f;
        static constexpr float kDegToRad = kPi / 180.0f;
        const float yawR = rot.y * kDegToRad;
        const float pitchR = rot.x * kDegToRad;
        const float lookX = -std::sin(yawR) * std::cos(pitchR);
        const float lookY = -std::sin(pitchR);
        const float lookZ = std::cos(yawR) * std::cos(pitchR);

        const ItemLabelInfo* best = nullptr;
        float bestDist = maxDist;
        for (const auto& info : items) {
            const bedrocktools::sdk::AABB aabb = getActorAABB(info.actor);
            float hitDist = 0.f;
            if (rayHitsAABB(playerPos.x, eyeY, playerPos.z,
                            lookX, lookY, lookZ, aabb, bestDist, hitDist)) {
                bestDist = hitDist;
                best = &info;
            }
        }
        if (best) selected.push_back(*best);
    }

    // Apply labels.
    for (const auto& info : selected) {
        captureItem(info.actor);
        if (g_setNameTag) {
            g_setNameTag(info.actor, buildLabel(info.name, info.count,
                                                m_showCount, m_multiline, m_bold, m_italic,
                                                m_prefix, m_suffix, m_textColor));
        }
        writeAlwaysShowItem(info.actor, 1);
    }

    // Restore items that no longer qualify, and forget actors that have been
    // gone from the nearby list for a while (picked up, despawned, ...).
    const auto isSelected = [&](void* actor) {
        return std::any_of(selected.begin(), selected.end(),
                           [&](const ItemLabelInfo& info) { return info.actor == actor; });
    };
    for (auto it = g_captured.begin(); it != g_captured.end();) {
        const bool selectedNow = isSelected(it->first);
        if (selectedNow) {
            it->second.unseenTicks = 0;
            ++it;
            continue;
        }

        const bool seenThisTick = fetchedThisTick.contains(it->first);
        if (seenThisTick) {
            // Still alive but no longer selected: restore right away.
            restoreItem(it->first);
            it = g_captured.erase(it);
        } else if (++it->second.unseenTicks > 60) {
            // Gone for ~3 seconds: restore if it is still a live item actor,
            // otherwise just drop the entry (the entity no longer exists).
            restoreItem(it->first);
            it = g_captured.erase(it);
        } else {
            ++it;
        }
    }
}

std::string ItemLabelsModule::buildLabel(const std::string& itemName, int count,
                                         bool showCount, bool multiline, bool bold,
                                         bool italic, const std::string& prefix,
                                         const std::string& suffix,
                                         const std::string& textColor) {
    const char colorCode = colorCodeFromHex(textColor);
    std::string color = std::string("\xC2\xA7") + colorCode;

    std::string style;
    if (bold) style += "\xC2\xA7l";
    if (italic) style += "\xC2\xA7o";

    std::string countPart;
    if (showCount && count > 0) {
        // Note: "7x" must stay a separate literal — "\xA77" would be read as
        // one out-of-range hex escape.
        countPart = "\xC2\xA7" "7x" + std::to_string(count);
    }

    std::string label;
    if (multiline) {
        label = prefix + color + style + itemName;
        const std::string second = countPart + suffix;
        if (!second.empty()) label += "\n" + color + second;
    } else {
        label = prefix + color + style + itemName + countPart + suffix;
    }
    return label;
}

void ItemLabelsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    // Radio configs persist as "<index>,<label>..." strings (same convention
    // as the Crosshair module's style picker).
    if (j.contains("m_mode")) {
        const auto& value = j["m_mode"];
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            const auto comma = text.find(',');
            try {
                const int mode = std::stoi(text.substr(0, comma));
                if (mode >= 0 && mode <= 1) m_mode = mode;
            } catch (...) {}
        } else if (value.is_number_integer()) {
            const int mode = value.get<int>();
            if (mode >= 0 && mode <= 1) m_mode = mode;
        }
    }

    m_maxDistance = j.value("m_maxDistance", m_maxDistance);
    m_maxDistance = std::clamp(m_maxDistance, 1.0f, 128.0f);

    m_maxItems = j.value("m_maxItems", m_maxItems);
    m_maxItems = std::clamp(m_maxItems, 1, 64);

    m_occlusion = j.value("m_occlusion", m_occlusion);
    m_showCount = j.value("m_showCount", m_showCount);
    m_multiline = j.value("m_multiline", m_multiline);
    m_bold = j.value("m_bold", m_bold);
    m_italic = j.value("m_italic", m_italic);

    m_prefix = j.value("m_prefix", m_prefix);
    m_suffix = j.value("m_suffix", m_suffix);

    if (j.contains("m_textColor") && j["m_textColor"].is_string()) {
        const std::string hex = j["m_textColor"].get<std::string>();
        if (hex.size() >= 6 && hex[0] == '#') m_textColor = hex;
    }
}

void ItemLabelsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);

    j["m_mode"] = std::to_string(m_mode) + ",Nearest,Crosshair";
    j["m_maxDistance"] = m_maxDistance;
    j["m_maxItems"] = m_maxItems;
    j["m_occlusion"] = m_occlusion;
    j["m_showCount"] = m_showCount;
    j["m_multiline"] = m_multiline;
    j["m_bold"] = m_bold;
    j["m_italic"] = m_italic;
    j["m_prefix"] = m_prefix;
    j["m_suffix"] = m_suffix;
    j["m_textColor"] = m_textColor;
}
