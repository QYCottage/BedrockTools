// Mob Health Indicator module
// Displays health above mobs as hearts or a health bar

#include "mobhealthindicator.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <core/memory/Hooks.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Function pointer types
using ActorIsInvisible_t = bool (*)(void*);
using ActorGetNameTag_t = std::string (*)(void*);
using ActorSetNameTag_t = void (*)(void*, const std::string&);
using SynchedActorDataEnsureIndexFn = void (*)(void*, std::uint16_t);
using ActorSynchedDataUpdateAlwaysShowNameTagFn = void (*)(void*, const void*);
using ActorFetchNearbyActorsSorted_t = void* (*)(void*, void*, int);

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

// Global state
static MobHealthIndicatorModule* g_mobHealthMod = nullptr;

static ActorIsInvisible_t s_actorIsInvisible = nullptr;
static ActorGetNameTag_t s_getNameTag = nullptr;
static ActorSetNameTag_t s_setNameTag = nullptr;
static SynchedActorDataEnsureIndexFn s_ensureIndex = nullptr;
static ActorSynchedDataUpdateAlwaysShowNameTagFn s_updateAlwaysShowNameTag = nullptr;
static ActorFetchNearbyActorsSorted_t s_actorFetchNearby = nullptr;

// Helper functions (similar to TNT timer)
static void* getEntityDataWrapper(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityData);
}

static void* getEntityContext(void* actor) {
    if (!actor) return nullptr;
    return reinterpret_cast<void*>(
        reinterpret_cast<std::uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mEntityContext);
}

static void* getDataComponent(void* actor) {
    void* wrapper = getEntityDataWrapper(actor);
    if (!wrapper) return nullptr;
    return *reinterpret_cast<void**>(wrapper);
}

static std::size_t getItemsSize(void* component) {
    if (!component) return 0;
    auto** begin = *reinterpret_cast<void***>(component);
    auto** end = *reinterpret_cast<void***>(reinterpret_cast<std::uintptr_t>(component) + sizeof(void*));
    if (!begin || !end || end < begin) return 0;
    return static_cast<std::size_t>(end - begin);
}

static void** getItemsBegin(void* component) {
    if (!component) return nullptr;
    return *reinterpret_cast<void***>(component);
}

static void markDataItemPresentAndDirty(void* component, std::size_t id) {
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

static void* findSCharDataItemVtable(void* component) {
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

static bool readAlwaysShowItem(void* actor, std::int8_t& value) {
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

static bool writeAlwaysShowItem(void* actor, std::int8_t value) {
    if (!actor || !s_ensureIndex || !s_updateAlwaysShowNameTag) return false;

    void* wrapper = getEntityDataWrapper(actor);
    void* context = getEntityContext(actor);
    void* component = getDataComponent(actor);
    if (!wrapper || !context || !component) return false;

    constexpr std::uint16_t id = static_cast<std::uint16_t>(
        bedrocktools::sdk::offsets::ActorDataIds::NametagAlwaysShow);

    s_ensureIndex(component, id);

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
    s_updateAlwaysShowNameTag(context, wrapper);
    return true;
}

// Read health from synched data (ID 1, float type)
static float readHealth(void* actor) {
    if (!actor) return 0.0f;

    void* component = getDataComponent(actor);
    if (!component) return 0.0f;

    constexpr std::size_t healthId = 1; // Health is synched data ID 1
    if (getItemsSize(component) <= healthId) return 0.0f;

    auto** begin = getItemsBegin(component);
    if (!begin) return 0.0f;

    void* item = begin[healthId];
    if (!item) return 0.0f;

    const auto itemAddress = reinterpret_cast<std::uintptr_t>(item);
    const auto type = *reinterpret_cast<const std::uint8_t*>(
        itemAddress + bedrocktools::sdk::offsets::DataItem::mType);
    const auto id = *reinterpret_cast<const std::uint16_t*>(
        itemAddress + bedrocktools::sdk::offsets::DataItem::mId);

    // Health is a float (type 3) with ID 1
    if (type != bedrocktools::sdk::offsets::DataItem::FloatType || id != healthId) return 0.0f;

    return *reinterpret_cast<const float*>(
        itemAddress + bedrocktools::sdk::offsets::DataItem::mValue);
}

static bool hasCategory(void* actor, uint32_t categoryBit) {
    uintptr_t actorAddr = (uintptr_t)actor;
    uint32_t categories = *(uint32_t*)(actorAddr + bedrocktools::sdk::offsets::Actor::mCategories);
    return (categories & categoryBit) != 0;
}

// Format health as hearts display
std::string MobHealthIndicatorModule::formatHealthHearts(float health, float maxHealth) {
    if (maxHealth <= 0.0f) maxHealth = 20.0f;

    int totalHearts = m_maxHearts;
    int filledHearts = static_cast<int>(std::round((health / maxHealth) * totalHearts));
    filledHearts = std::max(0, std::min(filledHearts, totalHearts));

    std::string result;
    // Minecraft formatting codes: §c = red, §8 = dark gray
    // Use UTF-8 encoded section sign (§ = 0xC2 0xA7)
    const char* red = "\xC2\xA7""c";
    const char* darkGray = "\xC2\xA7""8";
    const char* heart = "\xE2\x9D\xA4"; // ❤ (U+2764)

    result += red;
    for (int i = 0; i < filledHearts; ++i) {
        result += heart;
    }
    result += darkGray;
    for (int i = filledHearts; i < totalHearts; ++i) {
        result += heart;
    }

    // Add health text
    char buffer[32];
    const char* textColor = "\xC2\xA7""f"; // white
    std::snprintf(buffer, sizeof(buffer), " %s%.0f/%.0f", textColor, health, maxHealth);
    result += buffer;

    return result;
}

// Format health as bar display
std::string MobHealthIndicatorModule::formatHealthBar(float health, float maxHealth) {
    if (maxHealth <= 0.0f) maxHealth = 20.0f;

    int totalSegments = static_cast<int>(m_barLength);
    if (totalSegments < 1) totalSegments = 10;
    int filledSegments = static_cast<int>(std::round((health / maxHealth) * totalSegments));
    filledSegments = std::max(0, std::min(filledSegments, totalSegments));

    // Determine color based on health percentage
    float percent = health / maxHealth;
    const char* barColor;
    if (percent > 0.5f) {
        barColor = "\xC2\xA7""a"; // green
    } else if (percent > 0.25f) {
        barColor = "\xC2\xA7""e"; // yellow
    } else {
        barColor = "\xC2\xA7""c"; // red
    }

    const char* emptyColor = "\xC2\xA7""8"; // dark gray
    const char* textColor = "\xC2\xA7""f";  // white

    std::string result;
    result += "[";
    result += barColor;
    for (int i = 0; i < filledSegments; ++i) {
        result += "\xE2\x96\x88"; // █ (U+2588)
    }
    result += emptyColor;
    for (int i = filledSegments; i < totalSegments; ++i) {
        result += "\xE2\x96\x91"; // ░ (U+2591)
    }
    result += textColor;
    result += "]";

    // Add health text
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), " %.0f/%.0f", health, maxHealth);
    result += buffer;

    return result;
}

// Tick callback
static void mobHealthTickCallback(void* localPlayer) {
    if (g_mobHealthMod && g_mobHealthMod->enabled) {
        g_mobHealthMod->onLocalPlayerTick(localPlayer);
    }
}

void MobHealthIndicatorModule::onLocalPlayerTick(void* localPlayer) {
    if (!localPlayer) return;

    // Throttle updates
    if (++m_refreshTicks < 10) return; // Update every 10 ticks (0.5 seconds)
    m_refreshTicks = 0;

    m_localPlayer = localPlayer;

    if (!s_actorFetchNearby) return;

    // Get nearby actors
    constexpr float kFetchRadius = 30.0f;
    bedrocktools::sdk::Vec3 extent = {kFetchRadius, kFetchRadius, kFetchRadius};
    ActorVec actors = s_actorFetchNearby(localPlayer, &extent, 1);

    if (!actors.begin || !actors.end) return;

    std::lock_guard<std::mutex> lock(m_mutex);

    // Track which actors we've seen this tick
    std::unordered_map<void*, bool> seenActors;

    for (DistanceSortedActor* it = actors.begin; it < actors.end; ++it) {
        void* ent = it->mActor;
        if (!ent || ent == localPlayer) continue;

        // Check distance
        float distance = it->mDistance;
        if (distance > m_maxDistance) continue;

        // Check if entity is a mob
        bool isMob = hasCategory(ent, 2); // Category bit 2 = living entity

        if (!isMob) continue;

        // Skip invisible entities
        if (s_actorIsInvisible && s_actorIsInvisible(ent)) continue;

        seenActors[ent] = true;

        // Read health
        float health = readHealth(ent);
        float maxHealth = 20.0f; // Default max health

        // Skip entities with 0 health (dead or invalid)
        if (health <= 0.0f) continue;

        // Check if health changed (to avoid redundant nametag updates)
        auto lastHealthIt = m_lastHealth.find(ent);
        if (lastHealthIt != m_lastHealth.end() && std::abs(lastHealthIt->second - health) < 0.1f) {
            continue; // Health hasn't changed significantly
        }
        m_lastHealth[ent] = health;

        // Save original nametag state if not already saved
        if (m_originalStates.find(ent) == m_originalStates.end()) {
            OriginalNametagState state;
            if (s_getNameTag) state.name = s_getNameTag(ent);
            state.hadAlwaysShow = readAlwaysShowItem(ent, state.alwaysShowValue);
            m_originalStates[ent] = state;
        }

        // Format health display
        std::string healthText;
        if (m_displayMode == 0) {
            healthText = formatHealthHearts(health, maxHealth);
        } else {
            healthText = formatHealthBar(health, maxHealth);
        }

        // Set nametag
        if (s_setNameTag) {
            s_setNameTag(ent, healthText);
        }
        writeAlwaysShowItem(ent, 1);
    }

    // Restore nametags for actors that are no longer visible
    for (auto it = m_originalStates.begin(); it != m_originalStates.end();) {
        void* ent = it->first;
        if (seenActors.find(ent) == seenActors.end()) {
            // Actor is no longer visible, restore nametag
            if (s_setNameTag) {
                s_setNameTag(ent, it->second.name);
            }
            writeAlwaysShowItem(ent, it->second.hadAlwaysShow ? it->second.alwaysShowValue : 0);
            m_lastHealth.erase(ent);
            it = m_originalStates.erase(it);
        } else {
            ++it;
        }
    }
}

void MobHealthIndicatorModule::restoreAllNametags() {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [ent, state] : m_originalStates) {
        if (s_setNameTag) {
            s_setNameTag(ent, state.name);
        }
        writeAlwaysShowItem(ent, state.hadAlwaysShow ? state.alwaysShowValue : 0);
    }

    m_originalStates.clear();
    m_lastHealth.clear();
}

MobHealthIndicatorModule::MobHealthIndicatorModule()
    : Module("Mob Health Indicator", "Displays health above mobs as hearts or a health bar.") {
    g_mobHealthMod = this;
    m_showMobs = true; // Only show for mobs, not players
}

MobHealthIndicatorModule::~MobHealthIndicatorModule() {
    if (g_mobHealthMod == this) g_mobHealthMod = nullptr;
}

void MobHealthIndicatorModule::onInit() {
    // Resolve signatures
    s_actorIsInvisible = reinterpret_cast<ActorIsInvisible_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsInvisible));

    s_getNameTag = reinterpret_cast<ActorGetNameTag_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));

    s_setNameTag = reinterpret_cast<ActorSetNameTag_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSetNameTag));

    s_ensureIndex = reinterpret_cast<SynchedActorDataEnsureIndexFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SynchedActorDataEnsureIndex));

    s_updateAlwaysShowNameTag = reinterpret_cast<ActorSynchedDataUpdateAlwaysShowNameTagFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorSynchedDataUpdateAlwaysShowNameTag));

    s_actorFetchNearby = reinterpret_cast<ActorFetchNearbyActorsSorted_t>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorFetchNearbyActorsSorted));

    // Subscribe to tick event
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { mobHealthTickCallback(event.player); });
}

void MobHealthIndicatorModule::onEnable() {
    m_refreshTicks = 20; // Force immediate update
}

void MobHealthIndicatorModule::onDisable() {
    restoreAllNametags();
}

void MobHealthIndicatorModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_displayMode")) m_displayMode = j["m_displayMode"].get<int>();
    if (j.contains("m_maxDistance")) m_maxDistance = j["m_maxDistance"].get<float>();
    if (j.contains("m_showMobs")) m_showMobs = j["m_showMobs"].get<bool>();
    if (j.contains("m_maxHearts")) m_maxHearts = j["m_maxHearts"].get<int>();
    if (j.contains("m_barLength")) m_barLength = j["m_barLength"].get<float>();
}

void MobHealthIndicatorModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_displayMode"] = m_displayMode;
    j["m_maxDistance"] = m_maxDistance;
    j["m_showMobs"] = m_showMobs;
    j["m_maxHearts"] = m_maxHearts;
    j["m_barLength"] = m_barLength;
}
