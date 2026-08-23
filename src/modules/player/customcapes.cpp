#include "customcapes.hpp"
#include "customcapes_files.hpp"

#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/events/EventBus.hpp>
#include "../../config/ConfigManager.hpp"

// PNG only: the capes folder is .png files, and stripping the other decoders
// keeps the LTO'd binary small.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_JPEG
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include <stb/stb_image.h>
// Declarations only — the implementation lives in skinstealer.cpp.
#include <stb/stb_image_write.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

CustomCapesModule* g_customCapes = nullptr;

namespace {

using namespace bedrocktools::sdk::offsets;

// Deleter installed on cape blobs we hand to the engine. mce::Blob stores a
// plain function pointer (void(*)(unsigned char*)) next to the pixel
// pointer and calls it when the image is destroyed; pointing it at free()
// makes our malloc'd buffers behave exactly like a vanilla cape blob.
void freeBlobDeleter(unsigned char* data) {
    std::free(data);
}

// mce::ImageFormat::RGBA8Unorm == 4 (see include/bedrocktools/sdk/offsets/Skin.hpp).
constexpr std::uint32_t kCapeImageFormat = 4;

// How often (in ticks) a failed load is retried, in case the file appeared
// after the picker entry did.
constexpr int kLoadRetryTicks = 120;

// A synthetic non-empty cape id. Modern game versions gate cape rendering on
// the id being present at all, so a bare image blob is not enough.
constexpr const char* kCapeId = "bedrocktools";
constexpr std::size_t kCapeIdLen = 12; // strlen("bedrocktools")

// Writes a short (SSO) std::string into a libc++ string slot (24 bytes) that
// the game already owns, e.g. SerializedSkinImpl::mCapeId. libc++ packs a
// short string's length into the first byte as (len << 1) with the low bit
// clear; len <= 22 always fits inline so no heap allocation and no capacity
// bookkeeping is needed. The original bytes must be backed up first.
void writeShortStdString(uintptr_t addr, const char* text, std::size_t len) {
    unsigned char* p = reinterpret_cast<unsigned char*>(addr);
    std::memset(p, 0, 24);
    p[0] = static_cast<unsigned char>(len << 1);
    std::memcpy(p + 1, text, len);
}

std::string capeDirectoryForConfig() {
    const std::string configPath = bedrocktools::config::ConfigManager::get().getConfigPath();
    const std::size_t lastSlash = configPath.find_last_of('/');
    std::string dir = (lastSlash != std::string::npos) ? configPath.substr(0, lastSlash)
                                                       : "/sdcard/games/BedrockTools";
    return dir + "/capes";
}

// Resolves the local player's SerializedSkinImpl* following the same chain
// Skin Stealer uses: Player::mSkin -> ref -> ThreadOwner -> object.
void* resolvePlayerSkin(void* player) {
    void* skinRefPtr = *reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(player) + Player::mSkin);
    if (!skinRefPtr) return nullptr;

    void* sharedBase = reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(skinRefPtr) + SerializedSkinRef::mSkinImpl);
    void* threadOwner = *reinterpret_cast<void**>(sharedBase);
    if (!threadOwner) return nullptr;

    return reinterpret_cast<void*>(
        reinterpret_cast<uintptr_t>(threadOwner) + ThreadOwner::mObject);
}

} // namespace

CustomCapesModule::CustomCapesModule()
    : Module("Custom Capes", "Wear any PNG from the BedrockTools capes folder as your cape (local only).") {
    g_customCapes = this;
}

CustomCapesModule::~CustomCapesModule() {
    if (g_customCapes == this) g_customCapes = nullptr;
}

void CustomCapesModule::onInit() {
    m_capesDir = capeDirectoryForConfig();
    ensureCapesDirectory();
    m_files = customcapes::scanCapeFiles(m_capesDir);

    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>(
        [](auto& event) {
            if (g_customCapes) g_customCapes->onLocalPlayerTick(event.player);
        });
}

void CustomCapesModule::onEnable() {
    if (m_selectedIndex > 0 && !m_capeLoaded && !m_loadFailed) loadSelectedCape();
    m_needsApply = true;
}

void CustomCapesModule::onDisable() {
    // The next tick restores the vanilla cape through the live player
    // pointer; nothing is touched here so no pointer can dangle.
}

// ---------------------------------------------------------------------------
// capes directory
// ---------------------------------------------------------------------------

void CustomCapesModule::ensureCapesDirectory() {
    std::error_code ec;
    if (std::filesystem::is_directory(m_capesDir, ec)) return;
    if (!std::filesystem::create_directories(m_capesDir, ec) || ec) return;
    // First launch: drop a sample cape so the picker is not empty.
    writeSamplePng(m_capesDir + "/Sample Cape.png");
}

void CustomCapesModule::writeSamplePng(const std::string& path) const {
    // 10x16 purple gradient with bright accents. It is expanded onto the
    // 64x32 canvas through the same layout rules as user-supplied images
    // (design on the outer back face, lining color on the inner face, edge
    // colors on the top/bottom/side strips), so the sample shows exactly
    // what a picked file will look like in-game.
    std::vector<std::uint8_t> src(customcapes::kCapeBackWidth * customcapes::kCapeBackHeight * 4u);
    for (std::uint32_t y = 0; y < customcapes::kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < customcapes::kCapeBackWidth; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * customcapes::kCapeBackWidth + x) * 4u;
            const float t = static_cast<float>(y) / static_cast<float>(customcapes::kCapeBackHeight - 1);
            src[i + 0] = static_cast<std::uint8_t>(70 + 120 * t);
            src[i + 1] = static_cast<std::uint8_t>(30 + 30 * t);
            src[i + 2] = static_cast<std::uint8_t>(160 + 60 * t);
            src[i + 3] = 255;
            const bool border = x < 2 || y < 2 ||
                                x >= customcapes::kCapeBackWidth - 2 ||
                                y >= customcapes::kCapeBackHeight - 2;
            const bool stripe = x == 4 || x == 5;
            if (border || stripe) {
                src[i + 0] = 255; src[i + 1] = 220; src[i + 2] = 60; src[i + 3] = 255;
            }
        }
    }
    const std::vector<std::uint8_t> px = customcapes::resampleToCape(
        src.data(), customcapes::kCapeBackWidth, customcapes::kCapeBackHeight);
    stbi_write_png(path.c_str(), customcapes::kCapeWidth, customcapes::kCapeHeight, 4,
                   px.data(), customcapes::kCapeWidth * 4);
}

// ---------------------------------------------------------------------------
// config <-> picker
// ---------------------------------------------------------------------------

void CustomCapesModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);

    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    const int previousIndex = m_selectedIndex;

    if (j.contains("m_cape")) {
        int parsedIndex = m_selectedIndex;
        std::string parsedName;
        if (j["m_cape"].is_string()) {
            customcapes::parseRadioValue(j["m_cape"].get<std::string>(), parsedIndex, parsedName);
        } else if (j["m_cape"].is_number_integer()) {
            parsedIndex = j["m_cape"].get<int>();
        }
        m_files = customcapes::scanCapeFiles(m_capesDir);
        m_selectedIndex = customcapes::resolveSelectionIndex(parsedIndex, parsedName, m_files);
    }

    if (m_selectedIndex != previousIndex || (m_selectedIndex > 0 && !m_capeLoaded)) {
        releaseLoadedCape();
        if (m_selectedIndex > 0) loadSelectedCape();
        m_needsApply = true;
    }
}

void CustomCapesModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    if (m_capesDir.empty()) m_capesDir = capeDirectoryForConfig();
    m_files = customcapes::scanCapeFiles(m_capesDir);
    if (m_selectedIndex > static_cast<int>(m_files.size())) m_selectedIndex = 0;
    j["m_cape"] = customcapes::makeRadioValue(m_selectedIndex, m_files);
}

// ---------------------------------------------------------------------------
// cape image loading
// ---------------------------------------------------------------------------

void CustomCapesModule::releaseLoadedCape() {
    m_pixels.clear();
    m_pixels.shrink_to_fit();
    m_capeLoaded = false;
    m_loadFailed = false;
    m_retryTicks = 0;
}

void CustomCapesModule::loadSelectedCape() {
    releaseLoadedCape();
    if (m_selectedIndex <= 0 || m_selectedIndex > static_cast<int>(m_files.size())) return;

    const std::string path = m_capesDir + "/" + m_files[static_cast<std::size_t>(m_selectedIndex - 1)];

    int width = 0, height = 0, channels = 0;
    stbi_uc* decoded = stbi_load(path.c_str(), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0 ||
        width > static_cast<int>(customcapes::kMaxSourceDimension) ||
        height > static_cast<int>(customcapes::kMaxSourceDimension)) {
        if (decoded) stbi_image_free(decoded);
        m_loadFailed = true;
        return;
    }

    if (width == static_cast<int>(customcapes::kCapeWidth) &&
        height == static_cast<int>(customcapes::kCapeHeight)) {
        const std::size_t bytes = customcapes::kCapeWidth * customcapes::kCapeHeight * 4u;
        m_pixels.assign(decoded, decoded + bytes);
    } else {
        m_pixels = customcapes::resampleToCape(decoded, static_cast<std::uint32_t>(width),
                                               static_cast<std::uint32_t>(height));
    }
    stbi_image_free(decoded);
    m_capeLoaded = true;
}

// ---------------------------------------------------------------------------
// in-game skin patch
// ---------------------------------------------------------------------------

void CustomCapesModule::clearPatchState() {
    m_patchedSkin = nullptr;
    m_injectedBlob = nullptr;
    m_hasBackup = false;
    m_backup.pixels.clear();
}

void CustomCapesModule::onLocalPlayerTick(void* player) {
    if (!player) return;

    if (m_selectedIndex > 0 && !m_capeLoaded && enabled) {
        // The picker points at a file we have no pixels for; retry with a
        // cooldown so a broken/missing file does not stall the tick.
        if (m_retryTicks <= 0) {
            loadSelectedCape();
            if (!m_capeLoaded) m_retryTicks = kLoadRetryTicks;
        } else {
            --m_retryTicks;
        }
    }

    void* skin = resolvePlayerSkin(player);

    if (!enabled || m_selectedIndex <= 0 || !m_capeLoaded || !skin) {
        // Picker on None / module off / no decoded cape: put the vanilla
        // cape back if we still have one patched in.
        restoreOriginalCape(skin);
        return;
    }

    applyCustomCape(skin);
}

bool CustomCapesModule::applyCustomCape(void* skin) {
    // Persona skins are drawn from the persona pipeline (they have no classic
    // cape image), so leave them untouched.
    if (*reinterpret_cast<bool*>(
            reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mIsPersona)) {
        return false;
    }

    // Sanity-check the verified skin image before writing anywhere near it:
    // if a future version moved the member, bail out instead of corrupting
    // memory. Vanilla/private skins are 64x64 or 128x128 RGBA.
    const uintptr_t skinImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mSkinImage;
    const uint32_t skinW = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mWidth);
    const uint32_t skinH = *reinterpret_cast<uint32_t*>(skinImage + SkinImage::mHeight);
    void* skinPx = *reinterpret_cast<void**>(skinImage + Image::mBytesOffset);
    const bool layoutOk = skinPx != nullptr &&
                          (skinW == 64 || skinW == 128) && (skinH == 64 || skinH == 128);
    if (!layoutOk) return false;

    const uintptr_t capeImage =
        reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeImage;

    // The backing buffer the game currently sees, plus its metadata.
    void*& curBlob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    size_t& curBlobSize = *reinterpret_cast<size_t*>(capeImage + Image::mBlobSizeOffset);
    void*& curDeleter = *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset);
    uint32_t& curFormat = *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat);
    uint32_t& curWidth = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth);
    uint32_t& curHeight = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight);
    uint32_t& curDepth = *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth);
    uint8_t& curUsage = *reinterpret_cast<uint8_t*>(capeImage + Image::mUsage);

    if (m_patchedSkin != skin) {
        // Fresh skin object (join/dimension change/skin swap): the previous
        // object owns its blob (the engine frees it through our deleter), so
        // only back the new object up and start over.
        m_backup.format = curFormat;
        m_backup.width = curWidth;
        m_backup.height = curHeight;
        m_backup.depth = curDepth;
        m_backup.usage = curUsage;
        m_backup.deleter = curDeleter;
        m_backup.size = curBlobSize;
        m_backup.hadPixels = curBlob != nullptr &&
                             curBlobSize > 0 && curBlobSize < (64u * 1024u * 1024u);
        if (m_backup.hadPixels) {
            m_backup.pixels.assign(static_cast<std::uint8_t*>(curBlob),
                                   static_cast<std::uint8_t*>(curBlob) + curBlobSize);
        } else {
            m_backup.pixels.clear();
        }
        std::memcpy(m_backup.capeIdBytes,
                    reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(skin) +
                                            SerializedSkinImpl::mCapeId),
                    24);
        m_patchedSkin = skin;
        m_injectedBlob = nullptr;
        m_needsApply = true;
    }
    m_hasBackup = true;

    const bool alreadyApplied =
        !m_needsApply && curBlob == m_injectedBlob && m_injectedBlob != nullptr &&
        curWidth == customcapes::kCapeWidth && curHeight == customcapes::kCapeHeight;
    if (alreadyApplied) return true;

    // Hand the game a fresh buffer it then owns (freed via freeBlobDeleter).
    const std::size_t bytes = m_pixels.size();
    void* newBlob = std::malloc(bytes);
    if (!newBlob) return false;
    std::memcpy(newBlob, m_pixels.data(), bytes);

    if (curBlob == m_injectedBlob && m_injectedBlob != nullptr) {
        // The currently installed blob is ours; re-own it before replacing so
        // nothing leaks. The game cannot be moving this blob around inside
        // its own tick, and the render path only ever reads it.
        std::free(m_injectedBlob);
    }

    curFormat = kCapeImageFormat;
    curWidth = customcapes::kCapeWidth;
    curHeight = customcapes::kCapeHeight;
    curDepth = 1;
    curUsage = *reinterpret_cast<uint8_t*>(skinImage + Image::mUsage); // mirror the skin's usage tag
    curBlob = newBlob;
    curDeleter = reinterpret_cast<void*>(&freeBlobDeleter);
    curBlobSize = bytes;

    m_injectedBlob = newBlob;

    // Modern versions skip classic-cape rendering for an empty mCapeId, so
    // write a synthetic short-string id next to the image.
    writeShortStdString(reinterpret_cast<uintptr_t>(skin) + SerializedSkinImpl::mCapeId,
                        kCapeId, kCapeIdLen);

    m_needsApply = false;
    return true;
}

void CustomCapesModule::restoreOriginalCape(void* skin) {
    if (!m_hasBackup) {
        clearPatchState();
        return;
    }
    // Only ever write into the skin resolved from the *live* player; if the
    // object changed, the old one (with our blob) belongs to the engine and
    // its vanilla replacement is already in place.
    if (skin != m_patchedSkin || skin == nullptr) {
        clearPatchState();
        return;
    }

    const uintptr_t capeImage =
        reinterpret_cast<uintptr_t>(m_patchedSkin) + SerializedSkinImpl::mCapeImage;
    void*& curBlob = *reinterpret_cast<void**>(capeImage + Image::mBytesOffset);
    size_t& curBlobSize = *reinterpret_cast<size_t*>(capeImage + Image::mBlobSizeOffset);
    void*& curDeleter = *reinterpret_cast<void**>(capeImage + Image::mBlobDeleterOffset);
    uint32_t& curFormat = *reinterpret_cast<uint32_t*>(capeImage + Image::mImageFormat);
    uint32_t& curWidth = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mWidth);
    uint32_t& curHeight = *reinterpret_cast<uint32_t*>(capeImage + SkinImage::mHeight);
    uint32_t& curDepth = *reinterpret_cast<uint32_t*>(capeImage + Image::mDepth);
    uint8_t& curUsage = *reinterpret_cast<uint8_t*>(capeImage + Image::mUsage);

    // If the engine rebuilt the cape itself, the vanilla state is back.
    if (curBlob != m_injectedBlob || m_injectedBlob == nullptr) {
        clearPatchState();
        return;
    }

    std::free(curBlob); // our blob, currently installed -> safe to reclaim

    if (m_backup.hadPixels && !m_backup.pixels.empty()) {
        void* restored = std::malloc(m_backup.pixels.size());
        if (restored) {
            std::memcpy(restored, m_backup.pixels.data(), m_backup.pixels.size());
            curBlob = restored;
            curDeleter = reinterpret_cast<void*>(&freeBlobDeleter);
            curBlobSize = m_backup.pixels.size();
        } else {
            curBlob = nullptr;
            curDeleter = nullptr;
            curBlobSize = 0;
        }
        curFormat = m_backup.format;
        curWidth = m_backup.width;
        curHeight = m_backup.height;
        curDepth = m_backup.depth;
        curUsage = static_cast<std::uint8_t>(m_backup.usage);
    } else {
        // The skin never had a cape: restore an empty image.
        curBlob = nullptr;
        curDeleter = nullptr;
        curBlobSize = 0;
        curFormat = m_backup.format;
        curWidth = 0;
        curHeight = 0;
        curDepth = 1;
        curUsage = static_cast<std::uint8_t>(m_backup.usage);
    }

    // Put the original mCapeId back (it may be a heap pointer owned by the
    // engine), byte-for-byte as it was backed up.
    std::memcpy(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(m_patchedSkin) +
                                        SerializedSkinImpl::mCapeId),
                m_backup.capeIdBytes, 24);

    clearPatchState();
}
