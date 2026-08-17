#include "keystrokes.hpp"
#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Types.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <entt/entt.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using uint = uint32_t;
using ushort = uint16_t;
using uchar = unsigned char;

enum class EntityId : uint32_t {};

template <size_t N, typename T>
struct bitset {
    T value;
    void set(size_t index, bool v) {
        if (v) value |= (1ULL << index);
        else value &= ~(1ULL << index);
    }
    bool test(size_t index) const {
        return (value & (1ULL << index)) != 0;
    }
};

struct EntityIdTraits {
    using value_type = EntityId;
    using entity_type = uint32_t;
    using version_type = uint16_t;
    static constexpr uint32_t entity_mask = 0x3FFFF;
    static constexpr uint32_t version_mask = 0x3FFF;
};

template<>
struct entt::entt_traits<EntityId> : entt::basic_entt_traits<EntityIdTraits> {
    static constexpr std::size_t page_size = ENTT_SPARSE_PAGE;
};

struct MoveInputState {
    bitset<27, uint> mFlagValues;
    bedrocktools::sdk::Vec2 mAnalogMoveVector;
    uchar mLookSlightDirField;
    uchar mLookNormalDirField;
    uchar mLookSmoothDirField;
    uchar pad[1];
};

struct MoveInputComponent {
    MoveInputState mInputState;
    MoveInputState mRawInputState;
    uchar mHoldAutoJumpInWaterTicks;
    uchar pad[3];
    bedrocktools::sdk::Vec2 mMove;
    bedrocktools::sdk::Vec2 mLookDelta;
    bedrocktools::sdk::Vec2 mInteractDir;
    bedrocktools::sdk::Vec3 mDisplacement;
    bedrocktools::sdk::Vec3 mDisplacementDelta;
    bedrocktools::sdk::Vec3 mCameraOrientation;
    bitset<11, ushort> mFlagValues;
    std::array<bool, 2> mIsPaddling;
};

class EntityRegistry;

class EntityContext {
public:
    inline entt::basic_registry<EntityId>& getRegistry() { return mEnTTRegistry; }

    template <class T>
    inline T* tryGetComponent() {
        return getRegistry().try_get<T>(mEntity);
    }

    EntityRegistry& mRegistry;
    entt::basic_registry<EntityId>& mEnTTRegistry;
    EntityId const mEntity;
};

static void keystrokesHSVtoRGB(float h, float s, float v, float& out_r, float& out_g, float& out_b) {
    if (s == 0.0f) {
        out_r = out_g = out_b = v;
        return;
    }
    h = std::fmod(h, 1.0f) * 6.0f;
    int i = static_cast<int>(std::floor(h));
    float f = h - static_cast<float>(i);
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    switch (i) {
        case 0: out_r = v; out_g = t; out_b = p; break;
        case 1: out_r = q; out_g = v; out_b = p; break;
        case 2: out_r = p; out_g = v; out_b = t; break;
        case 3: out_r = p; out_g = q; out_b = v; break;
        case 4: out_r = t; out_g = p; out_b = v; break;
        default: out_r = v; out_g = p; out_b = q; break;
    }
}

static KeystrokesModule* g_keystrokesMod = nullptr;

// ---------------------------------------------------------------------------
// Mobile touch attack/build button support.
//
// The mobile touch buttons never produce mouse events, so the LMB/RMB keys
// are driven from the same GameMode calls the buttons trigger:
//   * attack button -> GameMode::attack, startDestroyBlock, continueDestroyBlock
//   * build button  -> GameMode::useItemOn
//
// GameMode::useItemOn and the destroy functions are discovered at runtime
// through the GameMode vtable: the first attack event carries the GameMode
// instance, we locate the attack slot (verified against the already-resolved
// attack signatures) and read the sibling slots at their fixed offsets
// (GameMode vtable layout: useItemOn = attack - 11, continueDestroyBlock =
// attack - 15, startDestroyBlock = attack - 17).
// ---------------------------------------------------------------------------
namespace {

constexpr int kUseItemOnSlotOffset = -11;
constexpr int kStartDestroySlotOffset = -17;
constexpr int kContinueDestroySlotOffset = -15;
constexpr int kMaxVtableScan = 96;
constexpr long long kTouchPulseUs = 250000;

using UseItemOnFn = bool (*)(void*, void*, void*, void*, unsigned char, void*, void*);
using DestroyFn = bool (*)(void*, void*, unsigned char, void*);
using ContinueDestroyFn = bool (*)(void*, void*, unsigned char, void*, void*);

UseItemOnFn g_useItemOnOriginal = nullptr;
DestroyFn g_startDestroyOriginal = nullptr;
ContinueDestroyFn g_continueDestroyOriginal = nullptr;

std::mutex g_touchHookMutex;
std::vector<std::uintptr_t> g_touchHookedAddrs;

long long keystrokesNowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void keystrokesHookIfNew(std::uintptr_t address, void* detour, void** originalSlot) {
    if (!address) return;
    std::lock_guard<std::mutex> lock(g_touchHookMutex);
    for (const auto hooked : g_touchHookedAddrs) {
        if (hooked == address) return;
    }
    if (bedrocktools::hooks::install(reinterpret_cast<void*>(address), detour, originalSlot)) {
        g_touchHookedAddrs.push_back(address);
    }
}

bool useItemOnDetour(void* gameMode, void* player, void* item, void* pos, unsigned char face, void* facePos, void* block) {
    if (g_keystrokesMod && g_keystrokesMod->enabled && g_keystrokesMod->touchMode()) {
        g_keystrokesMod->onTouchUse();
    }
    return g_useItemOnOriginal ? g_useItemOnOriginal(gameMode, player, item, pos, face, facePos, block) : false;
}

bool startDestroyBlockDetour(void* gameMode, void* pos, unsigned char face, void* outBool) {
    if (g_keystrokesMod && g_keystrokesMod->enabled && g_keystrokesMod->touchMode()) {
        g_keystrokesMod->onTouchAttack();
    }
    return g_startDestroyOriginal ? g_startDestroyOriginal(gameMode, pos, face, outBool) : false;
}

bool continueDestroyBlockDetour(void* gameMode, void* pos, unsigned char face, void* facePos, void* outBool) {
    if (g_keystrokesMod && g_keystrokesMod->enabled && g_keystrokesMod->touchMode()) {
        // Refreshes the pressed pulse while mining, without counting CPS for
        // every game tick (that would inflate the counter).
        g_keystrokesMod->onTouchAttackPulse();
    }
    return g_continueDestroyOriginal ? g_continueDestroyOriginal(gameMode, pos, face, facePos, outBool) : false;
}

void keystrokesTryInstallTouchHooks(void* gameMode) {
    if (!gameMode) return;

    const std::uintptr_t gmAttack = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::GameModeAttack);
    const std::uintptr_t smAttack = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::SurvivalModeAttack);
    if (!gmAttack && !smAttack) return;

    std::uintptr_t* const vtable = *reinterpret_cast<std::uintptr_t**>(gameMode);
    if (!vtable) return;

    int attackSlot = -1;
    for (int i = 0; i < kMaxVtableScan; ++i) {
        const std::uintptr_t fn = vtable[i];
        if (fn == gmAttack || fn == smAttack) {
            attackSlot = i;
            break;
        }
    }
    if (attackSlot < 0) return;

    const auto slotAddress = [&](int offset) -> std::uintptr_t {
        const int index = attackSlot + offset;
        if (index < 0 || index >= kMaxVtableScan) return 0;
        return vtable[index];
    };

    keystrokesHookIfNew(slotAddress(kUseItemOnSlotOffset), reinterpret_cast<void*>(&useItemOnDetour), reinterpret_cast<void**>(&g_useItemOnOriginal));
    keystrokesHookIfNew(slotAddress(kStartDestroySlotOffset), reinterpret_cast<void*>(&startDestroyBlockDetour), reinterpret_cast<void**>(&g_startDestroyOriginal));
    keystrokesHookIfNew(slotAddress(kContinueDestroySlotOffset), reinterpret_cast<void*>(&continueDestroyBlockDetour), reinterpret_cast<void**>(&g_continueDestroyOriginal));
}

}  // namespace

static void s_normalTickCallback(void* _this) {
    if (!g_keystrokesMod || !g_keystrokesMod->enabled) return;

    EntityContext* ctx = reinterpret_cast<EntityContext*>(reinterpret_cast<char*>(_this) + bedrocktools::sdk::offsets::Actor::mEntityContext);
    if (!ctx) return;

    auto* moveInput = ctx->tryGetComponent<MoveInputComponent>();
    if (!moveInput) return;

    // The input handler writes keyboard flags into mRawInputState, but some
    // game builds (and the touch path) only populate the processed mInputState
    // copy, so consult both.
    const auto& rawFlags = moveInput->mRawInputState.mFlagValues;
    const auto& inFlags = moveInput->mInputState.mFlagValues;
    const bool rawForward = rawFlags.test(13) || inFlags.test(13);
    const bool rawBackward = rawFlags.test(14) || inFlags.test(14);
    const bool rawLeft = rawFlags.test(15) || inFlags.test(15);
    const bool rawRight = rawFlags.test(16) || inFlags.test(16);
    const bool rawJump = rawFlags.test(7) || inFlags.test(7);
    const bool rawSneak = rawFlags.test(0) || inFlags.test(0);

    // On touch/mobile the joystick drives the analog move vector instead of
    // the keyboard flag bits, so map it to W/A/S/D as well. The vector uses
    // the game's own movement convention: +Y = forward, -Y = back, and on X
    // +X = left, -X = right (sideways is positive to the LEFT, matching the
    // game's move-vector convention). Getting X backwards shows A/D swapped.
    constexpr float kAnalogDeadzone = 0.2f;
    const auto& rawAnalog = moveInput->mRawInputState.mAnalogMoveVector;
    const auto& inAnalog = moveInput->mInputState.mAnalogMoveVector;
    const float analogX = std::abs(inAnalog.x) > std::abs(rawAnalog.x) ? inAnalog.x : rawAnalog.x;
    const float analogY = std::abs(inAnalog.y) > std::abs(rawAnalog.y) ? inAnalog.y : rawAnalog.y;

    g_keystrokesMod->bW = rawForward || analogY > kAnalogDeadzone;
    g_keystrokesMod->bS = rawBackward || analogY < -kAnalogDeadzone;
    g_keystrokesMod->bA = rawLeft || analogX > kAnalogDeadzone;
    g_keystrokesMod->bD = rawRight || analogX < -kAnalogDeadzone;
    g_keystrokesMod->bSpace = rawJump;
    g_keystrokesMod->bSneak = rawSneak;
}

KeystrokesModule::KeystrokesModule()
    : Module("Keystrokes", "Shows key presses and mouse CPS on screen.") {
    g_keystrokesMod = this;
}

KeystrokesModule::~KeystrokesModule() {
    if (g_keystrokesMod == this) g_keystrokesMod = nullptr;
}

void KeystrokesModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { s_normalTickCallback(event.player); });
    bedrocktools::events::bus().subscribe<bedrocktools::events::AttackEvent>([](auto& event) {
        if (!g_keystrokesMod) return;
        // First attack also hands us the GameMode instance used to discover
        // the touch build/break hooks (no signature required for them).
        keystrokesTryInstallTouchHooks(event.gameMode);
        if (!g_keystrokesMod->enabled || !g_keystrokesMod->touchMode()) return;
        g_keystrokesMod->onTouchAttack();
    });
}

void KeystrokesModule::onEnable() {
    m_mouseActive.store(true, std::memory_order_release);
}

void KeystrokesModule::onDisable() {
    m_mouseActive.store(false, std::memory_order_release);
    clearMouseState();
}

bool KeystrokesModule::onMouseEvent(int button, bool isDown) {
    m_lastMouseMs.store(keystrokesNowUs(), std::memory_order_relaxed);
    if (button == 1) {
        if (!isDown) {
            m_lmbDown.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return false;
        m_lmbDown.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_leftClicks.push_back(std::chrono::steady_clock::now());
    } else if (button == 2) {
        if (!isDown) {
            m_rmbDown.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return false;
        m_rmbDown.store(true, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_rightClicks.push_back(std::chrono::steady_clock::now());
    }
    return false;
}

std::pair<int, int> KeystrokesModule::getMouseCps() {
    const auto cutoff = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    while (!m_leftClicks.empty() && m_leftClicks.front() <= cutoff) m_leftClicks.pop_front();
    while (!m_rightClicks.empty() && m_rightClicks.front() <= cutoff) m_rightClicks.pop_front();
    return {static_cast<int>(m_leftClicks.size()), static_cast<int>(m_rightClicks.size())};
}

void KeystrokesModule::clearMouseState() {
    m_lmbDown.store(false, std::memory_order_relaxed);
    m_rmbDown.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    m_leftClicks.clear();
    m_rightClicks.clear();
}

bool KeystrokesModule::onTouchEvent(float x, float y, bool isDown) {
    (void)x;
    (void)y;
    (void)isDown;
    m_lastTouchMs.store(keystrokesNowUs(), std::memory_order_relaxed);
    return false;
}

bool KeystrokesModule::touchMode() const {
    return m_lastTouchMs.load(std::memory_order_relaxed) > m_lastMouseMs.load(std::memory_order_relaxed);
}

void KeystrokesModule::onTouchAttack() {
    m_touchRmbMs.store(keystrokesNowUs(), std::memory_order_relaxed);
    if (!m_showMouseCps.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    m_rightClicks.push_back(std::chrono::steady_clock::now());
}

void KeystrokesModule::onTouchAttackPulse() {
    m_touchRmbMs.store(keystrokesNowUs(), std::memory_order_relaxed);
}

void KeystrokesModule::onTouchUse() {
    m_touchLmbMs.store(keystrokesNowUs(), std::memory_order_relaxed);
    if (!m_showMouseCps.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    m_leftClicks.push_back(std::chrono::steady_clock::now());
}

void KeystrokesModule::onFrame() {
    if (!enabled) return;

    m_rainbowHue += 0.002f * m_rainbowSpeed;
    if (m_rainbowHue > 1.0f) m_rainbowHue -= 1.0f;

    auto updateAnim = [](KeyAnimState& state, bool pressed) {
        if (pressed) {
            state.pressProgress += 0.15f;
            if (state.pressProgress > 1.0f) state.pressProgress = 1.0f;
        } else {
            state.pressProgress -= 0.15f;
            if (state.pressProgress < 0.0f) state.pressProgress = 0.0f;
        }
    };

    const bool showMouseCps = m_showMouseCps.load(std::memory_order_relaxed);
    updateAnim(m_wState, bW);
    updateAnim(m_aState, bA);
    updateAnim(m_sState, bS);
    updateAnim(m_dState, bD);
    updateAnim(m_jumpState, bSpace);
    updateAnim(m_sneakState, bSneak);

    // Touch buttons have no down/up events; each GameMode call (attack or
    // build press) refreshes a short pulse that keeps the key lit.
    const long long now = keystrokesNowUs();
    const bool touchLmb = now - m_touchLmbMs.load(std::memory_order_relaxed) < kTouchPulseUs;
    const bool touchRmb = now - m_touchRmbMs.load(std::memory_order_relaxed) < kTouchPulseUs;
    updateAnim(m_lmbState, showMouseCps && (m_lmbDown.load(std::memory_order_relaxed) || touchLmb));
    updateAnim(m_rmbState, showMouseCps && (m_rmbDown.load(std::memory_order_relaxed) || touchRmb));

    std::vector<PLModMenu_DrawCommand> cmds;
    std::vector<std::string> textStore;
    textStore.reserve(16);

    float startX = hudPosX;
    float startY = hudPosY;
    float keySize = static_cast<float>(m_size);
    float spacing = 5.0f;

    // HUD editor hitbox: transparent full-bounds rect to make gaps draggable
    {
        const bool showMouseCps = m_showMouseCps.load(std::memory_order_relaxed);
        int rows = 2; // W and ASD
        if (showMouseCps) rows++;
        if (m_showJump) rows++;
        if (m_showSneak) rows++;
        float totalW = (keySize * 3) + (spacing * 2);
        float totalH = rows * keySize + (rows - 1) * spacing;
        PLModMenu_DrawCommand hitbox = {};
        hitbox.type = PL_DRAW_RECT_FILLED;
        hitbox.x = startX;
        hitbox.y = startY;
        hitbox.w = totalW;
        hitbox.h = totalH;
        hitbox.color = 0x02000000; // nearly transparent, keeps module draggable in hud editor
        if (m_roundKeys) hitbox.x3 = keySize * 0.08f;
        cmds.push_back(std::move(hitbox));
    }

    auto addKey = [&](float x, float y, float w, std::string_view label, std::string_view detail, const KeyAnimState& state) {
        float progress = state.pressProgress;
        float currentH = keySize - (keySize * 0.1f * progress);
        float currentW = w - (keySize * 0.1f * progress);
        float offsetX = (w - currentW) / 2.0f;
        float offsetY = (keySize - currentH) / 2.0f;

        uint32_t baseBg = 0x44000000;
        uint32_t targetBg = m_pressedColor;
        if (m_rainbow) {
            float r, g, b;
            keystrokesHSVtoRGB(m_rainbowHue, 1.0f, 1.0f, r, g, b);
            targetBg = (0xAA << 24) | (static_cast<int>(r * 255) << 16) | (static_cast<int>(g * 255) << 8) | static_cast<int>(b * 255);
        } else {
            targetBg = (0xAA << 24) | (targetBg & 0x00FFFFFF);
        }

        auto lerpColor = [](uint32_t a, uint32_t b, float t) -> uint32_t {
            int aa = (a >> 24) & 0xFF;
            int ar = (a >> 16) & 0xFF;
            int ag = (a >> 8) & 0xFF;
            int ab = a & 0xFF;
            int ba = (b >> 24) & 0xFF;
            int br = (b >> 16) & 0xFF;
            int bg = (b >> 8) & 0xFF;
            int bb = b & 0xFF;
            int ra = static_cast<int>(aa + (ba - aa) * t);
            int rr = static_cast<int>(ar + (br - ar) * t);
            int rg = static_cast<int>(ag + (bg - ag) * t);
            int rb = static_cast<int>(ab + (bb - ab) * t);
            return (ra << 24) | (rr << 16) | (rg << 8) | rb;
        };

        PLModMenu_DrawCommand bgCmd = {};
        bgCmd.type = PL_DRAW_RECT_FILLED;
        bgCmd.x = x + offsetX;
        bgCmd.y = y + offsetY;
        bgCmd.w = currentW;
        bgCmd.h = currentH;
        bgCmd.color = lerpColor(baseBg, targetBg, progress);
        if (m_roundKeys) bgCmd.x3 = keySize * 0.1f;
        cmds.push_back(std::move(bgCmd));

        PLModMenu_DrawCommand textCmd = {};
        textCmd.type = PL_DRAW_TEXT;
        textCmd.x = x + offsetX;
        textCmd.y = y + offsetY;
        textCmd.w = currentW;
        textCmd.h = detail.empty() ? currentH : currentH * 0.58f;
        textCmd.color = 0xFFFFFFFF;
        textCmd.size = currentH * (detail.empty() ? 0.5f : 0.34f);
        textCmd.text = label;
        cmds.push_back(std::move(textCmd));

        if (!detail.empty()) {
            PLModMenu_DrawCommand detailCmd = {};
            detailCmd.type = PL_DRAW_TEXT;
            detailCmd.x = x + offsetX;
            detailCmd.y = y + offsetY + currentH * 0.48f;
            detailCmd.w = currentW;
            detailCmd.h = currentH * 0.45f;
            detailCmd.color = 0xFFFFFFFF;
            detailCmd.size = currentH * 0.25f;
            detailCmd.text = detail;
            cmds.push_back(std::move(detailCmd));
        }
    };

    addKey(startX + keySize + spacing, startY, keySize, "W", "", m_wState);
    addKey(startX, startY + keySize + spacing, keySize, "A", "", m_aState);
    addKey(startX + keySize + spacing, startY + keySize + spacing, keySize, "S", "", m_sState);
    addKey(startX + (keySize + spacing) * 2, startY + keySize + spacing, keySize, "D", "", m_dState);

    float currentY = startY + (keySize + spacing) * 2;
    float totalW = (keySize * 3) + (spacing * 2);

    if (showMouseCps) {
        auto [leftCps, rightCps] = getMouseCps();
        const float mouseWidth = (totalW - spacing) * 0.5f;
        const std::string leftText = std::to_string(leftCps) + " CPS";
        const std::string rightText = std::to_string(rightCps) + " CPS";
        addKey(startX, currentY, mouseWidth, "LMB", leftText, m_lmbState);
        addKey(startX + mouseWidth + spacing, currentY, mouseWidth, "RMB", rightText, m_rmbState);
        currentY += keySize + spacing;
    }

    if (m_showJump) {
        addKey(startX, currentY, totalW, "JUMP", "", m_jumpState);
        currentY += keySize + spacing;
    }

    if (m_showSneak) {
        addKey(startX, currentY, totalW, "SNEAK", "", m_sneakState);
    }

    submitDrawCommands(moduleId, cmds);
}

void KeystrokesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("m_size")) m_size = j["m_size"].get<int>();
    if (j.contains("m_showJump")) m_showJump = j["m_showJump"].get<bool>();
    if (j.contains("m_showSneak")) m_showSneak = j["m_showSneak"].get<bool>();
    if (j.contains("m_showMouseCps")) m_showMouseCps.store(j["m_showMouseCps"].get<bool>(), std::memory_order_relaxed);
    if (j.contains("roundKeys")) m_roundKeys = j["roundKeys"].get<bool>();
    if (j.contains("rainbow")) m_rainbow = j["rainbow"].get<bool>();
    if (j.contains("rainbowSpeed")) m_rainbowSpeed = j["rainbowSpeed"].get<float>();
    if (j.contains("color")) {
        std::string hexStr = j["color"].get<std::string>();
        if (!hexStr.empty() && hexStr[0] == '#') {
            try {
                m_pressedColor = std::stoul(hexStr.substr(1), nullptr, 16);
            } catch (...) {
            }
        }
    }
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    if (!m_showMouseCps.load(std::memory_order_relaxed)) clearMouseState();
}

void KeystrokesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["m_size"] = m_size;
    j["m_showJump"] = m_showJump;
    j["m_showSneak"] = m_showSneak;
    j["m_showMouseCps"] = m_showMouseCps.load(std::memory_order_relaxed);
    j["roundKeys"] = m_roundKeys;
    j["rainbow"] = m_rainbow;
    j["rainbowSpeed"] = m_rainbowSpeed;

    char hexStr[10];
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_pressedColor);
    j["color"] = std::string(hexStr);

    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
}
