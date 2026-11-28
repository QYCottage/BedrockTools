// Host-side fake of the Preloader <pl/ModMenu.hpp> header for tests that
// include ModuleRegistry.hpp without the game. The struct layout mirrors
// the real preloader header (the one that ships in libpreloader.so on
// Android), so production code that targets the real header compiles
// unchanged against this stub. The DrawCommand layout and the image/texture
// registration API mirror the real preloader (LiteLDev/preloader-android
// include/pl/ModMenu.hpp) so module UI code can be built and
// unit-tested on the host. Nothing here is meant to be called — the
// only purpose is to let modules parse when compiled on a host without
// the LeviLauncher preloader.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pl::modmenu {

enum class ConfigType {
    Toggle,
    SliderInt,
    SliderFloat,
    Radio,
    Color,
    Keybind,
    Text,
    Button,
};

enum class DrawCommandType {
    Text,
    Rect,
    Line,
    RectFilled,
    CircleFilled,
    TriangleFilled,
    Image,
};

struct DrawCommand {
    DrawCommandType type{};
    float x{};
    float y{};
    float w{};
    float h{};
    float x3{}; // corner radius for Rect/RectFilled
    float y3{}; // extra Y coordinate (e.g. pointer/wing tip)
    std::uint32_t color{};
    float size{}; // line thickness or text size
    std::string text;
    std::string fontId;
    std::string imageId;
};

// Mirrors the real header (span-based) so the ModuleRegistry.hpp wrapper —
// which is an exact-match overload for std::vector — stays unambiguous.
inline void submitDrawCommands(std::string_view, std::span<const DrawCommand>) {}

struct HudEditorElement {
    std::string elementId;
    std::string displayName;
    std::string positionKeyX;
    std::string positionKeyY;
    float x{};
    float y{};
    float width{};
    float height{};
    float gridSize{16.0f};
    float snapThreshold{12.0f};
    float gridGap{4.0f};
    std::uint32_t snapFlags{0};
};

inline void submitHudEditorElements(std::string_view, std::span<const HudEditorElement>) {}

inline bool registerImage(std::string_view, std::span<const unsigned char>,
                          int, int) {
    return true;
}

inline bool registerFont(std::string_view, std::span<const unsigned char>) {
    return true;
}

// Overlay button API, mirroring the builder calls the real header accepts
// (see the Zoom / Command Hotkey modules for the production usage).
enum class ButtonBehavior {
    Click,
    Toggle,
};

enum class ButtonEvent {
    Click,
    StateChanged,
};

enum class ButtonStylePreset {
    Accent,
};

class ButtonBuilder {
public:
    ButtonBuilder(std::string_view id, std::string_view label)
        : mId(id), mLabel(label) {}

    ButtonBuilder& moduleId(std::string_view id) { mModuleId = id; return *this; }
    ButtonBuilder& label(std::string_view label) { mLabel = label; return *this; }
    ButtonBuilder& behavior(ButtonBehavior behavior) { mBehavior = behavior; return *this; }
    ButtonBuilder& stylePreset(ButtonStylePreset preset) { mPreset = preset; return *this; }
    ButtonBuilder& styleColors(std::uint32_t, std::uint32_t, std::uint32_t) { return *this; }
    ButtonBuilder& svgIcon(const char*, bool = true) { return *this; }
    ButtonBuilder& activeSvgIcon(const char*) { return *this; }
    ButtonBuilder& textColor(std::uint32_t) { return *this; }
    ButtonBuilder& activeTextColor(std::uint32_t) { return *this; }
    ButtonBuilder& sizeScale(float, float) { return *this; }
    ButtonBuilder& defaultVisible(bool) { return *this; }

    template <class Handler>
    ButtonBuilder& onEvent(Handler&&) { return *this; }

    bool registerButton() { return true; }

private:
    std::string_view mId;
    std::string_view mLabel;
    std::string_view mModuleId;
    ButtonBehavior mBehavior = ButtonBehavior::Click;
    ButtonStylePreset mPreset = ButtonStylePreset::Accent;
};

inline void unregisterButton(std::string_view) {}

} // namespace pl::modmenu
