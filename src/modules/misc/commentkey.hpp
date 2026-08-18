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
        // On-screen button: each comment can show or hide its own button.
        bool screen{false};
        float x{140.0f};
        float y{80.0f};
        float width{110.0f};
        float height{40.0f};
        float textSize{20.0f};
        // Launcher keycap text color.
        std::uint32_t textColor{0x373737};
    };

    explicit CommentKey(std::function<void(const std::string&)> sendFunc = nullptr);

    bool onKeyEvent(int keyCode, bool isDown) override;
    bool onTouchEvent(float x, float y, bool isDown) override;
    void onFrame() override;

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

    // Both require mMutex to be held (or a single-threaded context such as the
    // constructor).
    void applyDefaultComments();
    void normalizeComments();
    bool inside(const Comment& comment, float x, float y) const;
    bool isPressed(std::size_t index) const;

    std::vector<Comment> mComments;
    std::vector<bool> mKeyDown;
    std::function<void(const std::string&)> mSendFunc;
    std::chrono::steady_clock::time_point mLastSendTime{};
    mutable std::mutex mMutex;

    // Shared look of the on-screen comment buttons (per-comment text options
    // live on Comment itself).
    // Launcher on-screen button look ("keycap" preset used by the launcher's
    // own overlay buttons): light gray face, dark 2px border, tiny corner
    // radius and 0.85 alpha.
    float mButtonOpacity = 0.85f;
    float mButtonRadius = 2.0f;
    std::uint32_t mButtonColor = 0x8B8B8B;
    std::uint32_t mButtonBorderColor = 0x373737;
    float mButtonBorderWidth = 2.0f;

    // HUD editor integration - group position
    float hudPosX = 0.0f;
    float hudPosY = 0.0f;
    bool isHudModule = true;

    // per-comment drag via HUD edit mode (inside mod)
    bool mHudEditMode = false;
    int mDraggingIndex = -1;
    // Short pressed highlight, mirroring the launcher's active button state.
    int mPressedIndex = -1;
    std::chrono::steady_clock::time_point mPressedTime{};
    float mDragOffsetX = 0.0f;
    float mDragOffsetY = 0.0f;
};
