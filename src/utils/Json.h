#pragma once

// Minimal JSON value model used for serializing adjustment stacks and
// presets. Qt-free by design (the catalog and engine layers may not include
// Qt headers). Parsing is strict; callers receive nullopt on malformed input.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lrc::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int n) : type_(Type::Number), num_(static_cast<double>(n)) {}
    Value(double n) : type_(Type::Number), num_(n) {}
    Value(const char* s) : type_(Type::String), str_(s) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}
    explicit Value(Type t) : type_(t) {}

    static Value object() { return Value(Type::Object); }
    static Value array() { return Value(Type::Array); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const {
        return isBool() ? bool_ : def;
    }
    double asNumber(double def = 0.0) const {
        return isNumber() ? num_ : def;
    }
    int asInt(int def = 0) const {
        return isNumber() ? static_cast<int>(num_) : def;
    }
    const std::string& asString() const { return str_; }

    // Object access. The non-const operator[] inserts an empty value.
    Value& operator[](const std::string& key);
    const Value& operator[](const std::string& key) const;
    bool contains(const std::string& key) const;
    size_t size() const;

    // Array access.
    void push_back(Value v);
    Value& operator[](size_t index);
    const Value& operator[](size_t index) const;

    const std::map<std::string, Value>& members() const { return obj_; }
    const std::vector<Value>& items() const { return arr_; }

    std::string serialize(bool pretty = false) const;

    // Returns nullopt on parse failure. The root value is always an object
    // or an array.
    static std::optional<Value> parse(const std::string& text);

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::map<std::string, Value> obj_;
};

} // namespace lrc::json
