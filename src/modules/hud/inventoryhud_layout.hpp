#pragma once

// Pure layout math for the Inventory HUD module.
//
// The module paints the 27 slots of the player's main inventory (container
// slots 9-35, the 9x3 grid of the inventory screen) as item icons on the HUD.
// Armor and the offhand are NOT part of this module any more: they live in the
// separate Armor module (armorhud_layout.hpp) with their own HUD element and
// settings. Keeping the math header-only and free of Minecraft types lets the
// host unit tests cover it.

#include "slotdecor_layout.hpp"

#include <algorithm>
#include <cstddef>

namespace bedrocktools::inventoryhud {

using slotdecor::SlotRect;

inline constexpr std::size_t HotbarSlotCount = 9;
inline constexpr std::size_t FirstGridSlot = HotbarSlotCount; // container slot 9
inline constexpr std::size_t GridSlotCount = 27;              // container slots 9-35
inline constexpr std::size_t LastGridSlot = FirstGridSlot + GridSlotCount - 1;
inline constexpr std::size_t MinColumns = 1;
inline constexpr std::size_t MaxColumns = GridSlotCount;
inline constexpr std::size_t DefaultColumns = 9;

// Anchor and shape of the inventory grid element.
struct GridLayout {
    float x = 0.0f;                   // anchor (top-left of the grid), HUD units
    float y = 0.0f;
    float slotSize = 32.0f;           // width and height of one slot
    float gap = 4.0f;                 // space between two slots
    std::size_t columns = DefaultColumns;
};

inline std::size_t clampColumns(std::size_t columns) {
    return std::clamp(columns, MinColumns, MaxColumns);
}

// Rows needed to show every grid slot with the configured column count.
inline std::size_t rowCount(const GridLayout& layout) {
    const std::size_t columns = clampColumns(layout.columns);
    return (GridSlotCount + columns - 1) / columns;
}

// Container slot index of a grid cell (0-26 -> 9-35).
inline std::size_t containerSlot(std::size_t gridIndex) {
    if (gridIndex >= GridSlotCount) gridIndex = GridSlotCount - 1;
    return FirstGridSlot + gridIndex;
}

// Position of one grid cell. Out-of-range indices are clamped so callers can
// never produce a rectangle outside the element.
inline SlotRect gridSlotRect(const GridLayout& layout, std::size_t gridIndex) {
    if (gridIndex >= GridSlotCount) gridIndex = GridSlotCount - 1;
    const std::size_t columns = clampColumns(layout.columns);
    const float step = layout.slotSize + layout.gap;
    SlotRect rect;
    rect.size = layout.slotSize;
    rect.x = layout.x + step * static_cast<float>(gridIndex % columns);
    rect.y = layout.y + step * static_cast<float>(gridIndex / columns);
    return rect;
}

inline float gridWidth(const GridLayout& layout) {
    const std::size_t columns = clampColumns(layout.columns);
    return layout.slotSize * static_cast<float>(columns) + layout.gap * static_cast<float>(columns - 1);
}

inline float gridHeight(const GridLayout& layout) {
    const std::size_t rows = rowCount(layout);
    return layout.slotSize * static_cast<float>(rows) + layout.gap * static_cast<float>(rows - 1);
}

// ---- Per-slot decorations ---------------------------------------------------
//
// Shared with the Armor module; re-exported here so existing callers and tests
// keep working.

using slotdecor::DurabilityBar;
using slotdecor::TextAnchor;
using slotdecor::countTextAnchor;
using slotdecor::durabilityBar;
using slotdecor::durabilityColor;
using slotdecor::durabilityRatio;
using slotdecor::durabilityText;
using slotdecor::remainingDurability;

} // namespace bedrocktools::inventoryhud
