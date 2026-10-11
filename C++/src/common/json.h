// json.h —— 极简 JSON 解析/序列化（零第三方依赖，仅 C++17 标准库）
// 语义对齐 Python json 模块（dump 等价 json.dumps(..., ensure_ascii=False, indent=2)）
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace nw {

class Json {
public:
    enum class T { Null, Bool, Num, Str, Arr, Obj };

    Json();
    Json(std::nullptr_t);
    Json(bool);
    Json(double);
    Json(int);
    Json(long long);
    Json(const char*);
    Json(const std::string&);

    static Json obj();
    static Json arr();

    T type() const;
    bool isNull() const;
    bool isBool() const;
    bool isNum() const;
    bool isStr() const;
    bool isArr() const;
    bool isObj() const;

    bool asBool(bool def = false) const;
    double asNum(double def = 0) const;
    long long asInt(long long def = 0) const;
    std::string asStr(const std::string& def = "") const;

    bool has(const std::string& k) const;
    Json get(const std::string& k) const;          // 对象取值；不存在返回 Null，不抛异常
    void set(const std::string& k, const Json& v); // 自动把 Null 变对象
    void remove(const std::string& k);
    std::vector<std::string> keys() const;         // 对象键（插入顺序）
    size_t size() const;

    void push(const Json& v);                      // 数组追加；Null 变数组
    const Json& at(size_t i) const;                // 数组下标；越界抛 std::out_of_range
    const std::vector<Json>& items() const;        // 数组元素

    void clear();

    std::string dump(bool pretty = false) const;
    static Json parse(const std::string& utf8, bool* ok = nullptr);

private:
    void dump_to(std::string& out, bool pretty, int depth) const;

    T t_ = T::Null;
    bool b_ = false;
    double n_ = 0.0;
    std::string s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

} // namespace nw
