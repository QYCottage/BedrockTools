#pragma once

#include "../Module.hpp"
#include <string>

class QuickLootModule : public Module {
public:
    QuickLootModule();
    void onInit() override;

private:
    using HandleAutoPlaceFn = void (*)(void*, int, const std::string&, int);

    HandleAutoPlaceFn m_handleAutoPlace = nullptr;
    bool m_transferring = false;
};
