#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace bedrocktools::hive {

struct MapInfo {
    std::string name;
    std::string season;
    std::string variant;
    std::string imageUrl;
};

struct MapSnapshot {
    std::vector<MapInfo> maps;
    bool loading{};
    bool stale{};
    int httpStatus{};
    std::int64_t updatedAt{};
    std::string error;
    std::string retryAfter;
};

std::string apiGameId(std::string_view gameId);
MapSnapshot mapSnapshot(std::string_view gameId);
std::vector<MapInfo> mapsForVariant(std::string_view gameId, std::string_view variant);
std::vector<std::string> variantsForGame(std::string_view gameId);
void refreshMapsAsync(std::string gameId, bool force, std::function<void()> callback = {});
void clearMapCache();

}
