#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

namespace VTable {
inline constexpr std::size_t ClientInstance_getRegion = 31;
inline constexpr std::size_t BlockSource_getDimensionId = 18;
inline constexpr std::size_t RenderMaterialGroup_getMaterial = 2;
inline constexpr std::size_t HoverTextRendererRenderHoverBox = 17;
inline constexpr std::size_t MinecraftUIRenderContextGetLineLength = 2;
inline constexpr std::size_t MinecraftUIRenderContextDrawText = 6;
inline constexpr std::size_t MinecraftUIRenderContextFlushText = 7;
inline constexpr std::size_t MinecraftUIRenderContextDrawImage = 8;
inline constexpr std::size_t MinecraftUIRenderContextFlushImages = 10;
inline constexpr std::size_t MinecraftUIRenderContextFillRectangle = 16;
inline constexpr std::size_t MinecraftUIRenderContextGetTexture = 32;
inline constexpr std::size_t ClientInstanceGetMinecraftGame = 83;
inline constexpr std::size_t ClientInstanceGetLocalPlayer = 32;
inline constexpr std::size_t ItemGetMaxDamage = 37;
inline constexpr std::size_t ItemGetAnimationFrameFor = 120;
}

namespace ClientInstance {
inline constexpr std::size_t mLevelRenderer = 0x190;
}

}
