// Unit tests for the signature-free locator used by the Effect Display
// module to find HudScreen::_renderStatusEffects (see effectlocator.hpp).
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src tests/effectlocator_test.cpp -o /tmp/effectlocator_test
//     /tmp/effectlocator_test
//
// The test builds synthetic "binaries" out of real ARM64 instruction
// encodings (classic `stp x29, x30, [sp, #-N]!` prologues, ADRP+ADD string
// references, BL calls) and checks the selection logic, including the
// helper->caller resolution and the conservative failure modes.

#include "modules/hud/effectlocator.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace loc = bedrocktools::hud::effectlocator;

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

// ---- synthetic binary builder (real instruction encodings) ----

std::uint32_t encodeAdrp(std::uintptr_t pc, std::uintptr_t target, std::uint32_t rd) {
    std::int64_t delta = static_cast<std::int64_t>((target & ~0xFFFULL) - (pc & ~0xFFFULL));
    std::int32_t imm21 = static_cast<std::int32_t>(delta >> 12);
    std::uint32_t immlo = static_cast<std::uint32_t>(imm21) & 0x3;          // bits 30:29
    std::uint32_t immhi = (static_cast<std::uint32_t>(imm21) >> 2) & 0x7FFFF;
    return 0x90000000u | (immlo << 29) | (immhi << 5) | rd;
}

std::uint32_t encodeAddImm(std::uint32_t rd, std::uint32_t rn, std::uint32_t imm12) {
    return 0x91000000u | (imm12 << 10) | (rn << 5) | rd;
}

std::uint32_t encodeBl(std::uintptr_t pc, std::uintptr_t target) {
    std::int64_t delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(pc);
    std::int32_t imm26 = static_cast<std::int32_t>(delta >> 2);
    return 0x94000000u | (static_cast<std::uint32_t>(imm26) & 0x03FFFFFF);
}

struct FakeBinary {
    // The fake image lives in the vectors' real storage so the locator's
    // memchr/read helpers can touch it. Capacity is reserved up front so the
    // addresses stay stable while the image is built; the ADRP encoder
    // computes immediates from these actual addresses, so the (unrelated) page
    // relationship between the text and data vectors is irrelevant.
    std::vector<std::uint32_t> text;
    std::vector<char> data;
    std::map<std::string, std::uintptr_t> strings;
    std::uintptr_t textBase = 0;
    std::uintptr_t dataBase = 0;

    FakeBinary() {
        text.reserve(4096);
        data.reserve(0x4000);
        textBase = reinterpret_cast<std::uintptr_t>(text.data());
        dataBase = reinterpret_cast<std::uintptr_t>(data.data());
    }

    std::uintptr_t textAddr(std::size_t index) const { return textBase + index * 4; }

    std::uintptr_t stringAddress(const std::string& name) {
        auto it = strings.find(name);
        if (it != strings.end()) return it->second;
        std::uintptr_t addr = dataBase + data.size();
        while (addr % 4 != 0) { data.push_back('\0'); ++addr; }
        strings[name] = addr;
        data.insert(data.end(), name.begin(), name.end());
        data.push_back('\0');
        return addr;
    }

    // Classic prologue: stp x29, x30, [sp, #-0x10]! + mov x29, sp, then one
    // ADRP+ADD reference per icon, a nop, and ret.
    std::uintptr_t addFunctionClassic(const std::vector<std::string>& iconNames) {
        const std::uintptr_t entry = textAddr(text.size());
        text.push_back(0xA9BF7BFD);   // stp x29, x30, [sp, #-0x10]!
        text.push_back(0x910003FD);   // mov x29, sp
        text.push_back(0xD503201F);   // nop
        for (const auto& name : iconNames) {
            const std::uintptr_t strAddr = stringAddress(name);
            text.push_back(encodeAdrp(textAddr(text.size()), strAddr, 8));
            text.push_back(encodeAddImm(8, 8, strAddr & 0xFFF));
            text.push_back(0xD503201F);
        }
        text.push_back(0xD65F03C0);   // ret
        return entry;
    }

    // Adds a classic-prologue function that contains `bl helperEntry` in its
    // body (after `bodyNops` nops); reports its entry through `callerEntryOut`.
    void addCallerOf(std::uintptr_t helperEntry, std::uintptr_t& callerEntryOut, int bodyNops = 8) {
        const std::uintptr_t entry = textAddr(text.size());
        callerEntryOut = entry;
        text.push_back(0xA9BF7BFD);
        text.push_back(0x910003FD);
        for (int i = 0; i < bodyNops; ++i) text.push_back(0xD503201F);
        text.push_back(encodeBl(textAddr(text.size()), helperEntry));
        for (int i = 0; i < 4; ++i) text.push_back(0xD503201F);
        text.push_back(0xD65F03C0);
    }

    loc::ModuleRanges ranges() const {
        loc::ModuleRanges r;
        r.textStart = textBase;
        r.textEnd = textBase + text.size() * 4;
        r.dataStart = dataBase;
        r.dataEnd = dataBase + data.size();
        return r;
    }
};

const std::vector<std::string>& icons() {
    static const std::vector<std::string> all{
        loc::kVanillaEffectIconPaths.begin(), loc::kVanillaEffectIconPaths.end()};
    return all;
}

void testTopologyBHelperWithCallers() {
    FakeBinary bin;
    // Small helper (classic prologue) references four icons; two callers call
    // it. The locator must report the largest caller, not the helper.
    const std::uintptr_t helperEntry = bin.addFunctionClassic({icons()[0], icons()[1], icons()[2], icons()[3]});
    std::uintptr_t rendererEntry = 0, smallCallerEntry = 0;
    bin.addCallerOf(helperEntry, rendererEntry, 32);   // renderer-like: long body
    bin.addCallerOf(helperEntry, smallCallerEntry, 8);

    const std::uintptr_t found = loc::locateRenderStatusEffects(bin.ranges());
    check(found == rendererEntry, "helper topology resolves to the largest caller");
    check(found != helperEntry, "helper itself is never reported");
}

void testTopologyAInlineInRenderer() {
    FakeBinary bin;
    // The renderer itself references six icons inline and has no direct
    // callers; a distractor function references fewer icons.
    const std::uintptr_t rendererEntry = bin.addFunctionClassic(
        {icons()[0], icons()[1], icons()[2], icons()[3], icons()[4], icons()[5]});
    bin.addFunctionClassic({icons()[0], icons()[2]});

    const std::uintptr_t found = loc::locateRenderStatusEffects(bin.ranges());
    check(found == rendererEntry, "inline topology resolves to the renderer itself");
}

void testTieFailsSafe() {
    FakeBinary bin;
    // Two functions each reference three icons: ambiguous, must bail out.
    bin.addFunctionClassic({icons()[0], icons()[1], icons()[2]});
    bin.addFunctionClassic({icons()[3], icons()[4], icons()[5]});
    check(loc::locateRenderStatusEffects(bin.ranges()) == 0, "ambiguous tie yields no target");
}

void testTooFewRefsFailsSafe() {
    FakeBinary bin;
    bin.addFunctionClassic({icons()[0], icons()[1]});
    check(loc::locateRenderStatusEffects(bin.ranges()) == 0, "insufficient evidence yields no target");
}

void testDecodersAgainstKnownEncodings() {
    // `stp x29, x30, [sp, #-0x10]!`, `sub sp, sp, #0x10`, pacibsp, ret, nop.
    check(loc::isStrongPrologue(0xA9BF7BFD), "stp x29,x30,[sp,#-0x10]! is a prologue");
    check(loc::isStrongPrologue(0x6DBD23E9), "stp d9,d8,[sp,#-0x30]! is a prologue");
    check(loc::isStrongPrologue(0xD10043FF), "sub sp,sp,#0x10 is a prologue");
    check(loc::isStrongPrologue(0xD503233F), "pacibsp is a prologue");
    check(!loc::isStrongPrologue(0xD65F03C0), "ret is not a prologue");
    check(!loc::isStrongPrologue(0xA93B7BFD), "offset stp is not a prologue");
    check(!loc::isStrongPrologue(0xA9FF7BFD), "pre-index ldp is not a prologue");
    check(!loc::isStrongPrologue(0xD503201F), "nop is not a prologue");
}

} // namespace

int main() {
    std::printf("effectlocator_test\n");
    testDecodersAgainstKnownEncodings();
    testTopologyBHelperWithCallers();
    testTopologyAInlineInRenderer();
    testTieFailsSafe();
    testTooFewRefsFailsSafe();

    if (g_failures == 0) {
        std::printf("all effectlocator tests passed\n");
        return 0;
    }
    std::printf("%d effectlocator test(s) FAILED\n", g_failures);
    return 1;
}
