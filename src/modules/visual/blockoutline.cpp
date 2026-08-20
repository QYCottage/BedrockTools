#include "blockoutline.hpp"
#include "blockoutline_geometry.hpp"

#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/Types.hpp>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

using TessellatorBegin = void (*)(void*, void*, int, int, int);
using TessellatorColor = void (*)(void*, float, float, float, float);
using TessellatorVertex = void (*)(void*, float, float, float);
using RenderMeshImmediately = void (*)(void*, void*, void*, char*);
using RenderLevel = void (*)(void*, void*, void*);
using LevelGetHitResult = void* (*)(void*);

BlockOutlineModule* g_module = nullptr;
TessellatorBegin g_tessellatorBegin = nullptr;
TessellatorColor g_tessellatorColor = nullptr;
TessellatorVertex g_tessellatorVertex = nullptr;
RenderMeshImmediately g_renderMesh = nullptr;
RenderLevel g_renderLevelOriginal = nullptr;
LevelGetHitResult g_getHitResult = nullptr;

std::mutex g_targetMutex;
bedrocktools::sdk::BlockPos g_target{};
bool g_hasTarget = false;

void clearTarget() {
    std::lock_guard lock(g_targetMutex);
    g_hasTarget = false;
}

void updateTarget(bedrocktools::sdk::Player* player) {
    if (!g_module || !g_module->enabled || !player || !g_getHitResult) {
        clearTarget();
        return;
    }

    const auto playerAddress = reinterpret_cast<std::uintptr_t>(player);
    void* level = *reinterpret_cast<void**>(
        playerAddress + bedrocktools::sdk::offsets::Actor::mLevel);
    void* hitResult = level ? g_getHitResult(level) : nullptr;
    if (!hitResult) {
        clearTarget();
        return;
    }

    const auto hitAddress = reinterpret_cast<std::uintptr_t>(hitResult);
    const int type = *reinterpret_cast<const int*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mType);
    if (type != bedrocktools::sdk::offsets::HitResult::TypeBlock) {
        clearTarget();
        return;
    }

    const auto position = *reinterpret_cast<const bedrocktools::sdk::BlockPos*>(
        hitAddress + bedrocktools::sdk::offsets::HitResult::mBlockPos);
    std::lock_guard lock(g_targetMutex);
    g_target = position;
    g_hasTarget = true;
}

void renderLevelHook(void* levelRenderer, void* screenContext, void* renderParams) {
    if (g_renderLevelOriginal) {
        g_renderLevelOriginal(levelRenderer, screenContext, renderParams);
    }

    if (!g_module || !g_module->enabled || !screenContext || !levelRenderer ||
        !g_tessellatorBegin || !g_tessellatorColor || !g_tessellatorVertex ||
        !g_renderMesh) {
        return;
    }

    bedrocktools::sdk::BlockPos target{};
    {
        std::lock_guard lock(g_targetMutex);
        if (!g_hasTarget) return;
        target = g_target;
    }

    const auto contextAddress = reinterpret_cast<std::uintptr_t>(screenContext);
    void* tessellator = *reinterpret_cast<void**>(
        contextAddress + bedrocktools::sdk::offsets::ScreenContext::mTessellator);
    float* colorHolder = *reinterpret_cast<float**>(
        contextAddress + bedrocktools::sdk::offsets::ScreenContext::mColorHolder);
    if (reinterpret_cast<std::uintptr_t>(tessellator) < 0x1000 ||
        reinterpret_cast<std::uintptr_t>(colorHolder) < 0x1000) {
        return;
    }

    const auto rendererAddress = reinterpret_cast<std::uintptr_t>(levelRenderer);
    void* playerRenderer = *reinterpret_cast<void**>(
        rendererAddress + bedrocktools::sdk::offsets::LevelRenderer::mLevelRendererPlayer);
    if (reinterpret_cast<std::uintptr_t>(playerRenderer) < 0x1000) return;

    const auto playerRendererAddress = reinterpret_cast<std::uintptr_t>(playerRenderer);
    const auto camera = *reinterpret_cast<const bedrocktools::sdk::Vec3*>(
        playerRendererAddress + bedrocktools::sdk::offsets::LevelRendererPlayer::mCamPos);
    // The game's own selection material is always initialized with the level
    // renderer and has the depth/blend state expected for a block outline.
    void* material = reinterpret_cast<void*>(
        playerRendererAddress +
        bedrocktools::sdk::offsets::LevelRendererPlayer::mSelectionOverlayMaterial);

    const float savedColor[4] = {
        colorHolder[0], colorHolder[1], colorHolder[2], colorHolder[3]
    };
    colorHolder[0] = 1.0f;
    colorHolder[1] = 1.0f;
    colorHolder[2] = 1.0f;
    colorHolder[3] = 1.0f;

    const std::uint32_t color = g_module->outlineColor;
    const float red = static_cast<float>((color >> 16) & 0xFFu) / 255.0f;
    const float green = static_cast<float>((color >> 8) & 0xFFu) / 255.0f;
    const float blue = static_cast<float>(color & 0xFFu) / 255.0f;
    const float alpha = static_cast<float>((color >> 24) & 0xFFu) / 255.0f;

    constexpr int kLinePrimitive = 4;
    const auto lines = bedrocktools::modules::blockoutline::makeBox(
        static_cast<float>(target.x),
        static_cast<float>(target.y),
        static_cast<float>(target.z));
    g_tessellatorBegin(tessellator, nullptr, kLinePrimitive,
                       static_cast<int>(lines.size() * 2), 0);
    g_tessellatorColor(tessellator, red, green, blue, alpha);
    for (const auto& line : lines) {
        g_tessellatorVertex(tessellator,
            line.from.x - camera.x, line.from.y - camera.y, line.from.z - camera.z);
        g_tessellatorVertex(tessellator,
            line.to.x - camera.x, line.to.y - camera.y, line.to.z - camera.z);
    }

    char meshParams[0x58];
    std::memset(meshParams, 0, sizeof(meshParams));
    g_renderMesh(screenContext, tessellator, material, meshParams);

    colorHolder[0] = savedColor[0];
    colorHolder[1] = savedColor[1];
    colorHolder[2] = savedColor[2];
    colorHolder[3] = savedColor[3];
}

} // namespace

BlockOutlineModule::BlockOutlineModule()
    : Module("Block Outline", "Draws a configurable outline around the block you are looking at.") {
    g_module = this;
}

BlockOutlineModule::~BlockOutlineModule() {
    if (g_module == this) g_module = nullptr;
}

void BlockOutlineModule::onInit() {
    using bedrocktools::memory::SignatureId;

    m_renderLevel = reinterpret_cast<void*>(
        bedrocktools::memory::resolve(SignatureId::RenderLevel));
    g_tessellatorBegin = reinterpret_cast<TessellatorBegin>(
        bedrocktools::memory::resolve(SignatureId::TessellatorBegin));
    g_tessellatorColor = reinterpret_cast<TessellatorColor>(
        bedrocktools::memory::resolve(SignatureId::TessellatorColor));
    g_tessellatorVertex = reinterpret_cast<TessellatorVertex>(
        bedrocktools::memory::resolve(SignatureId::TessellatorVertex));

    auto renderMeshAddress = bedrocktools::memory::resolve(
        SignatureId::MeshHelpersRenderMeshImmediately2);
    if (!renderMeshAddress) {
        renderMeshAddress = bedrocktools::memory::resolve(
            SignatureId::MeshHelpersRenderMeshImmediately);
    }
    g_renderMesh = reinterpret_cast<RenderMeshImmediately>(renderMeshAddress);
    g_getHitResult = reinterpret_cast<LevelGetHitResult>(
        bedrocktools::memory::resolve(SignatureId::LevelGetHitResult));

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) { updateTarget(event.player); });
}

void BlockOutlineModule::installRenderHook() {
    if (m_hookInstalled || !m_renderLevel) return;
    const auto handle = bedrocktools::hooks::install(
        m_renderLevel,
        reinterpret_cast<void*>(renderLevelHook),
        reinterpret_cast<void**>(&g_renderLevelOriginal));
    m_hookInstalled = handle != nullptr;
}

void BlockOutlineModule::onEnable() {
    installRenderHook();
}

void BlockOutlineModule::onDisable() {
    clearTarget();
}

void BlockOutlineModule::loadConfig(const nlohmann::json& json) {
    Module::loadConfig(json);
    if (!json.contains("outlineColor") || !json["outlineColor"].is_string()) return;

    std::string value = json["outlineColor"].get<std::string>();
    if (!value.empty() && value.front() == '#') value.erase(value.begin());
    try {
        const auto parsed = std::stoul(value, nullptr, 16);
        // Six-digit colors are RGB; eight-digit colors are AARRGGBB.
        outlineColor = value.size() <= 6
            ? (0xFF000000u | static_cast<std::uint32_t>(parsed))
            : static_cast<std::uint32_t>(parsed);
    } catch (...) {
        // Preserve the last valid color.
    }
}

void BlockOutlineModule::saveConfig(nlohmann::json& json) {
    Module::saveConfig(json);
    char color[12]{};
    std::snprintf(color, sizeof(color), "#%08X", outlineColor);
    json["outlineColor"] = std::string(color);
}
