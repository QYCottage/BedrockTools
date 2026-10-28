#include "mappreview.hpp"
#include "core/PreviewSelection.hpp"
#include "core/UiRenderPass.hpp"
#include "core/RenderState.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/memory/Vtable.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr int MapWidth = 128;
constexpr int MapHeight = 128;
constexpr std::size_t MapPixelCount =
    static_cast<std::size_t>(MapWidth) * MapHeight;
constexpr float FramePadding = 4.0f;
constexpr float AnchorYOffset = 20.0f;

struct MapRect {
  float x0;
  float x1;
  float y0;
  float y1;
  float r;
  float g;
  float b;
  float a;
};

struct ItemStackBase {};

struct NbtTreeKey {
  const char *data;
  std::size_t len;
};

namespace mce {
struct Color {
  float r;
  float g;
  float b;
  float a;
};
}

class HashedString {
public:
  std::uint64_t hash;
  std::string value;
  mutable const HashedString *lastMatch;

  explicit HashedString(const char *text)
      : hash(computeHash(text ? std::string_view(text) : std::string_view())),
        value(text ? text : ""), lastMatch(nullptr) {}

private:
  static std::uint64_t computeHash(std::string_view text) {
    if (text.empty())
      return 0;
    constexpr std::uint64_t Offset = 0xCBF29CE484222325ULL;
    constexpr std::uint64_t Prime = 0x100000001B3ULL;
    std::uint64_t result = Offset;
    for (char ch : text)
      result = static_cast<std::uint64_t>(static_cast<unsigned char>(ch)) ^
               (Prime * result);
    return result;
  }
};

struct MaterialPtr {
  void *sharedPtrData[2]{nullptr, nullptr};

  MaterialPtr() = default;
  MaterialPtr(const MaterialPtr &) = delete;
  MaterialPtr &operator=(const MaterialPtr &) = delete;

  MaterialPtr(MaterialPtr &&other) noexcept
      : sharedPtrData{other.sharedPtrData[0], other.sharedPtrData[1]} {
    other.sharedPtrData[0] = nullptr;
    other.sharedPtrData[1] = nullptr;
  }

  MaterialPtr &operator=(MaterialPtr &&other) noexcept {
    if (this != &other) {
      sharedPtrData[0] = other.sharedPtrData[0];
      sharedPtrData[1] = other.sharedPtrData[1];
      other.sharedPtrData[0] = nullptr;
      other.sharedPtrData[1] = nullptr;
    }
    return *this;
  }

  ~MaterialPtr() {}

  explicit operator bool() const { return sharedPtrData[0] != nullptr; }
};

enum class PrimitiveMode : int { QuadList = 1 };

using NbtTreeFindFn = void *(*)(void *, const NbtTreeKey *);
using LevelGetMapSavedDataByTagFn = void *(*)(void *, const void *);
using LevelRequestMapInfoFn = void (*)(void *, std::int64_t, bool);
using TessellatorBeginFn = void (*)(void *, void *, int, int, int);
using TessellatorColorFn = void (*)(void *, float, float, float, float);
using TessellatorVertexFn = void (*)(void *, float, float, float);
using RenderMeshImmediatelyFn = void (*)(void *, void *, void *, void *);
using RenderHoverBoxFn = void (*)(void *, void *, void *, void *, float);

NbtTreeFindFn nbtTreeFind = nullptr;
LevelGetMapSavedDataByTagFn levelGetMapSavedDataByTag = nullptr;
LevelRequestMapInfoFn levelRequestMapInfo = nullptr;
TessellatorBeginFn tessellatorBegin = nullptr;
TessellatorColorFn tessellatorColor = nullptr;
TessellatorVertexFn tessellatorVertex = nullptr;
RenderMeshImmediatelyFn renderMeshImmediately = nullptr;
std::uintptr_t renderMaterialGroup = 0;
MaterialPtr uiFillMaterial;
MapPreviewModule *moduleInstance = nullptr;
std::uint64_t selectedRevision = 0;
bool selectedInContainer = false;
float selectedAnchorX = 0.0f;
float selectedAnchorY = 0.0f;
bool selectedAnchorValid = false;
bool hasPixels = false;
bool mapInfoRequested = false;
bool pendingLoad = false;
std::chrono::steady_clock::time_point lastRefresh{};
void *cachedLevel = nullptr;
void *cachedDimension = nullptr;
std::int64_t cachedMapUuid = 0;
std::array<std::uint32_t, MapPixelCount> pixels{};
std::vector<MapRect> cachedRects;
RenderHoverBoxFn renderHoverBoxOriginal = nullptr;
bool hooksInstalled = false;
bedrocktools::hooks::Handle renderHoverBoxHandle = nullptr;
constexpr const char *MinecraftLibrary = "libminecraftpe.so";

void **getVtable(void *instance) {
  return instance ? *reinterpret_cast<void ***>(instance) : nullptr;
}

std::uintptr_t resolveAdrp(std::uint32_t *instructions, std::size_t count,
                           std::uint32_t targetRegister) {
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t instruction = instructions[i];
    if ((instruction & 0x1F) != targetRegister)
      continue;
    if ((instruction & 0x9F000000) == 0x90000000) {
      std::uintptr_t page =
          (reinterpret_cast<std::uintptr_t>(&instructions[i]) & ~0xFFFULL) +
          ((static_cast<std::int64_t>(
                static_cast<std::uint64_t>(((instruction >> 3) & 0x1FFFFC) |
                                           ((instruction >> 29) & 3))
                << 43) >>
            31));
      for (std::size_t j = i + 1; j < count; ++j) {
        std::uint32_t add = instructions[j];
        if ((add & 0xFF000000) == 0x91000000 &&
            ((add >> 5) & 0x1F) == targetRegister &&
            (add & 0x1F) == targetRegister) {
          std::uint32_t immediate = (add >> 10) & 0xFFF;
          if (add & 0x400000)
            immediate <<= 12;
          return page + immediate;
        }
        if ((add & 0x1F) == targetRegister)
          break;
      }
    }
    if ((instruction & 0x9F000000) == 0x10000000) {
      std::int64_t immediate =
          static_cast<std::int64_t>(static_cast<std::uint64_t>(
                                        ((instruction >> 3) & 0x1FFFFC) |
                                        (instruction >> 29))
                                    << 43) >>
          43;
      return reinterpret_cast<std::uintptr_t>(&instructions[i]) + immediate;
    }
  }
  return 0;
}

MaterialPtr getMaterial(const char *name) {
  if (!renderMaterialGroup)
    return {};
  HashedString materialName(name);
  void **vtable = *reinterpret_cast<void ***>(renderMaterialGroup);
  if (!vtable ||
      !vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial])
    return {};
  using Fn = MaterialPtr (*)(void *, const HashedString *);
  return reinterpret_cast<Fn>(
      vtable[bedrocktools::sdk::offsets::VTable::RenderMaterialGroup_getMaterial])(
      reinterpret_cast<void *>(renderMaterialGroup), &materialName);
}

void ensureRenderMaterial() {
  if (!uiFillMaterial && renderMaterialGroup)
    uiFillMaterial = getMaterial("ui_fill_color");
}

void *getStackUserData(ItemStackBase *stack) {
  if (!stack)
    return nullptr;
  return *reinterpret_cast<void **>(
      reinterpret_cast<std::byte *>(stack) +
      bedrocktools::sdk::offsets::MapPreview::ItemStackBaseUserData);
}

void *treeFindNode(void *compound, const char *key, std::size_t length) {
  if (!compound || !nbtTreeFind)
    return nullptr;
  NbtTreeKey searchKey{key, length};
  auto *base = reinterpret_cast<std::byte *>(compound);
  void *treeRoot =
      base + bedrocktools::sdk::offsets::MapPreview::CompoundTagTreeRoot;
  void *treeEnd =
      base + bedrocktools::sdk::offsets::MapPreview::CompoundTagTreeEnd;
  void *node = nbtTreeFind(treeRoot, &searchKey);
  return node == treeEnd ? nullptr : node;
}

bool getMapUuid(void *compound, std::int64_t &uuid) {
  void *node = treeFindNode(compound, "map_uuid", 8);
  if (!node)
    return false;
  if (*reinterpret_cast<std::uint32_t *>(
          reinterpret_cast<std::byte *>(node) +
          bedrocktools::sdk::offsets::MapPreview::NbtNodeType) != 4)
    return false;
  uuid = *reinterpret_cast<std::int64_t *>(
      reinterpret_cast<std::byte *>(node) +
      bedrocktools::sdk::offsets::MapPreview::NbtNodeNumericValue);
  return true;
}

void clearSelection() {
  selectedRevision = 0;
  selectedInContainer = false;
  selectedAnchorX = 0.0f;
  selectedAnchorY = 0.0f;
  selectedAnchorValid = false;
  hasPixels = false;
  mapInfoRequested = false;
  pendingLoad = false;
  cachedLevel = nullptr;
  cachedDimension = nullptr;
  cachedMapUuid = 0;
  cachedRects.clear();
  lastRefresh = {};
}

mce::Color decodeColor(std::uint32_t abgr) {
  constexpr float ByteScale = 1.0f / 255.0f;
  return {static_cast<float>(abgr & 0xFF) * ByteScale,
          static_cast<float>((abgr >> 8) & 0xFF) * ByteScale,
          static_cast<float>((abgr >> 16) & 0xFF) * ByteScale,
          static_cast<float>((abgr >> 24) & 0xFF) * ByteScale};
}

void buildMapRects() {
  cachedRects.clear();
  if (cachedRects.capacity() < MapPixelCount)
    cachedRects.reserve(MapPixelCount);
  std::array<bool, MapPixelCount> visited{};
  for (int y = 0; y < MapHeight; ++y) {
    for (int x = 0; x < MapWidth; ++x) {
      std::size_t idx = static_cast<std::size_t>(y) * MapWidth + x;
      if (visited[idx])
        continue;
      std::uint32_t abgr = pixels[idx];
      mce::Color color = decodeColor(abgr);
      if (color.a <= 0.0f) {
        visited[idx] = true;
        continue;
      }
      int w = 1;
      while (x + w < MapWidth) {
        std::size_t ni = static_cast<std::size_t>(y) * MapWidth + x + w;
        if (visited[ni] || pixels[ni] != abgr)
          break;
        ++w;
      }
      int h = 1;
      while (y + h < MapHeight) {
        bool match = true;
        for (int dx = 0; dx < w; ++dx) {
          std::size_t ci =
              static_cast<std::size_t>(y + h) * MapWidth + x + dx;
          if (visited[ci] || pixels[ci] != abgr) {
            match = false;
            break;
          }
        }
        if (!match)
          break;
        ++h;
      }
      for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx)
          visited[static_cast<std::size_t>(y + dy) * MapWidth + x + dx] = true;
      cachedRects.push_back(
          {static_cast<float>(x), static_cast<float>(x + w),
           static_cast<float>(y), static_cast<float>(y + h), color.r, color.g,
           color.b, color.a});
    }
  }
}

bool copyMapPixels(void *mapData) {
  if (!mapData)
    return false;
  auto *base = reinterpret_cast<std::byte *>(mapData) +
               bedrocktools::sdk::offsets::MapPreview::MapItemSavedDataPixels;
  auto *begin = *reinterpret_cast<const std::uint32_t *const *>(base);
  auto *end =
      *reinterpret_cast<const std::uint32_t *const *>(base + sizeof(void *));
  if (!begin || !end)
    return false;
  std::uintptr_t beginAddress = reinterpret_cast<std::uintptr_t>(begin);
  std::uintptr_t endAddress = reinterpret_cast<std::uintptr_t>(end);
  constexpr std::size_t RequiredBytes = MapPixelCount * sizeof(std::uint32_t);
  if (endAddress < beginAddress || endAddress - beginAddress < RequiredBytes ||
      endAddress - beginAddress > RequiredBytes * 4)
    return false;
  std::copy_n(begin, MapPixelCount, pixels.begin());
  return true;
}

bool initSelection(ItemStackBase *stack, void *level, std::int64_t mapUuid) {
  void *userData = getStackUserData(stack);
  if (!userData || !level)
    return false;
  cachedLevel = level;
  cachedMapUuid = mapUuid;
  void *mapData = levelGetMapSavedDataByTag
                      ? levelGetMapSavedDataByTag(level, userData)
                      : nullptr;
  hasPixels = mapData && copyMapPixels(mapData);
  if (hasPixels)
    buildMapRects();
  pendingLoad = !hasPixels;
  if (levelRequestMapInfo && !mapInfoRequested) {
    levelRequestMapInfo(level, mapUuid, true);
    mapInfoRequested = true;
  }
  return true;
}

bool tryLoadPending(ItemStackBase *stack) {
  if (!cachedLevel || !stack || !levelGetMapSavedDataByTag)
    return false;
  void *userData = getStackUserData(stack);
  std::int64_t mapUuid = 0;
  if (!getMapUuid(userData, mapUuid) || mapUuid != cachedMapUuid) {
    clearSelection();
    return false;
  }
  void *mapData = levelGetMapSavedDataByTag(cachedLevel, userData);
  if (!mapData) {
    if (levelRequestMapInfo && !mapInfoRequested) {
      levelRequestMapInfo(cachedLevel, cachedMapUuid, true);
      mapInfoRequested = true;
    }
    return false;
  }
  const auto previousPixels = pixels;
  hasPixels = copyMapPixels(mapData);
  if (hasPixels) {
    if (cachedRects.empty() || pixels != previousPixels) buildMapRects();
    pendingLoad = false;
  }
  return hasPixels;
}

void emitQuad(void *tessellator, float x0, float y0, float x1, float y1,
              const mce::Color &color) {
  tessellatorColor(tessellator, color.r, color.g, color.b, color.a);
  tessellatorVertex(tessellator, x0, y0, 0.0f);
  tessellatorVertex(tessellator, x0, y1, 0.0f);
  tessellatorVertex(tessellator, x1, y1, 0.0f);
  tessellatorVertex(tessellator, x1, y0, 0.0f);
}

void drawMap(void *context, float x, float y, float scale) {
  if (!context || !tessellatorBegin || !tessellatorColor ||
      !tessellatorVertex || !renderMeshImmediately)
    return;
  ensureRenderMaterial();
  if (!uiFillMaterial)
    return;
  auto *contextBase = reinterpret_cast<std::byte *>(context);
  void *screenContext = *reinterpret_cast<void **>(
      contextBase +
      bedrocktools::sdk::offsets::MapPreview::MinecraftUIRenderContextScreenContext);
  if (!screenContext)
    return;
  auto *screenBase = reinterpret_cast<std::byte *>(screenContext);
  void *tessellator = *reinterpret_cast<void **>(
      screenBase + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
  if (!tessellator)
    return;

  bedrocktools::core::ScopedShaderColor shaderColor(screenContext);
  if (!shaderColor) return;
  std::size_t quadCount = cachedRects.size() + 2;
  int vertexCount = static_cast<int>(quadCount * 4);
  tessellatorBegin(tessellator, nullptr,
                   static_cast<int>(PrimitiveMode::QuadList), vertexCount, 0);

  float mapWidth = MapWidth * scale;
  float mapHeight = MapHeight * scale;
  emitQuad(tessellator, x, y, x + mapWidth + FramePadding * 2.0f,
           y + mapHeight + FramePadding * 2.0f,
           {0.08f, 0.07f, 0.05f, 1.0f});
  float originX = x + FramePadding;
  float originY = y + FramePadding;
  emitQuad(tessellator, originX, originY, originX + mapWidth,
           originY + mapHeight, {0.78f, 0.71f, 0.52f, 1.0f});
  for (const auto &rect : cachedRects) {
    emitQuad(tessellator, originX + rect.x0 * scale,
             originY + rect.y0 * scale, originX + rect.x1 * scale,
             originY + rect.y1 * scale,
             {rect.r, rect.g, rect.b, rect.a});
  }

  std::array<std::byte, bedrocktools::sdk::offsets::MapPreview::RenderMeshDataSize> renderData{};
  renderMeshImmediately(screenContext, tessellator, &uiFillMaterial, renderData.data());
}

bedrocktools::hooks::Handle hookVtable(const char *className, void **original, void *replacement, std::size_t slot) {
  const auto target = pl::memory::resolveVtableFunction(className, slot, MinecraftLibrary);
  if (!target)
    return nullptr;
  return bedrocktools::hooks::install(reinterpret_cast<void *>(target), replacement, original);
}

void renderHoverBoxHook(void *self, void *context, void *client, void *area, float value) {
  if (renderHoverBoxOriginal)
    renderHoverBoxOriginal(self, context, client, area, value);
  if (!self)
    return;
  auto *base = reinterpret_cast<std::byte *>(self);
  if (!moduleInstance || !moduleInstance->enabled) return;
  MapPreviewHandleHoverAnchor(
      *reinterpret_cast<float *>(base + bedrocktools::sdk::offsets::MapPreview::HoverRendererCursorX),
      *reinterpret_cast<float *>(base + bedrocktools::sdk::offsets::MapPreview::HoverRendererCursorY));
}

void renderPass(void* context, void*) {
  MapPreviewRender(context);
}

}

MapPreviewModule::MapPreviewModule()
    : Module("MapPreview", "Shows the selected filled map without holding it "
                           "in your main hand.") {
  moduleInstance = this;
}

MapPreviewModule::~MapPreviewModule() {
  onDisable();
  if (moduleInstance == this)
    moduleInstance = nullptr;
}

namespace {

bool installMapPreviewHooks() {
  if (hooksInstalled) return true;
  nbtTreeFind = reinterpret_cast<NbtTreeFindFn>(bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::NbtTreeFind));
  levelGetMapSavedDataByTag = reinterpret_cast<LevelGetMapSavedDataByTagFn>(
      bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LevelGetMapSavedDataByTag));
  levelRequestMapInfo = reinterpret_cast<LevelRequestMapInfoFn>(bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::LevelRequestMapInfo));
  tessellatorBegin = reinterpret_cast<TessellatorBeginFn>(bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::TessellatorBegin));
  tessellatorColor = reinterpret_cast<TessellatorColorFn>(bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::TessellatorColor));
  tessellatorVertex = reinterpret_cast<TessellatorVertexFn>(bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::TessellatorVertex));
  std::uintptr_t renderMesh = bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::MeshHelpersRenderMeshImmediately2);
  renderMeshImmediately = reinterpret_cast<RenderMeshImmediatelyFn>(renderMesh);
  std::uintptr_t materialGroupSignature = bedrocktools::memory::resolve(
      bedrocktools::memory::SignatureId::RenderMaterialGroupCommon);
  if (materialGroupSignature) {
    std::uintptr_t groupAddress =
        resolveAdrp(reinterpret_cast<std::uint32_t *>(materialGroupSignature), 2, 0);
    if (groupAddress)
      renderMaterialGroup =
          groupAddress + bedrocktools::sdk::offsets::MaterialGroup::mRenderMaterialGroupOffset;
  }
  renderHoverBoxHandle = hookVtable(
      "17HoverTextRenderer", reinterpret_cast<void **>(&renderHoverBoxOriginal),
      reinterpret_cast<void *>(renderHoverBoxHook),
      bedrocktools::sdk::offsets::VTable::HoverTextRendererRenderHoverBox);
  bedrocktools::core::preview::initialize();
  hooksInstalled = nbtTreeFind && levelGetMapSavedDataByTag
      && tessellatorBegin && tessellatorColor && tessellatorVertex && renderMeshImmediately
      && renderMaterialGroup && bedrocktools::core::uirender::subscribe(renderPass);
  if (!hooksInstalled) {
    if (renderHoverBoxHandle) bedrocktools::hooks::remove(renderHoverBoxHandle);
    renderHoverBoxHandle = nullptr;
    renderHoverBoxOriginal = nullptr;
  }
  return hooksInstalled;
}

void removeMapPreviewHooks() {
  if (renderHoverBoxHandle) bedrocktools::hooks::remove(renderHoverBoxHandle);
  renderHoverBoxHandle = nullptr;
  renderHoverBoxOriginal = nullptr;
  uiFillMaterial = {};
  renderMaterialGroup = 0;
  hooksInstalled = false;
}

}

void MapPreviewModule::onInit() {
  bedrocktools::core::preview::initialize();
}

void MapPreviewModule::onEnable() {
  installMapPreviewHooks();
}

void MapPreviewModule::onDisable() {
  clearSelection();
  removeMapPreviewHooks();
}

void MapPreviewModule::loadConfig(const nlohmann::json &j) {
  Module::loadConfig(j);
  if (j.contains("m_positionX"))
    m_positionX = std::clamp(j["m_positionX"].get<float>(), 0.0f, 2000.0f);
  if (j.contains("m_positionY"))
    m_positionY = std::clamp(j["m_positionY"].get<float>(), 0.0f, 2000.0f);
  if (j.contains("m_scale"))
    m_scale = std::clamp(j["m_scale"].get<float>(), 0.1f, 5.0f);
  if (j.contains("m_followSelectedMap"))
    m_followSelectedMap = j["m_followSelectedMap"].get<bool>();
}

void MapPreviewModule::saveConfig(nlohmann::json &j) {
  Module::saveConfig(j);
  j["m_positionX"] = m_positionX;
  j["m_positionY"] = m_positionY;
  j["m_scale"] = m_scale;
  j["m_followSelectedMap"] = m_followSelectedMap;
}

void MapPreviewHandleHoverAnchor(float x, float y) {
  if (!selectedInContainer || selectedAnchorValid || (!pendingLoad && !hasPixels))
    return;
  selectedAnchorX = x;
  selectedAnchorY = y;
  selectedAnchorValid = true;
}

void MapPreviewRender(void *context) {
  if (!moduleInstance || !moduleInstance->enabled || !context)
    return;
  const auto selection = bedrocktools::core::preview::current(context);
  auto* stack = static_cast<ItemStackBase*>(selection.stack);
  std::int64_t uuid = 0;
  if (!selection.level || !stack || !getMapUuid(getStackUserData(stack), uuid)) {
    clearSelection();
    return;
  }
  if (selection.revision != selectedRevision || selection.level != cachedLevel ||
      selection.dimension != cachedDimension || uuid != cachedMapUuid) {
    clearSelection();
    selectedRevision = selection.revision;
    selectedInContainer = selection.inContainer;
    cachedDimension = selection.dimension;
    initSelection(stack, selection.level, uuid);
  }
  if (!hasPixels && !pendingLoad) pendingLoad = true;
  const auto now = std::chrono::steady_clock::now();
  if (lastRefresh == std::chrono::steady_clock::time_point{} || now - lastRefresh >= std::chrono::milliseconds(250)) {
    lastRefresh = now;
    tryLoadPending(stack);
  }
  if (!hasPixels)
    return;
  float scale = std::clamp(moduleInstance->m_scale, 0.1f, 5.0f);
  float x = moduleInstance->m_positionX;
  float y = moduleInstance->m_positionY;
  if (moduleInstance->m_followSelectedMap && selectedAnchorValid) {
    x = std::max(0.0f, selectedAnchorX -
                           (MapWidth * scale + FramePadding * 2.0f) * 0.5f);
    y = std::max(0.0f, selectedAnchorY + AnchorYOffset);
  }
  struct Clip { float x0, x1, y0, y1; };
  void** table = getVtable(context);
  if (table && table[bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle]) {
    auto clip = reinterpret_cast<Clip (*)(void*)>(table[
        bedrocktools::sdk::offsets::VTable::MinecraftUIRenderContextGetFullClippingRectangle])(context);
    const float width = MapWidth * scale + FramePadding * 2.0f;
    const float height = MapHeight * scale + FramePadding * 2.0f;
    if (clip.x1 > clip.x0 && clip.y1 > clip.y0) {
      x = std::clamp(x, clip.x0, std::max(clip.x0, clip.x1 - width));
      y = std::clamp(y, clip.y0, std::max(clip.y0, clip.y1 - height));
    }
  }
  drawMap(context, x, y, scale);
}
