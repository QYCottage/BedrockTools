#include "armorhud.hpp"

#include "core/memory/Hooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <pl/memory/Vtable.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct ActorEquipmentComponent {
    void* hand;
    void* armorContainer;
};

static_assert(sizeof(ActorEquipmentComponent) == 0x10);

namespace {

constexpr std::size_t SlotCount = 6;
constexpr std::size_t FillingContainerItemsOffset = bedrocktools::sdk::offsets::Inventory::FillingContainerItems;
constexpr std::size_t ItemStackSize = bedrocktools::sdk::offsets::Inventory::ItemStackSize;
constexpr std::size_t MaxContainerSlots = 64;
constexpr float VanillaItemSize = 16.0f;
constexpr const char* MinecraftLibrary = "libminecraftpe.so";

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

using HudCameraRendererFn = void (*)(void*, void*, void*, void*, int);
using BaseActorRenderContextCtorFn = void (*)(void*, void*, void*, void*);
using ItemStackBaseGetDamageValueFn = int (*)(void*);
using ItemStackBaseGetRawNameIdFn = std::string (*)(void*);
using ItemRendererRenderGuiItemNewFn = std::uint64_t (*)(
    void*, void*, void*, unsigned int, unsigned char, std::uint64_t,
    float, float, float, float, float);

HudCameraRendererFn hudCameraRendererOriginal = nullptr;
BaseActorRenderContextCtorFn baseActorRenderContextCtor = nullptr;
ItemStackBaseGetDamageValueFn itemStackBaseGetDamageValue = nullptr;
ItemStackBaseGetRawNameIdFn itemStackBaseGetRawNameId = nullptr;
ItemRendererRenderGuiItemNewFn itemRendererRenderGuiItemNew = nullptr;
ArmorHudModule* moduleInstance = nullptr;
bedrocktools::hooks::Handle hudRendererHook = nullptr;

constexpr std::array<const char*, SlotCount> HudElementIds{
    "bedrocktools.armorhud.helmet",
    "bedrocktools.armorhud.chestplate",
    "bedrocktools.armorhud.leggings",
    "bedrocktools.armorhud.boots",
    "bedrocktools.armorhud.offhand",
    "bedrocktools.armorhud.mainhand"
};

constexpr std::array<const char*, SlotCount> HudElementNames{
    "Helmet", "Chestplate", "Leggings", "Boots", "Offhand", "Main Hand"
};

constexpr std::array<const char*, SlotCount> HudXKeys{
    "hudHelmetPosX", "hudChestplatePosX", "hudLeggingsPosX", "hudBootsPosX", "hudOffhandPosX", "hudMainhandPosX"
};

constexpr std::array<const char*, SlotCount> HudYKeys{
    "hudHelmetPosY", "hudChestplatePosY", "hudLeggingsPosY", "hudBootsPosY", "hudOffhandPosY", "hudMainhandPosY"
};

void** getVtable(void* object) {
    return object ? *reinterpret_cast<void***>(object) : nullptr;
}

void* getLocalPlayer(void* client) {
    void** vtable = getVtable(client);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client);
}

void* getCarriedItem(void* player) {
    void** vtable = getVtable(player);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::PlayerGetCarriedItem])(player);
}

void* getMinecraftGame(void* client) {
    if (!client) return nullptr;
    void** vtable = getVtable(client);
    if (vtable && vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetMinecraftGame]) {
        void* game = reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetMinecraftGame])(client);
        if (game) return game;
    }
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(client) + bedrocktools::sdk::offsets::ShulkerPreview::ClientInstanceMinecraftGame);
}

void* getStackItem(void* stack) {
    if (!stack) return nullptr;
    void* counter = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(stack) + bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseItem);
    if (!counter) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(counter) + bedrocktools::sdk::offsets::ShulkerPreview::SharedCounterPointer);
}

short getMaxDamage(void* item) {
    void** vtable = getVtable(item);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage]) return 0;
    return reinterpret_cast<short (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage])(item);
}

unsigned int getItemAnimationFrame(void* item, void* localPlayer, void* stack) {
    if (!item || !localPlayer || !stack) return 0;
    void** vtable = getVtable(item);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor]) return 0;
    using Fn = unsigned int (*)(void*, void*, int, void*, int);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor])(item, localPlayer, 0, stack, 1);
}

RectangleArea getFullClippingRectangle(void* context) {
    RectangleArea result{};
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle]) return result;
    using Fn = RectangleArea (*)(void*);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle])(context);
}

void flushImages(void* context) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    using Fn = void (*)(void*, const Color&, float, const HashedString&);
    static const HashedString material("ui_flush");
    static constexpr Color color{1.0f, 1.0f, 1.0f, 1.0f};
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages])(context, color, 1.0f, material);
}

void setHudOpacity(void* context, float opacity) {
    if (!context) return;
    void* screenContext = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) + bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
    if (!screenContext) return;
    auto* constantBuffers = *reinterpret_cast<std::byte**>(reinterpret_cast<std::byte*>(screenContext) + 0x20);
    if (!constantBuffers) return;
    auto* shaderConstantBuffer = *reinterpret_cast<std::byte**>(constantBuffers + 0x150);
    if (!shaderConstantBuffer) return;
    auto* opacityPtr = *reinterpret_cast<float**>(shaderConstantBuffer + 0x30);
    if (!opacityPtr) return;
    if (*opacityPtr != opacity) {
        *opacityPtr = opacity;
        *reinterpret_cast<std::uint8_t*>(shaderConstantBuffer + 0x29) = 1;
    }
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

bool validRectangle(const RectangleArea& area) {
    return std::isfinite(area.x0) && std::isfinite(area.x1) && std::isfinite(area.y0) && std::isfinite(area.y1) &&
           area.x1 > area.x0 && area.y1 > area.y0;
}

void destroyBaseActorRenderContext(void* context) {
    void** vtable = getVtable(context);
    if (vtable && vtable[0]) reinterpret_cast<void (*)(void*)>(vtable[0])(context);
}

std::size_t containerSize(void* container) {
    if (!container) return 0;
    auto* bytes = reinterpret_cast<std::byte*>(container);
    const auto begin = *reinterpret_cast<std::uintptr_t*>(bytes + FillingContainerItemsOffset);
    const auto end = *reinterpret_cast<std::uintptr_t*>(bytes + FillingContainerItemsOffset + sizeof(void*));
    if (!begin || end < begin) return 0;
    const auto span = end - begin;
    if (span % ItemStackSize != 0) return 0;
    const auto count = span / ItemStackSize;
    return count <= MaxContainerSlots ? count : 0;
}

void* containerStack(void* container, std::size_t slot) {
    if (!container) return nullptr;
    auto* bytes = reinterpret_cast<std::byte*>(container);
    const auto begin = *reinterpret_cast<std::uintptr_t*>(bytes + FillingContainerItemsOffset);
    const auto end = *reinterpret_cast<std::uintptr_t*>(bytes + FillingContainerItemsOffset + sizeof(void*));
    if (!begin || end < begin) return nullptr;
    const auto span = end - begin;
    if (span % ItemStackSize != 0) return nullptr;
    const auto count = span / ItemStackSize;
    if (slot >= count || count > MaxContainerSlots) return nullptr;
    return reinterpret_cast<void*>(begin + slot * ItemStackSize);
}

int getStackDamage(void* stack) {
    if (!stack || !itemStackBaseGetDamageValue) return 0;
    return std::max(0, itemStackBaseGetDamageValue(stack));
}

ActorEquipmentComponent* getEquipment(void* player) {
    if (!player) return nullptr;
    auto* context = reinterpret_cast<EntityContext*>(
        reinterpret_cast<std::uintptr_t>(player) + bedrocktools::sdk::offsets::Actor::mEntityContext);
    return context->tryGetComponent<ActorEquipmentComponent>();
}

std::array<void*, SlotCount> getHudStacks(void* player) {
    std::array<void*, SlotCount> stacks{};
    ActorEquipmentComponent* equipment = getEquipment(player);
    if (!equipment) return stacks;
    void* armor = equipment->armorContainer;
    void* hand = equipment->hand;
    if (containerSize(armor) >= 4) {
        stacks[0] = containerStack(armor, 0);
        stacks[1] = containerStack(armor, 1);
        stacks[2] = containerStack(armor, 2);
        stacks[3] = containerStack(armor, 3);
    }
    if (containerSize(hand) >= 2) stacks[4] = containerStack(hand, 1);
    stacks[5] = getCarriedItem(player);
    return stacks;
}

std::uint32_t parseColor(const std::string& value) {
    if (value.empty()) return 0xFFFFFFFFu;
    const std::string hex = value[0] == '#' ? value.substr(1) : value;
    try {
        if (hex.size() == 6) return 0xFF000000u | static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
        if (hex.size() == 8) return static_cast<std::uint32_t>(std::stoul(hex, nullptr, 16));
    } catch (...) {
    }
    return 0xFFFFFFFFu;
}

void hudCameraRendererDetour(void* self, void* context, void* client, void* value, int pass) {
    if (hudCameraRendererOriginal) hudCameraRendererOriginal(self, context, client, value, pass);
    if (moduleInstance && moduleInstance->enabled) moduleInstance->renderNative(context, client);
}

}

ArmorHudModule::ArmorHudModule()
    : Module("ArmorHUD", "Displays armor, offhand, and main-hand items with configurable durability information.") {
    moduleInstance = this;
}

ArmorHudModule::~ArmorHudModule() {
    if (hudRendererHook) {
        bedrocktools::hooks::remove(hudRendererHook);
        hudRendererHook = nullptr;
        hudCameraRendererOriginal = nullptr;
    }
    if (moduleInstance == this) moduleInstance = nullptr;
}

void ArmorHudModule::onInit() {
    baseActorRenderContextCtor = reinterpret_cast<BaseActorRenderContextCtorFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseActorRenderContextCtor));
    itemStackBaseGetDamageValue = reinterpret_cast<ItemStackBaseGetDamageValueFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetDamageValue));
    itemStackBaseGetRawNameId = reinterpret_cast<ItemStackBaseGetRawNameIdFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetRawNameId));
    itemRendererRenderGuiItemNew = reinterpret_cast<ItemRendererRenderGuiItemNewFn>(
        bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemRendererRenderGuiItemNew));

    const std::uintptr_t hudRenderer = pl::memory::resolveVtableFunction(
        "17HudCameraRenderer",
        bedrocktools::sdk::offsets::VTable::HudCameraRendererRender,
        MinecraftLibrary);
    if (hudRenderer && !hudRendererHook) {
        hudRendererHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(hudRenderer),
            reinterpret_cast<void*>(hudCameraRendererDetour),
            reinterpret_cast<void**>(&hudCameraRendererOriginal));
    }
}

void ArmorHudModule::onDisable() {
    clearRuntime();
    pl::modmenu::submitDrawCommands(moduleId, std::span<const pl::modmenu::DrawCommand>{});
    pl::modmenu::submitHudEditorElements(moduleId, std::span<const pl::modmenu::HudEditorElement>{});
}

ArmorHudModule::ConfigSnapshot ArmorHudModule::snapshotConfig() const {
    std::lock_guard lock(m_configMutex);
    return {
        std::array<SlotConfig, SlotCount>{
            SlotConfig{m_helmet, m_helmetDurability, hudHelmetPosX, hudHelmetPosY, m_helmetSize},
            SlotConfig{m_chestplate, m_chestplateDurability, hudChestplatePosX, hudChestplatePosY, m_chestplateSize},
            SlotConfig{m_leggings, m_leggingsDurability, hudLeggingsPosX, hudLeggingsPosY, m_leggingsSize},
            SlotConfig{m_boots, m_bootsDurability, hudBootsPosX, hudBootsPosY, m_bootsSize},
            SlotConfig{m_offhand, m_offhandDurability, hudOffhandPosX, hudOffhandPosY, m_offhandSize},
            SlotConfig{m_mainhand, m_mainhandDurability, hudMainhandPosX, hudMainhandPosY, m_mainhandSize}
        },
        m_showDamage,
        m_showRemaining,
        m_showMaxDurability,
        m_showPercentage,
        m_durabilityTextSize,
        m_durabilityTextPosition,
        m_durabilityTextGap,
        parseColor(m_textColor),
        m_gridSize,
        m_gridGap,
        m_snapThreshold,
        (m_snapToGrid ? pl::modmenu::HudSnapGrid : pl::modmenu::HudSnapNone) |
            (m_snapToElements ? pl::modmenu::HudSnapElements : pl::modmenu::HudSnapNone) |
            (m_snapToScreenCenter ? pl::modmenu::HudSnapScreenCenter : pl::modmenu::HudSnapNone)
    };
}

void ArmorHudModule::clearRuntime() {
    for (auto& slot : m_runtime) {
        slot.hasItem.store(false, std::memory_order_release);
        slot.damage.store(0, std::memory_order_release);
        slot.maxDamage.store(0, std::memory_order_release);
    }
}

void ArmorHudModule::submitEditorElements(const ConfigSnapshot& config) {
    std::vector<pl::modmenu::HudEditorElement> elements;
    elements.reserve(SlotCount);
    for (std::size_t i = 0; i < SlotCount; ++i) {
        const SlotConfig& slot = config.slots[i];
        if (!slot.enabled) continue;
        pl::modmenu::HudEditorElement element;
        element.elementId = HudElementIds[i];
        element.displayName = HudElementNames[i];
        element.positionKeyX = HudXKeys[i];
        element.positionKeyY = HudYKeys[i];
        element.x = slot.x;
        element.y = slot.y;
        element.width = std::max(1.0f, slot.size);
        element.height = std::max(1.0f, slot.size);
        element.gridSize = config.gridSize;
        element.snapThreshold = config.snapThreshold;
        element.gridGap = config.gridGap;
        element.snapFlags = config.snapFlags;
        elements.push_back(std::move(element));
    }
    pl::modmenu::submitHudEditorElements(moduleId, elements);
}

void ArmorHudModule::renderNative(void* context, void* client) {
    if (!context || !client || !baseActorRenderContextCtor || !itemRendererRenderGuiItemNew) {
        clearRuntime();
        return;
    }

    void* localPlayer = getLocalPlayer(client);
    if (!localPlayer) {
        clearRuntime();
        return;
    }

    const ConfigSnapshot config = snapshotConfig();
    const auto stacks = getHudStacks(localPlayer);
    const pl::modmenu::HudSurfaceSize surface = pl::modmenu::getHudSurfaceSize();
    const RectangleArea full = getFullClippingRectangle(context);
    const bool canRender = surface.width > 0.0f && surface.height > 0.0f && validRectangle(full);

    alignas(16) std::byte baseActorRenderContext[bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextStorageSize]{};
    void* itemRenderer = nullptr;
    if (canRender) {
        void* screenContext = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) + bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
        void* game = getMinecraftGame(client);
        if (screenContext && game) {
            baseActorRenderContextCtor(baseActorRenderContext, screenContext, client, game);
            itemRenderer = *reinterpret_cast<void**>(baseActorRenderContext + bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextItemRenderer);
        }
    }

    const float uiWidth = full.x1 - full.x0;
    const float uiHeight = full.y1 - full.y0;
    bool renderedAny = false;

    if (itemRenderer && canRender && itemStackBaseGetRawNameId) {
        bool renderedTexturePass = false;
        setHudOpacity(context, 90.0f);
        for (std::size_t i = 0; i < SlotCount; ++i) {
            const SlotConfig& slot = config.slots[i];
            void* stack = stacks[i];
            void* item = getStackItem(stack);
            if (!slot.enabled || !item || slot.size <= 0.0f || !needsTextureOpacityPass(stack)) continue;

            const float x = full.x0 + slot.x * uiWidth / surface.width;
            const float y = full.y0 + slot.y * uiHeight / surface.height;
            const float width = slot.size * uiWidth / surface.width;
            const float height = slot.size * uiHeight / surface.height;
            const float iconSize = std::max(1.0f, std::min(width, height));
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(iconSize)) continue;

            const unsigned int animationFrame = getItemAnimationFrame(item, localPlayer, stack);
            itemRendererRenderGuiItemNew(
                itemRenderer,
                baseActorRenderContext,
                stack,
                animationFrame,
                0,
                0,
                x,
                y,
                1.0f,
                20.0f,
                iconSize / VanillaItemSize);
            renderedTexturePass = true;
        }
        if (renderedTexturePass) flushImages(context);
        setHudOpacity(context, 1.0f);
    }

    for (std::size_t i = 0; i < SlotCount; ++i) {
        void* stack = stacks[i];
        void* item = getStackItem(stack);
        const bool hasItem = item != nullptr;
        int damage = 0;
        int maxDamage = 0;
        if (hasItem) {
            damage = std::max(0, getStackDamage(stack));
            maxDamage = std::max(0, static_cast<int>(getMaxDamage(item)));
        }
        m_runtime[i].hasItem.store(hasItem, std::memory_order_release);
        m_runtime[i].damage.store(damage, std::memory_order_release);
        m_runtime[i].maxDamage.store(maxDamage, std::memory_order_release);

        const SlotConfig& slot = config.slots[i];
        if (!slot.enabled || !hasItem || !itemRenderer || !canRender || slot.size <= 0.0f) continue;

        const float x = full.x0 + slot.x * uiWidth / surface.width;
        const float y = full.y0 + slot.y * uiHeight / surface.height;
        const float width = slot.size * uiWidth / surface.width;
        const float height = slot.size * uiHeight / surface.height;
        const float iconSize = std::max(1.0f, std::min(width, height));
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(iconSize)) continue;

        const unsigned int animationFrame = getItemAnimationFrame(item, localPlayer, stack);
        itemRendererRenderGuiItemNew(
            itemRenderer,
            baseActorRenderContext,
            stack,
            animationFrame,
            0,
            0,
            x,
            y,
            1.0f,
            1.0f,
            iconSize / VanillaItemSize);
        renderedAny = true;
    }

    if (itemRenderer) destroyBaseActorRenderContext(baseActorRenderContext);
    if (renderedAny) flushImages(context);
}

void ArmorHudModule::onFrame() {
    if (!enabled) return;

    const ConfigSnapshot config = snapshotConfig();
    submitEditorElements(config);

    std::vector<pl::modmenu::DrawCommand> commands;
    commands.reserve(SlotCount);

    for (std::size_t i = 0; i < SlotCount; ++i) {
        const SlotConfig& slot = config.slots[i];
        if (!slot.enabled) continue;

        const bool hasItem = m_runtime[i].hasItem.load(std::memory_order_acquire);
        const int damage = m_runtime[i].damage.load(std::memory_order_acquire);
        const int maxDamage = m_runtime[i].maxDamage.load(std::memory_order_acquire);
        if (!slot.durability || !hasItem || maxDamage <= 0) continue;

        const int safeDamage = std::clamp(damage, 0, maxDamage);
        const int remaining = maxDamage - safeDamage;
        const int percentage = static_cast<int>(std::lround(remaining * 100.0 / maxDamage));
        std::string text;
        auto append = [&](const std::string& part) {
            if (!text.empty()) text += "  ";
            text += part;
        };
        if (config.showDamage) append(std::to_string(safeDamage) + " dmg");
        if (config.showRemaining && config.showMaxDurability) {
            append(std::to_string(remaining) + "/" + std::to_string(maxDamage));
        } else if (config.showRemaining) {
            append(std::to_string(remaining));
        } else if (config.showMaxDurability) {
            append(std::to_string(maxDamage) + " max");
        }
        if (config.showPercentage) append(std::to_string(percentage) + "%");
        if (text.empty()) continue;

        pl::modmenu::DrawCommand durability;
        durability.type = pl::modmenu::DrawCommandType::Text;
        const float verticalCenterBaseline = slot.y + slot.size * 0.5f + config.durabilityTextSize * 0.35f;
        if (config.durabilityTextPosition == 1) {
            durability.x = slot.x - config.durabilityTextGap;
            durability.y = verticalCenterBaseline;
            durability.w = -1.0f;
        } else if (config.durabilityTextPosition == 2) {
            durability.x = slot.x + slot.size * 0.5f;
            durability.y = slot.y + slot.size + config.durabilityTextGap + config.durabilityTextSize;
            durability.w = -2.0f;
        } else {
            durability.x = slot.x + slot.size + config.durabilityTextGap;
            durability.y = verticalCenterBaseline;
            durability.w = 0.0f;
        }
        durability.color = config.textColor;
        durability.size = config.durabilityTextSize;
        durability.text = std::move(text);
        commands.push_back(std::move(durability));
    }

    pl::modmenu::submitDrawCommands(moduleId, commands);
}

void ArmorHudModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    std::lock_guard lock(m_configMutex);

    if (j.contains("m_helmet")) m_helmet = j["m_helmet"].get<bool>();
    if (j.contains("m_helmetDurability")) m_helmetDurability = j["m_helmetDurability"].get<bool>();
    if (j.contains("hudHelmetPosX")) hudHelmetPosX = std::clamp(j["hudHelmetPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudHelmetPosY")) hudHelmetPosY = std::clamp(j["hudHelmetPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_helmetSize")) m_helmetSize = std::clamp(j["m_helmetSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_chestplate")) m_chestplate = j["m_chestplate"].get<bool>();
    if (j.contains("m_chestplateDurability")) m_chestplateDurability = j["m_chestplateDurability"].get<bool>();
    if (j.contains("hudChestplatePosX")) hudChestplatePosX = std::clamp(j["hudChestplatePosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudChestplatePosY")) hudChestplatePosY = std::clamp(j["hudChestplatePosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_chestplateSize")) m_chestplateSize = std::clamp(j["m_chestplateSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_leggings")) m_leggings = j["m_leggings"].get<bool>();
    if (j.contains("m_leggingsDurability")) m_leggingsDurability = j["m_leggingsDurability"].get<bool>();
    if (j.contains("hudLeggingsPosX")) hudLeggingsPosX = std::clamp(j["hudLeggingsPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudLeggingsPosY")) hudLeggingsPosY = std::clamp(j["hudLeggingsPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_leggingsSize")) m_leggingsSize = std::clamp(j["m_leggingsSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_boots")) m_boots = j["m_boots"].get<bool>();
    if (j.contains("m_bootsDurability")) m_bootsDurability = j["m_bootsDurability"].get<bool>();
    if (j.contains("hudBootsPosX")) hudBootsPosX = std::clamp(j["hudBootsPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudBootsPosY")) hudBootsPosY = std::clamp(j["hudBootsPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_bootsSize")) m_bootsSize = std::clamp(j["m_bootsSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_offhand")) m_offhand = j["m_offhand"].get<bool>();
    if (j.contains("m_offhandDurability")) m_offhandDurability = j["m_offhandDurability"].get<bool>();
    if (j.contains("hudOffhandPosX")) hudOffhandPosX = std::clamp(j["hudOffhandPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudOffhandPosY")) hudOffhandPosY = std::clamp(j["hudOffhandPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_offhandSize")) m_offhandSize = std::clamp(j["m_offhandSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_mainhand")) m_mainhand = j["m_mainhand"].get<bool>();
    if (j.contains("m_mainhandDurability")) m_mainhandDurability = j["m_mainhandDurability"].get<bool>();
    if (j.contains("hudMainhandPosX")) hudMainhandPosX = std::clamp(j["hudMainhandPosX"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("hudMainhandPosY")) hudMainhandPosY = std::clamp(j["hudMainhandPosY"].get<float>(), 0.0f, 4000.0f);
    if (j.contains("m_mainhandSize")) m_mainhandSize = std::clamp(j["m_mainhandSize"].get<float>(), 8.0f, 100.0f);

    if (j.contains("m_showDamage")) m_showDamage = j["m_showDamage"].get<bool>();
    if (j.contains("m_showRemaining")) m_showRemaining = j["m_showRemaining"].get<bool>();
    if (j.contains("m_showMaxDurability")) m_showMaxDurability = j["m_showMaxDurability"].get<bool>();
    if (j.contains("m_showPercentage")) m_showPercentage = j["m_showPercentage"].get<bool>();
    if (j.contains("m_durabilityTextSize")) m_durabilityTextSize = std::clamp(j["m_durabilityTextSize"].get<float>(), 6.0f, 100.0f);
    if (j.contains("m_durabilityTextPosition")) {
        try {
            std::string value = j["m_durabilityTextPosition"].get<std::string>();
            const std::size_t separator = value.find(',');
            if (separator != std::string::npos) value.resize(separator);
            m_durabilityTextPosition = std::clamp(std::stoi(value), 0, 2);
        } catch (...) {
        }
    }
    if (j.contains("m_durabilityTextGap")) m_durabilityTextGap = std::clamp(j["m_durabilityTextGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_textColor")) m_textColor = j["m_textColor"].get<std::string>();
    if (j.contains("m_gridSize")) m_gridSize = std::clamp(j["m_gridSize"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_gridGap")) m_gridGap = std::clamp(j["m_gridGap"].get<float>(), 0.0f, 100.0f);
    if (j.contains("m_snapThreshold")) m_snapThreshold = std::clamp(j["m_snapThreshold"].get<float>(), 1.0f, 100.0f);
    if (j.contains("m_snapToGrid")) m_snapToGrid = j["m_snapToGrid"].get<bool>();
    if (j.contains("m_snapToElements")) m_snapToElements = j["m_snapToElements"].get<bool>();
    if (j.contains("m_snapToScreenCenter")) m_snapToScreenCenter = j["m_snapToScreenCenter"].get<bool>();
}

void ArmorHudModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    std::lock_guard lock(m_configMutex);

    j["m_helmet"] = m_helmet;
    j["m_helmetDurability"] = m_helmetDurability;
    j["hudHelmetPosX"] = hudHelmetPosX;
    j["hudHelmetPosY"] = hudHelmetPosY;
    j["m_helmetSize"] = m_helmetSize;

    j["m_chestplate"] = m_chestplate;
    j["m_chestplateDurability"] = m_chestplateDurability;
    j["hudChestplatePosX"] = hudChestplatePosX;
    j["hudChestplatePosY"] = hudChestplatePosY;
    j["m_chestplateSize"] = m_chestplateSize;

    j["m_leggings"] = m_leggings;
    j["m_leggingsDurability"] = m_leggingsDurability;
    j["hudLeggingsPosX"] = hudLeggingsPosX;
    j["hudLeggingsPosY"] = hudLeggingsPosY;
    j["m_leggingsSize"] = m_leggingsSize;

    j["m_boots"] = m_boots;
    j["m_bootsDurability"] = m_bootsDurability;
    j["hudBootsPosX"] = hudBootsPosX;
    j["hudBootsPosY"] = hudBootsPosY;
    j["m_bootsSize"] = m_bootsSize;

    j["m_offhand"] = m_offhand;
    j["m_offhandDurability"] = m_offhandDurability;
    j["hudOffhandPosX"] = hudOffhandPosX;
    j["hudOffhandPosY"] = hudOffhandPosY;
    j["m_offhandSize"] = m_offhandSize;

    j["m_mainhand"] = m_mainhand;
    j["m_mainhandDurability"] = m_mainhandDurability;
    j["hudMainhandPosX"] = hudMainhandPosX;
    j["hudMainhandPosY"] = hudMainhandPosY;
    j["m_mainhandSize"] = m_mainhandSize;

    j["m_showDamage"] = m_showDamage;
    j["m_showRemaining"] = m_showRemaining;
    j["m_showMaxDurability"] = m_showMaxDurability;
    j["m_showPercentage"] = m_showPercentage;
    j["m_durabilityTextSize"] = m_durabilityTextSize;
    j["m_durabilityTextPosition"] = std::to_string(m_durabilityTextPosition) + ",Right,Left,Below";
    j["m_durabilityTextGap"] = m_durabilityTextGap;
    j["m_textColor"] = m_textColor;
    j["m_gridSize"] = m_gridSize;
    j["m_gridGap"] = m_gridGap;
    j["m_snapThreshold"] = m_snapThreshold;
    j["m_snapToGrid"] = m_snapToGrid;
    j["m_snapToElements"] = m_snapToElements;
    j["m_snapToScreenCenter"] = m_snapToScreenCenter;
}
