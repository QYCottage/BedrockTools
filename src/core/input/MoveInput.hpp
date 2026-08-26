#pragma once

#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <entt/entt.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

enum class BedrockEntityId : std::uint32_t {};

template <std::size_t N, class T>
struct BedrockBitset {
    T value{};

    bool test(std::size_t index) const {
        return (value & (static_cast<T>(1) << index)) != 0;
    }

    void set(std::size_t index, bool state) {
        const T mask = static_cast<T>(static_cast<T>(1) << index);
        value = state ? static_cast<T>(value | mask) : static_cast<T>(value & ~mask);
    }
};

struct BedrockEntityIdTraits {
    using value_type = BedrockEntityId;
    using entity_type = std::uint32_t;
    using version_type = std::uint16_t;
    static constexpr std::uint32_t entity_mask = 0x3FFFF;
    static constexpr std::uint32_t version_mask = 0x3FFF;
};

template <>
struct entt::entt_traits<BedrockEntityId> : entt::basic_entt_traits<BedrockEntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};

namespace bedrocktools::input {

enum class MoveInputFlag : std::size_t {
    SneakDown = 0,
    JumpDown = 7,
    SprintDown = 8,
    Up = 13,
    Down = 14,
    Left = 15,
    Right = 16
};

struct MoveInputState {
    BedrockBitset<27, std::uint32_t> mFlagValues;
    sdk::Vec2 mAnalogMoveVector;
    std::uint8_t mLookSlightDirField;
    std::uint8_t mLookNormalDirField;
    std::uint8_t mLookSmoothDirField;
    std::uint8_t mPad;
};

struct MoveInputComponent {
    MoveInputState mInputState;
    MoveInputState mRawInputState;
    std::uint8_t mHoldAutoJumpInWaterTicks;
    std::uint8_t mPad[3];
    sdk::Vec2 mMove;
    sdk::Vec2 mLookDelta;
    sdk::Vec2 mInteractDir;
    sdk::Vec3 mDisplacement;
    sdk::Vec3 mDisplacementDelta;
    sdk::Vec3 mCameraOrientation;
    BedrockBitset<11, std::uint16_t> mFlagValues;
    std::array<bool, 2> mIsPaddling;
};

class EntityRegistry;

class EntityContext {
public:
    entt::basic_registry<BedrockEntityId>& registry() {
        return mEnTTRegistry;
    }

    template <class T>
    T* tryGetComponent() {
        return registry().try_get<T>(mEntity);
    }

    EntityRegistry& mRegistry;
    entt::basic_registry<BedrockEntityId>& mEnTTRegistry;
    BedrockEntityId const mEntity;
};

inline MoveInputComponent* getMoveInputComponent(sdk::Player* player) {
    if (!player) return nullptr;
    auto* context = reinterpret_cast<EntityContext*>(
        reinterpret_cast<std::uintptr_t>(player) + sdk::offsets::Actor::mEntityContext
    );
    return context ? context->tryGetComponent<MoveInputComponent>() : nullptr;
}

inline bool test(const MoveInputState& state, MoveInputFlag flag) {
    return state.mFlagValues.test(static_cast<std::size_t>(flag));
}

inline void set(MoveInputState& state, MoveInputFlag flag, bool value) {
    state.mFlagValues.set(static_cast<std::size_t>(flag), value);
}

}
