#pragma once

#include "../Module.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/sdk/Types.hpp>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

class VeinMinerModule final : public Module {
public:
    enum class Mode : int {
        Ores = 0,
        Logs = 1,
        SameBlock = 2,
    };

    VeinMinerModule();
    ~VeinMinerModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool handleDestroyBlock(void* gameMode, const bedrocktools::sdk::BlockPos& pos, std::uint8_t face);
    void handleLocalPlayerTick(void* player);

private:
    struct QueuedBlock {
        bedrocktools::sdk::BlockPos pos{};
        std::uint8_t face{};
    };

    void installHook();
    void clearQueue();
    void buildQueue(void* region, const bedrocktools::sdk::BlockPos& origin,
                    std::string_view originName, std::uint8_t face);
    bool acceptsOrigin(std::string_view name) const;
    bool belongsToVein(std::string_view candidate, std::string_view origin) const;

    bedrocktools::hooks::Handle m_hook = nullptr;
    void* m_patchTarget = nullptr;
    std::uint64_t m_tickSubscription = 0;

    void* m_activeGameMode = nullptr;
    void* m_activePlayer = nullptr;
    std::string m_originBlockName;
    std::deque<QueuedBlock> m_queue;

    Mode m_mode = Mode::Ores;
    int m_maxBlocks = 32;
    int m_blocksPerTick = 1;
    bool m_diagonalConnections = true;
};
