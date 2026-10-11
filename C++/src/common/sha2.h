// sha2.h —— SHA-256 / SHA-512 / HMAC-SHA256 / hex 编解码（零第三方依赖）
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nw {

// ---------------------------------------------------------------- hex 工具 --
std::string hex_encode(const void* data, size_t len);
std::string hex_encode(const std::string& s);
bool hex_decode(const std::string& hex, std::string& out); // 非法字符或奇数长度返回 false

// ------------------------------------------------------------------ 摘要 --
std::string sha256_hex(const void* data, size_t len);
std::string sha512_hex(const void* data, size_t len);
std::string sha256_hex(const std::string& s);
std::string sha512_hex(const std::string& s);

// 额外暴露的原始字节摘要（实现细节，供 crypto.cpp 内部使用）
std::string sha256_raw(const void* data, size_t len);
std::string sha512_raw(const void* data, size_t len);

struct Sha256Ctx {
    void init();
    void update(const void* data, size_t len);
    std::string hex();
    std::string raw(); // 额外：原始 32 字节摘要（会终结上下文）

private:
    void transform(const unsigned char* block);
    uint32_t h_[8];
    uint64_t len_;
    size_t buf_len_;
    unsigned char buf_[64];
    bool done_;
    std::string digest_;
};

struct Sha512Ctx {
    void init();
    void update(const void* data, size_t len);
    std::string hex();
    std::string raw(); // 额外：原始 64 字节摘要（会终结上下文）

private:
    void transform(const unsigned char* block);
    uint64_t h_[8];
    uint64_t len_;
    size_t buf_len_;
    unsigned char buf_[128];
    bool done_;
    std::string digest_;
};

// key 为 HMAC 密钥原始字节；消息 = 每个 part 的 UTF-8 字节 + 一个 0x00
std::string hmac_sha256_hex(const std::string& key, const std::vector<std::string>& parts);

// 大文件摘要；path 为宽字符路径；progress 收到 [0,1] 的比例（文件为空时不回调）
bool sha256_file_hex(const std::wstring& path, std::string& out,
                     std::function<void(double)> progress = {});

} // namespace nw
