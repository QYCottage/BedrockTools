#include "thirdpersonnametag.hpp"

#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>

#include <cstring>

ThirdPersonNametagModule::ThirdPersonNametagModule()
    : Module("Third Person Nametag", "Shows your own nametag in third person view.") {
    m_patched = false;
    m_patchTarget = nullptr;
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
}
