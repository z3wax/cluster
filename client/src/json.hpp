#pragma once
// Minimal JSON parser/serializer (C++17, hech qanday tashqi bog'liqliksiz)
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <cctype>

class Json {
public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    enum Type { Null, Bool, Num, Str, Arr, Obj };

    Json() : v_(nullptr) {}
    Json(bool b) : v_(b) {}
    Json(double d) : v_(d) {}
    Json(int i) : v_(static_cast<double>(i)) {}
    Json(long i) : v_(static_cast<double>(i)) {}
    Json(long long i) : v_(static_cast<double>(i)) {}
    Json(unsigned i) : v_(static_cast<double>(i)) {}
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
    int64_t asInt(int64_t dflt = 0) const { return isNum() ? (int64_t)std::llround(std::get<double>(v_)) : dflt; }
    std::string asStr(const std::string& dflt = "") const { return isStr() ? std::get<std::string>(v_) : dflt; }
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

    bool has(const std::string& key) const { return isObj() && std::get<Object>(v_).count(key) > 0; }

    void erase(const std::string& key) {
        if (isObj()) std::get<Object>(v_).erase(key);
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
        bool good = true;
        size_t i = 0;
        Json j = parseValue(s, i, good, 0);
        while (i < s.size() && ws(s[i])) i++;
        if (i < s.size()) good = false;
        if (ok) *ok = good;
        return good ? j : Json();
    }

private:
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v_;

    static bool ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

    static Json parseValue(const std::string& s, size_t& i, bool& ok, int depth) {
        if (depth > 64) { ok = false; return Json(); }
        while (i < s.size() && ws(s[i])) i++;
        if (i >= s.size()) { ok = false; return Json(); }
        char c = s[i];
        if (c == '{') return parseObj(s, i, ok, depth);
        if (c == '[') return parseArr(s, i, ok, depth);
        if (c == '"') return parseStr(s, i, ok);
        if (s.compare(i, 4, "true") == 0) { i += 4; return Json(true); }
        if (s.compare(i, 5, "false") == 0) { i += 5; return Json(false); }
        if (s.compare(i, 4, "null") == 0) { i += 4; return Json(); }
        return parseNum(s, i, ok);
    }

    static Json parseObj(const std::string& s, size_t& i, bool& ok, int depth) {
        i++; // {
        Object o;
        while (ok && i < s.size()) {
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == '}') { i++; return Json(o); }
            if (i >= s.size() || s[i] != '"') break;
            Json k = parseStr(s, i, ok);
            while (i < s.size() && ws(s[i])) i++;
            if (i >= s.size() || s[i] != ':') break;
            i++;
            Json v = parseValue(s, i, ok, depth + 1);
            if (!ok) break;
            o[k.asStr()] = v;
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') { i++; return Json(o); }
            break;
        }
        ok = false;
        return Json();
    }

    static Json parseArr(const std::string& s, size_t& i, bool& ok, int depth) {
        i++; // [
        Array a;
        while (ok && i < s.size()) {
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == ']') { i++; return Json(a); }
            a.push_back(parseValue(s, i, ok, depth + 1));
            if (!ok) break;
            while (i < s.size() && ws(s[i])) i++;
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == ']') { i++; return Json(a); }
            break;
        }
        ok = false;
        return Json();
    }

    static void putUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
        }
    }

    static bool hex4(const std::string& s, size_t i, uint32_t& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (size_t k = 0; k < 4; k++) {
            char c = s[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    static Json parseStr(const std::string& s, size_t& i, bool& ok) {
        i++; // "
        std::string out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return Json(out);
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            char e = s[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case '/': out += '/'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(s, i, cp)) { ok = false; return Json(); }
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        uint32_t lo;
                        if (hex4(s, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;
                        }
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: ok = false; return Json();
            }
        }
        ok = false;
        return Json();
    }

    static Json parseNum(const std::string& s, size_t& i, bool& ok) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') i++;
        while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' ||
                                s[i] == '.' || s[i] == 'e' || s[i] == 'E'))
            i++;
        std::string tok = s.substr(start, i - start);
        char* end = nullptr;
        double d = tok.empty() ? 0 : std::strtod(tok.c_str(), &end);
        if (tok.empty() || end != tok.c_str() + tok.size()) { ok = false; return Json(); }
        return Json(d);
    }

    void dumpInto(std::string& out) const {
        switch (v_.index()) {
            case Null: out += "null"; break;
            case Bool: out += std::get<bool>(v_) ? "true" : "false"; break;
            case Num: {
                double d = std::get<double>(v_);
                char buf[32];
                if (!std::isfinite(d)) { out += "null"; break; }
                if (d == std::floor(d) && std::fabs(d) < 9e15) snprintf(buf, sizeof(buf), "%lld", (long long)d);
                else snprintf(buf, sizeof(buf), "%.17g", d);
                out += buf;
                break;
            }
            case Str: {
                out += '"';
                for (char c : std::get<std::string>(v_)) {
                    unsigned char u = (unsigned char)c;
                    if (c == '"') out += "\\\"";
                    else if (c == '\\') out += "\\\\";
                    else if (c == '\n') out += "\\n";
                    else if (c == '\r') out += "\\r";
                    else if (c == '\t') out += "\\t";
                    else if (u < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", u);
                        out += buf;
                    } else out += c;
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
