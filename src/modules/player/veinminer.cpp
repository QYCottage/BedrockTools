#include "veinminer.hpp"

#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/events/LocalPlayerTickEvent.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/world/Dimension.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
using BlockPos = bedrocktools::sdk::BlockPos;
using DestroyBlockFn = bool (*)(void*, const BlockPos&, std::uint8_t);
using GetBlockFn = const bedrocktools::sdk::Block* (*)(void*, const BlockPos&);

DestroyBlockFn g_destroyBlockOriginal = nullptr;
GetBlockFn g_getBlock = nullptr;
VeinMinerModule* g_module = nullptr;

struct PosHash {
    std::size_t operator()(const BlockPos& p) const noexcept {
        std::size_t h = static_cast<std::uint32_t>(p.x) * 0x9E3779B1u;
        h ^= static_cast<std::uint32_t>(p.y) * 0x85EBCA77u + (h << 6) + (h >> 2);
        h ^= static_cast<std::uint32_t>(p.z) * 0xC2B2AE3Du + (h << 6) + (h >> 2);
        return h;
    }
};

struct PosEqual {
    bool operator()(const BlockPos& a, const BlockPos& b) const noexcept {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
};

bool endsWith(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool isAir(std::string_view name) {
    return name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air";
}

std::string_view blockName(void* region, const BlockPos& pos) {
    if (!region || !g_getBlock) return {};
    const auto* block = g_getBlock(region, pos);
    if (!block) return {};
    const std::string* name = block->fullName();
    return name ? std::string_view(*name) : std::string_view{};
}

void* playerFromGameMode(void* gameMode) {
    if (!gameMode) return nullptr;
    return bedrocktools::sdk::field<void*>(gameMode, bedrocktools::sdk::offsets::GameMode::mPlayer);
}

void* regionFromPlayer(void* rawPlayer) {
    auto* player = reinterpret_cast<bedrocktools::sdk::Player*>(rawPlayer);
    if (!player) return nullptr;
    auto* dimension = player->dimension();
    return dimension ? static_cast<void*>(dimension->blockSource()) : nullptr;
}

std::pair<std::string_view, std::string_view> splitNamespace(std::string_view name) {
    const auto colon = name.find(':');
    if (colon == std::string_view::npos) return {{}, name};
    return {name.substr(0, colon), name.substr(colon + 1)};
}

std::string normalizedOre(std::string_view name) {
    auto [nameSpace, path] = splitNamespace(name);
    if (path.rfind("deepslate_", 0) == 0) path.remove_prefix(10);
    std::string out;
    if (!nameSpace.empty()) {
        out.append(nameSpace);
        out.push_back(':');
    }
    out.append(path);
    return out;
}

std::string normalizedWood(std::string_view name) {
    auto [nameSpace, path] = splitNamespace(name);
    if (path.rfind("stripped_", 0) == 0) path.remove_prefix(9);

    static constexpr std::array<std::string_view, 4> suffixes{
        "_log", "_wood", "_stem", "_hyphae"
    };
    for (auto suffix : suffixes) {
        if (endsWith(path, suffix)) {
            path = path.substr(0, path.size() - suffix.size());
            break;
        }
    }

    std::string out;
    if (!nameSpace.empty()) {
        out.append(nameSpace);
        out.push_back(':');
    }
    out.append(path);
    return out;
}

bool isOreLike(std::string_view name) {
    auto [_, path] = splitNamespace(name);
    return endsWith(path, "_ore") || name == "minecraft:ancient_debris";
}

bool isLogLike(std::string_view name) {
    auto [_, path] = splitNamespace(name);
    if (path.rfind("stripped_", 0) == 0) path.remove_prefix(9);
    return endsWith(path, "_log") || endsWith(path, "_wood") ||
           endsWith(path, "_stem") || endsWith(path, "_hyphae");
}

bool destroyBlockDetour(void* gameMode, const BlockPos& pos, std::uint8_t face) {
    if (!g_destroyBlockOriginal) return false;
    if (!g_module || !g_module->enabled) return g_destroyBlockOriginal(gameMode, pos, face);
    return g_module->handleDestroyBlock(gameMode, pos, face);
}
} // namespace

VeinMinerModule::VeinMinerModule()
    : Module("Vein Miner", "Breaks connected ores or logs after you mine the first block.") {
    g_module = this;
}

VeinMinerModule::~VeinMinerModule() {
    clearQueue();
    if (m_tickSubscription != 0) {
        bedrocktools::events::bus().unsubscribe(m_tickSubscription);
        m_tickSubscription = 0;
    }
    if (m_hook) {
        bedrocktools::hooks::remove(m_hook);
        m_hook = nullptr;
        g_destroyBlockOriginal = nullptr;
    }
    if (g_module == this) g_module = nullptr;
}

void VeinMinerModule::onInit() {
    const auto destroyAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::SurvivalModeDestroyBlock);
    if (destroyAddress) m_patchTarget = reinterpret_cast<void*>(destroyAddress);

    const auto getBlockAddress = bedrocktools::memory::resolve(
        bedrocktools::memory::SignatureId::BlockSourceGetBlock);
    if (getBlockAddress) g_getBlock = reinterpret_cast<GetBlockFn>(getBlockAddress);

    if (m_tickSubscription == 0) {
        m_tickSubscription = bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
            [](auto& event) {
                if (g_module) g_module->handleLocalPlayerTick(event.player);
            });
    }
}

void VeinMinerModule::installHook() {
    if (m_hook || !m_patchTarget || !g_getBlock) return;
    m_hook = bedrocktools::hooks::install(
        m_patchTarget,
        reinterpret_cast<void*>(destroyBlockDetour),
        reinterpret_cast<void**>(&g_destroyBlockOriginal));
    if (!m_hook) g_destroyBlockOriginal = nullptr;
}

void VeinMinerModule::onEnable() {
    installHook();
}

void VeinMinerModule::onDisable() {
    clearQueue();
}

void VeinMinerModule::clearQueue() {
    m_queue.clear();
    m_originBlockName.clear();
    m_activeGameMode = nullptr;
    m_activePlayer = nullptr;
}

bool VeinMinerModule::acceptsOrigin(std::string_view name) const {
    if (name.empty() || isAir(name)) return false;
    switch (m_mode) {
        case Mode::Ores: return isOreLike(name);
        case Mode::Logs: return isLogLike(name);
        case Mode::SameBlock: return true;
    }
    return false;
}

bool VeinMinerModule::belongsToVein(std::string_view candidate, std::string_view origin) const {
    if (candidate.empty() || isAir(candidate)) return false;
    switch (m_mode) {
        case Mode::Ores:
            return isOreLike(candidate) && normalizedOre(candidate) == normalizedOre(origin);
        case Mode::Logs:
            return isLogLike(candidate) && normalizedWood(candidate) == normalizedWood(origin);
        case Mode::SameBlock:
            return candidate == origin;
    }
    return false;
}

bool VeinMinerModule::handleDestroyBlock(void* gameMode, const BlockPos& pos, std::uint8_t face) {
    if (!g_destroyBlockOriginal) return false;

    void* player = playerFromGameMode(gameMode);
    void* region = regionFromPlayer(player);

    // Capture the block before Minecraft turns it into air.
    std::string originName;
    if (region) {
        const auto name = blockName(region, pos);
        if (!name.empty()) originName.assign(name);
    }

    const bool destroyed = g_destroyBlockOriginal(gameMode, pos, face);
    if (!destroyed || !enabled || !region || !acceptsOrigin(originName)) return destroyed;

    clearQueue();
    m_activeGameMode = gameMode;
    m_activePlayer = player;
    m_originBlockName = originName;
    buildQueue(region, pos, originName, face);
    return destroyed;
}

void VeinMinerModule::buildQueue(void* region, const BlockPos& origin,
                                 std::string_view originName, std::uint8_t face) {
    if (!region || originName.empty()) return;

    const int maxBlocks = std::clamp(m_maxBlocks, 1, 128);
    if (maxBlocks <= 1) return;

    std::array<BlockPos, 26> offsets{};
    int offsetCount = 0;
    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dz = -1; dz <= 1; ++dz) {
                if (dx == 0 && dy == 0 && dz == 0) continue;
                if (!m_diagonalConnections && std::abs(dx) + std::abs(dy) + std::abs(dz) != 1) continue;
                offsets[offsetCount++] = {dx, dy, dz};
            }
        }
    }

    std::vector<BlockPos> frontier;
    frontier.reserve(static_cast<std::size_t>(maxBlocks));
    frontier.push_back(origin);

    std::unordered_set<BlockPos, PosHash, PosEqual> visited;
    visited.reserve(static_cast<std::size_t>(maxBlocks) * 4);
    visited.insert(origin);

    std::size_t cursor = 0;
    while (cursor < frontier.size() && static_cast<int>(m_queue.size()) < maxBlocks - 1) {
        const BlockPos current = frontier[cursor++];
        for (int i = 0; i < offsetCount && static_cast<int>(m_queue.size()) < maxBlocks - 1; ++i) {
            const BlockPos next{
                current.x + offsets[i].x,
                current.y + offsets[i].y,
                current.z + offsets[i].z,
            };

            if (!visited.insert(next).second) continue;
            const auto candidate = blockName(region, next);
            if (!belongsToVein(candidate, originName)) continue;

            frontier.push_back(next);
            m_queue.push_back({next, face});
        }
    }
}

void VeinMinerModule::handleLocalPlayerTick(void* player) {
    if (!enabled || m_queue.empty() || !g_destroyBlockOriginal || !m_activeGameMode) return;
    if (!player || player != m_activePlayer) {
        clearQueue();
        return;
    }

    void* region = regionFromPlayer(player);
    if (!region) {
        clearQueue();
        return;
    }

    const int perTick = std::clamp(m_blocksPerTick, 1, 8);
    for (int i = 0; i < perTick && !m_queue.empty(); ++i) {
        const QueuedBlock queued = m_queue.front();
        m_queue.pop_front();

        const auto currentName = blockName(region, queued.pos);
        if (belongsToVein(currentName, m_originBlockName)) {
            // SurvivalMode::destroyBlock checks GameMode destroy progress. A queued block
            // was not held by the player long enough, so temporarily mark the break as
            // complete, execute Minecraft's original destruction path, then restore state.
            float& oldProgress = bedrocktools::sdk::field<float>(
                m_activeGameMode, bedrocktools::sdk::offsets::GameMode::mOldDestroyProgress);
            float& progress = bedrocktools::sdk::field<float>(
                m_activeGameMode, bedrocktools::sdk::offsets::GameMode::mDestroyProgress);
            const float savedOldProgress = oldProgress;
            const float savedProgress = progress;
            oldProgress = 1.0f;
            progress = 1.0f;
            g_destroyBlockOriginal(m_activeGameMode, queued.pos, queued.face);
            oldProgress = savedOldProgress;
            progress = savedProgress;
        }
    }

    if (m_queue.empty()) clearQueue();
}

void VeinMinerModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (j.contains("mode")) {
        try {
            int value = 0;
            if (j["mode"].is_string()) {
                value = std::stoi(j["mode"].get<std::string>());
            } else if (j["mode"].is_number_integer()) {
                value = j["mode"].get<int>();
            }
            m_mode = static_cast<Mode>(std::clamp(value, 0, 2));
        } catch (...) {
            m_mode = Mode::Ores;
        }
    }

    if (j.contains("maxBlocks")) m_maxBlocks = std::clamp(j["maxBlocks"].get<int>(), 1, 128);
    if (j.contains("blocksPerTick")) m_blocksPerTick = std::clamp(j["blocksPerTick"].get<int>(), 1, 8);
    if (j.contains("diagonalConnections")) m_diagonalConnections = j["diagonalConnections"].get<bool>();
}

void VeinMinerModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    // BedrockTools' menu parser turns this encoded string into a Radio config:
    // selectedIndex,option0,option1,option2
    j["mode"] = std::to_string(static_cast<int>(m_mode)) + ",Ores,Logs,Same Block";
    j["maxBlocks"] = m_maxBlocks;
    j["blocksPerTick"] = m_blocksPerTick;
    j["diagonalConnections"] = m_diagonalConnections;
}
