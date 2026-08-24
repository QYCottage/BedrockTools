#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/HitResult.hpp>

namespace bedrocktools::sdk {

class Level {
public:
    void* actorManager() const { return field<void*>(this, offsets::Level::mActorManager); }

    HitResult* storedHitResult() {
        auto* wrapper = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(this) + offsets::Level::mHitResultWrapper);
        return reinterpret_cast<HitResult*>(reinterpret_cast<std::uintptr_t>(wrapper) + offsets::HitResultWrapper::mHitResult);
    }

    const HitResult* storedHitResult() const {
        auto* wrapper = reinterpret_cast<const void*>(reinterpret_cast<std::uintptr_t>(this) + offsets::Level::mHitResultWrapper);
        return reinterpret_cast<const HitResult*>(reinterpret_cast<std::uintptr_t>(wrapper) + offsets::HitResultWrapper::mHitResult);
    }
};

}
