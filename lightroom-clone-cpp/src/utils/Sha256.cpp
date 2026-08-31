// Self-contained SHA-256 implementation (FIPS 180-4).
//
// Extracted from src/app/ModelManager.cpp so it can be unit-tested headlessly
// (tests/TestSha256.cpp). The previous in-place copy had a buffer-remainder
// bug that silently produced wrong hashes for any input whose last chunk
// left a non-zero remainder in the 64-byte block buffer — i.e. every real
// model file (>64 bytes). That bug went undetected because the only model
// exercised by the headless suite (test_add_2x2) uses a placeholder SHA that
// is skipped at verify time.

#include "utils/Sha256.h"

#include <array>
#include <cstring>
#include <fstream>

namespace lrc {

namespace sha256_detail {

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buffer[64];
};

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t bsig0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t bsig1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t ssig0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t ssig1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

void sha256_init(Sha256Ctx* c) {
    c->state[0]=0x6a09e667; c->state[1]=0xbb67ae85; c->state[2]=0x3c6ef372; c->state[3]=0xa54ff53a;
    c->state[4]=0x510e527f; c->state[5]=0x9b05688c; c->state[6]=0x1f83d9ab; c->state[7]=0x5be0cd19;
    c->count = 0;
}

void sha256_transform(Sha256Ctx* c, const uint8_t* data) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(data[i*4]) << 24) | (uint32_t(data[i*4+1]) << 16)
             | (uint32_t(data[i*4+2]) << 8)  |  uint32_t(data[i*4+3]);
    for (int i = 16; i < 64; ++i)
        w[i] = ssig1(w[i-2]) + w[i-7] + ssig0(w[i-15]) + w[i-16];
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    uint32_t e = c->state[4], f = c->state[5], g = c->state[6], h = c->state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t t1 = h + bsig1(e) + ch(e,f,g) + K[i] + w[i];
        uint32_t t2 = bsig0(a) + maj(a,b,cc);
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->state[0] += a; c->state[1] += b; c->state[2] += cc; c->state[3] += d;
    c->state[4] += e; c->state[5] += f; c->state[6] += g; c->state[7] += h;
}

void sha256_update(Sha256Ctx* c, const uint8_t* data, size_t len) {
    const size_t idx = static_cast<size_t>(c->count % 64);
    c->count += len;
    if (idx && idx + len >= 64) {
        const size_t fill = 64 - idx;
        std::memcpy(c->buffer + idx, data, fill);
        sha256_transform(c, c->buffer);
        data += fill;
        len  -= fill;
    }
    while (len >= 64) {
        sha256_transform(c, data);
        data += 64;
        len  -= 64;
    }
    // Copy the remainder (< 64 bytes) into the buffer at the position
    // corresponding to the byte offset (count - len) — i.e. the buffer fill
    // position BEFORE this update's bytes were counted. This is the same as
    // `idx` when no full-block transform flushed the buffer this call, and 0
    // when a transform did flush it (buffer logically empty afterwards).
    if (len) {
        const size_t i = static_cast<size_t>((c->count - len) % 64);
        std::memcpy(c->buffer + i, data, len);
    }
}

void sha256_final(Sha256Ctx* c, uint8_t out[32]) {
    const uint64_t bits = c->count * 8;
    size_t idx = static_cast<size_t>(c->count % 64);
    c->buffer[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) c->buffer[idx++] = 0;
        sha256_transform(c, c->buffer);
        idx = 0;
    }
    while (idx < 56) c->buffer[idx++] = 0;
    for (int i = 7; i >= 0; --i) c->buffer[idx++] = static_cast<uint8_t>(bits >> (i * 8));
    sha256_transform(c, c->buffer);
    for (int i = 0; i < 8; ++i) {
        out[i*4    ] = static_cast<uint8_t>(c->state[i] >> 24);
        out[i*4 + 1] = static_cast<uint8_t>(c->state[i] >> 16);
        out[i*4 + 2] = static_cast<uint8_t>(c->state[i] >> 8);
        out[i*4 + 3] = static_cast<uint8_t>(c->state[i]);
    }
}

std::string to_hex(const uint8_t (&digest)[32]) {
    static const char hex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 32; ++i) {
        out[2*i    ] = hex[digest[i] >> 4];
        out[2*i + 1] = hex[digest[i] & 0x0F];
    }
    return out;
}

}  // namespace sha256_detail

std::string sha256Buffer(const uint8_t* data, size_t len) {
    sha256_detail::Sha256Ctx ctx;
    sha256_detail::sha256_init(&ctx);
    if (data && len) sha256_detail::sha256_update(&ctx, data, len);
    uint8_t digest[32];
    sha256_detail::sha256_final(&ctx, digest);
    return sha256_detail::to_hex(digest);
}

std::string sha256File(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    sha256_detail::Sha256Ctx ctx;
    sha256_detail::sha256_init(&ctx);
    std::array<char, 16384> buf{};
    while (f) {
        f.read(buf.data(), buf.size());
        const auto n = static_cast<size_t>(f.gcount());
        if (n == 0) break;
        sha256_detail::sha256_update(&ctx,
            reinterpret_cast<const uint8_t*>(buf.data()), n);
    }
    uint8_t digest[32];
    sha256_detail::sha256_final(&ctx, digest);
    return sha256_detail::to_hex(digest);
}

}  // namespace lrc
