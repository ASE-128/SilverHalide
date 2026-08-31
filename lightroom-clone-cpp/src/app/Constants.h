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

// Preview LRU cache budget. Single source of truth: the engine Pipeline reads
// this to size its default cache capacity (see Pipeline ctor). 256 MB holds
// roughly 40 previews at 1080p (~6 MB each in RGB U8), so a browsing session
// keeps the history of recent slider states across several photos.
inline constexpr int kCacheCapacityMB = 256;

// Label of the built-in dark stylesheet inside the Qt resource bundle.
inline constexpr const char* kDarkQssResource = ":/styles/dark.qss";

// Application icon (PNG) embedded in the Qt resource bundle.
inline constexpr const char* kAppIconResource = ":/icons/app_256.png";

// S13: throttle / debounce timing constants. Single source of truth for
// every QTimer::setInterval call that gates UI updates on rapid user input
// (slider drags, mask strokes, metadata typing, search typing, spot drags,
// coarse preview frames, histogram coalescing, adjustment DB writes).
// Values are the ones that shipped before the consolidation (no behaviour
// change); keeping them together makes future tuning auditable and lets the
// headless build reason about UI timing without a Qt pull.
namespace throttle {

// Develop panel: refine-path debounce (ms) before a slider change triggers a
// full-resolution re-render. Lower = more responsive, higher = fewer re-renders.
inline constexpr int kSliderDebounceMs = 50;

// Develop panel: coarse live-preview throttle (ms) while a slider is being
// dragged. The refine path keeps kSliderDebounceMs; this only gates the
// coarse pass so the slider feels live without flooding the pipeline.
inline constexpr int kSliderCoarseDebounceMs = 30;

// Develop panel: mask / brush-stroke geometry commit throttle (ms). Every
// mouse move reaches updateMaskGeometry; a leading-edge one-shot timer
// limits that to ~one commit per interval while the stroke runs, so
// continuous strokes do not force a full panel re-sync + DB write per move.
inline constexpr int kMaskGeometryCommitMs = 150;

// Develop panel: metadata (caption / title / keywords) commit debounce (ms)
// while the user types. Line edits and focus-out commit immediately via
// emitMetadataEdited; this only flushes the trailing type-ahead.
inline constexpr int kMetadataCommitMs = 600;

// Develop panel: color-wheel (highlight / shadow) commit debounce (ms) while
// the user drags a wheel. Coalesces the trailing adjustmentsComitted
// emissions so each drag becomes one undo entry instead of one per wheel tick.
inline constexpr int kColorWheelCommitMs = 600;

// MainWindow: histogram + status-bar update coalesce (ms). Coarse preview
// frames arrive at up to ~33Hz; this throttle bounds the UI repaint. The
// one-shot timer guarantees the last frame of a window still lands.
inline constexpr int kHistogramThrottleMs = 150;

// MainWindow: adjustment DB write debounce (ms). JSON serialize + the sqlite
// statements leave the slider hot path and coalesce here; the latest value
// always wins and is flushed on timeout / image switch / window close.
inline constexpr int kAdjustmentSaveDebounceMs = 500;

// MainWindow: spot-removal gesture commit debounce (ms). A spot drag emits
// spotRemovalsEdited live and once more on release, with no separate "gesture
// finished" signal; this turns the trailing emissions into one undo entry.
inline constexpr int kSpotCommitDebounceMs = 250;

// MainWindow: library search box debounce (ms) before the filter applies.
// Bounds re-filter work while the user types a long query.
inline constexpr int kSearchDebounceMs = 300;

} // namespace throttle

} // namespace lrc::app
