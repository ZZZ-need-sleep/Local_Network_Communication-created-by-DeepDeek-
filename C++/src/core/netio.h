// netio.h —— Winsock 封装与帧协议（与 Python 版一致：4 字节大端长度 + UTF-8 JSON）
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>

#include "../common/json.h"

namespace nw {

bool net_init();        // WSAStartup（幂等）
void net_shutdown();

// 帧
std::string encode_frame(const std::string& payload);
bool send_all(SOCKET s, const char* data, size_t len);
bool recv_exact(SOCKET s, char* buf, size_t n);
bool send_json(SOCKET s, const Json& obj);
bool recv_json(SOCKET s, Json& out, int max_bytes = 64 * 1024 * 1024);

// 地址
std::string local_ipv4();                       // 第一个非 127.0.0.1 的 IPv4，失败返回 "127.0.0.1"
std::vector<std::string> local_ipv4_list();     // 本机全部 IPv4（不含 127.0.0.1）；空则 {"127.0.0.1"}
std::vector<std::string> broadcast_targets();   // 255.255.255.255 + 各网段 x.x.x.255

// socket
SOCKET make_udp_socket(int want_port, int& bound_port);
SOCKET make_tcp_server(int want_port, int& bound_port);   // 依次试 want_port..want_port+20
SOCKET tcp_connect(const std::string& ip, int port, int timeout_ms);
void   close_socket(SOCKET s);

} // namespace nw
