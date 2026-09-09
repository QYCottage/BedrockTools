#include "huditems.hpp"

#include "core/memory/Hooks.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Vtable.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string_view>

// entt resolves components by their (pretty-function derived) type name, so
// this must be spelled exactly like the game's component and live at global
// scope; wrapping it in a namespace would silently make tryGetComponent fail.
struct ActorEquipmentComponent {
    void* hand;
    void* armorContainer;
};
static_assert(sizeof(ActorEquipmentComponent) == 0x10);

namespace bedrocktools::huditems {
namespace {

namespace offsets = bedrocktools::sdk::offsets;

constexpr std::size_t MaxContainerSlots = 64;
constexpr float VanillaItemSize = 16.0f;
constexpr const char* MinecraftLibrary = "libminecraftpe.so";

// ItemRenderer::renderGuiItemNew "mode" argument: 1 is the regular icon,
// 20 is the pass used by the dyed-leather opacity fix (see IconPainter).
constexpr float RegularItemMode = 1.0f;
constexpr float OpacityFixItemMode = 20.0f;
constexpr float OpacityFixHudOpacity = 90.0f;

// ScreenContext -> shader constant buffers -> HUD_OPACITY constant.
constexpr std::size_t ScreenContextShaderConstants = 0x20;
constexpr std::size_t ShaderConstantsHudOpacity = 0x150;
constexpr std::size_t ShaderConstantData = 0x30;
constexpr std::size_t ShaderConstantDirty = 0x29;

struct RectangleArea {
    float x0;
    float x1;
    float y0;
    float y1;
};

struct Color {
    float r;
    float g;
    float b;
    float a;
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

using ActorGetOffhandSlotFn = const void* (*)(const void*);
using HudCameraRendererFn = void (*)(void*, void*, void*, void*, int);
using BaseActorRenderContextCtorFn = void (*)(void*, void*, void*, void*);
using ItemStackBaseGetDamageValueFn = int (*)(void*);
using ItemStackBaseGetRawNameIdFn = std::string (*)(void*);
using ItemRendererRenderGuiItemNewFn = std::uint64_t (*)(
    void*, void*, void*, unsigned int, unsigned char, std::uint64_t,
    float, float, float, float, float);
using MinecraftUIRenderContextFillRectangleFn = void (*)(
    void*, const RectangleArea&, const Color&, float);

ActorGetOffhandSlotFn actorGetOffhandSlot = nullptr;
BaseActorRenderContextCtorFn baseActorRenderContextCtor = nullptr;
ItemStackBaseGetDamageValueFn itemStackBaseGetDamageValue = nullptr;
ItemStackBaseGetRawNameIdFn itemStackBaseGetRawNameId = nullptr;
ItemRendererRenderGuiItemNewFn itemRendererRenderGuiItemNew = nullptr;
bool functionsResolved = false;

struct ListenerEntry {
    RenderListener listener = nullptr;
    void* user = nullptr;
};

// Fixed capacity so the render thread never allocates while copying the list.
constexpr std::size_t MaxListeners = 8;
std::mutex listenerMutex;
std::array<ListenerEntry, MaxListeners> listeners{};
std::size_t listenerCount = 0;
HudCameraRendererFn hudCameraRendererOriginal = nullptr;
bedrocktools::hooks::Handle hudRendererHook = nullptr;

void** getVtable(void* object) {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

template <class T>
T read(const void* object, std::size_t offset) {
    return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object) + offset);
}

void hudCameraRendererDetour(void* self, void* context, void* client, void* value, int pass) {
    if (hudCameraRendererOriginal) hudCameraRendererOriginal(self, context, client, value, pass);
    std::array<ListenerEntry, MaxListeners> snapshot{};
    std::size_t count = 0;
    {
        std::lock_guard lock(listenerMutex);
        snapshot = listeners;
        count = listenerCount;
    }
    for (std::size_t i = 0; i < count; ++i) snapshot[i].listener(context, client, snapshot[i].user);
}

void installHook() {
    if (hudRendererHook) return;
    const std::uintptr_t hudRenderer = pl::memory::resolveVtableFunction(
        "17HudCameraRenderer",
        offsets::VTable::HudCameraRendererRender,
        MinecraftLibrary);
    if (!hudRenderer) return;
    hudRendererHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(hudRenderer),
        reinterpret_cast<void*>(hudCameraRendererDetour),
        reinterpret_cast<void**>(&hudCameraRendererOriginal));
}

void* getMinecraftGame(void* client) {
    if (!client) return nullptr;
    void** vtable = getVtable(client);
    if (vtable && vtable[offsets::VTable::ClientInstanceGetMinecraftGame]) {
        void* game = reinterpret_cast<void* (*)(void*)>(
            vtable[offsets::VTable::ClientInstanceGetMinecraftGame])(client);
        if (game) return game;
    }
    return read<void*>(client, offsets::ShulkerPreview::ClientInstanceMinecraftGame);
}

unsigned int getItemAnimationFrame(void* item, void* localPlayer, void* stack) {
    if (!item || !localPlayer || !stack) return 0;
    void** vtable = getVtable(item);
    if (!vtable || !vtable[offsets::VTable::ItemGetAnimationFrameFor]) return 0;
    using Fn = unsigned int (*)(void*, void*, int, void*, int);
    return reinterpret_cast<Fn>(vtable[offsets::VTable::ItemGetAnimationFrameFor])(item, localPlayer, 0, stack, 1);
}

RectangleArea getFullClippingRectangle(void* context) {
    RectangleArea result{};
    void** vtable = getVtable(context);
    if (!vtable || !vtable[offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle]) return result;
    using Fn = RectangleArea (*)(void*);
    return reinterpret_cast<Fn>(vtable[offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle])(context);
}

bool validRectangle(const RectangleArea& area) {
    return std::isfinite(area.x0) && std::isfinite(area.x1) &&
           std::isfinite(area.y0) && std::isfinite(area.y1) &&
           area.x1 > area.x0 && area.y1 > area.y0;
}

void flushImages(void* context) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    using Fn = void (*)(void*, const Color&, float, const HashedString&);
    static const HashedString material("ui_flush");
    static constexpr Color color{1.0f, 1.0f, 1.0f, 1.0f};
    reinterpret_cast<Fn>(vtable[offsets::VTable::MinecraftUIRenderContextFlushImages])(context, color, 1.0f, material);
}

void setHudOpacity(void* context, float opacity) {
    if (!context) return;
    void* screenContext = read<void*>(context, offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
    if (!screenContext) return;
    auto* constantBuffers = read<std::byte*>(screenContext, ScreenContextShaderConstants);
    if (!constantBuffers) return;
    auto* shaderConstantBuffer = read<std::byte*>(constantBuffers, ShaderConstantsHudOpacity);
    if (!shaderConstantBuffer) return;
    auto* opacityPtr = read<float*>(shaderConstantBuffer, ShaderConstantData);
    if (!opacityPtr) return;
    if (*opacityPtr != opacity) {
        *opacityPtr = opacity;
        *reinterpret_cast<std::uint8_t*>(shaderConstantBuffer + ShaderConstantDirty) = 1;
    }
}

void destroyBaseActorRenderContext(void* context) {
    void** vtable = getVtable(context);
    if (vtable && vtable[0]) reinterpret_cast<void (*)(void*)>(vtable[0])(context);
}

ActorEquipmentComponent* getEquipment(void* player) {
    if (!player) return nullptr;
    auto* context = reinterpret_cast<EntityContext*>(
        reinterpret_cast<std::uintptr_t>(player) + offsets::Actor::mEntityContext);
    return context->tryGetComponent<ActorEquipmentComponent>();
}

} // namespace

void initialize() {
    if (!actorGetOffhandSlot) {
        actorGetOffhandSlot = reinterpret_cast<ActorGetOffhandSlotFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetOffhandSlot));
    }
    if (!functionsResolved) {
        baseActorRenderContextCtor = reinterpret_cast<BaseActorRenderContextCtorFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseActorRenderContextCtor));
        itemStackBaseGetDamageValue = reinterpret_cast<ItemStackBaseGetDamageValueFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetDamageValue));
        itemStackBaseGetRawNameId = reinterpret_cast<ItemStackBaseGetRawNameIdFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetRawNameId));
        itemRendererRenderGuiItemNew = reinterpret_cast<ItemRendererRenderGuiItemNewFn>(
            bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemRendererRenderGuiItemNew));
        functionsResolved = baseActorRenderContextCtor && itemRendererRenderGuiItemNew;
    }
    installHook();
}

bool addRenderListener(RenderListener listener, void* user) {
    if (!listener) return false;
    installHook();
    std::lock_guard lock(listenerMutex);
    for (std::size_t i = 0; i < listenerCount; ++i) {
        if (listeners[i].listener == listener && listeners[i].user == user) return hudRendererHook != nullptr;
    }
    if (listenerCount >= MaxListeners) return false;
    listeners[listenerCount++] = {listener, user};
    return hudRendererHook != nullptr;
}

void removeRenderListener(RenderListener listener, void* user) {
    std::lock_guard lock(listenerMutex);
    for (std::size_t i = 0; i < listenerCount; ++i) {
        if (listeners[i].listener != listener || listeners[i].user != user) continue;
        for (std::size_t j = i + 1; j < listenerCount; ++j) listeners[j - 1] = listeners[j];
        listeners[--listenerCount] = {};
        return;
    }
}

void* getLocalPlayer(void* client) {
    void** vtable = getVtable(client);
    if (!vtable || !vtable[offsets::VTable::ClientInstanceGetLocalPlayer]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[offsets::VTable::ClientInstanceGetLocalPlayer])(client);
}

void* getCarriedItem(void* player) {
    void** vtable = getVtable(player);
    if (!vtable || !vtable[offsets::VTable::PlayerGetCarriedItem]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[offsets::VTable::PlayerGetCarriedItem])(player);
}

ContainerSlots containerSlots(void* container) {
    ContainerSlots slots;
    if (!container) return slots;
    const auto begin = read<std::uintptr_t>(container, offsets::Inventory::FillingContainerItems);
    const auto end = read<std::uintptr_t>(container, offsets::Inventory::FillingContainerItems + sizeof(void*));
    if (!begin || end < begin || begin % alignof(void*) != 0) return slots;
    const auto span = end - begin;
    if (span % offsets::Inventory::ItemStackSize != 0) return slots;
    const auto count = span / offsets::Inventory::ItemStackSize;
    if (count == 0 || count > MaxContainerSlots) return slots;
    slots.begin = begin;
    slots.count = static_cast<std::size_t>(count);
    return slots;
}

ContainerSlots playerInventory(void* player) {
    if (!player) return {};
    void* proxy = read<void*>(player, offsets::Inventory::PlayerInventory);
    if (!proxy) return {};
    void* container = read<void*>(proxy, offsets::Inventory::PlayerInventoryContainer);
    return containerSlots(container);
}

EquipmentStacks getEquipmentStacks(void* player) {
    EquipmentStacks stacks;
    if (!player) return stacks;
    if (ActorEquipmentComponent* equipment = getEquipment(player)) {
        const ContainerSlots armor = containerSlots(equipment->armorContainer);
        if (armor.count >= 4) {
            for (std::size_t i = 0; i < 4; ++i) stacks.armor[i] = armor.stack(i);
        }
        // The hand container holds both hands: slot 0 is the main hand and
        // slot 1 the offhand. This is how the upstream ArmorHUD reads the
        // offhand, and it works without any signature resolution, so it is
        // tried first. The Actor accessor below stays as a fallback for game
        // versions whose hand container does not expose this layout.
        const ContainerSlots hands = containerSlots(equipment->hand);
        if (hands.count >= 2) stacks.offhand = hands.stack(1);
    }
    if (!stacks.offhand && actorGetOffhandSlot) {
        stacks.offhand = const_cast<void*>(actorGetOffhandSlot(player));
    }
    stacks.mainhand = getCarriedItem(player);
    return stacks;
}

void* stackItem(void* stack) {
    if (!stack) return nullptr;
    void* counter = read<void*>(stack, offsets::ShulkerPreview::ItemStackBaseItem);
    if (!counter) return nullptr;
    return read<void*>(counter, offsets::ShulkerPreview::SharedCounterPointer);
}

std::uint8_t stackCount(void* stack) {
    if (!stack) return 0;
    return read<std::uint8_t>(stack, offsets::Inventory::ItemStackCount);
}

int stackDamage(void* stack) {
    if (!stack || !itemStackBaseGetDamageValue) return 0;
    return std::max(0, itemStackBaseGetDamageValue(stack));
}

int itemMaxDamage(void* item) {
    void** vtable = getVtable(item);
    if (!vtable || !vtable[offsets::VTable::ItemGetMaxDamage]) return 0;
    return std::max(0, static_cast<int>(reinterpret_cast<short (*)(void*)>(vtable[offsets::VTable::ItemGetMaxDamage])(item)));
}

bool needsTextureOpacityPass(void* stack) {
    if (!stack || !itemStackBaseGetRawNameId) return false;
    const std::string name = itemStackBaseGetRawNameId(stack);
    return name == "leather_helmet" ||
           name == "leather_chestplate" ||
           name == "leather_leggings" ||
           name == "leather_boots" ||
           name == "firework_star" ||
           name == "leather_horse_armor";
}

HudMapping hudMapping(void* context) {
    HudMapping mapping;
    if (!context) return mapping;
    const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
    const RectangleArea full = getFullClippingRectangle(context);
    if (surface.width <= 0.0f || surface.height <= 0.0f || !validRectangle(full)) return mapping;
    mapping.originX = full.x0;
    mapping.originY = full.y0;
    mapping.scaleX = (full.x1 - full.x0) / surface.width;
    mapping.scaleY = (full.y1 - full.y0) / surface.height;
    mapping.valid = std::isfinite(mapping.scaleX) && std::isfinite(mapping.scaleY) &&
                    mapping.scaleX > 0.0f && mapping.scaleY > 0.0f;
    return mapping;
}

IconPainter::IconPainter(void* context, void* client, bool wantRender)
    : mContext(context), mClient(client) {
    if (!context || !client) return;
    mPlayer = getLocalPlayer(client);
    if (!wantRender || !mPlayer || !baseActorRenderContextCtor || !itemRendererRenderGuiItemNew) return;
    mMapping = hudMapping(context);
    if (!mMapping.valid) return;
    void* screenContext = read<void*>(context, offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
    void* game = getMinecraftGame(client);
    if (!screenContext || !game) return;
    baseActorRenderContextCtor(mStorage, screenContext, client, game);
    mConstructed = true;
    mItemRenderer = *reinterpret_cast<void**>(mStorage + offsets::ShulkerPreview::BaseActorRenderContextItemRenderer);
}

IconPainter::~IconPainter() {
    if (mInFixPass) endOpacityFixPass();
    if (mConstructed) destroyBaseActorRenderContext(mStorage);
    if (mDrewAny) flushImages(mContext);
}

bool IconPainter::paint(void* stack, void* item, float hudX, float hudY, float hudSize, float mode) {
    if (!mItemRenderer || !stack || !item || hudSize <= 0.0f) return false;
    const float x = mMapping.x(hudX);
    const float y = mMapping.y(hudY);
    const float width = hudSize * mMapping.scaleX;
    const float height = hudSize * mMapping.scaleY;
    const float iconSize = std::max(1.0f, std::min(width, height));
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(iconSize)) return false;
    const unsigned int animationFrame = getItemAnimationFrame(item, mPlayer, stack);
    itemRendererRenderGuiItemNew(
        mItemRenderer,
        mStorage,
        stack,
        animationFrame,
        0,
        0,
        x,
        y,
        1.0f,
        mode,
        iconSize / VanillaItemSize);
    return true;
}

bool IconPainter::draw(void* stack, void* item, float hudX, float hudY, float hudSize) {
    if (!paint(stack, item, hudX, hudY, hudSize, RegularItemMode)) return false;
    mDrewAny = true;
    return true;
}

bool IconPainter::fillRect(float hudX, float hudY, float hudW, float hudH, std::uint32_t color) {
    if (mInFixPass || !mContext || !mMapping.valid || hudW <= 0.0f || hudH <= 0.0f) return false;
    void** vtable = getVtable(mContext);
    if (!vtable || !vtable[offsets::VTable::MinecraftUIRenderContextFillRectangle]) return false;

    // Fills go through the game's own MinecraftUIRenderContext, so the
    // rectangle must use the same UI coordinates the icons are painted in.
    const RectangleArea area{
        mMapping.x(hudX),
        mMapping.x(hudX + hudW),
        mMapping.y(hudY),
        mMapping.y(hudY + hudH)};
    // Like flushImages()/drawText(), fillRectangle() blends with a separate
    // opacity argument rather than Color::a: the caller packs the alpha into
    // the top byte of `color` (see withOpacity), so it must be decoded and
    // handed over as that trailing float. Leaving Color::a at 1.0 keeps the
    // RGB untouched, so only the trailing opacity drives the transparency.
    const float opacity = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
    const Color tint{
        static_cast<float>((color >> 16) & 0xFF) / 255.0f,
        static_cast<float>((color >> 8) & 0xFF) / 255.0f,
        static_cast<float>(color & 0xFF) / 255.0f,
        1.0f};
    reinterpret_cast<MinecraftUIRenderContextFillRectangleFn>(
        vtable[offsets::VTable::MinecraftUIRenderContextFillRectangle])(
        mContext, area, tint, opacity);
    // Like the icons, a queued fill only becomes visible when the image mesh
    // is flushed, so it counts as work for the destructor's flush.
    mDrewAny = true;
    return true;
}

bool IconPainter::supportsOpacityFix() const {
    return ready() && itemStackBaseGetRawNameId != nullptr;
}

void IconPainter::beginOpacityFixPass() {
    if (!supportsOpacityFix() || mInFixPass) return;
    setHudOpacity(mContext, OpacityFixHudOpacity);
    mInFixPass = true;
    mDrewFix = false;
}

bool IconPainter::drawOpacityFix(void* stack, void* item, float hudX, float hudY, float hudSize) {
    if (!mInFixPass) return false;
    if (!paint(stack, item, hudX, hudY, hudSize, OpacityFixItemMode)) return false;
    mDrewFix = true;
    return true;
}

void IconPainter::endOpacityFixPass() {
    if (!mInFixPass) return;
    if (mDrewFix) flushImages(mContext);
    setHudOpacity(mContext, 1.0f);
    mInFixPass = false;
    mDrewFix = false;
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

std::uint32_t withOpacity(std::uint32_t color, float opacity) {
    const auto alpha = static_cast<std::uint32_t>(std::clamp(opacity, 0.0f, 1.0f) * 255.0f);
    return (alpha << 24) | (color & 0x00FFFFFFu);
}

} // namespace bedrocktools::huditems
