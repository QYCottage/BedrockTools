// Tablist module — thanks to Kashifro
// GitHub: https://github.com/Kashifro

#include "tablist.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_set>

constexpr int HEAD_TEX_SIZE = 64;
using HeadPixels = std::array<uint8_t, HEAD_TEX_SIZE * HEAD_TEX_SIZE * 4>;

using GetRuntimeActorList_t = std::vector<void*>(*)(void*);
using ActorIsPlayer_t = bool(*)(void*);
using ActorGetNameTag_t = std::string(*)(void*);

static GetRuntimeActorList_t s_getRuntimeActorList = nullptr;
static ActorIsPlayer_t s_actorIsPlayer = nullptr;
static ActorGetNameTag_t s_actorGetNameTag = nullptr;

static std::string cleanPlayerName(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (std::size_t i = 0; i < input.size();) {
        const auto c = static_cast<unsigned char>(input[i]);
        if (c == 0xC2 && i + 1 < input.size() && static_cast<unsigned char>(input[i + 1]) == 0xA7) {
            i += 2;
            if (i < input.size()) ++i;
            continue;
        }
        if (c == 0xA7) {
            i += std::min<std::size_t>(2, input.size() - i);
            continue;
        }
        if (c >= 0x20 && c != 0x7F) output.push_back(input[i]);
        ++i;
    }

    auto first = output.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    auto last = output.find_last_not_of(' ');
    output = output.substr(first, last - first + 1);
    if (output.size() > 128) output.resize(128);
    return output;
}

static const void* getSkinImageFromActor(void* actor) {
    if (!actor) return nullptr;

    auto skinRef = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(actor) + bedrocktools::sdk::offsets::Player::mSkin);
    if (!skinRef) return nullptr;

    auto threadOwner = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(skinRef) + bedrocktools::sdk::offsets::SerializedSkinRef::mSkinImpl);
    if (!threadOwner) return nullptr;

    auto skinImpl = reinterpret_cast<uintptr_t>(threadOwner) + bedrocktools::sdk::offsets::ThreadOwner::mObject;
    auto image = reinterpret_cast<const void*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinImage);

    if (*reinterpret_cast<const bool*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mIsPersona)) {
        auto begin = *reinterpret_cast<const uintptr_t*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages);
        auto end = *reinterpret_cast<const uintptr_t*>(skinImpl + bedrocktools::sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages + sizeof(uintptr_t));
        if (begin && end >= begin && end - begin <= bedrocktools::sdk::offsets::AnimatedImageData::Size * 64) {
            for (auto entry = begin; entry < end; entry += bedrocktools::sdk::offsets::AnimatedImageData::Size) {
                auto type = *reinterpret_cast<const uint32_t*>(entry + bedrocktools::sdk::offsets::AnimatedImageData::mType);
                if (type == 2 || type == 3) {
                    image = reinterpret_cast<const void*>(entry + bedrocktools::sdk::offsets::AnimatedImageData::mImage);
                    if (type == 3) break;
                }
            }
        }
    }

    return image;
}

static bool extractHeadFromActor(void* actor, HeadPixels& out) {
    auto image = getSkinImageFromActor(actor);
    if (!image) return false;

    auto imageAddr = reinterpret_cast<uintptr_t>(image);
    auto width = *reinterpret_cast<const uint32_t*>(imageAddr + bedrocktools::sdk::offsets::SkinImage::mWidth);
    auto height = *reinterpret_cast<const uint32_t*>(imageAddr + bedrocktools::sdk::offsets::SkinImage::mHeight);
    auto pixels = *reinterpret_cast<const uint8_t* const*>(imageAddr + bedrocktools::sdk::offsets::Image::mBytesOffset);

    if (!pixels || width < 64 || width > 256 || height < 32 || height > 256 || width % 64 != 0 || height % 32 != 0) {
        return false;
    }

    const auto scale = width / 64;
    const int upScale = HEAD_TEX_SIZE / 8;
    out.fill(0);

    auto copyLayer = [&](int skinX, bool overlay) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                int srcX = skinX + x * static_cast<int>(scale) + static_cast<int>(scale / 2);
                int srcY = 8 * static_cast<int>(scale) + y * static_cast<int>(scale) + static_cast<int>(scale / 2);
                const auto* src = pixels + (static_cast<std::size_t>(srcY) * width + srcX) * 4;

                for (int sy = 0; sy < upScale; ++sy) {
                    for (int sx = 0; sx < upScale; ++sx) {
                        auto* dst = out.data() + ((y * upScale + sy) * HEAD_TEX_SIZE + x * upScale + sx) * 4;
                        if (!overlay) {
                            std::memcpy(dst, src, 4);
                            continue;
                        }

                        const auto alpha = src[3];
                        if (alpha == 0) continue;
                        if (alpha == 255) {
                            std::memcpy(dst, src, 4);
                            continue;
                        }

                        float a = alpha / 255.0f;
                        float inverse = 1.0f - a;
                        dst[0] = static_cast<uint8_t>(src[0] * a + dst[0] * inverse + 0.5f);
                        dst[1] = static_cast<uint8_t>(src[1] * a + dst[1] * inverse + 0.5f);
                        dst[2] = static_cast<uint8_t>(src[2] * a + dst[2] * inverse + 0.5f);
                        dst[3] = 255;
                    }
                }
            }
        }
    };

    copyLayer(8 * static_cast<int>(scale), false);
    copyLayer(40 * static_cast<int>(scale), true);
    return true;
}

static std::string getActorName(void* actor) {
    if (!actor) return {};

    if (s_actorGetNameTag) {
        auto name = cleanPlayerName(s_actorGetNameTag(actor));
        if (!name.empty()) return name;
    }

    auto* filteredName = reinterpret_cast<const std::string*>(reinterpret_cast<uintptr_t>(actor) + bedrocktools::sdk::offsets::Actor::mFilteredNameTag);
    if (filteredName && !filteredName->empty() && filteredName->size() <= 256) {
        auto name = cleanPlayerName(*filteredName);
        if (!name.empty()) return name;
    }

    auto* playerName = reinterpret_cast<const std::string*>(reinterpret_cast<uintptr_t>(actor) + bedrocktools::sdk::offsets::Player::mName);
    if (playerName && !playerName->empty() && playerName->size() <= 256) {
        return cleanPlayerName(*playerName);
    }

    return {};
}

static std::string getActorImageKey(void* actor) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "tablist_%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(actor)));
    return buffer;
}

TablistModule* TablistModule::s_instance = nullptr;

static void tablistTickCallback(void* localPlayer) {
    auto* instance = TablistModule::getInstance();
    if (instance && instance->enabled) instance->onLocalPlayerTick(localPlayer);
}

TablistModule::TablistModule() : Module("Tablist", "Displays the loaded player list on screen") {
    s_instance = this;
    hideInHudEditor = true;
    isHudModule = false;
}

TablistModule::~TablistModule() {
    if (s_instance == this) s_instance = nullptr;
}

TablistModule* TablistModule::getInstance() {
    return s_instance;
}

void TablistModule::onLocalPlayerTick(void* localPlayer) {
    if (!localPlayer) return;
    if (++m_refreshTicks < 20) return;
    m_refreshTicks = 0;

    std::vector<void*> actors;
    auto level = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(localPlayer) + bedrocktools::sdk::offsets::Actor::mLevel);
    if (level && s_getRuntimeActorList) {
        auto actorManager = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(level) + bedrocktools::sdk::offsets::Level::mActorManager);
        if (actorManager) actors = s_getRuntimeActorList(actorManager);
    }

    std::vector<TablistPlayerInfo> next;
    next.reserve(actors.size() + 1);
    std::unordered_set<void*> seen;
    seen.reserve(actors.size() + 1);

    auto addPlayer = [&](void* actor, bool trustedPlayer) {
        if (!actor || !seen.emplace(actor).second) return;
        if (!trustedPlayer && (!s_actorIsPlayer || !s_actorIsPlayer(actor))) return;

        auto name = getActorName(actor);
        if (name.empty()) return;

        auto imageKey = getActorImageKey(actor);
        if (m_showHeads) {
            HeadPixels head{};
            if (extractHeadFromActor(actor, head)) {
                pl::modmenu::registerImage(imageKey, head, HEAD_TEX_SIZE, HEAD_TEX_SIZE);
            }
        }

        next.push_back({std::move(name), std::move(imageKey)});
    };

    addPlayer(localPlayer, true);
    for (auto* actor : actors) addPlayer(actor, false);

    std::sort(next.begin(), next.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });

    std::lock_guard<std::mutex> lock(m_mutex);
    m_players.swap(next);
}

void TablistModule::onInit() {
    s_getRuntimeActorList = reinterpret_cast<GetRuntimeActorList_t>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorManagerList));
    s_actorIsPlayer = reinterpret_cast<ActorIsPlayer_t>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorIsPlayer));
    s_actorGetNameTag = reinterpret_cast<ActorGetNameTag_t>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ActorGetNameTag));
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) { tablistTickCallback(event.player); });
}

void TablistModule::onEnable() {
    m_refreshTicks = 20;
}

void TablistModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_players.clear();
}

void TablistModule::onFrame() {
    if (!enabled) return;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PLModMenu_DrawCommand> cmds;
    
    float startX = -20000.f - (m_colWidth / 2.f);
    float startY = 20.f; 
    float rowHeight = m_textSize + 10.f;
    float headSize = m_textSize;
    
    if (m_players.empty()) {
        
        PLModMenu_DrawCommand bgCmd = {};
        bgCmd.type = PL_DRAW_RECT_FILLED;
        bgCmd.x = startX;
        bgCmd.y = startY;
        bgCmd.w = m_colWidth;
        bgCmd.h = rowHeight + 10.f;
        bgCmd.color = m_bgColorHex;
        cmds.push_back(bgCmd);
        
        PLModMenu_DrawCommand txtCmd = {};
        txtCmd.type = PL_DRAW_TEXT;
        txtCmd.x = startX + 5.f;
        txtCmd.y = startY + 5.f + (rowHeight - m_textSize) / 2.f;
        txtCmd.w = 0.f; 
        txtCmd.h = m_textSize;
        txtCmd.size = m_textSize;
        txtCmd.color = m_textColorHex;
        txtCmd.text = "Tablist (Empty)";
        cmds.push_back(txtCmd);
        
        ::submitDrawCommands(moduleId, cmds);
        return;
    }
    
    int numPlayers = (int)m_players.size();
    int cols = std::min(numPlayers, m_maxColumns);
    if (cols == 0) cols = 1;
    int rows = (numPlayers + cols - 1) / cols;
    if (rows == 0) rows = 1;
    
    float totalWidth = cols * m_colWidth;
    float totalHeight = rows * rowHeight + 10.f;
    
    startX = -20000.f - (totalWidth / 2.f);
    startY = 20.f; 
    
    PLModMenu_DrawCommand bgCmd = {};
    bgCmd.type = PL_DRAW_RECT_FILLED;
    bgCmd.x = startX;
    bgCmd.y = startY;
    bgCmd.w = totalWidth;
    bgCmd.h = totalHeight;
    bgCmd.color = m_bgColorHex;
    cmds.push_back(bgCmd);

    for (int i = 0; i < numPlayers; ++i) {
        int row = i / cols;
        int col = i % cols;
        
        float cellX = startX + col * m_colWidth;
        float cellY = startY + 5.f + row * rowHeight;
        
        const auto& player = m_players[i];
        
        float textX = cellX + 5.f;
        
        if (m_showHeads) {
            PLModMenu_DrawCommand headCmd = {};
            headCmd.type = PL_DRAW_IMAGE;
            headCmd.x = textX;
            headCmd.y = cellY + (rowHeight - headSize) / 2.f;
            headCmd.w = headSize;
            headCmd.h = headSize;
            headCmd.color = 0xFFFFFFFF; 
            headCmd.imageId = player.imageId; 
            cmds.push_back(headCmd);
            
            textX += headSize + 5.f;
        }
        
        PLModMenu_DrawCommand txtCmd = {};
        txtCmd.type = PL_DRAW_TEXT;
        txtCmd.x = textX;
        
        txtCmd.y = cellY + (rowHeight / 2.f) + (m_textSize / 2.f) - (m_textSize * 0.1f);
        txtCmd.w = 0.f; 
        txtCmd.h = m_textSize;
        txtCmd.size = m_textSize;
        txtCmd.color = m_textColorHex;
        txtCmd.text = player.name.c_str();
        cmds.push_back(txtCmd);
    }
    
    ::submitDrawCommands(moduleId, cmds);
}

void TablistModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("hudPosX")) hudPosX = j["hudPosX"].get<float>();
    if (j.contains("hudPosY")) hudPosY = j["hudPosY"].get<float>();
    if (j.contains("isHudModule")) isHudModule = j["isHudModule"].get<bool>();
    
    if (j.contains("m_textSize")) m_textSize = j["m_textSize"].get<float>();
    if (j.contains("m_colWidth")) m_colWidth = j["m_colWidth"].get<float>();
    if (j.contains("m_maxColumns")) m_maxColumns = j["m_maxColumns"].get<int>();
    if (j.contains("m_showHeads")) m_showHeads = j["m_showHeads"].get<bool>();
    
    if (j.contains("textColorHex")) {
        std::string hexStr = j["textColorHex"].get<std::string>();
        if (hexStr.length() > 0 && hexStr[0] == '#') {
            try { m_textColorHex = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }
    if (j.contains("bgColorHex")) {
        std::string hexStr = j["bgColorHex"].get<std::string>();
        if (hexStr.length() > 0 && hexStr[0] == '#') {
            try { m_bgColorHex = std::stoul(hexStr.substr(1), nullptr, 16); } catch (...) {}
        }
    }
}

void TablistModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX;
    j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_textSize"] = m_textSize;
    j["m_colWidth"] = m_colWidth;
    j["m_maxColumns"] = m_maxColumns;
    j["m_showHeads"] = m_showHeads;
    
    char hexStr[10];
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_textColorHex);
    j["textColorHex"] = std::string(hexStr);
    
    snprintf(hexStr, sizeof(hexStr), "#%08X", m_bgColorHex);
    j["bgColorHex"] = std::string(hexStr);
}
