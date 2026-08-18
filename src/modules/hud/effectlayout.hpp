#pragma once

// ---------------------------------------------------------------------------
// MobEffectInstance memory-layout resolver for the Effect Display module.
//
// Bedrock keeps the active status effects of an entity in an EnTT component
// that owns a `std::vector<MobEffectInstance>`. The instance itself looks like
// this on modern builds (1.20.60+):
//
//     0x00  uint                           mId
//     0x04  EffectDuration                 mDuration            (int)
//     0x08  std::optional<EffectDuration>  mDurationEasy        (int + bool)
//     0x10  std::optional<EffectDuration>  mDurationNormal      (int + bool)
//     0x18  std::optional<EffectDuration>  mDurationHard        (int + bool)
//     0x20  int                            mAmplifier
//     0x24  bool                           mDisplayOnScreenTextureAnimation
//     0x25  bool                           mIsCounterPausedThisTick
//     0x26  bool                           mAmbient
//     0x27  bool                           mEffectVisible
//     0x28  MobEffect::FactorCalculationData             (tail, size varies)
//
// Older builds stored the three difficulty durations as plain ints, which put
// `mAmplifier` at 0x14 instead. The tail size also differs between platforms
// and game versions (it embeds a std::function), so the total stride is not
// stable either.
//
// The previous implementation scanned for "an int that looks like a small
// number", which very often locked onto `mDurationNormal` (whose value is a
// perfectly plausible amplifier) and therefore rendered a wrong effect level.
// This resolver instead validates the *whole* record: the optional flags, the
// four booleans that must follow the amplifier, duration sanity and id sanity.
// When the evidence is ambiguous it reports "amplifier unknown" so the HUD can
// hide the level instead of printing a wrong one.
//
// A single active effect used to fail resolution: scoreStride() did not award
// enough points for one id+duration pair, and the stride search returned an
// empty layout. The scorer now treats one well-formed record as sufficient,
// and resolveLayout() falls back to a documented modern Bedrock stride when
// there are not enough samples to vote.
//
// The header is intentionally dependency-free so it can be unit tested and
// reused by preview tooling.
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bedrocktools::hud::effects {

// Amplifier value used when the layout resolver could not identify the field
// with enough confidence. The HUD hides the level in that case.
inline constexpr int kUnknownAmplifier = -1;

struct EffectRecord {
    std::uint32_t id = 0;
    int durationTicks = 0;
    int amplifier = kUnknownAmplifier;
};

struct InstanceLayout {
    std::size_t stride = 0;            // sizeof(MobEffectInstance)
    std::size_t amplifierOffset = 0;   // offsetof(MobEffectInstance, mAmplifier)
    bool hasAmplifier = false;         // false => amplifier could not be trusted
    bool optionalDurations = false;    // modern std::optional difficulty durations
    int score = 0;

    constexpr bool valid() const { return stride != 0; }
};

namespace detail {

inline constexpr std::size_t kMaxRecords = 128;
inline constexpr std::size_t kMinStride = 0x28;
inline constexpr std::size_t kMaxStride = 0x100;

// Known-good amplifier offsets, most likely first. A candidate that matches one
// of these gets a bonus and wins ties, which keeps the resolver stable when two
// offsets happen to hold identical values.
inline constexpr std::array<std::size_t, 2> kKnownAmplifierOffsets{{0x20, 0x14}};

inline std::int32_t readInt(const std::uint8_t* base, std::size_t offset) {
    std::int32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

inline std::uint32_t readU32(const std::uint8_t* base, std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

inline std::uint8_t readByte(const std::uint8_t* base, std::size_t offset) {
    return base[offset];
}

inline bool isBooleanByte(std::uint8_t value) {
    return value <= 1;
}

// EffectDuration is a plain int. Bedrock stores the infinite duration as
// INT32_MAX; -1 shows up on some builds and in packets.
inline constexpr std::int32_t kInfiniteDuration = 2147483647;

inline bool plausibleDuration(std::int32_t ticks) {
    return ticks == -1 || ticks == kInfiniteDuration || (ticks >= 0 && ticks <= 2'000'000'000);
}

inline bool plausibleAmplifier(std::int32_t amplifier) {
    return amplifier >= 0 && amplifier <= 255;
}

inline bool knownAmplifierOffset(std::size_t offset) {
    for (const auto known : kKnownAmplifierOffsets) {
        if (offset == known) return true;
    }
    return false;
}

// True when the 8-byte header (mId + mDuration) of one record looks like a
// MobEffectInstance. Used by the single-record fallback when there are not
// enough samples for scoreStride() to vote.
inline bool looksLikeEffectHeader(const std::uint8_t* record) {
    const auto id = readU32(record, 0);
    if (id < 1 || id > 255) return false;
    return plausibleDuration(readInt(record, 4));
}

// Scores how well `stride` explains the buffer, using only the two fields whose
// offsets never move (mId at 0x00 and mDuration at 0x04).
//
// Weights are intentionally high: a lone potion must produce a clearly
// positive score. The previous 8+4 scheme left a single record (especially a
// modded / high id) too close to the rejection threshold, so the HUD stayed
// empty until two or three effects were active.
inline int scoreStride(const std::uint8_t* data, std::size_t count, std::size_t stride) {
    int score = 0;
    int active = 0;
    std::uint32_t seen[kMaxRecords]{};
    std::size_t seenCount = 0;

    for (std::size_t index = 0; index < count; ++index) {
        const auto* record = data + index * stride;
        const auto id = readU32(record, 0);
        const auto duration = readInt(record, 4);

        // Bedrock keeps a "no effect" placeholder (id 0) in the vector when an
        // effect slot is empty or has just been removed/expired, and
        // readRecords() skips those. They carry no evidence for or against a
        // stride, so ignore them here instead of rejecting the whole buffer.
        if (id == 0) continue;

        if (id >= 1 && id <= 64) score += 16;         // vanilla effect id range
        else if (id >= 65 && id <= 255) score += 10;  // modded / newer vanilla id
        else return -1000;                            // definitely not an id

        if (plausibleDuration(duration)) score += 8;
        else return -1000;

        // A matching id + duration pair is a complete instance header. Award
        // it even when this is the only record in the vector.
        score += 8;

        // Effects are unique per entity, so duplicate ids mean a wrong stride.
        for (std::size_t i = 0; i < seenCount; ++i) {
            if (seen[i] == id) score -= 24;
        }
        if (seenCount < kMaxRecords) seen[seenCount++] = id;

        if (duration != 0) ++active;
    }

    if (active == 0) score -= 8;
    // One confirmed live record is enough evidence; do not require a crowd
    // of effects before the HUD will light up.
    if (seenCount == 1 && active >= 1) score += 12;
    return score;
}

// Observed sizeof(MobEffectInstance) on modern (1.20.60+) Bedrock builds.
// The FactorCalculationData tail embeds a std::function, so the size depends
// on the STL / ABI; these are the sizes we have actually seen.
inline constexpr std::array<std::size_t, 8> kModernFallbackStrides{{
    0xA0, 0x98, 0x90, 0x88, 0x80, 0x78, 0x70, 0x68
}};

// Picks a stride when the voting loop had too few samples (or none that
// scored). Prefers a documented modern size; if the buffer itself is one
// plausible instance, uses `bytes` even when that length is not a multiple
// of 8 (the main search only steps by 8).
inline std::size_t fallbackStride(const std::uint8_t* data, std::size_t bytes) {
    if (data == nullptr || bytes < kMinStride) return 0;

    auto consider = [&](std::size_t stride) -> bool {
        if (stride < kMinStride || stride > kMaxStride) return false;
        if (bytes % stride != 0) return false;
        const auto count = bytes / stride;
        if (count == 0 || count > kMaxRecords) return false;
        if (!looksLikeEffectHeader(data)) return false;
        for (std::size_t index = 1; index < count; ++index) {
            const auto* record = data + index * stride;
            if (readU32(record, 0) == 0) continue;
            if (!looksLikeEffectHeader(record)) return false;
        }
        return true;
    };

    std::size_t found = 0;
    for (const auto stride : kModernFallbackStrides) {
        if (!consider(stride)) continue;
        // Smallest matching modern size wins: a multiple of the real stride
        // also "fits" but would skip every other record.
        if (found == 0 || stride < found) found = stride;
    }
    if (found != 0) return found;
    if (consider(bytes)) return bytes;
    return 0;
}

// The amplifier can only sit directly after the id, the duration and the three
// optional difficulty durations, so its offset is tightly bounded:
//   * modern builds  (std::optional<EffectDuration> x3): exactly 0x20
//   * older builds   (plain int difficulty durations)  : 0x08 .. 0x14
inline constexpr std::size_t kOptionalAmplifierOffset = 0x20;
inline constexpr std::size_t kMinPlainAmplifierOffset = 0x08;
inline constexpr std::size_t kMaxPlainAmplifierOffset = 0x14;

// Scores a candidate amplifier offset. The decisive signals are the four
// booleans that directly follow mAmplifier and, on modern builds, the
// "engaged" byte of each std::optional difficulty duration. Every record
// contributes the same maximum number of points regardless of the offset, so a
// far-away offset in a zero-filled tail cannot out-score the real field.
inline int scoreAmplifier(
    const std::uint8_t* data,
    std::size_t count,
    std::size_t stride,
    std::size_t amplifierOffset,
    bool optionalDurations
) {
    if (amplifierOffset + 8 > stride) return -1000;
    if (optionalDurations) {
        if (amplifierOffset != kOptionalAmplifierOffset) return -1000;
    } else if (amplifierOffset < kMinPlainAmplifierOffset || amplifierOffset > kMaxPlainAmplifierOffset) {
        return -1000;
    }

    int score = 0;
    bool anyLevelAboveOne = false;
    for (std::size_t index = 0; index < count; ++index) {
        const auto* record = data + index * stride;
        // Skip the same "no effect" (id 0) placeholders that scoreStride() and
        // readRecords() ignore; their tail bytes may not look like an effect.
        if (readU32(record, 0) == 0) continue;

        const auto duration = readInt(record, 4);
        const auto amplifier = readInt(record, amplifierOffset);

        if (!plausibleAmplifier(amplifier)) return -1000;
        if (amplifier <= 7) score += 10;        // realistic potion levels
        else if (amplifier <= 31) score += 2;   // command-applied levels
        else score -= 4;
        if (amplifier > 0) anyLevelAboveOne = true;

        // mDisplayOnScreenTextureAnimation / mIsCounterPausedThisTick /
        // mAmbient / mEffectVisible.
        for (std::size_t i = 4; i < 8; ++i) {
            if (!isBooleanByte(readByte(record, amplifierOffset + i))) return -1000;
            score += 3;
        }

        if (optionalDurations) {
            // mDurationEasy / mDurationNormal / mDurationHard: three fixed
            // {int value; bool engaged;} slots between duration and amplifier.
            for (std::size_t offset = 0x08; offset < kOptionalAmplifierOffset; offset += 8) {
                const auto engaged = readByte(record, offset + 4);
                if (!isBooleanByte(engaged)) return -1000;
                if (engaged && !plausibleDuration(readInt(record, offset))) return -1000;
                score += 4;
            }
        } else {
            // Every int between the duration and the amplifier must itself
            // look like a difficulty duration.
            for (std::size_t offset = 0x08; offset < amplifierOffset; offset += 4) {
                if (!plausibleDuration(readInt(record, offset))) return -1000;
                score += 2;
            }
        }

        // A field that mirrors the duration is almost certainly a duration.
        if (duration != 0 && amplifier == duration) score -= 14;
    }

    // A field that is zero for every effect carries no evidence of its own, so
    // only the structural checks above should decide. Offsets that actually
    // show a level get a small edge.
    if (anyLevelAboveOne) score += 6;
    if (knownAmplifierOffset(amplifierOffset)) score += 20;
    return score;
}

inline std::vector<int> amplifiersFor(
    const std::uint8_t* data,
    std::size_t count,
    std::size_t stride,
    std::size_t amplifierOffset
) {
    std::vector<int> values;
    values.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        values.push_back(readInt(data + index * stride, amplifierOffset));
    }
    return values;
}

} // namespace detail

// Resolves the layout of the MobEffectInstance array at `data` (`bytes` long).
// Returns an invalid layout when the buffer does not look like an effect list.
inline InstanceLayout resolveLayout(const std::uint8_t* data, std::size_t bytes) {
    InstanceLayout result{};
    if (data == nullptr || bytes == 0) return result;

    // ---- 1. Stride -------------------------------------------------------
    std::size_t bestStride = 0;
    int bestStrideScore = 0;
    auto considerStride = [&](std::size_t stride) {
        if (stride < detail::kMinStride || stride > detail::kMaxStride) return;
        if (bytes % stride != 0) return;
        const auto count = bytes / stride;
        if (count == 0 || count > detail::kMaxRecords) return;

        // Normalize by record count so a stride that happens to fit more
        // records does not win purely on volume.
        const int total = detail::scoreStride(data, count, stride);
        if (total <= 0) return;
        const int normalized = static_cast<int>(total / static_cast<int>(count));
        // Strides are tried in ascending order and ties keep the smallest one.
        // Any multiple of the real stride also "fits" the buffer (it just skips
        // records), so the smallest valid stride is the correct one.
        if (normalized > bestStrideScore) {
            bestStrideScore = normalized;
            bestStride = stride;
        }
    };

    for (std::size_t stride = detail::kMinStride; stride <= detail::kMaxStride; stride += 8) {
        considerStride(stride);
    }
    // A single MobEffectInstance is exactly `bytes` long. That size is not
    // always a multiple of 8 (the search above only steps by 8), so give the
    // exact length a vote of its own.
    if (bytes % 8 != 0) considerStride(bytes);

    // Too few samples (or none that scored): fall back to a documented modern
    // Bedrock stride instead of returning an empty layout and stalling the HUD.
    if (bestStride == 0) {
        bestStride = detail::fallbackStride(data, bytes);
        if (bestStride == 0) return result;
        const auto count = bytes / bestStride;
        const int total = detail::scoreStride(data, count, bestStride);
        bestStrideScore = total > 0
            ? static_cast<int>(total / static_cast<int>(count))
            : 1;
    }

    result.stride = bestStride;
    result.score = bestStrideScore;

    // ---- 2. Amplifier ----------------------------------------------------
    const auto count = bytes / bestStride;
    struct Candidate {
        std::size_t offset = 0;
        bool optionalDurations = false;
        int score = 0;
    };

    Candidate best{};
    std::vector<Candidate> tied;
    for (const bool optionalDurations : {true, false}) {
        const std::size_t first = optionalDurations ? detail::kOptionalAmplifierOffset
                                                    : detail::kMinPlainAmplifierOffset;
        const std::size_t last = optionalDurations ? detail::kOptionalAmplifierOffset
                                                   : detail::kMaxPlainAmplifierOffset;
        for (std::size_t offset = first; offset <= last; offset += 4) {
            if (offset + 8 > bestStride) break;
            const int score = detail::scoreAmplifier(data, count, bestStride, offset, optionalDurations);
            if (score <= 0) continue;
            if (score > best.score) {
                best = Candidate{offset, optionalDurations, score};
                tied.clear();
                tied.push_back(best);
            } else if (score == best.score) {
                tied.push_back(Candidate{offset, optionalDurations, score});
            }
        }
    }

    if (best.score <= 0) return result;   // keep stride, hide the level

    // Several offsets can score identically. That is harmless when they all
    // hold the same values; otherwise prefer a documented offset, and if none
    // of the tied candidates is documented, report the amplifier as unknown
    // rather than showing a wrong level.
    const auto reference = detail::amplifiersFor(data, count, bestStride, best.offset);
    bool ambiguous = false;
    for (const auto& candidate : tied) {
        if (candidate.offset == best.offset) continue;
        if (detail::amplifiersFor(data, count, bestStride, candidate.offset) != reference) {
            ambiguous = true;
            break;
        }
    }

    if (ambiguous) {
        bool resolved = false;
        for (const auto known : detail::kKnownAmplifierOffsets) {
            for (const auto& candidate : tied) {
                if (candidate.offset == known) {
                    best = candidate;
                    resolved = true;
                    break;
                }
            }
            if (resolved) break;
        }
        if (!resolved) return result;
    }

    result.amplifierOffset = best.offset;
    result.optionalDurations = best.optionalDurations;
    result.hasAmplifier = true;
    result.score += best.score;
    return result;
}

// Cheap re-check of an already-resolved layout against a new buffer. This runs
// every game tick, so it deliberately avoids the full offset search: it only
// confirms that the known stride and amplifier offset still describe the data.
inline bool validateLayout(const std::uint8_t* data, std::size_t bytes, const InstanceLayout& layout) {
    if (!layout.valid() || data == nullptr || bytes == 0) return false;
    if (bytes % layout.stride != 0) return false;

    const auto count = bytes / layout.stride;
    if (count == 0 || count > detail::kMaxRecords) return false;
    const int strideScore = detail::scoreStride(data, count, layout.stride);
    if (strideScore <= 0) {
        // A fallback layout is accepted on a single well-formed header so
        // one active effect cannot drop the HUD back to "No Effects".
        if (count != 1 || !detail::looksLikeEffectHeader(data)) return false;
    }
    if (!layout.hasAmplifier) return true;

    return detail::scoreAmplifier(
               data, count, layout.stride, layout.amplifierOffset, layout.optionalDurations
           ) > 0;
}

// Reads every active effect described by `layout` out of the buffer.
inline std::vector<EffectRecord> readRecords(
    const std::uint8_t* data,
    std::size_t bytes,
    const InstanceLayout& layout
) {
    std::vector<EffectRecord> records;
    if (!layout.valid() || data == nullptr || layout.stride == 0) return records;

    const auto count = bytes / layout.stride;
    records.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const auto* record = data + index * layout.stride;
        EffectRecord entry{};
        entry.id = detail::readU32(record, 0);
        entry.durationTicks = detail::readInt(record, 4);
        entry.amplifier = layout.hasAmplifier
            ? detail::readInt(record, layout.amplifierOffset)
            : kUnknownAmplifier;

        if (entry.id == 0 || entry.id > 255) continue;
        if (entry.durationTicks == 0) continue;
        if (layout.hasAmplifier && !detail::plausibleAmplifier(entry.amplifier)) continue;
        records.push_back(entry);
    }
    return records;
}

} // namespace bedrocktools::hud::effects
