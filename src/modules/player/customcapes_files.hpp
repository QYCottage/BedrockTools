#pragma once

// Pure helpers for the Custom Capes module.
//
// Everything in this header is plain C++ with no Minecraft, launcher or
// mod-menu dependencies so it can be unit-tested on the host (see
// tests/customcapes_test.cpp). It covers the three "dumb data" problems of
// the module:
//
//   * scanning the capes directory for usable PNG files
//   * (de)serializing the launcher "radio" config value used for the picker
//   * resampling an arbitrary RGBA image into the visible 10x16 classic-cape
//     box at (1,1) on the 64x32 canvas Minecraft expects

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>
#include <vector>

#if __has_include(<filesystem>)
#include <filesystem>
#define CUSTOMCAPES_HAS_FILESYSTEM 1
#else
#define CUSTOMCAPES_HAS_FILESYSTEM 0
#endif

namespace customcapes {

// Vanilla classic capes are rendered from a 64x32 RGBA8 texture. The cape is
// a 10x16x1 cuboid unwrapped at texture offset (0,0), which distributes its
// six faces over the canvas like this (standard box unwrap):
//
//     top    ( 1, 0) 10x 1      bottom (11, 0) 10x 1
//     right  ( 0, 1)  1x16      front  ( 1, 1) 10x16   <- inner face
//     left   (11, 1)  1x16      back   (12, 1) 10x16   <- outer face (design)
inline constexpr std::uint32_t kCapeWidth = 64;
inline constexpr std::uint32_t kCapeHeight = 32;

// The outer (worn, visible) face the image is painted onto — the region
// x=12..22, y=1..17 of the 64x32 canvas.
inline constexpr std::uint32_t kCapeBackX = 12;
inline constexpr std::uint32_t kCapeBackY = 1;
inline constexpr std::uint32_t kCapeBackWidth = 10;
inline constexpr std::uint32_t kCapeBackHeight = 16;

// The inner face on the other side of the cuboid (same 10x16 size). The
// image is never repeated here; it is filled with one flat lining color.
inline constexpr std::uint32_t kCapeFrontX = 1;
inline constexpr std::uint32_t kCapeFrontY = 1;

// The 1-voxel-thick edge strips of the cuboid; painting them with colors
// from the image's edges keeps the cape from looking paper-thin.
inline constexpr std::uint32_t kCapeTopX = 1;      // 10x1 strip on row y=0
inline constexpr std::uint32_t kCapeTopY = 0;
inline constexpr std::uint32_t kCapeBottomX = 11;  // 10x1 strip on row y=0
inline constexpr std::uint32_t kCapeBottomY = 0;
inline constexpr std::uint32_t kCapeSideRightX = 0; // 1x16 strip at x=0
inline constexpr std::uint32_t kCapeSideLeftX = 11; // 1x16 strip at x=11
inline constexpr std::uint32_t kCapeSideY = 1;

// Safety cap so a hostile/corrupt PNG can never exhaust device memory.
inline constexpr std::uint32_t kMaxSourceDimension = 4096;

// Index 0 of the radio picker is always the "no custom cape" entry.
inline constexpr const char* kNoneLabel = "None";

inline bool hasPngExtension(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".png";
}

// A file name becomes a radio option, and options are comma-separated, so a
// file whose name contains a comma could never round-trip through the
// launcher menu. Such files are skipped instead of corrupting the list.
inline bool isUsableCapeFileName(const std::string& name) {
    if (!hasPngExtension(name)) return false;
    return name.find(',') == std::string::npos;
}

// Lists the capes directory (non-recursive), returning plain file names of
// PNG files, sorted alphabetically. Missing/inaccessible directories simply
// yield an empty list.
inline std::vector<std::string> scanCapeFiles(const std::string& directory) {
    std::vector<std::string> files;
#if CUSTOMCAPES_HAS_FILESYSTEM
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec) || ec) return files;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;
        const std::string name = entry.path().filename().string();
        if (isUsableCapeFileName(name)) files.push_back(name);
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
#endif
    return files;
}

// Serializes the picker value in the launcher's radio format:
// "<currentIndex>,<None>,<file1>,<file2>,..." — the menu renders the part
// after the first comma as the option list and treats the part before it as
// the selected index (same convention the Crosshair module uses).
inline std::string makeRadioValue(int selectedIndex, const std::vector<std::string>& files) {
    const int optionCount = 1 + static_cast<int>(files.size());
    if (selectedIndex < 0) selectedIndex = 0;
    if (selectedIndex >= optionCount) selectedIndex = optionCount - 1;

    std::string value = std::to_string(selectedIndex);
    value += ',';
    value += kNoneLabel;
    for (const std::string& file : files) {
        value += ',';
        value += file;
    }
    return value;
}

// Parses a radio value coming from the config file or from the launcher
// (which reports just the index when the selection changes). Returns the
// selected index and, when the option list is embedded in the value, the
// selected option's name so the caller can recover the chosen file even if
// the on-disk listing changed since the value was written.
inline bool parseRadioValue(const std::string& value, int& outIndex, std::string& outName) {
    outIndex = 0;
    outName.clear();
    if (value.empty()) return false;

    std::vector<std::string> tokens;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = value.find(',', start);
        tokens.push_back(value.substr(start, comma == std::string::npos ? comma : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }

    bool numericIndex = !tokens[0].empty();
    for (char c : tokens[0]) {
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '+') {
            numericIndex = false;
            break;
        }
    }

    if (numericIndex) {
        try {
            outIndex = std::stoi(tokens[0]);
        } catch (...) {
            outIndex = 0;
        }
        const int option = outIndex; // option list includes "None" as entry 0
        if (option > 0 && option < static_cast<int>(tokens.size()) - 1) {
            outName = tokens[option + 1];
        } else if (option == 0 && tokens.size() > 1) {
            outName = tokens[1]; // normally "None"
        }
        return true;
    }

    // A bare file name (no numeric index): treat it as that cape's selection.
    outIndex = 0;
    outName = tokens[0];
    return !outName.empty();
}

// Maps a parsed radio selection back onto the current on-disk listing.
// files[0] never exists (index 0 == None). Returns the index within the
// "None + files" option space: 0 when nothing is selected/found.
inline int resolveSelectionIndex(int parsedIndex, const std::string& parsedName,
                                 const std::vector<std::string>& files) {
    // Prefer the recovered file name; it survives list reordering.
    if (!parsedName.empty() && parsedName != kNoneLabel) {
        for (std::size_t i = 0; i < files.size(); ++i) {
            if (files[i] == parsedName) return static_cast<int>(i) + 1;
        }
        // The file was deleted/renamed since the value was written.
        return 0;
    }
    if (parsedIndex <= 0) return 0;
    if (parsedIndex <= static_cast<int>(files.size())) return parsedIndex;
    return 0;
}

// Nearest-neighbor resample of an RGBA8 buffer onto the classic-cape layout
// of the 64x32 canvas:
//
//   * the image is painted onto the outer BACK face only, (12,1) 10x16;
//   * the inner FRONT face (1,1) never repeats the image — it gets one flat
//     lining color (a half-brightness average of the visible design), so the
//     same picture no longer shows on both sides of the cape;
//   * the Top/Bottom/Side edge strips are painted from the adjacent
//     back-face edge pixels, giving the 1-voxel-thick mesh visible thickness.
//
// An exact 64x32 input is already a complete cape canvas and is therefore
// copied pixel-for-pixel, preserving full manual control over every face.
// Sequential input/output traversal keeps the cache behavior linear, which
// is plenty for one-off loads of a few hundred KB.
inline std::vector<std::uint8_t> resampleToCape(const std::uint8_t* rgba, std::uint32_t width,
                                                std::uint32_t height) {
    std::vector<std::uint8_t> out;
    out.resize(static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u, 0);
    if (!rgba || width == 0 || height == 0) return out;

    if (width == kCapeWidth && height == kCapeHeight) {
        out.assign(rgba, rgba + out.size());
        return out;
    }

    const auto px = [](std::vector<std::uint8_t>& canvas, std::uint32_t x,
                       std::uint32_t y) -> std::uint8_t* {
        return &canvas[(static_cast<std::size_t>(y) * kCapeWidth + x) * 4u];
    };

    // 1) Image -> outer back face only, x=12..22, y=1..17.
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        const std::uint32_t srcY = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(y) * height) / kCapeBackHeight);
        for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
            const std::uint32_t srcX = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * width) / kCapeBackWidth);
            const std::size_t src = (static_cast<std::size_t>(srcY) * width + srcX) * 4u;
            std::uint8_t* dst = px(out, kCapeBackX + x, kCapeBackY + y);
            dst[0] = rgba[src + 0];
            dst[1] = rgba[src + 1];
            dst[2] = rgba[src + 2];
            dst[3] = rgba[src + 3];
        }
    }

    // 2) Inner front face: one flat lining color derived from the pixels
    // actually shown on the back face (RGB halved) — a repeat of the image
    // is exactly what must not happen here.
    std::uint32_t sum[4] = {0, 0, 0, 0};
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
            const std::uint8_t* p = px(out, kCapeBackX + x, kCapeBackY + y);
            for (int c = 0; c < 4; ++c) sum[c] += p[c];
        }
    }
    const std::uint32_t area = kCapeBackWidth * kCapeBackHeight;
    std::uint8_t liner[4];
    for (int c = 0; c < 3; ++c) liner[c] = static_cast<std::uint8_t>((sum[c] / area) / 2u);
    liner[3] = static_cast<std::uint8_t>(sum[3] / area);
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
            std::uint8_t* p = px(out, kCapeFrontX + x, kCapeFrontY + y);
            for (int c = 0; c < 4; ++c) p[c] = liner[c];
        }
    }

    // 3) Thickness: continue the design onto the 1-voxel edge strips by
    // copying the adjacent back-face edge pixels.
    for (std::uint32_t x = 0; x < kCapeBackWidth; ++x) {
        std::memcpy(px(out, kCapeTopX + x, kCapeTopY),                       // top strip
                    px(out, kCapeBackX + x, kCapeBackY), 4);                 // <- design top row
        std::memcpy(px(out, kCapeBottomX + x, kCapeBottomY),                 // bottom strip
                    px(out, kCapeBackX + x, kCapeBackY + kCapeBackHeight - 1),
                    4);                                                      // <- design bottom row
    }
    for (std::uint32_t y = 0; y < kCapeBackHeight; ++y) {
        std::memcpy(px(out, kCapeSideRightX, kCapeSideY + y),                // side strip
                    px(out, kCapeBackX, kCapeBackY + y), 4);                 // <- design left column
        std::memcpy(px(out, kCapeSideLeftX, kCapeSideY + y),                 // side strip
                    px(out, kCapeBackX + kCapeBackWidth - 1, kCapeBackY + y),
                    4);                                                      // <- design right column
    }

    return out;
}

} // namespace customcapes
