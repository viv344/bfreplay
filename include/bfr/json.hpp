// Minimal, allocation-free (after warm-up) JSON parser for Betfair stream messages.
//
// Parses one line into a flat arena of nodes that point back into the input buffer
// (string_view), so no string is copied. The arena is reused line after line, so in
// steady state parsing allocates nothing. Supports the full JSON grammar plus the bare
// NaN / Infinity / -Infinity tokens Python's json module accepts.
#pragma once
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace bfr::json {

enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

struct Node {
    Type type = Type::Null;
    bool boolean = false;
    uint32_t first_child = 0;   // 0 = none (index 0 is always the root)
    uint32_t next_sibling = 0;  // 0 = none
    double number = 0.0;
    std::string_view key;       // set when this node is an object member
    std::string_view str;       // raw (still-escaped) string contents
};

class Document;

class Value {
public:
    Value() = default;
    Value(const Document* d, uint32_t i) : doc_(d), idx_(i) {}
    explicit operator bool() const { return doc_ != nullptr; }
    Type type() const;
    bool is_null() const { return !doc_ || type() == Type::Null; }
    double number(double fallback = std::numeric_limits<double>::quiet_NaN()) const;
    std::string_view str() const;
    bool boolean() const;
    Value operator[](std::string_view key) const;   // object lookup (linear; objects are small)

    class Iterator {
    public:
        Iterator(const Document* d, uint32_t i) : doc_(d), idx_(i) {}
        Value operator*() const { return {doc_, idx_}; }
        Iterator& operator++();
        bool operator!=(const Iterator& o) const { return idx_ != o.idx_; }
    private:
        const Document* doc_;
        uint32_t idx_;
    };
    Iterator begin() const;
    Iterator end() const { return {doc_, 0}; }
    std::string_view key() const;

private:
    const Document* doc_ = nullptr;
    uint32_t idx_ = 0;
};

class Document {
public:
    // Returns false on malformed input (the caller skips the line).
    bool parse(std::string_view text) {
        nodes_.clear();
        p_ = text.data();
        end_ = text.data() + text.size();
        nodes_.emplace_back();
        skip_ws();
        if (!parse_value(0)) return false;
        skip_ws();
        return p_ == end_;
    }
    Value root() const { return nodes_.empty() ? Value{} : Value{this, 0}; }
    const Node& node(uint32_t i) const { return nodes_[i]; }
    size_t size() const { return nodes_.size(); }

private:
    std::vector<Node> nodes_;
    const char* p_ = nullptr;
    const char* end_ = nullptr;

    void skip_ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) ++p_;
    }
    bool literal(std::string_view lit) {
        if (static_cast<size_t>(end_ - p_) < lit.size() || std::string_view(p_, lit.size()) != lit) return false;
        p_ += lit.size();
        return true;
    }
    bool parse_string(std::string_view& out) {
        if (p_ >= end_ || *p_ != '"') return false;
        const char* start = ++p_;
        while (p_ < end_) {
            char c = *p_;
            if (c == '\\') { p_ += 2; continue; }
            if (c == '"') { out = std::string_view(start, p_ - start); ++p_; return true; }
            ++p_;
        }
        return false;
    }
    bool parse_number(double& out) {
        if (literal("NaN")) { out = std::numeric_limits<double>::quiet_NaN(); return true; }
        if (literal("Infinity")) { out = std::numeric_limits<double>::infinity(); return true; }
        if (literal("-Infinity")) { out = -std::numeric_limits<double>::infinity(); return true; }
        auto [ptr, ec] = std::from_chars(p_, end_, out);
        if (ec != std::errc()) return false;
        p_ = ptr;
        return true;
    }
    bool parse_value(uint32_t idx) {
        if (p_ >= end_) return false;
        switch (*p_) {
            case '{': return parse_container(idx, Type::Object, '}');
            case '[': return parse_container(idx, Type::Array, ']');
            case '"': {
                std::string_view s;
                if (!parse_string(s)) return false;
                nodes_[idx].type = Type::String;
                nodes_[idx].str = s;
                return true;
            }
            case 't': nodes_[idx].type = Type::Bool; nodes_[idx].boolean = true; return literal("true");
            case 'f': nodes_[idx].type = Type::Bool; nodes_[idx].boolean = false; return literal("false");
            case 'n': nodes_[idx].type = Type::Null; return literal("null");
            default: {
                double d;
                if (!parse_number(d)) return false;
                nodes_[idx].type = Type::Number;
                nodes_[idx].number = d;
                return true;
            }
        }
    }
    bool parse_container(uint32_t idx, Type t, char close) {
        nodes_[idx].type = t;
        ++p_;
        skip_ws();
        if (p_ < end_ && *p_ == close) { ++p_; return true; }
        uint32_t prev = 0;
        while (true) {
            skip_ws();
            std::string_view key;
            if (t == Type::Object) {
                if (!parse_string(key)) return false;
                skip_ws();
                if (p_ >= end_ || *p_ != ':') return false;
                ++p_;
                skip_ws();
            }
            uint32_t child = static_cast<uint32_t>(nodes_.size());
            nodes_.emplace_back();
            nodes_[child].key = key;
            if (prev == 0) nodes_[idx].first_child = child; else nodes_[prev].next_sibling = child;
            prev = child;
            if (!parse_value(child)) return false;      // note: may reallocate nodes_; use indices only
            skip_ws();
            if (p_ >= end_) return false;
            if (*p_ == ',') { ++p_; continue; }
            if (*p_ == close) { ++p_; return true; }
            return false;
        }
    }
};

inline Type Value::type() const { return doc_->node(idx_).type; }
inline std::string_view Value::str() const { return doc_ ? doc_->node(idx_).str : std::string_view{}; }
inline std::string_view Value::key() const { return doc_ ? doc_->node(idx_).key : std::string_view{}; }
inline bool Value::boolean() const { return doc_ && doc_->node(idx_).type == Type::Bool && doc_->node(idx_).boolean; }
inline double Value::number(double fallback) const {
    if (!doc_) return fallback;
    const Node& n = doc_->node(idx_);
    if (n.type == Type::Number) return n.number;
    if (n.type == Type::String) {                       // e.g. "bsp":"Infinity"
        if (n.str == "Infinity") return std::numeric_limits<double>::infinity();
        if (n.str == "-Infinity") return -std::numeric_limits<double>::infinity();
        if (n.str == "NaN") return std::numeric_limits<double>::quiet_NaN();
        double d;
        auto [ptr, ec] = std::from_chars(n.str.data(), n.str.data() + n.str.size(), d);
        if (ec == std::errc() && ptr == n.str.data() + n.str.size()) return d;
    }
    return fallback;
}
inline Value Value::operator[](std::string_view k) const {
    if (!doc_ || type() != Type::Object) return {};
    for (uint32_t c = doc_->node(idx_).first_child; c; c = doc_->node(c).next_sibling)
        if (doc_->node(c).key == k) return {doc_, c};
    return {};
}
inline Value::Iterator Value::begin() const {
    if (!doc_ || (type() != Type::Array && type() != Type::Object)) return {doc_, 0};
    return {doc_, doc_->node(idx_).first_child};
}
inline Value::Iterator& Value::Iterator::operator++() {
    idx_ = doc_->node(idx_).next_sibling;
    return *this;
}

}  // namespace bfr::json
