#pragma once

// Shared plumbing for the HUD modules that paint inventory items with the
// game's own ItemRenderer (ArmorHUD, Hotbar Slots, Inventory HUD).
//
// Those modules only differ in *which* stacks they show and *where*. Everything
// else lives here so it is written (and fixed for a new game version) once:
//
//   * the HudCameraRenderer::render hook that gives them a render pass,
//   * walking Player -> Inventory::PlayerInventory -> proxy ->
//     PlayerInventoryContainer -> FillingContainer::mItems in ItemStackSize
//     steps, plus the armor container and the equipment hand container
//     (offhand = slot 1, with the Actor::getOffhandSlot accessor as fallback),
//   * constructing a BaseActorRenderContext to reach the ItemRenderer,
//   * mapping launcher HUD units onto the MinecraftUIRenderContext,
//   * ItemRenderer::renderGuiItemNew for one icon and the final flushImages.
//
// The header stays free of preloader / entt includes; the heavy lifting is in
// huditems.cpp.

#include <bedrocktools/sdk/Offsets.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace bedrocktools::huditems {

// Resolves the ItemRenderer / ItemStackBase / BaseActorRenderContext
// functions from the signature table. Idempotent; call it from onInit().
void initialize();

// One HudCameraRenderer::render hook shared by every item HUD module. The
// listener runs on the render thread right after the vanilla HUD was drawn.
using RenderListener = void (*)(void* context, void* client, void* user);
bool addRenderListener(RenderListener listener, void* user);
void removeRenderListener(RenderListener listener, void* user);

// ---- Player / container access --------------------------------------------

void* getLocalPlayer(void* client);
void* getCarriedItem(void* player);

// The contiguous ItemStack array of a FillingContainer.
struct ContainerSlots {
    std::uintptr_t begin = 0; // first ItemStack, 0 when the container is unusable
    std::size_t count = 0;    // number of ItemStackSize-strided slots

    // ItemStack of a slot, nullptr when the index is out of range.
    void* stack(std::size_t index) const {
        if (!begin || index >= count) return nullptr;
        return reinterpret_cast<void*>(begin + index * sdk::offsets::Inventory::ItemStackSize);
    }
    explicit operator bool() const { return begin != 0 && count != 0; }
};

ContainerSlots containerSlots(void* container);

// The player's own inventory: slots 0-8 are the hotbar, 9-35 the 9x3 grid of
// the inventory screen.
ContainerSlots playerInventory(void* player);

struct EquipmentStacks {
    void* armor[4] = {}; // helmet, chestplate, leggings, boots
    void* offhand = nullptr;
    void* mainhand = nullptr;
};
EquipmentStacks getEquipmentStacks(void* player);

// ---- ItemStack inspection ---------------------------------------------------

void* stackItem(void* stack);         // Item*, nullptr for an empty slot
std::uint8_t stackCount(void* stack); // ItemStackBase::mCount
int stackDamage(void* stack);         // ItemStackBase::getDamageValue, >= 0
int itemMaxDamage(void* item);        // Item::getMaxDamage, 0 when unbreakable
// Items whose icon needs the HUD-opacity pass (dyed leather etc., see
// IconPainter::beginOpacityFixPass).
bool needsTextureOpacityPass(void* stack);

// ---- Drawing ----------------------------------------------------------------

// Launcher HUD units (pl::modmenu::getHudSurfaceSize) -> UI render context
// coordinates (MinecraftUIRenderContext::getFullClippingRectangle).
struct HudMapping {
    float originX = 0.0f;
    float originY = 0.0f;
    float scaleX = 0.0f;
    float scaleY = 0.0f;
    bool valid = false;

    float x(float hudX) const { return originX + hudX * scaleX; }
    float y(float hudY) const { return originY + hudY * scaleY; }
};
HudMapping hudMapping(void* context);

// Scope that owns a BaseActorRenderContext for one HUD render pass. Icons are
// batched into the UI image mesh and flushed when the painter goes away.
class IconPainter {
public:
    // `wantRender == false` skips all render setup (the module still gets the
    // player for bookkeeping); ready() is then false.
    IconPainter(void* context, void* client, bool wantRender = true);
    ~IconPainter();

    IconPainter(const IconPainter&) = delete;
    IconPainter& operator=(const IconPainter&) = delete;

    bool ready() const { return mItemRenderer != nullptr; }
    void* player() const { return mPlayer; }
    const HudMapping& mapping() const { return mMapping; }

    // Paints the icon of `stack` into the HUD-space square (hudX, hudY, hudSize).
    bool draw(void* stack, void* item, float hudX, float hudY, float hudSize);

    // Fills a HUD-space rectangle (hudX, hudY, hudW, hudH) with an ARGB color
    // through the same UI context the icons use. Call it before draw() for a
    // slot: fills are submitted first, so the icons of the same pass always
    // land on top of them (slot backgrounds).
    bool fillRect(float hudX, float hudY, float hudW, float hudH, std::uint32_t color);

    // Dyed leather armor loses its tinted pixels when the HUD opacity shader
    // constant is at its default; the fix is an extra pass at a high opacity
    // with the renderer's "20" mode for just those stacks. Only stacks for
    // which needsTextureOpacityPass() is true should be drawn in the pass.
    bool supportsOpacityFix() const;
    void beginOpacityFixPass();
    bool drawOpacityFix(void* stack, void* item, float hudX, float hudY, float hudSize);
    void endOpacityFixPass();

private:
    bool paint(void* stack, void* item, float hudX, float hudY, float hudSize, float mode);

    void* mContext = nullptr;
    void* mClient = nullptr;
    void* mPlayer = nullptr;
    HudMapping mMapping{};
    alignas(16) std::byte mStorage[sdk::offsets::ShulkerPreview::BaseActorRenderContextStorageSize]{};
    void* mItemRenderer = nullptr;
    bool mConstructed = false;
    bool mDrewAny = false;
    bool mDrewFix = false;
    bool mInFixPass = false;
};

// ---- Colors -----------------------------------------------------------------

// "#RRGGBB" / "#AARRGGBB" -> ARGB, `fallback` when unparsable.
std::uint32_t parseColor(const std::string& value, std::uint32_t fallback = 0xFFFFFFFFu);
std::uint32_t withOpacity(std::uint32_t color, float opacity);

} // namespace bedrocktools::huditems
