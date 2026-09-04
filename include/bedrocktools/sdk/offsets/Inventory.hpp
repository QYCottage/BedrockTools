#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets::Inventory {

inline constexpr std::size_t PlayerInventory = 0x570;
inline constexpr std::size_t PlayerInventoryContainer = 0xB8;
inline constexpr std::size_t ItemStackItemCounter = 0x8;
inline constexpr std::size_t ItemStackCount = 0x22;
inline constexpr std::size_t ItemStackValid = 0x23;
inline constexpr std::size_t ItemGetDescriptionIdVtableIndex = 5;
inline constexpr std::size_t FillingContainerItems = 0x140;
inline constexpr std::size_t ItemStackSize = 0x98;

}
