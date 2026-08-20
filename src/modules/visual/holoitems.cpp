#include "holoitems.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>

#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>

// ============================================================================
// Holo Items
//
// Draws a floating "Name xCount" label above every dropped item entity by
// reusing the game's own nametag system (setNameTag + AlwaysShowNameTag),
// exactly like the TNT Timer module does for primed TNT.
//
// All offsets below were reverse-engineered against Minecraft Bedrock 1.26.40
// (libminecraftpe.so, build 1.26.40). They are version-specific: after a game
// update you will need to re-verify them (or re-derive them with your
// libminecraftpe.so + a pattern-search pass).
// ============================================================================

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

HoloItemsModule* g_holoItems = nullptr;

ActorGetNameTagFn g_getNameTag = nullptr;
ActorSetNameTagFn g_setNameTag = nullptr;
SynchedActorDataEnsureIndexFn g_ensureIndex = nullptr;
ActorSynchedDataUpdateAlwaysShowNameTagFn g_updateAlwaysShowNameTag = nullptr;
ActorFetchNearbyActorsSortedFn g_fetchNearby = nullptr;

std::uintptr_t g_libBase = 0;            // runtime load bias of libminecraftpe.so
std::uintptr_t g_itemActorVtable = 0;    // runtime address of ItemActor's vtable

// actor -> original nametag captured before we modified it
std::unordered_map<void*, std::string> g_originalNames;
void* g_lastPlayer = nullptr;  // last local player seen (for restore on disable)

// --------------------------------------------------------------------------
// Version-specific constants (Minecraft Bedrock 1.26.40)
// --------------------------------------------------------------------------
constexpr std::size_t kItemActorItemStack = 0x390;       // ItemActor::mItemStack
constexpr std::size_t kItemStackBaseItem = 0x8;          // ptr to shared control block
constexpr std::size_t kItemStackBaseCount = 0x22;        // ItemStackBase::mCount (uint8)
constexpr std::size_t kItemFullName = 0x188;             // Item::mFullItemName (std::string)
constexpr std::size_t kActorCategories = 0x200;          // Actor::mCategories
constexpr std::uint32_t kActorCategoryItem = 0x200;      // ActorCategory::Item
constexpr std::size_t kItemActorVtableFileOffset = 0x12294e08; // ItemActor vtable, file VA

// File offset of Actor::getNameTag() — used to compute the load bias so the
// vtable address above can be converted to a runtime address.
constexpr std::uintptr_t kActorGetNameTagFileOffset = 0xeca0ae0;

int g_tickCounter = 0;

// --------------------------------------------------------------------------
// ItemStack helpers
// --------------------------------------------------------------------------
void* getItemStack(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(actor) + kItemActorItemStack
    );
}

void* getStackItem(void* stack) {
    if (!stack) return nullptr;
    void* controlBlock = *reinterpret_cast<void**>(
        reinterpret_cast<std::uintptr_t>(stack) + kItemStackBaseItem
    );
    return controlBlock ? *reinterpret_cast<void**>(controlBlock) : nullptr;
}

std::uint8_t getStackCount(void* stack) {
    if (!stack) return 1;
    const auto count = *reinterpret_cast<const std::uint8_t*>(
        reinterpret_cast<std::uintptr_t>(stack) + kItemStackBaseCount
    );
    return count == 0 ? 1 : count;
}

bool isItemActor(void* actor) {
    if (!actor) return false;

    // Primary check: compare the object's vtable against ItemActor's vtable.
    // This is exact for this game build and never depends on enum values.
    if (g_itemActorVtable != 0) {
        const auto vtable = *reinterpret_cast<void* const*>(actor);
        if (vtable == reinterpret_cast<void*>(g_itemActorVtable)) return true;
    }

    // Fallback: category bit check (ActorCategory::Item = 0x200).
    const auto categories = *reinterpret_cast<const std::uint32_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + kActorCategories
    );
    return (categories & kActorCategoryItem) != 0;
}

// --------------------------------------------------------------------------
// Display name formatting.
// Item::mFullItemName holds names like "minecraft:diamond" or
// "minecraft:redstone_block". We strip the namespace, replace '_' with ' ',
// and Title-Case each word so "redstone_block" becomes "Redstone Block".
// --------------------------------------------------------------------------
std::string formatDisplayName(const std::string& raw) {
    const char* s = raw.c_str();
    if (raw.rfind("minecraft:", 0) == 0) {
        s += 10;
    } else if (raw.rfind("item.", 0) == 0) {
        s += 5;
    } else if (raw.rfind("tile.", 0) == 0) {
        s += 5;
    }

    std::string out;
    out.reserve(raw.size());
    bool capitalize = true;
    for (const char* p = s; *p != '\0'; ++p) {
        char c = *p;
        if (c == '_') c = ' ';
        if (capitalize && c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        capitalize = (c == ' ');
        out.push_back(c);
    }

    if (out.empty()) out = "Item";
    return out;
}

std::string buildLabel(void* item, std::uint8_t count) {
    std::string name = "Item";
    if (item) {
        // Item::mFullItemName is a std::string. The mod and the game share the
        // same libc++ ABI, so a plain copy is safe.
        const auto* fullName = reinterpret_cast<const std::string*>(
            reinterpret_cast<std::uintptr_t>(item) + kItemFullName
        );
        name = formatDisplayName(*fullName);
    }

    std::string label;
    if (g_holoItems && g_holoItems->colorEnabled) label += "\xC2\xA7" "e"; // §e
    label += name;
    if (g_holoItems && g_holoItems->showCount) {
        label += "\xC2\xA7" "r" " ";
        label += "\xC2\xA7" "f"; // §f
        label += "x";
        label += std::to_string(static_cast<int>(count));
    }
    return label;
}

// --------------------------------------------------------------------------
// SynchedActorData helpers (force the nametag to always render).
// These mirror the TNT Timer module and are version-specific only through
// the offsets they reference.
// --------------------------------------------------------------------------
void* getEntityDataWrapper(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityData
    );
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
    auto** end = *reinterpret_cast<void***>(
        reinterpret_cast<std::uintptr_t>(component) + sizeof(void*)
    );
    if (!begin || !end || end < begin) return 0;
    return static_cast<std::size_t>(end - begin);
}

void markDataItemPresentAndDirty(void* component, std::size_t id) {
    if (!component || id >= 192) return;

    auto* dirty = reinterpret_cast<std::uint64_t*>(
        reinterpret_cast<std::uintptr_t>(component) + 0x18
    );
    auto* present = reinterpret_cast<std::uint64_t*>(
        reinterpret_cast<std::uintptr_t>(component) + 0x30
    );

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
            address + bedrocktools::sdk::offsets::DataItem::mType
        );
        if (type == 0) return *reinterpret_cast<void**>(item);
    }

    return nullptr;
}

bool writeAlwaysShowItem(void* actor, std::int8_t value) {
    if (!actor || !g_ensureIndex || !g_updateAlwaysShowNameTag) return false;

    void* wrapper = getEntityDataWrapper(actor);
    void* component = getDataComponent(actor);
    if (!wrapper || !component) return false;

    constexpr std::uint16_t id = static_cast<std::uint16_t>(
        bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow
    );

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
            reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mType
        ) = 0;
        *reinterpret_cast<std::uint16_t*>(
            reinterpret_cast<std::uintptr_t>(item) + bedrocktools::sdk::offsets::DataItem::mId
        ) = id;
        begin[id] = item;
    }

    const auto address = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mType
    );
    const auto itemId = *reinterpret_cast<const std::uint16_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mId
    );
    if (type != 0 || itemId != id) return false;

    *reinterpret_cast<std::int8_t*>(
        address + bedrocktools::sdk::offsets::DataItem::mValue
    ) = value;

    markDataItemPresentAndDirty(component, id);
    g_updateAlwaysShowNameTag(
        reinterpret_cast<void*>(
            reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityContext
        ),
        wrapper
    );
    return true;
}

void setItemLabel(void* actor, const std::string& label) {
    if (!actor || !g_setNameTag) return;

    if (!g_originalNames.contains(actor)) {
        g_originalNames[actor] = g_getNameTag ? g_getNameTag(actor) : std::string{};
    }

    g_setNameTag(actor, label);
    writeAlwaysShowItem(actor, 1);
}

void restoreItemLabel(void* actor) {
    if (!actor) return;
    const auto it = g_originalNames.find(actor);
    if (it == g_originalNames.end()) return;
    if (g_setNameTag) g_setNameTag(actor, it->second);
    writeAlwaysShowItem(actor, 0);
    g_originalNames.erase(it);
}

// --------------------------------------------------------------------------
// Per-tick processing
// --------------------------------------------------------------------------
void onLocalPlayerTick(void* player) {
    if (!g_holoItems || !g_holoItems->enabled) return;
    if (!player || !g_fetchNearby || !g_setNameTag) return;
    g_lastPlayer = player;

    if (g_tickCounter < g_holoItems->tickInterval) {
        ++g_tickCounter;
        return;
    }
    g_tickCounter = 0;

    const float r = g_holoItems->radius;
    bedrocktools::sdk::Vec3 extent{r, r, r};

    ActorVec actors = g_fetchNearby(player, &extent, 1);

    std::unordered_set<void*> current;
    if (actors.begin && actors.end) {
        for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
            void* actor = it->mActor;
            if (!actor || !isItemActor(actor)) continue;

            current.insert(actor);

            void* stack = getItemStack(actor);
            void* item = getStackItem(stack);
            const std::uint8_t count = getStackCount(stack);
            setItemLabel(actor, buildLabel(item, count));
        }
    }

    // Prune tracked actors that are no longer nearby. We do NOT dereference
    // them here (they may already be freed), so this is safe.
    for (auto it = g_originalNames.begin(); it != g_originalNames.end();) {
        if (!current.contains(it->first)) {
            it = g_originalNames.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace

// ============================================================================
// Module
// ============================================================================
HoloItemsModule::HoloItemsModule()
    : Module("Holo Items", "Shows a floating name and amount above every dropped item.") {
    g_holoItems = this;
}

HoloItemsModule::~HoloItemsModule() {
    if (g_holoItems == this) g_holoItems = nullptr;
}

void HoloItemsModule::onInit() {
    g_getNameTag = reinterpret_cast<ActorGetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag)
    );
    g_setNameTag = reinterpret_cast<ActorSetNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSetNameTag)
    );
    g_ensureIndex = reinterpret_cast<SynchedActorDataEnsureIndexFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SynchedActorDataEnsureIndex)
    );
    g_updateAlwaysShowNameTag = reinterpret_cast<ActorSynchedDataUpdateAlwaysShowNameTagFn>(
        bedrocktools::memory::resolve(
            bedrocktools::memory::SignatureId::ActorSynchedDataUpdateAlwaysShowNameTag
        )
    );
    g_fetchNearby = reinterpret_cast<ActorFetchNearbyActorsSortedFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted)
    );

    // Compute the load bias from Actor::getNameTag() so we can derive the
    // runtime address of ItemActor's vtable (used for exact item detection).
    const auto getNameTag = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::ActorGetNameTag
    );
    if (getNameTag != 0 && getNameTag > kActorGetNameTagFileOffset) {
        g_libBase = getNameTag - kActorGetNameTagFileOffset;
        g_itemActorVtable = g_libBase + kItemActorVtableFileOffset;
    }

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { onLocalPlayerTick(reinterpret_cast<void*>(event.player)); }
    );
}

void HoloItemsModule::onEnable() {
    // Nothing to patch; labels are applied from the tick callback.
}

void HoloItemsModule::onDisable() {
    // Restore items that are still alive and in range. We only ever touch
    // actors freshly returned by the fetch (guaranteed alive), never stale
    // pointers, so there is no risk of use-after-free.
    if (g_lastPlayer && g_fetchNearby && g_setNameTag) {
        const float r = radius;
        bedrocktools::sdk::Vec3 extent{r, r, r};
        ActorVec actors = g_fetchNearby(g_lastPlayer, &extent, 1);
        if (actors.begin && actors.end) {
            for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
                void* actor = it->mActor;
                if (actor && isItemActor(actor)) restoreItemLabel(actor);
            }
        }
    }
    g_originalNames.clear();
}

void HoloItemsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    tickInterval = j.value("tickInterval", tickInterval);
    radius = j.value("radius", radius);
    showCount = j.value("showCount", showCount);
    colorEnabled = j.value("colorEnabled", colorEnabled);
}

void HoloItemsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["tickInterval"] = tickInterval;
    j["radius"] = radius;
    j["showCount"] = showCount;
    j["colorEnabled"] = colorEnabled;
}
