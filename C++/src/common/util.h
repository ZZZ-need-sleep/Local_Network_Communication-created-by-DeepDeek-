// util.h —— Windows 平台工具（UTF-8 字符串 / 文件 / 时间 / 本机信息）
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nw {

// ------------------------------------------------------------ 字符串编码 --
std::string ws2u8(const std::wstring&);
std::wstring u82ws(const std::string&);
std::string to_lower_ascii(std::string);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);
std::string join(const std::vector<std::string>& v, const std::string& sep);

// ---------------------------------------------------------------- 时间 --
double now_ts();                 // 秒，含小数（等价 Python time.time()）
std::string fmt_time(double ts); // 本地时间 "%m-%d %H:%M:%S"

// ------------------------------------------------------------ 展示格式 --
std::string human_size(double n); // B/KB/MB/GB/TB
std::string sanitize_name(const std::string& s, size_t maxlen = 120);
std::string fmt_mac(const std::string& uid);

// ---------------------------------------------------------------- 路径 --
std::string get_exe_dir(); // 当前 exe 所在目录（UTF-8，结尾无分隔符）
std::string path_join(const std::string& a, const std::string& b);
std::string basename_utf8(const std::string&);

bool ensure_dir(const std::string& path);
bool file_exists(const std::string& utf8_path);
bool is_dir(const std::string& utf8_path);
long long file_size(const std::string& utf8_path);
bool read_file_utf8(const std::string& utf8_path, std::string& out);
bool write_file_utf8(const std::string& utf8_path, const std::string& data);
bool remove_file(const std::string& utf8_path);
bool copy_file(const std::string& from, const std::string& to, bool overwrite = true);
bool remove_dir_recursive(const std::string& utf8_path);
std::vector<std::string> list_dir(const std::string& utf8_path);

// ------------------------------------------------------------ 本机信息 --
std::string hostname_utf8();                    // 等价 Python socket.gethostname()
std::string machine_uid();                      // MAC（12 位大写 hex）或 "RND"+12 位
std::vector<std::string> local_ipv4_list();     // 不含 127.0.0.1；空则 {"127.0.0.1"}

} // namespace nw
