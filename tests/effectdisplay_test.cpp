// Unit tests for the Effect Display HUD module's language support.
//
// Verifies the feature end-to-end against the documented contract, with the
// launcher APIs stubbed (same pattern as crosshair_test.cpp):
//   * "Auto" follows the game language read from options.txt
//     (games/com.mojang/minecraftpe/options.txt, `game_language` key)
//   * rewriting options.txt switches the names on the next poll (~2s)
//   * Arabic names use the launcher's default font (empty fontId) because
//     the bundled pixel font has no Arabic glyphs; English keeps it
//   * endless effects show the localized infinity label
//   * a pinned language setting wins over the game language
//   * the language radio round-trips as "<index>,Auto,<language>..."
//
// Like crosshair_test.cpp, the module needs the preloader and nlohmann_json
// headers (normally provided by xmake). Build and run standalone (adjust the
// package paths to your xmake cache):
//
//     PRE_LOADER=$(echo ~/.xmake/packages/p/preloader/main/*/include)
//     JSON=$(echo ~/.xmake/packages/n/nlohmann_json/v3.11.3/*/include)
//     ENTT=$(echo ~/.xmake/packages/e/entt/v3.16.0/*/include)
//     FMT=$(echo ~/.xmake/packages/f/fmt/12.2.0/*/include)
//     g++ -std=c++20 -I include -I src -I "$PRE_LOADER" -I "$JSON" \
//         -I "$ENTT" -I "$FMT" tests/effectdisplay_test.cpp -o /tmp/effectdisplay_test
//     /tmp/effectdisplay_test

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// Real declarations first so the stubs below match them exactly.
#include <pl/ModMenu.hpp>
#include <pl/memory/Hook.hpp>
#include "bedrocktools/memory/Signatures.hpp"
#include "bedrocktools/events/EventBus.hpp"
#include "core/Runtime.hpp"

// ---- launcher / runtime stubs -------------------------------------------
static std::vector<pl::modmenu::DrawCommand> g_lastCmds;

namespace pl::memory {
    int hook(FuncPtr, FuncPtr, FuncPtr*, HookPriority) { return -1; }
    bool unhook(FuncPtr, FuncPtr) { return true; }
}

namespace pl::modmenu {
    bool registerModule(const ModuleInfo&) { return true; }
    void unregisterModule(std::string_view) {}
    void setModuleEnabled(std::string_view, bool) {}
    void submitDrawCommands(std::string_view, std::span<const DrawCommand> commands) {
        g_lastCmds.assign(commands.begin(), commands.end());
    }
    bool registerFont(std::string_view, std::span<const unsigned char>) { return true; }
    bool registerImage(std::string_view, std::span<const unsigned char>, int, int) { return true; }
    bool registerButton(const ButtonInfo&) { return true; }
    void unregisterButton(std::string_view) {}
}

namespace bedrocktools::memory {
    std::uintptr_t resolve(SignatureId) { return 0; }   // no vanilla-bar hook
}

namespace bedrocktools::events {
    EventBus& bus() { static EventBus instance; return instance; }
}

namespace bedrocktools::core {
    Runtime& Runtime::get() { static Runtime instance; return instance; }
    const std::filesystem::path& Runtime::resourceDirectory() const noexcept {
        static const std::filesystem::path empty;
        return empty;   // no minecraft.ttf on the host: font stays unregistered
    }
}

#include "modules/hud/effectdisplay.cpp"

namespace {
int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}
const pl::modmenu::DrawCommand* findText(const std::string& contains) {
    for (const auto& c : g_lastCmds) {
        if (c.type == pl::modmenu::DrawCommandType::Text && c.text.find(contains) != std::string::npos) return &c;
    }
    return nullptr;
}
void writeOptions(const std::filesystem::path& dir, const std::string& language, int epochOffset) {
    std::filesystem::create_directories(dir / "games/com.mojang/minecraftpe");
    const auto file = dir / "games/com.mojang/minecraftpe/options.txt";
    std::ofstream(file, std::ios::trunc) << "mp_username:Steve\ngame_difficulty_new:1\n"
                                            "game_language:" << language << "\nmp_server_visible:1\n";
    // Force well-separated mtimes (the watcher compares stat() seconds).
    std::filesystem::last_write_time(file,
        std::filesystem::file_time_type{} + std::chrono::seconds(epochOffset));
}
} // namespace

int main() {
    const auto sandbox = std::filesystem::temp_directory_path() / "effectdraw_sandbox";
    std::filesystem::remove_all(sandbox);
    writeOptions(sandbox, "en_US", 1000);
    std::filesystem::current_path(sandbox);

    EffectDisplayModule mod;
    mod.setMasterEnabled(true);
    nlohmann::json cfg;
    cfg["m_preview"] = true;
    mod.loadConfig(cfg);
    mod.onInit();
    mod.onFrame();

    std::printf("english (auto, from options.txt)\n");
    const auto* speed = findText("Speed");
    check(speed != nullptr, "preview shows Speed II");
    check(speed && speed->text == "Speed II", "name text is 'Speed II'");
    check(speed && speed->fontId == "minecraft", "English uses the pixel font");
    const auto* infinite = findText("\xE2\x88\x9E");
    check(infinite != nullptr, "endless Invisibility shows the infinity label");

    std::printf("arabic (auto, options.txt rewritten)\n");
    writeOptions(sandbox, "ar_SA", 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    mod.onFrame();
    const auto* arabic = findText("\xD8\xb3\xD8\xb1\xD8\xb9\xD8\xa9");  // سرعة
    check(arabic != nullptr, "preview shows سرعة (Speed in Arabic)");
    check(arabic && arabic->fontId.empty(), "Arabic switches to the system font");
    check(findText("Speed") == nullptr, "no English name left");

    std::printf("pinned language override (English while game says Arabic)\n");
    nlohmann::json pin;
    pin["m_language"] = "1";   // index 1 -> kLanguages[0] == en_US
    mod.loadConfig(pin);
    mod.onFrame();
    check(findText("Speed II") != nullptr, "pinned English wins over the game language");

    std::printf("config round-trip\n");
    nlohmann::json saved;
    mod.saveConfig(saved);
    const std::string radio = saved["m_language"];
    check(radio.rfind("1,Auto,English (US)", 0) == 0, "radio persists as '<index>,Auto,...' (got: " + radio.substr(0, 24) + "...)");
    size_t commas = std::count(radio.begin(), radio.end(), ',');
    check(commas == bedrocktools::hud::effects::languageCount() + 1,
          "one option per language plus Auto");

    std::printf("\n%s\n", g_failures ? "HARNESS FAILED" : "harness passed");
    return g_failures ? 1 : 0;
}
