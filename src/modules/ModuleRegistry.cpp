#include "ModuleRegistry.hpp"
#include "visual/fullbright.hpp"
#include "visual/motionblur.hpp"
#include "visual/fpsunlocker.hpp"
#include "visual/fogcolor.hpp"
#include "visual/glintcolor.hpp"
#include "visual/tnttimer.hpp"
#include "visual/nofog.hpp"
#include "hud/pingcounter.hpp"
#include "hud/reachcounter.hpp"
#include "hud/breakindicator.hpp"
#include "hud/playercoords.hpp"
#include "hud/compass.hpp"
#include "player/timechanger.hpp"
#include "player/weatherchanger.hpp"
#include "player/nick.hpp"
#include "misc/nodisconnect.hpp"
#include "misc/chattimestamps.hpp"
#include "misc/notouchborder.hpp"
#include "misc/forceglobalrp.hpp"
#include "misc/cpslimiter.hpp"
#include "hud/speeddisplay.hpp"
#include "hud/effectdisplay.hpp"
#include "hud/debugmenu.hpp"
#include "visual/viewmodel.hpp"
#include "hud/keystrokes.hpp"
#include "visual/thirdpersonnametag.hpp"
#include "hud/tablist.hpp"
#include "hud/combocounter.hpp"
#include "visual/chunkborder.hpp"
#include "visual/blockoutline.hpp"
#include "visual/hitbox.hpp"
#include "visual/zoom.hpp"
#include "visual/breadcrumbs.hpp"
#include "visual/lightoverlay.hpp"
#include "visual/shulkerpreview.hpp"
#include "visual/connectedglass.hpp"
#include "player/skinstealer.hpp"
#include "player/autogg.hpp"
#include "player/autoreq.hpp"
#include "misc/commentkey.hpp"
#include "misc/commandhotkey.hpp"
#include "hud/crosshair.hpp"
#include "hud/mobhealthindicator.hpp"


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
    registry.emplace<FullbrightModule>();
    registry.emplace<MotionBlurModule>();
    registry.emplace<FogColorModule>();
    registry.emplace<GlintColorModule>();
    registry.emplace<TntTimerModule>();
    registry.emplace<NoFogModule>();
    registry.emplace<PingCounterModule>();
    registry.emplace<ReachCounterModule>();
    registry.emplace<ComboDisplay>();
    registry.emplace<BreakIndicatorModule>();
    registry.emplace<PlayerCoordsModule>();
    registry.emplace<CompassModule>();
    registry.emplace<TimeChangerModule>();
    registry.emplace<WeatherChangerModule>();
    registry.emplace<NickModule>();
    registry.emplace<NoDisconnectModule>();
    registry.emplace<ChatTimestampsModule>();
    registry.emplace<NoTouchBorderModule>();
    registry.emplace<CpsLimiterModule>();
    registry.emplace<SpeedDisplayModule>();
    registry.emplace<EffectDisplayModule>();
    registry.emplace<DebugMenuModule>();
    registry.emplace<ViewModelModule>();
    registry.emplace<KeystrokesModule>();
    registry.emplace<ThirdPersonNametagModule>();
    registry.emplace<TablistModule>();
    registry.emplace<ChunkBorderModule>();
    registry.emplace<BlockOutlineModule>();
    registry.emplace<HitboxModule>();
    registry.emplace<ZoomModule>();
    registry.emplace<BreadcrumbsModule>();
    registry.emplace<SkinStealerModule>();
    registry.emplace<AutoGG>();
    registry.emplace<AutoReQ>();
    registry.emplace<FPSUnlockerModule>();
    registry.emplace<LightOverlayModule>();
    registry.emplace<ShulkerPreviewModule>();
    registry.emplace<ConnectedGlassModule>();
    registry.emplace<ForceGlobalRPModule>();
    registry.emplace<CommentKey>();
    registry.emplace<CommandHotkeyModule>();
    registry.emplace<CrosshairModule>();
    registry.emplace<MobHealthIndicatorModule>();
}
