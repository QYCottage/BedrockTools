#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

namespace Image {
inline constexpr std::size_t mBytesOffset = 0x18;
}

namespace SerializedSkinRef {
inline constexpr std::size_t mSkinImpl = 0;
}

namespace ThreadOwner {
inline constexpr std::size_t mObject = 0;
}

namespace SerializedSkinImpl {
inline constexpr std::size_t mSkinImage = 120;
inline constexpr std::size_t mIsPersona = 442;
inline constexpr std::size_t mSkinAnimatedImages = 216;
}

namespace AnimatedImageData {
inline constexpr std::size_t mType = 0;
inline constexpr std::size_t mImage = 8;
inline constexpr std::size_t Size = 64;
}

namespace SkinImage {
inline constexpr std::size_t mWidth = 4;
inline constexpr std::size_t mHeight = 8;
}

}
