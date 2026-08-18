#pragma once

#include <modules/Module.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

class CommentKey final : public Module {
public:
    static constexpr std::size_t MaxComments = 8;

    struct Comment {
        std::string text;
        int keyCode{0};
        bool enabled{false};
        // Each comment can show or hide its launcher-managed on-screen button.
        bool screen{false};
        float width{110.0f};
        float height{40.0f};
        std::uint32_t textColor{0x373737};
    };

    explicit CommentKey(std::function<void(const std::string&)> sendFunc = nullptr);
    ~CommentKey() override;

    void onInit() override;
    bool onKeyEvent(int keyCode, bool isDown) override;

    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Kept for callers that manage comments programmatically. The menu itself
    // exposes eight fixed slots instead of Add Comment / Remove Comment actions.
    void addComment(std::string text, int keyCode);
    void removeComment(std::size_t index);
    void updateComment(std::size_t index, std::string text, int keyCode, bool enabled);

    void sendComment(std::size_t index);

    const std::vector<Comment>& getComments() const noexcept { return mComments; }

    void setSendFunction(std::function<void(const std::string&)> sendFunc) {
        std::lock_guard<std::mutex> lock(mMutex);
        mSendFunc = std::move(sendFunc);
    }

    // Minimum time between keyboard-triggered comments, in milliseconds.
    int cooldownTime = 300;

private:
    void sendTextPacket(const std::string& text);
    void syncOverlayButtons();
    void unregisterOverlayButtons();

    // Both require mMutex to be held (or a single-threaded context such as the
    // constructor).
    void applyDefaultComments();
    void normalizeComments();

    std::vector<Comment> mComments;
    std::vector<bool> mKeyDown;
    std::function<void(const std::string&)> mSendFunc;
    std::chrono::steady_clock::time_point mLastSendTime{};
    mutable std::mutex mMutex;

    // Launcher keycap styling forwarded to each registered overlay button.
    float mButtonOpacity = 0.85f;
    std::uint32_t mButtonColor = 0x8B8B8B;
    std::uint32_t mButtonBorderColor = 0x373737;
};
