#pragma once

#include <bedrocktools/Export.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bedrocktools::memory {

enum class SignatureId : std::uint16_t {
    VersionString,
    Nametag,
    Fullbright,
    SetupFogPlayer,
    RaknetUpdate,
    NormalTick,
    Time,
    SetTime,
    EduMultiplayer,
    HudCursor,
    LevelInit,
    LevelDtor,
    ActorManagerList,
    DimensionTick,
    WeatherTick,
    WeatherGetRainLevel,
    WeatherGetLightningLevel,
    WeatherIsRaining,
    WeatherIsLightning,
    RenderItem,
    GetFov,
    GetPerspective,
    ClientInstanceUpdate,
    ClientInstanceGetLocalPlayer,
    ContainerScreenControllerDtor,
    ContainerScreenControllerOpen,
    ChatScreenDtor,
    ChatScreenOpen,
    BiomeGetTemperature,
    GetDestroyProgress,
    RenderLevel,
    TessellatorBegin,
    TessellatorColor,
    TessellatorVertex,
    MeshHelpersRenderMeshImmediately,
    MeshHelpersRenderMeshImmediately2,
    RenderMaterialGroupCommon,
    SurvivalModeAttack,
    SurvivalModeDestroyBlock,
    GameModeAttack,
    LevelGetHitResult,
    BlockSourceGetBiome,
    BlockSourceGetBlock,
    BlockSourceGetBrightness,
    BlockSourceIsSolidBlockingBlock,
    LocalPlayerApplyTurnDelta,
    BaseOptionRegistryGetHideItemInHand,
    HitResultGetEntity,
    ActorIsPlayer,
    ActorIsInvisible,
    ActorFetchNearbyActorsSorted,
    ActorGetNameTag,
    ActorSetNameTag,
    MinecraftUIRenderContextDrawText,
    ScreenViewRender,
    ContainerScreenControllerOnContainerSlotSelected,
    ContainerScreenControllerGetItemStack,
    ClientNetworkHandlerHandleSetTitle,
    ClientNetworkHandlerHandleText,
    LoopbackPacketSenderSendToServer,
    ClientInstanceGetPacketSender,
    MinecraftPacketsCreatePacket,
    LocalPlayerChangeDimension,
    NbtTreeFind,
    ItemStackBaseLoadItem,
    ItemStackBaseGetDamageValue,
    BaseActorRenderContextCtor,
    ItemRendererRenderGuiItemNew,
    ControlOptionEditorTick,
    ControlOptionEditorRender,
    BlockTessellatorTessellateFaceDown,
    BlockTessellatorTessellateFaceUp,
    BlockTessellatorTessellateFaceNorth,
    BlockTessellatorTessellateFaceSouth,
    BlockTessellatorTessellateFaceWest,
    BlockTessellatorTessellateFaceEast,
    BlockTessellatorTessellatePane,
    BlockSourceGetBlockForTessellation,
    TextureUVCoordinateSetCopyCtor,
    TextureUVCoordinateSetDtor,
    RenderChunkCoordinatorSetAllDirty,
    Count
};

inline constexpr std::size_t SignatureCount = static_cast<std::size_t>(SignatureId::Count);

struct SignatureDefinition {
    SignatureId id;
    std::string_view pattern;
};

BEDROCKTOOLS_API bool resolveAll(std::string_view libraryName = "libminecraftpe.so");
BEDROCKTOOLS_API std::uintptr_t resolve(SignatureId id);
BEDROCKTOOLS_API void clear();

}
