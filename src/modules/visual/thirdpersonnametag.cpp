#include "thirdpersonnametag.hpp"
#include "nametagicon_assets.hpp"
#include "nametagiconlayout.hpp"

#include "core/GameHooks.hpp"
#include "modules/ModuleRegistry.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <pl/ModMenu.hpp>
#include <EGL/egl.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Minimal base64 decoder for the embedded launcher-icon pixels. Runs once
// when the icon is registered; mirrors the decoder used by the Effect Display
// module so only this compact table lives in the binary.
bool decodeBase64(const char* encoded, std::size_t length, std::vector<unsigned char>& out) {
    static constexpr std::array<std::int8_t, 256> kBase64Table = [] {
        std::array<std::int8_t, 256> table{};
        table.fill(-1);
        constexpr char alphabet[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) {
            table[static_cast<unsigned char>(alphabet[i])] = static_cast<std::int8_t>(i);
        }
        return table;
    }();

    out.clear();
    out.reserve(length / 4 * 3);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const char ch = encoded[i];
        if (ch == '=') break;  // tolerate padding even though our data has none
        const auto value = kBase64Table[static_cast<unsigned char>(ch)];
        if (value < 0) return false;
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((accumulator >> bits) & 0xFF));
        }
    }
    return true;
}

constexpr float kPi = 3.14159265f;

// Nominal vertical field of view used for the head projection. Minecraft's
// default perspective is ~70 degrees; a small inaccuracy only changes how
// strongly the icon drifts with the camera, not where it is aimed, and the
// head sits near the screen center in third person anyway.
constexpr float kVerticalFov = 70.0f * kPi / 180.0f;

// The HUD overlay coordinate convention: values <= -19000 are interpreted by
// the launcher relative to the screen center, with -20000 being the exact
// center (the same convention the Crosshair and Tablist modules rely on).
constexpr float kHudCenter = -20000.0f;

// Gap between the top of the player's head and the vertical center of the
// nametag line the game draws (and therefore of the icon next to it).
// Bedrock renders the nametag billboard centred roughly 0.5 blocks above the
// top of the bounding box.  The previous value (0.45) placed the projected
// anchor slightly below the real text centre, making the icon sit lower than
// the name on screen.
constexpr float kNametagAnchorOffset = 0.5f;

// Assumed name length when the player's name cannot be read, so the icon still
// lands roughly where a typical nametag starts instead of on top of the name.
constexpr int kFallbackNameLength = 8;

} // namespace

ThirdPersonNametagModule::ThirdPersonNametagModule()
    : Module("Third Person Nametag", "Shows your own nametag in third person view.") {
    m_patched = false;
    m_patchTarget = nullptr;
    // The icon is anchored to the world, not to a draggable HUD slot.
    hideInHudEditor = true;
}

ThirdPersonNametagModule::~ThirdPersonNametagModule() {
    removePatch();
}

void ThirdPersonNametagModule::onInit() {
    if (!m_patchTarget) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::Nametag);
        if (addr != 0) {
            m_patchTarget = (void*)(addr + bedrocktools::sdk::offsets::NameTag::mExtractNameTagsPatchOffset);
            memcpy(m_originalBytes, m_patchTarget, 4);
        }
    }

    if (!m_getLocalPlayer) {
        uintptr_t fn = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientInstanceGetLocalPlayer);
        if (fn) m_getLocalPlayer = reinterpret_cast<GetLocalPlayerFn>(fn);
    }
}

void ThirdPersonNametagModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    uint32_t nop = 0xD503201F;
    bedrocktools::sdk::patchMemory(m_patchTarget, &nop, 4);
    m_patched = true;
}

void ThirdPersonNametagModule::removePatch() {
    if (!m_patched || !m_patchTarget) return;
    bedrocktools::sdk::patchMemory(m_patchTarget, m_originalBytes, 4);
    m_patched = false;
}

void ThirdPersonNametagModule::onEnable() {
    applyPatch();
}

void ThirdPersonNametagModule::onDisable() {
    removePatch();
    // Clear any leftover icon; onFrame stops running for disabled modules.
    ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
}

void ThirdPersonNametagModule::registerResources() {
    if (m_resourcesRegistered) return;

    std::vector<unsigned char> pixels;
    if (decodeBase64(bedrocktools::nametag::kNametagIconBase64,
                     std::strlen(bedrocktools::nametag::kNametagIconBase64),
                     pixels) &&
        pixels.size() == static_cast<std::size_t>(bedrocktools::nametag::kNametagIconSize *
                                                  bedrocktools::nametag::kNametagIconSize * 4)) {
        pl::modmenu::registerImage(bedrocktools::nametag::kNametagIconImageId,
                                   pixels,
                                   bedrocktools::nametag::kNametagIconSize,
                                   bedrocktools::nametag::kNametagIconSize);
        m_resourcesRegistered = true;
    }
}

int ThirdPersonNametagModule::localNameLength(bedrocktools::sdk::Player* player) const {
    if (!player) return kFallbackNameLength;

    // Player::mName is the name the client paints above the head. Guard the
    // read: a wrong offset on an unsupported game build would otherwise feed
    // a nonsense width into the layout.
    const std::string& name = player->name();
    if (name.empty() || name.size() > 256) return kFallbackNameLength;

    const int visible = bedrocktools::nametag::visibleNameLength(std::string_view(name));
    return visible > 0 ? visible : kFallbackNameLength;
}

void ThirdPersonNametagModule::onFrame() {
    if (!enabled || !m_nametagIcon) {
        ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    auto* client = reinterpret_cast<bedrocktools::sdk::ClientInstance*>(
        bedrocktools::core::gamehooks::clientInstance());
    auto* renderer = client ? client->levelRenderer() : nullptr;
    auto* playerRenderer = renderer ? renderer->playerRenderer() : nullptr;
    auto* player = (client && m_getLocalPlayer) ? m_getLocalPlayer(client) : nullptr;
    if (!client || !playerRenderer || !player) {
        ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    const bedrocktools::sdk::Vec3 camPos = playerRenderer->cameraPosition();
    const bedrocktools::sdk::Vec3 feet = player->position();

    // Third-person detection: the camera is meaningfully away from the eye.
    // In first person the camera sits inside the head and the icon is hidden
    // exactly like the vanilla nametag would be.
    const float eyeY = feet.y + 1.62f;
    const float dx = camPos.x - feet.x;
    const float dy = camPos.y - eyeY;
    const float dz = camPos.z - feet.z;
    if (dx * dx + dy * dy + dz * dz < 0.35f * 0.35f) {
        ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    // Screen dimensions for the NDC -> pixel mapping. onFrame runs inside the
    // eglSwapBuffers detour, so the EGL context and draw surface are current.
    EGLDisplay display = eglGetCurrentDisplay();
    EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
    if (display == EGL_NO_DISPLAY || surface == EGL_NO_SURFACE) return;
    EGLint width = 0, height = 0;
    eglQuerySurface(display, surface, EGL_WIDTH, &width);
    eglQuerySurface(display, surface, EGL_HEIGHT, &height);
    if (width <= 0 || height <= 0) return;

    // Anchor: the nametag line the game draws above the head (fallback to a
    // nominal height when the AABB component is unavailable).
    const auto bounds = player->bounds();
    const float headTop = (bounds.max.y > bounds.min.y) ? bounds.max.y : (feet.y + 1.8f);
    const bedrocktools::sdk::Vec3 anchor = {feet.x, headTop + kNametagAnchorOffset, feet.z};

    // Camera basis. The follow camera looks at the player, so the forward
    // vector is simply the direction from the camera to the eye.
    float fx = feet.x - camPos.x;
    float fy = eyeY - camPos.y;
    float fz = feet.z - camPos.z;
    const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (flen < 1e-4f) return;
    fx /= flen; fy /= flen; fz /= flen;

    // right = forward x worldUp (0,1,0) = (-fz, 0, fx).
    float rx = -fz, ry = 0.0f, rz = fx;
    float rlen = std::sqrt(rx * rx + rz * rz);
    if (rlen < 1e-4f) { rx = 1.0f; ry = 0.0f; rz = 0.0f; rlen = 1.0f; }
    rx /= rlen; rz /= rlen;

    // up = right x forward.
    const float ux = ry * fz - rz * fy;
    const float uy = rz * fx - rx * fz;
    const float uz = rx * fy - ry * fx;

    // Camera-space position of the anchor.
    const float relx = anchor.x - camPos.x;
    const float rely = anchor.y - camPos.y;
    const float relz = anchor.z - camPos.z;
    const float camX = relx * rx + rely * ry + relz * rz;
    const float camY = relx * ux + rely * uy + relz * uz;
    const float camZ = relx * fx + rely * fy + relz * fz;
    if (camZ <= 0.01f) {  // behind the camera
        ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    // Perspective divide to normalized device coordinates.
    const float t = std::tan(kVerticalFov * 0.5f);
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const float ndcX = camX / (camZ * t * aspect);
    const float ndcY = camY / (camZ * t);

    const float px = (ndcX + 1.0f) * 0.5f * static_cast<float>(width);
    const float py = (1.0f - ndcY) * 0.5f * static_cast<float>(height);

    registerResources();

    // The nametag is a world-space billboard: its on-screen size follows the
    // distance to the camera, so the icon is measured against the same
    // projection instead of a fixed pixel size. `camZ` is the depth along the
    // view axis, which is exactly what the perspective divide above used.
    const float textHeightPx =
        bedrocktools::nametag::nametagTextHeightPx(camZ, static_cast<float>(height), kVerticalFov);
    if (textHeightPx <= 0.0f) {
        ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{});
        return;
    }

    bedrocktools::nametag::IconPlacement placement;
    placement.nameCenterX = px;
    placement.nameCenterY = py;
    placement.textHeightPx = textHeightPx;
    placement.nameLength = localNameLength(player);
    placement.scale = m_nametagIconScale;

    // Sits immediately to the left of the name, vertically centered on it.
    const bedrocktools::nametag::IconRect rect =
        bedrocktools::nametag::iconRectLeftOfName(placement);

    // Screen-center-relative HUD coordinates.
    const float hudX = kHudCenter + (rect.x - static_cast<float>(width) * 0.5f);
    const float hudY = kHudCenter + (rect.y - static_cast<float>(height) * 0.5f);

    PLModMenu_DrawCommand icon{};
    icon.type = PL_DRAW_IMAGE;
    icon.x = hudX;
    icon.y = hudY;
    icon.w = rect.w;
    icon.h = rect.h;
    icon.color = 0xFFFFFFFF;
    icon.imageId = bedrocktools::nametag::kNametagIconImageId;

    ::submitDrawCommands(moduleId, std::vector<PLModMenu_DrawCommand>{icon});
}

void ThirdPersonNametagModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_nametagIcon")) m_nametagIcon = j["m_nametagIcon"].get<bool>();
    // "m_iconScale" is the pre-rename key; keep reading it so existing configs
    // do not silently lose the size the player picked.
    if (j.contains("m_iconScale")) m_nametagIconScale = j["m_iconScale"].get<float>();
    if (j.contains("m_nametagIconScale")) m_nametagIconScale = j["m_nametagIconScale"].get<float>();
}

void ThirdPersonNametagModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_nametagIcon"] = m_nametagIcon;
    j["m_nametagIconScale"] = m_nametagIconScale;
}
