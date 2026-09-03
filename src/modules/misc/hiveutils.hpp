#pragma once

#include "../Module.hpp"
#include <cstdint>
#include <string>

class HiveUtilsModule : public Module {
public:
    HiveUtilsModule();
    ~HiveUtilsModule() override;

    void onInit() override;
    void onEnable() override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
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

    static HiveUtilsModule* instance;
};
