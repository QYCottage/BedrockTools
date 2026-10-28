#pragma once

#include <bedrocktools/sdk/Offsets.hpp>
#include <array>
#include <cstddef>
#include <cstring>

namespace bedrocktools::core {

class ScopedShaderColor {
public:
    explicit ScopedShaderColor(void* screenContext) {
        if (!screenContext) return;
        mHolder = *reinterpret_cast<std::byte**>(static_cast<std::byte*>(screenContext) +
            bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
        if (!mHolder) return;
        std::memcpy(mSaved.data(), mHolder, sizeof(mSaved));
        const std::array<float, 4> white{1.0f, 1.0f, 1.0f, 1.0f};
        std::memcpy(mHolder, white.data(), sizeof(white));
        markDirty();
    }

    ~ScopedShaderColor() {
        if (!mHolder) return;
        std::memcpy(mHolder, mSaved.data(), sizeof(mSaved));
        markDirty();
    }

    explicit operator bool() const { return mHolder != nullptr; }
    ScopedShaderColor(const ScopedShaderColor&) = delete;
    ScopedShaderColor& operator=(const ScopedShaderColor&) = delete;

private:
    void markDirty() {
        mHolder[bedrocktools::sdk::offsets::ShaderColor::mDirty] = std::byte{1};
    }

    std::byte* mHolder = nullptr;
    std::array<float, 4> mSaved{};
};

}
