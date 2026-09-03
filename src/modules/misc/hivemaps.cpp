#include "hivemaps.hpp"
#include "config/ConfigManager.hpp"
#include <pl/Platform.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

namespace bedrocktools::hive {
namespace {

struct CacheEntry {
    std::vector<MapInfo> maps;
    std::int64_t updatedAt{};
    int httpStatus{};
    std::string error;
    std::string retryAfter;
    bool loading{};
};

std::mutex gMutex;
std::mutex gFileMutex;
std::map<std::string, CacheEntry> gCache;
std::map<std::string, std::vector<std::function<void()>>> gCallbacks;
bool gLoaded{};
constexpr std::int64_t kCacheSeconds = 3600;

std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::filesystem::path cachePath() {
    std::filesystem::path configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    return configPath.parent_path() / "hive_maps_cache.json";
}

// Offline fallback
void seedBundledMapsLocked() {
    auto &entry = gCache["bed"];
    if (!entry.maps.empty()) return;
    entry.updatedAt = 0;
    entry.maps = {
        {"Ablaze", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_ablaze.jpg"},
        {"Amulet", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_amulet.jpg"},
        {"Atlantis", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_atlantis.jpg"},
        {"Belfry", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_belfry.jpg"},
        {"Citadel", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_citadel.jpg"},
        {"Cityscape", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_cityscape.jpg"},
        {"Eggs", "SPRING", "DUOS", "https://cdn.playhive.com/maps/bed_duo_eggs.spring.jpg"},
        {"Enigma", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_enigma.jpg"},
        {"Fairy Garden", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_fairygarden_revamp.jpg"},
        {"Fairytale Forest", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_fairytaleforest.jpg"},
        {"Farmhouse", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_farmhouse.jpg"},
        {"Gates", "SUMMER", "DUOS", "https://cdn.playhive.com/maps/bed_duo_gates.summer.jpg"},
        {"Gates", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_gates.jpg"},
        {"Mithriller", "HALLOWEEN", "DUOS", "https://cdn.playhive.com/maps/bed_duo_mithril.halloween.jpg"},
        {"Mithril", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_mithril.jpg"},
        {"North Pole", "WINTERFEST", "DUOS", "https://cdn.playhive.com/maps/bed_duo_northpole.winterfest.jpg"},
        {"Oasis", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_oasis.jpg"},
        {"Orangewood", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_orangewood.jpg"},
        {"Present Factory", "WINTERFEST", "DUOS", "https://cdn.playhive.com/maps/bed_duo_presentfactory.winterfest.jpg"},
        {"Resurrected", "HALLOWEEN", "DUOS", "https://cdn.playhive.com/maps/bed_duo_resurrected.halloween.jpg"},
        {"Stranded", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_stranded.jpg"},
        {"Vanaheim", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_vanaheim.jpg"},
        {"Vineyard", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_vineyard.jpg"},
        {"Whirlwood", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_whirlwood.jpg"},
        {"Wicked Woods", "NO_SEASON", "DUOS", "https://cdn.playhive.com/maps/bed_duo_wickedwoods.jpg"},
        {"Manor", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_ltm_manor.jpg"},
        {"Greenstead", "NO_SEASON", "MEGA", "https://cdn.playhive.com/maps/bed_mega_greenstead.jpg"},
        {"Kingdom", "SPRING", "MEGA", "https://cdn.playhive.com/maps/bed_mega_kingdom.spring.jpg"},
        {"Kingdom", "WINTERFEST", "MEGA", "https://cdn.playhive.com/maps/bed_mega_kingdom.winterfest.jpg"},
        {"Kingdom", "NO_SEASON", "MEGA", "https://cdn.playhive.com/maps/bed_mega_kingdom.jpg"},
        {"Opulent", "NO_SEASON", "MEGA", "https://cdn.playhive.com/maps/bed_mega_opulent.jpg"},
        {"Plaza", "NO_SEASON", "MEGA", "https://cdn.playhive.com/maps/bed_mega_plaza.jpg"},
        {"Titan", "NO_SEASON", "MEGA", "https://cdn.playhive.com/maps/bed_mega_titan.jpg"},
        {"Aero", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_aero.jpg"},
        {"Arcane", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_arcane.jpg"},
        {"Beehive", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_beehive.jpg"},
        {"Brutaxis", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_brutaxis.jpg"},
        {"Castle", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_castle.jpg"},
        {"Enchantment", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_enchantment.jpg"},
        {"Facility", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_facility.jpg"},
        {"Festive", "WINTERFEST", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_festive.winterfest.jpg"},
        {"Haunted", "HALLOWEEN", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_haunted.halloween.jpg"},
        {"Igloo", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_igloo.jpg"},
        {"Library", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_library.jpg"},
        {"Lighthouse", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_lighthouse.jpg"},
        {"Mushroom", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_mushroom.jpg"},
        {"Trick or Treat", "HALLOWEEN", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_neighborhood.halloween.jpg"},
        {"Neighborhood", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_neighborhood.jpg"},
        {"Oceanic", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_oceanic.jpg"},
        {"Sandstorm", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_sandstorm.jpg"},
        {"Sprout", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_sprout.jpg"},
        {"Svartalfheim", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_svartalfheim.jpg"},
        {"Sway Bells", "WINTERFEST", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_sway.winterfest.jpg"},
        {"Sway", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_sway.jpg"},
        {"Tree Stump", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_treestump.jpg"},
        {"Turris", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_turris.jpg"},
        {"Warped", "NO_SEASON", "REGULAR", "https://cdn.playhive.com/maps/bed_solo_warped.jpg"},
        {"Spring", "SPRING", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_autumn.spring.jpg"},
        {"Summer", "SUMMER", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_autumn.summer.jpg"},
        {"Winter", "WINTERFEST", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_autumn.winterfest.jpg"},
        {"Autumn", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_autumn.jpg"},
        {"Cobalt", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_cobalt.jpg"},
        {"Coven", "HALLOWEEN", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_coven.halloween.jpg"},
        {"Cyberspace", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_cyberspace.jpg"},
        {"Gingerbread", "WINTERFEST", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_gingerbread.winterfest.jpg"},
        {"Grove", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_grove.jpg"},
        {"Hanging Gardens", "SPRING", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_hanginggardens.spring.jpg"},
        {"Hanging Gardens", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_hanginggardens.jpg"},
        {"Jotunheim", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_jotunheim.jpg"},
        {"Lumora", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_lumora.jpg"},
        {"O2", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_o2.jpg"},
        {"Pirates", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_pirates.jpg"},
        {"Prisma", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_prisma.jpg"},
        {"Quantum", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_quantum.jpg"},
        {"Rome", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_rome.jpg"},
        {"Ruins", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_ruins.jpg"},
        {"Serrano", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_serrano.jpg"},
        {"Soul's End", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_soulsend.jpg"},
        {"Summer Coast", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_summercoast.jpg"},
        {"Temple", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_temple.jpg"},
        {"Monster Labs", "HALLOWEEN", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_teslalabs.halloween.jpg"},
        {"Tesla Labs", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_teslalabs.jpg"},
        {"Tropical", "NO_SEASON", "SQUADS", "https://cdn.playhive.com/maps/bed_squad_tropical.jpg"},
    };
}

void loadCacheLocked() {
    if (gLoaded) return;
    gLoaded = true;
    seedBundledMapsLocked();
    try {
        std::lock_guard fileLock(gFileMutex);
        std::ifstream input(cachePath(), std::ios::binary);
        if (!input) return;
        nlohmann::json root;
        input >> root;
        if (!root.is_object()) return;
        for (auto &[game, value] : root.items()) {
            if (!value.is_object()) continue;
            CacheEntry entry;
            entry.updatedAt = value.value("updated_at", 0LL);
            entry.httpStatus = value.value("status", 0);
            if (value.contains("maps") && value["maps"].is_array()) {
                for (const auto &item : value["maps"]) {
                    if (!item.is_object()) continue;
                    std::string name = item.value("name", std::string{});
                    if (name.empty()) continue;
                    entry.maps.push_back({
                        std::move(name),
                        item.value("season", std::string{"NO_SEASON"}),
                        item.value("variant", std::string{"REGULAR"}),
                        item.value("image", std::string{})
                    });
                }
            }
            if (entry.httpStatus == 404 && entry.error.empty()) {
                entry.error = "Hive does not expose a selectable map list for this game";
            }
            if (entry.maps.empty() && game == "bed") entry.maps = gCache[game].maps;
            gCache[game] = std::move(entry);
        }
    } catch (...) {
    }
}

void saveCache() {
    nlohmann::json root = nlohmann::json::object();
    {
        std::lock_guard lock(gMutex);
        loadCacheLocked();
        for (const auto &[game, entry] : gCache) {
            nlohmann::json maps = nlohmann::json::array();
            for (const auto &map : entry.maps) {
                maps.push_back({
                    {"name", map.name},
                    {"season", map.season},
                    {"variant", map.variant},
                    {"image", map.imageUrl}
                });
            }
            root[game] = {
                {"updated_at", entry.updatedAt},
                {"status", entry.httpStatus},
                {"maps", std::move(maps)}
            };
        }
    }
    try {
        std::lock_guard fileLock(gFileMutex);
        const auto path = cachePath();
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (output) output << root.dump();
    } catch (...) {
    }
}

std::optional<std::vector<MapInfo>> parseMaps(const std::string &body) {
    try {
        auto root = nlohmann::json::parse(body);
        if (!root.is_array()) return std::nullopt;
        std::vector<MapInfo> result;
        for (const auto &item : root) {
            if (!item.is_object()) continue;
            std::string name = item.value("name", std::string{});
            if (name.empty()) continue;
            result.push_back({
                std::move(name),
                item.value("season", std::string{"NO_SEASON"}),
                item.value("variant", std::string{"REGULAR"}),
                item.value("image", std::string{})
            });
        }
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

void runCallbacks(std::vector<std::function<void()>> callbacks) {
    for (auto &callback : callbacks) {
        if (callback) callback();
    }
}

}

std::string apiGameId(std::string_view gameId) {
    std::string value(gameId);
    value = lower(value);
    const std::size_t dash = value.find('-');
    if (dash != std::string::npos) value.resize(dash);
    return value;
}

MapSnapshot mapSnapshot(std::string_view gameId) {
    std::lock_guard lock(gMutex);
    loadCacheLocked();
    const std::string game = apiGameId(gameId);
    auto it = gCache.find(game);
    if (it == gCache.end()) return {};
    const auto &entry = it->second;
    MapSnapshot snapshot;
    snapshot.maps = entry.maps;
    snapshot.loading = entry.loading;
    snapshot.updatedAt = entry.updatedAt;
    snapshot.httpStatus = entry.httpStatus;
    snapshot.error = entry.error;
    snapshot.retryAfter = entry.retryAfter;
    snapshot.stale = entry.updatedAt == 0 || nowSeconds() - entry.updatedAt >= kCacheSeconds;
    return snapshot;
}

std::vector<MapInfo> mapsForVariant(std::string_view gameId, std::string_view variant) {
    MapSnapshot snapshot = mapSnapshot(gameId);
    std::string wanted = lower(std::string(variant));
    std::vector<MapInfo> result;
    std::set<std::string> seen;
    for (const auto &map : snapshot.maps) {
        if (!wanted.empty() && lower(map.variant) != wanted) continue;
        std::string key = lower(map.name);
        if (!seen.insert(key).second) continue;
        result.push_back(map);
    }
    std::sort(result.begin(), result.end(), [](const MapInfo &left, const MapInfo &right) {
        return lower(left.name) < lower(right.name);
    });
    return result;
}

std::vector<std::string> variantsForGame(std::string_view gameId) {
    MapSnapshot snapshot = mapSnapshot(gameId);
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto &map : snapshot.maps) {
        std::string value = map.variant.empty() ? "REGULAR" : map.variant;
        std::string key = lower(value);
        if (seen.insert(key).second) result.push_back(std::move(value));
    }
    auto rank = [](std::string_view value) {
        static constexpr std::string_view order[] = {"REGULAR", "DUOS", "TRIOS", "SQUADS", "MEGA", "ROYALE"};
        for (int i = 0; i < static_cast<int>(std::size(order)); ++i) if (value == order[i]) return i;
        return 100;
    };
    std::sort(result.begin(), result.end(), [&](const std::string &a, const std::string &b) {
        int ra = rank(a), rb = rank(b);
        return ra == rb ? a < b : ra < rb;
    });
    return result;
}

void refreshMapsAsync(std::string gameId, bool force, std::function<void()> callback) {
    const std::string game = apiGameId(gameId);
    if (game.empty()) {
        if (callback) callback();
        return;
    }
    {
        std::lock_guard lock(gMutex);
        loadCacheLocked();
        auto &entry = gCache[game];
        if (callback) gCallbacks[game].push_back(std::move(callback));
        if (entry.loading) return;
        const bool fresh = entry.updatedAt != 0 && nowSeconds() - entry.updatedAt < kCacheSeconds &&
            (entry.httpStatus == 200 || entry.httpStatus == 404);
        if (!force && fresh) {
            auto callbacks = std::move(gCallbacks[game]);
            gCallbacks.erase(game);
            std::thread([callbacks = std::move(callbacks)]() mutable { runCallbacks(std::move(callbacks)); }).detach();
            return;
        }
        entry.loading = true;
        entry.error.clear();
        entry.retryAfter.clear();
    }

    std::thread([game]() {
        const auto response = pl::platform::httpGet("https://api.playhive.com/v0/game/map/" + game, 6000);
        const auto parsed = response.ok() ? parseMaps(response.body) : std::nullopt;
        std::vector<std::function<void()>> callbacks;
        bool shouldSave = false;
        {
            std::lock_guard lock(gMutex);
            auto &entry = gCache[game];
            entry.loading = false;
            entry.httpStatus = response.status;
            entry.retryAfter = response.retryAfter;
            if (response.ok() && parsed) {
                entry.maps = *parsed;
                entry.updatedAt = nowSeconds();
                entry.error.clear();
                shouldSave = true;
            } else if (response.ok()) {
                entry.error = "Hive API returned invalid map data";
            } else if (response.status == 404) {
                entry.maps.clear();
                entry.updatedAt = nowSeconds();
                entry.error = "Hive does not expose a selectable map list for this game";
                shouldSave = true;
            } else if (response.status == 429) {
                entry.error = response.retryAfter.empty() ? "Hive API rate limit reached" : "Hive API rate limited; retry after " + response.retryAfter;
            } else if (response.status > 0) {
                entry.error = "Hive API returned HTTP " + std::to_string(response.status);
            } else {
                entry.error = "Hive API request failed";
            }
            callbacks = std::move(gCallbacks[game]);
            gCallbacks.erase(game);
        }
        if (shouldSave) saveCache();
        runCallbacks(std::move(callbacks));
    }).detach();
}

void clearMapCache() {
    {
        std::lock_guard lock(gMutex);
        gCache.clear();
        seedBundledMapsLocked();
        gLoaded = true;
    }
    std::lock_guard fileLock(gFileMutex);
    std::error_code ec;
    std::filesystem::remove(cachePath(), ec);
}

}
