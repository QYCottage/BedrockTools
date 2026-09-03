#pragma once

#include "../Module.hpp"
#include <cstdint>
#include <string>
#include <mutex>
#include <atomic>

class HiveUtilsModule : public Module {
public:
    HiveUtilsModule();
    ~HiveUtilsModule() override;

    void onInit() override;
    void onEnable() override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    bool showInLegacyMenu(std::string_view key) const override;
    void onMenuRegistered() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    bool useHub = false;
    bool autoRequeue = true;
    bool autoRequeueSoloMode = false;
    bool autoRequeueTeamElimination = true;
    bool autoRequeueGameOver = true;
    bool roleMurderer = false;
    bool roleSheriff = false;
    bool roleInnocent = false;
    bool roleHider = false;
    bool roleSeeker = false;
    bool roleDeath = false;
    bool roleRunner = false;
    bool deathCountEnabled = false;
    int deathCountLimit = 5;
    bool copyCustomServerCode = false;
    bool copyCustomServerCodeIncludeCommand = false;
    bool hidePromoMessages = false;
    bool hideUnusedUnlocks = false;
    bool hidePlayerJoined = false;
    bool hideUnrankedPlayerMessages = false;
    bool hideHivePlusMessages = false;
    bool hideNoTeaming = false;
    bool autoAcceptFriend = false;
    bool autoAcceptParty = false;
    bool autoMapVote = false;
    std::string autoMapVoteRules;
    bool announceVote = false;
    std::string announceVoteMessage = "@here vote for {map}!";
    bool mapAvoider = false;
    std::string mapAvoiderRules;
    int requeueKeybind = 0;
    std::string uiMapVoteGame = "bed";
    std::string uiMapVoteVariant = "REGULAR";
    std::string uiMapAvoidGame = "bed";
    std::string uiMapAvoidVariant = "REGULAR";
    nlohmann::json mapVotePreferences = nlohmann::json::object();
    nlohmann::json mapAvoidPreferences = nlohmann::json::object();

    void publishMenuSchema();
    void refreshMapData(bool vote, bool force);
    std::string mapVoteRulesSnapshot() const;
    std::string mapAvoidRulesSnapshot() const;

    static HiveUtilsModule* instance;

private:
    mutable std::mutex mMapConfigMutex;
    std::mutex mMenuSchemaMutex;
    std::atomic_bool mMenuRegistered{false};
};
