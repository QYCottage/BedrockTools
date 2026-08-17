#include "ModuleMenu.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"
#include <pl/ModMenu.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

static void onModuleToggle(std::string_view module_id, bool enabled) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->setMasterEnabled(enabled);
    bedrocktools::config::ConfigManager::get().save();
}

static void onModuleKeybind(std::string_view module_id, std::string_view key, bool isDown) {
    if (ModuleRegistry::get().keybindBlocked()) return;
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;
    mod->onKeybindEvent(std::string(key), isDown);
}

static void onModuleConfigChanged(std::string_view module_id, std::string_view key, std::string_view value) {
    auto* mod = ModuleRegistry::get().find(module_id);
    if (!mod) return;

    nlohmann::json j;
    mod->saveConfig(j);

    std::string safeValue(value);
    std::string safeKey(key);
    if (!safeValue.empty()) {
        try {
            if (j.contains(safeKey)) {
                if (j[safeKey].is_boolean()) {
                    if (safeValue == "true") j[safeKey] = true;
                    else if (safeValue == "false") j[safeKey] = false;
                } else if (j[safeKey].is_number_integer()) {
                    char* end;
                    int val = std::strtol(safeValue.c_str(), &end, 10);
                    if (end != safeValue.c_str()) j[safeKey] = val;
                } else if (j[safeKey].is_number_float()) {
                    char* end;
                    float val = std::strtof(safeValue.c_str(), &end);
                    if (end != safeValue.c_str()) j[safeKey] = val;
                } else {
                    j[safeKey] = safeValue;
                }
            } else {
                j[safeKey] = safeValue;
            }
        } catch (...) {
            j[safeKey] = safeValue;
        }
    }
    mod->loadConfig(j);
    bedrocktools::config::ConfigManager::get().save();
}

void registerModulesWithLauncher() {
    auto& modules = ModuleRegistry::get().modules();

    for (auto* mod : modules) {
        if (!mod->showInMenu) continue;

        pl::modmenu::ModuleBuilder builder(mod->moduleId, mod->name);
        builder.description(mod->description)
                .defaultEnabled(mod->masterEnabled)
                .hideInHudEditor(mod->hideInHudEditor)
                .onToggle(onModuleToggle)
                .onConfigChanged(onModuleConfigChanged)
                .onKeybind(onModuleKeybind);

        nlohmann::json j;
        mod->saveConfig(j);

        static const char* baseKeys[] = {
                "enabled", "masterEnabled", "keybindActive", "shortcutEnabled", "shortcutSize", "shortcutOpacity",
                "lockPosition", "shortcutPosX", "shortcutPosY",
                "isHudModule", "hudPosX", "hudPosY"
        };

        struct TmpConfigEntry {
            std::string key;
            std::string displayName;
            pl::modmenu::ConfigType type;
            std::string default_value;
            std::string min_value;
            std::string max_value;
            std::string depends_on;
        };
        std::vector<TmpConfigEntry> configs;

        for (auto& [k, v] : j.items()) {
            bool isBase = false;
            for (const char* bk : baseKeys) {
                if (k == bk) { isBase = true; break; }
            }
            if (isBase) continue;

            std::string displayName;
            std::string sourceKey = k;
            if (sourceKey.size() > 2 && sourceKey[0] == 'm' && sourceKey[1] == '_') {
                sourceKey = sourceKey.substr(2);
            }
            for (size_t i = 0; i < sourceKey.size(); ++i) {
                if (i == 0) {
                    displayName += toupper(sourceKey[i]);
                } else if (isupper(sourceKey[i])) {
                    displayName += ' ';
                    displayName += sourceKey[i];
                } else {
                    displayName += sourceKey[i];
                }
            }

            TmpConfigEntry entry;
            entry.key = k;
            entry.displayName = displayName;

            if (v.is_boolean()) {
                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);
                if (kLower.find("button") != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Button;
                } else {
                    entry.type = pl::modmenu::ConfigType::Toggle;
                }
                entry.default_value = v.get<bool>() ? "true" : "false";
            } else if (v.is_number_integer()) {
                entry.type = pl::modmenu::ConfigType::SliderInt;
                entry.default_value = std::to_string(v.get<int>());

                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

                if (kLower.find("keybind") != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Keybind;
                } else {
                    int minVal = 0;
                    int maxVal = 200;
                    if (kLower.find("cps") != std::string::npos) {
                        minVal = 1;
                        maxVal = 30;
                    } else if (kLower.find("time") != std::string::npos) {
                        maxVal = 24000;
                    }

                    entry.min_value = std::to_string(minVal);
                    entry.max_value = std::to_string(maxVal);
                }
            } else if (v.is_number_float()) {
                entry.type = pl::modmenu::ConfigType::SliderFloat;
                entry.default_value = std::to_string(v.get<float>());

                std::string kLower = k;
                std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);

                float minVal = 0.0f;
                float maxVal = 100.0f;

                if (kLower.find("opacity") != std::string::npos ||
                    kLower.find("color") != std::string::npos ||
                    kLower.find("alpha") != std::string::npos) {
                    maxVal = 1.0f;
                } else if (kLower.find("scale") != std::string::npos) {
                    minVal = 0.1f;
                    maxVal = 5.0f;
                } else if (kLower == "borderwidth") {
                    maxVal = 4.0f;
                } else if (kLower.find("width") != std::string::npos) {
                    maxVal = 1000.0f;
                } else if (kLower.find("position") != std::string::npos ||
                           kLower.find("posx") != std::string::npos ||
                           kLower.find("posy") != std::string::npos) {
                    maxVal = 2000.0f;
                } else if (kLower.find("range") != std::string::npos) {
                    maxVal = 180.0f;
                } else if (kLower.find("fov") != std::string::npos) {
                    minVal = 1.0f;
                    maxVal = 179.0f;
                } else if (kLower.find("intensity") != std::string::npos) {
                    maxVal = 10.0f;
                } else if (kLower.find("speed") != std::string::npos || kLower.find("strength") != std::string::npos) {
                    minVal = 0.05f;
                    maxVal = 1.0f;
                } else if (kLower == "linethickness") {
                    // 1.0 == classic hairline box, higher values widen the
                    // hitbox lines (Hitbox module).
                    minVal = 1.0f;
                    maxVal = 10.0f;
                } else if (kLower.find("thick") != std::string::npos) {
                    maxVal = 20.0f;
                }

                entry.min_value = std::to_string(minVal);
                entry.max_value = std::to_string(maxVal);
            } else if (v.is_string()) {
                std::string str = v.get<std::string>();
                if (str.find(',') != std::string::npos) {
                    entry.type = pl::modmenu::ConfigType::Radio;
                    size_t firstComma = str.find(',');
                    entry.default_value = str.substr(0, firstComma);
                    entry.min_value = str.substr(firstComma + 1);
                } else if (!str.empty() && str[0] == '#') {
                    entry.type = pl::modmenu::ConfigType::Color;
                    entry.default_value = str;
                } else {
                    entry.type = pl::modmenu::ConfigType::Text;
                    entry.default_value = str;
                }
            } else {
                continue;
            }
            configs.push_back(entry);
        }

                for (auto& entry : configs) {
            std::string k = entry.key;
            if (entry.type != pl::modmenu::ConfigType::Toggle) {
                std::string bestParent = "";
                for (const auto& parentCandidate : configs) {
                    if (parentCandidate.type == pl::modmenu::ConfigType::Toggle) {
                        std::string pKey = parentCandidate.key;
                        if (k.length() > pKey.length() && k.compare(0, pKey.length(), pKey) == 0) {
                            if (pKey.length() > bestParent.length()) {
                                bestParent = pKey;
                            }
                        }
                    }
                }
                if (!bestParent.empty()) entry.depends_on = bestParent;
            }
            builder.config(entry.key, entry.displayName, entry.type, entry.default_value, entry.min_value, entry.max_value, entry.depends_on);
        }

        (void)builder.registerModule();
    }
}