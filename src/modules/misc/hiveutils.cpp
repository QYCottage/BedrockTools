#include "hiveutils.hpp"
#include "hivemaps.hpp"
#include "core/GameHooks.hpp"
#include "core/memory/Hooks.hpp"
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <pl/Platform.hpp>
#include <pl/ModMenuConfig.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <regex>
#include <set>
#include <string_view>
#include <vector>

HiveUtilsModule* HiveUtilsModule::instance = nullptr;

namespace {

using namespace bedrocktools;

using SendToServerFn = void* (*)(void*, void*);
using GetPacketSenderFn = void* (*)(void*);
using CreatePacketFn = std::shared_ptr<void> (*)(int);
using SetTitleHandlerFn = void (*)(void*, void*, void*);
using TextHandlerFn = void (*)(void*, void*, void*);
using ChangeDimensionFn = void (*)(void*, void*);
using ModalReceiveFn = void (*)(void*, void*, void*, const std::shared_ptr<void>&);

SendToServerFn sSendToServer = nullptr;
GetPacketSenderFn sGetPacketSender = nullptr;
CreatePacketFn sCreatePacket = nullptr;
SetTitleHandlerFn sSetTitleOriginal = nullptr;
TextHandlerFn sTextOriginal = nullptr;
ChangeDimensionFn sChangeDimensionOriginal = nullptr;
ModalReceiveFn sModalOriginal = nullptr;

std::chrono::steady_clock::time_point sLastRequeue{};
std::mutex sRequeueMutex;
std::string sCurrentGame;
std::string sCurrentTeamColor;
std::string sLastCopiedCode;
bool sListeningForConnection = false;
int sDeathCount = 0;

constexpr int kTextPacketId = 0x09;
constexpr int kCommandRequestPacketId = 0x4D;
constexpr int kModalFormRequestPacketId = 0x64;
constexpr int kModalFormResponsePacketId = 0x65;
constexpr int kRequeueCooldownMs = 3000;

std::string trim(std::string value) {
    auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c) != 0; });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c) != 0; }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string stripColorCodes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0xC2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA7) {
            i += 3;
            continue;
        }
        if (c == 0xA7 && i + 1 < text.size()) {
            i += 2;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t end = value.find(delimiter, start);
        if (end == std::string_view::npos) end = value.size();
        std::string part = trim(std::string(value.substr(start, end - start)));
        if (!part.empty()) result.push_back(std::move(part));
        if (end == value.size()) break;
        start = end + 1;
    }
    return result;
}

std::vector<std::string> mapsForGame(std::string_view rules, std::string_view game) {
    if (rules.empty() || game.empty()) return {};
    const std::string wanted = lower(trim(std::string(game)));
    std::vector<std::pair<std::string, std::vector<std::string>>> parsed;
    for (const auto& entry : split(rules, ';')) {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos) continue;
        std::string key = lower(trim(entry.substr(0, equals)));
        std::vector<std::string> values = split(std::string_view(entry).substr(equals + 1), '|');
        if (!key.empty() && !values.empty()) parsed.emplace_back(std::move(key), std::move(values));
    }
    auto findKey = [&](const std::string& key) -> std::vector<std::string> {
        for (const auto& [candidate, values] : parsed) {
            if (candidate == key) return values;
        }
        return {};
    };
    auto exact = findKey(wanted);
    if (!exact.empty()) return exact;
    const std::size_t dash = wanted.find('-');
    if (dash == std::string::npos) {
        for (const char* suffix : {"-solos", "-solo", "-regular", "-duos", "-squads"}) {
            auto fallback = findKey(wanted + suffix);
            if (!fallback.empty()) return fallback;
        }
    } else {
        const std::string suffix = wanted.substr(dash + 1);
        if (suffix == "solos" || suffix == "solo" || suffix == "regular") {
            auto fallback = findKey(wanted.substr(0, dash));
            if (!fallback.empty()) return fallback;
        }
    }
    return findKey("*");
}

bool isKnownHiveGame(std::string_view value) {
    std::string normalized = lower(trim(std::string(value)));
    if (normalized.empty()) return false;
    static constexpr std::string_view prefixes[] = {
        "hub", "replay", "bed", "sky", "wars", "sg", "dr", "hide", "murder", "ctf",
        "drop", "ground", "build", "bridge", "grav", "party", "mob", "gi", "just", "arcade"
    };
    for (auto prefix : prefixes) {
        if (normalized == prefix || normalized.rfind(std::string(prefix) + "-", 0) == 0) return true;
    }
    return false;
}

void ensurePacketFunctions() {
    if (!sSendToServer) sSendToServer = reinterpret_cast<SendToServerFn>(memory::resolve(memory::SignatureId::LoopbackPacketSenderSendToServer));
    if (!sGetPacketSender) sGetPacketSender = reinterpret_cast<GetPacketSenderFn>(memory::resolve(memory::SignatureId::ClientInstanceGetPacketSender));
    if (!sCreatePacket) sCreatePacket = reinterpret_cast<CreatePacketFn>(memory::resolve(memory::SignatureId::MinecraftPacketsCreatePacket));
}

void sendPacket(void* packet) {
    ensurePacketFunctions();
    void* client = core::gamehooks::clientInstance();
    if (!packet || !client || !sSendToServer || !sGetPacketSender) return;
    void* sender = sGetPacketSender(client);
    if (sender) sSendToServer(sender, packet);
}

void sendCommand(const std::string& command) {
    ensurePacketFunctions();
    if (!sCreatePacket) return;
    std::shared_ptr<void> packet = sCreatePacket(kCommandRequestPacketId);
    if (!packet) return;
    auto* payload = reinterpret_cast<std::byte*>(packet.get()) + sdk::offsets::Packet::Size;
    *reinterpret_cast<std::string*>(payload + sdk::offsets::CommandRequestPacketPayload::mCommand) = command;
    *reinterpret_cast<std::uint8_t*>(payload + sdk::offsets::CommandRequestPacketPayload::mOrigin + sdk::offsets::CommandOriginData::mType) = 0;
    *reinterpret_cast<bool*>(payload + sdk::offsets::CommandRequestPacketPayload::mInternalSource) = true;
    sendPacket(packet.get());
}

void sendChat(const std::string& message) {
    ensurePacketFunctions();
    if (!sCreatePacket || message.empty()) return;
    std::shared_ptr<void> packet = sCreatePacket(kTextPacketId);
    if (!packet) return;
    auto* payload = reinterpret_cast<std::byte*>(packet.get()) + sdk::offsets::Packet::Size;
    auto* oldMessage = reinterpret_cast<std::string*>(payload + sdk::offsets::TextPacketPayload::MessageOnly::mMessage);
    oldMessage->~basic_string();
    *reinterpret_cast<std::uint32_t*>(payload + sdk::offsets::TextPacketPayload::mVariantIndex) = 1;
    *reinterpret_cast<std::uint8_t*>(payload + sdk::offsets::TextPacketPayload::AuthorAndMessage::mType) = 1;
    new (payload + sdk::offsets::TextPacketPayload::AuthorAndMessage::mAuthor) std::string();
    new (payload + sdk::offsets::TextPacketPayload::AuthorAndMessage::mMessage) std::string(message);
    sendPacket(packet.get());
}

bool canRequeue() {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !mod->enabled || sCurrentGame.empty()) return false;
    if (lower(sCurrentGame).find("hub") != std::string::npos) return false;
    std::lock_guard<std::mutex> lock(sRequeueMutex);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - sLastRequeue).count();
    if (elapsed < kRequeueCooldownMs) return false;
    sLastRequeue = now;
    return true;
}

void requeue() {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !canRequeue()) return;
    sendCommand(mod->useHub ? "/hub" : "/q " + sCurrentGame);
}

void resetRequeueCooldown() {
    std::lock_guard<std::mutex> lock(sRequeueMutex);
    sLastRequeue = {};
}

void requestConnection() {
    sListeningForConnection = true;
    sendCommand("/connection");
}

std::string textPacketMessage(void* packet) {
    if (!packet) return {};
    auto* payload = reinterpret_cast<std::byte*>(packet) + sdk::offsets::Packet::Size;
    const std::uint32_t variant = *reinterpret_cast<std::uint32_t*>(payload + sdk::offsets::TextPacketPayload::mVariantIndex);
    if (variant == 1) return *reinterpret_cast<std::string*>(payload + sdk::offsets::TextPacketPayload::AuthorAndMessage::mMessage);
    if (variant == 0 || variant == 2) return *reinterpret_cast<std::string*>(payload + sdk::offsets::TextPacketPayload::MessageOnly::mMessage);
    return {};
}

bool captureConnectionMessage(const std::string& message) {
    if (!sListeningForConnection) return false;
    static constexpr std::string_view marker = "You are connected to server name ";
    const std::size_t pos = message.find(marker);
    if (pos == std::string::npos) {
        if (message.find("You are connected") != std::string::npos || message.find(" connected to ") != std::string::npos) return true;
        return false;
    }
    std::string server = message.substr(pos + marker.size());
    const std::size_t newline = server.find_first_of("\r\n");
    if (newline != std::string::npos) server.resize(newline);
    server = std::regex_replace(server, std::regex("\\d+"), "");
    server = trim(server);
    sCurrentGame = isKnownHiveGame(server) ? server : std::string();
    sListeningForConnection = false;
    return true;
}

bool titleShouldRequeue(const std::string& text) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !mod->autoRequeue || !mod->autoRequeueSoloMode) return false;
    return text.find("\xC2\xA7" "cYou died!") != std::string::npos ||
           text.find("\xC2\xA7" "7You're spectating the") != std::string::npos;
}

bool textShouldRequeue(const std::string& text) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod) return false;
    if (mod->autoRequeue && mod->autoRequeueGameOver && text == "\xC2\xA7" "c\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "c\xC2\xA7" "lGame OVER!") return true;
    if (mod->autoRequeue && mod->autoRequeueTeamElimination) {
        if (text.length() > 27 && text.substr(12, 15) == "You are on the ") sCurrentTeamColor = text.substr(27, text.length() - 28);
        if (!sCurrentTeamColor.empty() && text.find("\xC2\xA7" "7has been \xC2\xA7" "cELIMINATED\xC2\xA7" "7!") != std::string::npos && text.find(sCurrentTeamColor) != std::string::npos) return true;
    }
    if (mod->autoRequeue && mod->autoRequeueSoloMode) {
        if (text.rfind("\xC2\xA7" "a\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "eYou finished all maps and came in", 0) == 0 ||
            text.rfind("\xC2\xA7" "a\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "eYou finished in", 0) == 0) return true;
    }
    if (mod->roleMurderer && text == "\xC2\xA7" "c\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "c\xC2\xA7" "lMurderer") return true;
    if (mod->roleSheriff && text == "\xC2\xA7" "9\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "9\xC2\xA7" "lSheriff") return true;
    if (mod->roleInnocent && text == "\xC2\xA7" "a\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "a\xC2\xA7" "lInnocent") return true;
    if (mod->roleDeath && text == "\xC2\xA7" "d\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "bYou are a \xC2\xA7" "cDeath") return true;
    if (mod->roleRunner && text == "\xC2\xA7" "d\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "bYou are a \xC2\xA7" "aRunner") return true;
    if (mod->roleHider && text == "\xC2\xA7" "e\xC2\xA7" "l\xC2\xBB \xC2\xA7" "rYou are a \xC2\xA7" "eHIDER") return true;
    if (mod->roleSeeker && text == "\xC2\xA7" "c\xC2\xA7" "l\xC2\xBB \xC2\xA7" "rYou are a \xC2\xA7" "cSEEKER") return true;
    return false;
}

void handleDeathCounter(const std::string& text) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || lower(sCurrentGame) != "dr") return;
    if (text == "\xC2\xA7" "a\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "bThe game has started! Run!") sDeathCount = 0;
    if (mod->deathCountEnabled && text == "\xC2\xA7" "c\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "cYou died!") {
        ++sDeathCount;
        if (sDeathCount >= std::max(1, mod->deathCountLimit)) {
            requeue();
            sDeathCount = 0;
        }
    }
}

void handleAnnounceVote(const std::string& message) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !mod->announceVote) return;
    std::string clean = trim(stripColorCodes(message));
    if (clean.rfind("» ", 0) == 0) clean.erase(0, std::string("» ").size());
    static constexpr std::string_view prefix = "You voted for ";
    if (clean.rfind(prefix, 0) != 0) return;
    std::string map = trim(clean.substr(prefix.size()));
    if (map.empty()) return;
    std::string output = mod->announceVoteMessage;
    for (std::size_t pos = output.find("{map}"); pos != std::string::npos; pos = output.find("{map}", pos + map.size())) output.replace(pos, 5, map);
    sendChat(output);
}

void handleMapAvoider(const std::string& message) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !mod->mapAvoider || sCurrentGame.empty()) return;
    const auto avoided = mapsForGame(mod->mapAvoidRulesSnapshot(), sCurrentGame);
    if (avoided.empty()) return;
    std::string clean = trim(stripColorCodes(message));
    const std::string cleanLower = lower(clean);
    const std::size_t won = cleanLower.find(" won with ");
    if (won == std::string::npos) return;
    std::string map = trim(clean.substr(0, won));
    if (map.rfind("» ", 0) == 0) map = trim(map.substr(std::string("» ").size()));
    const std::string mapLower = lower(map);
    for (const auto& configured : avoided) {
        const std::string configuredLower = lower(trim(configured));
        if (!configuredLower.empty() && (mapLower == configuredLower || mapLower.find(configuredLower) != std::string::npos || configuredLower.find(mapLower) != std::string::npos)) {
            requeue();
            return;
        }
    }
}

void handleAutoAccept(const std::string& message) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod) return;
    const std::string clean = trim(stripColorCodes(message));
    static constexpr std::string_view friendPrefix = "You received a friend invite from ";
    if (mod->autoAcceptFriend && clean.rfind(friendPrefix, 0) == 0) {
        std::string name = trim(clean.substr(friendPrefix.size()));
        while (!name.empty() && (name.back() == '.' || name.back() == '!' || name.back() == ' ')) name.pop_back();
        if (!name.empty()) sendCommand("/f accept \"" + name + "\"");
    }
    static constexpr std::string_view partySuffix = " wants you to join their party!";
    const std::size_t party = clean.find(partySuffix);
    if (mod->autoAcceptParty && party != std::string::npos) {
        std::string name = trim(clean.substr(0, party));
        const std::size_t marker = name.rfind("» ");
        if (marker != std::string::npos) name = trim(name.substr(marker + std::string("» ").size()));
        if (!name.empty()) sendCommand("/p accept \"" + name + "\"");
    }
}

bool shouldHideText(const std::string& message) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod) return false;
    if (mod->hidePromoMessages && message.find("\xC2\xA7" "6[\xC2\xA7" "e!\xC2\xA7" "6]") != std::string::npos) return true;
    if (mod->hideUnusedUnlocks && message == "\xC2\xA7" "a\xC2\xA7" "l\xC2\xBB \xC2\xA7" "rYou have unused unlocks in your Locker!") return true;
    if (mod->hidePlayerJoined && std::regex_search(message, std::regex("joined\\. \xC2\xA7" "8\\[\\d+/\\d+\\]"))) return true;
    if (mod->hideUnrankedPlayerMessages && message.find(" \xC2\xA7" "7\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r") != std::string::npos && message.rfind("\xC2\xA7" "7", 0) == 0) return true;
    if (mod->hideHivePlusMessages && message.find("\xC2\xA7" "8 [\xC2\xA7" "a+\xC2\xA7" "8] \xC2\xA7" "7\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r") != std::string::npos) return true;
    if (mod->hideNoTeaming && message == "\xC2\xA7" "c\xC2\xA7" "l\xC2\xBB \xC2\xA7" "r\xC2\xA7" "c\xC2\xA7" "lNo teaming! \xC2\xA7" "r\xC2\xA7" "6Teamers will be banned.") return true;
    return false;
}

void handleCustomServerTitle(const std::string& text) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !mod->copyCustomServerCode) return;
    std::string clean = trim(stripColorCodes(text));
    static constexpr std::string_view prefix = "Join Code:";
    if (clean.rfind(prefix, 0) != 0) return;
    std::string code = trim(clean.substr(prefix.size()));
    if (code.empty() || code == sLastCopiedCode) return;
    sLastCopiedCode = code;
    pl::platform::setClipboardText(mod->copyCustomServerCodeIncludeCommand ? "/cs " + code : code);
}

std::optional<int> preferredMapIndex(const std::vector<std::string>& maps) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod) return std::nullopt;
    const auto prefs = mapsForGame(mod->mapVoteRulesSnapshot(), sCurrentGame);
    if (prefs.empty()) return std::nullopt;
    std::vector<std::string> normalized;
    normalized.reserve(maps.size());
    for (const auto& map : maps) normalized.push_back(lower(trim(map)));
    for (const auto& preference : prefs) {
        const std::string wanted = lower(trim(preference));
        for (std::size_t i = 0; i < normalized.size(); ++i) {
            if (normalized[i] == wanted) return static_cast<int>(i);
        }
    }
    return std::nullopt;
}

void sendModalResponse(std::uint32_t formId, int buttonIndex) {
    ensurePacketFunctions();
    if (!sCreatePacket) return;
    std::shared_ptr<void> packet = sCreatePacket(kModalFormResponsePacketId);
    if (!packet) return;
    auto* base = reinterpret_cast<std::byte*>(packet.get());
    auto* payload = base + sdk::offsets::Packet::Size;
    std::memset(payload, 0, sdk::offsets::ModalFormResponsePacketPayload::Size);
    *reinterpret_cast<std::uint32_t*>(payload + sdk::offsets::ModalFormResponsePacketPayload::mFormId) = formId;
    *reinterpret_cast<std::int64_t*>(payload + sdk::offsets::ModalFormResponsePacketPayload::mJsonValue) = buttonIndex;
    *reinterpret_cast<std::uint8_t*>(payload + sdk::offsets::ModalFormResponsePacketPayload::mJsonValueType) = 1;
    *reinterpret_cast<bool*>(payload + sdk::offsets::ModalFormResponsePacketPayload::mJsonResponseHasValue) = true;
    sendPacket(packet.get());
}

bool handleMapVoteModal(void* packet) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !mod->enabled || !mod->autoMapVote || !packet || sCurrentGame.empty()) return false;
    auto* payload = reinterpret_cast<std::byte*>(packet) + sdk::offsets::Packet::Size;
    const std::uint32_t formId = *reinterpret_cast<std::uint32_t*>(payload + sdk::offsets::ModalFormRequestPacketPayload::mFormId);
    const std::string& formJson = *reinterpret_cast<std::string*>(payload + sdk::offsets::ModalFormRequestPacketPayload::mFormJson);
    if (!nlohmann::json::accept(formJson)) return false;
    nlohmann::json form = nlohmann::json::parse(formJson, nullptr, false);
    if (!form.is_object() || !form.contains("title") || !form["title"].is_string() || !form.contains("buttons") || !form["buttons"].is_array()) return false;
    if (lower(trim(stripColorCodes(form["title"].get<std::string>()))) != "choose map") return false;
    std::vector<std::string> maps;
    for (const auto& button : form["buttons"]) {
        if (!button.is_object() || !button.contains("text") || !button["text"].is_string()) continue;
        std::string name = button["text"].get<std::string>();
        const std::size_t newline = name.find('\n');
        if (newline != std::string::npos) name.resize(newline);
        name = trim(stripColorCodes(name));
        if (!name.empty()) maps.push_back(std::move(name));
    }
    auto index = preferredMapIndex(maps);
    if (!index) return false;
    sendModalResponse(formId, *index);
    return true;
}

void modalReceiveDetour(void* dispatcher, void* networkIdentifier, void* netEventCallback, const std::shared_ptr<void>& packet) {
    if (packet && handleMapVoteModal(packet.get())) return;
    if (sModalOriginal) sModalOriginal(dispatcher, networkIdentifier, netEventCallback, packet);
}

void setTitleDetour(void* handler, void* source, void* packet) {
    if (sSetTitleOriginal) sSetTitleOriginal(handler, source, packet);
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !mod->enabled || !packet) return;
    auto* payload = reinterpret_cast<std::byte*>(packet) + sdk::offsets::Packet::Size;
    const int type = *reinterpret_cast<int*>(payload + sdk::offsets::SetTitlePacketPayload::mType);
    const std::string text = *reinterpret_cast<std::string*>(payload + sdk::offsets::SetTitlePacketPayload::mTitleText);
    handleCustomServerTitle(text);
    if ((type == 0 || type == 1) && titleShouldRequeue(text)) requeue();
}

void textDetour(void* handler, void* source, void* packet) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (!mod || !mod->enabled) {
        if (sTextOriginal) sTextOriginal(handler, source, packet);
        return;
    }
    const std::string message = textPacketMessage(packet);
    if (captureConnectionMessage(message)) return;
    if (message.find("Could not connect") != std::string::npos || message.find("server is full") != std::string::npos) {
        resetRequeueCooldown();
        requeue();
    }
    handleAnnounceVote(message);
    handleDeathCounter(message);
    handleMapAvoider(message);
    handleAutoAccept(message);
    if (textShouldRequeue(message)) requeue();
    if (shouldHideText(message)) return;
    if (sTextOriginal) sTextOriginal(handler, source, packet);
}

void changeDimensionDetour(void* player, void* packet) {
    HiveUtilsModule* mod = HiveUtilsModule::instance;
    if (mod && mod->enabled && packet) {
        auto* payload = reinterpret_cast<std::byte*>(packet) + sdk::offsets::Packet::Size;
        const int dimension = *reinterpret_cast<int*>(payload + sdk::offsets::ChangeDimensionPacketPayload::mDimensionId);
        if (dimension == 0) requestConnection();
    }
    if (sChangeDimensionOriginal) sChangeDimensionOriginal(player, packet);
}


struct HiveGameOption {
    std::string_view id;
    std::string_view label;
};

constexpr std::array<HiveGameOption, 13> kHiveMapGames{{
    {"bed", "BedWars"},
    {"sky", "SkyWars"},
    {"wars", "Treasure Wars"},
    {"sg", "Survival Games"},
    {"dr", "DeathRun"},
    {"hide", "Hide and Seek"},
    {"murder", "Murder Mystery"},
    {"ctf", "Capture The Flag"},
    {"drop", "Block Drop"},
    {"ground", "Ground Wars"},
    {"build", "Build Battle"},
    {"bridge", "The Bridge"},
    {"grav", "Gravity"},
}};

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string normalizeVariant(std::string value) {
    value = upper(trim(std::move(value)));
    if (value.empty() || value == "SOLOS" || value == "SOLO") return "REGULAR";
    return value;
}

std::string mapRuleKey(std::string_view game, std::string_view variant) {
    std::string base = upper(bedrocktools::hive::apiGameId(game));
    std::string normalized = normalizeVariant(std::string(variant));
    if (normalized == "REGULAR") return base;
    return base + "-" + normalized;
}

std::string variantLabel(std::string_view variant) {
    std::string value = normalizeVariant(std::string(variant));
    if (value == "REGULAR") return "Regular";
    std::string result = lower(value);
    bool capitalize = true;
    for (char& c : result) {
        if (c == '_' || c == '-') {
            c = ' ';
            capitalize = true;
        } else if (capitalize) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capitalize = false;
        }
    }
    return result;
}

std::vector<std::string> jsonStringList(const nlohmann::json& object, std::string_view key) {
    std::vector<std::string> result;
    if (!object.is_object()) return result;
    auto it = object.find(std::string(key));
    if (it == object.end() || !it->is_array()) return result;
    std::set<std::string> seen;
    for (const auto& value : *it) {
        if (!value.is_string()) continue;
        std::string name = trim(value.get<std::string>());
        if (name.empty()) continue;
        std::string normalized = lower(name);
        if (seen.insert(normalized).second) result.push_back(std::move(name));
    }
    return result;
}

void setJsonStringList(nlohmann::json& object, std::string_view key, const std::vector<std::string>& values) {
    if (!object.is_object()) object = nlohmann::json::object();
    nlohmann::json list = nlohmann::json::array();
    std::set<std::string> seen;
    for (const auto& raw : values) {
        std::string value = trim(raw);
        if (value.empty()) continue;
        std::string normalized = lower(value);
        if (seen.insert(normalized).second) list.push_back(std::move(value));
    }
    object[std::string(key)] = std::move(list);
}

std::string canonicalMapRuleKey(std::string key) {
    key = upper(trim(std::move(key)));
    if (key.empty() || key == "*") return key;
    const std::size_t dash = key.find('-');
    if (dash == std::string::npos) return key;
    const std::string suffix = key.substr(dash + 1);
    if (suffix == "SOLOS" || suffix == "SOLO" || suffix == "REGULAR") key.resize(dash);
    return key;
}

nlohmann::json parseLegacyMapRules(std::string_view rules) {
    nlohmann::json result = nlohmann::json::object();
    for (const auto& entry : split(rules, ';')) {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos) continue;
        std::string key = canonicalMapRuleKey(entry.substr(0, equals));
        if (key.empty()) continue;
        std::vector<std::string> values = split(std::string_view(entry).substr(equals + 1), '|');
        if (values.empty()) continue;
        auto merged = jsonStringList(result, key);
        merged.insert(merged.end(), values.begin(), values.end());
        setJsonStringList(result, key, merged);
    }
    return result;
}

std::string serializeMapRules(const nlohmann::json& preferences) {
    if (!preferences.is_object()) return {};
    std::vector<std::string> keys;
    keys.reserve(preferences.size());
    for (auto it = preferences.begin(); it != preferences.end(); ++it) keys.push_back(it.key());
    std::sort(keys.begin(), keys.end());
    std::string result;
    for (const auto& key : keys) {
        auto values = jsonStringList(preferences, key);
        if (values.empty()) continue;
        if (!result.empty()) result.push_back(';');
        result += key;
        result.push_back('=');
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) result.push_back('|');
            result += values[i];
        }
    }
    return result;
}

std::vector<std::string> parseJsonStringArray(std::string_view value) {
    std::vector<std::string> result;
    try {
        auto parsed = nlohmann::json::parse(value);
        if (!parsed.is_array()) return result;
        std::set<std::string> seen;
        for (const auto& item : parsed) {
            if (!item.is_string()) continue;
            std::string text = trim(item.get<std::string>());
            if (text.empty()) continue;
            std::string normalized = lower(text);
            if (seen.insert(normalized).second) result.push_back(std::move(text));
        }
    } catch (...) {
    }
    return result;
}

std::string jsonArrayString(const std::vector<std::string>& values) {
    return nlohmann::json(values).dump();
}

bool menuTruthy(std::string_view value) {
    std::string normalized = lower(trim(std::string(value)));
    return normalized == "true" || normalized == "1" || normalized == "on" || normalized == "yes";
}

std::string mapStatusText(const bedrocktools::hive::MapSnapshot& snapshot) {
    if (snapshot.loading) return snapshot.maps.empty() ? "Loading maps from Hive..." : "Refreshing maps from Hive...";
    if (!snapshot.error.empty()) {
        if (!snapshot.maps.empty()) return snapshot.error + ". Showing cached maps.";
        return snapshot.error;
    }
    if (snapshot.maps.empty()) return "No map data loaded yet.";
    if (snapshot.stale) return "Showing cached map data. Refresh to check for updates.";
    return "Loaded " + std::to_string(snapshot.maps.size()) + " maps from Hive.";
}

std::vector<pl::modmenu::ConfigOptionV2> gameOptions() {
    std::vector<pl::modmenu::ConfigOptionV2> result;
    result.reserve(kHiveMapGames.size());
    for (const auto& game : kHiveMapGames) result.push_back({std::string(game.id), std::string(game.label)});
    return result;
}

std::vector<pl::modmenu::ConfigOptionV2> variantOptions(std::string_view game, std::string_view selected) {
    std::vector<std::string> variants = bedrocktools::hive::variantsForGame(game);
    const std::string wanted = normalizeVariant(std::string(selected));
    if (variants.empty()) variants.push_back(wanted);
    bool found = false;
    for (auto& variant : variants) {
        variant = normalizeVariant(std::move(variant));
        if (variant == wanted) found = true;
    }
    if (!found) variants.push_back(wanted);
    std::sort(variants.begin(), variants.end());
    variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
    std::vector<pl::modmenu::ConfigOptionV2> result;
    result.reserve(variants.size());
    for (const auto& variant : variants) result.push_back({variant, variantLabel(variant)});
    return result;
}

std::vector<pl::modmenu::ConfigOptionV2> mapOptions(
    const std::vector<bedrocktools::hive::MapInfo>& available,
    const std::vector<std::string>& selected) {
    std::vector<pl::modmenu::ConfigOptionV2> result;
    std::set<std::string> present;
    for (const auto& map : available) {
        std::string key = lower(map.name);
        if (!present.insert(key).second) continue;
        std::string detail;
        if (!map.season.empty() && upper(map.season) != "NO_SEASON") detail = variantLabel(map.season);
        result.push_back({map.name, map.name, detail});
    }
    for (const auto& saved : selected) {
        if (present.insert(lower(saved)).second) result.push_back({saved, saved + " (saved)", "Currently unavailable from the Hive map list"});
    }
    return result;
}

void installModalHook() {
    ensurePacketFunctions();
    if (!sCreatePacket) return;
    std::shared_ptr<void> packet = sCreatePacket(kModalFormRequestPacketId);
    if (!packet) return;
    auto* base = reinterpret_cast<std::byte*>(packet.get());
    void* dispatcher = *reinterpret_cast<void**>(base + sdk::offsets::Packet::mHandlerDispatcher);
    if (!dispatcher) return;
    void** vtable = *reinterpret_cast<void***>(dispatcher);
    if (!vtable) return;
    void* target = vtable[sdk::offsets::PacketHandlerDispatcher::HandlePacketVtableIndex];
    if (target) hooks::install(target, reinterpret_cast<void*>(modalReceiveDetour), reinterpret_cast<void**>(&sModalOriginal));
}

}

HiveUtilsModule::HiveUtilsModule()
    : Module("Hive Utils", "Hive utilities for requeueing, role skipping, chat cleanup, invites, custom servers, map voting and map avoidance.") {
    instance = this;
    showInMenu = true;
}

HiveUtilsModule::~HiveUtilsModule() {
    if (instance == this) instance = nullptr;
}

void HiveUtilsModule::onInit() {
    const std::uintptr_t title = memory::resolve(memory::SignatureId::ClientNetworkHandlerHandleSetTitle);
    if (title) hooks::install(reinterpret_cast<void*>(title), reinterpret_cast<void*>(setTitleDetour), reinterpret_cast<void**>(&sSetTitleOriginal));
    const std::uintptr_t text = memory::resolve(memory::SignatureId::ClientNetworkHandlerHandleText);
    if (text) hooks::install(reinterpret_cast<void*>(text), reinterpret_cast<void*>(textDetour), reinterpret_cast<void**>(&sTextOriginal));
    const std::uintptr_t dimension = memory::resolve(memory::SignatureId::LocalPlayerChangeDimension);
    if (dimension) hooks::install(reinterpret_cast<void*>(dimension), reinterpret_cast<void*>(changeDimensionDetour), reinterpret_cast<void**>(&sChangeDimensionOriginal));
    installModalHook();
}

void HiveUtilsModule::onEnable() {
    requestConnection();
}

void HiveUtilsModule::onKeybindEvent(const std::string& key, bool isDown) {
    if (key == "requeueKeybind") {
        if (isDown) requeue();
        return;
    }
    Module::onKeybindEvent(key, isDown);
}

bool HiveUtilsModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    bool republish = false;
    bool refreshVote = false;
    bool refreshAvoid = false;
    bool forceRefresh = false;
    {
        std::lock_guard lock(mMapConfigMutex);
        if (key == "uiMapVoteGame") {
            uiMapVoteGame = bedrocktools::hive::apiGameId(value);
            if (uiMapVoteGame.empty()) uiMapVoteGame = "bed";
            uiMapVoteVariant = "REGULAR";
            republish = true;
            refreshVote = true;
        } else if (key == "uiMapVoteVariant") {
            uiMapVoteVariant = normalizeVariant(std::string(value));
            republish = true;
        } else if (key == "uiMapAvoidGame") {
            uiMapAvoidGame = bedrocktools::hive::apiGameId(value);
            if (uiMapAvoidGame.empty()) uiMapAvoidGame = "bed";
            uiMapAvoidVariant = "REGULAR";
            republish = true;
            refreshAvoid = true;
        } else if (key == "uiMapAvoidVariant") {
            uiMapAvoidVariant = normalizeVariant(std::string(value));
            republish = true;
        } else if (key == "mapVoteSelection") {
            const std::string ruleKey = mapRuleKey(uiMapVoteGame, uiMapVoteVariant);
            setJsonStringList(mapVotePreferences, ruleKey, parseJsonStringArray(value));
            autoMapVoteRules = serializeMapRules(mapVotePreferences);
            republish = true;
        } else if (key == "mapAvoidSelection") {
            const std::string ruleKey = mapRuleKey(uiMapAvoidGame, uiMapAvoidVariant);
            setJsonStringList(mapAvoidPreferences, ruleKey, parseJsonStringArray(value));
            mapAvoiderRules = serializeMapRules(mapAvoidPreferences);
            republish = true;
        } else if (key == "refreshMapVoteMaps") {
            refreshVote = menuTruthy(value);
            forceRefresh = refreshVote;
        } else if (key == "refreshMapAvoidMaps") {
            refreshAvoid = menuTruthy(value);
            forceRefresh = refreshAvoid;
        } else if (key == "clearHiveMapCache") {
            if (menuTruthy(value)) {
                bedrocktools::hive::clearMapCache();
                refreshVote = true;
                refreshAvoid = true;
                forceRefresh = true;
                republish = true;
            }
        } else if (key == "autoMapVoteRules") {
            autoMapVoteRules = std::string(value);
            mapVotePreferences = parseLegacyMapRules(autoMapVoteRules);
            republish = true;
        } else if (key == "mapAvoiderRules") {
            mapAvoiderRules = std::string(value);
            mapAvoidPreferences = parseLegacyMapRules(mapAvoiderRules);
            republish = true;
        } else {
            return false;
        }
    }
    if (republish && mMenuRegistered.load()) publishMenuSchema();
    if (refreshVote) refreshMapData(true, forceRefresh);
    if (refreshAvoid) refreshMapData(false, forceRefresh);
    return true;
}

bool HiveUtilsModule::showInLegacyMenu(std::string_view key) const {
    return key != "uiMapVoteGame"
        && key != "uiMapVoteVariant"
        && key != "uiMapAvoidGame"
        && key != "uiMapAvoidVariant";
}

void HiveUtilsModule::onMenuRegistered() {
    mMenuRegistered.store(true);
    publishMenuSchema();
    refreshMapData(true, false);
    refreshMapData(false, false);
}

std::string HiveUtilsModule::mapVoteRulesSnapshot() const {
    std::lock_guard lock(mMapConfigMutex);
    return autoMapVoteRules;
}

std::string HiveUtilsModule::mapAvoidRulesSnapshot() const {
    std::lock_guard lock(mMapConfigMutex);
    return mapAvoiderRules;
}

void HiveUtilsModule::refreshMapData(bool vote, bool force) {
    std::string game;
    {
        std::lock_guard lock(mMapConfigMutex);
        game = vote ? uiMapVoteGame : uiMapAvoidGame;
    }
    HiveUtilsModule* expected = this;
    bedrocktools::hive::refreshMapsAsync(game, force, [expected]() {
        HiveUtilsModule* current = HiveUtilsModule::instance;
        if (current == expected && current && current->mMenuRegistered.load()) current->publishMenuSchema();
    });
    if (mMenuRegistered.load()) publishMenuSchema();
}

void HiveUtilsModule::publishMenuSchema() {
    std::lock_guard schemaLock(mMenuSchemaMutex);
    if (!mMenuRegistered.load()) return;

    std::string voteGame;
    std::string voteVariant;
    std::string avoidGame;
    std::string avoidVariant;
    std::string voteRules;
    std::string avoidRules;
    nlohmann::json votePreferences;
    nlohmann::json avoidPreferences;
    {
        std::lock_guard lock(mMapConfigMutex);
        voteGame = uiMapVoteGame;
        voteVariant = normalizeVariant(uiMapVoteVariant);
        avoidGame = uiMapAvoidGame;
        avoidVariant = normalizeVariant(uiMapAvoidVariant);
        voteRules = autoMapVoteRules;
        avoidRules = mapAvoiderRules;
        votePreferences = mapVotePreferences;
        avoidPreferences = mapAvoidPreferences;
    }

    const std::string voteKey = mapRuleKey(voteGame, voteVariant);
    const std::string avoidKey = mapRuleKey(avoidGame, avoidVariant);
    const auto selectedVoteMaps = jsonStringList(votePreferences, voteKey);
    const auto selectedAvoidMaps = jsonStringList(avoidPreferences, avoidKey);
    const auto voteSnapshot = bedrocktools::hive::mapSnapshot(voteGame);
    const auto avoidSnapshot = bedrocktools::hive::mapSnapshot(avoidGame);
    const auto voteMaps = bedrocktools::hive::mapsForVariant(voteGame, voteVariant);
    const auto avoidMaps = bedrocktools::hive::mapsForVariant(avoidGame, avoidVariant);

    using namespace pl::modmenu;
    ConfigSchemaBuilder schema;
    schema.defaultCategory("requeue")
        .category("requeue", "Requeue", "Automatic requeue behavior and shortcut")
        .category("roles", "Roles", "Skip games when you receive selected Hive roles")
        .category("maps", "Maps", "Automatic map voting and map avoidance")
        .category("chat", "Chat", "Choose which Hive messages to hide")
        .category("social", "Social", "Automatic friend and party actions")
        .category("custom", "Custom Server", "Custom server code convenience options")
        .category("advanced", "Advanced", "Compatibility, raw rules and map cache tools");

    auto add = [&](ConfigNodeV2 node) {
        schema.node(std::move(node));
    };
    auto visible = [](std::string key) {
        return std::vector<ConfigConditionV2>{{std::move(key), ConfigConditionOpV2::Truthy, {}}};
    };
    auto section = [&](std::string id, std::string category, std::string title, std::string description = {}) {
        ConfigNodeV2 node;
        node.id = std::move(id);
        node.category = std::move(category);
        node.title = std::move(title);
        node.description = std::move(description);
        node.type = ConfigControlTypeV2::Section;
        node.collapsible = true;
        add(std::move(node));
    };
    auto toggle = [&](std::string key, std::string category, std::string sectionId, std::string title, std::string description = {}) {
        ConfigNodeV2 node;
        node.id = key;
        node.key = std::move(key);
        node.category = std::move(category);
        node.section = std::move(sectionId);
        node.title = std::move(title);
        node.description = std::move(description);
        node.type = ConfigControlTypeV2::Toggle;
        add(std::move(node));
    };

    section("requeue_behavior", "requeue", "Automatic Requeue");
    toggle("autoRequeue", "requeue", "requeue_behavior", "Auto Requeue", "Automatically find another match when a configured requeue condition is met.");
    {
        ConfigNodeV2 node;
        node.id = "requeue_conditions";
        node.category = "requeue";
        node.section = "requeue_behavior";
        node.title = "Requeue Conditions";
        node.description = "Choose the match events that should trigger an automatic requeue.";
        node.type = ConfigControlTypeV2::ToggleGroup;
        node.choiceStyle = ConfigChoiceStyleV2::Chips;
        node.options = {
            {"solo", "Solo death / finish", {}, "autoRequeueSoloMode"},
            {"team", "Team eliminated", {}, "autoRequeueTeamElimination"},
            {"gameover", "Game over", {}, "autoRequeueGameOver"},
        };
        node.visibleWhen = visible("autoRequeue");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "useHub";
        node.key = "useHub";
        node.category = "requeue";
        node.section = "requeue_behavior";
        node.title = "Return To Hub";
        node.description = "Use /hub instead of /q when a requeue condition is triggered.";
        node.type = ConfigControlTypeV2::Toggle;
        node.visibleWhen = visible("autoRequeue");
        add(std::move(node));
    }
    section("requeue_shortcut", "requeue", "Shortcut");
    {
        ConfigNodeV2 node;
        node.id = "requeueKeybind";
        node.key = "requeueKeybind";
        node.category = "requeue";
        node.section = "requeue_shortcut";
        node.title = "Requeue Keybind";
        node.description = "Immediately requeue the current Hive game.";
        node.type = ConfigControlTypeV2::Keybind;
        add(std::move(node));
    }

    section("murder_roles", "roles", "Murder Mystery");
    {
        ConfigNodeV2 node;
        node.id = "murder_roles_group";
        node.category = "roles";
        node.section = "murder_roles";
        node.title = "Requeue Roles";
        node.description = "Requeue when Hive assigns any selected role.";
        node.type = ConfigControlTypeV2::ToggleGroup;
        node.choiceStyle = ConfigChoiceStyleV2::Chips;
        node.options = {
            {"murderer", "Murderer", {}, "roleMurderer"},
            {"sheriff", "Sheriff", {}, "roleSheriff"},
            {"innocent", "Innocent", {}, "roleInnocent"},
        };
        add(std::move(node));
    }
    section("hide_roles", "roles", "Hide and Seek");
    {
        ConfigNodeV2 node;
        node.id = "hide_roles_group";
        node.category = "roles";
        node.section = "hide_roles";
        node.title = "Requeue Roles";
        node.type = ConfigControlTypeV2::ToggleGroup;
        node.choiceStyle = ConfigChoiceStyleV2::Chips;
        node.options = {
            {"hider", "Hider", {}, "roleHider"},
            {"seeker", "Seeker", {}, "roleSeeker"},
        };
        add(std::move(node));
    }
    section("deathrun_roles", "roles", "DeathRun");
    {
        ConfigNodeV2 node;
        node.id = "deathrun_roles_group";
        node.category = "roles";
        node.section = "deathrun_roles";
        node.title = "Requeue Roles";
        node.type = ConfigControlTypeV2::ToggleGroup;
        node.choiceStyle = ConfigChoiceStyleV2::Chips;
        node.options = {
            {"death", "Death", {}, "roleDeath"},
            {"runner", "Runner", {}, "roleRunner"},
        };
        add(std::move(node));
    }
    toggle("deathCountEnabled", "roles", "deathrun_roles", "Death Limit", "Requeue after dying the configured number of times in DeathRun.");
    {
        ConfigNodeV2 node;
        node.id = "deathCountLimit";
        node.key = "deathCountLimit";
        node.category = "roles";
        node.section = "deathrun_roles";
        node.title = "Deaths Before Requeue";
        node.type = ConfigControlTypeV2::SliderInt;
        node.minValue = "1";
        node.maxValue = "100";
        node.step = "1";
        node.unit = " deaths";
        node.visibleWhen = visible("deathCountEnabled");
        add(std::move(node));
    }

    section("map_vote", "maps", "Auto Map Vote", "Select maps visually. Preferred maps are tried from top to bottom.");
    toggle("autoMapVote", "maps", "map_vote", "Auto Map Vote", "Automatically vote for the highest-priority preferred map available in Hive's vote form.");
    {
        ConfigNodeV2 node;
        node.id = "uiMapVoteGame";
        node.key = "uiMapVoteGame";
        node.category = "maps";
        node.section = "map_vote";
        node.title = "Game";
        node.description = "Choose which game's map preferences to edit.";
        node.type = ConfigControlTypeV2::Choice;
        node.choiceStyle = ConfigChoiceStyleV2::Dropdown;
        node.options = gameOptions();
        node.currentValue = voteGame;
        node.visibleWhen = visible("autoMapVote");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "uiMapVoteVariant";
        node.key = "uiMapVoteVariant";
        node.category = "maps";
        node.section = "map_vote";
        node.title = "Variant";
        node.type = ConfigControlTypeV2::Choice;
        node.choiceStyle = ConfigChoiceStyleV2::Segmented;
        node.options = variantOptions(voteGame, voteVariant);
        node.currentValue = voteVariant;
        node.visibleWhen = visible("autoMapVote");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "map_vote_status";
        node.category = "maps";
        node.section = "map_vote";
        node.title = "Hive Map Data";
        node.description = mapStatusText(voteSnapshot);
        node.type = ConfigControlTypeV2::Info;
        node.visibleWhen = visible("autoMapVote");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "refreshMapVoteMaps";
        node.key = "refreshMapVoteMaps";
        node.category = "maps";
        node.section = "map_vote";
        node.title = voteSnapshot.loading ? "Refreshing Maps" : "Refresh Maps";
        node.description = "Fetch the latest enabled map list from the Hive API.";
        node.type = ConfigControlTypeV2::Button;
        node.actionValue = "true";
        node.disabled = voteSnapshot.loading;
        node.visibleWhen = visible("autoMapVote");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "mapVoteSelection";
        node.key = "mapVoteSelection";
        node.category = "maps";
        node.section = "map_vote";
        node.title = "Preferred Maps";
        node.description = "Long-press and drag selected maps to change voting priority. Tap an available map to add it.";
        node.type = ConfigControlTypeV2::OrderedList;
        node.searchable = true;
        node.allowReorder = true;
        node.options = mapOptions(voteMaps, selectedVoteMaps);
        node.currentValue = jsonArrayString(selectedVoteMaps);
        node.visibleWhen = visible("autoMapVote");
        add(std::move(node));
    }
    toggle("announceVote", "maps", "map_vote", "Announce Vote", "Send a chat message after voting for a map.");
    {
        ConfigNodeV2 node;
        node.id = "announceVoteMessage";
        node.key = "announceVoteMessage";
        node.category = "maps";
        node.section = "map_vote";
        node.title = "Announcement Message";
        node.description = "Use {map} where the selected map name should appear.";
        node.type = ConfigControlTypeV2::Text;
        node.placeholder = "@here vote for {map}!";
        node.maxLength = 128;
        node.visibleWhen = visible("announceVote");
        add(std::move(node));
    }

    section("map_avoid", "maps", "Map Avoider", "Choose maps that should trigger a new queue when they win the vote.");
    toggle("mapAvoider", "maps", "map_avoid", "Map Avoider", "Automatically find a different match when a configured map wins.");
    {
        ConfigNodeV2 node;
        node.id = "uiMapAvoidGame";
        node.key = "uiMapAvoidGame";
        node.category = "maps";
        node.section = "map_avoid";
        node.title = "Game";
        node.type = ConfigControlTypeV2::Choice;
        node.choiceStyle = ConfigChoiceStyleV2::Dropdown;
        node.options = gameOptions();
        node.currentValue = avoidGame;
        node.visibleWhen = visible("mapAvoider");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "uiMapAvoidVariant";
        node.key = "uiMapAvoidVariant";
        node.category = "maps";
        node.section = "map_avoid";
        node.title = "Variant";
        node.type = ConfigControlTypeV2::Choice;
        node.choiceStyle = ConfigChoiceStyleV2::Segmented;
        node.options = variantOptions(avoidGame, avoidVariant);
        node.currentValue = avoidVariant;
        node.visibleWhen = visible("mapAvoider");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "map_avoid_status";
        node.category = "maps";
        node.section = "map_avoid";
        node.title = "Hive Map Data";
        node.description = mapStatusText(avoidSnapshot);
        node.type = ConfigControlTypeV2::Info;
        node.visibleWhen = visible("mapAvoider");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "refreshMapAvoidMaps";
        node.key = "refreshMapAvoidMaps";
        node.category = "maps";
        node.section = "map_avoid";
        node.title = avoidSnapshot.loading ? "Refreshing Maps" : "Refresh Maps";
        node.description = "Fetch the latest enabled map list from the Hive API.";
        node.type = ConfigControlTypeV2::Button;
        node.actionValue = "true";
        node.disabled = avoidSnapshot.loading;
        node.visibleWhen = visible("mapAvoider");
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "mapAvoidSelection";
        node.key = "mapAvoidSelection";
        node.category = "maps";
        node.section = "map_avoid";
        node.title = "Avoided Maps";
        node.description = "Select every map you want Hive Utils to avoid for this game and variant.";
        node.type = ConfigControlTypeV2::MultiChoice;
        node.choiceStyle = ConfigChoiceStyleV2::Checklist;
        node.searchable = true;
        node.options = mapOptions(avoidMaps, selectedAvoidMaps);
        node.currentValue = jsonArrayString(selectedAvoidMaps);
        node.visibleWhen = visible("mapAvoider");
        add(std::move(node));
    }

    section("chat_filters", "chat", "Messages To Hide");
    {
        ConfigNodeV2 node;
        node.id = "chat_filter_group";
        node.category = "chat";
        node.section = "chat_filters";
        node.title = "Chat Filters";
        node.description = "Select the Hive messages you do not want to see.";
        node.type = ConfigControlTypeV2::ToggleGroup;
        node.choiceStyle = ConfigChoiceStyleV2::Checklist;
        node.options = {
            {"promo", "Promo / info", {}, "hidePromoMessages"},
            {"unlocks", "Unused Locker unlocks", {}, "hideUnusedUnlocks"},
            {"joined", "Player joined", {}, "hidePlayerJoined"},
            {"unranked", "Unranked player chat", {}, "hideUnrankedPlayerMessages"},
            {"hiveplus", "Hive+ chat", {}, "hideHivePlusMessages"},
            {"noteaming", "No teaming warning", {}, "hideNoTeaming"},
        };
        add(std::move(node));
    }

    section("social_accept", "social", "Auto Accept");
    toggle("autoAcceptFriend", "social", "social_accept", "Friend Requests", "Automatically accept incoming Hive friend requests.");
    toggle("autoAcceptParty", "social", "social_accept", "Party Invites", "Automatically accept incoming Hive party invites.");

    section("custom_code", "custom", "Join Code");
    toggle("copyCustomServerCode", "custom", "custom_code", "Copy Join Code", "Automatically copy a custom server join code when Hive displays it.");
    {
        ConfigNodeV2 node;
        node.id = "copyCustomServerCodeIncludeCommand";
        node.key = "copyCustomServerCodeIncludeCommand";
        node.category = "custom";
        node.section = "custom_code";
        node.title = "Include /cs Command";
        node.description = "Copy /cs CODE instead of only the join code.";
        node.type = ConfigControlTypeV2::Toggle;
        node.visibleWhen = visible("copyCustomServerCode");
        add(std::move(node));
    }

    section("advanced_rules", "advanced", "Raw Map Rules", "These fields are only for compatibility or manual recovery. Normal map setup should use the Maps category.");
    {
        ConfigNodeV2 node;
        node.id = "autoMapVoteRules";
        node.key = "autoMapVoteRules";
        node.category = "advanced";
        node.section = "advanced_rules";
        node.title = "Auto Vote Rules";
        node.description = "Legacy GAME=Map A|Map B format.";
        node.type = ConfigControlTypeV2::MultilineText;
        node.currentValue = voteRules;
        node.advanced = true;
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "mapAvoiderRules";
        node.key = "mapAvoiderRules";
        node.category = "advanced";
        node.section = "advanced_rules";
        node.title = "Map Avoider Rules";
        node.description = "Legacy GAME=Map A|Map B format.";
        node.type = ConfigControlTypeV2::MultilineText;
        node.currentValue = avoidRules;
        node.advanced = true;
        add(std::move(node));
    }
    section("advanced_cache", "advanced", "Map Cache");
    {
        ConfigNodeV2 node;
        node.id = "cache_status";
        node.category = "advanced";
        node.section = "advanced_cache";
        node.title = "Current Cache";
        node.description = "Vote: " + mapStatusText(voteSnapshot) + " Avoider: " + mapStatusText(avoidSnapshot);
        node.type = ConfigControlTypeV2::Info;
        add(std::move(node));
    }
    {
        ConfigNodeV2 node;
        node.id = "clearHiveMapCache";
        node.key = "clearHiveMapCache";
        node.category = "advanced";
        node.section = "advanced_cache";
        node.title = "Clear And Refresh Map Cache";
        node.description = "Discard cached Hive map metadata and fetch the selected games again.";
        node.type = ConfigControlTypeV2::Button;
        node.actionValue = "true";
        add(std::move(node));
    }

    pl::modmenu::setConfigSchemaJson(moduleId, schema.toJson());
}

void HiveUtilsModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    auto readBool = [&](const char* key, bool& value) { if (j.contains(key) && j[key].is_boolean()) value = j[key].get<bool>(); };
    auto readInt = [&](const char* key, int& value) { if (j.contains(key) && j[key].is_number_integer()) value = j[key].get<int>(); };
    auto readString = [&](const char* key, std::string& value) { if (j.contains(key) && j[key].is_string()) value = j[key].get<std::string>(); };
    readBool("useHub", useHub);
    readBool("autoRequeue", autoRequeue);
    readBool("autoRequeueSoloMode", autoRequeueSoloMode);
    readBool("autoRequeueTeamElimination", autoRequeueTeamElimination);
    readBool("autoRequeueGameOver", autoRequeueGameOver);
    readBool("roleMurderer", roleMurderer);
    readBool("roleSheriff", roleSheriff);
    readBool("roleInnocent", roleInnocent);
    readBool("roleHider", roleHider);
    readBool("roleSeeker", roleSeeker);
    readBool("roleDeath", roleDeath);
    readBool("roleRunner", roleRunner);
    readBool("deathCountEnabled", deathCountEnabled);
    readInt("deathCountLimit", deathCountLimit);
    readBool("copyCustomServerCode", copyCustomServerCode);
    readBool("copyCustomServerCodeIncludeCommand", copyCustomServerCodeIncludeCommand);
    readBool("hidePromoMessages", hidePromoMessages);
    readBool("hideUnusedUnlocks", hideUnusedUnlocks);
    readBool("hidePlayerJoined", hidePlayerJoined);
    readBool("hideUnrankedPlayerMessages", hideUnrankedPlayerMessages);
    readBool("hideHivePlusMessages", hideHivePlusMessages);
    readBool("hideNoTeaming", hideNoTeaming);
    readBool("autoAcceptFriend", autoAcceptFriend);
    readBool("autoAcceptParty", autoAcceptParty);
    readBool("autoMapVote", autoMapVote);
    readBool("announceVote", announceVote);
    readString("announceVoteMessage", announceVoteMessage);
    readBool("mapAvoider", mapAvoider);
    readInt("requeueKeybind", requeueKeybind);
    std::string loadedVoteRules = autoMapVoteRules;
    std::string loadedAvoidRules = mapAvoiderRules;
    std::string loadedVoteGame = uiMapVoteGame;
    std::string loadedVoteVariant = uiMapVoteVariant;
    std::string loadedAvoidGame = uiMapAvoidGame;
    std::string loadedAvoidVariant = uiMapAvoidVariant;
    readString("autoMapVoteRules", loadedVoteRules);
    readString("mapAvoiderRules", loadedAvoidRules);
    readString("uiMapVoteGame", loadedVoteGame);
    readString("uiMapVoteVariant", loadedVoteVariant);
    readString("uiMapAvoidGame", loadedAvoidGame);
    readString("uiMapAvoidVariant", loadedAvoidVariant);
    if (j.contains("soloMode") && j["soloMode"].is_boolean()) autoRequeueSoloMode = j["soloMode"].get<bool>();
    if (j.contains("teamElimination") && j["teamElimination"].is_boolean()) autoRequeueTeamElimination = j["teamElimination"].get<bool>();
    if (j.contains("gameOver") && j["gameOver"].is_boolean()) autoRequeueGameOver = j["gameOver"].get<bool>();
    deathCountLimit = std::max(1, deathCountLimit);
    loadedVoteGame = bedrocktools::hive::apiGameId(loadedVoteGame);
    loadedAvoidGame = bedrocktools::hive::apiGameId(loadedAvoidGame);
    if (loadedVoteGame.empty()) loadedVoteGame = "bed";
    if (loadedAvoidGame.empty()) loadedAvoidGame = "bed";
    loadedVoteVariant = normalizeVariant(std::move(loadedVoteVariant));
    loadedAvoidVariant = normalizeVariant(std::move(loadedAvoidVariant));
    {
        std::lock_guard lock(mMapConfigMutex);
        uiMapVoteGame = std::move(loadedVoteGame);
        uiMapVoteVariant = std::move(loadedVoteVariant);
        uiMapAvoidGame = std::move(loadedAvoidGame);
        uiMapAvoidVariant = std::move(loadedAvoidVariant);
        autoMapVoteRules = std::move(loadedVoteRules);
        mapAvoiderRules = std::move(loadedAvoidRules);
        if (j.contains("mapVotePreferences") && j["mapVotePreferences"].is_object()) mapVotePreferences = j["mapVotePreferences"];
        else mapVotePreferences = parseLegacyMapRules(autoMapVoteRules);
        if (j.contains("mapAvoidPreferences") && j["mapAvoidPreferences"].is_object()) mapAvoidPreferences = j["mapAvoidPreferences"];
        else mapAvoidPreferences = parseLegacyMapRules(mapAvoiderRules);
        autoMapVoteRules = serializeMapRules(mapVotePreferences);
        mapAvoiderRules = serializeMapRules(mapAvoidPreferences);
    }
    if (mMenuRegistered.load()) publishMenuSchema();
}

void HiveUtilsModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["useHub"] = useHub;
    j["autoRequeue"] = autoRequeue;
    j["autoRequeueSoloMode"] = autoRequeueSoloMode;
    j["autoRequeueTeamElimination"] = autoRequeueTeamElimination;
    j["autoRequeueGameOver"] = autoRequeueGameOver;
    j["roleMurderer"] = roleMurderer;
    j["roleSheriff"] = roleSheriff;
    j["roleInnocent"] = roleInnocent;
    j["roleHider"] = roleHider;
    j["roleSeeker"] = roleSeeker;
    j["roleDeath"] = roleDeath;
    j["roleRunner"] = roleRunner;
    j["deathCountEnabled"] = deathCountEnabled;
    j["deathCountLimit"] = deathCountLimit;
    j["copyCustomServerCode"] = copyCustomServerCode;
    j["copyCustomServerCodeIncludeCommand"] = copyCustomServerCodeIncludeCommand;
    j["hidePromoMessages"] = hidePromoMessages;
    j["hideUnusedUnlocks"] = hideUnusedUnlocks;
    j["hidePlayerJoined"] = hidePlayerJoined;
    j["hideUnrankedPlayerMessages"] = hideUnrankedPlayerMessages;
    j["hideHivePlusMessages"] = hideHivePlusMessages;
    j["hideNoTeaming"] = hideNoTeaming;
    j["autoAcceptFriend"] = autoAcceptFriend;
    j["autoAcceptParty"] = autoAcceptParty;
    j["autoMapVote"] = autoMapVote;
    j["announceVote"] = announceVote;
    j["announceVoteMessage"] = announceVoteMessage;
    j["mapAvoider"] = mapAvoider;
    j["requeueKeybind"] = requeueKeybind;
    std::lock_guard lock(mMapConfigMutex);
    autoMapVoteRules = serializeMapRules(mapVotePreferences);
    mapAvoiderRules = serializeMapRules(mapAvoidPreferences);
    j["autoMapVoteRules"] = autoMapVoteRules;
    j["mapAvoiderRules"] = mapAvoiderRules;
    j["uiMapVoteGame"] = uiMapVoteGame;
    j["uiMapVoteVariant"] = uiMapVoteVariant;
    j["uiMapAvoidGame"] = uiMapAvoidGame;
    j["uiMapAvoidVariant"] = uiMapAvoidVariant;
    j["mapVotePreferences"] = mapVotePreferences;
    j["mapAvoidPreferences"] = mapAvoidPreferences;
}

