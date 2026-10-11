// crypto.h —— 保密转存加解密（严格等价 Python neiwang.py 的同名函数）
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nw {

// SHA-512 计数器模式 XOR；返回小写十六进制
std::string secret_encrypt(const std::string& utf8_text, const std::string& key);

// 逆运算；hex_blob 非法或结果不是合法 UTF-8 时 *ok=false（等价 Python 抛 ValueError）
std::string secret_decrypt(const std::string& hex_blob, const std::string& key, bool* ok = nullptr);

// HMAC-SHA256，消息 = 每个 part 的 UTF-8 字节 + 0x00
std::string secret_sign(const std::string& key, const std::vector<std::string>& parts);

// 加密安全随机
std::string random_bytes(size_t n);
// 大写十六进制，等价 Python secrets.token_hex(n).upper()
std::string random_hex_upper(size_t nbytes);

} // namespace nw
