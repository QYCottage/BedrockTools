#pragma once

#include <cstddef>
#include <cstdint>

namespace bedrocktools::sdk::offsets {

namespace Player {
inline constexpr std::size_t mName = 2824;
inline constexpr std::size_t mSkin = 2552;
}

namespace Actor {
inline constexpr std::size_t mEntityContext = 0x8;
inline constexpr std::size_t mEntityData = 0x120;
inline constexpr std::size_t mStateVectorComponent = 0x208;
inline constexpr std::size_t mActorRotationComponent = 0x218;
inline constexpr std::size_t mLevel = 464;
inline constexpr std::size_t mDimension = 448;
inline constexpr std::size_t mHurtTime = 0x194;
inline constexpr std::size_t mCategories = 512;
inline constexpr std::size_t mNameTagHash = 384;
inline constexpr std::size_t mFilteredNameTag = 712;
}

namespace ActorDataIds {
inline constexpr std::size_t FuseTime = 55;
inline constexpr std::size_t NametagAlwaysShow = 81;
}

namespace ActorFlags {
inline constexpr int CanShowName = 14;
inline constexpr int AlwaysShowName = 15;
}

namespace DataItem {
inline constexpr std::size_t mType = 0x8;
inline constexpr std::size_t mId = 0xA;
inline constexpr std::size_t mValue = 0xC;
inline constexpr std::uint8_t IntType = 2;
}

namespace BuiltInActorComponents {
inline constexpr std::size_t mAABBShapeComponent = 8;
}

namespace AABBShapeComponent {
inline constexpr std::size_t mAABB = 0;
}

namespace Level {
inline constexpr std::size_t mActorManager = 0x470;
inline constexpr std::size_t mHitResultWrapper = 456;
}

namespace HitResult {
inline constexpr std::size_t mStartPos = 0;
inline constexpr std::size_t mType = 24;
inline constexpr std::size_t mPos = 44;
}

namespace HitResultWrapper {
inline constexpr std::size_t mHitResult = 0;
}

namespace Dimension {
inline constexpr std::size_t mBlockSource = 208;
inline constexpr std::size_t mWeather = 0x1B8;
}

namespace Biome {
inline constexpr std::size_t mHash = 400;
}

namespace Weather {
inline constexpr std::size_t mOldRainLevel = 0x34;
inline constexpr std::size_t mRainLevel = 0x38;
inline constexpr std::size_t mTargetRainLevel = 0x3C;
inline constexpr std::size_t mOldLightningLevel = 0x40;
inline constexpr std::size_t mLightningLevel = 0x44;
inline constexpr std::size_t mTargetLightningLevel = 0x48;
}

}
