// Unit tests for the Effect Display formatting helpers (level, duration,
// urgency color).
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src tests/effectformat_test.cpp -o /tmp/effectformat_test
//     /tmp/effectformat_test

#include "modules/hud/effectformat.hpp"

#include <cstdio>
#include <limits>
#include <string>

namespace fx = bedrocktools::hud::effects;

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::printf("  ok   %s\n", what.c_str());
    } else {
        std::printf("  FAIL %s\n", what.c_str());
        ++g_failures;
    }
}

void checkEqual(const std::string& got, const std::string& want, const std::string& what) {
    check(got == want, what + " -> \"" + got + "\" (want \"" + want + "\")");
}

void checkEqual(int got, int want, const std::string& what) {
    check(got == want, what + " -> " + std::to_string(got) + " (want " + std::to_string(want) + ")");
}

} // namespace

int main() {
    std::printf("level suffix (amplifier -> displayed level)\n");
    checkEqual(fx::levelSuffix(0), "I", "amplifier 0");
    checkEqual(fx::levelSuffix(1), "II", "amplifier 1");
    checkEqual(fx::levelSuffix(2), "III", "amplifier 2");
    checkEqual(fx::levelSuffix(3), "IV", "amplifier 3");
    checkEqual(fx::levelSuffix(4), "V", "amplifier 4");
    checkEqual(fx::levelSuffix(8), "IX", "amplifier 8");
    checkEqual(fx::levelSuffix(9), "X", "amplifier 9");
    checkEqual(fx::levelSuffix(13), "XIV", "amplifier 13 (was broken before: >20 fell back to digits)");
    checkEqual(fx::levelSuffix(49), "L", "amplifier 49");
    checkEqual(fx::levelSuffix(254), "CCLV", "amplifier 254 (max /effect level)");
    checkEqual(fx::levelSuffix(255), "CCLVI", "amplifier 255");

    std::printf("level suffix options\n");
    checkEqual(fx::levelSuffix(4, /*roman=*/false), "5", "arabic numerals");
    checkEqual(fx::levelSuffix(0, /*roman=*/true, /*hideLevelOne=*/true), "", "hide level I");
    checkEqual(fx::levelSuffix(1, /*roman=*/true, /*hideLevelOne=*/true), "II", "hide level I keeps level II");
    checkEqual(fx::levelSuffix(-1), "", "unknown amplifier prints nothing");
    checkEqual(fx::levelSuffix(-5), "", "negative amplifier prints nothing");

    std::printf("seconds remaining (rounded up)\n");
    checkEqual(fx::secondsRemaining(0), 0, "0 ticks");
    checkEqual(fx::secondsRemaining(1), 1, "1 tick still shows 1s");
    checkEqual(fx::secondsRemaining(19), 1, "19 ticks");
    checkEqual(fx::secondsRemaining(20), 1, "20 ticks");
    checkEqual(fx::secondsRemaining(21), 2, "21 ticks");
    checkEqual(fx::secondsRemaining(600), 30, "600 ticks");
    checkEqual(fx::secondsRemaining(-1), -1, "-1 means endless");
    checkEqual(fx::secondsRemaining(std::numeric_limits<int>::max()), -1, "INT32_MAX means endless");

    std::printf("duration formatting\n");
    checkEqual(fx::formatDuration(0), "0:00", "expired");
    checkEqual(fx::formatDuration(20 * 5), "0:05", "5 seconds");
    checkEqual(fx::formatDuration(20 * 65), "1:05", "1:05");
    checkEqual(fx::formatDuration(20 * 3 * 60), "3:00", "3 minutes");
    checkEqual(fx::formatDuration(20 * 8 * 60), "8:00", "8 minutes (potion of Fire Resistance)");
    checkEqual(fx::formatDuration(20 * (3600 + 2 * 60 + 3)), "1:02:03", "over an hour");
    checkEqual(fx::formatDuration(std::numeric_limits<int>::max()), "infinite", "endless");
    checkEqual(fx::formatDuration(-1), "infinite", "endless (-1)");

    std::printf("countdown color\n");
    {
        float alpha = 0.0f;
        check(fx::durationColor(std::numeric_limits<int>::max(), 0.0f, alpha) == 0xFF7FE8E0, "endless is teal");
        check(alpha == 1.0f, "endless is fully opaque");
        check(fx::durationColor(20 * 120, 0.0f, alpha) == 0xFFD8D8D8, ">= 60s is white");
        check(fx::durationColor(20 * 30, 0.0f, alpha) == 0xFFFFC94D, "10..59s is amber");
        check(fx::durationColor(20 * 5, 0.0f, alpha) == 0xFFFF5A5A, "< 10s is red");
        check(alpha < 1.0f, "< 10s pulses");
        check(fx::durationColor(20 * 10, 0.0f, alpha) == 0xFFFFC94D, "exactly 10s is amber");
    }

    if (g_failures == 0) {
        std::printf("\nall checks passed\n");
        return 0;
    }
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
}
