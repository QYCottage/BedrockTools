// Unit tests for the MobEffectInstance layout resolver used by the
// Effect Display HUD module.
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src tests/effectlayout_test.cpp -o /tmp/effectlayout_test
//     /tmp/effectlayout_test

#include "modules/hud/effectlayout.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace effects = bedrocktools::hud::effects;

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

struct Buffer {
    std::vector<std::uint8_t> bytes;
    std::size_t stride = 0;

    explicit Buffer(std::size_t recordStride, std::size_t count)
        : bytes(recordStride * count, 0), stride(recordStride) {}

    void putInt(std::size_t index, std::size_t offset, std::int32_t value) {
        std::memcpy(bytes.data() + index * stride + offset, &value, sizeof(value));
    }
    void putByte(std::size_t index, std::size_t offset, std::uint8_t value) {
        bytes[index * stride + offset] = value;
    }
    const std::uint8_t* data() const { return bytes.data(); }
    std::size_t size() const { return bytes.size(); }
};

struct Effect {
    std::uint32_t id;
    std::int32_t duration;
    std::int32_t amplifier;
};

// Modern (1.20.60+) instance:
//   0x00 id | 0x04 duration | 0x08/0x10/0x18 optional difficulty durations
//   0x20 amplifier | 0x24..0x27 four bools | 0x28.. FactorCalculationData tail
Buffer makeModern(const std::vector<Effect>& list, std::size_t stride = 0x88, bool difficultySet = false) {
    Buffer buffer(stride, list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        buffer.putInt(i, 0x00, static_cast<std::int32_t>(list[i].id));
        buffer.putInt(i, 0x04, list[i].duration);
        for (std::size_t slot = 0x08; slot <= 0x18; slot += 8) {
            buffer.putInt(i, slot, difficultySet ? list[i].duration : 0);
            buffer.putByte(i, slot + 4, difficultySet ? 1 : 0);
        }
        buffer.putInt(i, 0x20, list[i].amplifier);
        buffer.putByte(i, 0x24, 1);   // mDisplayOnScreenTextureAnimation
        buffer.putByte(i, 0x25, 0);   // mIsCounterPausedThisTick
        buffer.putByte(i, 0x26, 0);   // mAmbient
        buffer.putByte(i, 0x27, 1);   // mEffectVisible
        // Plausible garbage in the FactorCalculationData tail.
        for (std::size_t offset = 0x28; offset + 4 <= stride; offset += 4) {
            buffer.putInt(i, offset, static_cast<std::int32_t>(0x40000000 + offset * 7 + i));
        }
    }
    return buffer;
}

// Legacy instance: three plain-int difficulty durations, amplifier at 0x14.
Buffer makeLegacy(const std::vector<Effect>& list, std::size_t stride = 0x68) {
    Buffer buffer(stride, list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        buffer.putInt(i, 0x00, static_cast<std::int32_t>(list[i].id));
        buffer.putInt(i, 0x04, list[i].duration);
        buffer.putInt(i, 0x08, list[i].duration);
        buffer.putInt(i, 0x0C, list[i].duration);
        buffer.putInt(i, 0x10, list[i].duration);
        buffer.putInt(i, 0x14, list[i].amplifier);
        buffer.putByte(i, 0x18, 1);
        buffer.putByte(i, 0x19, 0);
        buffer.putByte(i, 0x1A, 0);
        buffer.putByte(i, 0x1B, 1);
        for (std::size_t offset = 0x1C; offset + 4 <= stride; offset += 4) {
            buffer.putInt(i, offset, static_cast<std::int32_t>(0x50000000 + offset * 3 + i));
        }
    }
    return buffer;
}

void expectEffects(const Buffer& buffer, const std::vector<Effect>& expected, const std::string& label) {
    const auto layout = effects::resolveLayout(buffer.data(), buffer.size());
    check(layout.valid(), label + ": layout resolved");
    check(layout.stride == buffer.stride,
          label + ": stride " + std::to_string(layout.stride) + " == " + std::to_string(buffer.stride));
    check(layout.hasAmplifier, label + ": amplifier field identified");
    if (!layout.valid()) return;

    const auto records = effects::readRecords(buffer.data(), buffer.size(), layout);
    check(records.size() == expected.size(),
          label + ": " + std::to_string(records.size()) + " records read");
    for (std::size_t i = 0; i < records.size() && i < expected.size(); ++i) {
        check(records[i].id == expected[i].id, label + ": id[" + std::to_string(i) + "]");
        check(records[i].durationTicks == expected[i].duration, label + ": duration[" + std::to_string(i) + "]");
        check(records[i].amplifier == expected[i].amplifier,
              label + ": amplifier[" + std::to_string(i) + "] = " +
                  std::to_string(records[i].amplifier) + " (want " + std::to_string(expected[i].amplifier) + ")");
    }
}

} // namespace

int main() {
    const std::vector<Effect> potions{
        {1, 3600, 1},    // Speed II
        {10, 900, 0},    // Regeneration I
        {12, 6000, 0},   // Fire Resistance I
        {21, 2400, 3},   // Health Boost IV
    };

    std::printf("modern layout (amplifier at 0x20)\n");
    expectEffects(makeModern(potions), potions, "modern");

    std::printf("modern layout, difficulty durations set\n");
    expectEffects(makeModern(potions, 0x88, true), potions, "modern-difficulty");

    std::printf("modern layout, 0x68 stride (libc++ std::function)\n");
    expectEffects(makeModern(potions, 0x68), potions, "modern-0x68");

    std::printf("legacy layout (amplifier at 0x14)\n");
    expectEffects(makeLegacy(potions), potions, "legacy");

    std::printf("single high-level effect\n");
    const std::vector<Effect> single{{5, 2'147'483'647, 9}};   // Strength X, infinite
    expectEffects(makeModern(single), single, "single");

    // Regression: with a zero-filled FactorCalculationData tail, plenty of
    // offsets hold a "plausible" amplifier of 0. The resolver must still pick
    // mAmplifier, otherwise every effect renders as level I.
    std::printf("zero-filled tail still finds the amplifier\n");
    {
        for (const int amplifier : {0, 1, 4, 9}) {
            Buffer buffer(0x88, 2);
            const std::uint32_t ids[]{1, 12};
            const std::int32_t durations[]{3600, 6000};
            for (std::size_t i = 0; i < 2; ++i) {
                buffer.putInt(i, 0x00, static_cast<std::int32_t>(ids[i]));
                buffer.putInt(i, 0x04, durations[i]);
                buffer.putInt(i, 0x20, amplifier);
                buffer.putByte(i, 0x24, 1);
                buffer.putByte(i, 0x27, 1);
                // tail left as zeros
            }
            const auto layout = effects::resolveLayout(buffer.data(), buffer.size());
            check(layout.hasAmplifier && layout.amplifierOffset == 0x20,
                  "zero tail, amplifier " + std::to_string(amplifier) + ": offset 0x" +
                      std::to_string(layout.amplifierOffset));
            const auto records = effects::readRecords(buffer.data(), buffer.size(), layout);
            check(records.size() == 2, "zero tail, amplifier " + std::to_string(amplifier) + ": 2 records");
            for (const auto& record : records) {
                check(record.amplifier == amplifier,
                      "zero tail: amplifier read back as " + std::to_string(record.amplifier));
            }
        }
    }

    // Regression: a beacon grants endless effects, whose duration is INT32_MAX.
    std::printf("endless (beacon) effects keep their level\n");
    {
        const std::vector<Effect> beacon{{1, 2'147'483'647, 1}, {3, 2'147'483'647, 1}};
        expectEffects(makeModern(beacon), beacon, "beacon");
    }

    std::printf("layout revalidation\n");
    {
        const auto buffer = makeModern(potions);
        const auto layout = effects::resolveLayout(buffer.data(), buffer.size());
        check(effects::validateLayout(buffer.data(), buffer.size(), layout), "same buffer revalidates");

        // A shorter list of the same shape (one effect expired) must still
        // validate against the cached layout.
        const auto shorter = makeModern({potions[0], potions[1]});
        check(effects::validateLayout(shorter.data(), shorter.size(), layout),
              "shorter list of the same build revalidates");

        // A buffer from a build with a different stride must not.
        const auto other = makeLegacy(potions);
        check(!effects::validateLayout(other.data(), other.size(), layout),
              "different stride fails revalidation");

        std::vector<std::uint8_t> junk(buffer.size());
        for (std::size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::uint8_t>(0x80 + (i * 91) % 0x7F);
        check(!effects::validateLayout(junk.data(), junk.size(), layout), "garbage fails revalidation");
    }

    std::printf("expired effects are skipped\n");
    {
        std::vector<Effect> mixed{{1, 3600, 1}, {10, 0, 0}, {19, 400, 2}};
        const auto buffer = makeModern(mixed);
        const auto layout = effects::resolveLayout(buffer.data(), buffer.size());
        const auto records = effects::readRecords(buffer.data(), buffer.size(), layout);
        check(records.size() == 2, "zero-duration entry dropped");
        if (records.size() == 2) {
            check(records[0].id == 1 && records[0].amplifier == 1, "first surviving record");
            check(records[1].id == 19 && records[1].amplifier == 2, "second surviving record");
        }
    }

    // Regression: Bedrock keeps a "no effect" placeholder (id 0) in the vector
    // when an effect slot has just been removed or expired. The resolver must
    // skip those the same way readRecords() does, otherwise a single placeholder
    // makes the whole HUD stop reading effects (they appear "stuck"/untracked).
    std::printf("id-0 no-effect placeholders are skipped, not fatal\n");
    {
        // Two real effects plus one id-0 placeholder whose tail is garbage, so
        // only the id-0 skip (not a lucky tail) can make this resolve.
        Buffer buffer(0x88, 3);
        const std::uint32_t ids[]{1, 10, 0};
        const std::int32_t durations[]{3600, 900, 0};
        const std::int32_t amplifiers[]{1, 0, 0};
        for (std::size_t i = 0; i < 3; ++i) {
            buffer.putInt(i, 0x00, static_cast<std::int32_t>(ids[i]));
            buffer.putInt(i, 0x04, durations[i]);
            buffer.putInt(i, 0x20, amplifiers[i]);
            buffer.putByte(i, 0x24, 1);
            buffer.putByte(i, 0x27, 1);
            for (std::size_t offset = 0x28; offset + 4 <= buffer.stride; offset += 4) {
                // id 0 placeholder gets garbage tail bytes.
                buffer.putInt(i, offset, static_cast<std::int32_t>(ids[i] == 0 ? 0xDEADBEEF : (0x40000000 + offset * 7 + i)));
            }
        }
        const auto layout = effects::resolveLayout(buffer.data(), buffer.size());
        check(layout.valid(), "id-0 placeholder: layout still resolves");
        if (layout.valid()) {
            check(layout.stride == 0x88, "id-0 placeholder: stride is the real one");
            const auto records = effects::readRecords(buffer.data(), buffer.size(), layout);
            check(records.size() == 2, "id-0 placeholder: only the 2 real effects are read");
            if (records.size() == 2) {
                check(records[0].id == 1 && records[1].id == 10,
                      "id-0 placeholder: Speed and Regeneration survive, placeholder skipped");
            }
            check(effects::validateLayout(buffer.data(), buffer.size(), layout),
                  "id-0 placeholder: cached layout revalidates");
        }
    }

    std::printf("garbage buffer is rejected\n");
    {
        std::vector<std::uint8_t> junk(0x88 * 3);
        for (std::size_t i = 0; i < junk.size(); ++i) junk[i] = static_cast<std::uint8_t>(0x80 + (i * 37) % 0x7F);
        const auto layout = effects::resolveLayout(junk.data(), junk.size());
        check(!layout.valid() || !layout.hasAmplifier, "garbage: no amplifier reported");
    }

    std::printf("empty buffer is rejected\n");
    {
        const auto layout = effects::resolveLayout(nullptr, 0);
        check(!layout.valid(), "empty: invalid layout");
    }

    if (g_failures == 0) {
        std::printf("\nall checks passed\n");
        return 0;
    }
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
}
