#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

namespace ControlOptionEditorControl {
inline constexpr std::size_t mReservedAreas = 0x120;
inline constexpr std::size_t ReservedAreaEntrySize = 0x30;
}

namespace GuiMessage {
inline constexpr std::size_t Type = 0x0;
inline constexpr std::size_t FullString = 0x70;
inline constexpr std::size_t FilteredFullString = 0x88;
inline constexpr std::size_t FilteredFullStringPresent = 0xA0;
}

namespace ShulkerPreview {
inline constexpr std::size_t ItemStackBaseItem = 0x8;
inline constexpr std::size_t ItemStackBaseUserData = 0x10;
inline constexpr std::size_t SharedCounterPointer = 0x0;
inline constexpr std::size_t ItemId = 0x8A;
inline constexpr std::size_t CompoundTagTreeRoot = 0x8;
inline constexpr std::size_t CompoundTagTreeEnd = 0x10;
inline constexpr std::size_t NbtNodePayload = 0x38;
inline constexpr std::size_t NbtNodeNumericValue = 0x40;
inline constexpr std::size_t NbtNodeType = 0x60;
inline constexpr std::size_t HoverRendererCursorX = 0x40;
inline constexpr std::size_t HoverRendererCursorY = 0x44;
inline constexpr std::size_t MinecraftUIRenderContextClient = 0x8;
inline constexpr std::size_t MinecraftUIRenderContextScreenContext = 0x10;
inline constexpr std::size_t ClientInstanceMinecraftGame = 0xA8;
inline constexpr std::size_t BaseActorRenderContextItemRenderer = 0x58;
inline constexpr std::size_t BaseActorRenderContextStorageSize = 0x400;
inline constexpr std::size_t ItemStackStorageSize = 0x800;
}

}
