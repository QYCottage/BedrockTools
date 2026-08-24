#pragma once

#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

namespace bedrocktools::sdk {

class HitResult {
public:
    int type() const { return field<int>(this, offsets::HitResult::mType); }
    const Vec3& startPosition() const { return field<Vec3>(this, offsets::HitResult::mStartPos); }
    const Vec3& position() const { return field<Vec3>(this, offsets::HitResult::mPos); }
};

}
