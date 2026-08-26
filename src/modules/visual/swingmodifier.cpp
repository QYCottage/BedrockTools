#include "swingmodifier.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

static SwingModifierModule* g_swingMod = nullptr;

SwingModifierModule::SwingModifierModule() : Module("Swing Modifier", "Modify ur swing like flux swing and the speed(visually)") {
    m_patched = false;
    m_renderFirstPersonHooked = false;
    m_getModifiedSwingDurationHooked = false;
    m_patchTarget = nullptr;
    m_patchTarget2 = nullptr;
    g_swingMod = this;
}

SwingModifierModule::~SwingModifierModule() {
    if (g_swingMod == this) g_swingMod = nullptr;
}

static void (*_renderFirstPerson_orig)(void*, void*, const void*, std::uint8_t) = nullptr;
static void _renderFirstPerson_hook(void* self, void* renderContext, const void* prevProj, std::uint8_t itemFlags) {
    if(g_swingMod) {
        if(g_swingMod->m_fluxSwing && g_swingMod->enabled) g_swingMod->applyPatch();
        else g_swingMod->removePatch();
    }
    _renderFirstPerson_orig(self, renderContext, prevProj, itemFlags);
}

static int (*_getModifiedSwingDuration_orig)(void*) = nullptr;
static int _getModifiedSwingDuration_hook(void* self) {
    if(!g_swingMod || !g_swingMod->enabled) return _getModifiedSwingDuration_orig(self);
    return g_swingMod->m_swingSpeed;
}

void SwingModifierModule::onInit() {
    uintptr_t renderFirstPerson = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ItemInHandRendererRenderFirstPerson);
    if (renderFirstPerson != 0) {
        if(!m_patchTarget){
            m_patchTarget = (void*)(renderFirstPerson + bedrocktools::sdk::offsets::ItemInHandRenderer::mRenderFirstPersonTransformPatchOffset1);
            m_patchTarget2 = (void*)(renderFirstPerson + bedrocktools::sdk::offsets::ItemInHandRenderer::mRenderFirstPersonTransformPatchOffset2);
            memcpy(m_originalBytes, m_patchTarget, 4);
            memcpy(m_originalBytes2, m_patchTarget2, 4);
        }
        if(!m_renderFirstPersonHooked){
            bedrocktools::hooks::install((void*)renderFirstPerson, (void*)_renderFirstPerson_hook, (void**)&_renderFirstPerson_orig);
            m_renderFirstPersonHooked = true;
        }
    }
    if(!m_getModifiedSwingDurationHooked){
        uintptr_t addr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MobGetModifiedSwingDuration);
        if (addr != 0) {
            bedrocktools::hooks::install((void*)addr, (void*)_getModifiedSwingDuration_hook, (void**)&_getModifiedSwingDuration_orig);
            m_getModifiedSwingDurationHooked = true;
        }
    }
}

void SwingModifierModule::applyPatch() {
    if (m_patched || !m_patchTarget) return;
    uint32_t nop = 0xD503201F; 
    bedrocktools::sdk::patchMemory(m_patchTarget, &nop, 4);
    bedrocktools::sdk::patchMemory(m_patchTarget2, &nop, 4);
    m_patched = true;
}

void SwingModifierModule::removePatch() {
    if (!m_patched || !m_patchTarget) return;
    bedrocktools::sdk::patchMemory(m_patchTarget, m_originalBytes, 4);
    bedrocktools::sdk::patchMemory(m_patchTarget2, m_originalBytes2, 4);
    m_patched = false;
}

void SwingModifierModule::onEnable() {
}

void SwingModifierModule::onDisable() {
    removePatch();
}

void SwingModifierModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_fluxSwing")) m_fluxSwing = j["m_fluxSwing"].get<bool>();
    if (j.contains("m_swingSpeed")) m_swingSpeed = j["m_swingSpeed"].get<int>();
}

void SwingModifierModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_fluxSwing"] = m_fluxSwing;
    j["m_swingSpeed"] = m_swingSpeed;
}
