#pragma once

#include "../Module.hpp"
#include <cstdint>
#include <string>
#include <vector>

// Custom Capes
//
// Lets the player wear any PNG as a classic cape. The module owns a "capes"
// directory next to config.json (`<configDir>/capes`, created on first
// launch together with a sample cape); every .png file in it shows up as an
// option of the module's radio picker in the launcher mod menu.
//
// The selected file is decoded with stb_image, resampled into the visible
// 10x16 classic-cape box at (1,1) on the 64x32 canvas Minecraft uses, and
// written into the local player's SerializedSkinImpl::mCapeImage each tick. Modern game versions
// only render the classic cape when SerializedSkinImpl::mCapeId is
// non-empty, so a synthetic short-string id is written alongside the image
// and restored together with it. The blob handed to the
// game is malloc'd and tagged with free() as its deleter, so whatever the
// engine does with the image afterwards (move, destroy, skin rebuild) is
// memory-safe. The original cape is restored when the module is disabled or
// "None" is picked.
//
// Memory safety notes:
//   * all patching happens inside the local-player tick, so the only skin
//     object ever dereferenced is one freshly resolved from the live player
//     pointer — a previous skin object can never dangle.
//   * when the skin object is replaced by the game, the old object owns the
//     blob we injected (freed by the engine through our deleter), so no
//     cleanup is needed on our side.
//   * the cape offsets are version-specific and derived from the verified
//     skin offsets (see include/bedrocktools/sdk/offsets/Skin.hpp); a sanity
//     check on the live skin image aborts the patch if the layout shifts.
//   * persona skins go through the persona pipeline (no classic cape image),
//     so the module leaves them untouched.
class CustomCapesModule : public Module {
public:
    CustomCapesModule();
    ~CustomCapesModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    // Called from the LocalPlayerTickEvent subscription.
    void onLocalPlayerTick(void* player);

    // Directory the module watches; exposed for the menu description.
    const std::string& capesDirectory() const { return m_capesDir; }

private:
    void ensureCapesDirectory();
    void writeSamplePng(const std::string& path) const;
    void loadSelectedCape();
    void releaseLoadedCape();

    bool applyCustomCape(void* skin);
    void restoreOriginalCape(void* skin);
    void clearPatchState();

    std::string m_capesDir;
    std::vector<std::string> m_files; // refreshed by saveConfig (menu build / save)
    int m_selectedIndex = 0;          // 0 = None, i>=1 -> m_files[i-1]

    // Decoded, already resampled cape pixels (module-owned), w*h = 64*32.
    std::vector<std::uint8_t> m_pixels;
    bool m_capeLoaded = false;
    bool m_loadFailed = false;
    int m_retryTicks = 0;

    // State of the in-game skin patch.
    void* m_patchedSkin = nullptr;  // SerializedSkinImpl* currently patched
    void* m_injectedBlob = nullptr; // pixel buffer currently handed to the game
    bool m_needsApply = true;

    // Backup of the cape image we overwrote so "None"/disable can restore it.
    struct CapeBackup {
        uint32_t format = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0;
        uint32_t usage = 0;
        void* deleter = nullptr;
        size_t size = 0;
        std::vector<std::uint8_t> pixels; // deep copy of original pixels (may be empty)
        bool hadPixels = false;
        std::uint8_t capeIdBytes[24] = {};  // raw copy of the original mCapeId std::string
    };
    CapeBackup m_backup;
    bool m_hasBackup = false;
};

extern CustomCapesModule* g_customCapes;
