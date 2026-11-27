// Host unit tests for PaperDollModule.
// Build via ./scripts/run_tests.sh.

#include "modules/hud/paperdoll.cpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

struct FakeHudPlayerRenderer {
    std::uint8_t header[0xC]{};
    float timeRemaining = 0.0f;
};

static_assert(offsetof(FakeHudPlayerRenderer, timeRemaining) ==
              bedrocktools::sdk::offsets::HudPlayerRenderer::mTimeRemaining);

void testDisabledLeavesTimerUntouched() {
    PaperDollModule mod;
    FakeHudPlayerRenderer renderer{};
    renderer.timeRemaining = 0.0f;

    mod.beforeRender(&renderer);
    assert(renderer.timeRemaining == 0.0f);
}

void testEnabledAlwaysVisibleKeepsTimerPositive() {
    PaperDollModule mod;
    mod.setMasterEnabled(true);
    FakeHudPlayerRenderer renderer{};
    renderer.timeRemaining = 0.0f;

    mod.beforeRender(&renderer);
    assert(renderer.timeRemaining > 0.0f);

    // Existing positive timer should not be shortened.
    renderer.timeRemaining = 2.5f;
    mod.beforeRender(&renderer);
    assert(renderer.timeRemaining == 2.5f);
}

void testAlwaysVisibleOffLeavesTimerUntouched() {
    PaperDollModule mod;
    mod.setMasterEnabled(true);
    mod.setAlwaysVisible(false);
    FakeHudPlayerRenderer renderer{};
    renderer.timeRemaining = 0.0f;

    mod.beforeRender(&renderer);
    assert(renderer.timeRemaining == 0.0f);
}

void testConfigRoundTrip() {
    PaperDollModule mod;
    mod.setMasterEnabled(true);
    mod.setAlwaysVisible(false);

    nlohmann::json saved;
    mod.saveConfig(saved);

    PaperDollModule restored;
    restored.loadConfig(saved);
    assert(restored.masterEnabled == true);
    assert(restored.enabled == true);
    assert(restored.alwaysVisible() == false);
}

} // namespace

int main() {
    testDisabledLeavesTimerUntouched();
    testEnabledAlwaysVisibleKeepsTimerPositive();
    testAlwaysVisibleOffLeavesTimerUntouched();
    testConfigRoundTrip();
    std::cout << "paperdoll_test: all checks passed\n";
    return 0;
}
