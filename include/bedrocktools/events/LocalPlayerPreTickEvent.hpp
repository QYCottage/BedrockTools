#pragma once

#include <bedrocktools/events/Event.hpp>

namespace bedrocktools::sdk { class Player; }

namespace bedrocktools::events {

struct LocalPlayerPreTickEvent {
    static constexpr EventType type = EventType::LocalPlayerPreTick;
    sdk::Player* player;
};

}
