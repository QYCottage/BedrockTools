#pragma once

#include "../Module.hpp"

class ForceGlobalRPModule : public Module {
public:
    ForceGlobalRPModule();
    ~ForceGlobalRPModule() override;
    void onInit() override;
    void onEnable() override;
    void onDisable() override;

private:
    void initFunc1();
    void initFunc2();
    bool m_func1_hooked = false;
    void* m_func1Target = nullptr;
    bool m_func2_hooked = false;
    void* m_func2Target = nullptr;
};