// Unit tests for the localized effect-name table of the Effect Display HUD
// module (table integrity, language-code matching, options.txt parsing).
//
// Build and run standalone (no game required):
//     g++ -std=c++20 -I src tests/effecti18n_test.cpp -o /tmp/effecti18n_test
//     /tmp/effecti18n_test

#include "modules/hud/effecti18n.hpp"

#include <cstdio>
#include <string>
#include <vector>

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

int indexForCode(const char* code) {
    return static_cast<int>(fx::languageIndexForCode(code));
}

std::size_t indexOf(const char* code) {
    const int index = fx::languageIndexForCode(code);
    return index < 0 ? fx::kFallbackLanguage : static_cast<std::size_t>(index);
}

} // namespace

int main() {
    std::printf("table integrity\n");
    check(fx::languageCount() >= 29, "at least the 29 official Bedrock languages are bundled");
    check(fx::kLocalizedEffectCount == 38, "38 effect slots (ids 0..37)");
    {
        std::vector<std::string> codes;
        for (std::size_t i = 0; i < fx::languageCount(); ++i) {
            const auto& lang = fx::kLanguages[i];
            codes.emplace_back(lang.code);
            check(lang.code && *lang.code, std::string("language ") + std::to_string(i) + " has a code");
            check(lang.nativeName && *lang.nativeName, lang.code + std::string(" has a native name"));
            check(lang.infiniteDuration && *lang.infiniteDuration, std::string(lang.code) + " has an infinite label");
            check(lang.unknownEffect && *lang.unknownEffect, std::string(lang.code) + " has an unknown-effect label");
            for (std::uint32_t id = 1; id < fx::kLocalizedEffectCount; ++id) {
                const char* name = lang.effectNames[id];
                if (name == nullptr || *name == '\0') {
                    check(false, std::string(lang.code) + " effect " + std::to_string(id) + " has a name");
                }
            }
        }
        bool unique = true;
        for (std::size_t i = 0; i < codes.size(); ++i) {
            for (std::size_t j = i + 1; j < codes.size(); ++j) {
                if (codes[i] == codes[j]) unique = false;
            }
        }
        check(unique, "language codes are unique");
    }
    check(std::string(fx::kLanguages[fx::kFallbackLanguage].code) == "en_US",
          "index 0 is en_US (the fallback)");

    std::printf("language code matching\n");
    checkEqual(indexForCode("en_US"), 0, "exact en_US matches index 0");
    checkEqual(indexForCode("ja_JP"), indexForCode("ja"), "ja matches ja_JP");
    checkEqual(indexForCode("ar_AE"), indexForCode("ar_SA"), "other Arabic regions match ar_SA");
    checkEqual(indexForCode("pt"), indexForCode("pt_BR"), "bare pt resolves to a Portuguese table");
    checkEqual(indexForCode("zz_ZZ"), -1, "unknown code does not match");
    checkEqual(indexForCode(""), -1, "empty code does not match");

    std::printf("localized names\n");
    checkEqual(fx::effectName(1, fx::kFallbackLanguage), "Speed", "Speed in English");
    checkEqual(fx::effectName(19, indexOf("de_DE")), "Vergiftung", "Poison in German");
    checkEqual(fx::effectName(10, indexOf("ar_SA")), "تجدد الصحة", "Regeneration in Arabic");
    checkEqual(fx::effectName(1, indexOf("ar_SA")), "سرعة", "Speed in Arabic");
    checkEqual(fx::effectName(1, indexOf("ja_JP")), "移動速度上昇", "Speed in Japanese");
    checkEqual(fx::effectName(25, fx::kFallbackLanguage), "Fatal Poison", "Fatal Poison in English");
    checkEqual(fx::effectName(37, fx::kFallbackLanguage), "Breath of the Nautilus",
               "Nautilus name has no stray leading space");
    checkEqual(fx::effectName(0, fx::kFallbackLanguage), "Unknown Effect", "id 0 reads as unknown");
    checkEqual(fx::effectName(999, indexOf("fr_FR")), "Effet inconnu", "out-of-range id is localized unknown");
    checkEqual(fx::effectName(3, fx::languageCount() + 5), "Haste", "bad language index falls back to English");
    checkEqual(fx::infiniteDurationLabel(indexOf("de_DE")), "∞", "infinite duration label");
    checkEqual(fx::unknownEffectLabel(indexOf("ar_SA")), "تأثير غير معروف", "unknown label in Arabic");

    std::printf("system font flags\n");
    check(!fx::languageNeedsSystemFont(fx::kFallbackLanguage), "English uses the bundled pixel font");
    check(!fx::languageNeedsSystemFont(indexOf("ru_RU")), "Cyrillic is covered by the pixel font");
    check(!fx::languageNeedsSystemFont(indexOf("el_GR")), "Greek is covered by the pixel font");
    check(fx::languageNeedsSystemFont(indexOf("ar_SA")), "Arabic needs the system font");
    check(fx::languageNeedsSystemFont(indexOf("ja_JP")), "Japanese needs the system font");
    check(fx::languageNeedsSystemFont(indexOf("zh_CN")), "Chinese needs the system font");
    check(fx::languageNeedsSystemFont(fx::languageCount() + 9) == false, "out-of-range language is safe");

    std::printf("options.txt parsing\n");
    checkEqual(std::string(fx::parseGameLanguage("game_language:en_US\n")),
               "en_US", "plain line");
    checkEqual(std::string(fx::parseGameLanguage("mp_username:Steve\r\ngame_language:ar_SA\r\nx:1\r\n")),
               "ar_SA", "CRLF endings and surrounding lines");
    checkEqual(std::string(fx::parseGameLanguage("# comment\ngame_skintypefull:Steve\ngame_language:fr_CA\n")),
               "fr_CA", "skips comments and other keys");
    checkEqual(std::string(fx::parseGameLanguage("  game_language: es_MX \n")),
               "es_MX", "tolerates padding spaces");
    checkEqual(std::string(fx::parseGameLanguage("game_lastcustomskinnew:game_language:en_US\n")),
               "", "key must be at line start");
    check(fx::parseGameLanguage("game_language:\n").empty(), "empty value means device default");
    check(fx::parseGameLanguage("game_language:").empty(), "empty value without trailing newline");
    check(fx::parseGameLanguage("").empty(), "empty file yields nothing");
    check(fx::parseGameLanguage("gfx_fov:70\nmp_server_visible:1\n").empty(),
          "file without the option yields nothing");
    checkEqual(std::string(fx::parseGameLanguage(std::string("x:1\n") + "game_language:uk_UA")),
               "uk_UA", "last line without newline");

    if (g_failures == 0) {
        std::printf("\nall checks passed\n");
        return 0;
    }
    std::printf("\n%d check(s) failed\n", g_failures);
    return 1;
}
