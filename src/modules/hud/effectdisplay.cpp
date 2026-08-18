#include "effectdisplay.hpp"

#include "core/Runtime.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <entt/entt.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

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

constexpr int kIconSize = 16;
using IconPixels = std::array<std::uint8_t, kIconSize * kIconSize * 4>;

EffectDisplayModule* g_effectDisplay = nullptr;

const EffectDefinition& definitionFor(std::uint32_t id) {
    static constexpr EffectDefinition unknown{"Unknown Effect", 0x888888};
    return id < kEffects.size() ? kEffects[id] : unknown;
}

const std::string& imageIdFor(std::uint32_t id) {
    static std::unordered_map<std::uint32_t, std::string> ids;
    const auto [it, inserted] = ids.try_emplace(id, "bedrocktools.effect." + std::to_string(id));
    return it->second;
}

void setPixel(IconPixels& pixels, int x, int y, std::uint32_t rgb, std::uint8_t alpha = 255) {
    if (x < 0 || y < 0 || x >= kIconSize || y >= kIconSize) return;
    const auto offset = static_cast<std::size_t>((y * kIconSize + x) * 4);
    pixels[offset] = static_cast<std::uint8_t>((rgb >> 16) & 0xFF);
    pixels[offset + 1] = static_cast<std::uint8_t>((rgb >> 8) & 0xFF);
    pixels[offset + 2] = static_cast<std::uint8_t>(rgb & 0xFF);
    pixels[offset + 3] = alpha;
}

IconPixels makeEffectIcon(std::uint32_t id, std::uint32_t color) {
    IconPixels pixels{};
    const std::uint32_t dark = ((color & 0xFEFEFE) >> 1);

    // Pixel-art medallion and one of six small glyphs. The palette follows
    // each vanilla effect color, so 36 effects remain easy to distinguish.
    for (int y = 1; y < 15; ++y) {
        for (int x = 1; x < 15; ++x) {
            const int dx = x - 7;
            const int dy = y - 7;
            const int distance = dx * dx + dy * dy;
            if (distance <= 43) setPixel(pixels, x, y, distance >= 32 ? dark : color);
        }
    }

    constexpr std::uint32_t white = 0xFFF7D6;
    switch (id % 6) {
        case 0: // flame/drop
            for (int y = 4; y <= 11; ++y) {
                const int half = y < 8 ? (y - 3) / 2 : (11 - y) / 2 + 1;
                for (int x = 7 - half; x <= 7 + half; ++x) setPixel(pixels, x, y, white);
            }
            break;
        case 1: // forward chevrons
            for (int i = 0; i < 5; ++i) {
                setPixel(pixels, 4 + i, 4 + i, white);
                setPixel(pixels, 4 + i, 10 - i, white);
                setPixel(pixels, 8 + i / 2, 4 + i, white);
                setPixel(pixels, 8 + i / 2, 10 - i, white);
            }
            break;
        case 2: // shield
            for (int y = 4; y <= 10; ++y) {
                const int inset = y > 8 ? y - 8 : 0;
                setPixel(pixels, 4 + inset, y, white);
                setPixel(pixels, 10 - inset, y, white);
            }
            for (int x = 4; x <= 10; ++x) setPixel(pixels, x, 4, white);
            break;
        case 3: // bubbles
            for (const auto [x, y] : std::array<std::pair<int, int>, 5>{{{5, 5}, {9, 4}, {7, 8}, {10, 10}, {4, 11}}}) {
                setPixel(pixels, x, y, white);
                setPixel(pixels, x + 1, y, white);
                setPixel(pixels, x, y + 1, white);
            }
            break;
        case 4: // eye
            for (int i = 0; i < 4; ++i) {
                setPixel(pixels, 4 + i, 7 - i / 2, white);
                setPixel(pixels, 4 + i, 8 + i / 2, white);
                setPixel(pixels, 10 - i, 7 - i / 2, white);
                setPixel(pixels, 10 - i, 8 + i / 2, white);
            }
            setPixel(pixels, 7, 7, dark);
            setPixel(pixels, 7, 8, dark);
            break;
        default: // sparkle
            for (int i = 3; i <= 11; ++i) {
                setPixel(pixels, 7, i, white);
                setPixel(pixels, i, 7, white);
            }
            setPixel(pixels, 5, 5, white);
            setPixel(pixels, 9, 9, white);
            break;
    }
    return pixels;
}

void ensureEffectIcon(std::uint32_t id) {
    static std::unordered_map<std::uint32_t, bool> registered;
    if (id == 0 || !registered.emplace(id, true).second) return;
    const auto icon = makeEffectIcon(id, definitionFor(id).color);
    pl::modmenu::registerImage(imageIdFor(id), icon, kIconSize, kIconSize);
}

std::string romanNumeral(int value) {
    if (value <= 0) return {};
    if (value > 20) return std::to_string(value);
    static constexpr std::array<std::pair<int, const char*>, 5> numerals{{
        {10, "X"}, {9, "IX"}, {5, "V"}, {4, "IV"}, {1, "I"}
    }};
    std::string result;
    for (const auto& [amount, text] : numerals) {
        while (value >= amount) {
            result += text;
            value -= amount;
        }
    }
    return result;
}

std::string formatDuration(int ticks) {
    if (ticks < 0 || ticks >= std::numeric_limits<int>::max() / 4) return "infinite";
    const int seconds = std::max(0, (ticks + 19) / 20);
    const int hours = seconds / 3600;
    const int minutes = (seconds / 60) % 60;
    const int remaining = seconds % 60;
    char output[24]{};
    if (hours > 0) std::snprintf(output, sizeof(output), "%d:%02d:%02d", hours, minutes, remaining);
    else std::snprintf(output, sizeof(output), "%d:%02d", minutes, remaining);
    return output;
}

struct LayoutCandidate {
    std::size_t stride = 0;
    std::size_t amplifierOffset = 0;
};

bool plausibleDuration(int ticks) {
    return ticks == -1 || (ticks >= 0 && ticks < 2'000'000'000);
}

bool plausibleAmplifier(int amplifier) {
    return amplifier >= 0 && amplifier <= 255;
}

bool isActiveEffect(std::uint32_t id, int duration, int amplifier) {
    return id != 0 && id <= 255 && duration != 0 && plausibleAmplifier(amplifier);
}

int scoreLayout(std::uintptr_t begin, std::size_t count, const LayoutCandidate& layout) {
    int score = 0;
    int active = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const auto address = begin + index * layout.stride;
        const auto id = *reinterpret_cast<const std::uint32_t*>(address);
        const auto duration = *reinterpret_cast<const int*>(address + 4);
        const auto amplifier = *reinterpret_cast<const int*>(address + layout.amplifierOffset);
        if (id >= 1 && id <= 64) score += 5;
        else if (id != 0) score -= 25;
        if (plausibleDuration(duration)) score += 2;
        else score -= 8;
        if (plausibleAmplifier(amplifier)) score += 2;
        else score -= 8;
        if (id == static_cast<std::uint32_t>(index) || id == static_cast<std::uint32_t>(index + 1)) ++score;
        if (isActiveEffect(id, duration, amplifier)) ++active;
    }
    if (active > 0) score += active * 4;
    else score -= 6;
    return score;
}

std::vector<EffectDisplayModule::ActiveEffect> collectEffects(
    std::uintptr_t begin,
    std::size_t count,
    const LayoutCandidate& layout
) {
    std::vector<EffectDisplayModule::ActiveEffect> result;
    result.reserve(std::min<std::size_t>(count, 36));
    for (std::size_t index = 0; index < count; ++index) {
        const auto address = begin + index * layout.stride;
        const auto id = *reinterpret_cast<const std::uint32_t*>(address);
        const auto duration = *reinterpret_cast<const int*>(address + 4);
        const auto amplifier = *reinterpret_cast<const int*>(address + layout.amplifierOffset);
        if (!isActiveEffect(id, duration, amplifier)) continue;
        result.push_back({id, duration, amplifier});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id == right.id;
    }), result.end());
    return result;
}

// Windows/MSVC MobEffectInstance is 0x88 because std::function is 64 bytes
// there. Android/libc++ uses a 32-byte std::function, so the same struct is
// 0x68. Scan every 8-byte stride instead of a short Windows-only list.
LayoutCandidate detectLayout(std::uintptr_t begin, std::size_t bytes) {
    static LayoutCandidate cached{};
    if (cached.stride != 0 && bytes % cached.stride == 0) {
        const auto count = bytes / cached.stride;
        if (count && count <= 128 && scoreLayout(begin, count, cached) > 0) return cached;
    }

    LayoutCandidate best{};
    int bestScore = 0;
    constexpr std::array<std::size_t, 7> amplifierOffsets{{0x10, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28}};

    for (std::size_t stride = 0x28; stride <= 0xC0; stride += 8) {
        if (bytes % stride != 0) continue;
        const auto count = bytes / stride;
        if (!count || count > 128) continue;

        for (const auto amplifierOffset : amplifierOffsets) {
            if (amplifierOffset + sizeof(int) > stride) continue;
            const LayoutCandidate candidate{stride, amplifierOffset};
            const int score = scoreLayout(begin, count, candidate);
            if (score > bestScore) {
                best = candidate;
                bestScore = score;
            }
        }
    }

    if (best.stride != 0) cached = best;
    return best;
}

std::vector<EffectDisplayModule::ActiveEffect> readComponent(const MobEffectsComponent& component) {
    if (!component.begin || component.end < component.begin || component.capacity < component.end) return {};

    const std::size_t bytes = component.end - component.begin;
    if (!bytes || bytes > 64 * 1024) return {};

    const auto layout = detectLayout(component.begin, bytes);
    if (layout.stride == 0) return {};
    return collectEffects(component.begin, bytes / layout.stride, layout);
}

void effectTickCallback(bedrocktools::sdk::Player* player) {
    if (g_effectDisplay && g_effectDisplay->enabled) g_effectDisplay->updateEffects(player);
}

} // namespace

EffectDisplayModule::EffectDisplayModule()
    : Module("Effect Display", "Shows every active status effect with its icon, level, and remaining duration.") {
    g_effectDisplay = this;
}

EffectDisplayModule::~EffectDisplayModule() {
    if (g_effectDisplay == this) g_effectDisplay = nullptr;
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
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        effectTickCallback(event.player);
    });
}

void EffectDisplayModule::onDisable() {
    std::lock_guard lock(m_mutex);
    m_effects.clear();
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
    std::lock_guard lock(m_mutex);
    m_effects.swap(next);
}

void EffectDisplayModule::onFrame() {
    if (!enabled) return;
    registerResources();

    std::vector<ActiveEffect> effects;
    {
        std::lock_guard lock(m_mutex);
        effects = m_effects;
    }
    if (effects.empty() && m_preview) {
        effects = {{1, 68 * 20, 1}, {12, 191 * 20, 0}, {13, 50 * 20, 0}, {14, 213 * 20, 0}};
    }

    const float scale = std::clamp(m_scale, 0.25f, 5.0f);
    const float panelWidth = std::max(110.0f, m_width) * scale;
    const float rowHeight = 46.0f * scale;
    const float padding = 7.0f * scale;
    const float iconSize = 28.0f * scale;
    const float nameSize = 18.0f * scale;
    const float durationSize = 16.0f * scale;
    const int visible = effects.empty()
        ? 1
        : std::min<int>(static_cast<int>(effects.size()), std::max(1, m_maxVisible));
    const float panelHeight = padding * 2.0f + rowHeight * visible;

    std::vector<PLModMenu_DrawCommand> commands;
    commands.reserve(2 + static_cast<std::size_t>(visible) * 3);

    if (m_showBackground) {
        PLModMenu_DrawCommand background{};
        background.type = PL_DRAW_RECT_FILLED;
        background.x = hudPosX;
        background.y = hudPosY;
        background.w = panelWidth;
        background.h = panelHeight;
        const auto alpha = static_cast<std::uint32_t>(std::clamp(m_backgroundOpacity, 0.0f, 1.0f) * 255.0f);
        background.color = (alpha << 24) | 0x10151F;
        commands.push_back(background);
    } else {
        // Keep a nearly-transparent hitbox so the HUD editor can still grab
        // the module when the background is hidden and no effects are active.
        PLModMenu_DrawCommand hitbox{};
        hitbox.type = PL_DRAW_RECT_FILLED;
        hitbox.x = hudPosX;
        hitbox.y = hudPosY;
        hitbox.w = panelWidth;
        hitbox.h = panelHeight;
        hitbox.color = 0x02000000;
        commands.push_back(hitbox);
    }

    if (effects.empty()) {
        PLModMenu_DrawCommand emptyCommand{};
        emptyCommand.type = PL_DRAW_TEXT;
        emptyCommand.x = hudPosX + padding;
        emptyCommand.y = hudPosY + padding;
        emptyCommand.w = panelWidth - padding * 2.0f;
        emptyCommand.h = nameSize + 3.0f * scale;
        emptyCommand.size = nameSize;
        emptyCommand.color = 0xFFD8D8D8;
        emptyCommand.fontId = "minecraft";
        emptyCommand.text = "No Effects";
        commands.push_back(emptyCommand);
        ::submitDrawCommands(moduleId, commands);
        return;
    }

    for (int index = 0; index < visible; ++index) {
        const auto& effect = effects[static_cast<std::size_t>(index)];
        ensureEffectIcon(effect.id);
        const float rowY = hudPosY + padding + rowHeight * index;
        float textX = hudPosX + padding;

        if (m_showIcons) {
            PLModMenu_DrawCommand icon{};
            icon.type = PL_DRAW_IMAGE;
            icon.x = textX;
            icon.y = rowY + (rowHeight - iconSize) * 0.5f;
            icon.w = iconSize;
            icon.h = iconSize;
            icon.color = 0xFFFFFFFF;
            icon.imageId = imageIdFor(effect.id);
            commands.push_back(icon);
            textX += iconSize + 7.0f * scale;
        }

        std::string name = definitionFor(effect.id).name;
        if (name.empty()) name = "Unknown Effect";
        if (m_showLevel && effect.amplifier >= 0) {
            const auto level = romanNumeral(effect.amplifier + 1);
            if (!level.empty()) name += " " + level;
        }
        const std::string duration = formatDuration(effect.durationTicks);

        PLModMenu_DrawCommand nameCommand{};
        nameCommand.type = PL_DRAW_TEXT;
        nameCommand.x = textX;
        nameCommand.y = rowY;
        nameCommand.w = panelWidth - (textX - hudPosX) - padding;
        nameCommand.h = nameSize + 3.0f * scale;
        nameCommand.size = nameSize;
        nameCommand.color = 0xFFF4F4F4;
        nameCommand.fontId = "minecraft";
        nameCommand.text = name;
        commands.push_back(nameCommand);

        PLModMenu_DrawCommand durationCommand{};
        durationCommand.type = PL_DRAW_TEXT;
        durationCommand.x = textX;
        durationCommand.y = rowY + 20.0f * scale;
        durationCommand.w = nameCommand.w;
        durationCommand.h = durationSize + 2.0f * scale;
        durationCommand.size = durationSize;
        durationCommand.color = 0xFFD8D8D8;
        durationCommand.fontId = "minecraft";
        durationCommand.text = duration;
        commands.push_back(durationCommand);
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
    j["m_preview"] = m_preview;
    j["m_maxVisible"] = m_maxVisible;
}
