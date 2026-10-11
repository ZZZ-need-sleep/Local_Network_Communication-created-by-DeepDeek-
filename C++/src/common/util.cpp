// util.cpp —— Windows 平台工具实现
#include "util.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <ipifcons.h>

#include "crypto.h"
#include "sha2.h"

namespace fs = std::filesystem;

namespace nw {

// =============================================================== 编码转换 ==
std::string ws2u8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int need = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                     nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out;
    out.resize(static_cast<size_t>(need));
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &out[0], need, nullptr,
                          nullptr);
    return out;
}

std::wstring u82ws(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out;
    out.resize(static_cast<size_t>(need));
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], need);
    return out;
}

static fs::path to_path(const std::string& utf8_path) { return fs::path(u82ws(utf8_path)); }

// =============================================================== 字符串 ==
std::string to_lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

static bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && is_ascii_space(s[b])) ++b;
    while (e > b && is_ascii_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& v, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

// =================================================================== 时间 ==
double now_ts() {
    using namespace std::chrono;
    auto tp = system_clock::now().time_since_epoch();
    return duration_cast<duration<double>>(tp).count();
}

std::string fmt_time(double ts) {
    std::time_t t = static_cast<std::time_t>(ts);
    std::tm tmv;
    std::memset(&tmv, 0, sizeof(tmv));
    if (::localtime_s(&tmv, &t) != 0) return std::string();
    char buf[64];
    if (std::strftime(buf, sizeof(buf), "%m-%d %H:%M:%S", &tmv) == 0) return std::string();
    return std::string(buf);
}

// =============================================================== 展示格式 ==
std::string human_size(double n) {
    static const char* units[5] = {"B", "KB", "MB", "GB", "TB"};
    for (int i = 0; i < 5; ++i) {
        if (n < 1024.0 || i == 4) {
            char buf[64];
            if (i == 0)
                std::snprintf(buf, sizeof(buf), "%lld B", static_cast<long long>(n));
            else
                std::snprintf(buf, sizeof(buf), "%.1f %s", n, units[i]);
            return std::string(buf);
        }
        n /= 1024.0;
    }
    return std::string("0 B");
}

// 按 UTF-8 码点截断，绝不切出半个字符
static std::string utf8_truncate(const std::string& s, size_t max_chars) {
    size_t count = 0;
    size_t i = 0;
    while (i < s.size() && count < max_chars) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t extra = 0;
        if (c < 0x80) extra = 0;
        else if ((c & 0xE0) == 0xC0) extra = 1;
        else if ((c & 0xF0) == 0xE0) extra = 2;
        else if ((c & 0xF8) == 0xF0) extra = 3;
        else extra = 0; // 非法起点字节：当作单字节推进
        if (i + extra >= s.size()) break; // 尾部残缺，丢弃
        i += extra + 1;
        ++count;
    }
    return s.substr(0, i);
}

std::string sanitize_name(const std::string& s, size_t maxlen) {
    const char* bad = "\\/:*?\"<>|\r\n\t";
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (std::strchr(bad, c) != nullptr && c != '\0')
            out.push_back('_');
        else
            out.push_back(c);
    }
    out = trim(out);
    if (out.empty()) out = "\xe6\x9c\xaa\xe5\x91\xbd\xe5\x90\x8d"; // "未命名"
    return utf8_truncate(out, maxlen);
}

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string fmt_mac(const std::string& uid) {
    std::string u = uid;
    if (u.size() == 12 && std::all_of(u.begin(), u.end(), is_hex_digit)) {
        std::string out;
        for (int i = 0; i < 12; i += 2) {
            if (i) out += '-';
            out += u.substr(static_cast<size_t>(i), 2);
        }
        for (char& c : out)
            if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
        return out;
    }
    if (u.size() == 15 && u.compare(0, 3, "RND") == 0) {
        std::string body = u.substr(3);
        for (char& c : body)
            if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
        std::string out = "RND";
        for (int i = 0; i < 12; i += 4) {
            out += '-';
            out += body.substr(static_cast<size_t>(i), 4);
        }
        return out;
    }
    return u.empty() ? std::string("\xe6\x9c\xaa\xe7\x9f\xa5") : u; // "未知"
}

// =================================================================== 路径 ==
std::string get_exe_dir() {
    std::wstring buf;
    buf.resize(1024);
    for (;;) {
        DWORD n = ::GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n == 0) return std::string();
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
        if (buf.size() > 32768) return std::string();
    }
    size_t pos = buf.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::string();
    std::wstring dir = buf.substr(0, pos);
    if (!dir.empty() && dir.back() == L':') dir.push_back(L'\\'); // 盘根 "C:\"
    return ws2u8(dir);
}

std::string path_join(const std::string& a, const std::string& b) {
    if (b.empty()) return a;
    // 等价 Python os.path.join：b 为绝对路径时直接返回 b
    bool b_abs = (b[0] == '\\' || b[0] == '/') || (b.size() >= 2 && b[1] == ':');
    if (b_abs) return b;
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::string basename_utf8(const std::string& p) {
    size_t pos = p.find_last_of("\\/");
    if (pos == std::string::npos) return p;
    return p.substr(pos + 1);
}

// =================================================================== 文件 ==
bool ensure_dir(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    if (fs::exists(to_path(path), ec)) return fs::is_directory(to_path(path), ec);
    fs::create_directories(to_path(path), ec);
    if (ec) return false;
    return fs::is_directory(to_path(path), ec);
}

bool file_exists(const std::string& utf8_path) {
    std::error_code ec;
    return fs::exists(to_path(utf8_path), ec);
}

bool is_dir(const std::string& utf8_path) {
    std::error_code ec;
    return fs::is_directory(to_path(utf8_path), ec);
}

long long file_size(const std::string& utf8_path) {
    std::error_code ec;
    auto sz = fs::file_size(to_path(utf8_path), ec);
    if (ec) return -1;
    return static_cast<long long>(sz);
}

bool read_file_utf8(const std::string& utf8_path, std::string& out) {
    HANDLE h = ::CreateFileW(u82ws(utf8_path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string data;
    char buf[65536];
    for (;;) {
        DWORD got = 0;
        if (!::ReadFile(h, buf, sizeof(buf), &got, nullptr)) {
            ::CloseHandle(h);
            return false;
        }
        if (got == 0) break;
        data.append(buf, got);
    }
    ::CloseHandle(h);
    out.swap(data);
    return true;
}

bool write_file_utf8(const std::string& utf8_path, const std::string& data) {
    HANDLE h = ::CreateFileW(u82ws(utf8_path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t off = 0;
    bool ok = true;
    while (off < data.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - off, 1u << 20));
        DWORD wrote = 0;
        if (!::WriteFile(h, data.data() + off, chunk, &wrote, nullptr) || wrote == 0) {
            ok = false;
            break;
        }
        off += wrote;
    }
    ::CloseHandle(h);
    return ok;
}

bool remove_file(const std::string& utf8_path) {
    std::error_code ec;
    return fs::remove(to_path(utf8_path), ec) && !ec;
}

bool copy_file(const std::string& from, const std::string& to, bool overwrite) {
    std::error_code ec;
    fs::copy_file(to_path(from), to_path(to),
                  overwrite ? fs::copy_options::overwrite_existing : fs::copy_options::none, ec);
    return !ec;
}

bool remove_dir_recursive(const std::string& utf8_path) {
    std::error_code ec;
    fs::remove_all(to_path(utf8_path), ec);
    return !ec;
}

std::vector<std::string> list_dir(const std::string& utf8_path) {
    std::vector<std::string> out;
    std::error_code ec;
    fs::directory_iterator it(to_path(utf8_path), ec);
    if (ec) return out;
    fs::directory_iterator end;
    while (it != end) {
        out.push_back(ws2u8(it->path().filename().wstring()));
        it.increment(ec);
        if (ec) break;
    }
    return out;
}

// =============================================================== 本机信息 ==
std::string hostname_utf8() {
    std::wstring buf;
    buf.resize(256);
    DWORD n = static_cast<DWORD>(buf.size());
    if (!::GetComputerNameW(&buf[0], &n)) return std::string();
    buf.resize(n);
    return ws2u8(buf);
}

std::string machine_uid() {
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                       reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        ret = ::GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (ret == NO_ERROR) {
        auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        for (; a != nullptr; a = a->Next) {
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (a->PhysicalAddressLength != 6) continue;
            if (a->PhysicalAddress[0] & 0x01) continue; // 多播位 -> 视为随机，放弃
            std::string h = hex_encode(a->PhysicalAddress, 6);
            for (char& c : h)
                if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
            return h;
        }
    }
    return "RND" + random_hex_upper(6);
}

std::vector<std::string> local_ipv4_list() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG ret = ::GetAdaptersAddresses(AF_INET, flags, nullptr,
                                       reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (ret == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        ret = ::GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (ret == NO_ERROR) {
        auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        for (; a != nullptr; a = a->Next) {
            for (auto* ua = a->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
                if (ua->Address.lpSockaddr == nullptr) continue;
                if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                unsigned char* b = reinterpret_cast<unsigned char*>(&sin->sin_addr);
                char tmp[32];
                std::snprintf(tmp, sizeof(tmp), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                std::string ip(tmp);
                if (ip == "127.0.0.1") continue;
                out.push_back(ip);
            }
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    if (out.empty()) out.push_back("127.0.0.1");
    return out;
}

} // namespace nw
