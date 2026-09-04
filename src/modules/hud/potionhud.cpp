#include "potionhud.hpp"
#include "potionhud_assets.hpp"

#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <bedrocktools/sdk/world/MobEffects.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>
#include <pl/memory/Vtable.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr const char* MinecraftLibrary = "libminecraftpe.so";
constexpr std::size_t MaxEffects = 64;
constexpr float VanillaEffectSize = 16.0f;
constexpr int WarningSeconds = 5;
constexpr const char* InstantHealthImageId = "bedrocktools.potionhud.instant_health";
constexpr const char* InstantDamageImageId = "bedrocktools.potionhud.instant_damage";
constexpr const char* SaturationImageId = "bedrocktools.potionhud.saturation";
constexpr const char* GenericPotionImageId = "bedrocktools.potionhud.generic";
struct RectangleArea {
    float x0;
    float x1;
    float y0;
    float y1;
};

struct UiVec2 {
    float x;
    float y;
};

struct Color {
    float r;
    float g;
    float b;
    float a;
};

struct ClientTexture {
    std::byte storage[24]{};
};

struct BedrockTextureData {
    ClientTexture clientTexture;
};

enum class ResourceFileSystem : int {
    UserPackage = 0
};

class ResourceLocation {
public:
    ResourceFileSystem fileSystem;
    std::string path;
    std::uint64_t pathHash;
    std::uint64_t fullHash;

    explicit ResourceLocation(std::string_view value)
        : fileSystem(ResourceFileSystem::UserPackage),
          path(value),
          pathHash(computeHash(path)),
          fullHash(pathHash ^ static_cast<std::uint64_t>(fileSystem)) {}

private:
    static std::uint64_t computeHash(std::string_view value) {
        constexpr std::uint64_t offset = 1469598103934665603ULL;
        constexpr std::uint64_t prime = 1099511628211ULL;
        std::uint64_t hash = offset;
        for (unsigned char ch : value) hash = static_cast<std::uint64_t>(ch) ^ (prime * hash);
        return hash;
    }
};

class TexturePtr {
public:
    std::shared_ptr<const BedrockTextureData> clientTexture;
    std::shared_ptr<ResourceLocation> resourceLocation;

    const ClientTexture& getClientTexture() const {
        static const ClientTexture empty{};
        return clientTexture ? clientTexture->clientTexture : empty;
    }
};

class HashedString {
public:
    std::uint64_t hash;
    std::string value;
    mutable const HashedString* lastMatch;

    explicit HashedString(const char* text)
        : hash(computeHash(text ? std::string_view(text) : std::string_view())),
          value(text ? text : ""),
          lastMatch(nullptr) {}

private:
    static std::uint64_t computeHash(std::string_view text) {
        if (text.empty()) return 0;
        constexpr std::uint64_t offset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t prime = 0x100000001B3ULL;
        std::uint64_t result = offset;
        for (char character : text) {
            result = static_cast<std::uint64_t>(static_cast<unsigned char>(character)) ^ (prime * result);
        }
        return result;
    }
};

struct RawEffect {
    std::uint32_t id;
    int duration;
    int amplifier;
    bool noCounter;
};

using HudMobEffectsRendererFn = void* (*)(void*, void*, void*, void*, int, void*);

HudMobEffectsRendererFn hudMobEffectsRendererOriginal = nullptr;
PotionHudModule* moduleInstance = nullptr;
bedrocktools::hooks::Handle hudMobEffectsRendererHook = nullptr;

void** getVtable(void* object) {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

void* getLocalPlayer(void* client) {
    void** vtable = getVtable(client);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client);
}

MobEffectsComponent* getMobEffects(void* player) {
    if (!player) return nullptr;
    auto* context = reinterpret_cast<EntityContext*>(
        reinterpret_cast<std::uintptr_t>(player) + bedrocktools::sdk::offsets::Actor::mEntityContext);
    return context->tryGetComponent<MobEffectsComponent>();
}

bool copyEffects(MobEffectsComponent* component, std::vector<RawEffect>& out) {
    if (!component) return false;
    const auto begin = reinterpret_cast<std::uintptr_t>(component->begin);
    const auto end = reinterpret_cast<std::uintptr_t>(component->end);
    const auto capacity = reinterpret_cast<std::uintptr_t>(component->capacity);
    if (begin == 0 && end == 0 && capacity == 0) return true;
    if (!begin || !end || !capacity || end < begin || capacity < end) return false;
    const auto span = end - begin;
    const auto capacitySpan = capacity - begin;
    if (span % sizeof(MobEffectInstance) != 0 || capacitySpan % sizeof(MobEffectInstance) != 0) return false;
    const std::size_t count = span / sizeof(MobEffectInstance);
    const std::size_t capacityCount = capacitySpan / sizeof(MobEffectInstance);
    if (count > MaxEffects || capacityCount > MaxEffects * 4 || count > capacityCount) return false;
    out.clear();
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* effect = reinterpret_cast<const MobEffectInstance*>(begin + i * sizeof(MobEffectInstance));
        const auto id = static_cast<std::uint32_t>(effect->id);
        if (id == 0 || id > static_cast<std::uint32_t>(MobEffectType::BreathOfTheNautilus)) continue;
        if (effect->duration < -1 || (effect->duration == 0 && !effect->noCounter)) continue;
        out.push_back({id, effect->duration, std::clamp(effect->amplifier, 0, 255), effect->noCounter});
    }
    return true;
}

RectangleArea getFullClippingRectangle(void* context) {
    RectangleArea result{};
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle]) return result;
    using Fn = RectangleArea (*)(void*);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle])(context);
}

bool validRectangle(const RectangleArea& area) {
    return std::isfinite(area.x0) && std::isfinite(area.x1) && std::isfinite(area.y0) && std::isfinite(area.y1) &&
           area.x1 > area.x0 && area.y1 > area.y0;
}

TexturePtr getTexture(void* context, const ResourceLocation& location) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture]) return {};
    using Fn = TexturePtr (*)(void*, const ResourceLocation&, bool);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture])(context, location, false);
}

void drawImage(void* context, const ClientTexture& texture, const UiVec2& position, const UiVec2& size) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage]) return;
    using Fn = void (*)(void*, const ClientTexture&, const UiVec2&, const UiVec2&, const UiVec2&, const UiVec2&, bool);
    static constexpr UiVec2 uv{0.0f, 0.0f};
    static constexpr UiVec2 uvSize{1.0f, 1.0f};
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage])(
        context, texture, position, size, uv, uvSize, false);
}

void flushImages(void* context) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    using Fn = void (*)(void*, const Color&, float, const HashedString&);
    static const HashedString material("ui_flush");
    static constexpr Color color{1.0f, 1.0f, 1.0f, 1.0f};
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages])(
        context, color, 1.0f, material);
}

std::uint32_t parseColor(const std::string& value, std::uint32_t fallback) {
    if (value.empty()) return fallback;
    const std::string hex = value[0] == '#' ? value.substr(1) : value;
    try {
        if (hex.size() == 6) return 0xFF000000u | static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        if (hex.size() == 8) return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
    }
    return fallback;
}

std::string_view effectName(std::uint32_t id) {
    switch (static_cast<MobEffectType>(id)) {
        case MobEffectType::Speed: return "Speed";
        case MobEffectType::Slowness: return "Slowness";
        case MobEffectType::Haste: return "Haste";
        case MobEffectType::MiningFatigue: return "Mining Fatigue";
        case MobEffectType::Strength: return "Strength";
        case MobEffectType::InstantHealth: return "Instant Health";
        case MobEffectType::InstantDamage: return "Instant Damage";
        case MobEffectType::JumpBoost: return "Jump Boost";
        case MobEffectType::Nausea: return "Nausea";
        case MobEffectType::Regeneration: return "Regeneration";
        case MobEffectType::Resistance: return "Resistance";
        case MobEffectType::FireResistance: return "Fire Resistance";
        case MobEffectType::WaterBreathing: return "Water Breathing";
        case MobEffectType::Invisibility: return "Invisibility";
        case MobEffectType::Blindness: return "Blindness";
        case MobEffectType::NightVision: return "Night Vision";
        case MobEffectType::Hunger: return "Hunger";
        case MobEffectType::Weakness: return "Weakness";
        case MobEffectType::Poison: return "Poison";
        case MobEffectType::Wither: return "Wither";
        case MobEffectType::HealthBoost: return "Health Boost";
        case MobEffectType::Absorption: return "Absorption";
        case MobEffectType::Saturation: return "Saturation";
        case MobEffectType::Levitation: return "Levitation";
        case MobEffectType::FatalPoison: return "Fatal Poison";
        case MobEffectType::ConduitPower: return "Conduit Power";
        case MobEffectType::SlowFalling: return "Slow Falling";
        case MobEffectType::BadOmen: return "Bad Omen";
        case MobEffectType::VillageHero: return "Village Hero";
        case MobEffectType::Darkness: return "Darkness";
        case MobEffectType::TrialOmen: return "Trial Omen";
        case MobEffectType::WindCharged: return "Wind Charged";
        case MobEffectType::Weaving: return "Weaving";
        case MobEffectType::Oozing: return "Oozing";
        case MobEffectType::Infested: return "Infested";
        case MobEffectType::RaidOmen: return "Raid Omen";
        case MobEffectType::BreathOfTheNautilus: return "Breath of the Nautilus";
        default: return "Unknown Effect";
    }
}

std::string_view effectTexturePath(std::uint32_t id) {
    switch (static_cast<MobEffectType>(id)) {
        case MobEffectType::Speed: return "textures/ui/speed_effect";
        case MobEffectType::Slowness: return "textures/ui/slowness_effect";
        case MobEffectType::Haste: return "textures/ui/haste_effect";
        case MobEffectType::MiningFatigue: return "textures/ui/mining_fatigue_effect";
        case MobEffectType::Strength: return "textures/ui/strength_effect";
        case MobEffectType::InstantHealth: return "textures/ui/instant_health_effect";
        case MobEffectType::InstantDamage: return "textures/ui/instant_damage_effect";
        case MobEffectType::JumpBoost: return "textures/ui/jump_boost_effect";
        case MobEffectType::Nausea: return "textures/ui/nausea_effect";
        case MobEffectType::Regeneration: return "textures/ui/regeneration_effect";
        case MobEffectType::Resistance: return "textures/ui/resistance_effect";
        case MobEffectType::FireResistance: return "textures/ui/fire_resistance_effect";
        case MobEffectType::WaterBreathing: return "textures/ui/water_breathing_effect";
        case MobEffectType::Invisibility: return "textures/ui/invisibility_effect";
        case MobEffectType::Blindness: return "textures/ui/blindness_effect";
        case MobEffectType::NightVision: return "textures/ui/night_vision_effect";
        case MobEffectType::Hunger: return "textures/ui/hunger_effect";
        case MobEffectType::Weakness: return "textures/ui/weakness_effect";
        case MobEffectType::Poison: return "textures/ui/poison_effect";
        case MobEffectType::Wither: return "textures/ui/wither_effect";
        case MobEffectType::HealthBoost: return "textures/ui/health_boost_effect";
        case MobEffectType::Absorption: return "textures/ui/absorption_effect";
        case MobEffectType::Saturation: return "textures/ui/saturation_effect";
        case MobEffectType::Levitation: return "textures/ui/levitation_effect";
        case MobEffectType::FatalPoison: return "textures/ui/fatal_poison_effect";
        case MobEffectType::ConduitPower: return "textures/ui/conduit_power_effect";
        case MobEffectType::SlowFalling: return "textures/ui/slow_falling_effect";
        case MobEffectType::BadOmen: return "textures/ui/bad_omen_effect";
        case MobEffectType::VillageHero: return "textures/ui/village_hero_effect";
        case MobEffectType::Darkness: return "textures/ui/darkness_effect";
        case MobEffectType::TrialOmen: return "textures/ui/trial_omen_effect";
        case MobEffectType::WindCharged: return "textures/ui/wind_charged_effect";
        case MobEffectType::Weaving: return "textures/ui/weaving_effect";
        case MobEffectType::Oozing: return "textures/ui/oozing_effect";
        case MobEffectType::Infested: return "textures/ui/infested_effect";
        case MobEffectType::RaidOmen: return "textures/ui/raid_omen_effect";
        case MobEffectType::BreathOfTheNautilus: return "textures/ui/breath_of_the_nautilus_effect";
        default: return {};
    }
}

bool usesNativeTexture(std::uint32_t id) {
    return id != static_cast<std::uint32_t>(MobEffectType::InstantHealth) &&
           id != static_cast<std::uint32_t>(MobEffectType::InstantDamage) &&
           id != static_cast<std::uint32_t>(MobEffectType::Saturation);
}

const char* fallbackImageId(std::uint32_t id) {
    if (id == static_cast<std::uint32_t>(MobEffectType::InstantHealth)) return InstantHealthImageId;
    if (id == static_cast<std::uint32_t>(MobEffectType::InstantDamage)) return InstantDamageImageId;
    if (id == static_cast<std::uint32_t>(MobEffectType::Saturation)) return SaturationImageId;
    return GenericPotionImageId;
}

std::string romanNumeral(int value) {
    if (value <= 0 || value > 3999) return std::to_string(value);
    static constexpr std::pair<int, std::string_view> table[] = {
        {1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"},
        {50, "L"}, {40, "XL"}, {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}
    };
    std::string result;
    for (const auto& [number, numeral] : table) {
        while (value >= number) {
            result += numeral;
            value -= number;
        }
    }
    return result;
}

std::string compactRoman(int value) {
    switch (value) {
        case 1: return "I";
        case 2: return "II";
        case 3: return "III";
        case 4: return "IV";
        case 5: return "V";
        default: return std::to_string(value);
    }
}

std::string formatDuration(int duration, bool noCounter) {
    if (noCounter || duration == -1) return "\xE2\x88\x9E";
    const int totalSeconds = std::max(0, duration / 20);
    const int minutes = totalSeconds / 60;
    const int seconds = totalSeconds % 60;
    std::string result = std::to_string(minutes) + ":";
    if (seconds < 10) result += '0';
    result += std::to_string(seconds);
    return result;
}

void* hudMobEffectsRendererDetour(void* renderer, void* context, void* client, void* owner, int pass, void* renderAabb) {
    if (moduleInstance && moduleInstance->enabled && moduleInstance->renderNative(context, client)) return nullptr;
    if (!hudMobEffectsRendererOriginal) return nullptr;
    return hudMobEffectsRendererOriginal(renderer, context, client, owner, pass, renderAabb);
}

}

PotionHudModule::PotionHudModule()
    : Module("PotionHUD", "Displays active potion effects with icons, names, amplifiers, and timers.") {
    moduleInstance = this;
}

PotionHudModule::~PotionHudModule() {
    if (hudMobEffectsRendererHook) {
        bedrocktools::hooks::remove(hudMobEffectsRendererHook);
        hudMobEffectsRendererHook = nullptr;
        hudMobEffectsRendererOriginal = nullptr;
    }
    if (moduleInstance == this) moduleInstance = nullptr;
}

void PotionHudModule::onInit() {
    pl::modmenu::registerImage(InstantHealthImageId, potionhud_assets::InstantHealthPixels, potionhud_assets::InstantHealthWidth, potionhud_assets::InstantHealthHeight);
    pl::modmenu::registerImage(InstantDamageImageId, potionhud_assets::InstantDamagePixels, potionhud_assets::InstantDamageWidth, potionhud_assets::InstantDamageHeight);
    pl::modmenu::registerImage(SaturationImageId, potionhud_assets::SaturationPixels, potionhud_assets::SaturationWidth, potionhud_assets::SaturationHeight);
    pl::modmenu::registerImage(GenericPotionImageId, potionhud_assets::GenericPotionPixels, potionhud_assets::GenericPotionWidth, potionhud_assets::GenericPotionHeight);

    const std::uintptr_t renderer = pl::memory::resolveVtableFunction(
        "21HudMobEffectsRenderer",
        bedrocktools::sdk::offsets::VTable::HudMobEffectsRendererRender,
        MinecraftLibrary);
    if (renderer && !hudMobEffectsRendererHook) {
        hudMobEffectsRendererHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(renderer),
            reinterpret_cast<void*>(hudMobEffectsRendererDetour),
            reinterpret_cast<void**>(&hudMobEffectsRendererOriginal));
    }
}

void PotionHudModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("effects")
        .category("effects", "Effects", "Choose what PotionHUD displays")
        .category("layout", "Layout", "Size, order, spacing, and text placement")
        .category("text", "Text", "Timer and label appearance")
        .category("colors", "Colors", "Normal, expiring, and shadow colors")
        .category("editor", "HUD Editor", "Placement and snapping while editing the HUD");

    auto node = [](std::string key, std::string title, std::string category, ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = std::move(key);
        value.title = std::move(title);
        value.category = std::move(category);
        value.type = type;
        return value;
    };
    auto section = [&](const char* id, const char* title, const char* category) {
        auto value = node(id, title, category, ConfigControlTypeV2::Section);
        value.key.clear();
        schema.node(std::move(value));
    };
    auto toggle = [&](const char* key, const char* title, const char* category, const char* sectionId) {
        auto value = node(key, title, category, ConfigControlTypeV2::Toggle);
        value.section = sectionId;
        schema.node(std::move(value));
    };
    auto slider = [&](const char* key, const char* title, const char* category, const char* sectionId,
                      const char* min, const char* max, const char* step, const char* unit) {
        auto value = node(key, title, category, ConfigControlTypeV2::SliderFloat);
        value.section = sectionId;
        value.minValue = min;
        value.maxValue = max;
        value.step = step;
        value.unit = unit;
        schema.node(std::move(value));
    };

    section("display", "Display", "effects");
    toggle("m_showText", "Show Text", "effects", "display");
    auto title = node("m_showTitle", "Show Effect Name", "effects", ConfigControlTypeV2::Toggle);
    title.section = "display";
    title.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(title));
    auto roman = node("m_useRoman", "Use Roman Numerals", "effects", ConfigControlTypeV2::Toggle);
    roman.section = "display";
    roman.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(roman));
    auto romanFull = node("m_useRomanFull", "Roman Numerals Above V", "effects", ConfigControlTypeV2::Toggle);
    romanFull.section = "display";
    romanFull.visibleWhen = {
        {"m_showText", ConfigConditionOpV2::Truthy, {}},
        {"m_useRoman", ConfigConditionOpV2::Truthy, {}}
    };
    schema.node(std::move(romanFull));
    section("activation", "Shortcut", "effects");
    auto keybind = node("keybind", "Toggle Keybind", "effects", ConfigControlTypeV2::Keybind);
    keybind.section = "activation";
    schema.node(std::move(keybind));

    section("effect_layout", "Effect Stack", "layout");
    slider("m_uiScale", "UI Scale", "layout", "effect_layout", "0.5", "3", "0.05", "x");
    slider("m_spacing", "Row Spacing", "layout", "effect_layout", "0.25", "3", "0.05", "x");
    toggle("m_bottomUp", "Bottom Up", "layout", "effect_layout");
    section("text_layout", "Text Placement", "layout");
    auto side = node("m_textSide", "Text Side", "layout", ConfigControlTypeV2::Choice);
    side.section = "text_layout";
    side.choiceStyle = ConfigChoiceStyleV2::Segmented;
    side.options = {
        {"0", "Right", {}, {}, false, {}, false},
        {"1", "Left", {}, {}, false, {}, false}
    };
    side.defaultValue = "0";
    side.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(side));
    auto textOffset = node("m_textOffsetX", "Distance From Icon", "layout", ConfigControlTypeV2::SliderFloat);
    textOffset.section = "text_layout";
    textOffset.minValue = "0";
    textOffset.maxValue = "20";
    textOffset.step = "0.5";
    textOffset.unit = " px";
    textOffset.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(textOffset));

    section("text_style", "Text Appearance", "text");
    auto textSize = node("m_textSize", "Text Size", "text", ConfigControlTypeV2::SliderFloat);
    textSize.section = "text_style";
    textSize.minValue = "4";
    textSize.maxValue = "20";
    textSize.step = "0.5";
    textSize.unit = " px";
    textSize.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(textSize));
    auto shadow = node("m_textShadow", "Text Shadow", "text", ConfigControlTypeV2::Toggle);
    shadow.section = "text_style";
    shadow.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(shadow));
    auto shadowOffset = node("m_shadowOffset", "Shadow Offset", "text", ConfigControlTypeV2::SliderFloat);
    shadowOffset.section = "text_style";
    shadowOffset.minValue = "0.25";
    shadowOffset.maxValue = "5";
    shadowOffset.step = "0.25";
    shadowOffset.unit = " px";
    shadowOffset.visibleWhen = {
        {"m_showText", ConfigConditionOpV2::Truthy, {}},
        {"m_textShadow", ConfigConditionOpV2::Truthy, {}}
    };
    schema.node(std::move(shadowOffset));

    section("text_colors", "Text Colors", "colors");
    auto mainColor = node("m_mainColor", "Main Color", "colors", ConfigControlTypeV2::Color);
    mainColor.section = "text_colors";
    mainColor.defaultValue = "#FFFFFFFF";
    mainColor.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(mainColor));
    auto lowColor = node("m_lowColor", "Effect About To Expire", "colors", ConfigControlTypeV2::Color);
    lowColor.section = "text_colors";
    lowColor.defaultValue = "#FFFF4040";
    lowColor.visibleWhen = {{"m_showText", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(lowColor));
    auto shadowColor = node("m_shadowColor", "Shadow Color", "colors", ConfigControlTypeV2::Color);
    shadowColor.section = "text_colors";
    shadowColor.defaultValue = "#8C000000";
    shadowColor.visibleWhen = {
        {"m_showText", ConfigConditionOpV2::Truthy, {}},
        {"m_textShadow", ConfigConditionOpV2::Truthy, {}}
    };
    schema.node(std::move(shadowColor));

    auto help = node("editor_help", "Move PotionHUD In The HUD Editor", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "The editor moves the whole active-effect stack as one HUD element.";
    schema.node(std::move(help));
    section("snapping", "Snapping", "editor");
    auto snapping = node("snap_targets", "Snap To", "editor", ConfigControlTypeV2::ToggleGroup);
    snapping.key.clear();
    snapping.section = "snapping";
    snapping.choiceStyle = ConfigChoiceStyleV2::Chips;
    snapping.options = {
        {"grid", "Grid", {}, "m_snapToGrid", false, {}, false},
        {"elements", "Other Elements", {}, "m_snapToElements", false, {}, false},
        {"center", "Screen Center", {}, "m_snapToScreenCenter", false, {}, false}
    };
    schema.node(std::move(snapping));
    slider("m_gridSize", "Grid Size", "editor", "snapping", "1", "100", "1", " px");
    slider("m_gridGap", "Gap Between Elements", "editor", "snapping", "0", "100", "1", " px");
    slider("m_snapThreshold", "Snap Distance", "editor", "snapping", "1", "100", "1", " px");

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void PotionHudModule::onDisable() {
    clearRuntime();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

PotionHudModule::ConfigSnapshot PotionHudModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    return {
        hudPosX,
        hudPosY,
        m_uiScale,
        m_spacing,
        m_bottomUp,
        m_showText,
        m_showTitle,
        m_useRoman,
        m_useRomanFull,
        m_textSize,
        m_textOffsetX,
        m_textSide,
        m_textShadow,
        m_shadowOffset,
        parseColor(m_mainColor, 0xFFFFFFFFu),
        parseColor(m_lowColor, 0xFFFF4040u),
        parseColor(m_shadowColor, 0x8C000000u),
        m_gridSize,
        m_gridGap,
        m_snapThreshold,
        (m_snapToGrid ? pl::modmenu::HudSnapGrid : pl::modmenu::HudSnapNone) |
            (m_snapToElements ? pl::modmenu::HudSnapElements : pl::modmenu::HudSnapNone) |
            (m_snapToScreenCenter ? pl::modmenu::HudSnapScreenCenter : pl::modmenu::HudSnapNone)
    };
}

std::vector<PotionHudModule::RuntimeEffect> PotionHudModule::snapshotRuntime(float& surfaceScale) const {
    std::lock_guard lock(m_runtimeMutex);
    surfaceScale = m_surfaceScale;
    return m_runtimeEffects;
}

void PotionHudModule::clearRuntime() {
    std::lock_guard lock(m_runtimeMutex);
    m_runtimeEffects.clear();
    m_surfaceScale = 1.0f;
    m_runtimeValid.store(false, std::memory_order_release);
}

float PotionHudModule::iconSurfaceSize(const ConfigSnapshot& config, float surfaceScale) {
    return std::max(1.0f, VanillaEffectSize * config.uiScale * std::max(0.1f, surfaceScale));
}

std::string PotionHudModule::titleForEffect(const RuntimeEffect& effect, const ConfigSnapshot& config) {
    const int level = effect.amplifier + 1;
    std::string result(effectName(effect.id));
    result += ' ';
    if (!config.useRoman) result += std::to_string(level);
    else if (config.useRomanFull) result += romanNumeral(level);
    else result += compactRoman(level);
    return result;
}

float PotionHudModule::rowSurfaceHeight(const ConfigSnapshot& config, float surfaceScale) {
    const float icon = iconSurfaceSize(config, surfaceScale);
    if (!config.showText) return icon;
    const float textSize = config.textSize * config.uiScale * surfaceScale;
    const float timerSize = textSize * 0.8f;
    const float textHeight = config.showTitle ? textSize + timerSize * 1.15f : timerSize;
    return std::max(icon, std::max(1.0f, textHeight));
}

float PotionHudModule::textSurfaceWidth(const ConfigSnapshot& config, const std::vector<RuntimeEffect>& effects, float surfaceScale) {
    if (!config.showText || effects.empty()) return 0.0f;
    const float textSize = config.textSize * config.uiScale * surfaceScale;
    const float timerSize = textSize * 0.8f;
    float width = timerSize * 5.0f * 0.56f;
    if (config.showTitle) {
        for (const auto& effect : effects) {
            width = std::max(width, static_cast<float>(titleForEffect(effect, config).size()) * textSize * 0.56f);
        }
    }
    return std::max(1.0f, width + surfaceScale * config.uiScale * 2.0f);
}

void PotionHudModule::submitEditorElement(const ConfigSnapshot& config, const std::vector<RuntimeEffect>& effects, float surfaceScale) {
    const float icon = iconSurfaceSize(config, surfaceScale);
    const float textWidth = textSurfaceWidth(config, effects, surfaceScale);
    const float gap = config.showText ? config.textOffsetX * config.uiScale * surfaceScale : 0.0f;
    const float rowHeight = rowSurfaceHeight(config, surfaceScale);
    const float rowStride = rowHeight * std::max(0.25f, config.spacing);
    const std::size_t count = std::max<std::size_t>(1, effects.size());

    pl::modmenu::HudEditorElement element;
    element.elementId = "bedrocktools.potionhud.effects";
    element.displayName = "PotionHUD";
    element.positionKeyX = "hudPosX";
    element.positionKeyY = "hudPosY";
    element.x = config.hudPosX;
    element.y = config.hudPosY;
    element.width = std::max(1.0f, icon + (config.showText && !effects.empty() ? gap + textWidth : 0.0f));
    element.height = std::max(1.0f, rowHeight + static_cast<float>(count - 1) * rowStride);
    element.gridSize = config.gridSize;
    element.gridGap = config.gridGap;
    element.snapThreshold = config.snapThreshold;
    element.snapFlags = config.snapFlags;
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>(&element, 1));
}

bool PotionHudModule::renderNative(void* context, void* client) {
    auto fail = [&]() {
        clearRuntime();
        return false;
    };
    if (!context || !client) return fail();
    void* player = getLocalPlayer(client);
    if (!player) return fail();
    MobEffectsComponent* component = getMobEffects(player);
    if (!component) return fail();

    std::vector<RawEffect> rawEffects;
    if (!copyEffects(component, rawEffects)) return fail();
    std::vector<RuntimeEffect> effects;
    effects.reserve(rawEffects.size());
    for (const auto& effect : rawEffects) effects.push_back({effect.id, effect.duration, effect.amplifier, effect.noCounter, false});

    const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
    const RectangleArea full = getFullClippingRectangle(context);
    if (surface.width <= 0.0f || surface.height <= 0.0f || !validRectangle(full)) return fail();

    const float uiWidth = full.x1 - full.x0;
    const float uiHeight = full.y1 - full.y0;
    const float scaleX = surface.width / uiWidth;
    const float scaleY = surface.height / uiHeight;
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 0.0f || scaleY <= 0.0f) return fail();
    const float surfaceScale = std::min(scaleX, scaleY);
    const ConfigSnapshot config = snapshotConfig();
    const float icon = iconSurfaceSize(config, surfaceScale);
    const float textWidth = textSurfaceWidth(config, effects, surfaceScale);
    const float gap = config.showText ? config.textOffsetX * config.uiScale * surfaceScale : 0.0f;
    const float iconSurfaceX = config.hudPosX + (config.showText && config.textSide == 1 ? textWidth + gap : 0.0f);
    const float rowHeight = rowSurfaceHeight(config, surfaceScale);
    const float rowStride = rowHeight * std::max(0.25f, config.spacing);

    for (std::size_t row = 0; row < effects.size(); ++row) {
        const std::size_t source = config.bottomUp ? effects.size() - 1 - row : row;
        RuntimeEffect& effect = effects[source];
        if (!usesNativeTexture(effect.id)) continue;
        const std::string_view path = effectTexturePath(effect.id);
        if (path.empty()) continue;

        TexturePtr texture = getTexture(context, ResourceLocation(path));
        if (!texture.clientTexture) continue;

        const float ySurface = config.hudPosY + static_cast<float>(row) * rowStride;
        const float xUi = full.x0 + iconSurfaceX / scaleX;
        const float yUi = full.y0 + ySurface / scaleY;
        const float widthUi = icon / scaleX;
        const float heightUi = icon / scaleY;
        if (!std::isfinite(xUi) || !std::isfinite(yUi) || !std::isfinite(widthUi) || !std::isfinite(heightUi)) continue;
        drawImage(context, texture.getClientTexture(), {xUi, yUi}, {widthUi, heightUi});
        flushImages(context);
        effect.nativeIcon = true;
    }

    {
        std::lock_guard lock(m_runtimeMutex);
        m_runtimeEffects = std::move(effects);
        m_surfaceScale = surfaceScale;
    }
    m_runtimeValid.store(true, std::memory_order_release);
    return true;
}

void PotionHudModule::onFrame() {
    if (!enabled) return;
    const ConfigSnapshot config = snapshotConfig();
    float surfaceScale = 1.0f;
    std::vector<RuntimeEffect> effects;
    if (m_runtimeValid.load(std::memory_order_acquire)) effects = snapshotRuntime(surfaceScale);
    submitEditorElement(config, effects, surfaceScale);

    const float icon = iconSurfaceSize(config, surfaceScale);
    const float textWidth = textSurfaceWidth(config, effects, surfaceScale);
    const float gap = config.showText ? config.textOffsetX * config.uiScale * surfaceScale : 0.0f;
    const float iconX = config.hudPosX + (config.showText && config.textSide == 1 ? textWidth + gap : 0.0f);
    const float rowHeight = rowSurfaceHeight(config, surfaceScale);
    const float rowStride = rowHeight * std::max(0.25f, config.spacing);
    const float textSize = std::max(1.0f, config.textSize * config.uiScale * surfaceScale);
    const float timerSize = std::max(1.0f, textSize * 0.8f);
    const float shadowOffset = config.shadowOffset * config.uiScale * surfaceScale;

    std::vector<pl::modmenu::DrawCommand> commands;
    commands.reserve(effects.size() * 6);

    auto addText = [&](float x, float y, float widthMode, float size, std::uint32_t color, std::string text) {
        pl::modmenu::DrawCommand command;
        command.type = pl::modmenu::DrawCommandType::Text;
        command.x = x;
        command.y = y;
        command.w = widthMode;
        command.size = size;
        command.color = color;
        command.text = std::move(text);
        commands.push_back(std::move(command));
    };

    for (std::size_t row = 0; row < effects.size(); ++row) {
        const std::size_t source = config.bottomUp ? effects.size() - 1 - row : row;
        const RuntimeEffect& effect = effects[source];
        const float rowY = config.hudPosY + static_cast<float>(row) * rowStride;

        if (!effect.nativeIcon) {
            pl::modmenu::DrawCommand image;
            image.type = pl::modmenu::DrawCommandType::Image;
            image.x = iconX;
            image.y = rowY;
            image.w = icon;
            image.h = icon;
            image.color = 0xFFFFFFFFu;
            image.imageId = fallbackImageId(effect.id);
            commands.push_back(std::move(image));
        }

        if (!config.showText) continue;
        const bool left = config.textSide == 1;
        const float textX = left ? config.hudPosX + textWidth : iconX + icon + gap;
        const float widthMode = left ? -1.0f : 0.0f;
        const bool expiring = !effect.noCounter && effect.duration != -1 && effect.duration / 20 <= WarningSeconds;
        const std::uint32_t effectColor = expiring ? config.lowColor : config.mainColor;
        const std::string timer = formatDuration(effect.duration, effect.noCounter);

        if (config.showTitle) {
            const std::string title = titleForEffect(effect, config);
            const float titleY = rowY + textSize;
            const float timerY = rowY + textSize + timerSize * 1.05f;
            if (config.textShadow) {
                addText(textX + shadowOffset, titleY + shadowOffset, widthMode, textSize, config.shadowColor, title);
                addText(textX + shadowOffset, timerY + shadowOffset, widthMode, timerSize, config.shadowColor, timer);
            }
            addText(textX, titleY, widthMode, textSize, effectColor, title);
            addText(textX, timerY, widthMode, timerSize, effectColor, timer);
        } else {
            const float timerY = rowY + icon * 0.5f + timerSize * 0.35f;
            if (config.textShadow) addText(textX + shadowOffset, timerY + shadowOffset, widthMode, timerSize, config.shadowColor, timer);
            addText(textX, timerY, widthMode, timerSize, effectColor, timer);
        }
    }

    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void PotionHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_configMutex);
    if (j.contains("hudPosX")) hudPosX = std::clamp(j["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudPosY")) hudPosY = std::clamp(j["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_uiScale")) m_uiScale = std::clamp(j["m_uiScale"].get<float>(), 0.5f, 3.0f);
    if (j.contains("m_spacing")) m_spacing = std::clamp(j["m_spacing"].get<float>(), 0.25f, 3.0f);
    if (j.contains("m_bottomUp")) m_bottomUp = j["m_bottomUp"].get<bool>();
    if (j.contains("m_showText")) m_showText = j["m_showText"].get<bool>();
    if (j.contains("m_showTitle")) m_showTitle = j["m_showTitle"].get<bool>();
    if (j.contains("m_useRoman")) m_useRoman = j["m_useRoman"].get<bool>();
    if (j.contains("m_useRomanFull")) m_useRomanFull = j["m_useRomanFull"].get<bool>();
    if (j.contains("m_textSize")) m_textSize = std::clamp(j["m_textSize"].get<float>(), 4.0f, 20.0f);
    if (j.contains("m_textOffsetX")) m_textOffsetX = std::clamp(j["m_textOffsetX"].get<float>(), 0.0f, 20.0f);
    if (j.contains("m_textSide")) {
        try {
            std::string value = j["m_textSide"].get<std::string>();
            const std::size_t separator = value.find(',');
            if (separator != std::string::npos) value.resize(separator);
            m_textSide = std::clamp(std::stoi(value), 0, 1);
        } catch (...) {
        }
    }
    if (j.contains("m_textShadow")) m_textShadow = j["m_textShadow"].get<bool>();
    if (j.contains("m_shadowOffset")) m_shadowOffset = std::clamp(j["m_shadowOffset"].get<float>(), 0.25f, 5.0f);
    if (j.contains("m_mainColor")) m_mainColor = j["m_mainColor"].get<std::string>();
    if (j.contains("m_lowColor")) m_lowColor = j["m_lowColor"].get<std::string>();
    if (j.contains("m_shadowColor")) m_shadowColor = j["m_shadowColor"].get<std::string>();
    if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
    if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
    if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
}

void PotionHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);
    j["isHudModule"] = true;
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["m_uiScale"] = m_uiScale;
    j["m_spacing"] = m_spacing;
    j["m_bottomUp"] = m_bottomUp;
    j["m_showText"] = m_showText;
    j["m_showTitle"] = m_showTitle;
    j["m_useRoman"] = m_useRoman;
    j["m_useRomanFull"] = m_useRomanFull;
    j["m_textSize"] = m_textSize;
    j["m_textOffsetX"] = m_textOffsetX;
    j["m_textSide"] = std::to_string(m_textSide) + ",Right,Left";
    j["m_textShadow"] = m_textShadow;
    j["m_shadowOffset"] = m_shadowOffset;
    j["m_mainColor"] = m_mainColor;
    j["m_lowColor"] = m_lowColor;
    j["m_shadowColor"] = m_shadowColor;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}
