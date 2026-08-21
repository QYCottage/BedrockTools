#pragma once

namespace bedrocktools::modules::blockoutline {

struct RgbColor {
    float r;
    float g;
    float b;
};

// Wraps any phase into [0, 1). Kept separate from rainbowRgb so the render
// hook (and the tests) can reason about the wrap-around explicitly.
constexpr float wrapPhase(float phase) {
    float p = phase - static_cast<float>(static_cast<long long>(phase));
    if (p < 0.0f) p += 1.0f;
    // A tiny negative phase can round all the way up to 1.0f when the +1
    // above is added; clamp it back into range.
    if (p >= 1.0f) p = 0.0f;
    return p;
}

// Converts a hue phase in [0, 1) to a fully saturated RGB color, cycling
// through the whole spectrum: red at 0, green at 1/3, blue at 2/3 and back
// to red at 1. Used by the "RGB" rainbow mode of the Block Outline module;
// constexpr so the host tests can pin exact points of the cycle.
constexpr RgbColor rainbowRgb(float phase) {
    const float p = wrapPhase(phase) * 6.0f;
    const int sector = static_cast<int>(p);
    const float frac = p - static_cast<float>(sector);
    switch (sector) {
        case 0:  return {1.0f, frac, 0.0f};
        case 1:  return {1.0f - frac, 1.0f, 0.0f};
        case 2:  return {0.0f, 1.0f, frac};
        case 3:  return {0.0f, 1.0f - frac, 1.0f};
        case 4:  return {frac, 0.0f, 1.0f};
        default: return {1.0f, 0.0f, 1.0f - frac};
    }
}

} // namespace bedrocktools::modules::blockoutline
