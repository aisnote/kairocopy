// json.hpp — a small JSON value: parse, read, build, print (from moviecpp). Used for the --json output, ffprobe and the
// web API. Header only; no dependencies.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kc::json {

class Value {
  public:
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Value() = default;
    Value(bool v) : type_(Bool), b_(v) {}
    Value(double v) : type_(Num), n_(v) {}
    Value(int v) : type_(Num), n_(v) {}
    Value(int64_t v) : type_(Num), n_(double(v)) {}
    Value(size_t v) : type_(Num), n_(double(v)) {}
    Value(const char* v) : type_(Str), s_(v) {}
    Value(std::string v) : type_(Str), s_(std::move(v)) {}
    Value(std::string_view v) : type_(Str), s_(v) {}
    static Value object() { Value v; v.type_ = Obj; return v; }
    static Value array() { Value v; v.type_ = Arr; return v; }
    static Value object(std::initializer_list<std::pair<std::string, Value>> kv) { Value v = object(); for (auto& p : kv) v.o_.push_back(p); return v; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Null; }
    bool is_object() const { return type_ == Obj; }
    bool is_array() const { return type_ == Arr; }
    bool is_string() const { return type_ == Str; }
    bool is_number() const { return type_ == Num; }
    explicit operator bool() const { return type_ != Null; }

    // readers never throw: a missing key or a wrong type gives the fallback
    double num(double fallback = 0) const { return type_ == Num ? n_ : type_ == Bool ? double(b_) : fallback; }
    int64_t integer(int64_t fallback = 0) const { return type_ == Num ? int64_t(std::llround(n_)) : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Bool ? b_ : type_ == Num ? n_ != 0 : fallback; }
    const std::string& str() const { static const std::string empty; return type_ == Str ? s_ : empty; }
    std::string str(const std::string& fallback) const { return type_ == Str ? s_ : fallback; }
    size_t size() const { return type_ == Arr ? a_.size() : type_ == Obj ? o_.size() : 0; }
    const Value& operator[](size_t i) const { return type_ == Arr && i < a_.size() ? a_[i] : null(); }
    const Value& operator[](std::string_view key) const {
        if (type_ == Obj) for (auto& p : o_) if (p.first == key) return p.second;
        return null();
    }
    const Value& operator[](const char* key) const { return (*this)[std::string_view(key)]; }
    bool has(std::string_view key) const { return !(*this)[key].is_null(); }
    const std::vector<Value>& items() const { static const std::vector<Value> none; return type_ == Arr ? a_ : none; }
    const std::vector<std::pair<std::string, Value>>& members() const { static const std::vector<std::pair<std::string, Value>> none; return type_ == Obj ? o_ : none; }

    Value& set(std::string key, Value v) {
        if (type_ != Obj) { *this = object(); }
        for (auto& p : o_) if (p.first == key) { p.second = std::move(v); return *this; }
        o_.emplace_back(std::move(key), std::move(v));
        return *this;
    }
    Value& push(Value v) { if (type_ != Arr) *this = array(); a_.push_back(std::move(v)); return *this; }
    // moves a member out (it becomes null): for large strings that should not be copied
    Value take(std::string_view key) {
        if (type_ == Obj) for (auto& p : o_) if (p.first == key) return std::exchange(p.second, Value());
        return Value();
    }
    std::string take_str() { return type_ == Str ? std::move(s_) : std::string(); }

    std::string dump(int indent = -1) const { std::string out; write(out, indent, 0); return out; }
    static Value parse(std::string_view text) { Parser p{text, 0}; Value v = p.value(); p.space(); if (p.i != text.size()) p.fail("trailing characters"); return v; }
    static void quote(std::string& out, std::string_view s) {
        out += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                    else out += char(c);
            }
        }
        out += '"';
    }

  private:
    static const Value& null() { static const Value v; return v; }
    void write(std::string& out, int indent, int depth) const {
        auto nl = [&](int d) { if (indent >= 0) { out += '\n'; out.append(size_t(indent) * d, ' '); } };
        switch (type_) {
            case Null: out += "null"; break;
            case Bool: out += b_ ? "true" : "false"; break;
            case Num: {
                if (!std::isfinite(n_)) { out += "null"; break; }
                char b[32];
                if (n_ == std::floor(n_) && std::fabs(n_) < 9e15) snprintf(b, sizeof b, "%lld", (long long)n_);
                else for (int digits = 6; digits <= 17; digits++) { snprintf(b, sizeof b, "%.*g", digits, n_); if (std::strtod(b, nullptr) == n_) break; }
                out += b;
                break;
            }
            case Str: quote(out, s_); break;
            case Arr:
                out += '[';
                for (size_t i = 0; i < a_.size(); i++) { if (i) out += ','; nl(depth + 1); a_[i].write(out, indent, depth + 1); }
                if (!a_.empty()) nl(depth);
                out += ']';
                break;
            case Obj:
                out += '{';
                for (size_t i = 0; i < o_.size(); i++) {
                    if (i) out += ',';
                    nl(depth + 1); quote(out, o_[i].first); out += indent >= 0 ? ": " : ":"; o_[i].second.write(out, indent, depth + 1);
                }
                if (!o_.empty()) nl(depth);
                out += '}';
                break;
        }
    }
    struct Parser {
        std::string_view t; size_t i;
        [[noreturn]] void fail(const char* what) const { throw std::runtime_error(std::string("JSON: ") + what + " at offset " + std::to_string(i)); }
        void space() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\t' || t[i] == '\r')) i++; }
        bool eat(std::string_view w) { if (t.substr(i, w.size()) == w) { i += w.size(); return true; } return false; }
        Value value() {
            space();
            if (i >= t.size()) fail("unexpected end");
            char c = t[i];
            if (c == '{') {
                Value v = object(); i++; space();
                if (i < t.size() && t[i] == '}') { i++; return v; }
                for (;;) {
                    space(); if (i >= t.size() || t[i] != '"') fail("expected a key");
                    std::string k = string(); space();
                    if (i >= t.size() || t[i] != ':') fail("expected ':'");
                    i++; v.o_.emplace_back(std::move(k), value()); space();
                    if (i < t.size() && t[i] == ',') { i++; continue; }
                    if (i < t.size() && t[i] == '}') { i++; return v; }
                    fail("expected ',' or '}'");
                }
            }
            if (c == '[') {
                Value v = array(); i++; space();
                if (i < t.size() && t[i] == ']') { i++; return v; }
                for (;;) {
                    v.a_.push_back(value()); space();
                    if (i < t.size() && t[i] == ',') { i++; continue; }
                    if (i < t.size() && t[i] == ']') { i++; return v; }
                    fail("expected ',' or ']'");
                }
            }
            if (c == '"') return Value(string());
            if (eat("true")) return Value(true);
            if (eat("false")) return Value(false);
            if (eat("null")) return Value();
            size_t j = i;
            while (j < t.size() && (std::isdigit((unsigned char)t[j]) || t[j] == '-' || t[j] == '+' || t[j] == '.' || t[j] == 'e' || t[j] == 'E')) j++;
            if (j == i) fail("unexpected character");
            std::string num(t.substr(i, j - i)); i = j;
            return Value(std::strtod(num.c_str(), nullptr));
        }
        static void utf8(std::string& s, uint32_t cp) {
            if (cp < 0x80) s += char(cp);
            else if (cp < 0x800) { s += char(0xC0 | (cp >> 6)); s += char(0x80 | (cp & 63)); }
            else if (cp < 0x10000) { s += char(0xE0 | (cp >> 12)); s += char(0x80 | ((cp >> 6) & 63)); s += char(0x80 | (cp & 63)); }
            else { s += char(0xF0 | (cp >> 18)); s += char(0x80 | ((cp >> 12) & 63)); s += char(0x80 | ((cp >> 6) & 63)); s += char(0x80 | (cp & 63)); }
        }
        uint32_t hex4() {
            if (i + 4 > t.size()) fail("bad \\u escape");
            uint32_t v = 0;
            for (int k = 0; k < 4; k++) { char c = t[i++]; v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10); }
            return v;
        }
        std::string string() {
            i++;                                              // the opening quote
            std::string s;
            for (;;) {
                size_t j = i;
                while (j < t.size() && t[j] != '"' && t[j] != '\\') j++;   // long runs (base64 pictures) are copied in one piece
                s.append(t.data() + i, j - i); i = j;
                if (i >= t.size()) fail("unterminated string");
                if (t[i] == '"') { i++; return s; }
                i++; if (i >= t.size()) fail("unterminated string");
                char c = t[i++];
                switch (c) {
                    case 'n': s += '\n'; break; case 't': s += '\t'; break; case 'r': s += '\r'; break;
                    case 'b': s += '\b'; break; case 'f': s += '\f'; break;
                    case 'u': {
                        uint32_t cp = hex4();
                        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t.size() && t[i] == '\\' && t[i + 1] == 'u') { i += 2; cp = 0x10000 + ((cp - 0xD800) << 10) + (hex4() - 0xDC00); }
                        utf8(s, cp); break;
                    }
                    default: s += c;
                }
            }
        }
    };
    Type type_ = Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> a_;
    std::vector<std::pair<std::string, Value>> o_;
};

}  // namespace kc::json
