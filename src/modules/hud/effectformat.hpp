#pragma once

// ---------------------------------------------------------------------------
// Pure formatting helpers for the Effect Display HUD module: effect level,
// remaining duration and the urgency color of the countdown.
//
// Everything here is dependency-free so it can be unit tested without the
// game (see tests/effectformat_test.cpp).
// ---------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace bedrocktools::hud::effects {

// Bedrock ticks at 20 Hz.
inline constexpr int kTicksPerSecond = 20;

// Bedrock marks an endless effect with INT32_MAX (a few packet paths use -1).
// Anything past a quarter of the int range can never be observed counting
// down, so it is treated as endless too.
inline constexpr int kInfiniteDurationThreshold = std::numeric_limits<int>::max() / 4;

inline bool isInfiniteDuration(int ticks) {
    return ticks < 0 || ticks >= kInfiniteDurationThreshold;
}

// Whole seconds left, rounded up, so the counter only reads 0:00 once the
// effect has actually ended. Returns -1 for endless effects. The arithmetic is
// done in 64 bits so an INT32_MAX duration cannot overflow.
inline int secondsRemaining(int ticks) {
    if (isInfiniteDuration(ticks)) return -1;
    const auto value = (static_cast<std::int64_t>(ticks) + kTicksPerSecond - 1) / kTicksPerSecond;
    return static_cast<int>(std::max<std::int64_t>(0, value));
}

// Converts an effect level to a Roman numeral, matching how Minecraft labels
// potency. 4000 and above has no Roman representation, so it falls back to
// digits.
inline std::string romanNumeral(int value) {
    if (value <= 0) return {};
    if (value > 3999) return std::to_string(value);
    static constexpr std::array<std::pair<int, const char*>, 13> numerals{{
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"},
        {100, "C"},  {90, "XC"},  {50, "L"},  {40, "XL"},
        {10, "X"},   {9, "IX"},   {5, "V"},   {4, "IV"},
        {1, "I"}
    }};
    std::string result;
    for (const auto& [amount, text] : numerals) {
        while (value >= amount) {
            result += text;
            value -= amount;
        }
    }
    return result;
}

// Level suffix appended to the effect name. `amplifier` is Bedrock's raw
// amplifier value, so the displayed level is amplifier + 1. A negative
// amplifier means the level is unknown and nothing is appended, which is how
// the HUD avoids ever printing a guessed level.
inline std::string levelSuffix(int amplifier, bool roman = true, bool hideLevelOne = false) {
    if (amplifier < 0) return {};
    const int level = amplifier + 1;
    if (level == 1 && hideLevelOne) return {};
    return roman ? romanNumeral(level) : std::to_string(level);
}

// "m:ss", "h:mm:ss" for long effects, or "infinite".
inline std::string formatDuration(int ticks) {
    const int seconds = secondsRemaining(ticks);
    if (seconds < 0) return "infinite";
    const int hours = seconds / 3600;
    const int minutes = (seconds / 60) % 60;
    const int remaining = seconds % 60;
    char output[24]{};
    if (hours > 0) std::snprintf(output, sizeof(output), "%d:%02d:%02d", hours, minutes, remaining);
    else std::snprintf(output, sizeof(output), "%d:%02d", minutes, remaining);
    return output;
}

// Colors the remaining-time text by urgency. Below ten seconds the text pulses
// so an about-to-expire effect is hard to miss. `pulsePhase` is a small
// accumulated phase in radians, not an absolute timestamp, so the oscillation
// stays smooth and precise no matter how long the session has been running.
inline std::uint32_t durationColor(int ticks, float pulsePhase, float& alphaOut) {
    alphaOut = 1.0f;
    const int seconds = secondsRemaining(ticks);
    if (seconds < 0) return 0xFF7FE8E0;      // endless
    if (seconds >= 60) return 0xFFD8D8D8;    // plenty of time
    if (seconds >= 10) return 0xFFFFC94D;    // running out
    alphaOut = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(pulsePhase));
    return 0xFFFF5A5A;                       // about to expire
}

} // namespace bedrocktools::hud::effects
