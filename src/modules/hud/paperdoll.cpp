#include "paperdoll.hpp"

#include "../../core/memory/Hooks.hpp"
#include <bedrocktools/events/AttackEvent.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/ScreenStateEvent.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Vtable.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char* MinecraftLibrary = "libminecraftpe.so";
constexpr float KeepAliveSeconds = 1.0f;
constexpr std::int64_t CombatWindowMs = 5000;
constexpr float DegToRad = 3.14159265358979323846f / 180.0f;
constexpr float AutoSpinDegPerMs = 0.09f; // ~90 deg/sec

using HudPlayerRendererFn = void (*)(void*, void*, void*, void*, int);
using MatrixRotateFn = void (*)(void*, const void*, float, const float*);

HudPlayerRendererFn hudPlayerRendererOriginal = nullptr;
bedrocktools::hooks::Handle hudPlayerRendererHook = nullptr;
MatrixRotateFn matrixRotateOriginal = nullptr;
bedrocktools::hooks::Handle matrixRotateHook = nullptr;
std::atomic<PaperDollModule*> g_instance{nullptr};

thread_local bool g_inPaperDollRender = false;
thread_local bool g_appliedRotationThisFrame = false;

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void matrixRotateDetour(void* out, const void* in, float angle, const float* axis) {
    if (!matrixRotateOriginal) return;
    matrixRotateOriginal(out, in, angle, axis);

    if (g_inPaperDollRender && !g_appliedRotationThisFrame) {
        if (auto* mod = g_instance.load(std::memory_order_acquire)) {
            const float extraDeg = mod->effectiveRotationDegrees();
            if (std::fabs(extraDeg) > 0.01f) {
                g_appliedRotationThisFrame = true;
                alignas(16) std::byte temp[64]{};
                std::memcpy(temp, out, sizeof(temp));
                const float yAxis[3] = {0.0f, 1.0f, 0.0f};
                matrixRotateOriginal(out, temp, extraDeg * DegToRad, yAxis);
            }
        }
    }
}

void hudPlayerRendererDetour(void* self, void* context, void* client, void* control, int pass) {
    PaperDollModule::ControlState state{};
    auto* mod = g_instance.load(std::memory_order_acquire);
    if (mod && !mod->beforeRender(self, control, state)) {
        return;
    }
    g_inPaperDollRender = true;
    g_appliedRotationThisFrame = false;
    if (hudPlayerRendererOriginal) {
        hudPlayerRendererOriginal(self, context, client, control, pass);
    }
    g_inPaperDollRender = false;
    if (mod) {
        mod->afterRender(control, state);
    }
}

void installHook() {
    if (hudPlayerRendererHook) return;
    const std::uintptr_t target = pl::memory::resolveVtableFunction(
        "17HudPlayerRenderer",
        bedrocktools::sdk::offsets::VTable::HudPlayerRendererRender,
        MinecraftLibrary);
    if (!target) return;
    hudPlayerRendererHook = bedrocktools::hooks::install(
        reinterpret_cast<void*>(target),
        reinterpret_cast<void*>(hudPlayerRendererDetour),
        reinterpret_cast<void**>(&hudPlayerRendererOriginal));

    if (!matrixRotateHook && target > bedrocktools::sdk::offsets::HudPlayerRenderer::RenderRva) {
        const std::uintptr_t libBase = target - bedrocktools::sdk::offsets::HudPlayerRenderer::RenderRva;
        const std::uintptr_t rotateAddr = libBase + bedrocktools::sdk::offsets::HudPlayerRenderer::MatrixRotateRva;
        matrixRotateHook = bedrocktools::hooks::install(
            reinterpret_cast<void*>(rotateAddr),
            reinterpret_cast<void*>(matrixRotateDetour),
            reinterpret_cast<void**>(&matrixRotateOriginal));
    }
}

bool parseBool(std::string_view value) {
    return value == "1" || value == "true" || value == "True";
}

float parseFloat(std::string_view value, float fallback) {
    std::string s(value);
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    return (end != s.c_str() && std::isfinite(v)) ? v : fallback;
}

int parseInt(std::string_view value, int fallback) {
    std::string s(value);
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    return (end != s.c_str()) ? static_cast<int>(v) : fallback;
}

} // namespace

PaperDollModule::PaperDollModule()
    : Module("Paper Doll", "Customize the HUD paper doll visibility, position, scale, and rotation") {
    g_instance.store(this, std::memory_order_release);
}

PaperDollModule::~PaperDollModule() {
    PaperDollModule* expected = this;
    g_instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void PaperDollModule::onInit() {
    installHook();

    bedrocktools::events::bus().subscribe<bedrocktools::events::ScreenStateEvent>(
        [this](const bedrocktools::events::ScreenStateEvent& event) {
            onScreenStateChanged(event.phase == bedrocktools::events::ScreenPhase::Opened);
        });

    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>(
        [this](const bedrocktools::events::AttackEvent&) {
            notifyCombatActivity();
        });
}

void PaperDollModule::onDisable() {
    pl::modmenu::submitHudEditorElements(moduleId, {});
}

void PaperDollModule::onFrame() {
    if (!enabled) return;
    const float sc = scale();
    pl::modmenu::HudEditorElement element;
    element.elementId = "paper_doll";
    element.displayName = "Paper Doll";
    element.positionKeyX = "hudPosX";
    element.positionKeyY = "hudPosY";
    element.x = hudPosX();
    element.y = hudPosY();
    element.width = std::max(24.0f, 48.0f * sc);
    element.height = std::max(32.0f, 64.0f * sc);
    std::vector<pl::modmenu::HudEditorElement> elements;
    elements.push_back(std::move(element));
    pl::modmenu::submitHudEditorElements(moduleId, elements);
}

void PaperDollModule::setHudPos(float x, float y) {
    m_hudPosX.store(std::clamp(x, 0.0f, 4000.0f), std::memory_order_relaxed);
    m_hudPosY.store(std::clamp(y, 0.0f, 4000.0f), std::memory_order_relaxed);
    m_useHudPos.store(true, std::memory_order_relaxed);
}

void PaperDollModule::setOffset(float x, float y) {
    m_offsetX.store(std::clamp(x, -400.0f, 400.0f), std::memory_order_relaxed);
    m_offsetY.store(std::clamp(y, -400.0f, 400.0f), std::memory_order_relaxed);
}

void PaperDollModule::setScale(float s) {
    m_scale.store(std::clamp(s, 0.25f, 3.0f), std::memory_order_relaxed);
}

void PaperDollModule::setRotation(float deg) {
    m_rotation.store(std::clamp(deg, -180.0f, 180.0f), std::memory_order_relaxed);
}

float PaperDollModule::effectiveRotationDegrees() const {
    float deg = rotation();
    if (autoSpin()) {
        deg = std::fmod(deg + static_cast<float>(nowMs()) * AutoSpinDegPerMs, 360.0f);
    }
    return deg;
}

void PaperDollModule::notifyCombatActivity() {
    m_lastCombatMs.store(nowMs(), std::memory_order_relaxed);
}

void PaperDollModule::onScreenStateChanged(bool opened) {
    if (opened) {
        m_screenDepth.fetch_add(1, std::memory_order_relaxed);
    } else {
        int cur = m_screenDepth.load(std::memory_order_relaxed);
        while (cur > 0 && !m_screenDepth.compare_exchange_weak(cur, cur - 1, std::memory_order_relaxed)) {
        }
    }
}

bool PaperDollModule::isCombatActive() const {
    const auto last = m_lastCombatMs.load(std::memory_order_relaxed);
    return last > 0 && (nowMs() - last) <= CombatWindowMs;
}

bool PaperDollModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "hudPosX") {
        setHudPos(parseFloat(value, hudPosX()), hudPosY());
        return true;
    }
    if (key == "hudPosY") {
        setHudPos(hudPosX(), parseFloat(value, hudPosY()));
        return true;
    }
    if (key == "alwaysVisible") {
        setAlwaysVisible(parseBool(value));
        return true;
    }
    if (key == "mode") {
        int m = std::clamp(parseInt(value, 0), 0, 2);
        setMode(static_cast<VisibilityMode>(m));
        return true;
    }
    if (key == "offsetX") {
        setOffset(parseFloat(value, offsetX()), offsetY());
        return true;
    }
    if (key == "offsetY") {
        setOffset(offsetX(), parseFloat(value, offsetY()));
        return true;
    }
    if (key == "scale") {
        setScale(parseFloat(value, scale()));
        return true;
    }
    if (key == "rotation") {
        setRotation(parseFloat(value, rotation()));
        return true;
    }
    if (key == "autoSpin") {
        setAutoSpin(parseBool(value));
        return true;
    }
    if (key == "hideInContainer") {
        setHideInContainer(parseBool(value));
        return true;
    }
    return false;
}

void PaperDollModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("mode")) {
        int m = std::clamp(j["mode"].get<int>(), 0, 2);
        setMode(static_cast<VisibilityMode>(m));
    } else if (j.contains("alwaysVisible")) {
        setAlwaysVisible(j["alwaysVisible"].get<bool>());
    }
    if (j.contains("hudPosX") || j.contains("hudPosY")) {
        float x = j.contains("hudPosX") ? j["hudPosX"].get<float>() : hudPosX();
        float y = j.contains("hudPosY") ? j["hudPosY"].get<float>() : hudPosY();
        setHudPos(x, y);
    }
    if (j.contains("offsetX") || j.contains("offsetY")) {
        float x = j.contains("offsetX") ? j["offsetX"].get<float>() : offsetX();
        float y = j.contains("offsetY") ? j["offsetY"].get<float>() : offsetY();
        setOffset(x, y);
    }
    if (j.contains("scale")) {
        setScale(j["scale"].get<float>());
    }
    if (j.contains("rotation")) {
        setRotation(j["rotation"].get<float>());
    }
    if (j.contains("autoSpin")) {
        setAutoSpin(j["autoSpin"].get<bool>());
    }
    if (j.contains("hideInContainer")) {
        setHideInContainer(j["hideInContainer"].get<bool>());
    }
}

void PaperDollModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX();
    j["hudPosY"] = hudPosY();
    j["mode"] = static_cast<int>(mode());
    j["alwaysVisible"] = alwaysVisible();
    j["offsetX"] = offsetX();
    j["offsetY"] = offsetY();
    j["scale"] = scale();
    j["rotation"] = rotation();
    j["autoSpin"] = autoSpin();
    j["hideInContainer"] = hideInContainer();
}

void PaperDollModule::beforeRender(void* renderer) const {
    ControlState unused{};
    beforeRender(renderer, nullptr, unused);
}

bool PaperDollModule::beforeRender(void* renderer, void* control, ControlState& state) const {
    if (!renderer || !enabled) return true;

    if (hiddenByScreen()) {
        return false;
    }

    auto* timeRemaining = reinterpret_cast<float*>(
        static_cast<std::byte*>(renderer) + bedrocktools::sdk::offsets::HudPlayerRenderer::mTimeRemaining);

    const auto currentMode = mode();
    if (currentMode == VisibilityMode::Always) {
        if (*timeRemaining <= 0.0f) {
            *timeRemaining = KeepAliveSeconds;
        }
    } else if (currentMode == VisibilityMode::CombatOnly) {
        if (isCombatActive() && *timeRemaining <= 0.0f) {
            *timeRemaining = KeepAliveSeconds;
        }
    }

    if (control) {
        const bool useHud = m_useHudPos.load(std::memory_order_relaxed);
        const float ox = offsetX();
        const float oy = offsetY();
        if (useHud || ox != 0.0f || oy != 0.0f) {
            auto* pos = reinterpret_cast<float*>(
                static_cast<std::byte*>(control) + bedrocktools::sdk::offsets::UIControl::mPosition);
            state.origPosX = pos[0];
            state.origPosY = pos[1];
            state.modifiedPos = true;
            if (useHud) {
                pos[0] = hudPosX() + ox;
                pos[1] = hudPosY() + oy;
            } else {
                pos[0] += ox;
                pos[1] += oy;
            }
        }

        const float sc = scale();
        if (sc != 1.0f) {
            auto* sz = reinterpret_cast<float*>(
                static_cast<std::byte*>(control) + bedrocktools::sdk::offsets::UIControl::mSize);
            state.origSizeX = sz[0];
            state.origSizeY = sz[1];
            state.modifiedSize = true;
            sz[0] *= sc;
            sz[1] *= sc;
        }
    }

    return true;
}

void PaperDollModule::afterRender(void* control, const ControlState& state) const {
    if (!control) return;
    if (state.modifiedPos) {
        auto* pos = reinterpret_cast<float*>(
            static_cast<std::byte*>(control) + bedrocktools::sdk::offsets::UIControl::mPosition);
        pos[0] = state.origPosX;
        pos[1] = state.origPosY;
    }
    if (state.modifiedSize) {
        auto* sz = reinterpret_cast<float*>(
            static_cast<std::byte*>(control) + bedrocktools::sdk::offsets::UIControl::mSize);
        sz[0] = state.origSizeX;
        sz[1] = state.origSizeY;
    }
}
