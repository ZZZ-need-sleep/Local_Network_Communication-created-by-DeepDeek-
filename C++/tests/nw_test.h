// nw_test.h —— C++ 测试公共断言与工具（零依赖）
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../src/common/json.h"
#include "../src/common/util.h"
#include "../src/core/core.h"
#include "../src/core/netio.h"

namespace nwt {

inline int& passed() { static int n = 0; return n; }
inline int& failed() { static int n = 0; return n; }
inline std::vector<std::string>& failures() { static std::vector<std::string> v; return v; }

inline void section(const char* name) { std::printf("-- %s --\n", name); }

inline void check(bool ok, const std::string& what) {
    if (ok) { passed()++; }
    else {
        failed()++;
        failures().push_back(what);
        std::printf("   [FAIL] %s\n", what.c_str());
    }
}

template <typename A, typename B>
inline void check_eq(const A& a, const B& b, const std::string& what) {
    bool ok = (a == b);
    if (ok) passed()++;
    else {
        failed()++;
        std::string m = what + "  (left=" + nw::Json(a).dump() + " right=" + nw::Json(b).dump() + ")";
        failures().push_back(m);
        std::printf("   [FAIL] %s\n", m.c_str());
    }
}

inline int summary(const char* title) {
    std::printf("=== %s: %d passed, %d failed ===\n", title, passed(), failed());
    for (auto& f : failures()) std::printf("  - %s\n", f.c_str());
    return failed() == 0 ? 0 : 1;
}

// 临时目录（进程内唯一）
inline std::string temp_dir(const std::string& tag) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lu", (unsigned long)(nw::now_ts() * 1000) % 100000000UL);
    std::string base = nw::get_exe_dir() + "\\_nwtest";
    nw::ensure_dir(base);
    std::string d = base + "\\" + tag + "_" + buf;
    nw::ensure_dir(d);
    return d;
}

inline void set_env(const char* k, const char* v) {
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 1);
#endif
}

// 构造一个端口可指定的 cfg
inline nw::Json make_cfg(int udp_port, int tcp_port, const std::string& name,
                         const std::string& uid, const nw::Json& groups = nw::Json::arr()) {
    nw::Json c = nw::Json::obj();
    c.set("name", nw::Json(name));
    c.set("uid", nw::Json(uid));
    c.set("tcp_port", nw::Json((long long)tcp_port));
    c.set("udp_port", nw::Json((long long)udp_port));
    c.set("groups", groups);
    return c;
}

} // namespace nwt
