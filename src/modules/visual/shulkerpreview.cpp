// Shulker preview module — thanks to Kashifro
// GitHub: https://github.com/Kashifro

#include "shulkerpreview.hpp"
#include <pl/memory/Vtable.hpp>
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr const char* MinecraftLibrary = "libminecraftpe.so";
constexpr int ShulkerCacheSize = 16;
constexpr int ShulkerSlotCount = 27;
constexpr int Columns = 9;
constexpr int Rows = 3;
constexpr float SlotStride = 18.0f;
constexpr float SlotDrawSize = 17.5f;
constexpr float PanelPadding = 6.0f;
constexpr float ItemDrawSize = 16.0f;
constexpr float ItemInset = (SlotStride - ItemDrawSize) * 0.5f;
constexpr float CountTextHeight = 6.0f;
constexpr float PanelWidth = Columns * SlotStride + PanelPadding * 2.0f;
constexpr float SelectedAnchorYOffset = SlotStride + 2.0f;

struct ItemStackBase {};
struct Item {};
struct Font {};

struct NbtTreeKey {
    const char* data;
    std::size_t len;
};

struct ListTagLayout {
    void* vtable;
    void* begin;
    void* end;
    void* cap;
    std::uint8_t type;
};

#pragma pack(push, 4)
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

struct TextMeasureData {
    float fontSize;
    float linePadding;
    bool renderShadow;
    bool showColorSymbol;
    bool hideHyphen;
};

struct CaretMeasureData {
    int position;
    bool shouldRender;
};
#pragma pack(pop)

namespace mce {
struct Color {
    float r;
    float g;
    float b;
    float a;
};

struct ClientTexture {
    std::byte storage[24]{};
};
}

struct BedrockTextureData {
    mce::ClientTexture clientTexture;
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

    explicit ResourceLocation(const char* value)
        : fileSystem(ResourceFileSystem::UserPackage),
          path(value ? value : ""),
          pathHash(computeHash(path)),
          fullHash(pathHash ^ static_cast<std::uint64_t>(fileSystem)) {}

private:
    static std::uint64_t computeHash(std::string_view value) {
        constexpr std::uint64_t Offset = 1469598103934665603ULL;
        constexpr std::uint64_t Prime = 1099511628211ULL;
        std::uint64_t hash = Offset;
        for (unsigned char ch : value) hash = static_cast<std::uint64_t>(ch) ^ (Prime * hash);
        return hash;
    }
};

namespace mce {
class TexturePtr {
public:
    std::shared_ptr<const BedrockTextureData> clientTexture;
    std::shared_ptr<ResourceLocation> resourceLocation;

    const ClientTexture& getClientTexture() const {
        static const ClientTexture empty{};
        return clientTexture ? clientTexture->clientTexture : empty;
    }
};
}

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
        constexpr std::uint64_t Offset = 0xCBF29CE484222325ULL;
        constexpr std::uint64_t Prime = 0x100000001B3ULL;
        std::uint64_t result = Offset;
        for (char ch : text) result = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^ (Prime * result);
        return result;
    }
};

enum class TextAlignment : std::uint8_t {
    Left,
    Right,
    Center
};

struct ItemStackStorage {
    alignas(16) std::byte data[bedrocktools::sdk::offsets::ShulkerPreview::ItemStackStorageSize];
};

struct ShulkerSlotCache {
    ItemStackStorage stack;
    std::uint8_t count;
    bool valid;
    bool enchanted;
    int bundleWeight; // 0..64 for bundles, -1 = not a bundle
};

struct CachedUiTextures {
    bool loaded = false;
    mce::TexturePtr panel;
    mce::TexturePtr slot;
};

using NbtTreeFindFn = void* (*)(void*, const NbtTreeKey*);
using ItemStackBaseLoadItemFn = void (*)(void*, void*);
using ItemStackBaseGetDamageValueFn = short (*)(ItemStackBase*);
using BaseActorRenderContextCtorFn = void (*)(void*, void*, void*, void*);
using ItemRendererRenderGuiItemNewFn = std::uint64_t (*)(void*, void*, void*, unsigned int, unsigned char, std::uint64_t, float, float, float, float, float);
using DrawTextFn = void (*)(void*, Font&, const RectangleArea&, const std::string&, const mce::Color&, TextAlignment, float, const TextMeasureData&, const CaretMeasureData&);
using RenderHoverBoxFn = void (*)(void*, void*, void*, void*, float);
using ContainerSlotSelectedFn = std::uint32_t (*)(void*, const std::string&, int);
using ContainerGetItemStackFn = ItemStackBase* (*)(void*, const std::string&, int);
using ScreenViewRenderFn = void (*)(void*, void*, void*, void*, void*, void*, void*, void*);

NbtTreeFindFn nbtTreeFind = nullptr;
ItemStackBaseLoadItemFn itemStackBaseLoadItem = nullptr;
ItemStackBaseGetDamageValueFn itemStackBaseGetDamageValue = nullptr;
BaseActorRenderContextCtorFn baseActorRenderContextCtor = nullptr;
ItemRendererRenderGuiItemNewFn itemRendererRenderGuiItemNew = nullptr;
ContainerSlotSelectedFn containerSlotSelectedOriginal = nullptr;
ContainerGetItemStackFn containerGetItemStack = nullptr;
ScreenViewRenderFn screenViewRenderOriginal = nullptr;
DrawTextFn drawTextOriginal = nullptr;
RenderHoverBoxFn renderHoverBoxOriginal = nullptr;
ShulkerPreviewModule* moduleInstance = nullptr;
void* activeUiContext = nullptr;
Font* activeFont = nullptr;
void* selectedController = nullptr;
std::string selectedCollection;
int selectedIndex = -1;
int activeCacheIndex = -1;
char activeColorCode = '0';
bool hasShulkerData = false;
float selectedAnchorX = 0.0f;
float selectedAnchorY = 0.0f;
bool selectedAnchorValid = false;
ShulkerSlotCache shulkerCache[ShulkerCacheSize][ShulkerSlotCount]{};

ItemStackBase* asStack(ItemStackStorage& storage) {
    return reinterpret_cast<ItemStackBase*>(storage.data);
}

void* getStackUserData(ItemStackBase* stack) {
    if (!stack) return nullptr;
    return *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(stack) + bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseUserData);
}

Item* getStackItem(ItemStackBase* stack) {
    if (!stack) return nullptr;
    void* counter = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(stack) + bedrocktools::sdk::offsets::ShulkerPreview::ItemStackBaseItem);
    if (!counter) return nullptr;
    return *reinterpret_cast<Item**>(reinterpret_cast<std::byte*>(counter) + bedrocktools::sdk::offsets::ShulkerPreview::SharedCounterPointer);
}

std::uint16_t getItemId(Item* item) {
    if (!item) return 0;
    return *reinterpret_cast<std::uint16_t*>(reinterpret_cast<std::byte*>(item) + bedrocktools::sdk::offsets::ShulkerPreview::ItemId);
}

short getItemMaxDamage(Item* item) {
    if (!item) return 0;
    void** vtable = *reinterpret_cast<void***>(item);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage]) return 0;
    return reinterpret_cast<short (*)(Item*)>(vtable[bedrocktools::sdk::offsets::VTable::ItemGetMaxDamage])(item);
}

void* treeFindNode(void* compound, const char* key, std::size_t length) {
    if (!compound || !nbtTreeFind) return nullptr;
    NbtTreeKey searchKey{key, length};
    auto* base = reinterpret_cast<std::byte*>(compound);
    void* treeRoot = base + bedrocktools::sdk::offsets::ShulkerPreview::CompoundTagTreeRoot;
    void* treeEnd = base + bedrocktools::sdk::offsets::ShulkerPreview::CompoundTagTreeEnd;
    void* node = nbtTreeFind(treeRoot, &searchKey);
    return node == treeEnd ? nullptr : node;
}

std::uint32_t nodeType(void* node) {
    return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(node) + bedrocktools::sdk::offsets::ShulkerPreview::NbtNodeType);
}

void* nodePayload(void* node) {
    return reinterpret_cast<std::byte*>(node) + bedrocktools::sdk::offsets::ShulkerPreview::NbtNodePayload;
}

template <std::size_t N>
bool containsTag(void* compound, const char (&key)[N]) {
    return treeFindNode(compound, key, N - 1) != nullptr;
}

template <std::size_t N>
void* getListTag(void* compound, const char (&key)[N]) {
    void* node = treeFindNode(compound, key, N - 1);
    if (!node || nodeType(node) != 9) return nullptr;
    return nodePayload(node);
}

template <std::size_t N>
void* getCompoundTag(void* compound, const char (&key)[N]) {
    void* node = treeFindNode(compound, key, N - 1);
    if (!node || nodeType(node) != 10) return nullptr;
    return nodePayload(node);
}

int listSize(ListTagLayout* list) {
    if (!list || !list->begin || !list->end) return 0;
    auto difference = reinterpret_cast<std::byte*>(list->end) - reinterpret_cast<std::byte*>(list->begin);
    if (difference <= 0) return 0;
    return static_cast<int>(difference / sizeof(void*));
}

void* listAt(ListTagLayout* list, int index) {
    if (!list || index < 0 || index >= listSize(list)) return nullptr;
    return reinterpret_cast<void**>(list->begin)[index];
}

bool hasEnchantmentData(void* compound) {
    if (!compound || !nbtTreeFind) return false;
    return containsTag(compound, "ench") ||
           containsTag(compound, "Enchantments") ||
           containsTag(compound, "StoredEnchantments") ||
           containsTag(compound, "minecraft:enchantments") ||
           containsTag(compound, "minecraft:stored_enchantments");
}

template <std::size_t N>
bool readIntTag(void* compound, const char (&key)[N], int& output) {
    void* node = treeFindNode(compound, key, N - 1);
    if (!node) return false;
    auto* value = reinterpret_cast<std::byte*>(node) + bedrocktools::sdk::offsets::ShulkerPreview::NbtNodeNumericValue;
    switch (nodeType(node)) {
        case 1:
            output = *reinterpret_cast<std::uint8_t*>(value);
            return true;
        case 2:
            output = *reinterpret_cast<std::uint16_t*>(value);
            return true;
        case 3:
            output = *reinterpret_cast<std::int32_t*>(value);
            return true;
        default:
            return false;
    }
}

// tag -> bundle_weight (0..64), -1 if not a bundle
int readBundleWeight(void* itemTag) {
    if (!itemTag) return -1;
    void* tagCompound = getCompoundTag(itemTag, "tag");
    if (!tagCompound) return -1;
    int weight = 0;
    return readIntTag(tagCompound, "bundle_weight", weight) ? weight : -1;
}

bool getShulkerColorCode(std::uint16_t id, char& code) {
    struct Entry {
        std::uint16_t id;
        char code;
    };
    static constexpr Entry entries[] = {
        {205, '0'}, {218, '1'}, {64923, '7'}, {64922, 'f'}, {64921, 'c'},
        {64920, '8'}, {64919, '9'}, {64918, 'g'}, {64917, '3'}, {64916, '2'},
        {64915, 'b'}, {64914, 'e'}, {64913, 'd'}, {64912, '5'}, {64911, 'a'},
        {64910, '6'}, {64909, '4'}
    };
    for (const auto& entry : entries) {
        if (entry.id == id) {
            code = entry.code;
            return true;
        }
    }
    return false;
}

mce::Color getShulkerTint(char code) {
    switch (code) {
        case '0': return {0.45f, 0.42f, 0.40f, 1.0f};
        case '1': return {0.78f, 0.76f, 0.74f, 1.0f};
        case '2': return {0.55f, 0.53f, 0.52f, 1.0f};
        case '3': return {0.32f, 0.31f, 0.30f, 1.0f};
        case '4': return {0.06f, 0.05f, 0.05f, 1.0f};
        case '5': return {0.33f, 0.25f, 0.14f, 1.0f};
        case '6': return {0.55f, 0.20f, 0.18f, 1.0f};
        case '7': return {0.70f, 0.42f, 0.18f, 1.0f};
        case '8': return {0.78f, 0.72f, 0.22f, 1.0f};
        case '9': return {0.42f, 0.65f, 0.22f, 1.0f};
        case 'a': return {0.18f, 0.40f, 0.18f, 1.0f};
        case 'b': return {0.18f, 0.55f, 0.55f, 1.0f};
        case 'c': return {0.28f, 0.46f, 0.62f, 1.0f};
        case 'd': return {0.18f, 0.24f, 0.58f, 1.0f};
        case 'e': return {0.45f, 0.26f, 0.60f, 1.0f};
        case 'f': return {0.65f, 0.34f, 0.58f, 1.0f};
        case 'g': return {0.78f, 0.52f, 0.62f, 1.0f};
        default: return {0.55f, 0.55f, 0.55f, 1.0f};
    }
}

mce::Color applyTint(const mce::Color& base) {
    float intensity = moduleInstance ? moduleInstance->m_tintIntensity : 2.0f;
    auto clamp = [](float value) { return std::clamp(value, 0.0f, 1.0f); };
    return {clamp(base.r * intensity), clamp(base.g * intensity), clamp(base.b * intensity), base.a};
}

void** getVtable(void* instance) {
    return instance ? *reinterpret_cast<void***>(instance) : nullptr;
}

float uiGetLineLength(void* context, Font& font, const std::string& text, float size, bool unknown) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetLineLength]) return 0.0f;
    using Fn = float (*)(void*, Font&, const std::string&, float, bool);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetLineLength])(context, font, text, size, unknown);
}

void uiDrawText(void* context, Font& font, const RectangleArea& rectangle, const std::string& text, const mce::Color& color, TextAlignment alignment, float alpha, const TextMeasureData& measure, const CaretMeasureData& caret) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawText]) return;
    reinterpret_cast<DrawTextFn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawText])(context, font, rectangle, text, color, alignment, alpha, measure, caret);
}

void uiFlushText(void* context, float value, std::optional<float> optionalValue) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushText]) return;
    using Fn = void (*)(void*, float, std::optional<float>);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushText])(context, value, optionalValue);
}

void uiDrawImage(void* context, const mce::ClientTexture& texture, const UiVec2& position, const UiVec2& size, const UiVec2& uv, const UiVec2& uvSize, bool tiled) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage]) return;
    using Fn = void (*)(void*, const mce::ClientTexture&, const UiVec2&, const UiVec2&, const UiVec2&, const UiVec2&, bool);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawImage])(context, texture, position, size, uv, uvSize, tiled);
}

void uiFlushImages(void* context, const mce::Color& color, float alpha, const HashedString& material) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages]) return;
    using Fn = void (*)(void*, const mce::Color&, float, const HashedString&);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFlushImages])(context, color, alpha, material);
}

void uiFillRectangle(void* context, const RectangleArea& rectangle, const mce::Color& color, float alpha) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle]) return;
    using Fn = void (*)(void*, const RectangleArea&, const mce::Color&, float);
    reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextFillRectangle])(context, rectangle, color, alpha);
}

mce::TexturePtr uiGetTexture(void* context, const ResourceLocation& location, bool forceReload) {
    void** vtable = getVtable(context);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture]) return {};
    using Fn = mce::TexturePtr (*)(void*, const ResourceLocation&, bool);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetTexture])(context, location, forceReload);
}

bool hasTexture(const mce::TexturePtr& texture) {
    return static_cast<bool>(texture.clientTexture);
}

CachedUiTextures& getTextures(void* context) {
    static CachedUiTextures textures;
    if (!textures.loaded) {
        textures.panel = uiGetTexture(context, ResourceLocation("textures/ui/dialog_background_opaque"), false);
        textures.slot = uiGetTexture(context, ResourceLocation("textures/ui/item_cell"), false);
        textures.loaded = true;
    }
    return textures;
}

template <typename Function>
void forEachSlot(float originX, float originY, Function&& function) {
    for (int row = 0; row < Rows; ++row) {
        for (int column = 0; column < Columns; ++column) {
            function(row * Columns + column, originX + column * SlotStride, originY + row * SlotStride);
        }
    }
}

void drawNineSlice(void* context, const mce::ClientTexture& texture, const RectangleArea& rectangle) {
    constexpr float textureSize = 16.0f;
    constexpr float slice = 4.0f;
    float width = std::max(0.0f, rectangle.x1 - rectangle.x0);
    float height = std::max(0.0f, rectangle.y1 - rectangle.y0);
    float middleWidth = std::max(0.0f, width - slice * 2.0f);
    float middleHeight = std::max(0.0f, height - slice * 2.0f);
    float textureMiddle = textureSize - slice * 2.0f;
    UiVec2 positions[9] = {
        {rectangle.x0, rectangle.y0},
        {rectangle.x0 + slice, rectangle.y0},
        {rectangle.x1 - slice, rectangle.y0},
        {rectangle.x0, rectangle.y0 + slice},
        {rectangle.x0 + slice, rectangle.y0 + slice},
        {rectangle.x1 - slice, rectangle.y0 + slice},
        {rectangle.x0, rectangle.y1 - slice},
        {rectangle.x0 + slice, rectangle.y1 - slice},
        {rectangle.x1 - slice, rectangle.y1 - slice}
    };
    UiVec2 sizes[9] = {
        {slice, slice}, {middleWidth, slice}, {slice, slice},
        {slice, middleHeight}, {middleWidth, middleHeight}, {slice, middleHeight},
        {slice, slice}, {middleWidth, slice}, {slice, slice}
    };
    UiVec2 uvPositions[9] = {
        {0.0f, 0.0f},
        {slice / textureSize, 0.0f},
        {(textureSize - slice) / textureSize, 0.0f},
        {0.0f, slice / textureSize},
        {slice / textureSize, slice / textureSize},
        {(textureSize - slice) / textureSize, slice / textureSize},
        {0.0f, (textureSize - slice) / textureSize},
        {slice / textureSize, (textureSize - slice) / textureSize},
        {(textureSize - slice) / textureSize, (textureSize - slice) / textureSize}
    };
    UiVec2 uvSizes[9] = {
        {slice / textureSize, slice / textureSize},
        {textureMiddle / textureSize, slice / textureSize},
        {slice / textureSize, slice / textureSize},
        {slice / textureSize, textureMiddle / textureSize},
        {textureMiddle / textureSize, textureMiddle / textureSize},
        {slice / textureSize, textureMiddle / textureSize},
        {slice / textureSize, slice / textureSize},
        {textureMiddle / textureSize, slice / textureSize},
        {slice / textureSize, slice / textureSize}
    };
    for (int index = 0; index < 9; ++index) {
        if (sizes[index].x <= 0.0f || sizes[index].y <= 0.0f) continue;
        uiDrawImage(context, texture, positions[index], sizes[index], uvPositions[index], uvSizes[index], false);
    }
}

ItemStackBase* getCachedStack(ShulkerSlotCache& cache) {
    if (!cache.valid) return nullptr;
    ItemStackBase* stack = asStack(cache.stack);
    return getStackItem(stack) ? stack : nullptr;
}

bool hasEnchantedItem(int cacheIndex) {
    for (int slot = 0; slot < ShulkerSlotCount; ++slot) {
        if (shulkerCache[cacheIndex][slot].valid && shulkerCache[cacheIndex][slot].enchanted) return true;
    }
    return false;
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

void* getClientLocalPlayer(void* client) {
    if (!client) return nullptr;
    void** vtable = getVtable(client);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer]) return nullptr;
    return reinterpret_cast<void* (*)(void*)>(vtable[bedrocktools::sdk::offsets::VTable::ClientInstanceGetLocalPlayer])(client);
}

unsigned int getItemAnimationFrame(Item* item, void* localPlayer, ItemStackBase* stack) {
    if (!item || !localPlayer || !stack) return 0;
    void** vtable = getVtable(item);
    if (!vtable || !vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor]) return 0;
    using Fn = unsigned int (*)(Item*, void*, int, ItemStackBase*, int);
    return reinterpret_cast<Fn>(vtable[bedrocktools::sdk::offsets::VTable::ItemGetAnimationFrameFor])(item, localPlayer, 0, stack, 1);
}

void destroyBaseActorRenderContext(void* context) {
    void** vtable = getVtable(context);
    if (vtable && vtable[0]) reinterpret_cast<void (*)(void*)>(vtable[0])(context);
}

void drawDurabilityBar(void* context, ItemStackBase* stack, float x, float y) {
    if (!itemStackBaseGetDamageValue) return;
    Item* item = getStackItem(stack);
    short maxDamage = getItemMaxDamage(item);
    if (maxDamage <= 0) return;
    short damage = itemStackBaseGetDamageValue(stack);
    if (damage <= 0) return;
    float ratio = static_cast<float>(maxDamage - damage) / static_cast<float>(maxDamage);
    ratio = std::clamp(ratio, 0.0f, 1.0f);
    float barX = x + 2.0f;
    float barY = y + 13.0f;
    uiFillRectangle(context, {barX, barX + 13.0f, barY, barY + 2.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f);
    uiFillRectangle(context, {barX, barX + 13.0f * ratio, barY, barY + 1.0f}, {1.0f - ratio, ratio, 0.0f, 1.0f}, 1.0f);
}

void drawBundleFullnessBar(void* context, float x, float y, int weight) {
    float ratio = std::clamp(static_cast<float>(weight) / 64.0f, 0.0f, 1.0f);
    float barX = x + 2.0f;
    float barY = y + 13.0f;
    mce::Color fill = weight >= 64 ? mce::Color{1.0f, 0.40f, 0.40f, 1.0f} : mce::Color{0.40f, 0.40f, 1.0f, 1.0f};
    uiFillRectangle(context, {barX, barX + 13.0f, barY, barY + 2.0f}, {0.0f, 0.0f, 0.0f, 1.0f}, 1.0f);
    uiFillRectangle(context, {barX, barX + 13.0f * ratio, barY, barY + 1.0f}, fill, 1.0f);
}

void drawIcons(void* context, int cacheIndex, float originX, float originY) {
    if (!baseActorRenderContextCtor || !itemRendererRenderGuiItemNew) return;
    void* client = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) + bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextClient);
    void* screenContext = *reinterpret_cast<void**>(reinterpret_cast<std::byte*>(context) + bedrocktools::sdk::offsets::ShulkerPreview::MinecraftUIRenderContextScreenContext);
    if (!client || !screenContext) return;
    void* game = getMinecraftGame(client);
    if (!game) return;
    alignas(16) std::byte baseActorRenderContext[bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextStorageSize]{};
    baseActorRenderContextCtor(baseActorRenderContext, screenContext, client, game);
    void* itemRenderer = *reinterpret_cast<void**>(baseActorRenderContext + bedrocktools::sdk::offsets::ShulkerPreview::BaseActorRenderContextItemRenderer);
    if (!itemRenderer) {
        destroyBaseActorRenderContext(baseActorRenderContext);
        return;
    }
    void* localPlayer = getClientLocalPlayer(client);
    forEachSlot(originX, originY, [&](int slot, float x, float y) {
        ItemStackBase* stack = getCachedStack(shulkerCache[cacheIndex][slot]);
        if (!stack) return;
        unsigned int aux = localPlayer ? getItemAnimationFrame(getStackItem(stack), localPlayer, stack) : 0;
        itemRendererRenderGuiItemNew(itemRenderer, baseActorRenderContext, stack, aux, 0, 0, x + ItemInset, y + ItemInset, 1.0f, 1.0f, 1.0f);
    });
    if (hasEnchantedItem(cacheIndex)) {
        forEachSlot(originX, originY, [&](int slot, float x, float y) {
            ShulkerSlotCache& cache = shulkerCache[cacheIndex][slot];
            if (!cache.enchanted) return;
            ItemStackBase* stack = getCachedStack(cache);
            if (!stack) return;
            unsigned int aux = localPlayer ? getItemAnimationFrame(getStackItem(stack), localPlayer, stack) : 0;
            itemRendererRenderGuiItemNew(itemRenderer, baseActorRenderContext, stack, aux, 1, 1, x + ItemInset, y + ItemInset, 1.0f, 1.0f, 1.0f);
        });
    }
    forEachSlot(originX, originY, [&](int slot, float x, float y) {
        ShulkerSlotCache& cache = shulkerCache[cacheIndex][slot];
        ItemStackBase* stack = getCachedStack(cache);
        if (!stack) return;
        drawDurabilityBar(context, stack, x, y);
        if (cache.bundleWeight >= 0) drawBundleFullnessBar(context, x, y, cache.bundleWeight);
    });
    destroyBaseActorRenderContext(baseActorRenderContext);
    static const HashedString material("ui_flush");
    uiFlushImages(context, {1.0f, 1.0f, 1.0f, 1.0f}, 1.0f, material);
}

void renderPreview(void* context, float x, float y, int cacheIndex, char colorCode) {
    if (!context || cacheIndex < 0 || cacheIndex >= ShulkerCacheSize) return;
    CachedUiTextures& textures = getTextures(context);
    static const HashedString material("ui_flush");
    mce::Color tint = applyTint(getShulkerTint(colorCode));
    RectangleArea panel{x, x + Columns * SlotStride + PanelPadding * 2.0f, y, y + Rows * SlotStride + PanelPadding * 2.0f};
    if (hasTexture(textures.panel)) drawNineSlice(context, textures.panel.getClientTexture(), panel);
    uiFlushImages(context, tint, 1.0f, material);
    float originX = x + PanelPadding;
    float originY = y + PanelPadding;
    if (hasTexture(textures.slot)) {
        forEachSlot(originX, originY, [&](int, float slotX, float slotY) {
            uiDrawImage(context, textures.slot.getClientTexture(), {slotX, slotY}, {SlotDrawSize, SlotDrawSize}, {0.0f, 0.0f}, {1.0f, 1.0f}, false);
        });
    }
    uiFlushImages(context, tint, 1.0f, material);
    drawIcons(context, cacheIndex, originX, originY);
    if (activeFont) {
        TextMeasureData measure{};
        measure.fontSize = 1.0f;
        CaretMeasureData caret{};
        forEachSlot(originX, originY, [&](int slot, float slotX, float slotY) {
            ShulkerSlotCache& cache = shulkerCache[cacheIndex][slot];
            if (!cache.valid || cache.count <= 1) return;
            char buffer[8];
            std::snprintf(buffer, sizeof(buffer), "%u", cache.count);
            std::string text(buffer);
            float width = uiGetLineLength(context, *activeFont, text, 1.0f, false);
            float right = slotX + SlotDrawSize - 0.5f;
            float bottom = slotY + SlotDrawSize - 1.5f;
            uiDrawText(context, *activeFont, {right - width, right, bottom - CountTextHeight, bottom}, text, {1.0f, 1.0f, 1.0f, 1.0f}, TextAlignment::Right, 1.0f, measure, caret);
        });
    }
    uiFlushText(context, 0.0f, std::nullopt);
}

bool hookVtable(const char* className, void** original, void* replacement, std::size_t slot) {
    const auto target = pl::memory::resolveVtableFunction(className, slot, MinecraftLibrary);
    if (!target) return false;
    return bedrocktools::hooks::install(reinterpret_cast<void*>(target), replacement, original) != nullptr;
}

void clearSelection() {
    selectedController = nullptr;
    selectedCollection.clear();
    selectedIndex = -1;
    hasShulkerData = false;
    activeCacheIndex = -1;
    activeColorCode = '0';
    selectedAnchorX = 0.0f;
    selectedAnchorY = 0.0f;
    selectedAnchorValid = false;
}

void clearCache(int cacheIndex) {
    if (cacheIndex < 0 || cacheIndex >= ShulkerCacheSize) return;
    for (int slot = 0; slot < ShulkerSlotCount; ++slot) {
        shulkerCache[cacheIndex][slot].valid = false;
        shulkerCache[cacheIndex][slot].enchanted = false;
        shulkerCache[cacheIndex][slot].count = 0;
        shulkerCache[cacheIndex][slot].bundleWeight = -1;
    }
}

bool loadSelectedShulker(ItemStackBase* stack) {
    if (!stack || !itemStackBaseLoadItem) return false;
    Item* item = getStackItem(stack);
    char colorCode = '0';
    if (!item || !getShulkerColorCode(getItemId(item), colorCode)) return false;
    int cacheIndex = static_cast<int>((reinterpret_cast<std::uintptr_t>(stack) >> 4) & (ShulkerCacheSize - 1));
    clearCache(cacheIndex);
    void* userData = getStackUserData(stack);
    if (userData) {
        auto* list = reinterpret_cast<ListTagLayout*>(getListTag(userData, "Items"));
        if (!list) list = reinterpret_cast<ListTagLayout*>(getListTag(userData, "items"));
        if (list) {
            int size = listSize(list);
            for (int index = 0; index < size; ++index) {
                void* tag = listAt(list, index);
                if (!tag) continue;
                int slotValue = 0;
                if (!readIntTag(tag, "Slot", slotValue) && !readIntTag(tag, "slot", slotValue)) continue;
                int countValue = 1;
                if (!readIntTag(tag, "Count", countValue) && !readIntTag(tag, "count", countValue)) countValue = 1;
                countValue = std::clamp(countValue, 0, 255);
                if (slotValue < 0 || slotValue >= ShulkerSlotCount) continue;
                ShulkerSlotCache& cache = shulkerCache[cacheIndex][slotValue];
                itemStackBaseLoadItem(asStack(cache.stack), tag);
                cache.count = static_cast<std::uint8_t>(countValue);
                void* loadedUserData = getStackUserData(asStack(cache.stack));
                cache.enchanted = hasEnchantmentData(tag) || hasEnchantmentData(loadedUserData);
                cache.bundleWeight = readBundleWeight(tag);
                cache.valid = true;
            }
        }
    }
    activeCacheIndex = cacheIndex;
    activeColorCode = colorCode;
    hasShulkerData = true;
    return true;
}

std::uint32_t containerSlotSelectedHook(void* self, const std::string& collectionName, int index) {
    std::uint32_t result = containerSlotSelectedOriginal ? containerSlotSelectedOriginal(self, collectionName, index) : 0;
    if (!moduleInstance || !moduleInstance->enabled || !containerGetItemStack) {
        clearSelection();
        return result;
    }
    ItemStackBase* stack = containerGetItemStack(self, collectionName, index);
    if (!loadSelectedShulker(stack)) {
        clearSelection();
        return result;
    }
    selectedController = self;
    selectedCollection = collectionName;
    selectedIndex = index;
    selectedAnchorValid = false;
    return result;
}

bool selectedSlotStillContainsShulker() {
    if (!selectedController || !containerGetItemStack || selectedIndex < 0) return false;
    ItemStackBase* stack = containerGetItemStack(selectedController, selectedCollection, selectedIndex);
    Item* item = getStackItem(stack);
    char colorCode = '0';
    return item && getShulkerColorCode(getItemId(item), colorCode);
}

void renderHoverBoxHook(void* self, void* context, void* client, void* area, float value) {
    if (renderHoverBoxOriginal) renderHoverBoxOriginal(self, context, client, area, value);
    if (!moduleInstance || !moduleInstance->enabled || !moduleInstance->m_followSelectedShulker || !selectedController || !hasShulkerData || !self) return;
    auto* base = reinterpret_cast<std::byte*>(self);
    selectedAnchorX = *reinterpret_cast<float*>(base + bedrocktools::sdk::offsets::ShulkerPreview::HoverRendererCursorX);
    selectedAnchorY = *reinterpret_cast<float*>(base + bedrocktools::sdk::offsets::ShulkerPreview::HoverRendererCursorY);
    selectedAnchorValid = true;
}

void screenViewRenderHook(void* self, void* a2, void* a3, void* a4, void* a5, void* a6, void* a7, void* a8) {
    activeUiContext = nullptr;
    activeFont = nullptr;
    if (screenViewRenderOriginal) screenViewRenderOriginal(self, a2, a3, a4, a5, a6, a7, a8);
    if (!moduleInstance || !moduleInstance->enabled || !selectedController || !activeUiContext || !hasShulkerData || activeCacheIndex < 0) return;
    if (!selectedSlotStillContainsShulker()) {
        clearSelection();
        return;
    }
    float previewX = moduleInstance->m_positionX;
    float previewY = moduleInstance->m_positionY;
    if (moduleInstance->m_followSelectedShulker && selectedAnchorValid) {
        previewX = std::max(0.0f, selectedAnchorX - PanelWidth * 0.5f);
        previewY = std::max(0.0f, selectedAnchorY + SelectedAnchorYOffset);
    }
    renderPreview(activeUiContext, previewX, previewY, activeCacheIndex, activeColorCode);
}

void drawTextHook(void* self, Font& font, const RectangleArea& rectangle, const std::string& text, const mce::Color& color, TextAlignment alignment, float alpha, const TextMeasureData& measure, const CaretMeasureData& caret) {
    activeUiContext = self;
    activeFont = &font;
    if (drawTextOriginal) drawTextOriginal(self, font, rectangle, text, color, alignment, alpha, measure, caret);
}

}

ShulkerPreviewModule::ShulkerPreviewModule()
    : Module("ShulkerPreview", "Shows the contents of a selected shulker box.") {
    moduleInstance = this;
}

ShulkerPreviewModule::~ShulkerPreviewModule() {
    if (moduleInstance == this) moduleInstance = nullptr;
}

void ShulkerPreviewModule::onInit() {
    if (m_hooksInstalled) return;
    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>([](auto& event) {
        if (event.screen == bedrocktools::events::ScreenKind::Container && event.phase == bedrocktools::events::ScreenPhase::Closed) {
            ShulkerPreviewHandleContainerDestroyed(event.controller);
        }
    });
    nbtTreeFind = reinterpret_cast<NbtTreeFindFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::NbtTreeFind));
    itemStackBaseLoadItem = reinterpret_cast<ItemStackBaseLoadItemFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseLoadItem));
    itemStackBaseGetDamageValue = reinterpret_cast<ItemStackBaseGetDamageValueFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemStackBaseGetDamageValue));
    baseActorRenderContextCtor = reinterpret_cast<BaseActorRenderContextCtorFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseActorRenderContextCtor));
    itemRendererRenderGuiItemNew = reinterpret_cast<ItemRendererRenderGuiItemNewFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemRendererRenderGuiItemNew));
    containerGetItemStack = reinterpret_cast<ContainerGetItemStackFn>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ContainerScreenControllerGetItemStack));
    uintptr_t selectedAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ContainerScreenControllerOnContainerSlotSelected);
    if (selectedAddress) {
        bedrocktools::hooks::install(reinterpret_cast<void*>(selectedAddress), reinterpret_cast<void*>(containerSlotSelectedHook), reinterpret_cast<void**>(&containerSlotSelectedOriginal));
    }
    uintptr_t screenRenderAddress = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ScreenViewRender);
    if (screenRenderAddress) {
        bedrocktools::hooks::install(reinterpret_cast<void*>(screenRenderAddress), reinterpret_cast<void*>(screenViewRenderHook), reinterpret_cast<void**>(&screenViewRenderOriginal));
    }
    hookVtable("17HoverTextRenderer", reinterpret_cast<void**>(&renderHoverBoxOriginal), reinterpret_cast<void*>(renderHoverBoxHook), bedrocktools::sdk::offsets::VTable::HoverTextRendererRenderHoverBox);
    hookVtable("24MinecraftUIRenderContext", reinterpret_cast<void**>(&drawTextOriginal), reinterpret_cast<void*>(drawTextHook), bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextDrawText);
    m_hooksInstalled = true;
}

void ShulkerPreviewModule::onDisable() {
    clearSelection();
}

void ShulkerPreviewModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_tintIntensity")) m_tintIntensity = std::clamp(j["m_tintIntensity"].get<float>(), 0.0f, 10.0f);
    if (j.contains("m_positionX")) m_positionX = std::clamp(j["m_positionX"].get<float>(), 0.0f, 2000.0f);
    if (j.contains("m_positionY")) m_positionY = std::clamp(j["m_positionY"].get<float>(), 0.0f, 2000.0f);
    if (j.contains("m_followSelectedShulker")) m_followSelectedShulker = j["m_followSelectedShulker"].get<bool>();
}

void ShulkerPreviewModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_tintIntensity"] = m_tintIntensity;
    j["m_positionX"] = m_positionX;
    j["m_positionY"] = m_positionY;
    j["m_followSelectedShulker"] = m_followSelectedShulker;
}

void ShulkerPreviewHandleContainerDestroyed(void* controller) {
    if (selectedController == controller) clearSelection();
}
