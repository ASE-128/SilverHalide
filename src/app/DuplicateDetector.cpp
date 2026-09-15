// PHASE_14: DuplicateDetector implementation.

#include "app/DuplicateDetector.h"
#include "utils/MathUtils.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lrc {

DuplicateDetector::DuplicateDetector(AiInferenceEngine* engine) : engine_(engine) {}
DuplicateDetector::~DuplicateDetector() = default;

// ---------------------------------------------------------------------------
// Stage 1: pHash (DCT-based, 8×8 → 64 bits).  Adapted from the standard
// "block-based DCT average" pHash algorithm widely used in open-source.
// ---------------------------------------------------------------------------

static float grayscaleAt(const ImageBuffer& img, int x, int y, int w, int h) {
    // Reads whatever format the buffer has and returns Rec.709 luminance [0,1].
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    const size_t i = (static_cast<size_t>(y) * w + x) * 3;
    float r, g, b;
    if (img.format() == PixelFormat::F32) {
        const float* s = reinterpret_cast<const float*>(img.data().data());
        r = s[i]; g = s[i+1]; b = s[i+2];
    } else if (img.format() == PixelFormat::U16) {
        const uint16_t* s = reinterpret_cast<const uint16_t*>(img.data().data());
        const float inv = 1.0f / 65535.0f;
        r = s[i] * inv; g = s[i+1] * inv; b = s[i+2] * inv;
    } else {
        const uint8_t* s = img.data().data();
        const float inv = 1.0f / 255.0f;
        r = s[i] * inv; g = s[i+1] * inv; b = s[i+2] * inv;
    }
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

uint64_t DuplicateDetector::computePHash(const ImageBuffer& img) {
    // Step 1: downscale to 32×32 greyscale (pHash canonical size before DCT)
    constexpr int N = 32;
    float buf[N * N];
    const int w = std::max(1, img.width()), h = std::max(1, img.height());
    for (int y = 0; y < N; ++y) {
        const float syF = (static_cast<float>(y) + 0.5f) * h / N;
        const int sy = std::min(h - 1, static_cast<int>(syF));
        for (int x = 0; x < N; ++x) {
            const float sxF = (static_cast<float>(x) + 0.5f) * w / N;
            const int sx = std::min(w - 1, static_cast<int>(sxF));
            buf[y * N + x] = grayscaleAt(img, sx, sy, w, h);
        }
    }
    // Step 2: 2-D Type-II DCT on 32×32.  Using the naive O(N^4) formula is
    // fine here because 32^4 is only ~1M operations / image; pHash is only
    // computed once per import.  For true batch scale this should be the
    // row-column separation formula (2N 1-D DCTs).
    constexpr int O = 8; // output coefficients we keep
    float dct[O * O];
    for (int u = 0; u < O; ++u) {
        for (int v = 0; v < O; ++v) {
            const float cu = (u == 0) ? 1.0f / std::sqrt(2.0f) : 1.0f;
            const float cv = (v == 0) ? 1.0f / std::sqrt(2.0f) : 1.0f;
            float s = 0.0f;
            for (int y = 0; y < N; ++y) {
                const float cosy = std::cos(static_cast<float>(2*y+1) * u * 3.14159265358979323f / (2 * N));
                for (int x = 0; x < N; ++x) {
                    const float cosx = std::cos(static_cast<float>(2*x+1) * v * 3.14159265358979323f / (2 * N));
                    s += buf[y * N + x] * cosy * cosx;
                }
            }
            dct[u * O + v] = (0.25f) * cu * cv * s;
        }
    }
    // Step 3: median of the 64 coefficients (skipping DC = dct[0])
    std::array<float, O*O - 1> ac{};
    for (int i = 1; i < O*O; ++i) ac[i - 1] = dct[i];
    std::sort(ac.begin(), ac.end());
    const float median = (ac.size() % 2 == 1)
        ? ac[ac.size() / 2]
        : 0.5f * (ac[ac.size()/2 - 1] + ac[ac.size()/2]);
    // Step 4: each bit = coefficient > median.  MSB first = top-left coeff.
    uint64_t hash = 0;
    for (int i = 0; i < O*O; ++i) {
        hash <<= 1;
        if (dct[i] > median) hash |= 1ULL;
    }
    return hash;
}

int DuplicateDetector::hammingDistance(uint64_t a, uint64_t b) {
    return std::bitset<64>(a ^ b).count();
}

// ---------------------------------------------------------------------------
// Stage 2: Embedding + cosine similarity
// ---------------------------------------------------------------------------
std::vector<float> DuplicateDetector::colorHistogramEmbedding(const ImageBuffer& img) {
    // 8 (hue) × 4 (sat) × 4 (val) = 128 bins.
    constexpr int HB = 8, SB = 4, VB = 4;
    std::vector<float> feat(static_cast<size_t>(HB) * SB * VB, 0.0f);
    const int w = img.width(), h = img.height();
    if (w <= 0 || h <= 0) return feat;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float r, g, b;
            const size_t i = (static_cast<size_t>(y) * w + x) * 3;
            if (img.format() == PixelFormat::F32) {
                const float* s = reinterpret_cast<const float*>(img.data().data());
                r = std::clamp(s[i], 0.0f, 1.0f);
                g = std::clamp(s[i+1], 0.0f, 1.0f);
                b = std::clamp(s[i+2], 0.0f, 1.0f);
            } else if (img.format() == PixelFormat::U16) {
                const uint16_t* s = reinterpret_cast<const uint16_t*>(img.data().data());
                const float inv = 1.0f / 65535.0f;
                r = s[i] * inv; g = s[i+1] * inv; b = s[i+2] * inv;
            } else {
                const uint8_t* s = img.data().data();
                const float inv = 1.0f / 255.0f;
                r = s[i] * inv; g = s[i+1] * inv; b = s[i+2] * inv;
            }
            const float mx = std::max({r, g, b});
            const float mn = std::min({r, g, b});
            const float d = mx - mn;
            float hh = 0.0f, ss = 0.0f, vv = mx;
            if (d > 1e-5f) {
                ss = d / std::max(mx, 1e-5f);
                if (mx == r)      hh = (g - b) / d + (g < b ? 6.0f : 0.0f);
                else if (mx == g) hh = (b - r) / d + 2.0f;
                else              hh = (r - g) / d + 4.0f;
                hh /= 6.0f;
            }
            int hi = std::clamp(static_cast<int>(hh * HB), 0, HB - 1);
            int si = std::clamp(static_cast<int>(ss * SB), 0, SB - 1);
            int vi = std::clamp(static_cast<int>(vv * VB), 0, VB - 1);
            ++feat[static_cast<size_t>(hi) * SB * VB + si * VB + vi];
        }
    }
    // L2 normalise
    float sum2 = 0.0f;
    for (auto v : feat) sum2 += v * v;
    if (sum2 > 0.0f) {
        const float inv = 1.0f / std::sqrt(sum2);
        for (auto& v : feat) v *= inv;
    }
    return feat;
}

std::optional<std::vector<float>> DuplicateDetector::embed(const PhotoFrame& frame,
                                                           const std::string& modelId) {
    if (frame.thumbnail.empty()) return std::nullopt;
    if (engine_ && engine_->hasModel(modelId)) {
        ModelShape outShape;
        outShape.layout = "NCHW";
        const auto info = engine_->modelInfo(modelId);
        if (info && !info->outputShape.dims.empty()) outShape.dims = info->outputShape.dims;
        else outShape.dims = {1, 1280};
        auto out = engine_->runImage(modelId, frame.thumbnail, outShape);
        if (out && !out->empty()) {
            // runImage returns an image buffer; flatten its pixels.
            const auto& d = out->data();
            std::vector<float> v;
            const size_t floats = d.size() / sizeof(float);
            v.resize(floats);
            std::memcpy(v.data(), d.data(), floats * sizeof(float));
            // L2 normalise
            float s2 = 0.0f;
            for (auto x : v) s2 += x * x;
            if (s2 > 0.0f) {
                const float inv = 1.0f / std::sqrt(s2);
                for (auto& x : v) x *= inv;
            }
            return v;
        }
    }
    // Fallback: 128-dim HSV histogram.
    return colorHistogramEmbedding(frame.thumbnail);
}

float DuplicateDetector::cosineSimilarity(const std::vector<float>& a,
                                          const std::vector<float>& b) {
    if (a.size() != b.size() || a.empty()) return 0.0f;
    float dot = 0.0f, na = 0.0f, nb = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na  += a[i] * a[i];
        nb  += b[i] * b[i];
    }
    if (na <= 0.0f || nb <= 0.0f) return 0.0f;
    const float cs = dot / std::sqrt(na * nb);
    return std::clamp(cs, -1.0f, 1.0f);
}

// ---------------------------------------------------------------------------
// detect() — end-to-end pipeline
// ---------------------------------------------------------------------------
DuplicateResult DuplicateDetector::detect(
    const std::vector<PhotoFrame>& frames,
    int maxHamming,
    float minCosSimilarity,
    std::vector<std::pair<int64_t, uint64_t>>* phashCache,
    std::vector<std::pair<int64_t, std::vector<float>>>* embedCache) {

    DuplicateResult res;
    res.totalPhotos = frames.size();

    // Lookup helpers backed by the caches
    std::unordered_map<int64_t, uint64_t> phashMap;
    if (phashCache) for (const auto& p : *phashCache) phashMap[p.first] = p.second;
    std::unordered_map<int64_t, std::vector<float>> embedMap;
    if (embedCache) for (const auto& p : *embedCache) embedMap[p.first] = p.second;

    // Stage 1: compute missing pHashes, write back into *phashCache.
    std::vector<uint64_t> phashes(frames.size());
    for (size_t i = 0; i < frames.size(); ++i) {
        auto it = phashMap.find(frames[i].photoId);
        if (it != phashMap.end()) {
            phashes[i] = it->second;
        } else {
            phashes[i] = computePHash(frames[i].thumbnail);
            phashMap[frames[i].photoId] = phashes[i];
            if (phashCache) phashCache->emplace_back(frames[i].photoId, phashes[i]);
        }
    }

    // Stage 1 grouping: union-find over pairs with Hamming dist <= threshold
    const size_t N = frames.size();
    std::vector<int> parent(N);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) {
        while (parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; }
        return x;
    };
    auto unite = [&](int a, int b) {
        a = find(a); b = find(b);
        if (a != b) parent[a] = b;
    };
    // O(N^2) pairwise Hamming.  For 10k photos this is 50M popcounts which
    // is still ~<100ms on modern CPUs; for larger catalogs callers should
    // bucket on the top 8 bits first (spec §2 implementation tip 2).
    for (size_t i = 0; i < N; ++i) {
        for (size_t j = i + 1; j < N; ++j) {
            if (hammingDistance(phashes[i], phashes[j]) <= maxHamming) {
                unite(static_cast<int>(i), static_cast<int>(j));
            }
        }
    }
    // Collect pHash candidate groups (≥2 members)
    std::unordered_map<int, std::vector<int>> stage1;
    for (size_t i = 0; i < N; ++i) stage1[find(static_cast<int>(i))].push_back(static_cast<int>(i));

    // Stage 2: for each candidate group, compute embeddings, pairwise cosine,
    // re-group by cosine >= threshold (again union-find so transitivity holds).
    for (const auto& [root, members] : stage1) {
        if (members.size() < 2) continue;
        const size_t M = members.size();
        std::vector<std::vector<float>> embs(M);
        bool allOk = true;
        for (size_t k = 0; k < M; ++k) {
            const int idx = members[k];
            auto eIt = embedMap.find(frames[idx].photoId);
            if (eIt != embedMap.end()) {
                embs[k] = eIt->second;
            } else {
                auto r = embed(frames[idx]);
                if (!r) { allOk = false; break; }
                embs[k] = *r;
                ++res.embeddedCount;
                embedMap[frames[idx].photoId] = embs[k];
                if (embedCache) embedCache->emplace_back(frames[idx].photoId, embs[k]);
            }
        }
        if (!allOk) continue;
        // Union-find for confirmed duplicates within this candidate group
        std::vector<int> p2(M);
        std::iota(p2.begin(), p2.end(), 0);
        std::function<int(int)> find2 = [&](int x) {
            while (p2[x] != x) { p2[x] = p2[p2[x]]; x = p2[x]; }
            return x;
        };
        auto u2 = [&](int a, int b) {
            a = find2(a); b = find2(b); if (a != b) p2[a] = b;
        };
        std::vector<std::tuple<size_t, size_t, float>> pairSims;
        for (size_t a = 0; a < M; ++a) {
            for (size_t b = a + 1; b < M; ++b) {
                const float s = cosineSimilarity(embs[a], embs[b]);
                pairSims.emplace_back(a, b, s);
                if (s >= minCosSimilarity) u2(static_cast<int>(a), static_cast<int>(b));
            }
        }
        std::unordered_map<int, std::vector<int>> sub;
        for (size_t k = 0; k < M; ++k) sub[find2(static_cast<int>(k))].push_back(static_cast<int>(k));
        for (const auto& [_, subMembers] : sub) {
            if (subMembers.size() < 2) continue;
            DuplicateGroup g;
            float simSum = 0.0f; int cnt = 0;
            for (size_t a = 0; a < subMembers.size(); ++a) {
                for (size_t b = a + 1; b < subMembers.size(); ++b) {
                    const float s = cosineSimilarity(embs[subMembers[a]], embs[subMembers[b]]);
                    simSum += s; ++cnt;
                }
                g.memberIds.push_back(frames[members[subMembers[a]]].photoId);
            }
            g.meanSimilarity = cnt > 0 ? simSum / static_cast<float>(cnt) : 0.0f;
            res.groups.push_back(std::move(g));
            ++res.groupsCounted;
        }
    }
    return res;
}

} // namespace lrc
