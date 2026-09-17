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

/**
 * @brief Minimal JSON value model used for serializing adjustment stacks and
 *        presets.
 *
 * Qt-free by design (the catalog and engine layers may not include Qt headers).
 * Parsing is strict; callers receive nullopt on malformed input. A value is one
 * of Null, Bool, Number, String, Array or Object.
 */
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

    /** @brief Creates an empty object value. */
    static Value object() { return Value(Type::Object); }
    /** @brief Creates an empty array value. */
    static Value array() { return Value(Type::Array); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    /** @brief Reads as bool, returning `def` when not a Bool. */
    bool asBool(bool def = false) const {
        return isBool() ? bool_ : def;
    }
    /** @brief Reads as number, returning `def` when not a Number. */
    double asNumber(double def = 0.0) const {
        return isNumber() ? num_ : def;
    }
    /** @brief Reads as int (from a Number), returning `def` otherwise. */
    int asInt(int def = 0) const {
        return isNumber() ? static_cast<int>(num_) : def;
    }
    /** @brief Returns the string payload (empty when not a String). */
    const std::string& asString() const { return str_; }

    // Object access. The non-const operator[] inserts an empty value.
    /** @brief Object element access (non-const); inserts an empty value and
     *         converts to an Object when not already one. */
    Value& operator[](const std::string& key);
    /** @brief Object element access (const); returns a shared Null value for a
     *         missing key or when not an Object. */
    const Value& operator[](const std::string& key) const;
    /** @brief True if this is an Object containing `key`. */
    bool contains(const std::string& key) const;
    /** @brief Element count: array/object size, or string length; 0 otherwise. */
    size_t size() const;

    // Array access.
    /** @brief Appends an element, converting to an Array when not already one. */
    void push_back(Value v);
    /** @brief Array element access (non-const); returns a shared Null when out
     *         of range or not an Array. */
    Value& operator[](size_t index);
    /** @brief Array element access (const); returns a shared Null when out of
     *         range or not an Array. */
    const Value& operator[](size_t index) const;

    const std::map<std::string, Value>& members() const { return obj_; }
    const std::vector<Value>& items() const { return arr_; }

    /** @brief Serializes this value to a JSON string.
     * @param pretty Emit indented output when true.
     */
    std::string serialize(bool pretty = false) const;

    /** @brief Parses a JSON document.
     * @param text Source text.
     * @return The decoded value, or nullopt on any parse failure. The root
     *         value is always an Object or an Array.
     */
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
