#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/HitResult.hpp>
#include <utility>

namespace bedrocktools::sdk {

class Level {
public:
    void* actorManager() const { return field<void*>(this, offsets::Level::mActorManager); }

    HitResult* storedHitResult() {
        return const_cast<HitResult*>(std::as_const(*this).storedHitResult());
    }

    const HitResult* storedHitResult() const {
        // Level owns HitResultWrapper through Bedrock::UniqueOwnerPointer.
        // The old implementation treated the owner object itself as the
        // wrapper, so callers read unrelated Level memory as a HitResult.
        const auto* owner = reinterpret_cast<const void*>(
            reinterpret_cast<std::uintptr_t>(this) +
            offsets::Level::mHitResultWrapper);
        const void* wrapper = field<void*>(
            owner, offsets::UniqueOwnerPointer::mValue);
        if (!wrapper) return nullptr;

        return reinterpret_cast<const HitResult*>(
            reinterpret_cast<std::uintptr_t>(wrapper) +
            offsets::HitResultWrapper::mHitResult);
    }
};

}
