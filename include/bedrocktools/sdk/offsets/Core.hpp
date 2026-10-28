#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

namespace VTable {
inline constexpr std::size_t ClientInstance_getRegion = 30;
inline constexpr std::size_t BlockSource_getDimensionId = 18;
inline constexpr std::size_t RenderMaterialGroup_getMaterial = 2;
inline constexpr std::size_t HoverTextRendererRenderHoverBox = 17;
inline constexpr std::size_t MinecraftUIRenderContextGetLineLength = 2;
inline constexpr std::size_t MinecraftUIRenderContextDrawText = 6;
inline constexpr std::size_t MinecraftUIRenderContextFlushText = 8;
inline constexpr std::size_t MinecraftUIRenderContextDrawImage = 9;
inline constexpr std::size_t MinecraftUIRenderContextFlushImages = 11;
inline constexpr std::size_t MinecraftUIRenderContextFillRectangle = 17;
inline constexpr std::size_t MinecraftUIRenderContextGetFullClippingRectangle = 28;
inline constexpr std::size_t MinecraftUIRenderContextGetTexture = 33;
inline constexpr std::size_t MinecraftUIRenderContextFinish = 31;
inline constexpr std::size_t HudCameraRendererRender = 17;
inline constexpr std::size_t HudMobEffectsRendererRender = 17;
inline constexpr std::size_t ClientInstanceGetMinecraftGame = 79;
inline constexpr std::size_t ClientInstanceGetLocalPlayer = 31;
inline constexpr std::size_t PlayerGetCarriedItem = 78;
inline constexpr std::size_t ContainerScreenControllerSetSelectedSlot = 58;
inline constexpr std::size_t ContainerScreenControllerGetSelectedSlotInfo = 59;
inline constexpr std::size_t ItemGetMaxDamage = 37;
inline constexpr std::size_t ItemGetAnimationFrameFor = 120;
inline constexpr std::size_t ClientInstanceGetGuiScale = 210;
}

namespace ClientInstance {
inline constexpr std::size_t mGuiData = 0x648;
inline constexpr std::size_t mLevelRenderer = 0x1B8;
}

}
