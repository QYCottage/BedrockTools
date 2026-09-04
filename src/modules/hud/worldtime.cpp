#include "worldtime.hpp"
#include "modules/ModuleRegistry.hpp"
#include "modules/player/timechanger.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <algorithm>
#include <cstdio>

namespace {
std::string formatWorldTime(int dayTicks, bool use24HourClock) {
    if (dayTicks < 0) return "Time: --:--";
    const int totalMinutes = ((dayTicks + 6000) % 24000) * 1440 / 24000;
    const int hour = totalMinutes / 60;
    const int minute = totalMinutes % 60;
    char text[32];
    if (use24HourClock) {
        std::snprintf(text, sizeof(text), "Time: %02d:%02d", hour, minute);
    } else {
        std::snprintf(text, sizeof(text), "Time: %d:%02d %s",
                      hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
    }
    return text;
}

float calcTextWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (char c : text) {
        if (c == 'i' || c == 'l' || c == '1' || c == ':' || c == '.' || c == ' ') width += size * 0.3f;
        else if (c == 'm' || c == 'w' || c == 'M' || c == 'W') width += size * 0.8f;
        else width += size * 0.58f;
    }
    return width;
}
}

WorldTimeModule::WorldTimeModule()
    : Module("World Time", "Displays the world clock in 12-hour or 24-hour format.") {}

WorldTimeModule::~WorldTimeModule() {
    bedrocktools::events::bus().unsubscribe(m_subscription);
}

void WorldTimeModule::onInit() {
    if (m_subscription != 0) return;
    m_timeChanger = static_cast<TimeChangerModule*>(ModuleRegistry::get().find("bedrocktools.Time Changer"));
    m_subscription = bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) {
            auto* player = event.player;
            auto* level = player ? player->level() : nullptr;
            if (!level || !m_timeChanger) {
                m_dayTicks.store(-1, std::memory_order_relaxed);
                return;
            }
            int ticks = m_timeChanger->getRealTime(level) % 24000;
            if (ticks < 0) ticks += 24000;
            m_dayTicks.store(ticks, std::memory_order_relaxed);
        });
}

void WorldTimeModule::onDisable() {
    m_dayTicks.store(-1, std::memory_order_relaxed);
}

void WorldTimeModule::onFrame() {
    if (!enabled) return;
    float x, y, size, opacity;
    bool use24HourClock, background;
    {
        std::lock_guard lock(m_configMutex);
        x = hudPosX;
        y = hudPosY;
        size = m_size;
        opacity = m_backgroundOpacity;
        use24HourClock = m_use24HourClock;
        background = m_background;
    }
    const std::string text = formatWorldTime(m_dayTicks.load(std::memory_order_relaxed), use24HourClock);
    const float width = calcTextWidth(text, size) + 12.0f;
    const float height = size + 8.0f;
    std::vector<pl::modmenu::DrawCommand> commands;
    if (background) {
        pl::modmenu::DrawCommand box;
        box.type = pl::modmenu::DrawCommandType::RectFilled;
        box.x = x;
        box.y = y;
        box.w = width;
        box.h = height;
        box.color = static_cast<std::uint32_t>(opacity * 255.0f) << 24;
        commands.push_back(std::move(box));
    }
    pl::modmenu::DrawCommand label;
    label.type = pl::modmenu::DrawCommandType::Text;
    label.x = x;
    label.y = y;
    label.w = width;
    label.h = height;
    label.size = size;
    label.color = 0xFFFFFFFF;
    label.text = text;
    commands.push_back(std::move(label));
    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void WorldTimeModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("clock")
        .category("clock", "Clock", "World time display format")
        .category("appearance", "Appearance", "Text and background");
    auto node = [](const char* key, const char* title, const char* category, ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = key;
        value.title = title;
        value.category = category;
        value.type = type;
        return value;
    };
    auto format = node("m_use24HourClock", "Time Format", "clock", ConfigControlTypeV2::Choice);
    format.choiceStyle = ConfigChoiceStyleV2::Segmented;
    format.options = {{"true", "24-hour"}, {"false", "12-hour (AM/PM)"}};
    schema.node(std::move(format));
    schema.node(node("keybind", "Keybind", "clock", ConfigControlTypeV2::Keybind));
    auto size = node("m_size", "Text Size", "appearance", ConfigControlTypeV2::SliderFloat);
    size.minValue = "6";
    size.maxValue = "100";
    size.step = "1";
    size.unit = " px";
    schema.node(std::move(size));
    schema.node(node("m_background", "Background", "appearance", ConfigControlTypeV2::Toggle));
    auto opacity = node("m_backgroundOpacity", "Background Opacity", "appearance", ConfigControlTypeV2::SliderFloat);
    opacity.minValue = "0";
    opacity.maxValue = "1";
    opacity.step = "0.01";
    opacity.visibleWhen = {{"m_background", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(opacity));
    setConfigSchemaJson(moduleId, schema.toJson());
}

void WorldTimeModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_configMutex);
    if (j.contains("hudPosX")) hudPosX = std::clamp(j["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudPosY")) hudPosY = std::clamp(j["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_size")) m_size = std::clamp(j["m_size"].get<float>(), 6.0f, 100.0f);
    if (j.contains("m_use24HourClock")) m_use24HourClock = j["m_use24HourClock"].get<bool>();
    if (j.contains("m_background")) m_background = j["m_background"].get<bool>();
    if (j.contains("m_backgroundOpacity")) m_backgroundOpacity = std::clamp(j["m_backgroundOpacity"].get<float>(), 0.0f, 1.0f);
}

void WorldTimeModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = true;
    j["m_size"] = m_size;
    j["m_use24HourClock"] = m_use24HourClock;
    j["m_background"] = m_background;
    j["m_backgroundOpacity"] = m_backgroundOpacity;
}
