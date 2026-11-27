#include "paperdoll.hpp"

#include "../../core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/ModMenu.hpp>
#include <pl/memory/Vtable.hpp>

#include <cstddef>
#include <cstdint>

namespace {

constexpr const char* MinecraftLibrary = "libminecraftpe.so";
constexpr float KeepAliveSeconds = 1.0f;

using HudPlayerRendererFn = void (*)(void*, void*, void*, void*, int);

HudPlayerRendererFn hudPlayerRendererOriginal = nullptr;
bedrocktools::hooks::Handle hudPlayerRendererHook = nullptr;
std::atomic<PaperDollModule*> g_instance{nullptr};

void hudPlayerRendererDetour(void* self, void* context, void* client, void* control, int pass) {
    if (auto* mod = g_instance.load(std::memory_order_acquire)) {
        mod->beforeRender(self);
    }
    if (hudPlayerRendererOriginal) {
        hudPlayerRendererOriginal(self, context, client, control, pass);
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
}

} // namespace

PaperDollModule::PaperDollModule()
    : Module("Paper Doll", "Keep the HUD paper doll visible to display your skin and armor") {
    g_instance.store(this, std::memory_order_release);
}

PaperDollModule::~PaperDollModule() {
    PaperDollModule* expected = this;
    g_instance.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void PaperDollModule::onInit() {
    installHook();
}

bool PaperDollModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "alwaysVisible") {
        setAlwaysVisible(value == "1" || value == "true");
        return true;
    }
    return false;
}

void PaperDollModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("alwaysVisible")) {
        setAlwaysVisible(j["alwaysVisible"].get<bool>());
    }
}

void PaperDollModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["alwaysVisible"] = alwaysVisible();
}

void PaperDollModule::beforeRender(void* renderer) const {
    if (!renderer || !enabled || !alwaysVisible()) return;
    auto* timeRemaining = reinterpret_cast<float*>(
        static_cast<std::byte*>(renderer) + bedrocktools::sdk::offsets::HudPlayerRenderer::mTimeRemaining);
    if (*timeRemaining <= 0.0f) {
        *timeRemaining = KeepAliveSeconds;
    }
}
