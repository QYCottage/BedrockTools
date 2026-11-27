#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets::Rtti {

namespace Actor {
inline constexpr std::uintptr_t VTableRva   = 0x13CF6268;
inline constexpr std::uintptr_t TypeInfoRva = 0x13D070A0;
inline constexpr std::size_t    SlotCount   = 137;
}

namespace Block {
inline constexpr std::uintptr_t VTableRva   = 0x13DFCC28;
inline constexpr std::uintptr_t TypeInfoRva = 0x13DFCF40;
inline constexpr std::size_t    SlotCount   = 2;
}

namespace ItemStackBase {
inline constexpr std::uintptr_t VTableRva   = 0x13D94068;
inline constexpr std::uintptr_t TypeInfoRva = 0x13D962F0;
inline constexpr std::size_t    SlotCount   = 2;
}

namespace ItemStack {
inline constexpr std::uintptr_t VTableRva   = 0x13D96B78;
inline constexpr std::uintptr_t TypeInfoRva = 0x13D96318;
inline constexpr std::size_t    SlotCount   = 9;
}

namespace Level {
inline constexpr std::uintptr_t VTableRva   = 0x13E0AC78;
inline constexpr std::uintptr_t TypeInfoRva = 0x13E0D5F8;
inline constexpr std::size_t    SlotCount   = 143;
}

namespace Mob {
inline constexpr std::uintptr_t VTableRva   = 0x13CB9090;
inline constexpr std::uintptr_t TypeInfoRva = 0x13CC8D80;
inline constexpr std::size_t    SlotCount   = 62;
}

namespace Player {
inline constexpr std::uintptr_t VTableRva   = 0x13D43858;
inline constexpr std::uintptr_t TypeInfoRva = 0x13D45580;
inline constexpr std::size_t    SlotCount   = 181;
}

namespace LocalPlayer {
inline constexpr std::uintptr_t VTableRva   = 0x139F7A00;
inline constexpr std::uintptr_t TypeInfoRva = 0x139F8A30;
inline constexpr std::size_t    SlotCount   = 247;
}

namespace ServerPlayer {
inline constexpr std::uintptr_t VTableRva   = 0x13C7CBB8;
inline constexpr std::uintptr_t TypeInfoRva = 0x13C7F660;
inline constexpr std::size_t    SlotCount   = 249;
}

}
