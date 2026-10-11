// netio.cpp —— Winsock 封装与帧协议
#include "netio.h"
#include "../common/util.h"

#include <cstring>
#include <mutex>

#pragma comment(lib, "ws2_32.lib")

namespace nw {

static std::once_flag g_wsa_once;

static void wsa_startup_once() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

bool net_init() {
    std::call_once(g_wsa_once, wsa_startup_once);
    return true;
}

void net_shutdown() { WSACleanup(); }

std::string encode_frame(const std::string& payload) {
    uint32_t n = (uint32_t)payload.size();
    std::string out;
    out.resize(4);
    out[0] = (char)((n >> 24) & 0xFF);
    out[1] = (char)((n >> 16) & 0xFF);
    out[2] = (char)((n >> 8) & 0xFF);
    out[3] = (char)(n & 0xFF);
    out += payload;
    return out;
}

bool send_all(SOCKET s, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        int n = send(s, data + off, (int)(len - off), 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

bool recv_exact(SOCKET s, char* buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        int r = recv(s, buf + off, (int)(n - off), 0);
        if (r <= 0) return false;
        off += (size_t)r;
    }
    return true;
}

bool send_json(SOCKET s, const Json& obj) {
    std::string payload = obj.dump(false);
    std::string frame = encode_frame(payload);
    return send_all(s, frame.data(), frame.size());
}

bool recv_json(SOCKET s, Json& out, int max_bytes) {
    char hdr[4];
    if (!recv_exact(s, hdr, 4)) return false;
    uint32_t n = ((uint32_t)(unsigned char)hdr[0] << 24) |
                 ((uint32_t)(unsigned char)hdr[1] << 16) |
                 ((uint32_t)(unsigned char)hdr[2] << 8) |
                 ((uint32_t)(unsigned char)hdr[3]);
    if (n > (uint32_t)max_bytes) return false;
    std::string body;
    body.resize(n);
    if (n > 0 && !recv_exact(s, &body[0], n)) return false;
    bool ok = false;
    out = Json::parse(body, &ok);
    return ok;
}

std::string local_ipv4() {
    std::vector<std::string> v = nw::local_ipv4_list();
    // 优先返回真正可路由的地址：跳过 169.254.x（APIPA，通常是失效网卡）与回环。
    // Python 版 get_local_ips()[0] 可能取到 169.254.x，导致对端连不上，
    // 这里做同样的修正（两版行为一致，且更稳）。
    for (auto& ip : v) {
        if (ip == "127.0.0.1") continue;
        if (ip.rfind("169.254.", 0) == 0) continue;
        return ip;
    }
    for (auto& ip : v) if (ip != "127.0.0.1") return ip;
    return v.empty() ? "127.0.0.1" : v[0];
}

std::vector<std::string> broadcast_targets() {
    std::vector<std::string> out;
    out.push_back("255.255.255.255");
    for (auto& ip : nw::local_ipv4_list()) {
        if (ip == "127.0.0.1") continue;
        size_t p = ip.rfind('.');
        if (p != std::string::npos && ip.find('.') != p)
            out.push_back(ip.substr(0, p) + ".255");
    }
    return out;
}

SOCKET make_udp_socket(int want_port, int& bound_port) {
    net_init();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons((u_short)want_port);
    if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
        a.sin_port = 0;
        if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    int alen = sizeof(a);
    getsockname(s, (sockaddr*)&a, &alen);
    bound_port = ntohs(a.sin_port);
    DWORD tv = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    return s;
}

SOCKET make_tcp_server(int want_port, int& bound_port) {
    net_init();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    for (int p = want_port; p <= want_port + 20; ++p) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = INADDR_ANY;
        a.sin_port = htons((u_short)p);
        if (bind(s, (sockaddr*)&a, sizeof(a)) == 0) {
            if (listen(s, 16) == 0) { bound_port = p; return s; }
        }
    }
    closesocket(s);
    return INVALID_SOCKET;
}

SOCKET tcp_connect(const std::string& ip, int port, int timeout_ms) {
    net_init();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    // 非阻塞 connect + select 以实现超时
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) { closesocket(s); return INVALID_SOCKET; }
    int rc = connect(s, (sockaddr*)&a, sizeof(a));
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) { closesocket(s); return INVALID_SOCKET; }
        fd_set wf, ef;
        FD_ZERO(&wf); FD_SET(s, &wf);
        FD_ZERO(&ef); FD_SET(s, &ef);
        timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        int sel = select(0, nullptr, &wf, &ef, &tv);
        if (sel <= 0 || FD_ISSET(s, &ef)) { closesocket(s); return INVALID_SOCKET; }
        int soerr = 0;
        int slen = sizeof(soerr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);
        if (soerr != 0) { closesocket(s); return INVALID_SOCKET; }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD tvv = (DWORD)timeout_ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tvv, sizeof(tvv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tvv, sizeof(tvv));
    return s;
}

void close_socket(SOCKET s) {
    if (s != INVALID_SOCKET) closesocket(s);
}

} // namespace nw
