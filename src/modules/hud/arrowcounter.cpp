#include "arrowcounter.hpp"

#include "core/InventoryAccess.hpp"
#include "modules/ModuleRegistry.hpp"
#include <algorithm>
#include <bedrocktools/events/EventBus.hpp>
#include <charconv>
#include <string_view>

namespace {

constexpr std::string_view ItemIdentifier = "arrow";

float textWidth(const std::string& text, float size) {
    float width = 0.0f;
    for (const char character : text) {
        if (character == 'i' || character == 'l' || character == '1' ||
            character == ':' || character == '.' || character == ' ') {
            width += size * 0.3f;
        } else if (character == 'm' || character == 'w' ||
                   character == 'M' || character == 'W') {
            width += size * 0.8f;
        } else {
            width += size * 0.58f;
        }
    }
    return width;
}

std::uint32_t parseRgb(const std::string& value, std::uint32_t fallback) {
    std::string_view text(value);
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() == 8) text.remove_prefix(2);
    if (text.size() != 6) return fallback;
    std::uint32_t result = 0;
    const auto conversion = std::from_chars(text.data(), text.data() + text.size(), result, 16);
    return conversion.ec == std::errc{} && conversion.ptr == text.data() + text.size() ? result & 0x00FFFFFFU : fallback;
}

std::uint32_t colorWithOpacity(const std::string& value, float opacity, std::uint32_t fallback) {
    const auto alpha = static_cast<std::uint32_t>(
        std::clamp(opacity, 0.0f, 1.0f) * 255.0f
    );
    return (alpha << 24U) | parseRgb(value, fallback);
}

}

ArrowCounterModule::ArrowCounterModule()
    : Module("Arrow Counter", "Displays arrows in the main inventory and offhand without double counting.") {}

void ArrowCounterModule::onInit() {
    bedrocktools::core::InventoryAccess::get().initialize();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [this](auto& event) { updateCount(event.player); }
    );
}

void ArrowCounterModule::updateCount(bedrocktools::sdk::Player* player) {
    if (!enabled) return;
    m_count.store(bedrocktools::core::InventoryAccess::get().countItems(player, ItemIdentifier),
                  std::memory_order_release);
}

void ArrowCounterModule::onDisable() {
    m_count.store(0, std::memory_order_release);
}

void ArrowCounterModule::onFrame() {
    if (!enabled) return;
    const int count = m_count.load(std::memory_order_acquire);
    std::lock_guard lock(m_configMutex);
    if (!m_showZero && count == 0) {
        submitDrawCommands(moduleId, {});
        return;
    }

    const std::string text = "Arrows: " + std::to_string(count);
    const float size = std::clamp(m_size, 6.0f, 100.0f);
    const float boxWidth = textWidth(text, size) + 12.0f;
    const float boxHeight = size + 8.0f;
    std::vector<PLModMenu_DrawCommand> commands;

    if (m_background) {
        PLModMenu_DrawCommand background{};
        background.type = PL_DRAW_RECT_FILLED;
        background.x = hudPosX;
        background.y = hudPosY;
        background.w = boxWidth;
        background.h = boxHeight;
        background.color = colorWithOpacity(
            m_backgroundColor,
            m_backgroundOpacity,
            0x000000U
        );
        commands.push_back(background);
    }

    PLModMenu_DrawCommand label{};
    label.type = PL_DRAW_TEXT;
    label.x = hudPosX + 6.0f;
    label.y = hudPosY + 4.0f;
    label.w = boxWidth;
    label.h = boxHeight;
    label.color = 0xFF000000U | parseRgb(m_textColor, 0xFFFFFFU);
    label.size = size;
    label.text = text.c_str();
    commands.push_back(label);
    submitDrawCommands(moduleId, commands);
}

void ArrowCounterModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    std::lock_guard lock(m_configMutex);
    if (json.contains("hudPosX")) hudPosX = json["hudPosX"].get<float>();
    if (json.contains("hudPosY")) hudPosY = json["hudPosY"].get<float>();
    if (json.contains("isHudModule")) isHudModule = json["isHudModule"].get<bool>();
    if (json.contains("m_size")) m_size = std::clamp(json["m_size"].get<float>(), 6.0f, 100.0f);
    if (json.contains("m_background")) m_background = json["m_background"].get<bool>();
    if (json.contains("m_backgroundColor")) m_backgroundColor = json["m_backgroundColor"].get<std::string>();
    if (json.contains("m_backgroundOpacity")) {
        m_backgroundOpacity = std::clamp(json["m_backgroundOpacity"].get<float>(), 0.0f, 1.0f);
    }
    if (json.contains("m_textColor")) m_textColor = json["m_textColor"].get<std::string>();
    if (json.contains("m_showZero")) m_showZero = json["m_showZero"].get<bool>();
}

void ArrowCounterModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    std::lock_guard lock(m_configMutex);
    json["hudPosX"] = hudPosX;
    json["hudPosY"] = hudPosY;
    json["isHudModule"] = isHudModule;
    json["m_size"] = m_size;
    json["m_background"] = m_background;
    json["m_backgroundColor"] = m_backgroundColor;
    json["m_backgroundOpacity"] = std::clamp(m_backgroundOpacity, 0.0f, 1.0f);
    json["m_textColor"] = m_textColor;
    json["m_showZero"] = m_showZero;
}

void ArrowCounterModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("display").category("display", "Display").category("appearance", "Appearance");
    auto node = [](const char* key, const char* title, const char* category, ConfigControlTypeV2 type) {
        ConfigNodeV2 value;
        value.id = key;
        value.key = key;
        value.title = title;
        value.category = category;
        value.type = type;
        return value;
    };
    schema.node(node("m_showZero", "Show When Empty", "display", ConfigControlTypeV2::Toggle));
    schema.node(node("keybind", "Keybind", "display", ConfigControlTypeV2::Keybind));
    auto size = node("m_size", "Text Size", "appearance", ConfigControlTypeV2::SliderFloat);
    size.minValue = "6";
    size.maxValue = "100";
    size.step = "1";
    schema.node(std::move(size));
    auto textColor = node("m_textColor", "Text Color", "appearance", ConfigControlTypeV2::Color);
    textColor.colorAlpha = false;
    schema.node(std::move(textColor));
    schema.node(node("m_background", "Background", "appearance", ConfigControlTypeV2::Toggle));
    auto backgroundColor = node("m_backgroundColor", "Background Color", "appearance", ConfigControlTypeV2::Color);
    backgroundColor.colorAlpha = false;
    backgroundColor.visibleWhen = {{"m_background", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(backgroundColor));
    auto opacity = node("m_backgroundOpacity", "Background Opacity", "appearance", ConfigControlTypeV2::SliderFloat);
    opacity.minValue = "0";
    opacity.maxValue = "1";
    opacity.step = "0.01";
    opacity.visibleWhen = {{"m_background", ConfigConditionOpV2::Truthy, {}}};
    schema.node(std::move(opacity));
    setConfigSchemaJson(moduleId, schema.toJson());
}
