#pragma once
#include "../Module.hpp"
#include <cstdint>

class ThirdPersonNametagModule : public Module {
private:
    bool m_patched;
    uint8_t m_originalBytes[4];
    void* m_patchTarget;

    void applyPatch();
    void removePatch();

public:
    ThirdPersonNametagModule();
    ~ThirdPersonNametagModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
};
