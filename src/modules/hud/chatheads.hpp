#pragma once

#include "../Module.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

inline constexpr int ChatHeadTextureSize = 64;
using ChatHeadPixels =
    std::array<std::uint8_t, ChatHeadTextureSize * ChatHeadTextureSize * 4>;

struct ChatHeadsPlayer {
    std::string              name;
    std::vector<std::string> aliases;
    std::string              imageId;
    bool                     needsTexture = false;
    ChatHeadPixels           pixels{};
};

struct ChatHeadsMessage {
    std::string fullString;
    std::string message;
};

class ChatHeadsModule : public Module {
public:
    struct HeadDrawRequest {
        std::string imageId;
        float       x     = 0.0f;
        float       y     = 0.0f;
        float       size  = 0.0f;
        float       alpha = 1.0f;
    };

    ChatHeadsModule();
    ~ChatHeadsModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void clearCache();
    void capturePlayerListPacket(const void* packet);
    void captureGuiMessage(const void* guiMessage);
    void refreshPlayersFromWorld(void* localPlayer);
    void setChatScreenOpen(bool open);
    bool isRecentChatText(const std::string& cleanText);
    bool queueHeadForText(void* renderContext, const std::string& text, float x, float y, float alpha);

    float m_headSize = 10.0f;
    float m_padding  = 2.0f;

private:
    std::mutex                   m_cacheMutex;
    std::vector<ChatHeadsPlayer> m_cache;

    std::mutex                   m_messageMutex;
    std::deque<ChatHeadsMessage> m_messages;

    std::mutex                            m_requestMutex;
    std::vector<HeadDrawRequest>          m_frameRequests;
    std::vector<HeadDrawRequest>          m_retainedOverlayRequests;
    std::chrono::steady_clock::time_point m_lastOverlayRequestTime{};

    std::uint64_t    m_nextImageId    = 0;
    int              m_refreshTicks   = 0;
    bool             m_hooksInstalled = false;
    std::atomic_bool m_chatScreenOpen{false};
};
