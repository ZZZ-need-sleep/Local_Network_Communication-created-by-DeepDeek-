// crypto.cpp —— 保密转存加解密 / 随机数
#include "crypto.h"

#include <cstring>

#include <windows.h>
#include <bcrypt.h>

#include "sha2.h"

namespace nw {

// 严格 UTF-8 校验（等价 Python bytes.decode("utf-8") 是否会失败）
static bool utf8_valid(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra;
        unsigned int cp;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + extra >= n) return false;
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // 拒绝过长编码 / 代理区 / 越界码点
        static const unsigned int kMin[4] = {0, 0x80, 0x800, 0x10000};
        if (cp < kMin[extra]) return false;
        if (cp > 0x10FFFF) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        i += extra + 1;
    }
    return true;
}

static void xor_stream(const std::string& data, const std::string& key, std::string& out) {
    out.clear();
    out.reserve(data.size());
    size_t i = 0;
    uint64_t counter = 0;
    unsigned char ctr_be[8];
    Sha512Ctx ctx;
    while (i < data.size()) {
        for (int k = 0; k < 8; ++k) ctr_be[k] = static_cast<unsigned char>(counter >> (56 - 8 * k));
        ctx.init();
        ctx.update(key.data(), key.size());
        ctx.update(ctr_be, 8);
        std::string block = ctx.raw();
        for (size_t b = 0; b < block.size(); ++b) {
            if (i >= data.size()) break;
            out.push_back(static_cast<char>(static_cast<unsigned char>(data[i]) ^
                                            static_cast<unsigned char>(block[b])));
            ++i;
        }
        ++counter;
    }
}

std::string secret_encrypt(const std::string& utf8_text, const std::string& key) {
    std::string out;
    xor_stream(utf8_text, key, out);
    return hex_encode(out);
}

std::string secret_decrypt(const std::string& hex_blob, const std::string& key, bool* ok) {
    // Python bytes.fromhex 允许空白字符
    std::string cleaned;
    cleaned.reserve(hex_blob.size());
    for (char c : hex_blob) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') continue;
        cleaned.push_back(c);
    }
    std::string data;
    if (!hex_decode(cleaned, data)) {
        if (ok) *ok = false;
        return std::string();
    }
    std::string out;
    xor_stream(data, key, out);
    if (!utf8_valid(out)) {
        if (ok) *ok = false;
        return std::string();
    }
    if (ok) *ok = true;
    return out;
}

std::string secret_sign(const std::string& key, const std::vector<std::string>& parts) {
    return hmac_sha256_hex(key, parts);
}

std::string random_bytes(size_t n) {
    std::string out;
    out.resize(n);
    if (n == 0) return out;
    NTSTATUS st = ::BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&out[0]),
                                    static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st < 0) {
        // 兜底：不应该发生
        std::memset(&out[0], 0, n);
    }
    return out;
}

std::string random_hex_upper(size_t nbytes) {
    std::string b = random_bytes(nbytes);
    std::string h = hex_encode(b);
    for (char& c : h) {
        if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
    }
    return h;
}

} // namespace nw
