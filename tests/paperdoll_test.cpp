// Host unit tests for PaperDollModule.
// Build via ./scripts/run_tests.sh.

#include "modules/hud/paperdoll.cpp"

namespace bedrocktools::memory {
std::uintptr_t resolve(SignatureId) { return 0; }
}

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

struct FakeHudPlayerRenderer {
    std::uint8_t header[0xC]{};
    float timeRemaining = 0.0f;
};

struct FakeUIControl {
    std::uint8_t header[0x10]{};
    float pos[2]{10.0f, 20.0f};
    std::uint8_t mid[0x28]{};
    float size[2]{40.0f, 60.0f};
};

static_assert(offsetof(FakeHudPlayerRenderer, timeRemaining) ==
              bedrocktools::sdk::offsets::HudPlayerRenderer::mTimeRemaining);
static_assert(offsetof(FakeUIControl, pos) ==
              bedrocktools::sdk::offsets::UIControl::mPosition);
static_assert(offsetof(FakeUIControl, size) ==
              bedrocktools::sdk::offsets::UIControl::mSize);

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

    renderer.timeRemaining = 2.5f;
    mod.beforeRender(&renderer);
    assert(renderer.timeRemaining == 2.5f);
}

void testCombatOnlyMode() {
    PaperDollModule mod;
    mod.setMasterEnabled(true);
    mod.setMode(PaperDollModule::VisibilityMode::CombatOnly);

    FakeHudPlayerRenderer renderer{};
    renderer.timeRemaining = 0.0f;

    // Before combat: timer stays 0.
    mod.beforeRender(&renderer);
    assert(renderer.timeRemaining == 0.0f);

    // After combat trigger: timer is kept positive.
    mod.notifyCombatActivity();
    mod.beforeRender(&renderer);
    assert(renderer.timeRemaining > 0.0f);
}

void testHideInContainer() {
    PaperDollModule mod;
    mod.setMasterEnabled(true);
    mod.setHideInContainer(true);

    FakeHudPlayerRenderer renderer{};
    FakeUIControl control{};
    PaperDollModule::ControlState state{};

    assert(mod.beforeRender(&renderer, &control, state) == true);

    mod.onScreenStateChanged(true);
    assert(mod.hiddenByScreen() == true);
    assert(mod.beforeRender(&renderer, &control, state) == false);

    mod.onScreenStateChanged(false);
    assert(mod.hiddenByScreen() == false);
    assert(mod.beforeRender(&renderer, &control, state) == true);
}

void testPositionAndScaleRestore() {
    PaperDollModule mod;
    mod.setMasterEnabled(true);
    mod.setOffset(15.0f, -5.0f);
    mod.setScale(1.5f);

    FakeHudPlayerRenderer renderer{};
    FakeUIControl control{};
    PaperDollModule::ControlState state{};

    assert(mod.beforeRender(&renderer, &control, state) == true);
    assert(std::fabs(control.pos[0] - 25.0f) < 1e-4f);
    assert(std::fabs(control.pos[1] - 15.0f) < 1e-4f);
    assert(std::fabs(control.size[0] - 60.0f) < 1e-4f);
    assert(std::fabs(control.size[1] - 90.0f) < 1e-4f);

    mod.afterRender(&control, state);
    assert(std::fabs(control.pos[0] - 10.0f) < 1e-4f);
    assert(std::fabs(control.pos[1] - 20.0f) < 1e-4f);
    assert(std::fabs(control.size[0] - 40.0f) < 1e-4f);
    assert(std::fabs(control.size[1] - 60.0f) < 1e-4f);
}

void testConfigRoundTrip() {
    PaperDollModule mod;
    mod.setMasterEnabled(true);
    mod.setMode(PaperDollModule::VisibilityMode::CombatOnly);
    mod.setOffset(30.0f, -12.5f);
    mod.setScale(1.75f);
    mod.setRotation(45.0f);
    mod.setAutoSpin(true);
    mod.setHideInContainer(false);

    nlohmann::json saved;
    mod.saveConfig(saved);

    PaperDollModule restored;
    restored.loadConfig(saved);
    assert(restored.masterEnabled == true);
    assert(restored.mode() == PaperDollModule::VisibilityMode::CombatOnly);
    assert(std::fabs(restored.offsetX() - 30.0f) < 1e-4f);
    assert(std::fabs(restored.offsetY() - (-12.5f)) < 1e-4f);
    assert(std::fabs(restored.scale() - 1.75f) < 1e-4f);
    assert(std::fabs(restored.rotation() - 45.0f) < 1e-4f);
    assert(restored.autoSpin() == true);
    assert(restored.hideInContainer() == false);
}

} // namespace

int main() {
    testDisabledLeavesTimerUntouched();
    testEnabledAlwaysVisibleKeepsTimerPositive();
    testCombatOnlyMode();
    testHideInContainer();
    testPositionAndScaleRestore();
    testConfigRoundTrip();
    std::cout << "paperdoll_test: all checks passed\n";
    return 0;
}
