#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

class Module {
public:
    const char* name;
    const char* description;
    std::string moduleId;
    bool masterEnabled = false;
    bool keybindActive = true;
    bool enabled = false;
    bool showInMenu = true;
    bool hideInHudEditor = false;
    int keybind = 0;

    Module(const char* n, const char* d) : name(n), description(d), moduleId(std::string("bedrocktools.") + n) {}
    virtual ~Module() = default;

    virtual void onInit()     {}
    virtual void onEnable()   {}
    virtual void onDisable()  {}
    virtual void onFrame()    {}
    virtual bool onMouseEvent(int button, bool isDown) { return false; }
    virtual bool onMenuConfigChanged(std::string_view key, std::string_view value) { return false; }
    virtual bool showInLegacyMenu(std::string_view key) const { return true; }
    virtual void onMenuRegistered() {}
    
    virtual void onKeybindEvent(const std::string& key, bool isDown) {
        if (key == "keybind" && isDown) {
            setKeybindActive(!keybindActive);
        }
    }

    void setMasterEnabled(bool state) {
        if (masterEnabled == state) return;
        masterEnabled = state;
        updateEnabledState();
    }

    void setKeybindActive(bool state) {
        if (keybindActive == state) return;
        keybindActive = state;
        updateEnabledState();
    }

    void updateEnabledState() {
        bool newState = masterEnabled && keybindActive;
        if (newState != enabled) {
            enabled = newState;
            if (enabled) onEnable();
            else onDisable();
        }
    }

    virtual void loadConfig(const nlohmann::json& j) {
        if (j.contains("keybind")) keybind = j["keybind"].get<int>();
        if (j.contains("keybindActive")) {
            keybindActive = j["keybindActive"].get<bool>();
            updateEnabledState();
        }
        if (j.contains("masterEnabled")) {
            masterEnabled = j["masterEnabled"].get<bool>();
            updateEnabledState();
        }
    }

    virtual void saveConfig(nlohmann::json& j) {
        j["keybind"] = keybind;
        j["keybindActive"] = keybindActive;
        j["masterEnabled"] = masterEnabled;
    }
};
