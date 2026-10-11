// test_common.cpp —— 基础层测试（JSON / SHA2 / crypto / util）
// 返回 0 表示全部通过，非 0 表示存在失败。
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "crypto.h"
#include "json.h"
#include "sha2.h"
#include "util.h"

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const std::string& name) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("[FAIL] %s\n", name.c_str());
    }
}

static void check_eq(const std::string& got, const std::string& want, const std::string& name) {
    if (got == want) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("[FAIL] %s\n  got : %s\n  want: %s\n", name.c_str(), got.c_str(),
                    want.c_str());
    }
}

static const char* U8_HELLO = "\xe4\xbd\xa0\xe5\xa5\xbd\xef\xbc\x8c\xe4\xb8\x96\xe7\x95\x8c";
static const char* U8_ZHONG = "\xe4\xb8\xad";
static const char* U8_EMOJI = "\xf0\x9f\x98\x80";

// =========================================================== 1. SHA 已知答案
static void test_sha_known() {
    std::printf("-- 1. SHA known-answer --\n");
    check_eq(nw::sha256_hex(std::string("abc")),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(abc)");
    check_eq(nw::sha512_hex(std::string("abc")),
             "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
             "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
             "sha512(abc)");
    check_eq(nw::sha256_hex(std::string("")),
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(empty)");
    check_eq(nw::sha512_hex(std::string("")),
             "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce"
             "47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e",
             "sha512(empty)");
    std::string a1000(1000, 'a');
    check_eq(nw::sha256_hex(a1000),
             "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3",
             "sha256(a*1000)");
    check_eq(nw::sha512_hex(a1000),
             "67ba5535a46e3f86dbfbed8cbbaf0125c76ed549ff8b0b9e03e0c88cf90fa634"
             "fa7b12b47d77b694de488ace8d9a65967dc96df599727d3292a8d9d447709c97",
             "sha512(a*1000)");
    check_eq(nw::hex_encode(std::string("\x00\x01\xfe\xff", 4)), "0001feff", "hex_encode");

    // 增量更新与一次性结果一致（覆盖 64/128 字节块边界）
    std::string big(1000, 'a');
    nw::Sha256Ctx c6;
    c6.init();
    for (size_t off = 0; off < big.size(); off += 7) {
        size_t n = big.size() - off < 7 ? big.size() - off : 7;
        c6.update(big.data() + off, n);
    }
    check_eq(c6.hex(), nw::sha256_hex(big), "Sha256Ctx incremental");
    nw::Sha512Ctx c5;
    c5.init();
    for (size_t off = 0; off < big.size(); off += 13) {
        size_t n = big.size() - off < 13 ? big.size() - off : 13;
        c5.update(big.data() + off, n);
    }
    check_eq(c5.hex(), nw::sha512_hex(big), "Sha512Ctx incremental");

    // hex_decode
    std::string out;
    check(nw::hex_decode("00ff10", out) && out == std::string("\x00\xff\x10", 3), "hex_decode ok");
    check(!nw::hex_decode("abc", out), "hex_decode odd length");
    check(!nw::hex_decode("zz", out), "hex_decode bad char");
}

// ==================================================== 2. 与 Python 交叉校验
// 下列期望值均由本机 Python 3.12.8 实际运行生成（命令见交付说明）：
//   secret_encrypt("你好，世界 Hello 123", b"0123456789abcdef")
//   secret_sign(b"k", "a", 1, "b")            # parts: "a" "1" "b"
//   secret_sign(b"key", "The quick brown fox jumps over the lazy dog")
//   secret_sign(b"", "x")
//   hmac.new(b"K"*100, b"", sha256) + "part" + b"\x00"
//   hmac.new(b"key", b"", sha256)
//   secret_encrypt("short", b"key")
static void test_python_cross() {
    std::printf("-- 2. Python cross-check --\n");
    const std::string key = "0123456789abcdef";

    const std::string enc1_hex = "7718a0fd13726f9feb6fe3cff2cfa40bc03f2d5c90687b40d1";
    std::string plain = std::string(U8_HELLO) + " Hello 123";
    check_eq(nw::secret_encrypt(plain, key), enc1_hex, "secret_encrypt(你好，世界 Hello 123)");

    bool ok = false;
    std::string back = nw::secret_decrypt(enc1_hex, key, &ok);
    check(ok, "secret_decrypt ok flag");
    check_eq(back, plain, "secret_decrypt roundtrip (python blob)");

    check_eq(nw::secret_sign("k", {"a", "1", "b"}),
             "d6cebb32a204768ffd1111787d3e0063bdd60bc3ed71035466b172168c78c798",
             "secret_sign(k, a, 1, b)");
    check_eq(nw::secret_sign("key", {"The quick brown fox jumps over the lazy dog"}),
             "7627434bc228e1b9d1dcebc8db87d05ca5677967a68fd653639a88f54a455579",
             "secret_sign(key, quick brown fox)  [parts append 0x00]");
    check_eq(nw::secret_sign("", {"x"}),
             "8476dc792482eed8e967fe5f735d4fc81cb1119080531a3d1b0573e161c659fd",
             "secret_sign(empty key, x)");
    check_eq(nw::hmac_sha256_hex(std::string(100, 'K'), {"part"}),
             "f86074a342ecc26b872532a62d8f27eb80d2cd07cd8e1d6580a0ca0e30b24a08",
             "hmac_sha256_hex(100-byte key)");
    check_eq(nw::hmac_sha256_hex("key", {}),
             "5d5d139563c95b5967b9bd9a8c9b233a9dedb45072794cd232dc1b74832607d0",
             "hmac_sha256_hex(key, no parts)");
    check_eq(nw::secret_encrypt("short", "key"), "68fa2ab44b", "secret_encrypt(short,key)");
    check(nw::secret_encrypt("", "key").empty(), "secret_encrypt(empty)");

    // 跨多个 SHA-512 分组的计数器模式：期望值是上述密文**字节**的 SHA-256（Python 生成），
    // 用来紧凑地校验 counter 递增与分组拼接完全一致。
    std::string t1;
    for (int i = 0; i < 10; ++i) t1 += std::string(U8_HELLO) + " Hello 123 ";
    check(t1.size() == 260, "multi-block plaintext #1 length 260");
    std::string b1 = nw::secret_encrypt(t1, key);
    check(b1.size() == 520, "multi-block blob #1 length 520");
    std::string bin1;
    check(nw::hex_decode(b1, bin1), "multi-block blob #1 hex decodes");
    check_eq(nw::sha256_hex(bin1),
             "3698421184095e238106c34d5edeec08b8dac1c500b15261c0d87ad2fcfdb685",
             "secret_encrypt multi-block #1 sha256(ciphertext)");

    std::string t2;
    for (int i = 0; i < 30; ++i) t2 += std::string(U8_EMOJI) + "\xf0\x9f\x8e\x89\xe4\xb8\xad\xe6\x96\x87";
    check(t2.size() == 420, "multi-block plaintext #2 length 420");
    std::string b2 = nw::secret_encrypt(t2, key);
    std::string bin2;
    check(nw::hex_decode(b2, bin2), "multi-block blob #2 hex decodes");
    check_eq(nw::sha256_hex(bin2),
             "e64b27d48773953d3ec668292fc6f0395f591092329d3e4dabc26d208a54f88e",
             "secret_encrypt multi-block #2 sha256(ciphertext)");
    bool ok2b = false;
    check(nw::secret_decrypt(b2, key, &ok2b) == t2 && ok2b,
          "multi-block #2 decrypt roundtrip");
}

// ================================================================= 3. JSON
static void test_json() {
    std::printf("-- 3. JSON --\n");
    const std::string src = "{\"a\":[1,2.5,\"x\\n\\u4e2d\",true,null],\"b\":{\"c\":-3}}";
    bool ok = false;
    nw::Json j = nw::Json::parse(src, &ok);
    check(ok, "json parse ok");
    check(j.isObj(), "json root is object");
    std::vector<std::string> ks = j.keys();
    check(ks.size() == 2 && ks[0] == "a" && ks[1] == "b", "json key insertion order");
    check(j.get("a").isArr() && j.get("a").size() == 5, "json array size");
    check(j.get("a").at(0).asInt() == 1, "json int 1");
    check(std::fabs(j.get("a").at(1).asNum() - 2.5) < 1e-12, "json 2.5");
    check_eq(j.get("a").at(2).asStr(), std::string("x\n") + U8_ZHONG, "json \\n and \\u4e2d");
    check(j.get("a").at(3).asBool() == true, "json true");
    check(j.get("a").at(4).isNull(), "json null");
    check(j.get("b").get("c").asInt() == -3, "json nested -3");
    check(!j.has("zzz") && j.get("zzz").isNull(), "json missing key -> Null");
    check(j.get("a").get("nope").isNull(), "json get on array -> Null");

    // at() 越界抛 std::out_of_range
    bool threw = false;
    try {
        (void)j.get("a").at(99);
    } catch (const std::out_of_range&) {
        threw = true;
    }
    check(threw, "json at() out_of_range throws");

    // 修改接口
    nw::Json m;
    m.set("k", nw::Json(7));
    m.set("k", nw::Json(8));
    m.set("j", nw::Json("v"));
    check(m.isObj() && m.size() == 2, "json set on Null -> object, replace in place");
    check(m.get("k").asInt() == 8, "json set replaces");
    check_eq(m.dump(), "{\"k\":8,\"j\":\"v\"}", "json set order preserved");
    m.remove("k");
    check_eq(m.dump(), "{\"j\":\"v\"}", "json remove");
    nw::Json a;
    a.push(nw::Json(1));
    a.push(nw::Json("z"));
    check(a.isArr() && a.size() == 2 && a.items().size() == 2, "json push on Null -> array");
    check_eq(a.dump(), "[1,\"z\"]", "json array dump");
    a.clear();
    check(a.isNull(), "json clear");

    // 往返
    std::string d1 = j.dump(false);
    bool ok2 = false;
    nw::Json j2 = nw::Json::parse(d1, &ok2);
    check(ok2, "json reparse ok");
    check_eq(j2.dump(false), d1, "json dump/reparse stable");
    check_eq(j2.dump(true), j.dump(true), "json pretty stable");
    check(d1.find(U8_ZHONG) != std::string::npos, "json dump keeps UTF-8 raw (ensure_ascii=False)");
    check(d1.find("\\u4e2d") == std::string::npos, "json dump does not escape CJK");
    check(d1.find("\\n") != std::string::npos, "json dump escapes newline");
    check_eq(nw::Json(1.0).dump(), "1", "json 1.0 -> 1 (integer form)");
    check_eq(nw::Json(2.5).dump(), "2.5", "json 2.5 dump");

    // pretty == Python json.dumps(indent=2, ensure_ascii=False)
    const std::string want_pretty =
        "{\n  \"a\": [\n    1,\n    2.5,\n    \"x\\n" + std::string(U8_ZHONG) +
        "\",\n    true,\n    null\n  ],\n  \"b\": {\n    \"c\": -3\n  }\n}";
    check_eq(j.dump(true), want_pretty, "json pretty == python indent=2");
    check_eq(nw::Json::arr().dump(true), "[]", "json pretty empty array");
    check_eq(nw::Json::obj().dump(true), "{}", "json pretty empty object");
    check_eq(nw::Json::parse("{\"k\":[]}").dump(true), "{\n  \"k\": []\n}",
             "json pretty nested empty");
    check_eq(nw::Json::parse("{\"k\":{}}").dump(true), "{\n  \"k\": {}\n}",
             "json pretty nested empty object");

    // 代理对 -> 4 字节 UTF-8
    bool ok3 = false;
    nw::Json emo = nw::Json::parse("\"\\ud83d\\ude00\"", &ok3);
    check(ok3, "json surrogate parse ok");
    check_eq(emo.asStr(), U8_EMOJI, "json surrogate pair -> U+1F600 (F0 9F 98 80)");
    check_eq(emo.dump(), "\"" + std::string(U8_EMOJI) + "\"", "json emoji dump");

    // 转义与空白容忍
    bool ok4 = false;
    nw::Json s1 = nw::Json::parse("  \t\r\n {\"t\":\"a\\\"b\\\\c\\/d\\b\\f\\t\"} \n", &ok4);
    check(ok4, "json whitespace tolerant");
    check_eq(s1.get("t").asStr(), std::string("a\"b\\c/d\b\f\t"), "json escape decoding");
    check_eq(s1.dump(), "{\"t\":\"a\\\"b\\\\c/d\\b\\f\\t\"}", "json escape re-encoding");

    // 失败用例
    bool bad = true;
    nw::Json::parse("{\"a\":}", &bad);
    check(!bad, "json `{\"a\":}` -> ok=false");
    bad = true;
    nw::Json::parse("{} x", &bad);
    check(!bad, "json trailing text -> ok=false");
    bad = true;
    nw::Json r = nw::Json::parse("[1,2", &bad);
    check(!bad && r.isNull(), "json truncated -> ok=false, Null");
    bad = true;
    nw::Json::parse("", &bad);
    check(!bad, "json empty input -> ok=false");
    bad = false;
    nw::Json::parse("123 ", &bad);
    check(bad, "json number with trailing space ok");
}

// ================================================================= 4. util
static void test_util() {
    std::printf("-- 4. util --\n");
    check_eq(nw::fmt_mac("AABBCCDDEEFF"), "AA-BB-CC-DD-EE-FF", "fmt_mac mac");
    check_eq(nw::fmt_mac("aabbccddeeff"), "AA-BB-CC-DD-EE-FF", "fmt_mac mac lowercase");
    check_eq(nw::fmt_mac("RND112233445566"), "RND-1122-3344-5566", "fmt_mac RND");
    check_eq(nw::fmt_mac("RNDaabbccddeeff"), "RND-AABB-CCDD-EEFF", "fmt_mac RND lowercase");
    check_eq(nw::fmt_mac(""), "\xe6\x9c\xaa\xe7\x9f\xa5", "fmt_mac empty -> 未知");
    check_eq(nw::fmt_mac("hello"), "hello", "fmt_mac passthrough");

    check_eq(nw::human_size(0), "0 B", "human_size(0)");
    check_eq(nw::human_size(512), "512 B", "human_size(512)");
    check_eq(nw::human_size(1023.9), "1023 B", "human_size(1023.9)");
    check_eq(nw::human_size(1024), "1.0 KB", "human_size(1024)");
    check_eq(nw::human_size(1536), "1.5 KB", "human_size(1536)");
    check_eq(nw::human_size(1048576), "1.0 MB", "human_size(1MiB)");
    check_eq(nw::human_size(1099511627776.0), "1.0 TB", "human_size(1TiB)");

    check_eq(nw::sanitize_name("a/b:c"), "a_b_c", "sanitize_name(a/b:c)");
    check_eq(nw::sanitize_name("  x  "), "x", "sanitize_name trim");
    check_eq(nw::sanitize_name(""), "\xe6\x9c\xaa\xe5\x91\xbd\xe5\x90\x8d", "sanitize_name empty");
    check_eq(nw::sanitize_name("a\\b*c?d\"e<f>g|h"), "a_b_c_d_e_f_g_h", "sanitize_name all bad");
    check_eq(nw::sanitize_name("a\r\n\tb"), "a___b", "sanitize_name crlf tab (each -> _)");
    // 按码点截断，不切出半个 UTF-8 字符
    check_eq(nw::sanitize_name("\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad", 3),
             "\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad", "sanitize_name utf8 truncate by char");
    check(nw::sanitize_name("\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad\xe4\xb8\xad", 3).size() == 9,
          "sanitize_name truncate byte length");

    check_eq(nw::to_lower_ascii("AbC\xe4\xb8\xad"), "abc\xe4\xb8\xad", "to_lower_ascii");
    check_eq(nw::trim(" \t x \n"), "x", "trim");
    std::vector<std::string> sp = nw::split("a,b,,c", ',');
    check(sp.size() == 4 && sp[0] == "a" && sp[1] == "b" && sp[2] == "" && sp[3] == "c", "split");
    check(nw::split("", ',').size() == 1 && nw::split("", ',')[0] == "", "split empty");
    check_eq(nw::join({"a", "b", "c"}, "-"), "a-b-c", "join");
    check_eq(nw::join({}, "-"), "", "join empty");

    // ws2u8 / u82ws 往返
    std::string cn = U8_HELLO;
    check_eq(nw::ws2u8(nw::u82ws(cn)), cn, "u8<->w roundtrip");

    // 时间
    double t = nw::now_ts();
    check(t > 1600000000.0 && t < 3000000000.0, "now_ts sane");
    check(nw::fmt_time(t).size() == 14, "fmt_time length 14 (MM-DD HH:MM:SS)");
    std::printf("   now_ts=%.3f fmt_time=%s\n", t, nw::fmt_time(t).c_str());
    check_eq(nw::fmt_time(t).substr(2, 1), "-", "fmt_time format dash");

    // 路径
    check_eq(nw::path_join("a", "b"), "a\\b", "path_join");
    check_eq(nw::path_join("a\\", "b"), "a\\b", "path_join trailing sep");
    check_eq(nw::path_join("", "b"), "b", "path_join empty a");
    check_eq(nw::path_join("a", "C:\\x"), "C:\\x", "path_join absolute b");
    check_eq(nw::basename_utf8("C:\\a\\b\\c.txt"), "c.txt", "basename");
    check_eq(nw::basename_utf8("c.txt"), "c.txt", "basename plain");

    std::string exe_dir = nw::get_exe_dir();
    std::printf("   exe_dir=%s\n", exe_dir.c_str());
    check(!exe_dir.empty() && nw::is_dir(exe_dir), "get_exe_dir exists");
    check(exe_dir.find('\\') != std::string::npos && exe_dir.back() != '\\',
          "get_exe_dir no trailing sep");

    // 文件操作
    std::string dir = nw::path_join(exe_dir, "nw_test_dir_\xe6\xb5\x8b\xe8\xaf\x95");
    check(nw::ensure_dir(dir), "ensure_dir (utf-8 name)");
    check(nw::is_dir(dir), "is_dir");
    std::string f1 = nw::path_join(dir, "\xe4\xb8\xad\xe6\x96\x87.txt");
    std::string payload = "hello \xe4\xb8\xad\xe6\x96\x87\n\x00\xff";
    check(nw::write_file_utf8(f1, payload), "write_file_utf8");
    check(nw::file_exists(f1), "file_exists");
    check(nw::file_size(f1) == static_cast<long long>(payload.size()), "file_size");
    std::string got;
    check(nw::read_file_utf8(f1, got) && got == payload, "read_file_utf8 roundtrip");
    std::vector<std::string> ls = nw::list_dir(dir);
    check(ls.size() == 1 && ls[0] == "\xe4\xb8\xad\xe6\x96\x87.txt", "list_dir");
    std::string f2 = nw::path_join(dir, "copy.bin");
    check(nw::copy_file(f1, f2, true), "copy_file");
    check(nw::file_exists(f2), "copy_file result exists");
    check(nw::remove_file(f2), "remove_file");
    check(!nw::file_exists(f2), "remove_file gone");
    check(!nw::remove_file(nw::path_join(dir, "no_such_file_xyz")), "remove_file missing -> false");
    check(nw::remove_dir_recursive(dir), "remove_dir_recursive");
    check(!nw::is_dir(dir), "dir removed");

    // 本机信息
    std::string host = nw::hostname_utf8();
    std::printf("   hostname=%s machine_uid=%s fmt_mac=%s\n", host.c_str(),
                nw::machine_uid().c_str(), nw::fmt_mac(nw::machine_uid()).c_str());
    check(!host.empty(), "hostname_utf8 non-empty");
    std::string uid = nw::machine_uid();
    check((uid.size() == 12 || (uid.size() == 15 && uid.compare(0, 3, "RND") == 0)),
          "machine_uid shape");
    check(nw::fmt_mac(uid).find("--") == std::string::npos, "machine_uid fmt_mac sane");
    std::vector<std::string> ips = nw::local_ipv4_list();
    std::printf("   local_ipv4:");
    for (const auto& ip : ips) std::printf(" %s", ip.c_str());
    std::printf("\n");
    check(!ips.empty(), "local_ipv4_list non-empty");
    bool no_loop = true;
    for (const auto& ip : ips)
        if (ip == "127.0.0.1" && ips.size() > 1) no_loop = false;
    check(no_loop, "local_ipv4_list excludes 127.0.0.1 unless alone");
}

// =============================================================== 5. 加解密
static void test_crypto() {
    std::printf("-- 5. crypto roundtrip / tamper --\n");
    const std::string key = "0123456789abcdef";

    // 用随机字节从 UTF-8 字符池里抽出的随机文本（>1000 字节）
    // 池子 = 8 个常用汉字 + 8 个 emoji，贴近真实群聊内容。
    static const char* pool[] = {"\xe4\xbd\xa0",         // 你
                                 "\xe5\xa5\xbd",         // 好
                                 "\xe4\xb8\x96",         // 世
                                 "\xe7\x95\x8c",         // 界
                                 "\xe6\xb6\x88",         // 消
                                 "\xe6\x81\xaf",         // 息
                                 "\xe6\xb5\x8b",         // 测
                                 "\xe8\xaf\x95",         // 试
                                 "\xf0\x9f\x98\x80",     // U+1F600
                                 "\xf0\x9f\x8e\x89",     // U+1F389
                                 "\xf0\x9f\x91\x8d",     // U+1F44D
                                 "\xf0\x9f\x98\x82",     // U+1F602
                                 "\xf0\x9f\x98\x8a",     // U+1F60A
                                 "\xf0\x9f\x94\xa5",     // U+1F525
                                 "\xf0\x9f\x92\xa1",     // U+1F4A1
                                 "\xf0\x9f\x9a\x80"};    // U+1F680
    const size_t pool_n = sizeof(pool) / sizeof(pool[0]);
    std::string rnd = nw::random_bytes(600);
    std::string text;
    text.reserve(2400);
    for (size_t i = 0; i < rnd.size(); ++i)
        text += pool[static_cast<unsigned char>(rnd[i]) % pool_n];
    std::printf("   plaintext bytes=%zu\n", text.size());
    check(text.size() >= 1000, "random text >= 1000 bytes");

    std::string blob = nw::secret_encrypt(text, key);
    check(blob.size() == text.size() * 2, "blob is hex of same length");
    bool ok = false;
    std::string dec = nw::secret_decrypt(blob, key, &ok);
    check(ok, "decrypt ok");
    check(dec == text, "encrypt/decrypt 1000-byte roundtrip");
    // 错 key
    bool okw = true;
    nw::secret_decrypt(blob, "wrong-key", &okw);
    check(!okw, "wrong key -> ok=false (invalid utf8)");

    // 篡改 hex 一个字符（改成另一个十六进制字符）-> 大多数情况 ok=false。
    // 原理说明：secret_* 是「SHA-512 计数器 XOR」，本身不做认证，ok=false 只可能
    // 来自「解密结果不是合法 UTF-8」。改动密文某个 hex 字符后，对应明文字节的那个
    // 半字节会变成 keystream 的随机值，等价于「随机替换明文的半个字节」：
    //   · 改动字节的**高**半字节 -> 该字节多半不再是合法的 lead/continuation（约 88% 被拒）
    //   · 改动字节的**低**半字节 -> 续字节低半字节仍落在 0x80..0xBF，仍然是合法 UTF-8
    // 所以任意位置均匀篡改的整体检出率约 50%（理论天花板），而「字节高半字节」篡改
    // 的检出率接近 90%。下面两条断言分别覆盖这两种口径。
    // 另：真实完整性校验请用 secret_sign（HMAC），见前面第 2 组用例。
    int tamper_total = 0, tamper_fail = 0, tamper_same = 0;
    int hi_total = 0, hi_fail = 0, lo_total = 0, lo_fail = 0;
    for (size_t p = 0; p < blob.size(); ++p) {
        std::string t = blob;
        char orig = t[p];
        t[p] = (orig == '0') ? '7' : '0';
        bool o = true;
        std::string got = nw::secret_decrypt(t, key, &o);
        ++tamper_total;
        if (!o)
            ++tamper_fail;
        else if (got == text)
            ++tamper_same;
        if (p % 2 == 0) { // 每个字节的高半字节
            ++hi_total;
            if (!o) ++hi_fail;
        } else {
            ++lo_total;
            if (!o) ++lo_fail;
        }
    }
    double ratio = static_cast<double>(tamper_fail) / static_cast<double>(tamper_total);
    double ratio_hi = static_cast<double>(hi_fail) / static_cast<double>(hi_total);
    double ratio_lo = static_cast<double>(lo_fail) / static_cast<double>(lo_total);
    std::printf("   tamper any hex char : %d/%d ok=false (%.1f%%)\n", tamper_fail, tamper_total,
                ratio * 100.0);
    std::printf("   tamper high nibble  : %d/%d ok=false (%.1f%%)  [low nibble %.1f%%]\n", hi_fail,
                hi_total, ratio_hi * 100.0, ratio_lo * 100.0);
    std::printf("   silently identical  : %d\n", tamper_same);
    check(ratio_hi > 0.8, "tampered hex (high nibble) mostly -> ok=false");
    check(ratio > 0.4, "tampered hex (any position) ~half -> ok=false");
    check(tamper_same == 0, "no tamper is silently accepted with identical plaintext");

    // 非法 hex
    bool oh = true;
    nw::secret_decrypt("zzzz", key, &oh);
    check(!oh, "invalid hex -> ok=false");
    oh = true;
    nw::secret_decrypt("abc", key, &oh);
    check(!oh, "odd hex -> ok=false");

    // 随机数
    std::string r1 = nw::random_bytes(32), r2 = nw::random_bytes(32);
    check(r1.size() == 32 && r2.size() == 32 && r1 != r2, "random_bytes differs");
    check(nw::random_bytes(0).empty(), "random_bytes(0) empty");
    std::string h = nw::random_hex_upper(6);
    check(h.size() == 12, "random_hex_upper length");
    bool upper = true;
    for (char c : h)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) upper = false;
    check(upper, "random_hex_upper is UPPER hex");
}

// =========================================================== 6. 文件 SHA256
static void test_file_sha() {
    std::printf("-- 6. sha256_file_hex --\n");
    std::string path = nw::path_join(nw::get_exe_dir(), "nw_tmp_sha_test.bin");
    std::string data;
    for (int i = 0; i < 200000; ++i) data.push_back(static_cast<char>('A' + (i % 26)));
    check(nw::write_file_utf8(path, data), "write temp file");
    std::string want = nw::sha256_hex(data);
    std::string got;
    int calls = 0;
    double last = -1.0;
    bool ok = nw::sha256_file_hex(nw::u82ws(path), got, [&](double p) {
        ++calls;
        last = p;
    });
    check(ok, "sha256_file_hex ok");
    check_eq(got, want, "sha256_file_hex == sha256_hex(memory)");
    check(calls > 0 && last > 0.99, "progress callback fired to 1.0");
    std::printf("   file=%s calls=%d last=%.3f\n", got.c_str(), calls, last);
    check(!nw::sha256_file_hex(nw::u82ws(path + ".missing"), got), "missing file -> false");
    check(nw::remove_file(path), "cleanup temp file");
}

int main() {
    std::printf("=== test_common (base layer) ===\n");
    test_sha_known();
    test_python_cross();
    test_json();
    test_util();
    test_crypto();
    test_file_sha();
    std::printf("=== result: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
