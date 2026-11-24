#include "ModuleRegistry.hpp"
#include "misc/hitsound.hpp"
#include "hud/effectdisplay.hpp"
#include "visual/hitbox.hpp"
#include "visual/blockoutline.hpp"
#include "visual/jumpcircle.hpp"
#include "visual/damagecircle.hpp"
#include "visual/hitrange.hpp"
#include "misc/commentkey.hpp"
#include "misc/commandhotkey.hpp"
#include "hud/crosshair.hpp"
#include <bedrocktools/modules/visual/wings.hpp>
#include "hud/inventoryhud.hpp"
#include "hud/armorhud.hpp"


ModuleRegistry& ModuleRegistry::get() {
    static ModuleRegistry registry;
    return registry;
}

Module* ModuleRegistry::find(std::string_view id) const {
    const auto it = mById.find(id);
    return it == mById.end() ? nullptr : it->second;
}

const std::vector<Module*>& ModuleRegistry::modules() const {
    return mView;
}

void ModuleRegistry::initialize() {
    if (mInitialized) return;
    for (auto* module : mView) module->onInit();
    mInitialized = true;
}

void ModuleRegistry::onFrame() {
    for (auto* module : mView) if (module->enabled) module->onFrame();
}

bool ModuleRegistry::onMouseEvent(int button, bool isDown) {
    bool consumed = false;
    for (auto* module : mView) if (module->onMouseEvent(button, isDown)) consumed = true;
    return consumed;
}

bool ModuleRegistry::onKeyEvent(int key, bool isDown) {
    bool consumed = false;
    for (auto* module : mView) if (module->onKeyEvent(key, isDown)) consumed = true;
    return consumed;
}

bool ModuleRegistry::onTouchEvent(float x, float y, bool isDown) {
    bool consumed = false;
    for (auto* module : mView) if (module->onTouchEvent(x, y, isDown)) consumed = true;
    return consumed;
}

void ModuleRegistry::setKeybindBlocked(bool blocked) {
    mKeybindBlocked = blocked;
}

bool ModuleRegistry::keybindBlocked() const {
    return mKeybindBlocked;
}

void registerAllModules() {
    auto& registry = ModuleRegistry::get();
    if (!registry.modules().empty()) return;
    registry.emplace<HitSoundModule>();
    registry.emplace<EffectDisplayModule>();
    registry.emplace<HitboxModule>();
    registry.emplace<BlockOutlineModule>();
    registry.emplace<CommentKey>();
    registry.emplace<CommandHotkeyModule>();
    registry.emplace<CrosshairModule>();
    registry.emplace<WingsModule>();
    registry.emplace<InventoryHudModule>();
    registry.emplace<ArmorModule>();
    registry.emplace<JumpCircleModule>();
    registry.emplace<DamageCircleModule>();
    registry.emplace<HitRangeModule>();
}
