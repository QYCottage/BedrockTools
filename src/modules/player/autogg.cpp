#include "autogg.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include "core/memory/Hooks.hpp"
#include "core/GameHooks.hpp"
#include "core/DelayedAction.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <pl/ModMenuConfig.hpp>
#include <algorithm>
#include <string>
#include <memory>
#include <new>
#include <regex>

AutoGG* AutoGG::instance = nullptr;

static bedrocktools::core::DelayedAction sGGAction;

AutoGG::AutoGG() : Module("AutoGG", "Automatically says GG at the end of a game.") {
    instance = this;
    showInMenu = true;
}

AutoGG::~AutoGG() {
    if (instance == this) {
        sGGAction.cancel();
        instance = nullptr;
    }
}

static void* (*sendToServer)(void* sender, void* packet) = nullptr;
static void* (*getPacketSender)(void* clientInstance) = nullptr;
static void* (*getLocalPlayer)(void* clientInstance) = nullptr;
static std::shared_ptr<void> (*createPacket)(int id) = nullptr;

static void scheduleGG() {
    AutoGG* mod = AutoGG::instance;
    if (!mod || !mod->enabled || !getLocalPlayer) return;
    void* client = bedrocktools::core::gamehooks::clientInstance();
    void* player = client ? getLocalPlayer(client) : nullptr;
    sGGAction.schedule(client, player, mod->delaySeconds, mod->ggMessage);
}

static void sendGGCommand(const std::string& message) {
    if (!AutoGG::instance || !AutoGG::instance->enabled) return;
    
    
    if (!sendToServer) sendToServer = (decltype(sendToServer)) bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LoopbackPacketSenderSendToServer);
    if (!getPacketSender) getPacketSender = (decltype(getPacketSender)) bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientInstanceGetPacketSender);
    if (!createPacket) createPacket = (decltype(createPacket)) bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::MinecraftPacketsCreatePacket);
    
    if (!createPacket || !sendToServer || !getPacketSender || !bedrocktools::core::gamehooks::clientInstance()) return;

    std::shared_ptr<void> pktSp = createPacket(9); 
    
    void* pkt = pktSp.get();
    if (!pkt) return;
    
    uintptr_t payload = (uintptr_t)pkt + bedrocktools::sdk::offsets::Packet::Size;
    
    std::string* msgOnlyStr = reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::MessageOnly::mMessage);
    msgOnlyStr->~basic_string();

    *reinterpret_cast<uint32_t*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::mVariantIndex) = 1;

    *reinterpret_cast<uint8_t*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::AuthorAndMessage::mType) = 1;
    new ((void*)(payload + bedrocktools::sdk::offsets::TextPacketPayload::AuthorAndMessage::mAuthor)) std::string("");
    new ((void*)(payload + bedrocktools::sdk::offsets::TextPacketPayload::AuthorAndMessage::mMessage)) std::string(message);
    
    
    void* sender = getPacketSender(bedrocktools::core::gamehooks::clientInstance());
    if (sender) {
        sendToServer(sender, pkt);
    }
}

static std::string getTextPacketMessage(void* packet) {
    uintptr_t payload = (uintptr_t)packet + bedrocktools::sdk::offsets::Packet::Size;
    uint32_t variantIndex = *reinterpret_cast<uint32_t*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::mVariantIndex);
    
    if (variantIndex == 1) {
        return *reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::AuthorAndMessage::mMessage);
    } else if (variantIndex == 0 || variantIndex == 2) {
        return *reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::TextPacketPayload::MessageOnly::mMessage);
    }
    return "";
}

static bool checkTitle(std::string const& text) {
    if (text.empty() || !AutoGG::instance) return false;
    
    if (text.find("\xC2\xA7" "f\xC2\xA7" "aYou won the game!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "f\xC2\xA7" "cYou lost the game!") != std::string::npos) return true;
    if (text == "   ") return true;
    if (text.find("Team\xC2\xA7" "r\xC2\xA7" "a won the game!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "bHiders\xC2\xA7" "r\xC2\xA7" "f Win") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "eSeekers\xC2\xA7" "r\xC2\xA7" "f Win") != std::string::npos) return true;
    if (text.find("Finished") != std::string::npos) return true;
    if (text.find("Out of Time!") != std::string::npos) return true;

    static std::regex const galaxiteRegex(
        R"(Is The \xC2\xA76\xC2\xA7l(Chronos|Rush) (Champion|Champions)!)",
        std::regex::optimize
    );
    if (std::regex_search(text, galaxiteRegex)) return true;

    if (text.find("\xC2\xA7" "aYou Win!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "cGame Over!") != std::string::npos) return true;
    
    
    if (text == "\xC2\xA7" "cYou died!" || text == "\xC2\xA7" "7You're spectating the \xC2\xA7" "as\xC2\xA7" "eh\xC2\xA7" "6o\xC2\xA7" "cw\xC2\xA7" "7!") return true;
    
    return false;
}

static bool checkText(std::string const& text) {
    if (text.empty() || !AutoGG::instance) return false;

    if (text.find("Finished") != std::string::npos) return true;
    if (text.find("Out of Time!") != std::string::npos) return true;
    if (text.find("You won the game!") != std::string::npos) return true;
    if (text.find("You lost the game!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "aYou Win!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "cGame Over!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "c\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "c\xC2\xA7" "lGame OVER!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "r\xC2\xA7" "c\xC2\xA7" "lGame OVER!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "a won the game!") != std::string::npos) return true;
    if (text.find("\xC2\xA7" "a has won the game!") != std::string::npos) return true;

    if (text == "Game OVER!" || text == "You are out of lives!") return true;
    
    return false;
}

static void (*onHandleSetTitlePacket_orig)(void*, void*, void*) = nullptr;
static void onHandleSetTitlePacket(void* handler, void* source, void* packet) {
    if (onHandleSetTitlePacket_orig) {
        onHandleSetTitlePacket_orig(handler, source, packet);
    }

    if (AutoGG::instance && AutoGG::instance->enabled) {
        uintptr_t payload = (uintptr_t)packet + bedrocktools::sdk::offsets::Packet::Size;
        int packetType = *reinterpret_cast<int*>(payload + bedrocktools::sdk::offsets::SetTitlePacketPayload::mType);
        
        if (packetType == 0 || packetType == 1) {
            std::string text = *reinterpret_cast<std::string*>(payload + bedrocktools::sdk::offsets::SetTitlePacketPayload::mTitleText);
            if (checkTitle(text)) {
                scheduleGG();
            }
        }
    }
}

static void (*onHandleTextPacket_orig)(void*, void*, void*) = nullptr;
static void onHandleTextPacket(void* handler, void* source, void* packet) {
    if (onHandleTextPacket_orig) {
        onHandleTextPacket_orig(handler, source, packet);
    }

    if (AutoGG::instance && AutoGG::instance->enabled) {
        std::string message = getTextPacketMessage(packet);
        if (checkText(message)) {
            scheduleGG();
        }
    }
}

static void (*onChangeDimension_orig)(void*, void*) = nullptr;
static void onChangeDimension(void* player, void* packet) {
    sGGAction.cancel();
    if (onChangeDimension_orig) onChangeDimension_orig(player, packet);
}

void AutoGG::onInit() {
    getLocalPlayer = reinterpret_cast<decltype(getLocalPlayer)>(bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientInstanceGetLocalPlayer));
    bedrocktools::events::bus().subscribe<bedrocktools::events::ClientInstanceUpdateEvent>([](auto& event) {
        if (!AutoGG::instance || !AutoGG::instance->enabled) {
            sGGAction.cancel();
            return;
        }
        void* client = event.clientInstance;
        void* player = client && getLocalPlayer ? getLocalPlayer(client) : nullptr;
        auto message = sGGAction.take(client, player);
        if (message) sendGGCommand(*message);
    });

    uintptr_t setTitleAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientNetworkHandlerHandleSetTitle);
    if (setTitleAddr) {
        bedrocktools::hooks::install((void*)setTitleAddr, (void*)onHandleSetTitlePacket, (void**)&onHandleSetTitlePacket_orig);
    }
    
    uintptr_t setTxtAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::ClientNetworkHandlerHandleText);
    if (setTxtAddr) {
        bedrocktools::hooks::install((void*)setTxtAddr, (void*)onHandleTextPacket, (void**)&onHandleTextPacket_orig);
    }

    uintptr_t dimensionAddr = bedrocktools::memory::resolve(bedrocktools::memory::SignatureId::LocalPlayerChangeDimension);
    if (dimensionAddr) {
        bedrocktools::hooks::install((void*)dimensionAddr, (void*)onChangeDimension, (void**)&onChangeDimension_orig);
    }
}

void AutoGG::onDisable() {
    sGGAction.cancel();
}

void AutoGG::onMenuRegistered() {
    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("general").category("general", "General");

    ConfigNodeV2 message;
    message.id = "ggMessage";
    message.key = "ggMessage";
    message.category = "general";
    message.title = "GG Message";
    message.type = ConfigControlTypeV2::Text;
    schema.node(std::move(message));

    ConfigNodeV2 delay;
    delay.id = "delaySeconds";
    delay.key = "delaySeconds";
    delay.category = "general";
    delay.title = "GG Delay";
    delay.description = "Seconds to wait after a game ends before sending GG. Set 0 for no delay.";
    delay.type = ConfigControlTypeV2::SliderInt;
    delay.minValue = "0";
    delay.maxValue = "10";
    delay.step = "1";
    delay.unit = " s";
    schema.node(std::move(delay));

    ConfigNodeV2 keybindNode;
    keybindNode.id = "keybind";
    keybindNode.key = "keybind";
    keybindNode.category = "general";
    keybindNode.title = "Keybind";
    keybindNode.type = ConfigControlTypeV2::Keybind;
    schema.node(std::move(keybindNode));
    setConfigSchemaJson(moduleId, schema.toJson());
}

void AutoGG::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    if (j.contains("ggMessage")) ggMessage = j["ggMessage"].get<std::string>();
    if (j.contains("delaySeconds") && j["delaySeconds"].is_number_integer()) {
        delaySeconds = j["delaySeconds"].get<int>();
    }
    delaySeconds = std::clamp(delaySeconds, 0, 10);
}

void AutoGG::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["ggMessage"] = ggMessage;
    j["delaySeconds"] = delaySeconds;
}
