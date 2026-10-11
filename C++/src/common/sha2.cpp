// sha2.cpp —— SHA-256 / SHA-512 / HMAC-SHA256 实现
#include "sha2.h"

#include <cstdio>
#include <cstring>

#include <windows.h>

namespace nw {

// ============================================================ hex 编解码 ==
static const char* kHexLower = "0123456789abcdef";

std::string hex_encode(const void* data, size_t len) {
    static const char* H = kHexLower;
    std::string out;
    out.resize(len * 2);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = H[p[i] >> 4];
        out[i * 2 + 1] = H[p[i] & 0x0F];
    }
    return out;
}

std::string hex_encode(const std::string& s) { return hex_encode(s.data(), s.size()); }

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) return false;
    std::string tmp;
    tmp.resize(hex.size() / 2);
    for (size_t i = 0; i < tmp.size(); ++i) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        tmp[i] = static_cast<char>((hi << 4) | lo);
    }
    out.swap(tmp);
    return true;
}

// ================================================================= SHA-256 ==
static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static const uint32_t K256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

static inline uint32_t load_be32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

static inline void store_be32(unsigned char* p, uint32_t v) {
    p[0] = static_cast<unsigned char>(v >> 24);
    p[1] = static_cast<unsigned char>(v >> 16);
    p[2] = static_cast<unsigned char>(v >> 8);
    p[3] = static_cast<unsigned char>(v);
}

static inline void store_be64(unsigned char* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(v >> (56 - 8 * i));
}

void Sha256Ctx::init() {
    static const uint32_t IV[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::memcpy(h_, IV, sizeof(IV));
    len_ = 0;
    buf_len_ = 0;
    done_ = false;
    digest_.clear();
}

void Sha256Ctx::transform(const unsigned char* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = load_be32(block + i * 4);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256Ctx::update(const void* data, size_t len) {
    if (done_ || len == 0) return;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    len_ += len;
    if (buf_len_ > 0) {
        size_t need = 64 - buf_len_;
        size_t take = len < need ? len : need;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ == 64) {
            transform(buf_);
            buf_len_ = 0;
        }
    }
    while (len >= 64) {
        transform(p);
        p += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(buf_, p, len);
        buf_len_ = len;
    }
}

std::string Sha256Ctx::raw() {
    if (!done_) {
        uint64_t bitlen = len_ * 8;
        unsigned char pad[128];
        size_t padlen;
        pad[0] = 0x80;
        size_t rem = static_cast<size_t>(len_ % 64);
        size_t zeros = (rem < 56) ? (56 - rem - 1) : (120 - rem - 1);
        std::memset(pad + 1, 0, zeros);
        padlen = 1 + zeros;
        store_be64(pad + padlen, bitlen);
        padlen += 8;
        update(pad, padlen);
        unsigned char out[32];
        for (int i = 0; i < 8; ++i) store_be32(out + i * 4, h_[i]);
        digest_.assign(reinterpret_cast<char*>(out), 32);
        done_ = true;
    }
    return digest_;
}

std::string Sha256Ctx::hex() { return hex_encode(raw()); }

std::string sha256_raw(const void* data, size_t len) {
    Sha256Ctx c;
    c.init();
    c.update(data, len);
    return c.raw();
}

std::string sha256_hex(const void* data, size_t len) { return hex_encode(sha256_raw(data, len)); }
std::string sha256_hex(const std::string& s) { return sha256_hex(s.data(), s.size()); }

// ================================================================= SHA-512 ==
static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full, 0xe9b5dba58189dbbcull,
    0x3956c25bf348b538ull, 0x59f111f1b605d019ull, 0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull,
    0xd807aa98a3030242ull, 0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull, 0xc19bf174cf692694ull,
    0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull, 0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull,
    0x2de92c6f592b0275ull, 0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full, 0xbf597fc7beef0ee4ull,
    0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull, 0x06ca6351e003826full, 0x142929670a0e6e70ull,
    0x27b70a8546d22ffcull, 0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull, 0x92722c851482353bull,
    0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull, 0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull,
    0xd192e819d6ef5218ull, 0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull, 0x34b0bcb5e19b48a8ull,
    0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull, 0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull,
    0x748f82ee5defb2fcull, 0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull, 0xc67178f2e372532bull,
    0xca273eceea26619cull, 0xd186b8c721c0c207ull, 0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull,
    0x06f067aa72176fbaull, 0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull, 0x431d67c49c100d4cull,
    0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull, 0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull};

static inline uint64_t load_be64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | static_cast<uint64_t>(p[i]);
    return v;
}

void Sha512Ctx::init() {
    static const uint64_t IV[8] = {
        0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull, 0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
        0x510e527fade682d1ull, 0x9b05688c2b3e6c1full, 0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull};
    std::memcpy(h_, IV, sizeof(IV));
    len_ = 0;
    buf_len_ = 0;
    done_ = false;
    digest_.clear();
}

void Sha512Ctx::transform(const unsigned char* block) {
    uint64_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = load_be64(block + i * 8);
    for (int i = 16; i < 80; ++i) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint64_t e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 80; ++i) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha512Ctx::update(const void* data, size_t len) {
    if (done_ || len == 0) return;
    const unsigned char* p = static_cast<const unsigned char*>(data);
    len_ += len;
    if (buf_len_ > 0) {
        size_t need = 128 - buf_len_;
        size_t take = len < need ? len : need;
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ == 128) {
            transform(buf_);
            buf_len_ = 0;
        }
    }
    while (len >= 128) {
        transform(p);
        p += 128;
        len -= 128;
    }
    if (len > 0) {
        std::memcpy(buf_, p, len);
        buf_len_ = len;
    }
}

std::string Sha512Ctx::raw() {
    if (!done_) {
        uint64_t bitlen = len_ * 8;
        unsigned char pad[256];
        size_t rem = static_cast<size_t>(len_ % 128);
        size_t zeros = (rem < 112) ? (112 - rem - 1) : (240 - rem - 1);
        pad[0] = 0x80;
        std::memset(pad + 1, 0, zeros);
        size_t padlen = 1 + zeros;
        store_be64(pad + padlen, len_ >> 61); // 128 位长度的高 64 位
        store_be64(pad + padlen + 8, bitlen);
        padlen += 16;
        update(pad, padlen);
        unsigned char out[64];
        for (int i = 0; i < 8; ++i) store_be64(out + i * 8, h_[i]);
        digest_.assign(reinterpret_cast<char*>(out), 64);
        done_ = true;
    }
    return digest_;
}

std::string Sha512Ctx::hex() { return hex_encode(raw()); }

std::string sha512_raw(const void* data, size_t len) {
    Sha512Ctx c;
    c.init();
    c.update(data, len);
    return c.raw();
}

std::string sha512_hex(const void* data, size_t len) { return hex_encode(sha512_raw(data, len)); }
std::string sha512_hex(const std::string& s) { return sha512_hex(s.data(), s.size()); }

// ================================================================ HMAC-SHA256
std::string hmac_sha256_hex(const std::string& key, const std::vector<std::string>& parts) {
    unsigned char k[64];
    std::memset(k, 0, sizeof(k));
    if (key.size() > 64) {
        std::string kh = sha256_raw(key.data(), key.size());
        std::memcpy(k, kh.data(), kh.size());
    } else {
        std::memcpy(k, key.data(), key.size());
    }
    unsigned char ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<unsigned char>(k[i] ^ 0x36);
        opad[i] = static_cast<unsigned char>(k[i] ^ 0x5c);
    }
    Sha256Ctx inner;
    inner.init();
    inner.update(ipad, 64);
    for (const std::string& p : parts) {
        inner.update(p.data(), p.size());
        const char zero = '\0';
        inner.update(&zero, 1);
    }
    std::string ih = inner.raw();
    Sha256Ctx outer;
    outer.init();
    outer.update(opad, 64);
    outer.update(ih.data(), ih.size());
    return hex_encode(outer.raw());
}

// ============================================================== 文件摘要 ==
bool sha256_file_hex(const std::wstring& path, std::string& out,
                     std::function<void(double)> progress) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz;
    unsigned long long total = 0;
    if (::GetFileSizeEx(h, &sz)) total = static_cast<unsigned long long>(sz.QuadPart);

    Sha256Ctx ctx;
    ctx.init();
    const DWORD kChunk = 1u << 20;
    std::vector<unsigned char> buf(kChunk);
    unsigned long long done = 0;
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(h, buf.data(), kChunk, &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        ctx.update(buf.data(), got);
        done += got;
        if (progress && total > 0) progress(static_cast<double>(done) / static_cast<double>(total));
    }
    ::CloseHandle(h);
    if (!ok) return false;
    out = ctx.hex();
    return true;
}

} // namespace nw
