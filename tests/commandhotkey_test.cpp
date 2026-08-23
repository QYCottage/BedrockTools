// Unit tests for the Command Hotkey on-screen buttons.
//
// Verifies the contract that matters for the bug where editing the "Command
// Label" (or "Command" itself) did not update the visible button:
//   * a label-only config edit re-registers the native button definition so the
//     launcher's overlay snapshot picks up the new label (refreshExternalButtonsForModule
//     then re-applies it in place)
//   * editing the command while the label is empty updates the derived label
//   * an unchanged config does NOT rebuild the buttons (no needless churn
//     while unrelated sliders/toggles fire)
//
// Build and run standalone (adjust the two package paths to your xmake cache):
//
//     PRE_LOADER=$(echo ~/.xmake/packages/p/preloader/main/*/include)
//     JSON=$(echo ~/.xmake/packages/n/nlohmann_json/v3.11.3/*/include)
//     g++ -std=c++20 -I include -I src -I "$PRE_LOADER" -I "$JSON" \
//         tests/commandhotkey_test.cpp -o /tmp/commandhotkey_test
//     /tmp/commandhotkey_test

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

// Real declarations first so the stubs below match them exactly.
#include "pl/ModMenu.hpp"
#include "bedrocktools/memory/Signatures.hpp"

// ---------------------------------------------------------------------------
// Launcher / SDK stubs
// ---------------------------------------------------------------------------

// Recorded overlay activity (buttonId -> last registered label).
static std::unordered_map<std::string, std::string> g_labelById;
static std::unordered_map<std::string, int> g_registerCount;
static std::unordered_map<std::string, int> g_unregisterCount;

namespace pl::modmenu {
    bool registerModule(const ModuleInfo&) { return true; }
    void unregisterModule(std::string_view) {}
    void setModuleEnabled(std::string_view, bool) {}
    void submitDrawCommands(std::string_view, std::span<const DrawCommand>) {}
    bool registerFont(std::string_view, std::span<const unsigned char>) { return true; }
    bool registerImage(std::string_view, std::span<const unsigned char>, int, int) { return true; }
    bool registerButton(const ButtonInfo& info) {
        g_labelById[info.buttonId] = info.label;
        ++g_registerCount[info.buttonId];
        return true;
    }
    void unregisterButton(std::string_view id) {
        ++g_unregisterCount[std::string(id)];
    }
}

namespace bedrocktools::memory {
    std::uintptr_t resolve(SignatureId) { return 0; }
}

namespace bedrocktools::core::gamehooks {
    void* clientInstance() { return nullptr; }
}

// The module under test, included directly so its anonymous-namespace helpers
// (launcherLabel, defaultLabel, ...) are reachable.
#include "modules/misc/commandhotkey.cpp"

// ModuleRegistry is only referenced for keybind gating in onKeyEvent; stub the
// members the translation unit touches.
ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry instance;
    return instance;
}
bool ModuleRegistry::keybindBlocked() const { return false; }
Module* ModuleRegistry::find(std::string_view) const { return nullptr; }
const std::vector<Module*>& ModuleRegistry::modules() const {
    static std::vector<Module*> empty;
    return empty;
}

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (condition) std::printf("  ok   %s\n", what);
    else { std::printf("  FAIL %s\n", what); ++g_failures; }
}

} // namespace

int main() {
    std::printf("Command Hotkey overlay buttons\n");

    CommandHotkeyModule mod;

    // Initial config: slot 1 enabled with a command and a custom label.
    nlohmann::json j;
    mod.saveConfig(j);                 // emit the full key set
    j["m_command1"] = true;
    j["m_command1Command"] = "/home";
    j["m_command1Label"] = "MyHome";
    j["m_command1Width"] = 64.0f;
    j["m_command1Height"] = 64.0f;
    j["m_command1TextColor"] = "#373737";
    j["m_buttonBorderColor"] = "#373737"; // non-legacy so text/label keys apply
    mod.loadConfig(j);

    const std::string btn1 = "bedrocktools.CommandHotkey.Button1";
    check(g_registerCount[btn1] >= 1, "slot 1 registered on first load");
    check(g_labelById[btn1] == "MyHome", "button shows the configured label");

    const int regAfterFirst = g_registerCount[btn1];
    const int unregAfterFirst = g_unregisterCount[btn1];

    // Re-load the *same* config: nothing changed, so no rebuild is expected.
    mod.loadConfig(j);
    check(g_registerCount[btn1] == regAfterFirst, "unchanged config does not rebuild");
    check(g_unregisterCount[btn1] == unregAfterFirst, "unchanged config does not unregister");

    // Now change ONLY the label (simulating typing in the "Command Label" field).
    nlohmann::json j2 = j;
    j2["m_command1Label"] = "GoHome";
    mod.loadConfig(j2);
    check(g_registerCount[btn1] > regAfterFirst, "label-only change re-registers the button");
    check(g_labelById[btn1] == "GoHome", "button label updated to the new text");

    // Change ONLY the command while the label is empty -> derived label changes.
    nlohmann::json j3 = j;
    j3["m_command1Label"] = "";        // empty label -> derived from command
    j3["m_command1Command"] = "/spawn";
    const int regBeforeCmd = g_registerCount[btn1];
    mod.loadConfig(j3);
    check(g_registerCount[btn1] > regBeforeCmd, "command-only change (empty label) re-registers");
    check(g_labelById[btn1] == "spawn", "derived label follows the new command");

    // Changing the keybind alone must NOT rebuild (keybind does not affect the button).
    // Branch from j3 so the only difference is the keybind.
    nlohmann::json j4 = j3;
    j4["m_command1Keybind"] = 42;
    const int regBeforeKey = g_registerCount[btn1];
    mod.loadConfig(j4);
    check(g_registerCount[btn1] == regBeforeKey, "keybind-only change does not rebuild");

    std::printf(g_failures == 0 ? "ALL TESTS PASSED\n" : "%d TEST(S) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
