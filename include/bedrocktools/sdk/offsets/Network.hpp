#pragma once

#include <cstddef>

namespace bedrocktools::sdk::offsets {

namespace RakNetConnector {
inline constexpr std::size_t mAvgPing = 0x108;
}

namespace Packet {
inline constexpr std::size_t Size = 0x30;
inline constexpr std::size_t mHandlerDispatcher = 0x20;
}

namespace PacketHandlerDispatcher {
inline constexpr std::size_t HandlePacketVtableIndex = 2;
}

namespace ResourcePacksInfoPacket {
inline constexpr std::size_t mResourcePackRequired = 0x30;
inline constexpr std::size_t mForceDisableVibrantVisuals = 0x33;
}

namespace ResourcePackStackPacket {
inline constexpr std::size_t mResourcePackRequired = 0x68;
}

namespace TextPacketPayload {
inline constexpr std::size_t mBody = 0x58;
inline constexpr std::size_t mVariantIndex = 0x90;
namespace AuthorAndMessage {
inline constexpr std::size_t mType = 0x58;
inline constexpr std::size_t mAuthor = 0x60;
inline constexpr std::size_t mMessage = 0x78;
}
namespace MessageOnly {
inline constexpr std::size_t mType = 0x58;
inline constexpr std::size_t mMessage = 0x60;
}
}

namespace CommandRequestPacketPayload {
inline constexpr std::size_t mCommand = 0;
inline constexpr std::size_t mOrigin = 24;
inline constexpr std::size_t mInternalSource = 84;
}

namespace CommandOriginData {
inline constexpr std::size_t mType = 0;
}

namespace ChangeDimensionPacketPayload {
inline constexpr std::size_t mDimensionId = 0;
}

namespace SetTitlePacketPayload {
inline constexpr std::size_t mType = 0;
inline constexpr std::size_t mTitleText = 8;
}

namespace ModalFormRequestPacketPayload {
inline constexpr std::size_t mFormId = 0x0;
inline constexpr std::size_t mFormJson = 0x8;
}

namespace ModalFormResponsePacketPayload {
inline constexpr std::size_t mFormId = 0x0;
inline constexpr std::size_t mJsonValue = 0x8;
inline constexpr std::size_t mJsonValueType = 0x10;
inline constexpr std::size_t mJsonResponseHasValue = 0x18;
inline constexpr std::size_t mCancelReason = 0x20;
inline constexpr std::size_t Size = 0x24;
}

}
