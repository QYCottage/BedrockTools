#include "inventoryhud.hpp"

#include "huditems.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <pl/ModMenu.hpp>
#include <pl/ModMenuConfig.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace huditems = bedrocktools::huditems;
namespace layout = bedrocktools::inventoryhud;
namespace decor = bedrocktools::slotdecor;
using layout::GridLayout;
using decor::SlotRect;

constexpr std::size_t GridSlotCount = InventoryHudModule::GridSlotCount;
constexpr const char* GridElementId = "bedrocktools.inventoryhud.grid";

void renderListener(void* context, void* client, void* user) {
    auto* module = static_cast<InventoryHudModule*>(user);
    if (module && module->enabled) module->renderNative(context, client);
}

} // namespace

InventoryHudModule::InventoryHudModule()
    : Module("Inventory HUD",
             "Shows the items of your inventory grid on the HUD without opening the inventory.") {
}

InventoryHudModule::~InventoryHudModule() {
    huditems::removeRenderListener(renderListener, this);
}

void InventoryHudModule::onInit() {
    huditems::initialize();
    huditems::addRenderListener(renderListener, this);

    // The real inventory (and any other container UI) draws the same items at
    // full size, so the HUD copy is hidden while one is open. The counter
    // tracks nested opens the same way the runtime's keybind blocker does.
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>([this](auto& event) {
        if (event.screen != bedrocktools::events::ScreenKind::Container) return;
        if (event.phase == bedrocktools::events::ScreenPhase::Opened) {
            m_containerDepth.fetch_add(1, std::memory_order_acq_rel);
        } else {
            int depth = m_containerDepth.load(std::memory_order_acquire);
            while (depth > 0 &&
                   !m_containerDepth.compare_exchange_weak(depth, depth - 1, std::memory_order_acq_rel)) {
            }
        }
    });
}

void InventoryHudModule::onDisable() {
    clearRuntime();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

bool InventoryHudModule::hiddenByScreen() const {
    return m_containerDepth.load(std::memory_order_acquire) > 0;
}

bedrocktools::inventoryhud::GridLayout InventoryHudModule::gridLayout() const {
    layout::GridLayout grid;
    grid.x = hudPosX;
    grid.y = hudPosY;
    grid.slotSize = m_slotSize;
    grid.gap = m_slotGap;
    grid.columns = layout::clampColumns(static_cast<std::size_t>(std::max(1, m_columns)));
    return grid;
}

InventoryHudModule::ConfigSnapshot InventoryHudModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    ConfigSnapshot config;
    config.grid = gridLayout();
    config.stackCount = m_showStackCount;
    config.durability = m_showDurability;
    config.hideInContainer = m_hideInContainer;
    config.slotBackground = m_slotBackground;
    config.slotBgColor = huditems::withOpacity(huditems::parseColor(m_slotBgColor, 0xFF000000u), m_slotBgOpacity);
    config.countTextSize = m_countTextSize;
    config.countColor = huditems::parseColor(m_countColor, 0xFFFFFFFFu);
    config.gridSize = m_gridSize;
    config.gridGap = m_gridGap;
    config.snapThreshold = m_snapThreshold;
    config.snapFlags =
        (m_snapToGrid ? pl::modmenu::HudSnapGrid : pl::modmenu::HudSnapNone) |
        (m_snapToElements ? pl::modmenu::HudSnapElements : pl::modmenu::HudSnapNone) |
        (m_snapToScreenCenter ? pl::modmenu::HudSnapScreenCenter : pl::modmenu::HudSnapNone);
    return config;
}

void InventoryHudModule::clearRuntime() {
    for (auto& slot : m_grid) storeRuntime(slot, nullptr, nullptr, false);
}

// Publishes what the render thread saw for one slot to onFrame(); a null
// `item` clears the slot.
void InventoryHudModule::storeRuntime(SlotRuntime& runtime, void* stack, void* item, bool wantDurability) {
    const bool hasItem = item != nullptr;
    runtime.hasItem.store(hasItem, std::memory_order_release);
    runtime.count.store(hasItem ? huditems::stackCount(stack) : 0, std::memory_order_release);
    int damage = 0;
    int maxDamage = 0;
    if (hasItem && wantDurability) {
        maxDamage = huditems::itemMaxDamage(item);
        if (maxDamage > 0) damage = huditems::stackDamage(stack);
    }
    runtime.damage.store(damage, std::memory_order_release);
    runtime.maxDamage.store(maxDamage, std::memory_order_release);
}

void InventoryHudModule::renderNative(void* context, void* client) {
    const ConfigSnapshot config = snapshotConfig();
    const bool hidden = config.hideInContainer && hiddenByScreen();

    // Even while hidden the player is still needed to keep the overlay
    // bookkeeping (counts / durability) current, so only the render setup
    // is skipped.
    huditems::IconPainter painter(context, client, !hidden);
    void* localPlayer = painter.player();
    if (!localPlayer) {
        clearRuntime();
        return;
    }

    const huditems::ContainerSlots inventory = huditems::playerInventory(localPlayer);
    if (inventory.count <= layout::LastGridSlot) {
        // Not the 36-slot player inventory we expect (e.g. mid-teleport
        // rebuild); do not read outside the container.
        clearRuntime();
        return;
    }

    std::array<void*, GridSlotCount> stacks{};
    std::array<void*, GridSlotCount> items{};
    for (std::size_t i = 0; i < GridSlotCount; ++i) {
        stacks[i] = inventory.stack(layout::containerSlot(i));
        items[i] = huditems::stackItem(stacks[i]);
        storeRuntime(m_grid[i], stacks[i], items[i], config.durability);
    }

    if (!painter.ready()) return;

    // The slot cells are painted before the icons of the same pass, so each
    // item stays on top of its background. Cells cover empty slots too,
    // keeping the grid's shape steady while the inventory changes.
    if (config.slotBackground) {
        for (std::size_t i = 0; i < GridSlotCount; ++i) {
            const SlotRect rect = layout::gridSlotRect(config.grid, i);
            painter.fillRect(rect.x, rect.y, rect.size, rect.size, config.slotBgColor);
        }
    }

    // Dyed leather armor (and a few other tinted items) need the HUD opacity
    // fix pass first, otherwise their tinted pixels come out transparent.
    if (painter.supportsOpacityFix()) {
        painter.beginOpacityFixPass();
        for (std::size_t i = 0; i < GridSlotCount; ++i) {
            if (!items[i] || !huditems::needsTextureOpacityPass(stacks[i])) continue;
            const SlotRect rect = layout::gridSlotRect(config.grid, i);
            painter.drawOpacityFix(stacks[i], items[i], rect.x, rect.y, rect.size);
        }
        painter.endOpacityFixPass();
    }

    // Only occupied slots are submitted to the ItemRenderer; empty ones cost
    // nothing.
    for (std::size_t i = 0; i < GridSlotCount; ++i) {
        if (!items[i]) continue;
        const SlotRect rect = layout::gridSlotRect(config.grid, i);
        painter.draw(stacks[i], items[i], rect.x, rect.y, rect.size);
    }
}

void InventoryHudModule::onFrame() {
    if (!enabled) return;

    const ConfigSnapshot config = snapshotConfig();

    // The editor box always covers the full grid so the element can be placed
    // even while every slot is empty.
    std::vector<pl::modmenu::HudEditorElement> elements;
    {
        pl::modmenu::HudEditorElement element;
        element.elementId = GridElementId;
        element.displayName = "Inventory Grid";
        element.positionKeyX = "hudPosX";
        element.positionKeyY = "hudPosY";
        element.x = config.grid.x;
        element.y = config.grid.y;
        element.width = std::max(1.0f, layout::gridWidth(config.grid));
        element.height = std::max(1.0f, layout::gridHeight(config.grid));
        element.gridSize = config.gridSize;
        element.snapThreshold = config.snapThreshold;
        element.gridGap = config.gridGap;
        element.snapFlags = config.snapFlags;
        elements.push_back(std::move(element));
    }
    pl::modmenu::submitHudEditorElements(moduleId, elements);

    std::vector<pl::modmenu::DrawCommand> commands;
    const bool hidden = config.hideInContainer && hiddenByScreen();
    if (!hidden && (config.stackCount || config.durability)) {
        auto decorate = [&](const SlotRuntime& runtime, const SlotRect& rect) {
            if (!runtime.hasItem.load(std::memory_order_acquire) || rect.size <= 0.0f) return;

            const int maxDamage = runtime.maxDamage.load(std::memory_order_acquire);
            const int damage = runtime.damage.load(std::memory_order_acquire);
            if (config.durability && maxDamage > 0 && damage > 0) {
                const float ratio = decor::durabilityRatio(damage, maxDamage);
                const decor::DurabilityBar bar = decor::durabilityBar(rect, ratio);

                pl::modmenu::DrawCommand background;
                background.type = pl::modmenu::DrawCommandType::RectFilled;
                background.x = bar.x;
                background.y = bar.y;
                background.w = bar.width;
                background.h = bar.height;
                background.color = 0xFF000000u;
                commands.push_back(std::move(background));

                if (bar.fillWidth > 0.0f) {
                    pl::modmenu::DrawCommand fill;
                    fill.type = pl::modmenu::DrawCommandType::RectFilled;
                    fill.x = bar.x;
                    fill.y = bar.y;
                    fill.w = bar.fillWidth;
                    fill.h = bar.fillHeight;
                    fill.color = decor::durabilityColor(ratio);
                    commands.push_back(std::move(fill));
                }
            }

            const std::uint8_t count = runtime.count.load(std::memory_order_acquire);
            if (config.stackCount && count > 1) {
                const decor::TextAnchor anchor = decor::countTextAnchor(rect);
                pl::modmenu::DrawCommand text;
                text.type = pl::modmenu::DrawCommandType::Text;
                text.x = anchor.x;
                text.y = anchor.y;
                text.w = -1.0f; // right-aligned at x
                text.color = config.countColor;
                text.size = config.countTextSize;
                text.text = std::to_string(static_cast<unsigned>(count));
                commands.push_back(std::move(text));
            }
        };

        for (std::size_t i = 0; i < GridSlotCount; ++i) {
            decorate(m_grid[i], layout::gridSlotRect(config.grid, i));
        }
    }
    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void InventoryHudModule::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("grid")
        .category("grid", "Grid", "Shape and size of the inventory grid")
        .category("details", "Details", "Extra information drawn on each slot")
        .category("visibility", "Visibility", "When the inventory grid is shown")
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
    auto slider = [&](const char* key, std::string title, const char* category,
                      const char* sectionId, const char* min, const char* max,
                      const char* unit = " px", const char* enabledKey = nullptr) {
        auto value = node(key, std::move(title), category, ConfigControlTypeV2::SliderFloat);
        value.section = sectionId;
        value.minValue = min;
        value.maxValue = max;
        value.step = "1";
        value.unit = unit;
        if (enabledKey) value.visibleWhen = {{enabledKey, ConfigConditionOpV2::Truthy, {}}};
        schema.node(std::move(value));
    };

    section("grid_shape", "Layout", "grid");
    {
        auto columns = node("m_columns", "Columns", "grid", ConfigControlTypeV2::SliderInt);
        columns.section = "grid_shape";
        columns.description = "9 columns match the inventory screen; fewer columns wrap the 27 slots into more rows.";
        columns.minValue = std::to_string(layout::MinColumns);
        columns.maxValue = std::to_string(layout::MaxColumns);
        columns.step = "1";
        schema.node(std::move(columns));
    }
    slider("m_slotSize", "Slot Size", "grid", "grid_shape", "8", "100");
    slider("m_slotGap", "Gap Between Slots", "grid", "grid_shape", "0", "50");
    section("activation", "Shortcut", "grid");
    {
        auto toggleKey = node("keybind", "Toggle Keybind", "grid", ConfigControlTypeV2::Keybind);
        toggleKey.section = "activation";
        schema.node(std::move(toggleKey));
    }

    section("slot_details", "Slot Details", "details");
    {
        auto features = node("slot_features", "Show", "details", ConfigControlTypeV2::ToggleGroup);
        features.key.clear();
        features.section = "slot_details";
        features.description = "Armor and offhand now live in the separate Armor module.";
        features.choiceStyle = ConfigChoiceStyleV2::Checklist;
        features.options = {
            {"count", "Stack Count", {}, "m_showStackCount"},
            {"durability", "Durability Bar", {}, "m_showDurability"}
        };
        schema.node(std::move(features));
    }
    section("count_text", "Number Text", "details");
    slider("m_countTextSize", "Text Size", "details", "count_text", "6", "40");
    {
        auto color = node("m_countColor", "Text Color", "details", ConfigControlTypeV2::Color);
        color.section = "count_text";
        color.defaultValue = "#FFFFFF";
        color.description = "Used for stack counts.";
        schema.node(std::move(color));
    }
    section("slot_background", "Slot Background", "details");
    {
        auto toggle = node("m_slotBackground", "Slot Background", "details", ConfigControlTypeV2::Toggle);
        toggle.section = "slot_background";
        toggle.description = "Draws a cell behind every slot of the grid, including empty ones, so the grid reads like the inventory screen.";
        schema.node(std::move(toggle));

        auto opacity = node("m_slotBgOpacity", "Background Opacity", "details", ConfigControlTypeV2::SliderFloat);
        opacity.section = "slot_background";
        opacity.minValue = "0.05";
        opacity.maxValue = "1";
        opacity.step = "0.05";
        opacity.visibleWhen = {{"m_slotBackground", ConfigConditionOpV2::Truthy, {}}};
        schema.node(std::move(opacity));

        auto color = node("m_slotBgColor", "Background Color", "details", ConfigControlTypeV2::Color);
        color.section = "slot_background";
        color.defaultValue = "#000000";
        color.visibleWhen = {{"m_slotBackground", ConfigConditionOpV2::Truthy, {}}};
        color.description = "Color of the cells behind the slots; the opacity slider above sets how strongly they show.";
        schema.node(std::move(color));
    }

    section("auto_hide", "Automatic Hiding", "visibility");
    {
        auto hide = node("m_hideInContainer", "Hide While Inventory Is Open", "visibility", ConfigControlTypeV2::Toggle);
        hide.section = "auto_hide";
        hide.description = "Hides the grid while the inventory, a chest or any other container screen is open.";
        schema.node(std::move(hide));
    }

    auto help = node("editor_help", "Armor Moved To Its Own Module", "editor", ConfigControlTypeV2::Info);
    help.key.clear();
    help.description = "This module owns a single HUD Editor element: Inventory Grid. Your armor and offhand are drawn by the separate Armor module, which has its own toggle, element and settings.";
    schema.node(std::move(help));
    section("snapping", "Snapping", "editor");
    {
        auto snapping = node("snap_targets", "Snap To", "editor", ConfigControlTypeV2::ToggleGroup);
        snapping.key.clear();
        snapping.section = "snapping";
        snapping.choiceStyle = ConfigChoiceStyleV2::Chips;
        snapping.options = {
            {"grid", "Grid", {}, "m_snapToGrid"},
            {"items", "Other Elements", {}, "m_snapToElements"},
            {"center", "Screen Center", {}, "m_snapToScreenCenter"}
        };
        schema.node(std::move(snapping));
    }
    slider("m_gridSize", "Grid Size", "editor", "snapping", "1", "100", " px", "m_snapToGrid");
    slider("m_gridGap", "Gap Between Elements", "editor", "snapping", "0", "100", " px", "m_snapToElements");
    slider("m_snapThreshold", "Snap Distance", "editor", "snapping", "1", "100");

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void InventoryHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_configMutex);

    if (j.contains("hudPosX")) hudPosX = std::clamp(j["hudPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudPosY")) hudPosY = std::clamp(j["hudPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_columns")) {
        m_columns = std::clamp(j["m_columns"].get<int>(),
                               static_cast<int>(layout::MinColumns), static_cast<int>(layout::MaxColumns));
    }
    if (j.contains("m_slotSize")) m_slotSize = std::clamp(j["m_slotSize"].get<float>(), 8.0f, 100.0f);
    if (j.contains("m_slotGap")) m_slotGap = std::clamp(j["m_slotGap"].get<float>(), 0.0f, 50.0f);
    if (j.contains("m_showStackCount")) m_showStackCount = j["m_showStackCount"].get<bool>();
    if (j.contains("m_showDurability")) m_showDurability = j["m_showDurability"].get<bool>();
    if (j.contains("m_hideInContainer")) m_hideInContainer = j["m_hideInContainer"].get<bool>();
    if (j.contains("m_slotBackground")) m_slotBackground = j["m_slotBackground"].get<bool>();
    if (j.contains("m_slotBgOpacity")) m_slotBgOpacity = std::clamp(j["m_slotBgOpacity"].get<float>(), 0.05f, 1.0f);
    if (j.contains("m_slotBgColor")) m_slotBgColor = j["m_slotBgColor"].get<std::string>();
    if (j.contains("m_countTextSize")) m_countTextSize = std::clamp(j["m_countTextSize"].get<float>(), 6.0f, 40.0f);
    if (j.contains("m_countColor")) m_countColor = j["m_countColor"].get<std::string>();
    if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
    if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
    if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
}

void InventoryHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);

    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["m_columns"] = m_columns;
    j["m_slotSize"] = m_slotSize;
    j["m_slotGap"] = m_slotGap;
    j["m_showStackCount"] = m_showStackCount;
    j["m_showDurability"] = m_showDurability;
    j["m_hideInContainer"] = m_hideInContainer;
    j["m_slotBackground"] = m_slotBackground;
    j["m_slotBgOpacity"] = m_slotBgOpacity;
    j["m_slotBgColor"] = m_slotBgColor;
    j["m_countTextSize"] = m_countTextSize;
    j["m_countColor"] = m_countColor;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}
