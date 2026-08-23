#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

// mce::Image (Android/libc++ build, size = 0x30 / 48 bytes):
//   ImageFormat mImageFormat; // 0x00 (RGBA8Unorm = 4)
//   uint32      mWidth;       // 0x04
//   uint32      mHeight;      // 0x08
//   uint32      mDepth;       // 0x0C
//   ImageUsage  mUsage;       // 0x10 (uchar: Unknown = 0, SRGB = 1)
//   mce::Blob   mImageBytes;  // 0x18 (size = 0x18: { unique_ptr mBlob; void(*mDeleter)(uchar*); size_t mSize })
namespace Image {
inline constexpr std::size_t mImageFormat = 0x00;
inline constexpr std::size_t mDepth = 0x0C;
inline constexpr std::size_t mUsage = 0x10;
inline constexpr std::size_t mBytesOffset = 0x18;     // mce::Blob::mBlob (pixel pointer)
inline constexpr std::size_t mBlobDeleterOffset = 0x20; // mce::Blob deleter function (void(*)(unsigned char*))
inline constexpr std::size_t mBlobSizeOffset = 0x28;  // mce::Blob::mSize (pixel buffer byte count)
inline constexpr std::size_t Size = 0x30;
}

namespace SerializedSkinRef {
inline constexpr std::size_t mSkinImpl = 0;
}

namespace ThreadOwner {
inline constexpr std::size_t mObject = 0;
}

// SerializedSkinImpl member layout (Android arm64, libc++ std::string = 24
// bytes, Json::Value = 24, MinEngineVersion = 24, unordered_map = 32).
// The offsets of mSkinImage (120), mSkinAnimatedImages (216) and mIsPersona
// (442) below are verified in-game by the Skin Stealer module; every other
// offset here is derived from that verified layout and member order:
//
//   mId                              0
//   mPlayFabId                       24
//   mFullId                          48
//   mResourcePatch                   72
//   mDefaultGeometryName             96
//   mSkinImage                       120   (verified)
//   mCapeImage                       168
//   mSkinAnimatedImages              216   (verified)
//   mGeometryData                    240
//   mGeometryDataMinEngineVersion    264
//   mGeometryDataMutable             288
//   mAnimationData                   312
//   mCapeId                          336
//   mPersonaPieces                   360
//   mArmSizeType                     384
//   mPieceTintColors                 392   (8-aligned)
//   mSkinColor                       424
//   mIsTrustedSkin                   440
//   mIsPremium                       441
//   mIsPersona                       442   (verified)
//   mIsPersonaCapeOnClassicSkin      443
//   mIsPrimaryUser                   444
//   mOverridesPlayerAppearance       445
namespace SerializedSkinImpl {
inline constexpr std::size_t mSkinImage = 120;
inline constexpr std::size_t mCapeImage = 168;
inline constexpr std::size_t mCapeId = 336;
inline constexpr std::size_t mIsPersona = 442;
inline constexpr std::size_t mIsPersonaCapeOnClassicSkin = 443;
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
