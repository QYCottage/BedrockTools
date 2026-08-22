#include "zoom.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <pl/ModMenu.hpp>
#include <cmath>
#include <algorithm>

// Zoom is applied through the camera FOV: a smaller FOV means a larger zoom.
// The scroll gesture on the zoom button adjusts the target FOV between these
// bounds. kZoomFovMax matches the vanilla FOV so zooming out can never widen
// the view beyond normal (i.e. "no zoom").
namespace {
constexpr float kZoomFovMin = 5.0f;
constexpr float kZoomFovMax = 70.0f;
} // namespace

static ZoomModule* g_zoomMod = nullptr;

static float (*_getFov_orig)(void*, float, int) = nullptr;

static float _getFov_zoom_hook(void* _this, float a, int enableVariableFOV) {
    float originalFov = 0.0f;
    if (_getFov_orig)
        originalFov = _getFov_orig(_this, a, enableVariableFOV);
    
    if (!g_zoomMod) return originalFov;

    if (originalFov == 70.0f || originalFov == 60.0f) {
        return originalFov;
    }

    g_zoomMod->m_baseFov = originalFov;

    if (g_zoomMod->m_isFirstTime) {
        g_zoomMod->m_currentFov = originalFov;
        g_zoomMod->m_isFirstTime = false;
    }

    if (g_zoomMod->isZoomActive()) {
        g_zoomMod->m_animationFinished = false;
        g_zoomMod->m_currentFov = std::lerp(g_zoomMod->m_currentFov, g_zoomMod->m_targetZoomFov, g_zoomMod->m_animSpeed);
        return g_zoomMod->m_currentFov;
    } else {
        if (!g_zoomMod->m_animationFinished) {
            g_zoomMod->m_currentFov = std::lerp(g_zoomMod->m_currentFov, originalFov, g_zoomMod->m_animSpeed);
            if (std::abs(g_zoomMod->m_currentFov - originalFov) < 0.5f) {
                g_zoomMod->m_animationFinished = true;
                g_zoomMod->m_currentFov = originalFov;
            }
            return g_zoomMod->m_currentFov;
        }
    }

    return originalFov;
}


struct Vec2 { float x, y; };
static void (*_applyTurnDelta_orig)(void*, Vec2*) = nullptr;

static void _applyTurnDelta_hook(void* _this, Vec2* rotationDelta) {
    if (g_zoomMod && (g_zoomMod->isZoomActive() || !g_zoomMod->m_animationFinished) && g_zoomMod->m_lowSens && g_zoomMod->m_baseFov > 0.1f) {
        float zoomRatio = g_zoomMod->m_currentFov / g_zoomMod->m_baseFov;
        float strength = g_zoomMod->m_lowSensStrength;
        float multiplier = 1.0f - (1.0f - zoomRatio) * strength;
        multiplier = std::max(0.01f, std::min(1.0f, multiplier));
        
        Vec2 modifiedDelta = { rotationDelta->x * multiplier, rotationDelta->y * multiplier };
        if (_applyTurnDelta_orig)
            _applyTurnDelta_orig(_this, &modifiedDelta);
    } else {
        if (_applyTurnDelta_orig)
            _applyTurnDelta_orig(_this, rotationDelta);
    }
}


static bool (*_getHideItemInHand_orig)(void*) = nullptr;

static bool _getHideItemInHand_hook(void* _this) {
    bool hide = false;
    if (_getHideItemInHand_orig)
        hide = _getHideItemInHand_orig(_this);
    
    if (g_zoomMod && g_zoomMod->isZoomActive() && g_zoomMod->m_hideHand) {
        return true;
    }
    
    return hide;
}

ZoomModule::ZoomModule() 
    : Module("Zoom", "Smoothly zooms your camera like OptiFine.") {
    this->keybind = 0;
    g_zoomMod = this;
}

ZoomModule::~ZoomModule() {
    if (g_zoomMod == this) g_zoomMod = nullptr;
}

void ZoomModule::onInit() {
    if (!m_fovHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GetFov);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_getFov_zoom_hook, (void**)&_getFov_orig);
            m_fovHooked = true;
        }
    }
    
    if (!m_turnDeltaHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LocalPlayerApplyTurnDelta);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_applyTurnDelta_hook, (void**)&_applyTurnDelta_orig);
            m_turnDeltaHooked = true;
        }
    }
    
    if (!m_hideHandHooked) {
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::BaseOptionRegistryGetHideItemInHand);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_getHideItemInHand_hook, (void**)&_getHideItemInHand_orig);
            m_hideHandHooked = true;
        }
    }
    
    updateZoomButton();
}

void ZoomModule::onEnable() {
    m_isFirstTime = true;
    m_animationFinished = false;
}

void ZoomModule::onDisable() {
    m_animationFinished = false;
    m_keyZooming = false;
    m_buttonZooming = false;
}

bool ZoomModule::isZoomActive() {
    if (!enabled) return false;
    return m_keyZooming || m_buttonZooming;
}

void ZoomModule::onKeybindEvent(const std::string& key, bool isDown) {
    if (key == "keybind") {
        if (isDown && !m_keyZooming) {
            m_isFirstTime = true;
            m_animationFinished = false;
        }
        m_keyZooming = isDown;
    }
}

void ZoomModule::adjustZoomByScroll(float delta) {
    // Positive scroll (drag / wheel up) zooms in by lowering the target FOV;
    // negative scroll zooms out by raising it. The result is clamped so you
    // never zoom past the minimum FOV or widen beyond the vanilla FOV.
    m_targetZoomFov -= delta * m_scrollSensitivity;
    m_targetZoomFov = std::clamp(m_targetZoomFov, kZoomFovMin, kZoomFovMax);
}

void ZoomModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_targetZoomFov")) m_targetZoomFov = j["m_targetZoomFov"].get<float>();
    if (j.contains("m_animSpeed")) m_animSpeed = j["m_animSpeed"].get<float>();
    if (j.contains("m_scrollSensitivity")) m_scrollSensitivity = j["m_scrollSensitivity"].get<float>();
    if (j.contains("m_lowSens")) m_lowSens = j["m_lowSens"].get<bool>();
    if (j.contains("m_lowSensStrength")) m_lowSensStrength = j["m_lowSensStrength"].get<float>();
    if (j.contains("m_hideHand")) m_hideHand = j["m_hideHand"].get<bool>();
    if (j.contains("m_overlayToggle")) m_overlayToggle = j["m_overlayToggle"].get<bool>();
    
    updateZoomButton();
}

void ZoomModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_targetZoomFov"] = m_targetZoomFov;
    j["m_animSpeed"] = m_animSpeed;
    j["m_scrollSensitivity"] = m_scrollSensitivity;
    j["m_lowSens"] = m_lowSens;
    j["m_lowSensStrength"] = m_lowSensStrength;
    j["m_hideHand"] = m_hideHand;
    j["m_overlayToggle"] = m_overlayToggle;
}

static const char* zoomDisabledSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
    <path fill="#373737" d="M28,14 A14,14 0 1,1 28,42 A14,14 0 1,1 28,14 Z M28,20 A8,8 0 1,0 28,36 A8,8 0 1,0 28,20 Z M38,34 L52,48 L48,52 L34,38 Z M27,24 H29 V27 H32 V29 H29 V32 H27 V29 H24 V27 H27 Z"/>
</svg>)svg";

static const char* zoomEnabledSvg = R"svg(<svg viewBox="0 0 64 64" xmlns="http://www.w3.org/2000/svg">
    <path fill="#C6C6C6" stroke="#373737" stroke-width="2" d="M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z"/>
    <g transform="translate(32, 32) scale(0.85) translate(-32, -32)">
        <path fill="#8B8B8B" stroke="#5B5B5B" stroke-width="2" d="M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z"/>
        <path fill="#373737" d="M28,14 A14,14 0 1,1 28,42 A14,14 0 1,1 28,14 Z M28,20 A8,8 0 1,0 28,36 A8,8 0 1,0 28,20 Z M38,34 L52,48 L48,52 L34,38 Z M27,24 H29 V27 H32 V29 H29 V32 H27 V29 H24 V27 H27 Z"/>
    </g>
</svg>)svg";

void ZoomModule::updateZoomButton() {
    if (m_overlayToggle) {
        pl::modmenu::ButtonBuilder("bedrocktools.Zoom.Button", "Zoom")
                .moduleId("bedrocktools.Zoom")
                // Hold-to-zoom: zoom is active only while the button is held,
                // and stops as soon as it is released.
                .behavior(pl::modmenu::ButtonBehavior::Hold)
                .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
                .styleColors(0x00000001, 0x00000001, 0x00000001)
                .svgIcon(zoomDisabledSvg)
                .activeSvgIcon(zoomEnabledSvg)
                .onEvent([this](std::string_view buttonId, pl::modmenu::ButtonEvent event, float value) {
                    (void)buttonId;
                    // Hold mode: zoom starts as soon as the button is pressed
                    // and stops as soon as it is released.
                    if (event == pl::modmenu::ButtonEvent::Down) {
                        if (!this->m_buttonZooming) {
                            this->m_buttonZooming = true;
                            this->m_isFirstTime = true;
                            this->m_animationFinished = false;
                        }
                    } else if (event == pl::modmenu::ButtonEvent::Up) {
                        this->m_buttonZooming = false;
                    } else if (event == pl::modmenu::ButtonEvent::Scroll) {
                        // While held, dragging/scrolling up zooms in and down
                        // zooms out by adjusting the target FOV.
                        if (this->m_buttonZooming)
                            this->adjustZoomByScroll(value);
                    }
                })
                .registerButton();
    } else {
        pl::modmenu::unregisterButton("bedrocktools.Zoom.Button");
    }
}
