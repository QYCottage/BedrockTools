#pragma once

// ---------------------------------------------------------------------------
// Signature-free locator for HudScreen::_renderStatusEffects
// ---------------------------------------------------------------------------
// Bedrock draws the vanilla status-effect (potion) bar through the custom UI
// renderer `mob_effects_renderer` (declared in the vanilla hud_screen.json and
// inventory screens as `"renderer": "mob_effects_renderer"`, visible while the
// `#status_effects_visible` binding is true). That renderer is the C++
// function HudScreen::_renderStatusEffects, which the Effect Display module
// hooks so the vanilla bar can be hidden while the module's own panel is
// shown.
//
// The reliable way to reach that function is a per-build byte pattern
// (SignatureId::RenderPotionEffects in src/core/memory/Signatures.cpp). This
// header provides a build-independent fallback for when no pattern is
// available: the renderer must reference the vanilla status-effect icon
// texture paths ("textures/ui/<effect>_effect"), so we scan the game library
// for ARM64 ADRP+ADD instruction pairs that compute those string addresses and
// cluster the hits into the function(s) containing them.
//
// The locator is deliberately conservative:
//   * it only reports a target when one function clearly dominates the
//     references (strictly more hits than any other candidate, several
//     distinct icons, a plausible code span);
//   * a candidate that is tiny and called from elsewhere is treated as an
//     icon-path helper, and the largest of its callers (the render loop) is
//     reported instead — an early-return detour may only be applied to a void
//     render callback, never to a value-returning leaf;
//   * callers of the module are expected to keep a warm-up gate in front of
//     the detour, so a wrong guess that is not called at frame rate (a
//     one-shot constructor, say) simply never suppresses anything.
// If nothing trustworthy is found the locator returns 0 and the vanilla bar
// stays visible.
//
// The header is intentionally dependency-free (no Android/game APIs) so it
// can be unit tested on the host; only the segment enumeration
// (collectModuleRanges in effectdisplay.cpp) is platform-specific.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bedrocktools::hud::effectlocator {

// Executable (code) and readable (rodata/data) address ranges of the game
// library, filled in by the caller (collectModuleRanges in effectdisplay.cpp).
struct ModuleRanges {
    std::uintptr_t textStart = 0;
    std::uintptr_t textEnd = 0;
    std::uintptr_t dataStart = 0;
    std::uintptr_t dataEnd = 0;
};

// Decodes an ARM64 ADRP instruction. On success imm21 is the sign-extended
// 21-bit page offset (already sign-extended to 32 bits) and rd the register.
// The 21-bit immediate is assembled from immhi (bits 23:5) and the two low
// bits held in bits 30:29 of the instruction word.
inline bool decodeAdrp(std::uint32_t insn, std::int32_t& imm21, std::uint32_t& rd) {
    if ((insn & 0x9F000000) != 0x90000000) return false;
    const std::uint32_t immhi = (insn >> 5) & 0x7FFFF;
    const std::uint32_t immlo = (insn >> 29) & 0x3;
    std::uint32_t imm = (immhi << 2) | immlo;
    if (imm & 0x100000) imm |= 0xFFE00000;   // sign extend 21 bits to 32
    imm21 = static_cast<std::int32_t>(imm);
    rd = insn & 31;
    return true;
}

// Decodes an ARM64 ADD (immediate, 64-bit, non-shifted): Xd = Xn + imm12.
inline bool decodeAddImm(std::uint32_t insn, std::uint32_t& imm12, std::uint32_t& rn, std::uint32_t& rd) {
    if ((insn & 0xFF800000) != 0x91000000) return false;
    imm12 = (insn >> 10) & 0xFFF;
    rn = (insn >> 5) & 31;
    rd = insn & 31;
    return true;
}

// Decodes an ARM64 BL (branch-and-link): target = pc + imm26<<2.
inline bool decodeBl(std::uint32_t insn, std::uintptr_t pc, std::uintptr_t& target) {
    if ((insn & 0xFC000000) != 0x94000000) return false;
    std::int32_t imm26 = static_cast<std::int32_t>(insn & 0x03FFFFFF);
    if (imm26 & 0x02000000) imm26 |= 0xFC000000;   // sign extend 26 bits
    target = pc + static_cast<std::uintptr_t>(static_cast<std::int64_t>(imm26) << 2);
    return true;
}

// True for the strong ARM64 prologue markers used to walk back from a code
// reference to the start of its function: pacibsp, a pre-index STP of a GP or
// FP/SIMD register pair to sp (the classic frame setup), or `sub sp, sp, #imm`.
// The masks were validated against real compiled ARM64 code: the 0xA9... and
// 0x6D... constants match exactly the pre-index (writeback) store-pair
// encodings with sp as the base, and nothing else (offset/post-index modes,
// LDP loads, 32-bit/128-bit pairs differ in the opc/mode bits).
inline bool isStrongPrologue(std::uint32_t insn) {
    if (insn == 0xD503233F) return true;                       // pacibsp
    if ((insn & 0xFFC003E0) == 0xA98003E0) return true;        // STP Xt, Xt2, [sp, #imm]!
    if ((insn & 0xFFC003E0) == 0x6D8003E0) return true;        // STP Dt, Dt2, [sp, #imm]!
    if ((insn & 0xFF8003FF) == 0xD10003FF) return true;        // SUB sp, sp, #imm
    return false;
}

// Walks backward from `pc` (which must lie inside the text range) for at most
// `maxInstructions` and returns the nearest strong prologue marker, or 0.
inline std::uintptr_t findFunctionStart(const ModuleRanges& ranges, std::uintptr_t pc, std::size_t maxInstructions) {
    if (pc < ranges.textStart || pc >= ranges.textEnd) return 0;
    const std::size_t index = (pc - ranges.textStart) / 4;
    for (std::size_t i = index; i > 0 && (index - i) < maxInstructions; --i) {
        std::uint32_t insn = 0;
        std::memcpy(&insn, reinterpret_cast<const void*>(ranges.textStart + (i - 1) * 4), sizeof(insn));
        if (isStrongPrologue(insn)) return ranges.textStart + (i - 1) * 4;
    }
    return 0;
}

// Finds the first occurrence of `needle` inside the readable ranges.
inline const char* findStringInRanges(const ModuleRanges& ranges, const char* needle) {
    const std::size_t len = std::strlen(needle);
    if (len == 0 || !ranges.dataStart || ranges.dataEnd - ranges.dataStart < len) return nullptr;
    const char* base = reinterpret_cast<const char*>(ranges.dataStart);
    const std::size_t size = ranges.dataEnd - ranges.dataStart;
    for (std::size_t offset = 0; offset + len <= size;) {
        const void* hit = std::memchr(base + offset, needle[0], size - offset);
        if (!hit) return nullptr;
        const std::size_t at = static_cast<const char*>(hit) - base;
        if (std::memcmp(hit, needle, len) == 0) return base + at;
        offset = at + 1;
    }
    return nullptr;
}

// Scans the text range once and indexes every ADRP+ADD pair by the address it
// computes, so locateRenderStatusEffects() can look references up per icon
// string without re-scanning the (large) code section for each one.
//
// The compiler emits the canonical `ADRP Xd, page; ADD Xd, Xd, #lo12`
// sequence, so each ADRP is paired with the first ADD that consumes its
// register. Using only the *first* such ADD is important: several icon strings
// usually share one 4 KB page, so a naive "any ADRP-to-page followed by any
// ADD-with-low12 within N instructions" scan would pair an ADRP for one string
// with a neighboring string's ADD and report phantom references.
inline std::unordered_map<std::uintptr_t, std::vector<std::uintptr_t>>
collectAllStringRefs(const ModuleRanges& ranges) {
    std::unordered_map<std::uintptr_t, std::vector<std::uintptr_t>> refsByTarget;
    const std::size_t textSize = ranges.textEnd - ranges.textStart;
    if (textSize < 4 || textSize > 512u * 1024u * 1024u) return refsByTarget;
    const std::size_t count = textSize / 4;
    const auto* text = reinterpret_cast<const std::uint8_t*>(ranges.textStart);

    for (std::size_t i = 0; i + 1 < count; ++i) {
        std::uint32_t insn = 0;
        std::memcpy(&insn, text + i * 4, sizeof(insn));
        std::int32_t imm21 = 0;
        std::uint32_t rd = 0;
        if (!decodeAdrp(insn, imm21, rd)) continue;

        const std::uintptr_t pc = ranges.textStart + i * 4;
        const std::uintptr_t page = (pc & ~0xFFFULL) +
            static_cast<std::uintptr_t>(static_cast<std::int64_t>(imm21) << 12);

        // Find the first ADD (immediate) whose source register is the ADRP's
        // destination; that is the ADRP's pairing instruction.
        const std::size_t limit = std::min<std::size_t>(count, i + 1 + 8);
        std::size_t pairIndex = 0;
        for (std::size_t j = i + 1; j < limit; ++j) {
            std::uint32_t next = 0;
            std::memcpy(&next, text + j * 4, sizeof(next));
            std::uint32_t imm12 = 0, rn = 0, rd2 = 0;
            if (!decodeAddImm(next, imm12, rn, rd2)) continue;
            if (rn != rd) continue;
            pairIndex = j;
            break;
        }
        if (pairIndex == 0) continue;

        std::uint32_t imm12 = 0, rn = 0, rd2 = 0;
        std::uint32_t pair = 0;
        std::memcpy(&pair, text + pairIndex * 4, sizeof(pair));
        decodeAddImm(pair, imm12, rn, rd2);

        refsByTarget[page + imm12].push_back(pc);
    }
    return refsByTarget;
}

// Icon texture paths referenced by the vanilla status-effect bar renderer.
// The HUD draws icons for all of these, so a direct hit on several of them
// strongly suggests we found HudScreen::_renderStatusEffects itself.
inline constexpr std::array<const char*, 10> kVanillaEffectIconPaths{{
    "textures/ui/speed_effect",
    "textures/ui/slowness_effect",
    "textures/ui/haste_effect",
    "textures/ui/strength_effect",
    "textures/ui/regeneration_effect",
    "textures/ui/fire_resistance_effect",
    "textures/ui/water_breathing_effect",
    "textures/ui/poison_effect",
    "textures/ui/night_vision_effect",
    "textures/ui/absorption_effect",
}};

// Best-effort, build-independent location of HudScreen::_renderStatusEffects.
// Returns 0 when no single function can be identified with confidence.
//
// The icon texture paths can be referenced either directly inside the
// renderer itself (a big switch over effect ids) or from a small helper the
// renderer calls (a "pick icon path" leaf). Both topologies are handled: a
// candidate that is tiny and has direct BL callers is treated as a helper and
// the largest of its callers is reported instead — the early-return detour may
// only be applied to a void render callback, never to a value-returning leaf.
// Everything stays behind the caller's warm-up gate, so a wrong guess that is
// not called at frame rate simply never suppresses anything.
inline std::uintptr_t locateRenderStatusEffects(const ModuleRanges& ranges) {
    struct Candidate {
        std::size_t refCount = 0;
        std::size_t distinctStrings = 0;
        std::uintptr_t minPc = 0;
        std::uintptr_t maxPc = 0;
    };
    std::unordered_map<std::uintptr_t, Candidate> clusters;

    // One pass over the code section, then a per-icon lookup.
    const auto refsByTarget = collectAllStringRefs(ranges);

    for (const char* path : kVanillaEffectIconPaths) {
        const char* stringAddr = findStringInRanges(ranges, path);
        if (!stringAddr) continue;
        auto it = refsByTarget.find(reinterpret_cast<std::uintptr_t>(stringAddr));
        if (it == refsByTarget.end() || it->second.empty()) continue;
        auto refs = it->second;

        // Deduplicate hits and fold them into their containing function.
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());

        std::unordered_map<std::uintptr_t, std::size_t> entriesForThisString;
        for (const std::uintptr_t ref : refs) {
            const std::uintptr_t entry = findFunctionStart(ranges, ref, 512);
            if (!entry) continue;
            entriesForThisString[entry]++;
        }

        for (const auto& [entry, hits] : entriesForThisString) {
            auto& candidate = clusters[entry];
            candidate.refCount += hits;
            ++candidate.distinctStrings;
            if (candidate.minPc == 0 || refs.front() < candidate.minPc) candidate.minPc = refs.front();
            if (refs.back() > candidate.maxPc) candidate.maxPc = refs.back();
        }
    }

    if (clusters.empty()) return 0;

    std::vector<std::pair<std::uintptr_t, const Candidate*>> ranked;
    ranked.reserve(clusters.size());
    for (const auto& [entry, candidate] : clusters) ranked.emplace_back(entry, &candidate);
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.second->refCount > b.second->refCount;
    });

    const auto& [topEntry, top] = ranked.front();
    const Candidate& runnerUp = ranked.size() > 1 ? *ranked[1].second : Candidate{};
    if (top->refCount < 4) return 0;                    // too few references
    if (top->distinctStrings < 3) return 0;             // must touch several icons
    if (top->refCount <= runnerUp.refCount) return 0;   // must dominate other functions

    // The references must lie inside the candidate function and within a
    // plausible span for a renderer that loops over every status effect.
    if (top->minPc < topEntry || top->maxPc < top->minPc) return 0;
    if (top->maxPc - topEntry > 128u * 1024u) return 0;

    // A candidate that is tiny and called from elsewhere is almost certainly
    // an icon-path helper; the actual draw callback is its caller. Resolve the
    // callers and prefer the largest one (the longest prologue-to-call span is
    // the best guess for the function that loops over every active effect).
    const bool looksLikeHelper =
        top->maxPc - topEntry <= 0x100u &&
        ranges.textEnd - ranges.textStart >= 8;
    if (looksLikeHelper) {
        std::unordered_map<std::uintptr_t, std::pair<std::uintptr_t, std::size_t>> callers;  // entry -> (bl pc, span)
        const std::size_t textSize = ranges.textEnd - ranges.textStart;
        const std::size_t count = textSize / 4;
        const auto* text = reinterpret_cast<const std::uint8_t*>(ranges.textStart);
        for (std::size_t i = 0; i < count; ++i) {
            std::uint32_t insn = 0;
            std::memcpy(&insn, text + i * 4, sizeof(insn));
            const std::uintptr_t pc = ranges.textStart + i * 4;
            std::uintptr_t target = 0;
            if (!decodeBl(insn, pc, target) || target != topEntry) continue;
            const std::uintptr_t callerEntry = findFunctionStart(ranges, pc, 512);
            if (!callerEntry || callerEntry == topEntry) continue;
            auto& entryRef = callers[callerEntry];
            if (pc - callerEntry > entryRef.second) {
                entryRef.first = pc;
                entryRef.second = pc - callerEntry;
            }
        }
        if (!callers.empty()) {
            std::uintptr_t bestEntry = 0;
            std::size_t bestSpan = 0;
            for (const auto& [entry, bl] : callers) {
                if (bl.second > bestSpan) {
                    bestEntry = entry;
                    bestSpan = bl.second;
                }
            }
            if (bestEntry) return bestEntry;
        }
    }

    return topEntry;
}

} // namespace bedrocktools::hud::effectlocator
