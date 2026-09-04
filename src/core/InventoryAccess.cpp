#include "InventoryAccess.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/offsets/Inventory.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bedrocktools::core {
namespace {
using namespace bedrocktools::sdk::offsets::Inventory;

template<class T>
T read(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}

bool inventorySlots(const void* player, std::uintptr_t& begin, std::size_t& size) {
    begin = 0;
    size = 0;
    if (!player) return false;
    const void* proxy = read<const void*>(player, PlayerInventory);
    if (!proxy) return false;
    const void* inventory = read<const void*>(proxy, PlayerInventoryContainer);
    if (!inventory) return false;
    begin = read<std::uintptr_t>(inventory, FillingContainerItems);
    const auto end = read<std::uintptr_t>(inventory, FillingContainerItems + sizeof(void*));
    if (!begin && !end) return true;
    if (!begin || end < begin || begin % alignof(void*) != 0) return false;
    const auto bytes = end - begin;
    if (bytes % ItemStackSize != 0) return false;
    size = bytes / ItemStackSize;
    return size <= 64;
}
}

InventoryAccess& InventoryAccess::get() {
    static InventoryAccess access;
    return access;
}

void InventoryAccess::initialize() {
    m_getOffhandSlot = reinterpret_cast<GetOffhandSlotFn>(
        memory::resolve(memory::SignatureId::ActorGetOffhandSlot));
}

int InventoryAccess::countItems(const sdk::Player* player, std::string_view identifierPart) const {
    if (!player) return 0;
    std::uintptr_t begin = 0;
    std::size_t size = 0;
    int total = 0;
    const bool valid = inventorySlots(player, begin, size);
    if (valid) {
        for (std::size_t slot = 0; slot < size; ++slot) {
            total += countStack(reinterpret_cast<const void*>(begin + slot * ItemStackSize), identifierPart);
        }
    }
    const void* offhand = m_getOffhandSlot ? m_getOffhandSlot(player) : nullptr;
    const auto offhandAddress = reinterpret_cast<std::uintptr_t>(offhand);
    const bool alreadyCounted = valid && begin && offhandAddress >= begin &&
        offhandAddress - begin < size * ItemStackSize && (offhandAddress - begin) % ItemStackSize == 0;
    if (!alreadyCounted) total += countStack(offhand, identifierPart);
    return total;
}

int InventoryAccess::countStack(const void* stack, std::string_view identifierPart) const {
    if (!stack || identifierPart.empty() || !read<std::uint8_t>(stack, ItemStackValid)) return 0;
    const auto count = read<std::uint8_t>(stack, ItemStackCount);
    if (count == 0) return 0;
    const void* counter = read<const void*>(stack, ItemStackItemCounter);
    const void* item = counter ? read<const void*>(counter, 0) : nullptr;
    if (!item) return 0;
    const auto* vtable = read<const std::uintptr_t*>(item, 0);
    if (!vtable || !vtable[ItemGetDescriptionIdVtableIndex]) return 0;
    using GetDescriptionIdFn = const std::string& (*)(const void*);
    const auto& identifier = reinterpret_cast<GetDescriptionIdFn>(vtable[ItemGetDescriptionIdVtableIndex])(item);
    return identifier.size() <= 128 && identifier.find(identifierPart) != std::string::npos ? count : 0;
}

}
