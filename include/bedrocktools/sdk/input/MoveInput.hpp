#pragma once

#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <entt/entt.hpp>
#include <array>
#include <cstddef>
#include <cstdint>

template <std::size_t N, class T>
struct BedrockBitset {
    T value{};

    bool test(std::size_t index) const {
        return index < N && (value & (T{1} << index)) != 0;
    }

    void set(std::size_t index, bool enabled) {
        if (index >= N) return;
        const T mask = T{1} << index;
        if (enabled) value |= mask;
        else value &= static_cast<T>(~mask);
    }
};

struct MoveInputState {
    enum class Flag : std::uint8_t {
        SneakDown = 0,
        SneakToggleDown = 1,
        WantDownSlow = 2,
        WantUpSlow = 3,
        BlockSelectDown = 4,
        AscendBlock = 5,
        DescendBlock = 6,
        JumpDown = 7,
        SprintDown = 8,
        UpLeft = 9,
        UpRight = 10,
        DownLeft = 11,
        DownRight = 12,
        Up = 13,
        Down = 14,
        Left = 15,
        Right = 16,
        Ascend = 17,
        Descend = 18,
        ChangeHeight = 19,
        LookCenter = 20,
        SneakInputCurrentlyDown = 21,
        SneakInputWasReleased = 22,
        SneakInputWasPressed = 23,
        JumpInputWasReleased = 24,
        JumpInputWasPressed = 25,
        JumpInputCurrentlyDown = 26
    };

    BedrockBitset<27, std::uint32_t> mFlagValues;
    bedrocktools::sdk::Vec2 mAnalogMoveVector;
    std::uint8_t mLookSlightDirField;
    std::uint8_t mLookNormalDirField;
    std::uint8_t mLookSmoothDirField;
    std::uint8_t mPadding;

    bool test(Flag flag) const {
        return mFlagValues.test(static_cast<std::size_t>(flag));
    }

    void set(Flag flag, bool enabled) {
        mFlagValues.set(static_cast<std::size_t>(flag), enabled);
    }
};

struct MoveInputComponent {
    enum class Flag : std::uint8_t {
        Sneaking = 0,
        Sprinting = 1,
        WantUp = 2,
        WantDown = 3,
        Jumping = 4,
        AutoJumpingInWater = 5,
        MoveInputStateLocked = 6,
        PersistSneak = 7,
        AutoJumpEnabled = 8,
        IsCameraRelativeMovementEnabled = 9,
        IsRotControlledByMoveDirection = 10
    };

    MoveInputState mInputState;
    MoveInputState mRawInputState;
    std::uint8_t mHoldAutoJumpInWaterTicks;
    std::uint8_t mPadding[3];
    bedrocktools::sdk::Vec2 mMove;
    bedrocktools::sdk::Vec2 mLookDelta;
    bedrocktools::sdk::Vec2 mInteractDir;
    bedrocktools::sdk::Vec3 mDisplacement;
    bedrocktools::sdk::Vec3 mDisplacementDelta;
    bedrocktools::sdk::Vec3 mCameraOrientation;
    BedrockBitset<11, std::uint16_t> mFlagValues;
    std::array<bool, 2> mIsPaddling;
};

enum class EntityId : std::uint32_t {};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = std::uint32_t;
    using version_type = std::uint16_t;
    static constexpr std::uint32_t entity_mask = 0x3FFFF;
    static constexpr std::uint32_t version_mask = 0x3FFF;
};

namespace entt {
template <>
struct entt_traits<EntityId> : basic_entt_traits<EntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};
}

class EntityRegistry;

class EntityContext {
public:
    entt::basic_registry<EntityId>& getRegistry() {
        return mEnTTRegistry;
    }

    template <class T>
    T* tryGetComponent() {
        return getRegistry().try_get<T>(mEntity);
    }

    EntityRegistry& mRegistry;
    entt::basic_registry<EntityId>& mEnTTRegistry;
    EntityId const mEntity;
};

static_assert(sizeof(MoveInputState) == 0x10);
static_assert(offsetof(MoveInputState, mAnalogMoveVector) == 0x4);
static_assert(offsetof(MoveInputComponent, mRawInputState) == 0x10);
static_assert(offsetof(MoveInputComponent, mMove) == 0x24);
static_assert(offsetof(MoveInputComponent, mFlagValues) == 0x60);
static_assert(sizeof(MoveInputComponent) == 0x64);

namespace bedrocktools::sdk {

inline MoveInputComponent* moveInputComponent(void* actor) {
    if (!actor) return nullptr;
    auto* context = reinterpret_cast<EntityContext*>(
        reinterpret_cast<std::uintptr_t>(actor) + offsets::Actor::mEntityContext
    );
    return context->tryGetComponent<MoveInputComponent>();
}

}
