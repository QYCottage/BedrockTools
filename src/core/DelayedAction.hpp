#pragma once

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace bedrocktools::core {

class DelayedAction {
public:
    using Clock = std::chrono::steady_clock;

    bool schedule(void* client, void* player, int delaySeconds, std::string value,
                  Clock::time_point now = Clock::now()) {
        if (!client || !player) return false;
        std::lock_guard lock(mMutex);
        if (mPending || (mLastAction && now - *mLastAction < std::chrono::seconds(3))) return false;
        mPending = Pending{client, player, now + std::chrono::seconds(std::clamp(delaySeconds, 0, 10)), std::move(value)};
        return true;
    }

    std::optional<std::string> take(void* client, void* player, Clock::time_point now = Clock::now()) {
        std::lock_guard lock(mMutex);
        if (!mPending) return std::nullopt;
        if (!client || !player || client != mPending->client || player != mPending->player) {
            mPending.reset();
            return std::nullopt;
        }
        if (now < mPending->due) return std::nullopt;
        std::string value = std::move(mPending->value);
        mPending.reset();
        mLastAction = now;
        return value;
    }

    void cancel() {
        std::lock_guard lock(mMutex);
        mPending.reset();
    }

    void resetCooldown() {
        std::lock_guard lock(mMutex);
        mLastAction.reset();
    }

private:
    struct Pending {
        void* client;
        void* player;
        Clock::time_point due;
        std::string value;
    };

    std::mutex mMutex;
    std::optional<Pending> mPending;
    std::optional<Clock::time_point> mLastAction;
};

}
