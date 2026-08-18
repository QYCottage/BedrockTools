#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets {

namespace LevelRendererPlayer {
inline constexpr std::size_t mFogColorRed = 0x424;
inline constexpr std::size_t mFogColorGreen = 0x428;
inline constexpr std::size_t mFogColorBlue = 0x42C;
inline constexpr std::size_t mBaseFogStart = 0x434;
inline constexpr std::size_t mBaseFogEnd = 0x438;
inline constexpr std::size_t mCurrentFogDensityMax = 0x464;
inline constexpr std::size_t mCamPos = 0x61C;
inline constexpr std::size_t mSelectionOverlayMaterial = 0x1030;
}

namespace ScreenContext {
inline constexpr std::size_t mActorShaderConstants = 0x20;
inline constexpr std::size_t mColorHolder = 0x30;
inline constexpr std::size_t mTessellator = 0xB8;
}

namespace ActorShaderConstants {
inline constexpr std::size_t mGlintColor = 0xF8;
}

namespace ShaderConstant {
inline constexpr std::size_t mDirty = 0x29;
inline constexpr std::size_t mData = 0x30;
}

namespace MaterialGroup {
inline constexpr std::size_t mRenderMaterialGroupOffset = 32;
}

namespace RenderContext {
inline constexpr std::size_t mMatrixStackWrapper = 0x28;
}

namespace MatrixStackWrapper {
inline constexpr std::size_t mMatrixStack = 0x18;
}

namespace MatrixStack {
inline constexpr std::size_t mBlocks = 0x50;
inline constexpr std::size_t mStart = 0x68;
inline constexpr std::size_t mSize = 0x70;
}

namespace NameTag {
inline constexpr std::size_t mExtractNameTagsPatchOffset = 0x1A0;
}

namespace BlockTessellator {
inline constexpr std::size_t mRegion = 0x8;
inline constexpr std::size_t mInternalTexture = 0x18;
inline constexpr std::size_t mUseInternalTexture = 0x71;
inline constexpr std::size_t mXFlipTexture = 0x74;
inline constexpr std::size_t mFlipFace = 0x174;
inline constexpr std::size_t mTextureOverride = 0x17C;
inline constexpr std::size_t mCurrentShapeBB = 0x5F0;
}

namespace Tessellator {
inline constexpr std::size_t mTextureU = 0x17C;
inline constexpr std::size_t mTextureV = 0x180;
}

namespace Block {
inline constexpr std::size_t mBlockType = 0x68;
}

namespace BlockType {
inline constexpr std::size_t mNameInfo = 0x88;
}

namespace NameInfo {
inline constexpr std::size_t mFullName = 0x40;
}

namespace HashedString {
inline constexpr std::size_t mString = 0x8;
}

namespace TextureUVCoordinateSet {
inline constexpr std::size_t mU0 = 0x4;
inline constexpr std::size_t mV0 = 0x8;
inline constexpr std::size_t mU1 = 0xC;
inline constexpr std::size_t mV1 = 0x10;
inline constexpr std::size_t mSizeW = 0x14;
inline constexpr std::size_t mSizeH = 0x16;
inline constexpr std::size_t mIsotropicFaceData = 0x50;
inline constexpr std::size_t Size = 0x58;
inline constexpr std::size_t Alignment = 0x8;
}

namespace IsotropicFaceData {
inline constexpr std::size_t mTextureIsotropic = 0x0;
}

namespace FlipFace {
inline constexpr std::size_t ElementSize = 0x1;
inline constexpr std::uint8_t DontRotate = 5;
}

namespace LevelRenderer {
inline constexpr std::size_t mRenderChunkCoordinators = 0x28;
inline constexpr std::size_t mLevelRendererPlayer = 0x420;
}

namespace HashTable {
inline constexpr std::size_t mFirstNode = 0x10;
}

namespace HashNode {
inline constexpr std::size_t mNext = 0x0;
inline constexpr std::size_t mValuePointer = 0x18;
}

namespace RenderChunkCoordinator {
inline constexpr std::size_t MaxNodes = 64;
}

}
