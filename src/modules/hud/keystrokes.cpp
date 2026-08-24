#include "keystrokes.hpp"
#include "modules/ModuleRegistry.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/sdk/input/MoveInput.hpp>
#include <cmath>
#include <string>
#include <string_view>
#include <cstdint>

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

static constexpr std::uint16_t swingSourceBit(std::uint8_t source) {
    return source < 16 ? static_cast<std::uint16_t>(1u << source) : 0;
}

static constexpr std::uint16_t buildSwingMask = static_cast<std::uint16_t>((1u << 1) | (1u << 3) | (1u << 5));
static constexpr std::uint16_t interactSwingMask = static_cast<std::uint16_t>(1u << 3);
static constexpr std::uint16_t useItemSwingMask = static_cast<std::uint16_t>(1u << 5);
KeystrokesModule::PlayerSwingFn KeystrokesModule::s_playerSwingOriginal = nullptr;

static std::int64_t keystrokesNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool KeystrokesModule::playerSwingDetour(void* player, std::uint8_t source) {
    const bool result = s_playerSwingOriginal ? s_playerSwingOriginal(player, source) : false;
    if (result && g_keystrokesMod && g_keystrokesMod->enabled) {
        constexpr std::uint8_t buildSource = 1;
        constexpr std::uint8_t mineSource = 2;
        if (source == buildSource) {
            g_keystrokesMod->queueNativeRight(buildSwingMask);
        } else {
            const bool miningSwing = source == mineSource && g_keystrokesMod->m_destroyActive.load(std::memory_order_relaxed);
            if (!miningSwing) g_keystrokesMod->queueNativeSwing(source);
        }
    }
    return result;
}

static void s_normalTickCallback(void* player) {
    if (!g_keystrokesMod || !g_keystrokesMod->enabled) return;

    auto* moveInput = bedrocktools::sdk::moveInputComponent(player);
    if (!moveInput) return;

    const auto& raw = moveInput->mRawInputState;
    const auto analog = raw.mAnalogMoveVector;
    constexpr float analogThreshold = 0.05f;

    g_keystrokesMod->bW = raw.test(MoveInputState::Flag::Up) || raw.test(MoveInputState::Flag::UpLeft) || raw.test(MoveInputState::Flag::UpRight) || analog.y > analogThreshold;
    g_keystrokesMod->bA = raw.test(MoveInputState::Flag::Left) || raw.test(MoveInputState::Flag::UpLeft) || raw.test(MoveInputState::Flag::DownLeft) || analog.x > analogThreshold;
    g_keystrokesMod->bS = raw.test(MoveInputState::Flag::Down) || raw.test(MoveInputState::Flag::DownLeft) || raw.test(MoveInputState::Flag::DownRight) || analog.y < -analogThreshold;
    g_keystrokesMod->bD = raw.test(MoveInputState::Flag::Right) || raw.test(MoveInputState::Flag::UpRight) || raw.test(MoveInputState::Flag::DownRight) || analog.x < -analogThreshold;
    g_keystrokesMod->bSpace = raw.test(MoveInputState::Flag::JumpDown);
    g_keystrokesMod->bSneak = raw.test(MoveInputState::Flag::SneakDown);
}

KeystrokesModule::KeystrokesModule()
    : Module("Keystrokes", "Shows key presses and mouse CPS on screen.") {
    g_keystrokesMod = this;
}

KeystrokesModule::~KeystrokesModule() {
    if (g_keystrokesMod == this) g_keystrokesMod = nullptr;
}

void KeystrokesModule::onInit() {
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([this](auto& event) {
        resolveNativeInputTick();
        s_normalTickCallback(event.player);
    });
    bedrocktools::events::bus().subscribe<bedrocktools::events::GameModeActionEvent>([this](auto& event) {
        switch (event.action) {
            case bedrocktools::events::GameModeAction::StartDestroyBlock:
                if (!m_destroyActive.exchange(true, std::memory_order_relaxed)) queueNativeExplicitLeft();
                break;
            case bedrocktools::events::GameModeAction::StopDestroyBlock:
                m_destroyActive.store(false, std::memory_order_relaxed);
                break;
            case bedrocktools::events::GameModeAction::Attack:
                queueNativeExplicitLeft();
                break;
            case bedrocktools::events::GameModeAction::Interact:
                queueNativeRight(interactSwingMask);
                break;
            case bedrocktools::events::GameModeAction::UseItemOn:
                break;
            case bedrocktools::events::GameModeAction::UseItem:
                if (event.hasNativeResult && event.nativeResult) queueNativeRight(useItemSwingMask);
                break;
            case bedrocktools::events::GameModeAction::StartBuildBlock:
            case bedrocktools::events::GameModeAction::UseItemAsAttack:
                break;
        }
    });

    if (!m_playerSwingHooked) {
        const auto address = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LocalPlayerSwing);
        if (address) {
            const auto handle = bedrocktools::hooks::install(reinterpret_cast<void*>(address), reinterpret_cast<void*>(playerSwingDetour), reinterpret_cast<void**>(&s_playerSwingOriginal));
            m_playerSwingHooked = handle != nullptr;
        }
    }
}

void KeystrokesModule::onEnable() {
    m_mouseActive.store(true, std::memory_order_release);
}

void KeystrokesModule::onDisable() {
    m_mouseActive.store(false, std::memory_order_release);
    clearMouseState();
}

bool KeystrokesModule::onMouseEvent(int button, bool isDown) {
    if (button == 1) {
        if (!isDown) {
            m_lmbDown.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return false;
        m_lmbDown.store(true, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        m_lastMouseLmbNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_leftClicks.push_back(now);
    } else if (button == 2) {
        if (!isDown) {
            m_rmbDown.store(false, std::memory_order_relaxed);
            return false;
        }
        if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return false;
        m_rmbDown.store(true, std::memory_order_relaxed);
        const auto now = std::chrono::steady_clock::now();
        m_lastMouseRmbNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        m_rightClicks.push_back(now);
    }
    return false;
}

void KeystrokesModule::commitNativeClickBatchLocked() {
    if (!m_nativeBatchActive) return;

    constexpr std::int64_t mouseCorrelationNs = 120000000;
    constexpr std::int64_t pressDurationNs = 110000000;
    const auto lmbMouseNs = m_lastMouseLmbNs.load(std::memory_order_relaxed);
    const auto rmbMouseNs = m_lastMouseRmbNs.load(std::memory_order_relaxed);
    const auto nearBatch = [&](std::int64_t value) {
        if (value == 0) return false;
        const auto delta = value > m_nativeBatchLastNs ? value - m_nativeBatchLastNs : m_nativeBatchLastNs - value;
        return delta < mouseCorrelationNs;
    };

    if (!nearBatch(lmbMouseNs) && !nearBatch(rmbMouseNs)) {
        const auto now = std::chrono::steady_clock::now();
        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        if (m_nativeBatchRight) {
            m_rightClicks.push_back(now);
            m_nativeRmbUntilNs.store(nowNs + pressDurationNs, std::memory_order_relaxed);
        } else if (m_nativeBatchLeft) {
            m_leftClicks.push_back(now);
            m_nativeLmbUntilNs.store(nowNs + pressDurationNs, std::memory_order_relaxed);
        }
    }

    m_nativeBatchActive = false;
    m_nativeBatchLeft = false;
    m_nativeBatchRight = false;
    m_nativeBatchLastNs = 0;
}

void KeystrokesModule::queueNativeClick(bool left) {
    if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return;

    constexpr std::int64_t batchGapNs = 45000000;
    const auto nowNs = keystrokesNowNs();
    std::lock_guard<std::mutex> lock(m_mouseMutex);

    if (m_nativeBatchActive && nowNs - m_nativeBatchLastNs > batchGapNs) commitNativeClickBatchLocked();
    if (!m_nativeBatchActive) m_nativeBatchActive = true;
    if (left) m_nativeBatchLeft = true;
    else m_nativeBatchRight = true;
    m_nativeBatchLastNs = nowNs;
}

void KeystrokesModule::queueNativeSwing(std::uint8_t source) {
    if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    const bool ambiguous = source == 1 || source == 3 || source == 5;
    const auto sourceBit = swingSourceBit(source);
    if (sourceBit != 0 && (m_nativeRmbSwingSuppressMask & sourceBit) != 0) return;
    if (m_nativeRightThisTick && ambiguous) return;
    if (m_nativeExplicitLeftThisTick) return;
    if (ambiguous && m_nativeSwingSuppressCount > 0) {
        --m_nativeSwingSuppressCount;
        if (m_nativeSwingSuppressCount == 0) m_nativeSwingSuppressTicks = 0;
        return;
    }
    m_pendingNativeSwings.push_back(PendingNativeSwing{source, static_cast<std::uint8_t>(ambiguous ? 2 : 1)});
}

void KeystrokesModule::queueNativeExplicitLeft() {
    if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return;
    bool shouldQueue = false;
    {
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        if (m_nativeExplicitLeftThisTick) return;
        if (!m_pendingNativeSwings.empty()) m_pendingNativeSwings.pop_back();
        else {
            m_nativeSwingSuppressCount = 1;
            m_nativeSwingSuppressTicks = 1;
        }
        m_nativeExplicitLeftThisTick = true;
        shouldQueue = true;
    }
    if (shouldQueue) queueNativeClick(true);
}

void KeystrokesModule::queueNativeRight(std::uint16_t swingSourceMask) {
    if (!m_mouseActive.load(std::memory_order_acquire) || !m_showMouseCps.load(std::memory_order_relaxed)) return;
    bool shouldQueue = false;
    {
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        for (auto it = m_pendingNativeSwings.begin(); it != m_pendingNativeSwings.end();) {
            const auto bit = swingSourceBit(it->source);
            if (bit != 0 && (swingSourceMask & bit) != 0) it = m_pendingNativeSwings.erase(it);
            else ++it;
        }
        m_nativeRmbSwingSuppressMask = static_cast<std::uint16_t>(m_nativeRmbSwingSuppressMask | swingSourceMask);
        if (m_nativeRmbSwingSuppressTicks < 2) m_nativeRmbSwingSuppressTicks = 2;
        if (!m_nativeRightThisTick) {
            m_nativeRightThisTick = true;
            shouldQueue = true;
        }
    }
    if (shouldQueue) queueNativeClick(false);
}

void KeystrokesModule::resolveNativeInputTick() {
    std::uint32_t expiredSwings = 0;
    {
        std::lock_guard<std::mutex> lock(m_mouseMutex);
        for (auto it = m_pendingNativeSwings.begin(); it != m_pendingNativeSwings.end();) {
            if (it->ticksRemaining > 0) --it->ticksRemaining;
            if (it->ticksRemaining == 0) {
                ++expiredSwings;
                it = m_pendingNativeSwings.erase(it);
            } else {
                ++it;
            }
        }
        if (m_nativeSwingSuppressTicks > 0) {
            --m_nativeSwingSuppressTicks;
            if (m_nativeSwingSuppressTicks == 0) m_nativeSwingSuppressCount = 0;
        }
        if (m_nativeRmbSwingSuppressTicks > 0) {
            --m_nativeRmbSwingSuppressTicks;
            if (m_nativeRmbSwingSuppressTicks == 0) m_nativeRmbSwingSuppressMask = 0;
        }
        m_nativeExplicitLeftThisTick = false;
        m_nativeRightThisTick = false;
    }
    if (expiredSwings > 0) queueNativeClick(true);
}

void KeystrokesModule::flushNativeClickBatch(bool force) {
    constexpr std::int64_t batchGapNs = 45000000;
    const auto nowNs = keystrokesNowNs();
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    if (!m_nativeBatchActive) return;
    if (force || nowNs - m_nativeBatchLastNs >= batchGapNs) commitNativeClickBatchLocked();
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
    m_nativeLmbUntilNs.store(0, std::memory_order_relaxed);
    m_nativeRmbUntilNs.store(0, std::memory_order_relaxed);
    m_lastMouseLmbNs.store(0, std::memory_order_relaxed);
    m_lastMouseRmbNs.store(0, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(m_mouseMutex);
    m_nativeBatchActive = false;
    m_nativeBatchLeft = false;
    m_nativeBatchRight = false;
    m_nativeBatchLastNs = 0;
    m_pendingNativeSwings.clear();
    m_nativeSwingSuppressCount = 0;
    m_nativeSwingSuppressTicks = 0;
    m_nativeRmbSwingSuppressMask = 0;
    m_nativeRmbSwingSuppressTicks = 0;
    m_nativeExplicitLeftThisTick = false;
    m_nativeRightThisTick = false;
    m_destroyActive.store(false, std::memory_order_relaxed);
    m_leftClicks.clear();
    m_rightClicks.clear();
}

void KeystrokesModule::onFrame() {
    if (!enabled) return;

    flushNativeClickBatch(false);

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
    const auto nowNs = keystrokesNowNs();
    const bool lmbPressed = m_lmbDown.load(std::memory_order_relaxed) || nowNs < m_nativeLmbUntilNs.load(std::memory_order_relaxed);
    const bool rmbPressed = m_rmbDown.load(std::memory_order_relaxed) || nowNs < m_nativeRmbUntilNs.load(std::memory_order_relaxed);
    updateAnim(m_lmbState, showMouseCps && lmbPressed);
    updateAnim(m_rmbState, showMouseCps && rmbPressed);

    std::vector<PLModMenu_DrawCommand> cmds;

    float startX = hudPosX;
    float startY = hudPosY;
    float keySize = static_cast<float>(m_size);
    float spacing = 5.0f;

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
