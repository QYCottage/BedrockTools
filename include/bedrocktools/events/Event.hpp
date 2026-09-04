#pragma once

#include <cstdint>

namespace bedrocktools::events {

enum class EventType : std::uint32_t {
    Frame = 1,
    LocalPlayerTick,
    ClientInstanceUpdate,
    Attack,
    MouseInput,
    ScreenState,
    LocalPlayerPreTick,
    GameModeAction,
    ContainerSlotSelected
};

enum class EventPriority : std::int32_t {
    First = 200,
    Early = 100,
    Normal = 0,
    Late = -100,
    Last = -200
};

class Cancellable {
public:
    bool cancelled() const { return mCancelled; }
    void cancel(bool value = true) { mCancelled = value; }

private:
    bool mCancelled = false;
};

}
