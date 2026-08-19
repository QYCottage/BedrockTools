#include "effectdisplay.hpp"

#include "effectformat.hpp"
#include "effectlayout.hpp"
#include "effectlocator.hpp"
#include "core/Runtime.hpp"
#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <entt/entt.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <limits>
#include <link.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace effectlocator = bedrocktools::hud::effectlocator;

// These names and entity traits intentionally match Bedrock's EnTT types.
// They must remain in the global namespace because EnTT's component IDs are
// generated from the fully-qualified C++ type names.
enum class EntityId : std::uint32_t {};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = std::uint32_t;
    using version_type = std::uint16_t;
    static constexpr std::uint32_t entity_mask = 0x3FFFF;
    static constexpr std::uint32_t version_mask = 0x3FFF;
};

template <>
struct entt::entt_traits<EntityId> : entt::basic_entt_traits<EntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};

// Minecraft stores status effects in this EnTT component as a vector of
// MobEffectInstance objects. Keeping the component as three raw vector
// pointers lets this module support adjacent Bedrock builds whose instance
// type has a different tail, while still validating every address and field.
struct MobEffectsComponent {
    std::uintptr_t begin;
    std::uintptr_t end;
    std::uintptr_t capacity;
};

class EntityRegistry;
class EntityContext {
public:
    entt::basic_registry<EntityId>& getRegistry() { return mEnTTRegistry; }

    template <class T>
    T* tryGetComponent() {
        return getRegistry().try_get<T>(mEntity);
    }

    EntityRegistry& mRegistry;
    entt::basic_registry<EntityId>& mEnTTRegistry;
    const EntityId mEntity;
};

namespace {

struct EffectDefinition {
    const char* name;
    std::uint32_t color;
};

// Bedrock's complete built-in effect range (including the 1.21 trial effects
// and Breath of the Nautilus). Index zero is the internal no-effect value.
constexpr std::array<EffectDefinition, 38> kEffects{{
    {"", 0x777777},
    {"Speed", 0x7CAFC6}, {"Slowness", 0x5A6C81}, {"Haste", 0xD9C043},
    {"Mining Fatigue", 0x4A4217}, {"Strength", 0x932423}, {"Instant Health", 0xF82423},
    {"Instant Damage", 0x430A09}, {"Jump Boost", 0x22FF4C}, {"Nausea", 0x551D4A},
    {"Regeneration", 0xCD5CAB}, {"Resistance", 0x99453A}, {"Fire Resistance", 0xE49A3A},
    {"Water Breathing", 0x2E5299}, {"Invisibility", 0x7F8392}, {"Blindness", 0x1F1F23},
    {"Night Vision", 0x1F1FA1}, {"Hunger", 0x587653}, {"Weakness", 0x484D48},
    {"Poison", 0x4E9331}, {"Wither", 0x352A27}, {"Health Boost", 0xF87D23},
    {"Absorption", 0x2552A5}, {"Saturation", 0xF82423}, {"Levitation", 0xCEFFFF},
    {"Fatal Poison", 0x4E9331}, {"Conduit Power", 0x1DC2D1}, {"Slow Falling", 0xF3CFB9},
    {"Bad Omen", 0x0B6138}, {"Hero of the Village", 0x44FF44}, {"Darkness", 0x292721},
    {"Trial Omen", 0x8F6F78}, {"Wind Charged", 0xB8D8D8}, {"Weaving", 0x78695A},
    {"Oozing", 0x99C256}, {"Infested", 0x8C9B8C}, {"Raid Omen", 0x6D485D},
    {"Breath of the Nautilus", 0x4EC2C8},
}};

EffectDisplayModule* g_effectDisplay = nullptr;

const EffectDefinition& definitionFor(std::uint32_t id) {
    static constexpr EffectDefinition unknown{"Unknown Effect", 0x888888};
    return id < kEffects.size() ? kEffects[id] : unknown;
}

// ---------------------------------------------------------------------------
// Vanilla effect icons
// ---------------------------------------------------------------------------
// The official Bedrock status-effect icons are 18x18 RGBA textures shipped
// with the game under `textures/ui/<effect>_effect.png`. Their pixel data is
// embedded below (base64, straight alpha) so the HUD draws the exact vanilla
// artwork — original size and colors — without any runtime texture loading.
// The data was decoded from the vanilla resource pack (Mojang/bedrock-samples).
constexpr int kEffectIconSize = 18;

// Maps a Bedrock effect id to the vanilla UI texture that holds its icon.
const char* getEffectIconPath(int effectId) {
    switch (effectId) {
        case 1:  return "textures/ui/speed_effect";
        case 2:  return "textures/ui/slowness_effect";
        case 3:  return "textures/ui/haste_effect";
        case 4:  return "textures/ui/mining_fatigue_effect";
        case 5:  return "textures/ui/strength_effect";
        // Instant Health (6), Instant Damage (7), Saturation (23) and Fatal
        // Poison (25) have no icon in the vanilla HUD; they share the generic
        // placeholder, as does any unknown id.
        case 8:  return "textures/ui/jump_boost_effect";
        case 9:  return "textures/ui/nausea_effect";
        case 10: return "textures/ui/regeneration_effect";
        case 11: return "textures/ui/resistance_effect";
        case 12: return "textures/ui/fire_resistance_effect";
        case 13: return "textures/ui/water_breathing_effect";
        case 14: return "textures/ui/invisibility_effect";
        case 15: return "textures/ui/blindness_effect";
        case 16: return "textures/ui/night_vision_effect";
        case 17: return "textures/ui/hunger_effect";
        case 18: return "textures/ui/weakness_effect";
        case 19: return "textures/ui/poison_effect";
        case 20: return "textures/ui/wither_effect";
        case 21: return "textures/ui/health_boost_effect";
        case 22: return "textures/ui/absorption_effect";
        case 24: return "textures/ui/levitation_effect";
        case 26: return "textures/ui/conduit_power_effect";
        case 27: return "textures/ui/slow_falling_effect";
        case 28: return "textures/ui/bad_omen_effect";
        case 29: return "textures/ui/village_hero_effect";
        case 30: return "textures/ui/darkness_effect";
        case 31: return "textures/ui/trial_omen_effect";
        case 32: return "textures/ui/wind_charged_effect";
        case 33: return "textures/ui/weaving_effect";
        case 34: return "textures/ui/oozing_effect";
        case 35: return "textures/ui/infested_effect";
        case 36: return "textures/ui/raid_omen_effect";
        case 37: return "textures/ui/breath_of_the_nautilus_effect";
        default: return "textures/ui/generic_effect";
    }
}

// One embedded vanilla icon: the texture path from getEffectIconPath() and its
// 18x18 RGBA pixels, base64-encoded (1296 bytes, no padding). The mod menu
// copies the decoded pixels when the icon is registered, so only this compact
// string table lives in the binary.
struct VanillaIconAsset {
    const char* path;
    const char* base64;
};

// 34 unique vanilla effect icons, 18x18 RGBA, base64 (straight alpha)
constexpr std::array<VanillaIconAsset, 34> kVanillaIconAssets{{
    {"textures/ui/speed_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wBQUFD/////AFBQUP9jYmL/Y2Ji/1BQUP9NFxP/TRcT/00XE/////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AE0XE/+LODP/izgz/2YlIf9NFxP/////AP///wD///8A"
        "////AMLCwv////8AwsLC/8LCwv/q6ur/6urq/+rq6v9NFxP/TRcT/zsNCv9mJSH/rVJN/61STf9mJSH/TRcT/////wD///8A"
        "////AP///wD///8A////AP///wAkCAX/TRcT/4s4M/+LODP/ZiUh/00XE/87DQr/izgz/61STf+LODP/izgz/00XE/////8A"
        "////AP///wD///8A////ACQIBf+LODP/Ow0K/61STf+tUk3/izgz/2YlIf87DQr/ZiUh/4s4M/+LODP/ZiUh/00XE/////8A"
        "////AP///wD///8A////ACQIBf+tUk3/Ow0K/4s4M/+tUk3/izgz/2YlIf9NFxP/Ow0K/2YlIf9mJSH/TRcT/zsNCv////8A"
        "////AP///wD///8A////AP///wAkCAX/izgz/zsNCv+LODP/izgz/4s4M/9mJSH/Ow0K/00XE/9NFxP/Ow0K/////wD///8A"
        "UFBQ/////wBQUFD/Y2Ji/2NiYv8kCAX/izgz/zsNCv9mJSH/izgz/2YlIf9NFxP/TRcT/zsNCv87DQr/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AJAgF/2YlIf87DQr/ZiUh/2YlIf9NFxP/Ow0K/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AJAgF/2YlIf87DQr/TRcT/2YlIf9mJSH/ZiUh/zsNCv87DQr/////AP///wD///8A"
        "////AP///wDCwsL/////AOrq6v////8AwsLC/+rq6v9mJSH/Ow0K/2YlIf9mJSH/izgz/4s4M/9mJSH/Ow0K/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////ACQIBf9mJSH/Ow0K/00XE/+LODP/rVJN/4s4M/+LODP/Ow0K/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////ACQIBf9NFxP/ZiUh/zsNCv87DQr/TRcT/zsNCv87DQr/Ow0K/zsNCv////8A"
        "////AP///wD///8AUFBQ/////wBjYmL/Y2Ji/1BQUP8kCAX/TRcT/2YlIf+LODP/rVJN/4s4M/+LODP/ZiUh/zsNCv////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AJAgF/yQIBf8kCAX/JAgF/yQIBf8kCAX/JAgF/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/slowness_effect",
        "////AP///wD///8A////AP///wD///8AEhIa/xISGv8SEhr/EhIa/xISGv////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wASEhr/KCgz/z8/Tf8/P03/KCgz/ygoM/8SEhr/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////ABISGv8/P03/V1dp/1dXaf9XV2n/Pz9N/z8/Tf8oKDP/EhIa/////wD///8A////AP///wD///8A"
        "////AP///wD///8AEhIa/ygoM/9XV2n/iYmT/4mJk/9XV2n/V1dp/ygoM/8/P03/KCgz/xISGv////8A////AP///wD///8A"
        "////AP///wD///8AEhIa/z8/Tf9XV2n/iYmT/4mJk/9XV2n/V1dp/z8/Tf8oKDP/Pz9N/xISGv////8A////AP///wD///8A"
        "////AP///wD///8AEhIa/z8/Tf9XV2n/V1dp/1dXaf9XV2n/V1dp/z8/Tf8oKDP/Pz9N/xISGv////8A////AP///wD///8A"
        "////AP///wD///8AEhIa/ygoM/8/P03/V1dp/1dXaf9XV2n/Pz9N/ygoM/8eHif/KCgz/xISGv////8A////AP///wD///8A"
        "////AP///wD///8AEhIa/ygoM/8oKDP/Pz9N/z8/Tf8oKDP/KCgz/ygoM/8eHif/KCgz/xISGv////8A////AP///wD///8A"
        "////AP///wD///8A////ABISGv8/P03/KCgz/ygoM/8eHif/Hh4n/x4eJ/8oKDP/Pz9N/1dXaf8SEhr/////AP///wD///8A"
        "////AP///wD///8A////AP///wASEhr/Pz9N/z8/Tf8/P03/Pz9N/ygoM/8eHif/////AD8/Tf8eHif/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AEhIa/xISGv8SEhr/EhIa/xISGv8oKDP/KCgz/x4eJ/8eHif/V1dp/x4eJ/////8A"
        "////AP///wBXV2n/Pz9N/1dXaf////8A////AP///wD///8A////AP///wASEhr/EhIa/x4eJ/8/P03/////AFdXaf////8A"
        "////AFdXaf8eHif/EhIa/xISGv9XV2n/////AP///wD///8A////AP///wD///8A////AP///wA/P03/iYmT/x4eJ/////8A"
        "////AD8/Tf+JiZP/Hh4n/xISGv8/P03/////AP///wD///8A////AP///wD///8A////AP///wBXV2n/Pz9N/////wD///8A"
        "////ABISGv9XV2n/V1dp/z8/Tf8SEhr/////AFdXaf8eHif/////AFdXaf8/P03/////AFdXaf8pPj//////AP///wD///8A"
        "////AP///wASEhr/EhIa/xISGv////8AHh4n/z8/Tf////8AEhIa/z8/Tf8eHif/Pz9N/x4eJ/////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/haste_effect",
        "////AP///wD///8AsmQR/7JkEf+yZBH/////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wCyZBH/6bEV//3/dv/9/3b/smQR/7JkEf////8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AsmQR/7JkEf/psRX//f92//3/dv+yZBH/smQR/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wCyZBH/smQR/+mxFf/9/3b//f92/7JkEf+yZBH/HA0F/xwNBf////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ALJkEf+yZBH/6bEV//3/dv/u4FD/dSgC/2s/I/8cDQX/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AsmQR/+7gUP/9/3b/7uBQ/3UoAv8cDQX/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8ALBQG/7JkEf/u4FD/6bEV/9yWE/91KAL/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wAsFAb/PyIP/2s/I/91KAL/3JYT/+mxFf91KAL/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ACwUBv8/Ig//az8j/z8iD/8cDQX/dSgC/9yWE//psRX/dSgC/////wD///8A"
        "////AP///wD///8A////AP///wD///8ALBQG/z8iD/9rPyP/PyIP/xwNBf////8A////AHUoAv/psRX/dSgC/////wD///8A"
        "////AP///wD///8A////AP///wAsFAb/Hh4n/2s/I/8/Ig//HA0F/////wD///8A////AHUoAv/clhP/6bEV/3UoAv////8A"
        "////AP///wD///8A////ACwUBv8eHif/Pz9N/x4eJ/8cDQX/////AP///wD///8A////AP///wB1KAL/6bEV/3UoAv////8A"
        "////AP///wD///8ALBQG/x4eJ/8/P03/Hh4n/xwNBf////8A////AP///wD///8A////AP///wB1KAL/3JYT/+mxFf91KAL/"
        "////AP///wAsFAb/Hh4n/z8/Tf8eHif/HA0F/////wD///8A////AP///wD///8A////AP///wD///8AdSgC/9yWE/91KAL/"
        "////ACwUBv8/Ig//az8j/x4eJ/8cDQX/////AP///wD///8A////AP///wD///8A////AP///wD///8AdSgC/9yWE/91KAL/"
        "LBQG/z8iD/9rPyP/PyIP/xwNBf////8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AHUoAv////8A"
        "LBQG/2s/I/8/Ig//HA0F/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////ABwNBf8cDQX/////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/mining_fatigue_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wAxNlb/MTZW/zE2Vv8xNlb/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////ADE2Vv/L0PX/y9D1/8vQ9f/L0PX/MTZW/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AMTZW/8vQ9f+ttOT/bnSb/2BjgP+Fi7D/y9D1/xAVNP////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wAxNlb/y9D1/4WLsP9gY4D/YGOA/2BjgP9udJv/y9D1/xAVNP////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wAxNlb/y9D1/4WLsP9gY4D/YGOA/250m/9udJv/rbTk/xAVNP////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wAxNlb/y9D1/6205P+Fi7D/hYuw/250m/+Fi7D/y9D1/xAVNP////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wAxNlb/y9D1/6205P+ttOT/hYuw/4WLsP+ttOT/EBU0/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////ADE2Vv+Fi7D/rbTk/8vQ9f/L0PX/y9D1/6205P8QFTT/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AMTZW/4WLsP+ttOT/bnSb/xAVNP8QFTT/EBU0/xAVNP////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wAxNlb/hYuw/6205P9udJv/EBU0/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AEBU0/zE2Vv+ttOT/rbTk/4WLsP8QFTT/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wAxNlb/bnSb/6205P/L0PX/bnSb/xAVNP////8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////ADE2Vv+ttOT/y9D1/8vQ9f+Fi7D/EBU0/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "MTZW/250m/+Fi7D/rbTk/8vQ9f9udJv/EBU0/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "MTZW/250m/+Fi7D/hYuw/250m/8QFTT/////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////ABAVNP9udJv/bnSb/xAVNP////8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wAQFTT/EBU0/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/strength_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wDY2Nj/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD/////////AP///wBERET/RERE/0RERP////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD/////////AERERP/Y2Nj/2NjY/xgYGP////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AL6+vv//////vr6+/9jY2P++vr7/2NjY/xgYGP////8A"
        "////AP///wD///8A////AP///wD///8A////ANjY2P/////////////////Y2Nj/////////////////2NjY/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AL6+vv//////2NjY/76+vv8YGBj/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8ARERE/9jY2P//////vr6+/xgYGP////8A////AP///wD///8A"
        "////AP///wD///8ARERE/0RERP////8A////AP///wBERET/2NjY/76+vv//////GBgY/////wD///8A////AP///wD///8A"
        "////AP///wD///8ARERE/2tra/9ERET/////AERERP/Y2Nj/vr6+/9jY2P++vr7/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AERERP+Wlpb/GBgY/9jY2P+Wlpb/2NjY/xgYGP////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AERERP+Wlpb/lpaW/2tra//Y2Nj/GBgY/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wBERET/a2tr/0RERP8YGBj/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AEk2Ff9oTh7/GBgY/0RERP9ERET/GBgY/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8ASTYV/4lnJ/8oHgv/////ABgYGP8YGBj/RERE/xgYGP////8A////AP///wD///8A////AP///wD///8A"
        "////AERERP9ERET/aE4e/ygeC/////8A////AP///wD///8AGBgY/xgYGP////8A////AP///wD///8A////AP///wD///8A"
        "////AERERP9ra2v/GBgY/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////ABgYGP8YGBj/GBgY/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/generic_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD///////////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD///////////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP//////////////////////////AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP//////////////////////////AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAD///////////////////////////////////////////////8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAD///////////////////////////////////////////////////////////////////////////////8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAD///////////////////////////////////////////////8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP//////////////////////////AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP//////////////////////////AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD///////////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD///////////////8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/////wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/jump_boost_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wBar8//Wq/P/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AFqvz/+a2Pr/mtj6/1qvz/////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AWq/P/5rY+v+a2Pr/mtj6/5rY+v9ar8//////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wBar8//mtj6/5rY+v+a2Pr/mtj6/5rY+v+a2Pr/Wq/P/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AFqvz/+a2Pr/mtj6/5rY+v+a2Pr/mtj6/5rY+v+a2Pr/mtj6/1qvz/////8A////AP///wD///8A"
        "////AP///wD///8ARY+r/0WPq/9Fj6v/Wq/P/5rY+v+a2Pr/mtj6/5rY+v9ar8//RY+r/0WPq/9Fj6v/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8ARY+r/5rY+v+a2Pr/mtj6/5rY+v9Fj6v/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8ARY+r/1qvz/9Fj6v/RY+r/1qvz/9Fj6v/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8ARY+r/0WPq//6+vr/+vr6/0WPq/9Fj6v/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AWq/P//r6+v/6+vr/+vr6//r6+v9ar8//////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wBar8//3vPy//r6+v/6+vr/+vr6//r6+v/e8/L/Wq/P/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AFqvz/+a2Pr/+vr6//r6+v/6+vr/+vr6//r6+v/6+vr/mtj6/1qvz/////8A////AP///wD///8A"
        "////AP///wD///8ARY+r/0WPq/9Fj6v/mtj6/97z8v/6+vr/+vr6/97z8v+a2Pr/RY+r/0WPq/9Fj6v/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8ARY+r/97z8v/6+vr/+vr6/97z8v9Fj6v/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8ARY+r/5rY+v/e8/L/3vPy/5rY+v9Fj6v/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8ARY+r/0WPq/9Fj6v/RY+r/0WPq/9Fj6v/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/nausea_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8ANksV/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/Om8a/zZLFf////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/NksV/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AT4wp/zpvGv82SxX/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/ZKY6/3bbTP9kpjr/NksV/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/dttM/3bbTP9220z/ZKY6/zZLFf////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/ZKY6/3bbTP9220z/dttM/zZLFf////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/T4wp/3bbTP9220z/ZKY6/zZLFf////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AOm8a/zpvGv86bxr/Om8a/2SmOv9kpjr/ZKY6/zZLFf////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wA6bxr/ZKY6/3bbTP9220z/dttM/3bbTP9kpjr/T4wp/zZLFf////8A////AP///wD///8A"
        "////AP///wD///8A////ADpvGv9kpjr/dttM/3bbTP9220z/dttM/2SmOv9PjCn/NksV/////wD///8A////AP///wD///8A"
        "////AP///wD///8AOm8a/0+MKf9kpjr/ZKY6/3bbTP9220z/ZKY6/0+MKf82SxX/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AOm8a/0+MKf86bxr/ZKY6/2SmOv9PjCn/NksV/zZLFf////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AT4wp/zZLFf////8ANksV/zZLFf82SxX/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AT4wp/zZLFf////8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AOm8a/zpvGv82SxX/////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////ADZLFf82SxX/////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/regeneration_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AGwQDP9sEAz/bBAM/////wD///8A////AP///wBsEAz/bBAM/zsEAv////8A////AP///wD///8A"
        "////AP///wD///8AbBAM/8YiO///Bgb/xiI7/zsEAv////8A////AGwQDP+hDgr//wYG/6EOCv87BAL/////AP///wD///8A"
        "////AP///wBsEAz/xiI7//8GBv//Bgb//wYG/8YiO/87BAL/OwQC/8YiO///Bgb//wYG//8GBv/GIjv/OwQC/////wD///8A"
        "////AGwQDP/GIjv//wYG//+Bgf//gYH//wYG//8GBv/GIjv/oQ4K//8GBv//Bgb//wYG//8GBv/GIjv/oQ4K/zsEAv////8A"
        "////AGwQDP//Bgb//wYG//+Bgf//gYH//wYG//8GBv//Bgb//wYG//8GBv//Bgb//wYG//8GBv/GIjv/xiI7/zsEAv////8A"
        "////AGwQDP//Bgb//wYG//8GBv//Bgb//wYG//8GBv//Bgb//wYG//8GBv//Bgb//wYG//8GBv/GIjv/xiI7/zsEAv////8A"
        "////AGwQDP/GIjv//wYG//8GBv//Bgb//wYG//8GBv//Bgb//wYG//8GBv//Bgb//wYG//8GBv/GIjv/oQ4K/zsEAv////8A"
        "////AP///wBsEAz//wYG//8GBv//Bgb//wYG//8GBv//Bgb//wYG//8GBv//Bgb//wYG/8YiO//GIjv/OwQC/////wD///8A"
        "////AP///wBsEAz/xiI7//8GBv//Bgb//wYG//8GBv//Bgb//wYG//8GBv//Bgb//wYG/8YiO/+hDgr/OwQC/////wD///8A"
        "////AP///wD///8AbBAM/8YiO///Bgb//wYG//8GBv//Bgb//wYG//8GBv//Bgb/xiI7/6EOCv87BAL/////AP///wD///8A"
        "////AP///wD///8A////AGwQDP/GIjv//wYG//8GBv//Bgb//wYG//8GBv/GIjv/oQ4K/zsEAv////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wA7BAL/xiI7//8GBv//Bgb/xiI7/8YiO/+hDgr/OwQC/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AOwQC/8YiO//GIjv/xiI7/6EOCv87BAL/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ADsEAv/GIjv/oQ4K/zsEAv////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA7BAL/OwQC/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/resistance_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8APkhi/z5IYv8+SGL/////AP///wD///8A////AP///wD///8APkhi/z5IYv8+SGL/////AP///wD///8A"
        "////AP///wA+SGL/lp2r/5adq/+Cipv/Pkhi/z5IYv8+SGL/Pkhi/z5IYv8+SGL/goqb/5adq/+Wnav/Pkhi/////wD///8A"
        "////AP///wA+SGL/XGR4/4KKm/+Wnav/lp2r/66zv/+us7//rrO//66zv/+Wnav/lp2r/4KKm/9cZHj/Pkhi/////wD///8A"
        "////AP///wA+SGL/XGR4/292if+Cipv/goqb/4KKm/+Cipv/goqb/4KKm/+Cipv/goqb/292if9cZHj/Pkhi/////wD///8A"
        "////AP///wA+SGL/XGR4/292if8/SF//P0hf/4KKm/+Cipv/goqb/4KKm/+Cipv/goqb/292if9cZHj/Pkhi/////wD///8A"
        "////AP///wA+SGL/TVZv/292if9cZHj/TVZv/01Wb/+Cipv/goqb/4KKm/+Cipv/goqb/292if9NVm//Pkhi/////wD///8A"
        "////AP///wA+SGL/TVZv/292if+Cipv/XGR4/01Wb/9NVm//goqb/z9IX/9cZHj/goqb/292if9NVm//Pkhi/////wD///8A"
        "////AP///wAeJDT/TVZv/292if+Cipv/goqb/1xkeP9NVm//TVZv/01Wb/+Cipv/goqb/292if9NVm//HiQ0/////wD///8A"
        "////AP///wAeJDT/TVZv/1xkeP+Cipv/goqb/4KKm/9cZHj/TVZv/z9IX/+Cipv/goqb/1xkeP9NVm//HiQ0/////wD///8A"
        "////AP///wAeJDT/P0hf/1xkeP+Cipv/goqb/z9IX/9NVm//P0hf/01Wb/9cZHj/goqb/1xkeP8/SF//HiQ0/////wD///8A"
        "////AP///wD///8AHiQ0/1xkeP9vdon/goqb/1xkeP+Cipv/goqb/1xkeP9cZHj/b3aJ/1xkeP8eJDT/////AP///wD///8A"
        "////AP///wD///8AHiQ0/01Wb/9vdon/goqb/4KKm/+Cipv/goqb/4KKm/+Cipv/b3aJ/01Wb/8eJDT/////AP///wD///8A"
        "////AP///wD///8A////AB4kNP9cZHj/b3aJ/4KKm/+Cipv/goqb/4KKm/9vdon/XGR4/x4kNP////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wAeJDT/P0hf/1xkeP9vdon/b3aJ/1xkeP8/SF//HiQ0/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AHiQ0/z9IX/8/SF//P0hf/z9IX/8eJDT/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AB4kNP8eJDT/HiQ0/x4kNP////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/fire_resistance_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AYj5O/2I+Tv9iPk7/////AP///wD///8A////AP///wD///8AYj5O/2I+Tv9iPk7/////AP///wD///8A"
        "////AP///wBiPk7/q5+W/6uflv+bhoL/Yj5O/2I+Tv9iPk7/Yj5O/2I+Tv9iPk7/m4aC/6uflv+rn5b/Yj5O/////wD///8A"
        "////AP///wBiPk7/eFxq/5uGgv+rn5b/q5+W/7+1rv+/ta7/v7Wu/7+1rv+rn5b/q5+W/5uGgv94XGr/Yj5O/////wD///8A"
        "////AP///wBiPk7/eFxq/4lvgf+bhoL/m4aC/5uGgv//qj//9NkZ/5uGgv+bhoL/m4aC/4lvgf94XGr/Yj5O/////wD///8A"
        "////AP///wBiPk7/eFxq/4lvgf+bhoL/m4aC//+qP//02Rn/m4aC/5uGgv+bhoL/m4aC/4lvgf94XGr/Yj5O/////wD///8A"
        "////AP///wBiPk7/b01d/4lvgf+bhoL/m4aC//+qP//02Rn//6o//5uGgv+bhoL/m4aC/4lvgf9vTV3/Yj5O/////wD///8A"
        "////AP///wBiPk7/b01d/4lvgf//qj///6o//5uGgv//qj//9NkZ//+qP/+bhoL/m4aC/4lvgf9vTV3/Yj5O/////wD///8A"
        "////AP///wA0Hib/b01d/4lvgf+bhoL/9NkZ/5uGgv+bhoL//6o///TZGf+bhoL/6owV//+qP/9vTV3/NB4m/////wD///8A"
        "////AP///wA0Hib/b01d/3hcav/02Rn//6o//5uGgv//qj//9NkZ///62f+bhoL//6o//3hcav9vTV3/NB4m/////wD///8A"
        "////AP///wA0Hib/Xz9a/3hcav/02Rn///rZ//+qP//02Rn///rZ//TZGf//qj//6owV/3hcav9fP1r/NB4m/////wD///8A"
        "////AP///wD///8ANB4m/3hcav//qj//9NkZ//TZGf//+tn///rZ/+qMFf//qj//y1ES/3hcav80Hib/////AP///wD///8A"
        "////AP///wD///8ANB4m/29NXf+Jb4H//6o//+qMFf//+tn/9NkZ/8tREv/qjBX/iW+B/29NXf80Hib/////AP///wD///8A"
        "////AP///wD///8A////ADQeJv94XGr/iW+B/5uGgv/LURL/y1ES/+qMFf+Jb4H/eFxq/zQeJv////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wA0Hib/Xz9a/3hcav+Jb4H/iW+B/3hcav9fP1r/NB4m/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8ANB4m/18/Wv9fP1r/Xz9a/18/Wv80Hib/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ADQeJv80Hib/NB4m/zQeJv////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/water_breathing_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AFqvz/9ar8//aLbT/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AWq/P/////wD///8A////AGi20/////8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wBar8//////APL////y////////AP///wBWob3/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wBar8//////APL///////8A////AP///wBWob3/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wBottP/////AP///wD///8A////AP///wBottP/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AaLbT/////wD///8A////AFqvz/////8A////AP///wBar8//Wq/P/2i20/////8A////AP///wD///8A"
        "////AP///wD///8A////AFahvf9ottP/gszo/////wD///8A////AFqvz/////8A////AP///wBottP/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AWq/P/////wDy////8v///////wD///8AVqG9/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AWq/P/////wDy////////AP///wD///8AVqG9/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AaLbT/////wD///8A////AP///wD///8AaLbT/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AGi20/////8A////AP///wBar8//////AP///wD///8A"
        "////AP///wD///8A////AP///wBar8//Wq/P/2i20/////8A////AP///wBWob3/aLbT/4LM6P////8A////AP///wD///8A"
        "////AP///wD///8A////AFqvz//y////////AP///wBWob3/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AFqvz/////8A////AP///wBWob3/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AGi20/////8A////AP///wBottP/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wBWob3/aLbT/4LM6P////8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/invisibility_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ALJkEf+yZBH/smQR/7JkEf////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wCyZBH/6bEV//3/dv/9/3b/7uBQ/+7gUP/psRX/smQR/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////ALJkEf/9/3b/7uBQ/3G+5/9xvuf/mtj6/3G+5//u4FD/6bEV/7JkEf////8A////AP///wD///8A"
        "////AP///wD///8A////AOmxFf/u4FD/cb7n/5rY+v//////wvb//5rY+v9xvuf/7uBQ/+mxFf////8A////AP///wD///8A"
        "////AP///wD///8AsmQR//3/dv9xvuf/mtj6////////////mtj6/5rY+v+a2Pr/cb7n/+mxFf+yZBH/////AP///wD///8A"
        "////AP///wD///8AsmQR//3/dv9xvuf///////////+a2Pr/mtj6/5rY+v/C9v//mtj6/+7gUP+yZBH/////AP///wD///8A"
        "////AP///wD///8AsmQR//3/dv+a2Pr/wvb//5rY+v+a2Pr/mtj6//////+a2Pr/cb7n/+7gUP+yZBH/////AP///wD///8A"
        "////AP///wD///8AsmQR//3/dv+a2Pr/mtj6/5rY+v+a2Pr//////8L2//9xvuf/cb7n//3/dv+yZBH/////AP///wD///8A"
        "////AP///wD///8AsmQR/+7gUP9xvuf/mtj6/5rY+v//////wvb//3G+5/9xvuf/cb7n//3/dv+yZBH/////AP///wD///8A"
        "////AP///wD///8AsmQR/+mxFf9xvuf/mtj6/8L2//+a2Pr/cb7n/3G+5/+a2Pr/cb7n//3/dv+yZBH/////AP///wD///8A"
        "////AP///wD///8A////ANyWE//psRX/mtj6/5rY+v9xvuf/cb7n/5rY+v9xvuf//f92/9yWE/////8A////AP///wD///8A"
        "////AP///wD///8A////ALJkEf/clhP/6bEV/3G+5/9xvuf/mtj6/3G+5//9/3b/6bEV/7JkEf////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wCyZBH/3JYT/+7gUP/u4FD//f92//3/dv/clhP/smQR/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ANyWE//clhP/3JYT/9yWE/////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A6bEV/+7gUP/psRX/3JYT/7JkEf/clhP/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wDclhP/smQR/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wDpsRX/3JYT/////wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/blindness_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AyAQE/8gEBP////8A////ABgmOf8YJjn/GCY5/xgmOf////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AyAQE//8GBv/IBAT/GCY5/5q7t////////////5q7t/8YJjn/GCY5/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AMgEBP//Win//1op//////////////////////+au7f/VXZy/xgmOf////8A////AP///wD///8A"
        "////AP///wD///8AGCY5/5q7t///Bgb//1op//9aKf8hISH/ISEh/1BraP///////////5q7t/8YJjn/////AP///wD///8A"
        "////AP///wAYJjn/mru3/////////////wYG//9aKf//Bgb/5+fn/6CgoP9Qa2j///////////+au7f/GCY5/////wD///8A"
        "////ABgmOf+au7f/////////////////ISEh//8GBv//Bgb//wYG/6CgoP8hISH/////////////////mru3/xgmOf////8A"
        "////ABIcKf9VdnL/////////////////ISEh/ywsLP/IBAT//wYG//8GBv8hISH/////////////////VXZy/xIcKf////8A"
        "////AP///wASHCn/mru3////////////UGto/6CgoP8sLCz/yAQE//8GBv//Bgb///////////+au7f/Ehwp/////wD///8A"
        "////AP///wD///8AEhwp/5q7t/+au7f/mru3/1BraP8hISH/ISEh/5ASEv/IBAT/yAQE/5q7t/8SHCn/////AP///wD///8A"
        "////AP///wD///8A////ABIcKf9VdnL/mru3/5q7t/+au7f/mru3/5q7t/+QEhL/yAQE/5ASEv////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wASHCn/Ehwp/1V2cv+au7f/mru3/1V2cv8SHCn/kBIS/8gEBP+QEhL/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ABIcKf8SHCn/Ehwp/xIcKf////8A////AJASEv+QEhL/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/night_vision_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AB8WVP8fFlT/HxZU/x8WVP////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wAfFlT/HxZU/y0/nv+EjfL/LT+e/y0/nv8fFlT/HxZU/////wD///8A////AP///wD///8A"
        "////AP///wD///8AHxZU/x8WVP+EjfL/LT+e/y0/nv/u4FD///SJ/+7gUP9bZMv/LT+e/x8WVP8fFlT/////AP///wD///8A"
        "////AP///wAfFlT/LT+e/y0/nv8tP57/W2TL/+7gUP//9In/1rdH/y0/nv8tP57/LT+e/4SN8v8tP57/HxZU/////wD///8A"
        "////ABALLf8uPIn/hI3y/ysedP8uPIn/LT+e/+7gUP//9In/LT+e/1tky/8tP57/Kx50/ysedP8rHnT/LjyJ/xALLf////8A"
        "////AP///wAQCy3/Kx50/ysedP8tP57/Kx50/9a3R//u4FD/1rdH/ysedP8rHnT/LjyJ/ysedP8tP57/EAst/////wD///8A"
        "////AP///wD///8AEAst/xALLf8rHnT/Kx50/ysedP/Wt0f/7uBQ/9a3R/+EjfL/Kx50/xALLf8QCy3/////AP///wD///8A"
        "////AP///wD///8A////AP///wAQCy3/EAst/ysedP8uPIn/Kx50/ysedP8QCy3/EAst/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ABALLf8QCy3/EAst/xALLf////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/hunger_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wCzroz/s66M/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////ALOujP////3////9/3t+a/9mLxL/TRcT/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AF9iTv////3/5+G7/6UnIv+lJyL/gkUk/00XE/////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////ADoRDv9fYk7/pSci/904MP/RLib/gkUk/2YvEv9NFxP/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////ADoRDv9mLxL/3Tgw/9EuJv+lJyL/r1Ig/4JFJP9mLxL/TRcT/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////ADoRDv+CRST/pSci/4JFJP+vUiD/r1Ig/69SIP+CRST/TRcT/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////ADoRDv9mLxL/gkUk/69SIP+CRST/gkUk/69SIP+vUiD/TRcT/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wA6EQ7/Zi8S/4JFJP9mLxL/gkUk/2YvEv+CRST/TRcT/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AOhEO/2YvEv9mLxL/Zi8S/4JFJP9NFxP/X2JO/7OujP////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ADoRDv86EQ7/OhEO/zoRDv9fYk7/s66M/+fhu/+zroz/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8AX2JO/+fhu//n4bv/e35r/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AF9iTv9fYk7/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/weakness_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8AGBgY/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wBERET/////AP///wAYGBj/a2tr/xgYGP////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AERERP/Y2Nj/RERE/////wD///8AGBgY/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AERERP////8A////AP///wBERET/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8ARERE/5aWlv9ERET/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AERERP////8ARERE/////wD///8A////AERERP////8A////AP///wD///8A"
        "////AP///wD///8ARERE/0RERP////8A////AP///wBERET/2NjY/0RERP////8ARERE/5aWlv9ERET/////AP///wD///8A"
        "////AP///wD///8ARERE/2tra/9ERET/////AERERP/Y2Nj/lpaW/0RERP////8A////AERERP////8A////AP///wD///8A"
        "////AP///wD///8A////AERERP+Wlpb/GBgY/76+vv+Wlpb/vr6+/xgYGP////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AERERP+Wlpb/lpaW/2tra/++vr7/GBgY/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wBERET/a2tr/0RERP8YGBj/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AEk2Ff9oTh7/GBgY/0RERP9ERET/GBgY/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8ASTYV/4lnJ/8oHgv/////ABgYGP8YGBj/RERE/xgYGP////8A////AP///wD///8A////AP///wD///8A"
        "////AERERP9ERET/aE4e/ygeC/////8A////AP///wD///8AGBgY/xgYGP////8A////AP///wD///8A////AP///wD///8A"
        "////AERERP9ra2v/GBgY/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////ABgYGP8YGBj/GBgY/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/poison_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////ADpvGv86bxr/Om8a/zpvGv82SxX/////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AOm8a/2SmOv9220z/dttM/3bbTP9kpjr/NksV/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/ZKY6/3bbTP9kpjr/dttM/2SmOv9220z/ZKY6/zZLFf////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/dttM/0+MKf9PjCn/dttM/0+MKf9PjCn/dttM/zZLFf////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/dttM/0+MKf9kpjr/dttM/2SmOv9PjCn/dttM/zZLFf////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6bxr/ZKY6/3bbTP9220z/T4wp/3bbTP9220z/ZKY6/zZLFf////8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8ANksV/2SmOv9220z/dttM/3bbTP9kpjr/NksV/////wD///8A"
        "////AP///wD///8AOm8a/zpvGv86bxr/////AP///wD///8ANksV/0+MKf9kpjr/T4wp/2SmOv9PjCn/NksV/////wD///8A"
        "////AP///wA6bxr/v+lw/3bbTP9220z/Om8a/////wD///8A////ADZLFf82SxX/NksV/zZLFf82SxX/////AP///wD///8A"
        "////AP///wA6bxr/dttM/3bbTP9kpjr/NksV/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wA6bxr/T4wp/2SmOv9PjCn/NksV/////wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8ANksV/zZLFf82SxX/////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////ADpvGv86bxr/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AOm8a/7/pcP9220z/Om8a/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8AOm8a/3bbTP9PjCn/NksV/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////ADZLFf82SxX/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/wither_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////ACwiIv8sIiL/LCIi/////wD///8A////AP///wAsIiL/LCIi/z8kK/////8A////AP///wD///8A"
        "////AP///wD///8ALCIi/1o3QP92SFf/WjdA/xoWFv////8A////ACwiIv92SFf/WjdA/z8kK/8aFhb/////AP///wD///8A"
        "////AP///wAsIiL/WjdA/3ZIV/92SFf/dkhX/1o3QP8aFhb/LCIi/1o3QP8/JCv/GhYW/xoWFv////8AWjdA/xoWFv////8A"
        "////ACwiIv9aN0D/dkhX/4hmcf+vo6f/iGZx/3ZIV/8/JCv/GhYW/z8kK/8aFhb/PyQr/////wA/JCv/PyQr/xoWFv////8A"
        "////ACwiIv92SFf/WjdA/6+jp/+IZnH/dkhX/3ZIV/9aN0D/PyQr/xoWFv////8A////AP///wD///8AGhYW/////wD///8A"
        "////ACwiIv92SFf/dkhX/4hmcf92SFf/dkhX/3ZIV/92SFf/PyQr/xoWFv8/JCv/////AD8kK/8/JCv/////AP///wD///8A"
        "////ACwiIv9aN0D/dkhX/3ZIV/9aN0D/dkhX/3ZIV/9aN0D/PyQr/z8kK/8aFhb/////AD8kK/92SFf/dkhX/z8kK/////8A"
        "////AP///wAsIiL/dkhX/1o3QP9aN0D/dkhX/1o3QP8/JCv/WjdA/z8kK/8aFhb/PyQr/1o3QP92SFf/WjdA/xoWFv////8A"
        "////AP///wAsIiL/WjdA/z8kK/92SFf/dkhX/1o3QP92SFf/dkhX/1o3QP8/JCv/GhYW/z8kK/8/JCv/GhYW/////wD///8A"
        "////AP///wD///8ALCIi/z8kK/92SFf/dkhX/3ZIV/92SFf/dkhX/1o3QP9aN0D/WjdA/xoWFv////8A////AP///wD///8A"
        "////AP///wD///8A////ACwiIv9aN0D/dkhX/3ZIV/92SFf/dkhX/1o3QP9aN0D/PyQr/xoWFv////8A////AP///wD///8A"
        "////AP///wD///8A////AP///wAaFhb/WjdA/3ZIV/92SFf/WjdA/1o3QP8/JCv/GhYW/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AGhYW/1o3QP9aN0D/WjdA/z8kK/8aFhb/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////ABoWFv9aN0D/PyQr/xoWFv////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wAaFhb/GhYW/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/health_boost_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AbBAM/2wQDP9sEAz/////AP///wD///8A////AGwQDP87BAL/OwQC/////wD///8A"
        "////AP///wD///8A////AP///wBsEAz//wYG//8GBv/GIjv/bBAM/////wD///8AbBAM/8YiO//GIjv/oQ4K/zsEAv////8A"
        "////AP///wD///8A////AGwQDP//Bgb//4GB//+Bgf//Bgb/xiI7/2wQDP9sEAz/xiI7//8GBv//Bgb/xiI7/6EOCv87BAL/"
        "////AP///wD///8A////AGwQDP//Bgb//4GB//+Bgf//Bgb//wYG//8GBv//Bgb//wYG//8GBv/GIjv/xiI7/6EOCv87BAL/"
        "////AP///wD///8A////AGwQDP9sEAz/bBAM/8YiO///Bgb/xiI7/2wQDP87BAL/xiI7//8GBv/GIjv/oQ4K/6EOCv87BAL/"
        "////AP///wD///8A////AGwQDP//Bgb/xiI7/2wQDP/GIjv/bBAM/8YiO/+hDgr/OwQC/6EOCv+hDgr/oQ4K/zsEAv////8A"
        "////AP///wD///8AbBAM//8GBv//gYH//wYG/8YiO/9sEAz/xiI7//8GBv/GIjv/oQ4K/zsEAv+hDgr/bBAM/zsEAv////8A"
        "////AP///wBsEAz/bBAM/8YiO/87BAL/OwQC//8GBv//Bgb//wYG//8GBv/GIjv/xiI7/zsEAv9sEAz/OwQC/////wD///8A"
        "////AGwQDP//Bgb/xiI7/zsEAv/GIjv/oQ4K/zsEAv/GIjv//wYG/8YiO//GIjv/oQ4K/zsEAv87BAL/////AP///wD///8A"
        "bBAM//8GBv//gYH//wYG//8GBv/GIjv/xiI7/6EOCv87BAL/xiI7/6EOCv+hDgr/OwQC/zsEAv////8A////AP///wD///8A"
        "bBAM//8GBv//Bgb//wYG//8GBv/GIjv/xiI7/8YiO/87BAL/oQ4K/6EOCv87BAL/OwQC/////wD///8A////AP///wD///8A"
        "bBAM/8YiO///Bgb//wYG/8YiO//GIjv/xiI7/6EOCv87BAL/bBAM/zsEAv87BAL/////AP///wD///8A////AP///wD///8A"
        "////AGwQDP+hDgr/xiI7/8YiO//GIjv/oQ4K/zsEAv9sEAz/OwQC/////wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wBsEAz/oQ4K/8YiO/+hDgr/OwQC/////wA7BAL/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8AbBAM/6EOCv87BAL/////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////ADsEAv////8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/absorption_effect",
        "////AP///wD///8AOqrE/////wD///8A////AP///wA6qsT/////AP///wD///8A////ADqqxP////8A////AP///wD///8A"
        "////AP///wAmXWj/pO3+/6Tt/v9m0On/Jl1o/////wA6qsT/NH6M/////wAmXWj/ZtDp/2bQ6f86qsT/Jl1o/////wD///8A"
        "////AP///wCk7f7/lqqr/5aqq/9+laX/pO3+/6Tt/v+k7f7/pO3+/2bQ6f9m0On/fpWl/5aqq/+Wqqv/OqrE/////wD///8A"
        "////AP///wCk7f7/UHqD/36Vpf+Wqqv/lqqr/66/v/+uv7//rr+//66/v/+Wqqv/lqqr/36Vpf9QeoP/ZtDp/////wD///8A"
        "////ADqqxP+k7f7/UHqD/2WFkv9+laX/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/fpWl/2WFkv9QeoP/ZtDp/zqqxP80foz/"
        "////AP///wBm0On/UHqD/2WFkv9+laX/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/fpWl/2WFkv9QeoP/ZtDp/zR+jP////8A"
        "////AP///wBm0On/P2d4/2WFkv9+laX/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/fpWl/2WFkv8/Z3j/OqrE/////wD///8A"
        "NH6M/zqqxP9m0On/P2d4/2WFkv9+laX/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/fpWl/2WFkv8/Z3j/OqrE/////wD///8A"
        "////ADR+jP86qsT/P2d4/2WFkv9+laX/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/fpWl/2WFkv8/Z3j/NH6M/////wD///8A"
        "////AP///wBm0On/P2d4/1B6g/9+laX/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/fpWl/1B6g/8/Z3j/OqrE/zR+jP////8A"
        "////AP///wA6qsT/Kltq/1B6g/9+laX/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/fpWl/1B6g/8qW2r/ZtDp/////wD///8A"
        "////AP///wAmXWj/OqrE/1B6g/9lhZL/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/ZYWS/1B6g/80foz/Jl1o/////wD///8A"
        "////AP///wA0foz/NH6M/z9neP9lhZL/fpWl/36Vpf9+laX/fpWl/36Vpf9+laX/ZYWS/z9neP86qsT/NH6M/////wD///8A"
        "////ADqqxP////8A////ADqqxP9QeoP/ZYWS/36Vpf9+laX/fpWl/36Vpf9lhZL/UHqD/zR+jP////8A////ADR+jP////8A"
        "////AP///wD///8A////ADR+jP80foz/Kltq/1B6g/9lhZL/ZYWS/1B6g/8qW2r/NH6M/zqqxP////8A////AP///wD///8A"
        "////AP///wD///8AOqrE/////wD///8ANH6M/ypbav8qW2r/Kltq/ypbav86qsT/////AP///wA6qsT/////AP///wD///8A"
        "////AP///wA6qsT/////AP///wD///8A////ADR+jP80foz/OqrE/zqqxP////8A////AP///wD///8AOqrE/////wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wA6qsT/////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/levitation_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AHW53v91ud7/dbne/3W53v////8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8Adbne/9Ps8f//+fX///n1/9Ps8f91ud7/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wB1ud7///n1///59f//+fX///n1///59f91ud7/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8Adbne/3W53v/T7PH///n1///59f//+fX///n1///59f/T7PH/dbne/////wD///8A////AP///wD///8A"
        "////AP///wB1ud7/0+zx///59f//+fX/0+zx/9Ps8f//+fX///n1/9Ps8f/T7PH/0+zx/3W53v91ud7/////AP///wD///8A"
        "////AHW53v+x3/f///n1///59f//+fX///n1///59f/T7PH/0+zx/7Hf9///+fX///n1/7Hf9/+x3/f/cJ63/////wD///8A"
        "////AHW53v/T7PH/0+zx///59f//+fX///n1///59f/T7PH/sd/3/9Ps8f//+fX///n1///59f/T7PH/sd/3/3Cet/////8A"
        "////AHW53v+x3/f/0+zx/9Ps8f//+fX///n1/9Ps8f+x3/f/0+zx///59f//+fX/0+zx/9Ps8f+x3/f/sd/3/3Cet/////8A"
        "////AP///wBwnrf/cJ63/7Hf9//T7PH/0+zx/9Ps8f+x3/f/0+zx/9Ps8f/T7PH/0+zx/3Cet/9wnrf/cJ63/////wD///8A"
        "////AP///wD///8A////AHCet/9wnrf/sd/3/7Hf9//T7PH/0+zx/9Ps8f+x3/f/cJ63/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8AcJ63/3Cet/9wnrf/cJ63/3Cet/9wnrf/////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AFVWVv9VVlb/VVZW/1VWVv9VVlb/VVZW/1VWVv9VVlb/VVZW/////wD///8A////AP///wD///8A"
        "////AP///wD///8AVVZW/1VWVv9VVlb/VVZW/1VWVv9VVlb/VVZW/1VWVv9VVlb/VVZW/1VWVv////8A////AP///wD///8A"
        "////AP///wD///8A////AFVWVv9VVlb/VVZW/1VWVv9VVlb/VVZW/1VWVv9VVlb/VVZW/////wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/conduit_power_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAs6Tf8LOk3/CzpN/ws6Tf8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAALOk3/CzpN/w9Ydf8dbY3/HW2N/w9Ydf8LOk3/CzpN/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAs6Tf8PWHX/HW2N/x3C0f8dwtH/H5ax/yGAof8dbY3/D1h1/ws6Tf8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAACzpN/w9Ydf8dbY3/HcLR/yja7v8o2u7/HcLR/x+Wsf8hgKH/HW2N/w9Ydf8LOk3/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAACzpN/x1tjf8flrH/HcLR//z6+P/nx3L/z58k/x+Wsf8flrH/IYCh/x1tjf8LOk3/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAALOk3/DUhf/yGAof8flrH/HcLR/+fHcv/PnyT/n3se/8+fJP8flrH/IYCh/x1tjf8NSF//CCBU/wAAAAAAAAAA"
        "AAAAAAAAAAALOk3/D1h1/yGAof8flrH/H5ax/8+fJP+fex7/ZSkf/8+fJP8flrH/IYCh/x1tjf8PWHX/CCBU/wAAAAAAAAAA"
        "AAAAAAAAAAALOk3/D1h1/yGAof8hgKH/H5ax/8+fJP9EIBL/ZSkf/597Hv8hgKH/IYCh/x1tjf8PWHX/CCBU/wAAAAAAAAAA"
        "AAAAAAAAAAALOk3/DUhf/x1tjf8hgKH/IYCh/597Hv9lKR//ZSkf/597Hv8hgKH/HW2N/x1tjf8NSF//CCBU/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAACCBU/w9Ydf8hgKH/IYCh/597Hv9lKR//dVAl/597Hv8dbY3/D1h1/x1tjf8IIFT/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAACCBU/w1IX/8dbY3/IYCh/yGAof+fex7/n3se/x1tjf8PWHX/HW2N/w9Ydf8IIFT/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAggVP8NSF//HW2N/x1tjf8dbY3/HW2N/x1tjf8dbY3/DUhf/wggVP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAIIFT/CCBU/w1IX/8PWHX/D1h1/w1IX/8IIFT/CCBU/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAggVP8IIFT/CCBU/wggVP8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/slow_falling_effect",
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wAAAAAAAAAAAAAAAAAAAAAAAAAAAO3iyf8AAAAA7eLJ///86////Ov/AAAAAAAAAAAAAAAAAAAAAP///wD///8A"
        "////AP///wAAAAAAAAAAAAAAAAAAAAAA7eLJ///86//t4sn/7eLJ/4Jtcv+CbXL/7eLJ/wAAAAAAAAAAAAAAAP///wD///8A"
        "////AP///wAAAAAAAAAAAO3iyf8AAAAA//zr/+3iyf+CbXL/gm1y/+3iyf/t4sn/AAAAAAAAAAAAAAAAAAAAAP///wD///8A"
        "////AP///wAAAAAAAAAAAP/86//t4sn/7eLJ/4Jtcv+ypo7/7eLJ/7Kmjv8AAAAAAAAAAAAAAAAAAAAAAAAAAP///wD///8A"
        "////AP///wAAAAAA7eLJ///86/+ypo7/gm1y/+3iyf///Ov/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP///wD///8A"
        "////AP///wAAAAAA//zr///86/+CbXL/7eLJ///86/8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADt4sn/AAAAAP///wD///8A"
        "////AP///wD//Ov///zr/4Jtcv/t4sn/7eLJ/wAAAADt4sn/AAAAAAAAAAAAAAAAAAAAAO3iyf///Ov/7eLJ/////wD///8A"
        "////AP///wDt4sn/gm1y/7Kmjv///Ov///zr/+3iyf8AAAAAAAAAAAAAAAAAAAAAAAAAAP/86/+CbXL///zr/////wD///8A"
        "////AP///wCypo7/gm1y///86////Ov/7eLJ/wAAAAAAAAAAAAAAAAAAAAAAAAAA7eLJ///86/+CbXL/7eLJ/////wD///8A"
        "////AP///wCCbXL/sqaO/+3iyf8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA//zr/4Jtcv///Ov/AAAAAP///wD///8A"
        "////AP///wCCbXL/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAD//Ov/7eLJ/4Jtcv/t4sn///zr/////wD///8A"
        "////AP///wCCbXL/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADt4sn/gm1y///86/8AAAAAAAAAAP///wD///8A"
        "////AP///wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAO3iyf+CbXL/7eLJ/+3iyf/t4sn/AAAAAP///wD///8A"
        "////AP///wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgm1y/4Jtcv/t4sn/7eLJ/wAAAAAAAAAAAAAAAP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
        "////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A////AP///wD///8A"
    },
    {"textures/ui/bad_omen_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAYLDn/AAAAAAAAAAAAAAAAAAAAAAAAAAAYLDn/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGCw5/ypEVf8qRFX/GCw5/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAYLDn/AAAAACpEVf8YLDn/AAAAAAAAAAAqRFX/QGJ4/ypEVf8iHSz/AAAAAAAAAAAYLDn/AAAAAAAAAAAAAAAA"
        "AAAAABgsOf8qRFX/GCw5/xgsOf8iHSz/AAAAACIdLP8YLDn/KkRV/yIdLP8iHSz/AAAAAAAAAAAqRFX/GCw5/wAAAAAAAAAA"
        "AAAAABgsOf9AYnj/KkRV/yIdLP8AAAAAAAAAAAAAAAAYLDn/GCw5/yIdLP8YLDn/AAAAABgsOf9AYnj/KkRV/xgsOf8AAAAA"
        "AAAAACpEVf9AYnj/QGJ4/ypEVf8YLDn/AAAAABgsOf8qRFX/GCw5/xgsOf8YLDn/GCw5/xgsOf8qRFX/GCw5/xgsOf8AAAAA"
        "AAAAAAAAAAAYLDn/KkRV/xgsOf8iHSz/KkRV/0BieP9AYnj/KkRV/xgsOf8qRFX/Ih0s/yIdLP8AAAAAIh0s/yIdLP8AAAAA"
        "AAAAABgsOf8AAAAAIh0s/ypEVf8iHSz/QAAB/yIdLP8qRFX/Ih0s/yIdLP9AAAH/Ih0s/0BieP8AAAAAIh0s/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAABgsOf9AAAH/qAAv//8ASP9AAAH/QAAB//8ASP+oAC//XQAO/yIdLP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAABgsOf8AAAAAGCw5/ypEVf8YLDn/Ih0s/0AAAf8YLDn/KkRV/0AAAf8iHSz/KkRV/yIdLP8YLDn/GCw5/wAAAAAAAAAA"
        "AAAAACpEVf9AYnj/QGJ4/ypEVf8qRFX/GCw5/yIdLP8iHSz/GCw5/yIdLP8YLDn/GCw5/0BieP8qRFX/KkRV/xgsOf8AAAAA"
        "AAAAABgsOf8qRFX/QGJ4/0BieP8YLDn/GCw5/xgsOf8iHSz/Ih0s/xgsOf8iHSz/AAAAABgsOf8qRFX/GCw5/yIdLP8AAAAA"
        "AAAAAAAAAAAYLDn/GCw5/wAAAAAYLDn/KkRV/yIdLP8AAAAAAAAAACIdLP8AAAAAAAAAAAAAAAAYLDn/Ih0s/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAIh0s/yIdLP8AAAAAGCw5/wAAAAAAAAAAAAAAAAAAAAAAAAAAGCw5/xgsOf8YLDn/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAqRFX/QGJ4/yIdLP8AAAAAAAAAACpEVf9AYnj/KkRV/yIdLP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGCw5/wAAAAAAAAAAAAAAAAAAAAAYLDn/Ih0s/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/village_hero_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABTAP8AUwD/AFMA/wBTAP8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAFMA/9v/6/9B84T/QfOE/xfdYv8AUwD/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAUwD/2//r/6/9zf9B84T/QfOE/wCqLP8X3WL/AC0A/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAABTAP/b/+v/r/3N/6/9zf9B84T/QfOE/wCqLP8Aqiz/F91i/wAtAP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAABTAP+v/c3/F91i/xfdYv+v/c3/r/3N/wCqLP8AlSn/AKos/wAtAP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAABTAP+v/c3/F91i/xfdYv+v/c3/gvat/wCqLP8Aexj/AKos/wAtAP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAABTAP+v/c3/F91i/xfdYv+v/c3/gvat/wCqLP8Aexj/AKos/wAtAP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAABTAP+v/c3/F91i/xfdYv+C9q3/gvat/wCqLP8Aexj/F91i/wAtAP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAABTAP+v/c3/F91i/xfdYv+C9q3/QfOE/wB7GP8AlSn/F91i/wAtAP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAUwD/AKos/wCVKf8Aexj/AHsY/wCVKf+C9q3/AC0A/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAKsRAP/jFwD/4xcA/wAAAAAAAAAAAFMA/wCqLP8AlSn/AHsY/xfdYv8ALQD/AAAAAAAAAADjFwD/4xcA/6sRAP8AAAAA"
        "AAAAAOMXAP9+Cgr/qxEA/wAAAAAAAAAAAAAAAAAtAP8ALQD/AC0A/wAtAP8AAAAAAAAAAAAAAACrEQD/fgoK/+MXAP8AAAAA"
        "AAAAAKsRAP9+Cgr/qxEA/+MXAP8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAOMXAP+rEQD/fgoK/6sRAP8AAAAA"
        "AAAAAH4KCv8AAAAAfgoK/6sRAP/jFwD/4xcA/+MXAP/jFwD/4xcA/+MXAP/jFwD/4xcA/6sRAP9+Cgr/AAAAAH4KCv8AAAAA"
        "AAAAAAAAAAAAAAAAfgoK/6sRAP+rEQD/qxEA/6sRAP+rEQD/qxEA/6sRAP+rEQD/qxEA/6sRAP9+Cgr/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAH4KCv+rEQD/qxEA/6sRAP+rEQD/qxEA/6sRAP+rEQD/qxEA/34KCv8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/darkness_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGhoa/xoaGv8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAaGhr/ExMV/wkFBf8JBQX/ExMV/xoaGv8AAAAAAAAAABMTFf8aGhr/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAGhoa/wkFBf8JBQX/CQUF/wkFBf8JBQX/CQUF/wkFBf8aGhr/AAAAABoaGv8JBQX/Ghoa/wAAAAAAAAAA"
        "AAAAAAAAAAATExX/CQUF/wkFBf8JBQX/CQUF/xMTFf8TExX/CQUF/wkFBf8JBQX/AAAAAAAAAAAJBQX/CQUF/wAAAAAAAAAA"
        "AAAAABoaGv8JBQX/CQUF/wkFBf8TExX/AAAAAAAAAAAAAAAAGhoa/wkFBf8JBQX/Ghoa/wAAAAAaGhr/CQUF/xoaGv8AAAAA"
        "AAAAABMTFf8JBQX/CQUF/wkFBf8AAAAAAAAAABoaGv8aGhr/AAAAABMTFf8JBQX/ExMV/wAAAAAAAAAACQUF/xMTFf8AAAAA"
        "AAAAAAkFBf8JBQX/CQUF/xoaGv8AAAAACQUF/xMTFf8aGhr/ExMV/wkFBf8JBQX/ExMV/wAAAAAAAAAACQUF/wkFBf8AAAAA"
        "AAAAAAkFBf8JBQX/CQUF/wAAAAAAAAAACQUF/wkFBf8JBQX/CQUF/wkFBf8JBQX/Ghoa/wAAAAAaGhr/CQUF/wkFBf8AAAAA"
        "AAAAAAkFBf8JBQX/CQUF/xoaGv8AAAAAExMV/wkFBf8JBQX/CQUF/wkFBf8aGhr/AAAAAAAAAAAaGhr/CQUF/xMTFf8AAAAA"
        "AAAAABMTFf8JBQX/CQUF/xMTFf8AAAAAAAAAABMTFf8JBQX/ExMV/xoaGv8AAAAAAAAAAAAAAAATExX/CQUF/xoaGv8AAAAA"
        "AAAAABMTFf8JBQX/CQUF/wkFBf8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABoaGv8JBQX/CQUF/xoaGv8AAAAA"
        "AAAAABoaGv8JBQX/CQUF/wkFBf8TExX/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGhoa/xMTFf8JBQX/CQUF/wAAAAAAAAAA"
        "AAAAAAAAAAATExX/CQUF/wkFBf8JBQX/ExMV/xoaGv8AAAAAAAAAABoaGv8TExX/CQUF/wkFBf8JBQX/Ghoa/wAAAAAAAAAA"
        "AAAAAAAAAAAaGhr/ExMV/wkFBf8JBQX/CQUF/wkFBf8JBQX/CQUF/wkFBf8JBQX/CQUF/wkFBf8aGhr/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAGhoa/xMTFf8TExX/ExMV/wkFBf8JBQX/CQUF/wkFBf8JBQX/ExMV/xoaGv8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAaGhr/ExMV/xMTFf8TExX/ExMV/xoaGv8aGhr/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/trial_omen_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACptdv8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAqbXb/AAAAAAAAAAAAAAAAKm12/yptdv8AAAAAAAAAACptdv8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAlt7j/Km12/wAAAAAqbXb/Jbe4/yptdv8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAACW3uP8z////Jbe4/wAAAAAlt7j/M////yW3uP8qbXb/AAAAACptdv8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAJbe4/zP///8lt7j/Km12/yptdv8lt7j/M////zP///8qbXb/AAAAACW3uP8qbXb/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAlt7j/M/////X///8lt7j/Jbe4/yW3uP8z////Jbe4/zP///8lt7j/Km12/zP///8lt7j/Km12/wAAAAAAAAAA"
        "AAAAAAAAAAAqbXb/Jbe4//X///8z////Jbe4/yptdv8lt7j/Km12/yW3uP8lt7j/M/////X///8z////Jbe4/yptdv8AAAAA"
        "AAAAAAAAAAAqbXb/Km12/yptdv8lt7j/Km12/ypEVf8qRFX/GCw5/xgsOf8qbXb/Jbe4//X////1////Jbe4/yptdv8AAAAA"
        "AAAAAAAAAAAAAAAAKm12/wAAAAAqbXb/GCw5/0BieP9AYnj/KkRV/ypEVf8YLDn/Km12/yW3uP8z////Km12/wAAAAAAAAAA"
        "AAAAACptdv8AAAAAJbe4/wAAAAAqbXb/qAAv//8ASP8YLDn/GCw5//8ASP+oAC//Km12/yptdv8lt7j/Km12/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAM////yW3uP8qbXb/KiQ1/yokNf9AYnj/KkRV/yokNf8qJDX/Km12/wAAAAAqbXb/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAJbe4/zP///8lt7j/KkRV/xgsOf8qJDX/KiQ1/xgsOf8qRFX/Jbe4/yptdv8qbXb/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAADP///8z////Jbe4/ypEVf8YLDn/GCw5/ypEVf8lt7j/M////yW3uP8qbXb/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAKm12/zP///8lt7j/Km12/yW3uP8qbXb/Km12/yW3uP8z////M////yptdv8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAACptdv8qbXb/Km12/yW3uP8z////Jbe4/yW3uP8lt7j/Km12/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACptdv8lt7j/Km12/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/wind_charged_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAgYyp/4GMqf+BjKn/gYyp/4GMqf8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACBjKn/vcn//+vv///r7///6+///73J//9teoz/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIGMqf+9yf//6+///+vv///r7///6+///+vv///r7///bXqM/wAAAAAAAAAA"
        "AAAAAAAAAACBjKn/gYyp/4GMqf+BjKn/AAAAAIGMqf/r7///6+///+vv//+cp8//nKfP/+vv///r7///vcn//216jP8AAAAA"
        "AAAAAIGMqf+9yf//6+///+vv//+9yf//bXqM/4GMqf/r7///6+///5ynz//r7///6+///5ynz//r7///vcn//216jP8AAAAA"
        "gYyp/73J///r7///6+///+vv///r7///vcn//216jP/r7///6+///5ynz/+cp8//6+///4GMqf/r7///vcn//216jP8AAAAA"
        "gYyp/+vv///r7///nKfP/5ynz//r7///6+///216jP+9yf//6+///+vv///r7///6+///4GMqf/r7///vcn//216jP8AAAAA"
        "gYyp/+vv///r7///nKfP/+vv//+cp8//6+///73J//9teoz/vcn//+vv///r7///bXqM/5ynz/+9yf//vcn//216jP8AAAAA"
        "AAAAAG16jP/r7///6+///+vv//+BjKn/6+///73J//9teoz/bXqM/216jP9teoz/nKfP/73J//+9yf//bXqM/wAAAAAAAAAA"
        "AAAAAAAAAABteoz/gYyp/4GMqf+cp8//6+///5ynz/9teoz/nKfP/5ynz/+9yf//vcn//4GMqf+BjKn/bXqM/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAbXqM/216jP/r7///vcn//216jP+cp8//nKfP/73J///r7///gYyp/73J///r7///vcn//216jP8AAAAA"
        "AAAAAG16jP9teoz/nKfP/+vv//+cp8//bXqM/5ynz/+cp8//vcn//+vv//+BjKn/vcn//+vv///r7///6+///73J//9teoz/"
        "bXqM/5ynz/+9yf//vcn//216jP9teoz/nKfP/5ynz/+9yf//6+///5ynz/+BjKn/6+///+vv//+cp8//nKfP/+vv//9teoz/"
        "bXqM/216jP9teoz/bXqM/5ynz/+cp8//nKfP/73J///r7///nKfP/216jP9teoz/vcn//+vv//+9yf//gYyp/+vv//9teoz/"
        "AAAAAG16jP+BjKn/nKfP/5ynz/+9yf//6+///+vv//9teoz/bXqM/wAAAAAAAAAAbXqM/216jP+BjKn/6+///73J//9teoz/"
        "bXqM/73J///r7///6+///+vv//+9yf//bXqM/216jP8AAAAAAAAAAG16jP9teoz/nKfP/73J///r7///vcn//216jP8AAAAA"
        "bXqM/216jP9teoz/bXqM/216jP9teoz/AAAAAAAAAAAAAAAAbXqM/5ynz/+9yf//6+///+vv//9teoz/bXqM/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAbXqM/216jP9teoz/bXqM/216jP8AAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/weaving_effect",
        "AAAAAHyFh/98hYf/fIWH/3yFh/98hYf/fIWH/3yFh/98hYf/fIWH/3yFh/98hYf/fIWH/3yFh/98hYf/bnh6/254ev8AAAAA"
        "fIWH/42Ulv///////////8TO0v/EztL/HxgY/8TO0v///////////////////////////////////////////8TO0v9ueHr/"
        "fIWH/////////////////42Ulv9ueHr/bnh6/x8YGP9ueHr/bnh6/254ev9ueHr/bnh6/254ev9ueHr/bnh6/254ev9ueHr/"
        "fIWH////////////Oz9B/x8YGP8fGBj/bnh6/x8YGP8AAAAAbnh6/254ev8AAAAAAAAAAG54ev/EztL/bnh6/wAAAAAAAAAA"
        "fIWH/8TO0v+NlJb/HxgY/zs/Qf87P0H/HxgY/zs/Qf8fGBj/HxgY/x8YGP8AAAAAAAAAAG54ev/EztL/bnh6/wAAAAAAAAAA"
        "fIWH/8TO0v9ueHr/HxgY/y4tLv87P0H/Oz9B/x8YGP/EztL/xM7S//////9ueHr/bnh6/254ev+NlJb/bnh6/wAAAAAAAAAA"
        "fIWH/x8YGP9ueHr/bnh6/x8YGP8uLS7/HxgY/zs/Qf8fGBj/HxgY/x8YGP+NlJb////////////EztL/jZSW/254ev8AAAAA"
        "fIWH/8TO0v8fGBj/HxgY/zs/Qf8fGBj/HxgY/y4tLv8fGBj/AAAAAG54ev8fGBj/HxgY/254ev///////////8TO0v9ueHr/"
        "fIWH//////9ueHr/AAAAAB8YGP/EztL/HxgY/x8YGP87P0H/bnh6/wAAAABueHr/xM7S/254ev9ueHr/bnh6/254ev9ueHr/"
        "fIWH//////+NlJb/bnh6/x8YGP/EztL/HxgY/254ev/EztL/xM7S/254ev9ueHr//////254ev8AAAAAAAAAAAAAAAAAAAAA"
        "fIWH//////+NlJb/xM7S/x8YGP//////HxgY/wAAAABueHr///////////+NlJb/xM7S/254ev8AAAAAAAAAAAAAAAAAAAAA"
        "fIWH//////+NlJb/bnh6/254ev//////bnh6/x8YGP8AAAAAbnh6////////////jZSW/254ev8AAAAAAAAAAAAAAAAAAAAA"
        "fIWH//////9ueHr/AAAAAAAAAABueHr//////x8YGP9ueHr/bnh6/42Ulv///////////254ev8AAAAAAAAAAAAAAAAAAAAA"
        "fIWH//////9ueHr/AAAAAAAAAABueHr//////42Ulv/EztL//////8TO0v+NlJb///////////9ueHr/AAAAAAAAAAAAAAAA"
        "fIWH//////9ueHr/bnh6/254ev+NlJb/xM7S//////+NlJb/bnh6/254ev9ueHr/bnh6///////EztL/bnh6/wAAAAAAAAAA"
        "bnh6//////9ueHr/xM7S/8TO0v+NlJb/jZSW//////9ueHr/AAAAAAAAAAAAAAAAAAAAAG54ev/EztL/bnh6/wAAAAAAAAAA"
        "bnh6/8TO0v9ueHr/bnh6/254ev9ueHr/bnh6/8TO0v9ueHr/AAAAAAAAAAAAAAAAAAAAAAAAAABueHr/bnh6/wAAAAAAAAAA"
        "AAAAAG54ev9ueHr/AAAAAAAAAAAAAAAAAAAAAG54ev9ueHr/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/oozing_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA8YzL/PGMy/zxjMv88YzL/PGMy/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADxjMv+y/5T/sv+U/7L/lP+U1nz/c8Ji/zxjMv8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADxjMv+y/5T/c8Ji/3PCYv9zwmL/c8Ji/y9NJ/8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPGMy/1GaPv+U1nz/PGMy/3PCYv88YzL/c8Ji/y9NJ/8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA8YzL/lNZ8/3PCYv9zwmL/L00n/3PCYv8vTSf/ZLBS/y9NJ/8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADxjMv+y/5T/PGMy/zxjMv9zwmL/UZo+/1GaPv9Rmj7/UZo+/y9NJ/8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAPGMy/7L/lP88YzL/AAAAADxjMv+U1nz/PGMy/y9NJ/8vTSf/L00n/zxjMv8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAA8YzL/sv+U/zxjMv8AAAAAPGMy/7L/lP9Rmj7/UZo+/3PCYv8vTSf/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAADxjMv+U1nz/c8Ji/zxjMv88YzL/sv+U/5TWfP9Rmj7/c8Ji/y9NJ/8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAA8YzL/PGMy/zxjMv88YzL/UZo+/5TWfP+y/5T/lNZ8/1GaPv9zwmL/L00n/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAADxjMv+y/5T/sv+U/7L/lP+U1nz/c8Ji/3PCYv9zwmL/c8Ji/3PCYv88YzL/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAADxjMv+y/5T/c8Ji/3PCYv9zwmL/c8Ji/zxjMv9Rmj7/PGMy/zxjMv8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAADxjMv+U1nz/PGMy/3PCYv88YzL/c8Ji/y9NJ/8vTSf/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAADxjMv9zwmL/L00n/3PCYv8vTSf/ZLBS/y9NJ/8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAADxjMv9Rmj7/UZo+/1GaPv9Rmj7/UZo+/y9NJ/8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAvTSf/L00n/y9NJ/8vTSf/L00n/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/infested_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAXFxc/wAAAABcXFz/XFxc/1xcXP9cXFz/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAXFxc/1xcXP+cnJz/urq6/1xcXP+6urr/XFxc/1xcXP8AAAAA"
        "AAAAAAAAAAAAAAAAXFxc/wAAAAAAAAAAAAAAAAAAAAAAAAD/dHR0/wAAAP90dHT/dHR0/0lJSf90dHT/XFxc/3R0dP8AAAAA"
        "AAAAAFxcXP8AAAAAXFxc/1xcXP9cXFz/AAAAAAAAAABJSUn/SUlJ/0lJSf9JSUn/SUlJ/0lJSf90dHT/XFxc/5ycnP9cXFz/"
        "AAAAAFxcXP9cXFz/nJyc/wAAAP+cnJz/AAAA/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAElJSf9JSUn/SUlJ/3R0dP8AAAD/"
        "XFxc/3R0dP90dHT/SUlJ/0lJSf9cXFz/XFxc/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAASUlJ/wAAAP8AAAD/"
        "AAAA/0lJSf9JSUn/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAFtsdv8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAW2x2/1tsdv8AAAAAW2x2/1tsdv8AAAAAAAAAAFtsdv8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAFtsdv8AAAAASUlJ/0lJSf9JSUn/SUlJ/0lJSf9JSUn/SUlJ/1tsdv8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAABbbHb/AAAAAFtsdv9JSUn/urq6/7q6uv+Mm4z/urq6/3iOm/+cnJz/gYGB/0lJSf8AAAAAW2x2/wAAAAAAAAAA"
        "AAAAAAAAAABbbHb/W2x2/0lJSf9cXFz/eI6b/4GBgf9cXFz/XFxc/1xcXP9cXFz/XFxc/0lJSf9bbHb/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAABJSUn/SUlJ/7q6uv9cXFz/urq6/1xcXP+6urr/urq6/5ycnP+cnJz/jJuM/5ycnP9JSUn/AAAAAAAAAAAAAAAA"
        "AAAAAElJSf+cnJz/XFxc/4ybjP9cXFz/nJyc/1xcXP+6urr/jJuM/1tsdv94jpv/W2x2/4GBgf9JSUn/AAAAAFtsdv8AAAAA"
        "AAAA/wAAAP94jpv/XFxc/4GBgf9cXFz/nJyc/1xcXP94jpv/gYGB/1xcXP9cXFz/XFxc/1xcXP9JSUn/W2x2/wAAAAAAAAAA"
        "AAAA/wAAAP90dHT/SUlJ/3R0dP9JSUn/jJuM/1xcXP+cnJz/XFxc/4GBgf+Mm4z/gYGB/4GBgf+BgYH/SUlJ/wAAAAAAAAAA"
        "AAAAAAAAAABJSUn/SUlJ/0lJSf9JSUn/dHR0/0lJSf90dHT/XFxc/0lJSf8AAAD/gYGB/0lJSf8AAAD/SUlJ/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAASUlJ/0lJSf90dHT/SUlJ/wAAAP8AAAD/gYGB/wAAAP8AAAD/SUlJ/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAABJSUn/SUlJ/0lJSf9JSUn/SUlJ/0lJSf9JSUn/AAAAAAAAAAAAAAAA"
    },
    {"textures/ui/raid_omen_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAYLDn/AAAAAAAAAAAAAAAAAAAAAAAAAAAYLDn/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGCw5/ypEVf8qRFX/GCw5/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAACpEVf8YLDn/AAAAAAAAAAAqRFX/QGJ4/ypEVf8iGzD/AAAAAAAAAAAYLDn/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAACgsLP9RVVX/UVVV/1FVVf9RVVX/UVVV/1FVVf9RVVX/UVVV/ygsLP8qRFX/GCw5/wAAAAAAAAAA"
        "AAAAAAAAAAAYLDn/AAAAAFFVVf+VoKD/lZub/46Tk/+Vm5v/lZub/5WgoP+Vm5v/lZub/1FVVf9AYnj/KkRV/xgsOf8AAAAA"
        "AAAAABgsOf8YLDn/GCw5/1FVVf+VoKD/lZub/46Tk/+Vm5v/lZub/5Wbm/+Vm5v/laCg/1FVVf8qRFX/GCw5/xgsOf8AAAAA"
        "AAAAABgsOf9AYnj/KkRV/1FVVf+Vm5v/lZub/5Wbm/+Vm5v/lZub/5Wbm/+Ok5P/laCg/1FVVf8AAAAAGCw5/yIbMP8AAAAA"
        "AAAAACpEVf8qRFX/KCws/ygsLP8oLCz/c3d3/46Tk/+Vm5v/jpOT/46Tk/9zd3f/KCws/ygsLP8oLCz/AAAAACIbMP8AAAAA"
        "AAAAAAAAAAAqRFX/KCws/ygsLP8oLCz/KCws/2JoaP+Ok5P/jpOT/2JoaP8oLCz/KCws/ygsLP8oLCz/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAACgsLP8oLCz/KCws/ygsLP9iaGj/Ymho/ygsLP8oLCz/KCws/ygsLP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAABoTh7/KB4I/zk5Of8oLCz/qAAv//8ASP8oLCz/KCws//8ASP+oAC//KCws/zk5Of8oHgj/aE4e/wAAAAAAAAAA"
        "AAAAAGJiYv9ERET/lpaW/1FVVf9obm7/KCws/ygsLP9obm7/aG5u/ygsLP8oLCz/aG5u/1FVVf+Wlpb/RERE/2JiYv8AAAAA"
        "YmJi/9nZ2f/BwcH/YmJi/1FVVf9obm7/ipCQ/1FVVf9obm7/aG5u/z9DQ/+KkJD/aG5u/1FVVf9iYmL/wcHB/9nZ2f9iYmL/"
        "YmJi///////BwcH/2dnZ/0RERP9obm7/ipCQ/1FVVf9obm7/aG5u/z9DQ/+KkJD/aG5u/0RERP/Z2dn/wcHB//////9iYmL/"
        "AAAAAGJiYv///////////ygsLP8oLCz/KCws/ygsLP9obm7/aG5u/ygsLP8oLCz/KCws/ygsLP///////////2JiYv8AAAAA"
        "AAAAAAAAAABiYmL/YmJi/wAAAAAoHQr/STYW/yYeDP8mHgz/Jh4M/yYeDP9JNhb/KB0K/wAAAABiYmL/YmJi/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAKB0K/0g2Fv8oHQr/KB0K/0g2Fv8oHQr/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAACgdCv8oHQr/STYW/ygdCv8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    },
    {"textures/ui/breath_of_the_nautilus_effect",
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAACAVzn/gFc5/4BXOf+JKij/iSoo/wAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAIkqKP//e2P///fs///37P//e2P//3tj/4kqKP+AVzn/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAiSoo//97Y//tY0z/7WNM/+HXzf/mo5P/7WNM/+1jTP//9+z/gFc5/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAACAVzn///fs/+ajk//tY0z/iSoo/4BXOf+AVzn/iSoo/+1jTP/h183///fs/4BXOf8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAIBXOf/mo5P///fs/+HXzf+AVzn/4dfN/+HXzf/h183/4dfN/4kqKP/mo5P//3tj/4kqKP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAIBXOf//e2P/7WNM/4kqKP/h183/5qOT/4BXOf+AVzn/4dfN/4BXOf/tY0z//3tj/4kqKP8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAIkqKP//e2P/7WNM/4kqKP/mo5P/7WNM/4kqKP/h183/w7SZ/4kqKP/bWkH/w7SZ/4BXOf8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAIkqKP//e2P///fs/4BXOf/tY0z/4dfN/+1jTP+JKij/gFc5/8RPM//DtJn/rIRm/4BXOf8AAAAAAAAAAAAAAAAAAAAA"
        "AAAAAIBXOf/DtJn/4dfN/+HXzf+JKij/w7SZ/8RPM//DtJn/w7SZ/6yEZv/ETzP/gFc5/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAACAVzn/w7SZ/+ajk//bWkH/iSoo/4kqKP+AVzn/gFc5/4BXOf+AVzn/oTIw/wAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAgFc5/8RPM//ETzP/xE8z/8O0mf+shGb/gFc5/8RPM//ETzP/xE8z/6EyMP/ETzP/AAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAIkqKP/ETzP/w7SZ/6yEZv+AVzn/xE8z/8RPM//tY0z//3tj//97Y///e2P//3tj/+1jTP/ETzP/"
        "AAAAAAAAAAAAAAAAAAAAAAAAAACJKij/gFc5/4BXOf/ETzP/xE8z/wcHHP//e2P//3tj//97Y//tY0z/xE8z/6EyMP8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAKEyMP+hMjD/xE8z/+1jTP/tY0z/7WNM/8RPM/+hMjD/oTIw/6EyMP+hMjD/"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAoTIw/6EyMP/tY0z//3tj//97Y//tY0z/oTIw/wAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAChMjD/7WNM/6EyMP+hMjD/oTIw/6EyMP8AAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAChMjD/oTIw/wAAAAChMjD/AAAAAAAAAAAAAAAA"
    },
}};


// Minimal base64 decoder for the embedded icon pixels. Runs once per effect
// the first time its icon is registered.
bool decodeBase64(std::string_view encoded, std::vector<unsigned char>& out) {
    static constexpr std::array<std::int8_t, 256> kBase64Table = [] {
        std::array<std::int8_t, 256> table{};
        table.fill(-1);
        constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            table[static_cast<unsigned char>(alphabet[i])] = static_cast<std::int8_t>(i);
        }
        return table;
    }();

    out.clear();
    out.reserve(encoded.size() / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (const char ch : encoded) {
        if (ch == '=') break;  // tolerate padding even though our data has none
        const auto value = kBase64Table[static_cast<unsigned char>(ch)];
        if (value < 0) return false;
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

// Decodes the embedded pixels for a vanilla icon path. Returns an empty vector
// when the path is unknown or the payload is malformed.
std::vector<unsigned char> vanillaIconPixels(const char* path) {
    for (const auto& asset : kVanillaIconAssets) {
        if (std::strcmp(asset.path, path) != 0) continue;
        std::vector<unsigned char> pixels;
        if (decodeBase64(asset.base64, pixels) &&
            pixels.size() == static_cast<std::size_t>(kEffectIconSize * kEffectIconSize * 4)) {
            return pixels;
        }
        break;
    }
    return {};
}

void ensureEffectIcon(std::uint32_t id) {
    static std::unordered_map<std::uint32_t, bool> registered;
    if (id == 0 || !registered.emplace(id, true).second) return;

    // Register the vanilla icon for this effect under its texture path. The
    // mod menu copies the pixels, so the temporary buffer may go out of scope.
    const char* path = getEffectIconPath(id);
    const auto pixels = vanillaIconPixels(path);
    if (pixels.empty()) return;
    pl::modmenu::registerImage(path, pixels, kEffectIconSize, kEffectIconSize);
}

// ---------------------------------------------------------------------------
// Time helpers for the countdown colors, progress bars and entrance
// animations. All animations run off a steady clock so they stay smooth
// regardless of the game's tick rate.
// ---------------------------------------------------------------------------

using SteadyClock = std::chrono::steady_clock;

// Elapsed seconds between two clock time points, as a float. The subtraction
// happens in the clock's integer domain, so short intervals stay exact even
// though the clock has been running for a long time.
float secondsSince(SteadyClock::time_point from, SteadyClock::time_point to) {
    return std::chrono::duration<float>(to - from).count();
}

float easeOutCubic(float t) {
    return 1.0f - std::pow(1.0f - t, 3.0f);
}

float easeOutBack(float t) {
    constexpr float c1 = 1.70158f;
    constexpr float c3 = c1 + 1.0f;
    return 1.0f + c3 * std::pow(t - 1.0f, 3.0f) + c1 * std::pow(t - 1.0f, 2.0f);
}

std::uint32_t withAlpha(std::uint32_t color, float alpha) {
    const auto a = static_cast<std::uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (a << 24) | (color & 0x00FFFFFF);
}

namespace effectlayout = bedrocktools::hud::effects;

// Shared formatting helpers (level, duration, urgency color).
using bedrocktools::hud::effects::durationColor;
using bedrocktools::hud::effects::formatDuration;
using bedrocktools::hud::effects::isInfiniteDuration;
using bedrocktools::hud::effects::levelSuffix;

// Amplifier value meaning "the level could not be determined". The row then
// shows only the effect name, which is far better than a wrong level.
constexpr int kUnknownAmplifier = effectlayout::kUnknownAmplifier;

// Resolves (and caches) the MobEffectInstance layout for the running game
// build, then reads the active effects out of the component's vector.
//
// The layout is re-validated on every read and only cached once it has been
// confirmed a few times in a row, so a wrong guess made on a half-initialized
// component cannot stick around for the rest of the session.
struct LayoutCache {
    effectlayout::InstanceLayout layout{};
    int confirmations = 0;

    // Number of consecutive confirmations after which the cached layout is
    // trusted enough to survive a single contradicting read.
    static constexpr int kTrustedConfirmations = 4;

    const effectlayout::InstanceLayout* resolve(const std::uint8_t* data, std::size_t bytes) {
        // Fast path: the cached layout still explains this buffer. This is a
        // cheap re-check, not a full search, because it runs every tick.
        if (effectlayout::validateLayout(data, bytes, layout)) {
            if (confirmations < kTrustedConfirmations * 2) ++confirmations;
            return &layout;
        }

        // The buffer no longer matches. Search again; a well-confirmed layout
        // survives one bad read (a torn vector during a resize, for example).
        const auto fresh = effectlayout::resolveLayout(data, bytes);
        if (!fresh.valid()) {
            if (confirmations >= kTrustedConfirmations) {
                --confirmations;
                return nullptr;   // skip this tick, keep the learned layout
            }
            layout = {};
            confirmations = 0;
            return nullptr;
        }

        layout = fresh;
        confirmations = 1;
        return &layout;
    }
};

std::vector<EffectDisplayModule::ActiveEffect> readComponent(const MobEffectsComponent& component) {
    if (!component.begin || component.end < component.begin || component.capacity < component.end) return {};

    const std::size_t bytes = component.end - component.begin;
    if (!bytes || bytes > 64 * 1024) return {};

    static LayoutCache cache;
    const auto* data = reinterpret_cast<const std::uint8_t*>(component.begin);
    const auto* layout = cache.resolve(data, bytes);
    if (!layout) return {};

    const auto records = effectlayout::readRecords(data, bytes, *layout);

    std::vector<EffectDisplayModule::ActiveEffect> result;
    result.reserve(records.size());
    for (const auto& record : records) {
        result.push_back({
            record.id,
            record.durationTicks,
            layout->hasAmplifier ? record.amplifier : kUnknownAmplifier
        });
    }

    // Effects are unique per entity; keep them in a stable id order so the HUD
    // rows do not jump around between ticks.
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id == right.id;
    }), result.end());
    return result;
}

void effectTickCallback(bedrocktools::sdk::Player* player) {
    if (g_effectDisplay && g_effectDisplay->enabled) g_effectDisplay->updateEffects(player);
}

// ---------------------------------------------------------------------------
// Vanilla potion-bar hook
// ---------------------------------------------------------------------------
// Bedrock draws the built-in status-effect (potion) bar from
// `HudScreen::_renderStatusEffects(MinecraftUIRenderContext&, ScreenView&,
// float, float)`. It is the custom UI renderer wired to the vanilla
// `mob_effects_renderer` element of hud_screen.json (visible while the
// `#status_effects_visible` binding is set). Hooking it lets this module skip
// the draw call entirely, so the vanilla bar cannot overlap this module's own
// panel.
//
// The address is resolved in installVanillaBarHook(): first through
// `SignatureId::RenderPotionEffects` (see src/core/memory/Signatures.cpp), and
// when that pattern is not available, through the signature-free locator
// below. Both paths fail quietly and leave the vanilla bar visible, so the
// module itself remains fully functional either way.
//
// The prototype below matches Bedrock's `_renderStatusEffects` signature on
// the builds the rest of this codebase targets. If a different build renames
// or re-shapes this function, only the prototype (and the pattern) need to be
// updated here — the early-return logic stays the same.
using RenderPotionEffectsFn = void (*)(void* self, void* renderContext, void* screenView, float posX, float posY);

RenderPotionEffectsFn g_origRenderPotionEffects = nullptr;

// ---------------------------------------------------------------------------
// Signature-free fallback locator
// ---------------------------------------------------------------------------
// The pure ARM64 scanning and clustering logic lives in effectlocator.hpp so
// it can be unit tested on the host. Only the platform-specific piece stays
// here: enumerating the loaded game library's code/data ranges.

bool collectModuleRanges(effectlocator::ModuleRanges& ranges) {
    struct Context { effectlocator::ModuleRanges* out; };
    Context context{&ranges};
    dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* opaque) -> int {
        auto* out = static_cast<Context*>(opaque)->out;
        if (!info->dlpi_name || !std::strstr(info->dlpi_name, "libminecraftpe.so")) return 0;
        const std::uintptr_t base = static_cast<std::uintptr_t>(info->dlpi_addr);
        for (int i = 0; i < info->dlpi_phnum; ++i) {
            const auto& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD || ph.p_memsz == 0) continue;
            const std::uintptr_t start = base + ph.p_vaddr;
            const std::uintptr_t end = start + ph.p_memsz;
            if ((ph.p_flags & PF_X) != 0) {
                if (out->textStart == 0 || start < out->textStart) out->textStart = start;
                if (end > out->textEnd) out->textEnd = end;
            } else if ((ph.p_flags & PF_R) != 0) {
                if (out->dataStart == 0 || start < out->dataStart) out->dataStart = start;
                if (end > out->dataEnd) out->dataEnd = end;
            }
        }
        return 1;   // stop iterating: first match is the game library
    }, &context);
    return ranges.textStart != 0 && ranges.textEnd > ranges.textStart &&
           ranges.dataStart != 0 && ranges.dataEnd > ranges.dataStart;
}

} // namespace

void EffectDisplayModule::renderPotionEffectsDetour(void* self, void* renderContext, void* screenView, float posX, float posY) {
    // Suppress the vanilla potion bar while the module is enabled and the
    // "hide vanilla HUD" option is active. Return immediately so the game
    // never reaches its own drawing code for the bar.
    if (g_effectDisplay && g_effectDisplay->enabled && g_effectDisplay->m_hideVanillaHud) {
        // When the hook came from the signature-free locator, do not trust it
        // blindly: a function that is never called at frame rate (a one-shot
        // constructor, say) must keep running untouched. Only after the detour
        // has been entered a few dozen times (about a second of rendering) do
        // we start suppressing; if it never reaches that count the vanilla bar
        // simply stays visible.
        const bool warmedUp = !g_effectDisplay->m_locatorMode || g_effectDisplay->detourReady();
        g_effectDisplay->m_detourEntries.fetch_add(1, std::memory_order_relaxed);
        if (warmedUp) return;
    }

    // Otherwise behave exactly like the original function.
    if (g_origRenderPotionEffects) {
        g_origRenderPotionEffects(self, renderContext, screenView, posX, posY);
    }
}

void EffectDisplayModule::installVanillaBarHook() {
    if (m_vanillaBarHooked) return;

    // Preferred: a real byte pattern for HudScreen::_renderStatusEffects (see
    // src/core/memory/Signatures.cpp). A pattern is exact and is therefore
    // trusted immediately (no warm-up).
    std::uintptr_t address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::RenderPotionEffects);

    // Fallback: signature-free locator. It scans the loaded game library for
    // the code that references the vanilla effect icon textures. See
    // effectlocator::locateRenderStatusEffects() for the confidence rules and
    // limitations; the segment enumeration lives in collectModuleRanges().
    if (!address) {
        effectlocator::ModuleRanges ranges;
        if (collectModuleRanges(ranges)) {
            address = effectlocator::locateRenderStatusEffects(ranges);
        }
        m_locatorMode = address != 0;
    }

    if (!address) return;   // neither path resolved: keep the vanilla bar

    m_vanillaBarHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(address),
        reinterpret_cast<void*>(&EffectDisplayModule::renderPotionEffectsDetour),
        reinterpret_cast<void**>(&g_origRenderPotionEffects));
    m_vanillaBarHooked = m_vanillaBarHook != nullptr;
    if (!m_vanillaBarHooked) m_locatorMode = false;
}

EffectDisplayModule::EffectDisplayModule()
    : Module("Effect Display", "Shows every active status effect with its icon, level, and remaining duration.") {
    g_effectDisplay = this;
}

EffectDisplayModule::~EffectDisplayModule() {
    if (g_effectDisplay == this) g_effectDisplay = nullptr;
    if (m_vanillaBarHook) bedrocktools::hooks::remove(m_vanillaBarHook);
    m_vanillaBarHook = nullptr;
    m_vanillaBarHooked = false;
}

void EffectDisplayModule::registerResources() {
    if (m_resourcesRegistered) return;

    const auto fontPath = bedrocktools::core::Runtime::get().resourceDirectory() / "minecraft.ttf";
    std::ifstream fontFile(fontPath, std::ios::binary);
    if (fontFile) {
        std::vector<unsigned char> font((std::istreambuf_iterator<char>(fontFile)), std::istreambuf_iterator<char>());
        if (!font.empty()) pl::modmenu::registerFont("minecraft", font);
    }

    for (std::uint32_t id = 1; id < kEffects.size(); ++id) {
        ensureEffectIcon(id);
    }
    m_resourcesRegistered = true;
}

void EffectDisplayModule::onInit() {
    registerResources();
    installVanillaBarHook();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        effectTickCallback(event.player);
    });
}

void EffectDisplayModule::onDisable() {
    std::lock_guard lock(m_mutex);
    m_effects.clear();
    m_timing.clear();
    m_lastChangeAt = SteadyClock::time_point{};
    ::submitDrawCommands(moduleId, {});
}

void EffectDisplayModule::updateEffects(bedrocktools::sdk::Player* player) {
    std::vector<ActiveEffect> next;
    if (player) {
        auto* context = reinterpret_cast<EntityContext*>(player->entityContext());
        if (context) {
            if (auto* component = context->tryGetComponent<MobEffectsComponent>()) next = readComponent(*component);
        }
    }

    const auto now = SteadyClock::now();
    std::lock_guard lock(m_mutex);

    // Detect any change in the active effect set so the panel can re-animate.
    // Both entries are sorted by id, so a straight pairwise comparison is
    // enough; the level is part of the identity, so upgrading Speed I to
    // Speed II counts as a change.
    bool changed = next.size() != m_effects.size();
    if (!changed) {
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (next[i].id != m_effects[i].id || next[i].amplifier != m_effects[i].amplifier) {
                changed = true;
                break;
            }
        }
    }

    // Track appearance time and longest observed duration per effect. The max
    // duration is the reference for the remaining-time bar and is relearned
    // whenever the effect goes missing for a moment, its level changes, or its
    // duration jumps back up (a fresh potion of the same effect).
    for (const auto& effect : next) {
        auto it = m_timing.find(effect.id);
        if (it == m_timing.end()) {
            m_timing.emplace(effect.id, EffectTiming{now, now, std::max(effect.durationTicks, 0), effect.amplifier});
            continue;
        }

        it->second.lastSeenAt = now;
        if (it->second.amplifier != effect.amplifier) {
            // A different potency is effectively a new effect instance.
            it->second.amplifier = effect.amplifier;
            it->second.appearAt = now;
            it->second.maxDurationTicks = std::max(effect.durationTicks, 0);
        } else if (effect.durationTicks > it->second.maxDurationTicks) {
            it->second.maxDurationTicks = std::max(effect.durationTicks, 0);
        }
    }
    for (auto it = m_timing.begin(); it != m_timing.end();) {
        if (now - it->second.lastSeenAt > std::chrono::milliseconds(1500)) it = m_timing.erase(it);
        else ++it;
    }

    if (changed) m_lastChangeAt = now;
    m_effects.swap(next);
}

void EffectDisplayModule::onFrame() {
    if (!enabled) return;
    registerResources();

    const auto now = SteadyClock::now();
    std::vector<ActiveEffect> effects;
    std::unordered_map<std::uint32_t, EffectTiming> timing;
    SteadyClock::time_point changeAt{};
    {
        std::lock_guard lock(m_mutex);
        effects = m_effects;
        timing = m_timing;
        changeAt = m_lastChangeAt;
    }

    // Advance the low-time pulse phase by the real frame delta.
    if (m_lastFrameTime == SteadyClock::time_point{}) m_lastFrameTime = now;
    m_pulsePhase += secondsSince(m_lastFrameTime, now) * 12.0f;
    if (m_pulsePhase > 25.0f) m_pulsePhase -= 25.0f;
    m_lastFrameTime = now;

    // HUD-editor preview: a believable spread of effects.
    if (effects.empty() && m_preview) {
        effects = {
            {1, 68 * 20, 1},                                  // Speed II
            {12, 191 * 20, 0},                                // Fire Resistance I
            {13, 50 * 20, 2},                                 // Water Breathing III
            {14, std::numeric_limits<int>::max(), 0},         // Invisibility, endless
        };
        for (const auto& effect : effects) {
            const int reference = isInfiniteDuration(effect.durationTicks)
                ? effect.durationTicks
                : effect.durationTicks * 2;
            timing.emplace(
                effect.id,
                EffectTiming{now - std::chrono::seconds(1), now, reference, effect.amplifier}
            );
        }
    }

    const float scale = std::clamp(m_scale, 0.25f, 5.0f);
    const float panelWidth = std::max(110.0f, m_width) * scale;
    const float rowHeight = 48.0f * scale;
    const float padding = 8.0f * scale;
    const float iconSize = static_cast<float>(kEffectIconSize) * scale;
    const float nameSize = 18.0f * scale;
    const float durationSize = 15.0f * scale;
    const float barHeight = 3.0f * scale;
    const float cornerRadius = 9.0f * scale;
    const int visible = effects.empty()
        ? 1
        : std::min<int>(static_cast<int>(effects.size()), std::max(1, m_maxVisible));
    const float panelHeight = padding * 2.0f + rowHeight * visible;

    // Whole-panel entrance: fade in with a gentle slide whenever the set of
    // active effects changes.
    float fade = 1.0f;
    if (m_animate && changeAt != SteadyClock::time_point{}) {
        const float t = std::clamp(secondsSince(changeAt, now) / 0.22f, 0.0f, 1.0f);
        fade = easeOutCubic(t);
    }
    const float panelY = hudPosY + (1.0f - fade) * 8.0f * scale;

    std::vector<PLModMenu_DrawCommand> commands;
    commands.reserve(8 + static_cast<std::size_t>(visible) * 8);

    if (m_showBackground) {
        const auto bgAlpha = std::clamp(m_backgroundOpacity * fade, 0.0f, 1.0f);

        // Soft border around the panel.
        PLModMenu_DrawCommand border{};
        border.type = PL_DRAW_RECT_FILLED;
        border.x = hudPosX - 1.0f * scale;
        border.y = panelY - 1.0f * scale;
        border.w = panelWidth + 2.0f * scale;
        border.h = panelHeight + 2.0f * scale;
        border.x3 = cornerRadius + 1.0f * scale;
        border.color = withAlpha(0x5A6C8C, bgAlpha * 0.45f);
        commands.push_back(border);

        PLModMenu_DrawCommand background{};
        background.type = PL_DRAW_RECT_FILLED;
        background.x = hudPosX;
        background.y = panelY;
        background.w = panelWidth;
        background.h = panelHeight;
        background.x3 = cornerRadius;
        background.color = (static_cast<std::uint32_t>(bgAlpha * 255.0f) << 24) | 0x0E1420;
        commands.push_back(background);

        // Hairline separators between rows.
        if (visible > 1) {
            for (int index = 1; index < visible; ++index) {
                PLModMenu_DrawCommand separator{};
                separator.type = PL_DRAW_LINE;
                separator.x = hudPosX + padding;
                separator.y = panelY + padding + rowHeight * index;
                separator.w = panelWidth - padding * 2.0f;
                separator.h = 0.0f;
                separator.size = std::max(0.5f, 1.0f * scale);
                separator.color = withAlpha(0xFFFFFF, 0.08f * fade);
                commands.push_back(separator);
            }
        }
    } else {
        // Keep a nearly-transparent hitbox so the HUD editor can still grab
        // the module when the background is hidden and no effects are active.
        PLModMenu_DrawCommand hitbox{};
        hitbox.type = PL_DRAW_RECT_FILLED;
        hitbox.x = hudPosX;
        hitbox.y = panelY;
        hitbox.w = panelWidth;
        hitbox.h = panelHeight;
        hitbox.color = 0x02000000;
        commands.push_back(hitbox);
    }

    if (effects.empty()) {
        PLModMenu_DrawCommand emptyCommand{};
        emptyCommand.type = PL_DRAW_TEXT;
        emptyCommand.x = hudPosX + padding;
        emptyCommand.y = panelY + padding;
        emptyCommand.w = panelWidth - padding * 2.0f;
        emptyCommand.h = nameSize + 3.0f * scale;
        emptyCommand.size = nameSize;
        emptyCommand.color = withAlpha(0xFFD8D8D8, fade);
        emptyCommand.fontId = "minecraft";
        emptyCommand.text = "No Effects";
        commands.push_back(emptyCommand);
        ::submitDrawCommands(moduleId, commands);
        return;
    }

    for (int index = 0; index < visible; ++index) {
        const auto& effect = effects[static_cast<std::size_t>(index)];
        ensureEffectIcon(effect.id);
        const float rowY = panelY + padding + rowHeight * index;
        float textX = hudPosX + padding;

        // Newly-applied effects pop in with a little overshoot.
        float iconPop = 1.0f;
        if (m_animate) {
            const auto it = timing.find(effect.id);
            if (it != timing.end() && it->second.appearAt != SteadyClock::time_point{}) {
                const float t = std::clamp(secondsSince(it->second.appearAt, now) / 0.20f, 0.0f, 1.0f);
                iconPop = 0.55f + 0.45f * easeOutBack(t);
            }
        }

        if (m_showIcons) {
            const float drawSize = iconSize * iconPop;
            PLModMenu_DrawCommand icon{};
            icon.type = PL_DRAW_IMAGE;
            icon.x = textX + (iconSize - drawSize) * 0.5f;
            icon.y = rowY + (rowHeight - iconSize) * 0.5f + (iconSize - drawSize) * 0.5f;
            icon.w = drawSize;
            icon.h = drawSize;
            icon.color = withAlpha(0xFFFFFF, fade);
            icon.imageId = getEffectIconPath(effect.id);
            commands.push_back(icon);
            textX += iconSize + 7.0f * scale;
        }

        std::string name = definitionFor(effect.id).name;
        if (name.empty()) name = "Unknown Effect";
        if (m_showLevel) {
            const auto level = levelSuffix(effect.amplifier, m_romanLevels, m_hideLevelOne);
            if (!level.empty()) name += " " + level;
        }
        const std::string duration = formatDuration(effect.durationTicks);
        const float contentWidth = panelWidth - (textX - hudPosX) - padding;

        float durationAlpha = 1.0f;
        const std::uint32_t durationColorValue = durationColor(effect.durationTicks, m_pulsePhase, durationAlpha);

        PLModMenu_DrawCommand nameCommand{};
        nameCommand.type = PL_DRAW_TEXT;
        nameCommand.x = textX;
        nameCommand.y = rowY;
        nameCommand.w = contentWidth;
        nameCommand.h = nameSize + 3.0f * scale;
        nameCommand.size = nameSize;
        nameCommand.color = withAlpha(0xFFF4F4F4, fade);
        nameCommand.fontId = "minecraft";
        nameCommand.text = name;
        commands.push_back(nameCommand);

        PLModMenu_DrawCommand durationCommand{};
        durationCommand.type = PL_DRAW_TEXT;
        durationCommand.x = textX;
        durationCommand.y = rowY + 19.0f * scale;
        durationCommand.w = contentWidth;
        durationCommand.h = durationSize + 2.0f * scale;
        durationCommand.size = durationSize;
        durationCommand.color = withAlpha(durationColorValue, fade * durationAlpha);
        durationCommand.fontId = "minecraft";
        durationCommand.text = duration;
        commands.push_back(durationCommand);

        // Remaining-time progress bar, tinted with the effect's own color.
        if (m_showProgressBar) {
            // Endless effects always show a full bar; timed ones are measured
            // against the longest duration seen for this exact effect+level.
            float fraction = 1.0f;
            const auto it = timing.find(effect.id);
            if (!isInfiniteDuration(effect.durationTicks) &&
                it != timing.end() && it->second.maxDurationTicks > 0) {
                fraction = std::clamp(
                    static_cast<float>(effect.durationTicks) / static_cast<float>(it->second.maxDurationTicks),
                    0.0f,
                    1.0f
                );
            }
            const float barY = rowY + rowHeight - barHeight - 6.0f * scale;
            const float barRadius = barHeight * 0.5f;

            PLModMenu_DrawCommand track{};
            track.type = PL_DRAW_RECT_FILLED;
            track.x = textX;
            track.y = barY;
            track.w = contentWidth;
            track.h = barHeight;
            track.x3 = barRadius;
            track.color = withAlpha(0xFFFFFF, 0.10f * fade);
            commands.push_back(track);

            if (fraction > 0.01f) {
                PLModMenu_DrawCommand fill{};
                fill.type = PL_DRAW_RECT_FILLED;
                fill.x = textX;
                fill.y = barY;
                fill.w = std::max(contentWidth * fraction, barHeight * 2.0f);
                fill.h = barHeight;
                fill.x3 = barRadius;
                fill.color = withAlpha(definitionFor(effect.id).color, 0.85f * fade);
                commands.push_back(fill);
            }
        }
    }

    ::submitDrawCommands(moduleId, commands);
}

void EffectDisplayModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (j.contains("m_scale")) m_scale = j["m_scale"].get<float>();
    if (j.contains("m_width")) m_width = j["m_width"].get<float>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = j["m_backgroundOpacity"].get<float>();
    if (j.contains("m_showBackground")) m_showBackground = j["m_showBackground"].get<bool>();
    if (j.contains("m_showIcons")) m_showIcons = j["m_showIcons"].get<bool>();
    if (j.contains("m_showLevel")) m_showLevel = j["m_showLevel"].get<bool>();
    if (j.contains("m_romanLevels")) m_romanLevels = j["m_romanLevels"].get<bool>();
    if (j.contains("m_hideLevelOne")) m_hideLevelOne = j["m_hideLevelOne"].get<bool>();
    if (j.contains("m_showProgressBar")) m_showProgressBar = j["m_showProgressBar"].get<bool>();
    if (j.contains("m_hideVanillaHud")) m_hideVanillaHud = j["m_hideVanillaHud"].get<bool>();
    if (j.contains("m_animate")) m_animate = j["m_animate"].get<bool>();
    if (j.contains("m_preview")) m_preview = j["m_preview"].get<bool>();
    if (j.contains("m_maxVisible")) m_maxVisible = j["m_maxVisible"].get<int>();
}

void EffectDisplayModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_scale"] = m_scale;
    j["m_width"] = m_width;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
    j["m_showBackground"] = m_showBackground;
    j["m_showIcons"] = m_showIcons;
    j["m_showLevel"] = m_showLevel;
    j["m_romanLevels"] = m_romanLevels;
    j["m_hideLevelOne"] = m_hideLevelOne;
    j["m_showProgressBar"] = m_showProgressBar;
    j["m_hideVanillaHud"] = m_hideVanillaHud;
    j["m_animate"] = m_animate;
    j["m_preview"] = m_preview;
    j["m_maxVisible"] = m_maxVisible;
}
