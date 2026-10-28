#include "freelook.hpp"
#include "motionblur.hpp"

#include "core/GameHooks.hpp"
#include "core/Runtime.hpp"
#include "core/memory/Hooks.hpp"
#include "launcher/OverlayBridge.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/client/ClientInstance.hpp>
#include <pl/Logger.hpp>
#include <pl/ModMenu.hpp>
#include <array>
#include <cstring>
#include <mutex>

namespace {

using namespace bedrocktools;
using UpdatePlayerFn = void (*)(void*, void*, sdk::Player*);
using TurnDeltaFn = void (*)(sdk::Player*, sdk::Vec2*);
using PerspectiveFn = int (*)(void*);
using HeadRotationGetFn = sdk::Vec2 (*)(sdk::Player*);
using HeadRotationSetFn = void (*)(sdk::Player*, float, float);

FreelookModule* moduleInstance = nullptr;
UpdatePlayerFn updatePlayerOriginal = nullptr;
TurnDeltaFn turnDeltaOriginal = nullptr;
PerspectiveFn perspectiveOriginal = nullptr;
HeadRotationGetFn getHeadRotation = nullptr;
HeadRotationSetFn setHeadRotation = nullptr;
hooks::Handle updatePlayerHook = nullptr;
hooks::Handle turnDeltaHook = nullptr;
hooks::Handle perspectiveHook = nullptr;
events::Subscription tickSubscription = 0;
events::Subscription postTickSubscription = 0;
std::recursive_mutex stateMutex;

struct InputComponent {
    std::array<std::byte, 0x54> padding;
    sdk::Vec3 cameraOrientation;
    std::uint16_t flags;
};

static_assert(offsetof(InputComponent, cameraOrientation) == 0x54);
static_assert(offsetof(InputComponent, flags) == 0x60);
constexpr std::uint16_t movementFlags = (1u << 9) | (1u << 10);

InputComponent* moveInput(sdk::Player* player) {
    if (!player) return nullptr;
    auto* context = player->entityContext();
    auto* registry = sdk::field<void*>(context, 0x8);
    if (!registry) return nullptr;
    const auto buckets = sdk::field<std::uintptr_t>(registry, 0x38);
    const auto bucketsEnd = sdk::field<std::uintptr_t>(registry, 0x40);
    const auto nodes = sdk::field<std::uintptr_t>(registry, 0x50);
    const auto nodesEnd = sdk::field<std::uintptr_t>(registry, 0x58);
    if (!buckets || !nodes || bucketsEnd <= buckets || nodesEnd <= nodes) return nullptr;
    const auto count = (bucketsEnd - buckets) / sizeof(std::uintptr_t);
    if (!count || (count & (count - 1))) return nullptr;
    constexpr std::uint32_t hash = 0x018b1887;
    const auto nodeCount = (nodesEnd - nodes) / 0x20;
    auto index = sdk::field<std::uintptr_t>(reinterpret_cast<void*>(buckets),
        ((count - 1) & hash) * sizeof(std::uintptr_t));
    void* pool = nullptr;
    for (std::uintptr_t remaining = nodeCount; remaining && index < nodeCount; --remaining) {
        auto* node = reinterpret_cast<void*>(nodes + index * 0x20);
        if (sdk::field<std::uint32_t>(node, 0x8) == hash) {
            pool = sdk::field<void*>(node, 0x10);
            break;
        }
        index = sdk::field<std::uintptr_t>(node, 0);
    }
    if (!pool) return nullptr;
    const auto entity = sdk::field<std::uint32_t>(context, 0x10);
    const auto sparse = sdk::field<std::uintptr_t>(pool, 0x8);
    const auto sparseEnd = sdk::field<std::uintptr_t>(pool, 0x10);
    const auto sparsePage = (entity & 0x3ffffu) >> 11;
    if (!sparse || sparseEnd <= sparse || sparsePage >= (sparseEnd - sparse) / sizeof(void*)) return nullptr;
    auto* page = sdk::field<void*>(reinterpret_cast<void*>(sparse), sparsePage * sizeof(void*));
    if (!page) return nullptr;
    const auto packed = sdk::field<std::uint32_t>(page, (entity & 0x7ffu) * sizeof(std::uint32_t));
    if ((packed ^ (entity & 0xfffc0000u)) > 0x3fffeu) return nullptr;
    const auto payload = sdk::field<std::uintptr_t>(pool, 0x50);
    const auto payloadEnd = sdk::field<std::uintptr_t>(pool, 0x58);
    const auto payloadPage = (packed & 0x3ffffu) >> 7;
    if (!payload || payloadEnd <= payload || payloadPage >= (payloadEnd - payload) / sizeof(void*)) return nullptr;
    auto* storage = sdk::field<void*>(reinterpret_cast<void*>(payload), payloadPage * sizeof(void*));
    return storage ? reinterpret_cast<InputComponent*>(static_cast<char*>(storage) + (packed & 0x7fu) * 0x64) : nullptr;
}

struct Session {
    sdk::Player* player = nullptr;
    void* level = nullptr;
    void* dimension = nullptr;
    void* camera = nullptr;
    std::array<float, 4> rotation{};
    sdk::Vec2 headRotation{};
    std::array<float, 4> orientation{};
    sdk::Vec3 inputOrientation{};
    bool hasInput = false;
    std::uint16_t inputFlags = 0;
};

Session session;
Session lastCamera;

bool hooksReady() {
    return updatePlayerHook && turnDeltaHook && perspectiveHook && getHeadRotation && setHeadRotation;
}

sdk::Player* localPlayer() {
    auto* client = static_cast<sdk::ClientInstance*>(core::gamehooks::clientInstance());
    if (!client) return nullptr;
    using GetPlayerFn = sdk::Player* (*)(sdk::ClientInstance*);
    auto getPlayer = reinterpret_cast<GetPlayerFn>(memory::resolve(memory::SignatureId::ClientInstanceGetLocalPlayer));
    return getPlayer ? getPlayer(client) : nullptr;
}

bool samePlayer(sdk::Player* player) {
    return player && session.player == player && session.level == player->level() &&
        session.dimension == player->dimension();
}

void restorePlayer(sdk::Player* player) {
    if (!samePlayer(player)) return;
    if (auto* rotation = player->rotationComponent()) {
        std::memcpy(rotation, session.rotation.data(), sizeof(session.rotation));
    }
    setHeadRotation(player, session.headRotation.x, session.headRotation.y);
}

void restoreInput(sdk::Player* player, bool finishing) {
    if (!samePlayer(player) || !session.hasInput) return;
    if (auto* input = moveInput(player)) {
        input->cameraOrientation = session.inputOrientation;
        input->flags = static_cast<std::uint16_t>((input->flags & ~movementFlags) |
            (finishing ? session.inputFlags : 0));
    }
}

void discardSession(sdk::Player* player) {
    if (session.player == player && session.hasInput) {
        if (auto* input = moveInput(player)) {
            input->flags = static_cast<std::uint16_t>((input->flags & ~movementFlags) | session.inputFlags);
        }
    }
    session = {};
    if (!player || lastCamera.player != player || lastCamera.level != player->level() ||
        lastCamera.dimension != player->dimension()) lastCamera = {};
}

bool beginSession(sdk::Player* player) {
    if (!player || player != localPlayer() || !hooksReady()) return false;
    if (samePlayer(player)) return true;
    discardSession(player);
    auto* rotation = player->rotationComponent();
    if (!rotation) return false;
    session.player = player;
    session.level = player->level();
    session.dimension = player->dimension();
    std::memcpy(session.rotation.data(), rotation, sizeof(session.rotation));
    session.headRotation = getHeadRotation(player);
    if (lastCamera.player == player && lastCamera.level == session.level &&
        lastCamera.dimension == session.dimension) {
        session.camera = lastCamera.camera;
        session.orientation = lastCamera.orientation;
    }
    if (auto* input = moveInput(player)) {
        session.hasInput = true;
        session.inputOrientation = input->cameraOrientation;
        session.inputFlags = input->flags & movementFlags;
    }
    return true;
}

bool requested() {
    return moduleInstance && hooksReady() && moduleInstance->requested() &&
        !ModuleRegistry::get().keybindBlocked();
}

void updatePlayerDetour(void* camera, void* component, sdk::Player* player) {
    std::lock_guard lock(stateMutex);
    if (!updatePlayerOriginal) return;
    if (player != localPlayer()) {
        updatePlayerOriginal(camera, component, player);
        return;
    }
    if (requested() && camera && beginSession(player)) {
        if (session.camera != camera) {
            session.camera = camera;
            std::memcpy(session.orientation.data(),
                static_cast<char*>(camera) + sdk::offsets::CameraComponent::mOrientation,
                sizeof(session.orientation));
        }
        restorePlayer(player);
        restoreInput(player, false);
        MotionBlurCaptureCamera(camera, player);
        return;
    }
    if (samePlayer(player)) {
        restorePlayer(player);
        restoreInput(player, true);
        if (camera && session.camera == camera) {
            std::memcpy(static_cast<char*>(camera) + sdk::offsets::CameraComponent::mOrientation,
                session.orientation.data(), sizeof(session.orientation));
        }
        session = {};
    }
    updatePlayerOriginal(camera, component, player);
    MotionBlurCaptureCamera(camera, player);
    if (camera && player) {
        lastCamera.player = player;
        lastCamera.level = player->level();
        lastCamera.dimension = player->dimension();
        lastCamera.camera = camera;
        std::memcpy(lastCamera.orientation.data(),
            static_cast<char*>(camera) + sdk::offsets::CameraComponent::mOrientation,
            sizeof(lastCamera.orientation));
    }
}

void turnDeltaDetour(sdk::Player* player, sdk::Vec2* delta) {
    std::lock_guard lock(stateMutex);
    if (!turnDeltaOriginal) return;
    const bool active = requested() && delta && beginSession(player);
    if (!active && samePlayer(player)) {
        restorePlayer(player);
        restoreInput(player, true);
    }
    turnDeltaOriginal(player, delta);
    if (active) {
        restorePlayer(player);
        restoreInput(player, false);
    }
}

int perspectiveDetour(void* self) {
    std::lock_guard lock(stateMutex);
    const int perspective = perspectiveOriginal ? perspectiveOriginal(self) : 0;
    auto* player = localPlayer();
    return requested() && beginSession(player) ? 1 : perspective;
}

void synchronizePlayer(sdk::Player* player) {
    std::lock_guard lock(stateMutex);
    if (!player || (session.player && !samePlayer(player))) {
        discardSession(player);
        if (moduleInstance) moduleInstance->resetInput();
        return;
    }
    if (requested() && beginSession(player)) {
        restorePlayer(player);
        restoreInput(player, false);
    } else if (samePlayer(player)) {
        restorePlayer(player);
        restoreInput(player, true);
        if (!session.camera) session = {};
    }
}

constexpr const char* buttonId = "bedrocktools.Freelook.Button";
constexpr const char* inactiveSvg = R"svg(<svg viewBox="0 0 512 512" xmlns="http://www.w3.org/2000/svg">
<path fill="#000000" d="M23 0h466v23h-466ZM0 23h47v24h-47ZM465 23h47v24h-47ZM0 47h23v23h-23ZM489 47h23v23h-23ZM0 70h23v23h-23ZM489 70h23v23h-23ZM0 93h23v279h-23ZM489 93h23v279h-23ZM0 372h23v24h-23ZM489 372h23v24h-23ZM0 396h23v23h-23ZM489 396h23v23h-23ZM0 419h23v23h-23ZM489 419h23v23h-23ZM0 442h23v23h-23ZM489 442h23v23h-23ZM0 465h47v24h-47ZM465 465h47v24h-47ZM23 489h466v23h-466Z"/>
<path fill="#686868" d="M47 23h418v24h-418ZM23 47h466v23h-466ZM23 70h70v23h-70ZM419 70h70v23h-70ZM23 93h47v279h-47ZM442 93h47v279h-47ZM23 372h70v24h-70ZM419 372h70v24h-70ZM23 396h466v23h-466ZM47 419h418v23h-418Z"/>
<path fill="#808080" d="M93 70h326v23h-326ZM70 93h372v279h-372ZM93 372h326v24h-326Z"/>
<path fill="#505051" d="M23 419h24v23h-24ZM23 442h24v23h-24Z"/>
<path fill="#121213" d="M465 419h24v23h-24ZM465 442h24v23h-24Z"/>
<path fill="#39393C" d="M47 442h418v23h-418ZM47 465h418v24h-418Z"/>
<g fill="#000000" transform="translate(0 0)">
<path fill="none" stroke="#000000" stroke-width="24" stroke-linecap="square" d="M332 159A106 106 0 1 0 332 307"/>
<path d="M360 280L314 285L349 326Z"/>
<g transform="translate(0 -12)">
<path fill-rule="evenodd" d="M184 205H211L228 185H285L301 205H328V285H184ZM256 218a27 27 0 1 0 0 54a27 27 0 1 0 0-54Z"/>
<circle cx="256" cy="245" r="12"/>
</g>
</g>
</svg>)svg";
constexpr const char* activeSvg = R"svg(<svg viewBox="0 0 512 512" xmlns="http://www.w3.org/2000/svg">
<path fill="#000000" d="M23 23h466v24h-466ZM0 47h47v23h-47ZM465 47h47v23h-47ZM0 70h23v23h-23ZM489 70h23v23h-23ZM0 93h23v23h-23ZM489 93h23v23h-23ZM0 116h23v280h-23ZM489 116h23v280h-23ZM0 396h23v23h-23ZM489 396h23v23h-23ZM0 419h23v23h-23ZM489 419h23v23h-23ZM0 442h23v23h-23ZM489 442h23v23h-23ZM0 465h47v24h-47ZM465 465h47v24h-47ZM23 489h466v23h-466Z"/>
<path fill="#A8A8A8" d="M47 47h418v23h-418ZM23 70h466v23h-466ZM23 93h70v23h-70ZM419 93h70v23h-70ZM23 116h47v280h-47ZM442 116h47v280h-47ZM23 396h70v23h-70ZM419 396h70v23h-70ZM23 419h466v23h-466ZM47 442h418v23h-418Z"/>
<path fill="#808080" d="M93 93h326v23h-326ZM70 116h372v280h-372ZM93 396h326v23h-326Z"/>
<path fill="#505051" d="M23 442h24v23h-24Z"/>
<path fill="#121213" d="M465 442h24v23h-24Z"/>
<path fill="#39393C" d="M47 465h418v24h-418Z"/>
<g fill="#E6E6E6" transform="translate(0 23)">
<path fill="none" stroke="#E6E6E6" stroke-width="24" stroke-linecap="square" d="M332 159A106 106 0 1 0 332 307"/>
<path d="M360 280L314 285L349 326Z"/>
<g transform="translate(0 -12)">
<path fill-rule="evenodd" d="M184 205H211L228 185H285L301 205H328V285H184ZM256 218a27 27 0 1 0 0 54a27 27 0 1 0 0-54Z"/>
<circle cx="256" cy="245" r="12"/>
</g>
</g>
</svg>)svg";

}

FreelookModule::FreelookModule()
    : Module("Freelook", "Look around without changing your player's facing or movement direction.") {
    moduleInstance = this;
}

FreelookModule::~FreelookModule() {
    onDisable();
    pl::modmenu::unregisterButton(buttonId);
    if (tickSubscription) events::bus().unsubscribe(tickSubscription);
    if (postTickSubscription) events::bus().unsubscribe(postTickSubscription);
    tickSubscription = 0;
    postTickSubscription = 0;
    hooks::remove(perspectiveHook);
    hooks::remove(turnDeltaHook);
    hooks::remove(updatePlayerHook);
    perspectiveHook = nullptr;
    turnDeltaHook = nullptr;
    updatePlayerHook = nullptr;
    if (moduleInstance == this) moduleInstance = nullptr;
}

void FreelookModule::onInit() {
    updateButton();
    if (hooksReady()) return;
    const auto update = memory::resolve(memory::SignatureId::UpdatePlayerFromCamera);
    const auto turn = memory::resolve(memory::SignatureId::LocalPlayerApplyTurnDelta);
    const auto perspective = memory::resolve(memory::SignatureId::GetPerspective);
    getHeadRotation = reinterpret_cast<HeadRotationGetFn>(memory::resolve(memory::SignatureId::ActorGetHeadRotation));
    setHeadRotation = reinterpret_cast<HeadRotationSetFn>(memory::resolve(memory::SignatureId::ActorSetHeadRotation));
    if (!update || !turn || !perspective || !getHeadRotation || !setHeadRotation) {
        pl::log::Logger::getOrCreate("BedrockTools").error(
            "Freelook signatures missing: camera={}, turn={}, perspective={}, headGet={}, headSet={}",
            update, turn, perspective, getHeadRotation != nullptr, setHeadRotation != nullptr);
        return;
    }
    updatePlayerHook = hooks::install(reinterpret_cast<void*>(update), reinterpret_cast<void*>(updatePlayerDetour),
        reinterpret_cast<void**>(&updatePlayerOriginal));
    turnDeltaHook = hooks::install(reinterpret_cast<void*>(turn), reinterpret_cast<void*>(turnDeltaDetour),
        reinterpret_cast<void**>(&turnDeltaOriginal));
    perspectiveHook = hooks::install(reinterpret_cast<void*>(perspective), reinterpret_cast<void*>(perspectiveDetour),
        reinterpret_cast<void**>(&perspectiveOriginal));
    if (!hooksReady()) {
        pl::log::Logger::getOrCreate("BedrockTools").error("Freelook hook installation failed");
        hooks::remove(perspectiveHook);
        hooks::remove(turnDeltaHook);
        hooks::remove(updatePlayerHook);
        perspectiveHook = nullptr;
        turnDeltaHook = nullptr;
        updatePlayerHook = nullptr;
        updatePlayerOriginal = nullptr;
        turnDeltaOriginal = nullptr;
        perspectiveOriginal = nullptr;
        return;
    }
    tickSubscription = events::bus().subscribe<events::LocalPlayerPreTickEvent>([](auto& event) {
        synchronizePlayer(event.player);
    }, events::EventPriority::Last);
    postTickSubscription = events::bus().subscribe<events::LocalPlayerTickEvent>([](auto& event) {
        synchronizePlayer(event.player);
    }, events::EventPriority::Last);
}

void FreelookModule::onEnable() {
    onInit();
    resetInput();
    m_available.store(hooksReady(), std::memory_order_release);
    launcher::setFreelookOverlayVisible(m_overlayToggle && m_buttonRegistered);
}

void FreelookModule::onDisable() {
    m_available.store(false, std::memory_order_release);
    resetInput();
    launcher::setFreelookOverlayVisible(false);
}

bool FreelookModule::requested() const {
    return m_available.load(std::memory_order_acquire) &&
        (m_buttonActive.load(std::memory_order_acquire) || m_keyActive.load(std::memory_order_acquire));
}

void FreelookModule::resetInput() {
    if (m_buttonActive.exchange(false, std::memory_order_acq_rel)) launcher::resetFreelookOverlay();
    m_keyActive.store(false, std::memory_order_release);
}

void FreelookModule::onFrame() {
    auto* player = localPlayer();
    if (!player || ModuleRegistry::get().keybindBlocked()) resetInput();
    if (!player) {
        std::lock_guard lock(stateMutex);
        discardSession(nullptr);
    }
}

void FreelookModule::onKeybindEvent(const std::string& key, bool isDown) {
    if (key == "keybind") m_keyActive.store(isDown, std::memory_order_release);
}

void FreelookModule::onMenuRegistered() {
    updateButton();
}

void FreelookModule::loadConfig(const nlohmann::json& j) {
    auto config = j;
    config["keybindActive"] = true;
    if (config.contains("m_overlayToggle") && config["m_overlayToggle"].is_boolean()) {
        m_overlayToggle = config["m_overlayToggle"].get<bool>();
    }
    Module::loadConfig(config);
    updateButton();
}

void FreelookModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_overlayToggle"] = m_overlayToggle;
}

void FreelookModule::updateButton() {
    if (!m_overlayToggle) {
        pl::modmenu::unregisterButton(buttonId);
        m_buttonRegistered = false;
        m_buttonActive.store(false, std::memory_order_release);
        launcher::setFreelookOverlayVisible(false);
        return;
    }
    if (m_buttonRegistered) {
        launcher::setFreelookOverlayVisible(enabled);
        return;
    }
    m_buttonRegistered = pl::modmenu::ButtonBuilder(buttonId, "Freelook")
        .moduleId(moduleId)
        .modId(core::Runtime::get().modId())
        .label("F")
        .behavior(pl::modmenu::ButtonBehavior::Toggle)
        .defaultVisible(true)
        .stylePreset(pl::modmenu::ButtonStylePreset::Keycap)
        .styleColors(0x00000001, 0x00000001, 0x00000001)
        .sizeScale(1.0f, 1.0f)
        .svgIcon(inactiveSvg)
        .activeSvgIcon(activeSvg)
        .onEvent([this](std::string_view, pl::modmenu::ButtonEvent event, float value) {
            if (event == pl::modmenu::ButtonEvent::StateChanged) {
                m_buttonActive.store(value > 0.5f && m_available.load(std::memory_order_acquire),
                    std::memory_order_release);
            }
        })
        .registerButton();
    if (!m_buttonRegistered) {
        pl::log::Logger::getOrCreate("BedrockTools").error("Freelook overlay button registration failed");
    }
    launcher::setFreelookOverlayVisible(enabled && m_buttonRegistered);
}
