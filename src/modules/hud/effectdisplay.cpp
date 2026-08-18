#include "effectdisplay.hpp"

#include "effecticons.hpp"
#include "effectformat.hpp"
#include "effectlayout.hpp"
#include "core/Runtime.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <entt/entt.hpp>
#include <pl/ModMenu.hpp>

#include <algorithm>
#include <array>
#include <chrono>
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

void ensureEffectIcon(std::uint32_t id) {
    static std::unordered_map<std::uint32_t, bool> registered;
    if (id == 0 || !registered.emplace(id, true).second) return;
    const auto icon = makeEffectIcon(id, definitionFor(id).color);
    pl::modmenu::registerImage(imageIdFor(id), icon, kIconSize, kIconSize);
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
    const float iconSize = 30.0f * scale;
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
            icon.imageId = imageIdFor(effect.id);
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
    j["m_animate"] = m_animate;
    j["m_preview"] = m_preview;
    j["m_maxVisible"] = m_maxVisible;
}
