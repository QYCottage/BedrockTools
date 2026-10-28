#include "PreviewSelection.hpp"
#include "GameHooks.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/offsets/Inventory.hpp>
#include <algorithm>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace bedrocktools::core::preview {
namespace {

struct NativeSlotData {
    std::string collection;
    int index;
};

struct SelectedSlotInfo {
    const NativeSlotData* slot;
    int amount;
};

static_assert(sizeof(std::string) == 0x18);
static_assert(offsetof(NativeSlotData, index) == 0x18);
static_assert(sizeof(NativeSlotData) == 0x20);
static_assert(sizeof(SelectedSlotInfo) == 0x10);
static_assert(offsetof(SelectedSlotInfo, amount) == 0x8);

using SetSelectedSlotFn = void (*)(void*, const std::string&, int);
using GetSelectedSlotInfoFn = SelectedSlotInfo (*)(const void*);
using GetItemStackFn = void* (*)(void*, const std::string&, int);
SetSelectedSlotFn setSelectedSlotOriginal = nullptr;
GetSelectedSlotInfoFn getSelectedSlotInfoOriginal = nullptr;
GetItemStackFn getItemStack = nullptr;
bedrocktools::hooks::Handle selectionHook = nullptr;
bedrocktools::hooks::Handle selectedInfoHook = nullptr;
std::mutex initializationMutex;
std::mutex selectionMutex;
bool subscribed = false;
std::uint64_t revision = 1;
void* currentClient = nullptr;
void* currentPlayer = nullptr;
void* currentLevel = nullptr;
void* currentDimension = nullptr;
thread_local bool pollingSelection = false;

struct ContainerSelection {
    void* controller;
    std::string collection;
    int index = -1;
};

std::vector<ContainerSelection> containers;

void* localPlayer(void* client) {
    void** table = client ? *reinterpret_cast<void***>(client) : nullptr;
    const auto slot = sdk::offsets::VTable::ClientInstanceGetLocalPlayer;
    return table && table[slot] ? reinterpret_cast<void* (*)(void*)>(table[slot])(client) : nullptr;
}

void synchronize(void* client, void* player, void* level, void* dimension) {
    if (client == currentClient && player == currentPlayer && level == currentLevel &&
        dimension == currentDimension) return;
    const bool firstContext = currentClient == nullptr || currentLevel == nullptr;
    currentClient = client;
    currentPlayer = player;
    currentLevel = level;
    currentDimension = dimension;
    if (!firstContext) containers.clear();
    ++revision;
}

void observeController(void* controller) {
    if (!controller || pollingSelection) return;
    std::lock_guard lock(selectionMutex);
    auto found = std::find_if(containers.begin(), containers.end(),
        [controller](const auto& entry) { return entry.controller == controller; });
    if (found == containers.end()) {
        void* client = gamehooks::clientInstance();
        void* player = localPlayer(client);
        void* level = player ? *reinterpret_cast<void**>(static_cast<std::byte*>(player) + sdk::offsets::Actor::mLevel) : nullptr;
        void* dimension = player ? *reinterpret_cast<void**>(static_cast<std::byte*>(player) + sdk::offsets::Actor::mDimension) : nullptr;
        if (client && player && level) synchronize(client, player, level, dimension);
        containers.push_back({controller, {}, -1});
    } else {
        if (found + 1 == containers.end()) return;
        auto selected = std::move(*found);
        containers.erase(found);
        containers.push_back(std::move(selected));
    }
    ++revision;
}

void setSelectedSlot(void* controller, const std::string& collection, int index) {
    if (setSelectedSlotOriginal) setSelectedSlotOriginal(controller, collection, index);
    observeController(controller);
}

SelectedSlotInfo selectedSlotInfo(const void* controller) {
    const auto result = getSelectedSlotInfoOriginal
        ? getSelectedSlotInfoOriginal(controller) : SelectedSlotInfo{nullptr, -1};
    observeController(const_cast<void*>(controller));
    return result;
}

class PollScope {
public:
    PollScope() : previous(pollingSelection) { pollingSelection = true; }
    ~PollScope() { pollingSelection = previous; }

private:
    bool previous;
};

bool validStack(void* stack) {
    if (!stack) return false;
    auto* base = static_cast<std::byte*>(stack);
    if (!*reinterpret_cast<std::uint8_t*>(base + sdk::offsets::Inventory::ItemStackValid)) return false;
    if (!*reinterpret_cast<std::uint8_t*>(base + sdk::offsets::Inventory::ItemStackCount)) return false;
    void* counter = *reinterpret_cast<void**>(base + sdk::offsets::Inventory::ItemStackItemCounter);
    return counter && *reinterpret_cast<void**>(counter);
}

void screenState(events::ScreenStateEvent& event) {
    if (event.screen != events::ScreenKind::Container || !event.controller) return;
    if (event.phase == events::ScreenPhase::Opened) {
        observeController(event.controller);
        return;
    }
    std::lock_guard lock(selectionMutex);
    const auto end = std::remove_if(containers.begin(), containers.end(),
        [&](const auto& entry) { return entry.controller == event.controller; });
    const bool removed = end != containers.end();
    containers.erase(end, containers.end());
    if (removed) ++revision;
}

}

bool initialize() {
    std::lock_guard lock(initializationMutex);
    if (!subscribed) {
        events::bus().subscribe<events::ScreenStateEvent>(screenState);
        events::bus().subscribe<events::ContainerSlotSelectedEvent>([](auto& event) {
            if (event.afterSelection && !event.cancelled()) observeController(event.controller);
        });
        subscribed = true;
    }
    if (!getItemStack) getItemStack = reinterpret_cast<GetItemStackFn>(
        memory::resolve(memory::SignatureId::ContainerScreenControllerGetItemStack));
    if (!selectionHook) {
        const auto target = memory::resolve(memory::SignatureId::ContainerScreenControllerHandleSelectSlot);
        if (target) selectionHook = bedrocktools::hooks::install(reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(setSelectedSlot), reinterpret_cast<void**>(&setSelectedSlotOriginal));
    }
    if (!selectedInfoHook) {
        const auto target = memory::resolve(memory::SignatureId::ContainerScreenControllerGetSelectedSlotInfo);
        if (target) selectedInfoHook = bedrocktools::hooks::install(reinterpret_cast<void*>(target),
            reinterpret_cast<void*>(selectedSlotInfo), reinterpret_cast<void**>(&getSelectedSlotInfoOriginal));
    }
    return getItemStack && selectionHook && selectedInfoHook;
}

Selection current(void* uiContext) {
    Selection result;
    if (!uiContext) return result;
    void* client = *reinterpret_cast<void**>(static_cast<std::byte*>(uiContext) +
        sdk::offsets::MinecraftUIRenderContext::mClient);
    void* activeClient = gamehooks::clientInstance();
    if (!client || (activeClient && client != activeClient)) return result;
    result.player = localPlayer(client);
    if (result.player) {
        result.level = *reinterpret_cast<void**>(static_cast<std::byte*>(result.player) + sdk::offsets::Actor::mLevel);
        result.dimension = *reinterpret_cast<void**>(static_cast<std::byte*>(result.player) + sdk::offsets::Actor::mDimension);
    }
    std::lock_guard lock(selectionMutex);
    synchronize(client, result.player, result.level, result.dimension);
    if (!result.player || !result.level) {
        result.revision = revision;
        return result;
    }
    if (!containers.empty()) {
        result.inContainer = true;
        auto& selected = containers.back();
        PollScope scope;
        void** table = *reinterpret_cast<void***>(selected.controller);
        const auto slot = sdk::offsets::VTable::ContainerScreenControllerGetSelectedSlotInfo;
        SelectedSlotInfo info{nullptr, -1};
        if (table && table[slot]) {
            info = reinterpret_cast<GetSelectedSlotInfoFn>(table[slot])(selected.controller);
        }
        const bool valid = info.slot && info.slot->index >= 0 && !info.slot->collection.empty();
        if (valid) {
            if (selected.collection != info.slot->collection || selected.index != info.slot->index) {
                selected.collection = info.slot->collection;
                selected.index = info.slot->index;
                ++revision;
            }
            if (getItemStack) result.stack = getItemStack(selected.controller, info.slot->collection, info.slot->index);
        } else if (selected.index >= 0 || !selected.collection.empty()) {
            selected.collection.clear();
            selected.index = -1;
            ++revision;
        }
    } else {
        void** table = *reinterpret_cast<void***>(result.player);
        const auto slot = sdk::offsets::VTable::PlayerGetCarriedItem;
        if (table && table[slot]) result.stack = reinterpret_cast<void* (*)(void*)>(table[slot])(result.player);
    }
    if (!validStack(result.stack)) result.stack = nullptr;
    result.revision = revision;
    return result;
}

}
