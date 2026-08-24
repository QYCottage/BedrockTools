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
//   * resampling an arbitrary RGBA image into the visible classic-cape face
//     and the Elytra UV area of the 64x32 canvas Minecraft expects

#include <algorithm>
#include <array>
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
//     right  ( 0, 1)  1x16      back   ( 1, 1) 10x16   <- outer face (design)
//     left   (11, 1)  1x16      front  (12, 1) 10x16   <- inner face (lining)
inline constexpr std::uint32_t kCapeWidth = 64;
inline constexpr std::uint32_t kCapeHeight = 32;

// The outer (worn, visible) face the image is painted onto — the region
// x=1..11, y=1..17 of the 64x32 canvas.
inline constexpr std::uint32_t kCapeBackX = 1;
inline constexpr std::uint32_t kCapeBackY = 1;
inline constexpr std::uint32_t kCapeBackWidth = 10;
inline constexpr std::uint32_t kCapeBackHeight = 16;

// The inner face on the other side of the cuboid (same 10x16 size). The
// image is never repeated here; it is filled with one flat lining color.
inline constexpr std::uint32_t kCapeFrontX = 12;
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

// When an Elytra is equipped, Bedrock renders its 10x20x2 wing cube from the
// same cape image, at UV (22,0). Both wings share this area (the other wing is
// mirrored by the model), so a cape image with a transparent right-hand side
// produces an untextured/default Elytra. These are the six box-unwrapped
// regions sampled by geometry.elytra.
inline constexpr std::uint32_t kElytraUvX = 22;
inline constexpr std::uint32_t kElytraUvY = 0;
inline constexpr std::uint32_t kElytraUvWidth = 24;
inline constexpr std::uint32_t kElytraUvHeight = 22;
inline constexpr std::uint32_t kElytraFaceWidth = 10;
inline constexpr std::uint32_t kElytraFaceHeight = 20;
inline constexpr std::uint32_t kElytraDepth = 2;
inline constexpr std::uint32_t kElytraTopX = 24;       // 10x2
inline constexpr std::uint32_t kElytraTopY = 0;
inline constexpr std::uint32_t kElytraBottomX = 34;    // 10x2
inline constexpr std::uint32_t kElytraBottomY = 0;
inline constexpr std::uint32_t kElytraSideRightX = 22; // 2x20
inline constexpr std::uint32_t kElytraFrontX = 24;     // 10x20
inline constexpr std::uint32_t kElytraSideLeftX = 34;  // 2x20
inline constexpr std::uint32_t kElytraBackX = 36;      // 10x20, visible design
inline constexpr std::uint32_t kElytraSideY = 2;

// Vanilla's wing cube is rectangular, but transparent texels cut it into the
// familiar tapered Elytra silhouette. Each bit below represents x=22..45 for
// one row y=0..21 of textures/models/armor/elytra.png. Reusing only its alpha
// silhouette lets a custom cape supply the colors without turning the Elytra
// into two opaque rectangular slabs.
inline constexpr std::array<std::uint32_t, kElytraUvHeight> kElytraAlphaMask = {
    0x03fe00u, 0x000c00u, 0x0ff000u, 0x1ff000u, 0x3fe000u, 0x3fe000u,
    0x3fe000u, 0x7fe000u, 0x7fe000u, 0x7fe000u, 0x7fe000u, 0xffc001u,
    0xffc001u, 0xffc001u, 0xffc001u, 0xffc001u, 0xff8001u, 0xff8001u,
    0xff8001u, 0xff0001u, 0xff0001u, 0xfe0001u,
};

// True for every texel actually addressed by the Elytra box unwrap. The two
// unused 2x2 corners on the first two rows are deliberately excluded.
inline constexpr bool isElytraUvPixel(std::uint32_t x, std::uint32_t y) {
    if (x < kElytraUvX || x >= kElytraUvX + kElytraUvWidth ||
        y < kElytraUvY || y >= kElytraUvY + kElytraUvHeight) {
        return false;
    }
    return y >= kElytraDepth ||
           (x >= kElytraTopX && x < kElytraBottomX + kElytraFaceWidth);
}

inline constexpr bool isElytraMaskPixel(std::uint32_t x, std::uint32_t y) {
    if (x < kElytraUvX || x >= kElytraUvX + kElytraUvWidth ||
        y >= kElytraUvHeight) {
        return false;
    }
    return (kElytraAlphaMask[y] & (1u << (x - kElytraUvX))) != 0;
}

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

// Returns true when a complete 64x32 cape canvas already contains visible
// artwork in any texel used by geometry.elytra. Checking the whole UV unwrap,
// rather than only the vanilla silhouette, preserves custom wing shapes.
inline bool hasElytraArtwork(const std::vector<std::uint8_t>& canvas) {
    const std::size_t expected = static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u;
    if (canvas.size() < expected) return false;
    for (std::uint32_t y = 0; y < kElytraUvHeight; ++y) {
        for (std::uint32_t x = kElytraUvX; x < kElytraUvX + kElytraUvWidth; ++x) {
            if (!isElytraUvPixel(x, y)) continue;
            const std::size_t i = (static_cast<std::size_t>(y) * kCapeWidth + x) * 4u;
            if (canvas[i + 3] != 0) return true;
        }
    }
    return false;
}

// Paints the visible cape design onto the shared Elytra wing UV. This is a
// fallback for generated capes and complete 64x32 capes whose Elytra area is
// empty. The RGB and source alpha come from the cape's outer face; the final
// alpha is also clipped by the vanilla tapered-wing mask.
inline void paintElytraFromCape(std::vector<std::uint8_t>& canvas) {
    const std::size_t expected = static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u;
    if (canvas.size() < expected) return;

    const auto pixel = [&](std::uint32_t x, std::uint32_t y) -> std::uint8_t* {
        return &canvas[(static_cast<std::size_t>(y) * kCapeWidth + x) * 4u];
    };

    // Remove invisible RGB left in an otherwise transparent input, then add
    // only the texels belonging to the vanilla wing silhouette.
    for (std::uint32_t y = 0; y < kElytraUvHeight; ++y) {
        for (std::uint32_t x = kElytraUvX; x < kElytraUvX + kElytraUvWidth; ++x) {
            if (!isElytraUvPixel(x, y)) continue;
            std::memset(pixel(x, y), 0, 4);
        }
    }

    for (std::uint32_t y = 0; y < kElytraUvHeight; ++y) {
        for (std::uint32_t x = kElytraUvX; x < kElytraUvX + kElytraUvWidth; ++x) {
            if (!isElytraMaskPixel(x, y)) continue;

            // Map each unwrapped face back to an adjacent column of the cape
            // design. The large visible face (x=36..45) gets all ten columns;
            // the thin side strips continue the corresponding edge color.
            std::uint32_t designX = 0;
            if (x < kElytraTopX) {
                designX = kCapeBackWidth - 1;
            } else if (x < kElytraTopX + kElytraFaceWidth) {
                designX = x - kElytraTopX;
            } else if (x < kElytraBackX) {
                designX = 0;
            } else {
                designX = std::min(x - kElytraBackX, kCapeBackWidth - 1);
            }

            const std::uint32_t wingY = y < kElytraSideY ? 0 : y - kElytraSideY;
            const std::uint32_t designY = std::min(
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(wingY) * kCapeBackHeight) /
                    kElytraFaceHeight),
                kCapeBackHeight - 1);

            std::memcpy(pixel(x, y),
                        pixel(kCapeBackX + designX, kCapeBackY + designY), 4);
        }
    }
}

// Nearest-neighbor resample of an RGBA8 buffer onto the classic-cape and
// Elytra layouts of the 64x32 canvas:
//
//   * the image is painted onto the outer BACK face, (1,1) 10x16;
//   * the inner FRONT face (12,1) never repeats the image — it gets one flat
//     lining color (a half-brightness average of the visible design);
//   * the cape's Top/Bottom/Side strips continue the adjacent edge pixels;
//   * the same design is mapped onto the tapered Elytra UV at (22,0), so it
//     remains visible when the player equips an Elytra.
//
// An exact 64x32 input keeps its manually authored Elytra pixels. If that UV
// area is fully transparent, the fallback is generated from the cape face.
// Sequential input/output traversal keeps the cache behavior linear, which
// is plenty for one-off loads of a few hundred KB.
inline std::vector<std::uint8_t> resampleToCape(const std::uint8_t* rgba, std::uint32_t width,
                                                std::uint32_t height) {
    std::vector<std::uint8_t> out;
    out.resize(static_cast<std::size_t>(kCapeWidth) * kCapeHeight * 4u, 0);
    if (!rgba || width == 0 || height == 0) return out;

    if (width == kCapeWidth && height == kCapeHeight) {
        out.assign(rgba, rgba + out.size());
        if (!hasElytraArtwork(out)) paintElytraFromCape(out);
        return out;
    }

    const auto px = [](std::vector<std::uint8_t>& canvas, std::uint32_t x,
                       std::uint32_t y) -> std::uint8_t* {
        return &canvas[(static_cast<std::size_t>(y) * kCapeWidth + x) * 4u];
    };

    // 1) Image -> outer back face only, x=1..11, y=1..17.
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

    // 4) Elytra: reuse the cape design in the UV area sampled by both wings.
    paintElytraFromCape(out);

    return out;
}

} // namespace customcapes
