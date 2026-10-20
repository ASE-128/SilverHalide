#pragma once

// Minimal JSON value model used for serializing adjustment stacks and
// presets. Qt-free by design (the catalog and engine layers may not include
// Qt headers). Parsing is strict; callers receive nullopt on malformed input.

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace lrc::json {

/**
 * @brief Minimal JSON value model used for serializing adjustment stacks and
 *        presets.
 *
 * Qt-free by design (the catalog and engine layers may not include Qt headers).
 * Parsing is strict; callers receive nullopt on malformed input. A value is one
 * of Null, Bool, Number, String, Array or Object.
 *
 * @note The payload lives in a std::variant so a scalar only carries the
 *       storage it needs instead of the three container members the previous
 *       "all members resident" layout had. Integers are kept as long long so
 *       ids and counters beyond 2^53 survive a round trip; every value that
 *       is not an integer is stored as double.
 *
 * @note Values are ordinary value types: copies are deep, and accessors that
 *       return references (asString / operator[] / members / items) borrow
 *       from the value and are invalidated by the next mutation.
 *
 * @note This type is not synchronized; concurrent mutation of the same Value
 *       is a data race. Read-only access from multiple threads is safe.
 */
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    /** @brief Object container type.
     *
     * Ordered by key (so serialization is stable across runs). The transparent
     * std::less<> comparator lets lookups take a string literal or a
     * std::string_view without constructing a temporary std::string.
     */
    using Object = std::map<std::string, Value, std::less<>>;
    /** @brief Array container type. */
    using Array = std::vector<Value>;

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : data_(b) {}
    Value(int n) : data_(static_cast<long long>(n)) {}
    // Integral values are stored as long long so ids and counters beyond 2^53 do
    // not lose precision on a round trip; asNumber() still exposes them as double.
    Value(long long n) : data_(n) {}
    Value(double n) : data_(n) {}
    Value(const char* s) : data_(std::string(s)) {}
    Value(std::string s) : data_(std::move(s)) {}

    /**
     * @brief Creates an empty value of the requested Type.
     *
     * @note Type::Number yields a floating-point zero (isInt() == false),
     *       matching the historical default-constructed Number payload.
     */
    explicit Value(Type t) {
        switch (t) {
        case Type::Null:
            break; // monostate is already the default
        case Type::Bool:
            data_ = false;
            break;
        case Type::Number:
            data_ = 0.0;
            break;
        case Type::String:
            data_ = std::string();
            break;
        case Type::Array:
            data_ = Array();
            break;
        case Type::Object:
            data_ = Object();
            break;
        }
    }

    /** @brief Creates an empty object value. */
    static Value object() { return Value(Type::Object); }
    /** @brief Creates an empty array value. */
    static Value array() { return Value(Type::Array); }

    Type type() const {
        return std::visit(
            [](const auto& payload) -> Type {
                using T = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                    return Type::Null;
                else if constexpr (std::is_same_v<T, bool>)
                    return Type::Bool;
                else if constexpr (std::is_same_v<T, long long> ||
                                   std::is_same_v<T, double>)
                    return Type::Number;
                else if constexpr (std::is_same_v<T, std::string>)
                    return Type::String;
                else if constexpr (std::is_same_v<T, Array>)
                    return Type::Array;
                else
                    return Type::Object;
            },
            data_);
    }
    // Query accessors below are noexcept: they use holds_alternative /
    // get_if, which the standard declares noexcept and which never allocate.
    bool isNull() const noexcept {
        return std::holds_alternative<std::monostate>(data_);
    }
    bool isBool() const noexcept { return std::holds_alternative<bool>(data_); }
    bool isNumber() const noexcept {
        return std::holds_alternative<long long>(data_) ||
               std::holds_alternative<double>(data_);
    }
    bool isString() const noexcept {
        return std::holds_alternative<std::string>(data_);
    }
    bool isArray() const noexcept { return std::holds_alternative<Array>(data_); }
    bool isObject() const noexcept {
        return std::holds_alternative<Object>(data_);
    }

    /** @brief Reads as bool, returning def when not a Bool. */
    bool asBool(bool def = false) const noexcept {
        if (const auto* b = std::get_if<bool>(&data_))
            return *b;
        return def;
    }
    /** @brief True when this Number was created/parsed as an integer. */
    bool isInt() const noexcept {
        return std::holds_alternative<long long>(data_);
    }
    /** @brief Reads as number, returning def when not a Number. */
    double asNumber(double def = 0.0) const noexcept {
        if (const auto* i = std::get_if<long long>(&data_))
            return static_cast<double>(*i);
        if (const auto* d = std::get_if<double>(&data_))
            return *d;
        return def;
    }
    /**
     * @brief Reads as a 64-bit integer, returning def otherwise.
     *
     * @note A Number that was parsed as a float is truncated toward zero.
     */
    long long asInt64(long long def = 0) const noexcept {
        if (const auto* i = std::get_if<long long>(&data_))
            return *i;
        if (const auto* d = std::get_if<double>(&data_))
            return static_cast<long long>(*d);
        return def;
    }
    /** @brief Reads as int (from a Number), returning def otherwise. */
    int asInt(int def = 0) const noexcept {
        return isNumber() ? static_cast<int>(asInt64()) : def;
    }
    /** @brief Returns the string payload (empty when not a String). */
    const std::string& asString() const noexcept {
        if (const auto* s = std::get_if<std::string>(&data_))
            return *s;
        static const std::string kEmpty;
        return kEmpty;
    }

    // Object access. The non-const operator[] inserts an empty value.
    /** @brief Object element access (non-const); inserts an empty value and
     *         converts to an Object when not already one.
     *
     * @param key Borrowed for the duration of the call. An existing key is
     *            looked up without allocating; the key is copied into the
     *            object only when it has to be inserted, so the view may
     *            safely point to a temporary.
     */
    Value& operator[](std::string_view key);
    /** @brief Object element access (const); returns a shared Null value for a
     *         missing key or when not an Object.
     *
     * @param key Borrowed for the duration of the call only.
     */
    const Value& operator[](std::string_view key) const;
    /** @brief True if this is an Object containing key.
     *
     * @param key Borrowed for the duration of the call only.
     */
    bool contains(std::string_view key) const noexcept;
    /** @brief Element count: array/object size, or string length; 0 otherwise. */
    size_t size() const noexcept;

    // Array access.
    /** @brief Appends an element, converting to an Array when not already one. */
    void push_back(Value v);
    /** @brief Array element access (non-const); returns a shared Null when out
     *         of range or not an Array. */
    Value& operator[](size_t index);
    /** @brief Array element access (const); returns a shared Null when out of
     *         range or not an Array. */
    const Value& operator[](size_t index) const;

    /** @brief Object members (empty map when not an Object). */
    const Object& members() const noexcept {
        if (const auto* o = std::get_if<Object>(&data_))
            return *o;
        static const Object kEmpty;
        return kEmpty;
    }
    /** @brief Array items (empty vector when not an Array). */
    const Array& items() const noexcept {
        if (const auto* a = std::get_if<Array>(&data_))
            return *a;
        static const Array kEmpty;
        return kEmpty;
    }

    /** @brief Serializes this value to a JSON string.
     * @param pretty Emit indented output when true.
     *
     * @note The emitted bytes (including the historical indentation and the
     *       setprecision(17) floating-point formatting) are part of the
     *       persisted preset / catalog format and must not change.
     */
    std::string serialize(bool pretty = false) const;

    /** @brief Parses a JSON document.
     * @param text Source text; borrowed for the duration of the call only
     *             (the decoded Value owns nothing that points back into it).
     * @return The decoded value, or nullopt on any parse failure. The root
     *         value is always an Object or an Array.
     */
    [[nodiscard]] static std::optional<Value> parse(std::string_view text);

private:
    // Exactly one alternative is populated:
    //   monostate -> Null, bool -> Bool, long long -> integral Number,
    //   double -> floating-point Number, string -> String,
    //   Array -> Array, Object -> Object.
    using Storage = std::variant<std::monostate, bool, long long, double,
                                 std::string, Array, Object>;
    Storage data_;
};

} // namespace lrc::json
