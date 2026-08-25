#pragma once

// Application-wide constants. Kept Qt-free so the engine can share the values
// when it needs them (e.g. preview resolution).

namespace lrc::app {

inline constexpr const char* kAppName = "Silver Halide";
inline constexpr const char* kAppVersion = "2.12.0";
inline constexpr const char* kAppOrganization = "ASE-128";

// Name of the built-in default catalog. When no catalog has been chosen
// or created, imports go into this catalog. (default catalog name)
inline constexpr const char* kDefaultCatalogName = "Default";
// File name of the built-in default catalog.
inline constexpr const char* kDefaultCatalogFile = "Default.lrcat";

// Longest preview side (pixels). Slider updates must complete a full
// preview render within 200 ms at this resolution.
inline constexpr int kPreviewMaxDim = 1920;

// Longest coarse preview side (pixels). A fast low-res render shown
// immediately while the full 1920 preview is still refining (coarse-then-
// refine, M5). Kept distinct so the LRU cache keys separate the two.
inline constexpr int kCoarseMaxDim = 480;

// Longest filmstrip thumbnail side.
inline constexpr int kThumbnailDim = 320;

// Develop panel debounce (ms) before a slider change triggers a re-render.
inline constexpr int kDebounceMs = 50;

// Preview LRU cache budget. Single source of truth: the engine Pipeline reads
// this to size its default cache capacity (see Pipeline ctor). 256 MB holds
// roughly 40 previews at 1080p (~6 MB each in RGB U8), so a browsing session
// keeps the history of recent slider states across several photos.
inline constexpr int kCacheCapacityMB = 256;

// Label of the built-in dark stylesheet inside the Qt resource bundle.
inline constexpr const char* kDarkQssResource = ":/styles/dark.qss";

// Application icon (PNG) embedded in the Qt resource bundle.
inline constexpr const char* kAppIconResource = ":/icons/app_256.png";

} // namespace lrc::app
