#pragma once

#include "../Module.hpp"

class NoDisconnectModule : public Module {
public:
    NoDisconnectModule();
    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    static void setRuntimeActive(bool state);
};
