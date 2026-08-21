#!/usr/bin/env python3
"""Regenerates src/modules/hud/effecti18n.hpp — the localized effect-name table
used by the Effect Display HUD module.

Sources (all official Mojang translations):
  * Bedrock Edition resource-pack texts for the 29 languages Mojang ships in
    bedrock-samples (https://github.com/Mojang/bedrock-samples),
    subdirectory resource_pack/texts/<code>.lang. Effect names live under the
    `potion.*` / `effect.*` keys.
  * Java Edition ar_sa.json (https://github.com/misode/mcmeta, branch
    `assets`) for Arabic, which Bedrock's samples repository does not ship.

The script also embeds a small curated override list (see ARABIC_OVERRIDES and
FATAL_POISON below) for the two Bedrock-only effects that have no official
lang key (Fatal Poison and Breath of the Nautilus have no dedicated string in
some files, and a few Arabic strings needed cleanup), and it checks every
generated string against the character map of resources/minecraft.ttf so each
language knows whether the bundled pixel font can render it (otherwise the
module falls back to the launcher's system font, which handles Arabic/CJK and
proper text shaping).

Example:
    python3 scripts/gen_effect_translations.py \
        --bedrock-texts /path/to/bedrock-samples/resource_pack/texts \
        --java-ar-sa   /path/to/mcmeta/assets/minecraft/lang/ar_sa.json \
        --font         resources/minecraft.ttf

The generated header is committed so normal builds never need the sources.
"""

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUTPUT = ROOT / "src" / "modules" / "hud" / "effecti18n.hpp"

# Bedrock effect id -> lang key (bedrock-samples resource_pack/texts/*.lang).
# Index 0 is Bedrock's internal "no effect" value and is never displayed.
BEDROCK_KEYS = {
    1: "potion.moveSpeed",
    2: "potion.moveSlowdown",
    3: "potion.digSpeed",
    4: "potion.digSlowDown",
    5: "potion.damageBoost",
    6: "potion.heal",
    7: "potion.harm",
    8: "potion.jump",
    9: "potion.confusion",
    10: "potion.regeneration",
    11: "potion.resistance",
    12: "potion.fireResistance",
    13: "potion.waterBreathing",
    14: "potion.invisibility",
    15: "potion.blindness",
    16: "potion.nightVision",
    17: "potion.hunger",
    18: "potion.weakness",
    19: "potion.poison",
    20: "potion.wither",
    21: "potion.healthBoost",
    22: "potion.absorption",
    23: "potion.saturation",
    24: "potion.levitation",
    26: "potion.conduitPower",
    27: "potion.slowFalling",
    28: "effect.badOmen",
    29: "effect.villageHero",
    30: "effect.darkness",
    31: "effect.trial_omen",
    32: "effect.wind_charged",
    33: "effect.weaving",
    34: "effect.oozing",
    35: "effect.infested",
    36: "effect.raid_omen",
    37: "effect.customNautilus",
}

# Bedrock effect id -> Java Edition translation key (used for Arabic only).
JAVA_KEYS = {
    1: "effect.minecraft.speed",
    2: "effect.minecraft.slowness",
    3: "effect.minecraft.haste",
    4: "effect.minecraft.mining_fatigue",
    5: "effect.minecraft.strength",
    6: "effect.minecraft.instant_health",
    7: "effect.minecraft.instant_damage",
    8: "effect.minecraft.jump_boost",
    9: "effect.minecraft.nausea",
    10: "effect.minecraft.regeneration",
    11: "effect.minecraft.resistance",
    12: "effect.minecraft.fire_resistance",
    13: "effect.minecraft.water_breathing",
    14: "effect.minecraft.invisibility",
    15: "effect.minecraft.blindness",
    16: "effect.minecraft.night_vision",
    17: "effect.minecraft.hunger",
    18: "effect.minecraft.weakness",
    19: "effect.minecraft.poison",
    20: "effect.minecraft.wither",
    21: "effect.minecraft.health_boost",
    22: "effect.minecraft.absorption",
    23: "effect.minecraft.saturation",
    24: "effect.minecraft.levitation",
    26: "effect.minecraft.conduit_power",
    27: "effect.minecraft.slow_falling",
    28: "effect.minecraft.bad_omen",
    29: "effect.minecraft.hero_of_the_village",
    30: "effect.minecraft.darkness",
    31: "effect.minecraft.trial_omen",
    32: "effect.minecraft.wind_charged",
    33: "effect.minecraft.weaving",
    34: "effect.minecraft.oozing",
    35: "effect.minecraft.infested",
    36: "effect.minecraft.raid_omen",
    37: "effect.minecraft.breath_of_the_nautilus",
}

INFINITE_KEY = "effect.duration.infinite"

# The 29 languages Mojang ships for Bedrock (languages.json order, en_US
# first because it is the runtime fallback), plus Arabic from Java Edition.
BEDROCK_CODES = [
    "en_US", "en_GB", "de_DE", "es_ES", "es_MX", "fr_FR", "fr_CA", "it_IT",
    "ja_JP", "ko_KR", "pt_BR", "pt_PT", "ru_RU", "zh_CN", "zh_TW", "nl_NL",
    "bg_BG", "cs_CZ", "da_DK", "el_GR", "fi_FI", "hu_HU", "id_ID", "nb_NO",
    "pl_PL", "sk_SK", "sv_SE", "tr_TR", "uk_UA",
]
ARABIC_CODE = "ar_SA"

# Native names for the in-menu language picker (shortened from Mojang's
# language_names.json, with regions kept where two variants share a name).
NATIVE_NAMES = {
    "en_US": "English (US)", "en_GB": "English (UK)",
    "de_DE": "Deutsch", "es_ES": "Español (ES)", "es_MX": "Español (MX)",
    "fr_FR": "Français (FR)", "fr_CA": "Français (CA)", "it_IT": "Italiano",
    "ja_JP": "日本語", "ko_KR": "한국어", "pt_BR": "Português (BR)",
    "pt_PT": "Português (PT)", "ru_RU": "Русский", "zh_CN": "简体中文",
    "zh_TW": "繁體中文", "nl_NL": "Nederlands", "bg_BG": "Български",
    "cs_CZ": "Čeština", "da_DK": "Dansk", "el_GR": "Ελληνικά",
    "fi_FI": "Suomi", "hu_HU": "Magyar", "id_ID": "Bahasa Indonesia",
    "nb_NO": "Norsk", "pl_PL": "Polski", "sk_SK": "Slovenčina",
    "sv_SE": "Svenska", "tr_TR": "Türkçe", "uk_UA": "Українська",
    "ar_SA": "العربية",
}

# Fatal Poison (id 25) is Bedrock-exclusive and has no official lang key.
# These use each language's official word for "poison" (taken from the same
# lang files) with the standard "fatal/deadly" adjective; ja/pt/es/zh follow
# the Minecraft Wiki's established translations.
FATAL_POISON = {
    "en_US": "Fatal Poison", "en_GB": "Fatal Poison",
    "bg_BG": "Смъртоносна отрова", "cs_CZ": "Smrtelná otrava",
    "da_DK": "Dødelig forgiftning", "de_DE": "Tödliche Vergiftung",
    "el_GR": "Θανατηφόρο δηλητήριο", "es_ES": "Veneno fatal",
    "es_MX": "Veneno fatal", "fi_FI": "Tappava myrkky",
    "fr_FR": "Poison fatal", "fr_CA": "Poison fatal",
    "hu_HU": "Halálos méreg", "id_ID": "Racun fatal",
    "it_IT": "Avvelenamento letale", "ja_JP": "致死毒",
    "ko_KR": "치명적인 독", "nb_NO": "Dødelig gift", "nl_NL": "Dodelijk vergif",
    "pl_PL": "Śmiertelna trucizna", "pt_BR": "Veneno fatal",
    "pt_PT": "Veneno fatal", "ru_RU": "Смертельное отравление",
    "sk_SK": "Smrteľná otrava", "sv_SE": "Dödlig förgiftning",
    "tr_TR": "Ölümcül zehir", "uk_UA": "Смертельна отрута",
    "zh_CN": "致命中毒", "zh_TW": "致命中毒",
    "ar_SA": "سم قاتل",
}

# "Unknown Effect" is this module's own fallback text, so it is translated
# here rather than pulled from a game key.
UNKNOWN_EFFECT = {
    "en_US": "Unknown Effect", "en_GB": "Unknown Effect",
    "bg_BG": "Непознат ефект", "cs_CZ": "Neznámý efekt",
    "da_DK": "Ukendt effekt", "de_DE": "Unbekannter Effekt",
    "el_GR": "Άγνωστο εφέ", "es_ES": "Efecto desconocido",
    "es_MX": "Efecto desconocido", "fi_FI": "Tuntematon efekti",
    "fr_FR": "Effet inconnu", "fr_CA": "Effet inconnu",
    "hu_HU": "Ismeretlen hatás", "id_ID": "Efek tidak diketahui",
    "it_IT": "Effetto sconosciuto", "ja_JP": "不明な効果",
    "ko_KR": "알 수 없는 효과", "nb_NO": "Ukjent effekt",
    "nl_NL": "Onbekend effect", "pl_PL": "Nieznany efekt",
    "pt_BR": "Efeito desconhecido", "pt_PT": "Efeito desconhecido",
    "ru_RU": "Неизвестный эффект", "sk_SK": "Neznámy efekt",
    "sv_SE": "Okänd effekt", "tr_TR": "Bilinmeyen efekt",
    "uk_UA": "Невідомий ефект", "zh_CN": "未知效果", "zh_TW": "未知效果",
    "ar_SA": "تأثير غير معروف",
}

# The Arabic file contains a few rough strings; these use the wording the
# Arabic Minecraft community already uses for those effects.
ARABIC_OVERRIDES = {
    20: "الذبول",        # Wither (official file: "رئيس الذبل")
    32: "مشحون بالرياح",  # Wind Charged (official file has odd word order)
    35: "الغزو",          # Infested (matches Arabic potion guides)
}


def parse_bedrock_lang(path: Path) -> dict:
    entries = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        entries[key.strip()] = value.strip().strip('"')
    return entries


def ttf_cmap_characters(path: Path) -> set:
    """Characters covered by the font's cmap (format 4/6/12 subtables)."""
    data = path.read_bytes()
    num_tables = struct.unpack(">H", data[4:6])[0]
    tables = {}
    for i in range(num_tables):
        off = 12 + i * 16
        tag = data[off:off + 4].decode("latin-1")
        tables[tag] = struct.unpack(">III", data[off + 4:off + 16])[1]
    chars = set()
    for start, count in ((tables["cmap"] + 4, struct.unpack(">H", data[tables["cmap"] + 2:tables["cmap"] + 4])[0]),):
        for i in range(count):
            rec = start + i * 8
            _, _, sub_off = struct.unpack(">HHI", data[rec:rec + 8])
            sub = tables["cmap"] + sub_off
            fmt = struct.unpack(">H", data[sub:sub + 2])[0]
            if fmt == 4:
                seg_count = struct.unpack(">H", data[sub + 6:sub + 8])[0] // 2
                end_off, start_off = sub + 14, sub + 14 + seg_count * 2 + 2
                for s in range(seg_count):
                    end_code = struct.unpack(">H", data[end_off + s * 2:end_off + s * 2 + 2])[0]
                    start_code = struct.unpack(">H", data[start_off + s * 2:start_off + s * 2 + 2])[0]
                    for c in range(start_code, end_code + 1):
                        if c != 0xFFFF:
                            chars.add(c)
            elif fmt == 6:
                first, count2 = struct.unpack(">HH", data[sub + 6:sub + 10])
                chars.update(range(first, first + count2))
            elif fmt == 12:
                groups = struct.unpack(">I", data[sub + 12:sub + 16])[0]
                for g in range(groups):
                    lo, hi, _ = struct.unpack(">III", data[sub + 16 + g * 12:sub + 16 + g * 12 + 12])
                    chars.update(range(lo, hi + 1))
    return chars


def utf8_codepoints(text: str):
    for byte in text.encode("utf-8"):
        yield byte


def needs_system_font(strings, covered: set) -> bool:
    """True when any character of any string is missing from the font.

    Walks the raw UTF-8 bytes of each string and decodes them back into
    code points; a leading byte >= 0x80 belongs to a multi-byte sequence,
    so checking only the ASCII range plus continuation bytes is not enough.
    """
    for text in strings:
        encoded = text.encode("utf-8")
        i = 0
        while i < len(encoded):
            byte = encoded[i]
            if byte < 0x80:
                code = byte
                length = 1
            elif byte & 0xE0 == 0xC0:
                code, length = byte & 0x1F, 2
            elif byte & 0xF0 == 0xE0:
                code, length = byte & 0x0F, 3
            else:
                code, length = byte & 0x07, 4
            for extra in encoded[i + 1:i + length]:
                code = (code << 6) | (extra & 0x3F)
            if code not in covered:
                return True
            i += length
    return False


def cpp_string(text: str) -> str:
    out = text.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{out}"'


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bedrock-texts", required=True, type=Path,
                        help="bedrock-samples resource_pack/texts directory")
    parser.add_argument("--java-ar-sa", required=True, type=Path,
                        help="mcmeta assets ar_sa.json (official Arabic)")
    parser.add_argument("--font", default=ROOT / "resources" / "minecraft.ttf",
                        type=Path, help="bundled HUD font used to test coverage")
    parser.add_argument("--check", action="store_true",
                        help="verify the committed header matches instead of writing")
    args = parser.parse_args()

    import json
    bedrock = {code: parse_bedrock_lang(args.bedrock_texts / f"{code}.lang")
               for code in BEDROCK_CODES}
    english = bedrock["en_US"]
    arabic = json.loads(args.java_ar_sa.read_text(encoding="utf-8"))
    covered = ttf_cmap_characters(args.font)

    codes = BEDROCK_CODES + [ARABIC_CODE]
    languages = []
    report = []
    for code in codes:
        names = [""]
        for effect_id in range(1, 38):
            if code == ARABIC_CODE:
                if effect_id in ARABIC_OVERRIDES:
                    value = ARABIC_OVERRIDES[effect_id]
                elif effect_id == 25:
                    value = FATAL_POISON[code]
                elif effect_id in JAVA_KEYS:
                    value = arabic.get(JAVA_KEYS[effect_id], "")
                else:
                    value = ""
            elif effect_id == 25:
                value = FATAL_POISON[code]
            else:
                value = bedrock[code].get(BEDROCK_KEYS[effect_id], "")
            value = value.strip().strip('"')
            if not value:
                value = english.get(BEDROCK_KEYS.get(effect_id, ""), "")
                report.append(f"{code}: effect {effect_id} fell back to English")
            names.append(value)

        if code == ARABIC_CODE:
            infinite = arabic.get(INFINITE_KEY, "∞")
        else:
            infinite = bedrock[code].get(INFINITE_KEY, english.get(INFINITE_KEY, "∞"))
        unknown = UNKNOWN_EFFECT[code]

        system_font = needs_system_font(names[1:] + [infinite, unknown], covered)
        languages.append({
            "code": code,
            "native": NATIVE_NAMES[code],
            "system_font": system_font,
            "names": names,
            "infinite": infinite.strip().strip('"') or "∞",
            "unknown": unknown,
        })

    for line in report:
        print(f"  note: {line}", file=sys.stderr)

    # The table itself: index 0 is English (en_US), the universal fallback.
    array_lines = ["// Index 0 is English (en_US): the universal fallback for unknown codes.",
                   f"inline constexpr std::array<EffectLanguage, {len(languages)}> kLanguages{{{{"]
    for lang in languages:
        rows = []
        for start_index in range(0, len(lang["names"]), 4):
            chunk = ", ".join(cpp_string(v) for v in lang["names"][start_index:start_index + 4])
            rows.append(f"        {chunk},")
        body = "\n".join(rows)
        flag = "true" if lang["system_font"] else "false"
        array_lines.append(f"    // {lang['code']} — {lang['native']}")
        array_lines.append(
            f"    {{ {cpp_string(lang['code'])}, {cpp_string(lang['native'])}, {flag},\n"
            f"      {{\n{body}\n      }},\n"
            f"      {cpp_string(lang['infinite'])}, {cpp_string(lang['unknown'])} }},"
        )
    array_lines.append("}};")

    doc = []
    doc.append("#pragma once")
    doc.append("")
    doc.append("// ---------------------------------------------------------------------------")
    doc.append("// Localized names for the Effect Display HUD module.")
    doc.append("//")
    doc.append("// Effect names follow the game's language setting: the module reads the")
    doc.append("// `game_language` option out of the game's options.txt (see")
    doc.append("// effectdisplay.cpp) and picks the matching table below, falling back to")
    doc.append("// English. The strings are Mojang's own translations, generated by")
    doc.append("// scripts/gen_effect_translations.py from:")
    doc.append("//   * Bedrock Edition texts (github.com/Mojang/bedrock-samples,")
    doc.append("//     resource_pack/texts) for the 29 languages Mojang ships, and")
    doc.append("//   * Java Edition ar_sa.json (github.com/misode/mcmeta) for Arabic.")
    doc.append("// Fatal Poison (Bedrock-only, no official lang key) and this module's own")
    doc.append("// \"Unknown Effect\" fallback are translated inside that script.")
    doc.append("//")
    doc.append("// `systemFont` marks languages the bundled pixel font cannot render (it")
    doc.append("// has no Arabic/CJK glyphs); those names are drawn with the launcher's")
    doc.append("// default system font instead, which also shapes RTL text correctly. The")
    doc.append("// flags are computed against resources/minecraft.ttf by the generator.")
    doc.append("//")
    doc.append("// Everything here is dependency-free so it can be unit tested without")
    doc.append("// the game (see tests/effecti18n_test.cpp). Do not edit by hand —")
    doc.append("// regenerate with scripts/gen_effect_translations.py.")
    doc.append("// ---------------------------------------------------------------------------")
    doc.append("")
    doc.append("#include <array>")
    doc.append("#include <cstddef>")
    doc.append("#include <cstdint>")
    doc.append("#include <string_view>")
    doc.append("")
    doc.append("namespace bedrocktools::hud::effects {")
    doc.append("")
    doc.append("// Number of effect slots per language, matching Bedrock's built-in effect")
    doc.append("// ids; index 0 is the internal no-effect value and stays empty.")
    doc.append(f"inline constexpr std::size_t kLocalizedEffectCount = {max(BEDROCK_KEYS) + 1};")
    doc.append("")
    doc.append("struct EffectLanguage {")
    doc.append("    const char* code;              // game language code, e.g. \"en_US\"")
    doc.append("    const char* nativeName;        // label shown in the mod-menu picker")
    doc.append("    bool systemFont;               // true -> draw with the system font")
    doc.append("    const char* const effectNames[kLocalizedEffectCount];")
    doc.append("    const char* infiniteDuration;  // label for endless effects")
    doc.append("    const char* unknownEffect;     // fallback for unknown ids")
    doc.append("};")
    doc.append("")
    doc.extend(array_lines)
    doc.append("")
    doc.append("// Label of the mod-menu option that follows the game language.")
    doc.append("inline constexpr const char* kAutoLanguageOption = \"Auto\";")
    doc.append("")
    doc.append("// English is index 0 and doubles as the fallback for unknown ids/codes.")
    doc.append("inline constexpr std::size_t kFallbackLanguage = 0;")
    doc.append("")
    doc.append("inline std::size_t languageCount() { return kLanguages.size(); }")
    doc.append("")
    doc.append("// Matches a game language code against the table: an exact code wins")
    doc.append("// (\"pt_BR\"), otherwise the language part is tried (\"ar_AE\" -> Arabic,")
    doc.append("// \"ja\" -> Japanese). Returns -1 when nothing matches.")
    doc.append("inline int languageIndexForCode(std::string_view code) {")
    doc.append("    if (code.empty()) return -1;")
    doc.append("    for (std::size_t i = 0; i < kLanguages.size(); ++i) {")
    doc.append("        if (code == kLanguages[i].code) return static_cast<int>(i);")
    doc.append("    }")
    doc.append("    const auto separator = code.find('_');")
    doc.append("    const std::string_view base = separator == std::string_view::npos")
    doc.append("        ? code : code.substr(0, separator);")
    doc.append("    for (std::size_t i = 0; i < kLanguages.size(); ++i) {")
    doc.append("        const std::string_view known = kLanguages[i].code;")
    doc.append("        if (known.size() > base.size() && known.compare(0, base.size(), base) == 0")
    doc.append("            && known[base.size()] == '_') {")
    doc.append("            return static_cast<int>(i);")
    doc.append("        }")
    doc.append("    }")
    doc.append("    return -1;")
    doc.append("}")
    doc.append("")
    doc.append("// Localized name of a status effect. Unknown or empty ids return the")
    doc.append("// localized \"Unknown Effect\" fallback; gaps fall back to English.")
    doc.append("inline const char* effectName(std::uint32_t effectId, std::size_t language) {")
    doc.append("    if (language >= kLanguages.size()) language = kFallbackLanguage;")
    doc.append("    const auto& lang = kLanguages[language];")
    doc.append("    if (effectId == 0 || effectId >= kLocalizedEffectCount) return lang.unknownEffect;")
    doc.append("    const char* name = lang.effectNames[effectId];")
    doc.append("    if (name == nullptr || *name == '\\0') name = kLanguages[kFallbackLanguage].effectNames[effectId];")
    doc.append("    return name;")
    doc.append("}")
    doc.append("")
    doc.append("inline const char* infiniteDurationLabel(std::size_t language) {")
    doc.append("    if (language >= kLanguages.size()) language = kFallbackLanguage;")
    doc.append("    return kLanguages[language].infiniteDuration;")
    doc.append("}")
    doc.append("")
    doc.append("inline const char* unknownEffectLabel(std::size_t language) {")
    doc.append("    if (language >= kLanguages.size()) language = kFallbackLanguage;")
    doc.append("    return kLanguages[language].unknownEffect;")
    doc.append("}")
    doc.append("")
    doc.append("// True when this language must be drawn with the launcher's system font")
    doc.append("// because the bundled pixel font has no glyphs for its script.")
    doc.append("inline bool languageNeedsSystemFont(std::size_t language) {")
    doc.append("    return language < kLanguages.size() && kLanguages[language].systemFont;")
    doc.append("}")
    doc.append("")
    doc.append("// Extracts the `game_language` value (e.g. \"ar_SA\") out of the contents")
    doc.append("// of a Bedrock options.txt. Returns an empty view when the option is")
    doc.append("// missing or empty (the game then follows the device default). Lines are")
    doc.append("// `key:value`; both \\n and \\r\\n endings are tolerated.")
    doc.append("inline std::string_view parseGameLanguage(std::string_view fileContents) {")
    doc.append("    std::string_view rest = fileContents;")
    doc.append("    while (!rest.empty()) {")
    doc.append("        auto lineEnd = rest.find('\\n');")
    doc.append("        std::string_view line = rest.substr(0, lineEnd);")
    doc.append("        rest = lineEnd == std::string_view::npos ? std::string_view{} : rest.substr(lineEnd + 1);")
    doc.append("        while (!line.empty() && (line.back() == '\\r' || line.back() == ' ')) line.remove_suffix(1);")
    doc.append("        while (!line.empty() && (line.front() == ' ' || line.front() == '\\t')) line.remove_prefix(1);")
    doc.append("        constexpr std::string_view kKey = \"game_language:\";")
    doc.append("        if (line.size() > kKey.size() && line.substr(0, kKey.size()) == kKey) {")
    doc.append("            std::string_view value = line.substr(kKey.size());")
    doc.append("            while (!value.empty() && value.front() == ' ') value.remove_prefix(1);")
    doc.append("            return value;")
    doc.append("        }")
    doc.append("    }")
    doc.append("    return {};")
    doc.append("}")
    doc.append("")
    doc.append("} // namespace bedrocktools::hud::effects")

    generated = "\n".join(doc) + "\n"

    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.exists() else ""
        if current == generated:
            print("effecti18n.hpp is up to date")
            return 0
        print("effecti18n.hpp differs from what the generator would produce", file=sys.stderr)
        return 1

    OUTPUT.write_text(generated, encoding="utf-8")
    print(f"wrote {OUTPUT} ({len(languages)} languages)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
