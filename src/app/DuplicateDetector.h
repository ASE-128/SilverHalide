#pragma once

/**
 * @brief Two-stage duplicate detection across a photo set.
 *
 * Stage 1 — pHash (perceptual hash, 64 bit) of a 320 px thumbnail.
 *           Hamming distance < 10 → candidate duplicates. Pure C++,
 *           no model dependency.
 * Stage 2 — deep-feature cosine similarity for each candidate group,
 *           using a MobileNetV3-style embedding via AiInferenceEngine.
 *           Cosine similarity > 0.92 → confirmed duplicate. Falls back
 *           to a simple colour-histogram intersection when the embed
 *           model is unavailable so grouping still works.
 *
 * Thread-safe call pattern: callers build a `PhotoFrame` list (thumbnail
 * buffer + id) on their worker thread, then `detect()` returns a vector of
 * groups. The database persistence of phash / embedding happens in the
 * `CatalogDatabase` class — this class intentionally knows nothing of SQLite.
 *
 * Pure C++. Lives in src/app/.
 */

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/AiInference.h"
#include "engine/ImageBuffer.h"

namespace lrc {

/** @brief A single photo candidate for duplicate detection. */
struct PhotoFrame {
    int64_t photoId = 0;
    ImageBuffer thumbnail; // any format / size >= 32 px on a side
};

/** @brief A confirmed set of duplicate photos (>= 2 members). */
struct DuplicateGroup {
    std::vector<int64_t> memberIds; // sorted by input order
    float meanSimilarity = 0.0f;    // average pairwise similarity (0..1)
};

/** Result of the two-stage detector. */
struct DuplicateResult {
    std::vector<DuplicateGroup> groups;
    // Stats (for the "scan summary" dialog line)
    size_t totalPhotos = 0;
    size_t groupsCounted = 0;
    size_t embeddedCount = 0;  // # of embeddings actually computed
};

class DuplicateDetector {
public:
    explicit DuplicateDetector(AiInferenceEngine* engine);
    ~DuplicateDetector();

    // --- Stage 1: pHash (pure C++, always on) -----------------------------
    /** Computes a 64-bit perceptual hash for any RGB buffer. */
    static uint64_t computePHash(const ImageBuffer& img);

    /** Hamming distance between two 64-bit hashes (popcount of xor). */
    static int hammingDistance(uint64_t a, uint64_t b);

    // --- Stage 2: deep embedding ------------------------------------------
    /**
     * @brief Extract the feature embedding vector for a single photo.
     *        If the embed model ("mobilenetv3-embed") is loaded returns a
     *        real 1280-dim CNN embedding.  Otherwise returns a 128-dim
     *        HSV-colour-histogram feature so the similarity path is still
     *        meaningful.  An empty optional on fatal error.
     */
    std::optional<std::vector<float>> embed(const PhotoFrame& frame,
                                            const std::string& modelId = "mobilenetv3-embed");

    /** Cosine similarity of two vectors (returns 0 on size mismatch). */
    static float cosineSimilarity(const std::vector<float>& a,
                                  const std::vector<float>& b);

    // --- High-level detection ---------------------------------------------
    /**
     * @brief End-to-end duplicate detection.  Takes a list of frames, runs
     *        pHash → candidate grouping → embedding → confirm.  Returns a
     *        vector of DuplicateGroup (each group has ≥2 members).
     *
     * @param frames            The photo set to scan.
     * @param maxHamming        Stage-1 Hamming threshold (default 10).
     * @param minCosSimilarity  Stage-2 cosine threshold (default 0.92).
     * @param phashCache        Optional.  If provided, maps photoId → a
     *                          pre-computed pHash; missing ids are computed
     *                          and written back out so callers can persist.
     * @param embedCache        Optional.  Maps photoId → pre-computed
     *                          embedding; missing ids are computed and
     *                          written back out (the database persistence
     *                          path).
     */
    DuplicateResult detect(
        const std::vector<PhotoFrame>& frames,
        int maxHamming = 10,
        float minCosSimilarity = 0.92f,
        std::vector<std::pair<int64_t, uint64_t>>* phashCache = nullptr,
        std::vector<std::pair<int64_t, std::vector<float>>>* embedCache = nullptr);

    /** The 128-bin colour histogram fallback embedding (HxSxV = 8×4×4 = 128).
     *  Exposed for tests. */
    static std::vector<float> colorHistogramEmbedding(const ImageBuffer& img);

private:
    AiInferenceEngine* engine_;
};

} // namespace lrc
