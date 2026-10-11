// json.cpp —— 极简 JSON 解析/序列化实现
#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace nw {

static const std::vector<Json> kEmptyArr;

Json::Json() : t_(T::Null) {}
Json::Json(std::nullptr_t) : t_(T::Null) {}
Json::Json(bool v) : t_(T::Bool), b_(v) {}
Json::Json(double v) : t_(T::Num), n_(v) {}
Json::Json(int v) : t_(T::Num), n_(static_cast<double>(v)) {}
Json::Json(long long v) : t_(T::Num), n_(static_cast<double>(v)) {}
Json::Json(const char* v) : t_(T::Str), s_(v ? v : "") {}
Json::Json(const std::string& v) : t_(T::Str), s_(v) {}

Json Json::obj() {
    Json j;
    j.t_ = T::Obj;
    return j;
}

Json Json::arr() {
    Json j;
    j.t_ = T::Arr;
    return j;
}

Json::T Json::type() const { return t_; }
bool Json::isNull() const { return t_ == T::Null; }
bool Json::isBool() const { return t_ == T::Bool; }
bool Json::isNum() const { return t_ == T::Num; }
bool Json::isStr() const { return t_ == T::Str; }
bool Json::isArr() const { return t_ == T::Arr; }
bool Json::isObj() const { return t_ == T::Obj; }

bool Json::asBool(bool def) const { return t_ == T::Bool ? b_ : def; }
double Json::asNum(double def) const { return t_ == T::Num ? n_ : def; }

long long Json::asInt(long long def) const {
    if (t_ != T::Num) return def;
    if (!std::isfinite(n_)) return def;
    return static_cast<long long>(n_);
}

std::string Json::asStr(const std::string& def) const { return t_ == T::Str ? s_ : def; }

bool Json::has(const std::string& k) const {
    if (t_ != T::Obj) return false;
    for (const auto& kv : obj_)
        if (kv.first == k) return true;
    return false;
}

Json Json::get(const std::string& k) const {
    if (t_ == T::Obj) {
        for (const auto& kv : obj_)
            if (kv.first == k) return kv.second;
    }
    return Json();
}

void Json::set(const std::string& k, const Json& v) {
    if (t_ != T::Obj) {
        clear();
        t_ = T::Obj;
    }
    for (auto& kv : obj_) {
        if (kv.first == k) {
            kv.second = v;
            return;
        }
    }
    obj_.emplace_back(k, v);
}

void Json::remove(const std::string& k) {
    if (t_ != T::Obj) return;
    for (size_t i = 0; i < obj_.size(); ++i) {
        if (obj_[i].first == k) {
            obj_.erase(obj_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

std::vector<std::string> Json::keys() const {
    std::vector<std::string> out;
    if (t_ == T::Obj) {
        out.reserve(obj_.size());
        for (const auto& kv : obj_) out.push_back(kv.first);
    }
    return out;
}

size_t Json::size() const {
    switch (t_) {
        case T::Arr: return arr_.size();
        case T::Obj: return obj_.size();
        case T::Str: return s_.size();
        default: return 0;
    }
}

void Json::push(const Json& v) {
    if (t_ != T::Arr) {
        clear();
        t_ = T::Arr;
    }
    arr_.push_back(v);
}

const Json& Json::at(size_t i) const {
    if (t_ != T::Arr || i >= arr_.size()) throw std::out_of_range("Json::at out of range");
    return arr_[i];
}

const std::vector<Json>& Json::items() const { return t_ == T::Arr ? arr_ : kEmptyArr; }

void Json::clear() {
    t_ = T::Null;
    b_ = false;
    n_ = 0.0;
    s_.clear();
    arr_.clear();
    obj_.clear();
}

// ================================================================ 序列化 ==
static void escape_into(std::string& out, const std::string& s) {
    out.push_back('"');
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

static void num_into(std::string& out, double n) {
    if (std::isnan(n)) {
        out += "NaN";
        return;
    }
    if (std::isinf(n)) {
        out += n > 0 ? "Infinity" : "-Infinity";
        return;
    }
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), n);
    if (res.ec == std::errc()) {
        out.append(buf, res.ptr);
        return;
    }
    std::snprintf(buf, sizeof(buf), "%.17g", n);
    out += buf;
}

static void indent_into(std::string& out, int depth) {
    out.push_back('\n');
    out.append(static_cast<size_t>(depth) * 2, ' ');
}

void Json::dump_to(std::string& out, bool pretty, int depth) const {
    switch (t_) {
        case T::Null: out += "null"; break;
        case T::Bool: out += b_ ? "true" : "false"; break;
        case T::Num: num_into(out, n_); break;
        case T::Str: escape_into(out, s_); break;
        case T::Arr:
            if (arr_.empty()) {
                out += "[]";
                break;
            }
            out.push_back('[');
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out.push_back(',');
                if (pretty) indent_into(out, depth + 1);
                arr_[i].dump_to(out, pretty, depth + 1);
            }
            if (pretty) indent_into(out, depth);
            out.push_back(']');
            break;
        case T::Obj:
            if (obj_.empty()) {
                out += "{}";
                break;
            }
            out.push_back('{');
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out.push_back(',');
                if (pretty) indent_into(out, depth + 1);
                escape_into(out, obj_[i].first);
                out.push_back(':');
                if (pretty) out.push_back(' ');
                obj_[i].second.dump_to(out, pretty, depth + 1);
            }
            if (pretty) indent_into(out, depth);
            out.push_back('}');
            break;
    }
}

std::string Json::dump(bool pretty) const {
    std::string out;
    dump_to(out, pretty, 0);
    return out;
}

// ================================================================== 解析 ==
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    int depth = 0;

    explicit Parser(const std::string& str) : s(str) {}

    void skip_ws() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i;
            else break;
        }
    }

    bool fail() {
        ok = false;
        return false;
    }

    static void append_utf8(std::string& out, unsigned int cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_hex4(unsigned int& v) {
        if (i + 4 > s.size()) return fail();
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i + static_cast<size_t>(k)];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return fail();
            v = (v << 4) | static_cast<unsigned int>(d);
        }
        i += 4;
        return true;
    }

    bool parse_string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail();
        ++i;
        out.clear();
        while (i < s.size()) {
            unsigned char c = static_cast<unsigned char>(s[i]);
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) return fail();
                char e = s[i++];
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        unsigned int cp = 0;
                        if (!parse_hex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // 高代理：需要 \uXXXX 低代理
                            if (i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                                size_t save = i;
                                i += 2;
                                unsigned int lo = 0;
                                if (!parse_hex4(lo)) return false;
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    unsigned int full =
                                        0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                    append_utf8(out, full);
                                } else {
                                    // 不是低代理：回退，单独编码高代理（CESU-8 形式）
                                    i = save;
                                    append_utf8(out, cp);
                                }
                            } else {
                                append_utf8(out, cp);
                            }
                        } else {
                            append_utf8(out, cp);
                        }
                        break;
                    }
                    default: return fail();
                }
                continue;
            }
            if (c < 0x20) return fail(); // 未转义控制字符非法
            out.push_back(static_cast<char>(c));
            ++i;
        }
        return fail();
    }

    bool parse_number(Json& out) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail();
        if (s[i] == '0') {
            ++i;
        } else {
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail();
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail();
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        std::string tok = s.substr(start, i - start);
        char* endp = nullptr;
        double v = std::strtod(tok.c_str(), &endp);
        if (endp == tok.c_str()) return fail();
        out = Json(v);
        return true;
    }

    bool parse_value(Json& out) {
        if (++depth > 200) return fail();
        skip_ws();
        if (i >= s.size()) return fail();
        bool r = false;
        char c = s[i];
        if (c == '{') {
            ++i;
            Json o = Json::obj();
            skip_ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                out = o;
                --depth;
                return true;
            }
            for (;;) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) return fail();
                skip_ws();
                if (i >= s.size() || s[i] != ':') return fail();
                ++i;
                Json v;
                if (!parse_value(v)) return fail();
                o.set(key, v);
                skip_ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    out = o;
                    r = true;
                    break;
                }
                return fail();
            }
            --depth;
            return r;
        } else if (c == '[') {
            ++i;
            Json a = Json::arr();
            skip_ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                out = a;
                --depth;
                return true;
            }
            for (;;) {
                Json v;
                if (!parse_value(v)) return fail();
                a.push(v);
                skip_ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    out = a;
                    r = true;
                    break;
                }
                return fail();
            }
            --depth;
            return r;
        } else if (c == '"') {
            std::string v;
            if (!parse_string(v)) return fail();
            out = Json(v);
        } else if (c == 't') {
            if (s.compare(i, 4, "true") != 0) return fail();
            i += 4;
            out = Json(true);
        } else if (c == 'f') {
            if (s.compare(i, 5, "false") != 0) return fail();
            i += 5;
            out = Json(false);
        } else if (c == 'n') {
            if (s.compare(i, 4, "null") != 0) return fail();
            i += 4;
            out = Json(nullptr);
        } else if (c == 'N' || c == 'I') { // 兼容 Python 的 NaN / Infinity
            if (s.compare(i, 3, "NaN") == 0) {
                i += 3;
                out = Json(std::nan(""));
            } else if (s.compare(i, 8, "Infinity") == 0) {
                i += 8;
                out = Json(std::numeric_limits<double>::infinity());
            } else if (s.compare(i, 9, "-Infinity") == 0) {
                i += 9;
                out = Json(-std::numeric_limits<double>::infinity());
            } else {
                return fail();
            }
        } else if (c == '-') {
            if (s.compare(i, 9, "-Infinity") == 0) {
                i += 9;
                out = Json(-std::numeric_limits<double>::infinity());
            } else if (!parse_number(out)) {
                return fail();
            }
        } else if (c >= '0' && c <= '9') {
            if (!parse_number(out)) return fail();
        } else {
            return fail();
        }
        --depth;
        return true;
    }
};

} // namespace

Json Json::parse(const std::string& utf8, bool* ok) {
    Parser p(utf8);
    Json out;
    if (!p.parse_value(out)) {
        if (ok) *ok = false;
        return Json();
    }
    p.skip_ws();
    if (p.i != utf8.size()) {
        if (ok) *ok = false;
        return Json();
    }
    if (ok) *ok = true;
    return out;
}

} // namespace nw
