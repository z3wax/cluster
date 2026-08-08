#pragma once
// Minimal JSON parser/serializer (C++17, hech qanday tashqi bog'liqliksiz)
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <sstream>

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    enum Type { Null, Bool, Num, Str, Arr, Obj };

    Json() : v_(nullptr) {}
    Json(bool b) : v_(b) {}
    Json(double d) : v_(d) {}
    Json(int i) : v_(static_cast<double>(i)) {}
    Json(int64_t i) : v_(static_cast<double>(i)) {}
    Json(const char* s) : v_(std::string(s)) {}
    Json(const std::string& s) : v_(s) {}
    Json(const Array& a) : v_(a) {}
    Json(const Object& o) : v_(o) {}

    Type type() const { return static_cast<Type>(v_.index()); }

    bool isNull() const { return v_.index() == Null; }
    bool isBool() const { return v_.index() == Bool; }
    bool isNum() const { return v_.index() == Num; }
    bool isStr() const { return v_.index() == Str; }
    bool isArr() const { return v_.index() == Arr; }
    bool isObj() const { return v_.index() == Obj; }

    bool asBool(bool dflt = false) const { return isBool() ? std::get<bool>(v_) : dflt; }
    double asNum(double dflt = 0) const { return isNum() ? std::get<double>(v_) : dflt; }
    int64_t asInt(int64_t dflt = 0) const { return isNum() ? (int64_t)std::get<double>(v_) : dflt; }
    const std::string& asStr(const std::string& dflt = "") const {
        static const std::string empty;
        return isStr() ? std::get<std::string>(v_) : dflt;
    }
    const Array& asArr() const { static const Array e; return isArr() ? std::get<Array>(v_) : e; }
    const Object& asObj() const { static const Object e; return isObj() ? std::get<Object>(v_) : e; }

    // operator[] — object uchun
    const Json& operator[](const std::string& key) const {
        static const Json none;
        if (!isObj()) return none;
        const auto& o = std::get<Object>(v_);
        auto it = o.find(key);
        return it == o.end() ? none : it->second;
    }

    Json& operator[](const std::string& key) {
        if (v_.index() != Obj) v_ = Object();
        return std::get<Object>(v_)[key];
    }

    void push(const Json& j) {
        if (v_.index() != Arr) v_ = Array();
        std::get<Array>(v_).push_back(j);
    }

    std::string dump() const {
        std::string out;
        dumpInto(out);
        return out;
    }

    // ---- Parse ----
    static Json parse(const std::string& s, bool* ok = nullptr) {
        bool bad = false;
        size_t i = 0;
        Json j = parseValue(s, i, &bad);
        while (i < s.size() && ws(s[i])) i++;
        if (!bad && i < s.size()) bad = true;
        if (ok) *ok = !bad;
        return j;
    }

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v_;

    static bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    static Json parseValue(const std::string& s, size_t& i, bool* ok) {
        while (i < s.size() && ws(s[i])) i++;
        if (i >= s.size()) { if (ok) *ok = false; return Json(); }
        char c = s[i];
        if (c == '{') return parseObj(s, i, ok);
        if (c == '[') return parseArr(s, i, ok);
        if (c == '"') return parseStr(s, i, ok);
        if (c == 't' || c == 'f') return parseBool(s, i, ok);
        if (c == 'n') { if (s.compare(i, 4, "null") == 0) { i += 4; return Json(); } if (ok) *ok = false; return Json(); }
        return parseNum(s, i, ok);
    }

    static Json parseObj(const std::string& s, size_t& i, bool* ok) {
        i++; // {
        Object o;
        while (i < s.size()) {
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == '}') { i++; return Json(o); }
            Json k = parseValue(s, i, ok);
            if (!k.isStr()) { if (ok) *ok = false; return Json(); }
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == ':') i++;
            Json v = parseValue(s, i, ok);
            o[k.asStr()] = v;
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') { i++; return Json(o); }
            break;
        }
        if (ok) *ok = false;
        return Json();
    }

    static Json parseArr(const std::string& s, size_t& i, bool* ok) {
        i++; // [
        Array a;
        while (i < s.size()) {
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == ']') { i++; return Json(a); }
            a.push_back(parseValue(s, i, ok));
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == ']') { i++; return Json(a); }
            break;
        }
        if (ok) *ok = false;
        return Json();
    }

    static Json parseStr(const std::string& s, size_t& i, bool* ok) {
        i++; // "
        std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return Json(out);
            if (c == '\\' && i < s.size()) {
                char e = s[i++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '\\': out += '\\'; break;
                    case '"': out += '"'; break;
                    case '/': out += '/'; break;
                    case 'u': {
                        if (i + 4 <= s.size()) {
                            out += (char)std::strtol(s.substr(i, 4).c_str(), nullptr, 16);
                            i += 4;
                        }
                        break;
                    }
                    default: out += '\\'; out += e; // noma'lum escape — aslini saqlaymiz
                }
            } else out += c;
        }
        if (ok) *ok = false;
        return Json();
    }

    static Json parseBool(const std::string& s, size_t& i, bool* ok) {
        if (s.compare(i, 4, "true") == 0) { i += 4; return Json(true); }
        if (s.compare(i, 5, "false") == 0) { i += 5; return Json(false); }
        if (ok) *ok = false;
        return Json();
    }

    static Json parseNum(const std::string& s, size_t& i, bool* ok) {
        size_t start = i;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '-' || s[i] == '+' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            i++;
        std::string tok = s.substr(start, i - start);
        if (tok.empty()) { if (ok) *ok = false; return Json(); }
        return Json(std::strtod(tok.c_str(), nullptr));
    }

    void dumpInto(std::string& out) const {
        switch (v_.index()) {
            case Null: out += "null"; break;
            case Bool: out += std::get<bool>(v_) ? "true" : "false"; break;
            case Num: {
                std::ostringstream ss;
                double d = std::get<double>(v_);
                if (d == (int64_t)d && std::abs(d) < 1e15) ss << (int64_t)d;
                else ss << d;
                out += ss.str();
                break;
            }
            case Str: {
                out += '"';
                for (char c : std::get<std::string>(v_)) {
                    if (c == '"') out += "\\\"";
                    else if (c == '\\') out += "\\\\";
                    else if (c == '\n') out += "\\n";
                    else if (c == '\r') out += "\\r";
                    else if (c == '\t') out += "\\t";
                    else out += c;
                }
                out += '"';
                break;
            }
            case Arr: {
                out += '[';
                bool first = true;
                for (const auto& e : std::get<Array>(v_)) { if (!first) out += ','; first = false; e.dumpInto(out); }
                out += ']';
                break;
            }
            case Obj: {
                out += '{';
                bool first = true;
                for (const auto& kv : std::get<Object>(v_)) {
                    if (!first) out += ',';
                    first = false;
                    Json(kv.first).dumpInto(out);
                    out += ':';
                    kv.second.dumpInto(out);
                }
                out += '}';
                break;
            }
        }
    }
};
