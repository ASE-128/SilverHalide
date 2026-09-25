#pragma once

// P-B B-2 pregrade analyzer — pure C++ / Qt-free.
//
// Produces a lightweight per-image marker for the import batch pregrade queue:
//   • hasSubject — foreground saliency (optional rmbg2-subject model path,
//     deterministic saliency fallback when the model is absent).
//   • score — a deterministic quality/sharpness score in [0,100].
//   • suggestAuto — whether the tonal statistics suggest running B-1 auto-develop.
//
// DESIGN RULES (enforced by this header):
//   • No Qt includes anywhere in this header or the .cpp.
//   • Non-destructive: the input buffer is taken by const reference and never
//     mutated (verified by tests that snapshot the source bytes).
//   • Deterministic given the same input + engine state; callable from any
//     thread. Never throws; never crashes on a missing/unloadable model — a
//     failed or absent subject model falls back to statistics and sets
//     `processed=false` only when the input itself is degenerate.

#include <cstdint>

#include "engine/ImageBuffer.h"

namespace lrc {

class AiInferenceEngine;

/**
 * @brief Lightweight per-image pregrade marker (B-2).
 *
 * Written into the catalog after a background queue pass over each imported
 * image. `processed` is false only for empty / degenerate input; a missing
 * subject model does NOT set processed=false (the analyzer degrades to
 * deterministic saliency so the batch still completes).
 */
struct PregradeResult {
    bool hasSubject = false;  // strong foreground saliency detected
    int score = 0;            // deterministic quality score in [0,100]
    bool suggestAuto = false; // tonal stats suggest B-1 auto-develop
    bool processed = false;   // true when the input was analysable
};

/**
 * @brief Static batch-pregrade analyzer (B-2).
 *
 * Analyzes a (preferably downsampled) linear ProPhoto buffer to derive a
 * PregradeResult without writing any pixels. Optionally uses the rmbg2-subject
 * segmentation model for the `hasSubject` signal when an inference engine is
 * attached and the model is loaded; otherwise falls back to a deterministic
 * centre-weighted saliency metric so the batch is never blocked by a missing
 * model.
 *
 * @param linear Source buffer to analyze (const; never mutated). May be F32,
 *               U16, or U8; internally converted to F32 luminance.
 * @param engine Optional shared inference engine (may be null). Subject
 *               segmentation is only attempted when engine->hasModel(...)
 *               reports the model ready.
 * @return PregradeResult for the input.
 *
 * @note Qt-free; safe to call from the import worker thread. The subject-model
 *       path reuses AiMaskGenerator's existing session/preprocessing.
 */
class PregradeAnalyzer {
public:
    static PregradeResult pregrade(const ImageBuffer& linear,
                                   AiInferenceEngine* engine);
};

} // namespace lrc
