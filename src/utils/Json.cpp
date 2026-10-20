#include "utils/Json.h"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace lrc::json {

Value& Value::operator[](std::string_view key) {
    if (!isObject())
        data_ = Object();
    auto& obj = std::get<Object>(data_);
    // Look the key up first so an existing entry costs no allocation; the key
    // is only copied out of the borrowed view when it must be inserted.
    const auto it = obj.find(key);
    if (it != obj.end())
        return it->second;
    return obj.emplace(std::string(key), Value()).first->second;
}

const Value& Value::operator[](std::string_view key) const {
    static const Value kNull;
    if (const auto* obj = std::get_if<Object>(&data_)) {
        // Heterogeneous lookup through the transparent comparator: no
        // temporary std::string for literal or string_view keys.
        const auto it = obj->find(key);
        return it == obj->end() ? kNull : it->second;
    }
    return kNull;
}

bool Value::contains(std::string_view key) const noexcept {
    if (const auto* obj = std::get_if<Object>(&data_))
        return obj->find(key) != obj->end();
    return false;
}

size_t Value::size() const noexcept {
    if (const auto* str = std::get_if<std::string>(&data_))
        return str->size();
    if (const auto* arr = std::get_if<Array>(&data_))
        return arr->size();
    if (const auto* obj = std::get_if<Object>(&data_))
        return obj->size();
    return 0;
}

void Value::push_back(Value v) {
    if (!isArray())
        data_ = Array();
    std::get<Array>(data_).push_back(std::move(v));
}

Value& Value::operator[](size_t index) {
    // Grows on demand, exactly like the object overload inserts a key. Growing
    // (instead of returning a shared mutable Null) removes the global mutable
    // state that made an out-of-range write a data race across threads.
    if (!isArray())
        data_ = Array();
    auto& arr = std::get<Array>(data_);
    if (index >= arr.size())
        arr.resize(index + 1);
    return arr[index];
}

const Value& Value::operator[](size_t index) const {
    static const Value kNull;
    if (const auto* arr = std::get_if<Array>(&data_)) {
        if (index < arr->size())
            return (*arr)[index];
    }
    return kNull;
}

namespace {

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

// Parser over a non-owning view of the source text. The view must stay valid
// for the whole parse call; Value::parse guarantees that by borrowing the
// caller's std::string for the duration of the call only.
class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    /** @brief Parses a whole document; trailing non-space input is an error. */
    std::optional<Value> parseRoot() {
        skipWs();
        auto v = parseValue();
        if (!v)
            return std::nullopt;
        skipWs();
        if (pos_ != text_.size())
            return std::nullopt;
        return v;
    }

private:
    std::string_view text_;
    size_t pos_ = 0;

    void skipWs() {
        while (pos_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[pos_])))
            ++pos_;
    }

    bool consume(char c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    std::optional<Value> parseValue() {
        skipWs();
        if (pos_ >= text_.size())
            return std::nullopt;
        const char c = text_[pos_];
        if (c == '{')
            return parseObject();
        if (c == '[')
            return parseArray();
        if (c == '"')
            return parseString();
        if (c == 't' || c == 'f')
            return parseBool();
        if (c == 'n')
            return parseNull();
        return parseNumber();
    }

    std::optional<Value> parseObject() {
        consume('{');
        skipWs();
        Value obj(Value::Type::Object);
        if (consume('}'))
            return obj;
        for (;;) {
            skipWs();
            auto key = parseString();
            if (!key)
                return std::nullopt;
            skipWs();
            if (!consume(':'))
                return std::nullopt;
            auto val = parseValue();
            if (!val)
                return std::nullopt;
            obj[key->asString()] = std::move(*val);
            skipWs();
            if (consume(','))
                continue;
            if (consume('}'))
                return obj;
            return std::nullopt;
        }
    }

    std::optional<Value> parseArray() {
        consume('[');
        skipWs();
        Value arr(Value::Type::Array);
        if (consume(']'))
            return arr;
        for (;;) {
            auto v = parseValue();
            if (!v)
                return std::nullopt;
            arr.push_back(std::move(*v));
            skipWs();
            if (consume(','))
                continue;
            if (consume(']'))
                return arr;
            return std::nullopt;
        }
    }

    std::optional<Value> parseString() {
        if (!consume('"'))
            return std::nullopt;
        std::string out;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"')
                return Value(std::move(out));
            if (static_cast<unsigned char>(c) < 0x20)
                return std::nullopt; // raw control characters are invalid
            if (c == '\\') {
                if (pos_ >= text_.size())
                    return std::nullopt;
                const char e = text_[pos_++];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (!appendUtf16Escaped(out))
                        return std::nullopt;
                    break;
                }
                default: return std::nullopt;
                }
            } else {
                out += c;
            }
        }
        return std::nullopt;
    }

    // Reads \uXXXX (and a following low surrogate), appends UTF-8.
    bool appendUtf16Escaped(std::string& out) {
        uint32_t cp = 0;
        if (!readHex4(cp))
            return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // High surrogate: require a low surrogate immediately.
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' ||
                text_[pos_ + 1] != 'u')
                return false;
            pos_ += 2;
            uint32_t low = 0;
            if (!readHex4(low) || low < 0xDC00 || low > 0xDFFF)
                return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return false; // lone low surrogate
        }
        appendUtf8(out, cp);
        return true;
    }

    bool readHex4(uint32_t& out) {
        if (pos_ + 4 > text_.size())
            return false;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[pos_ + i];
            int d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return false;
            v = (v << 4) | static_cast<uint32_t>(d);
        }
        pos_ += 4;
        out = v;
        return true;
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    std::optional<Value> parseBool() {
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return Value(true);
        }
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return Value(false);
        }
        return std::nullopt;
    }

    std::optional<Value> parseNull() {
        if (text_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return Value(nullptr);
        }
        return std::nullopt;
    }

    std::optional<Value> parseNumber() {
        const size_t start = pos_;
        if (consume('-')) { /* negative */ }
        if (pos_ >= text_.size())
            return std::nullopt;
        // Integer part.
        if (consume('0')) {
            // A leading zero must not be followed by another digit.
            if (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
                return std::nullopt;
        } else {
            if (!std::isdigit(static_cast<unsigned char>(text_[pos_])))
                return std::nullopt;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        }
        // Fraction.
        if (consume('.')) {
            if (pos_ >= text_.size() ||
                !std::isdigit(static_cast<unsigned char>(text_[pos_])))
                return std::nullopt;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        }
        // Exponent.
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < text_.size() &&
                (text_[pos_] == '+' || text_[pos_] == '-'))
                ++pos_;
            if (pos_ >= text_.size() ||
                !std::isdigit(static_cast<unsigned char>(text_[pos_])))
                return std::nullopt;
            while (pos_ < text_.size() &&
                   std::isdigit(static_cast<unsigned char>(text_[pos_])))
                ++pos_;
        }
        // The token is a validated JSON number, so the conversion below only
        // has to deal with representability, not with malformed input.
        const std::string_view token = text_.substr(start, pos_ - start);
        const char* const first = token.data();
        const char* const last = first + token.size();
        // Integer tokens are kept exact (long long); anything with a fraction
        // or an exponent - and integers too large for long long - goes through
        // the floating-point path, mirroring the previous strtoll/strtod code.
        if (token.find_first_of(".eE") == std::string_view::npos) {
            long long iv = 0;
            const auto intResult = std::from_chars(first, last, iv);
            if (intResult.ec == std::errc{} && intResult.ptr == last)
                return Value(iv);
            if (intResult.ec == std::errc::invalid_argument)
                return std::nullopt;
        }
        double dv = 0.0;
        const auto fpResult = std::from_chars(first, last, dv);
        if (fpResult.ec == std::errc{} && fpResult.ptr == last)
            return Value(dv);
        if (fpResult.ec == std::errc::result_out_of_range) {
            // The previous implementation accepted magnitudes outside the
            // double range as +/-inf (strtod returns HUGE_VAL); preserve that
            // behavior. std::from_chars leaves the destination unmodified on
            // range errors, so fall back to the C conversion for this rare
            // path only.
            const std::string copy(token);
            char* end = nullptr;
            const double value = std::strtod(copy.c_str(), &end);
            if (end && *end == '\0')
                return Value(value);
        }
        return std::nullopt;
    }
};

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

void appendEscapedString(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                // Match the historical stream output: \u followed by four
                // lowercase hex digits (the high two are always 0 here).
                static constexpr char kHex[] = "0123456789abcdef";
                const auto v = static_cast<unsigned char>(c);
                out += "\\u00";
                out += kHex[(v >> 4) & 0x0F];
                out += kHex[v & 0x0F];
            } else {
                out += c;
            }
            break;
        }
    }
    out += '"';
}

void appendNumber(std::string& out, const Value& v) {
    if (v.isInt()) {
        char buf[24];
        const auto result = std::to_chars(buf, buf + sizeof(buf), v.asInt64());
        out.append(buf, static_cast<size_t>(result.ptr - buf));
        return;
    }
    // Floating-point formatting intentionally keeps the historical
    // ostringstream + setprecision(17) form: the serialized bytes are part of
    // the persisted preset / catalog format, and std::to_chars' shortest
    // representation is not byte-compatible with it.
    std::ostringstream os;
    os << std::setprecision(17) << v.asNumber();
    out += os.str();
}

void writeValue(std::string& out, const Value& v, bool pretty, int indent) {
    const auto pad = [&]() {
        if (pretty)
            out.append(static_cast<size_t>(indent) * 2, ' ');
    };
    // A separator is a newline followed by the *current* indent level (not the
    // child level). The exact spacing is part of the persisted format, so it
    // must not be corrected.
    const auto padNl = [&]() {
        if (pretty) {
            out += '\n';
            pad();
        }
    };
    switch (v.type()) {
    case Value::Type::Null:
        out += "null";
        break;
    case Value::Type::Bool:
        out += v.asBool() ? "true" : "false";
        break;
    case Value::Type::Number:
        appendNumber(out, v);
        break;
    case Value::Type::String:
        appendEscapedString(out, v.asString());
        break;
    case Value::Type::Array: {
        out += '[';
        const auto& items = v.items();
        for (size_t i = 0; i < items.size(); ++i) {
            if (i)
                out += ',';
            padNl();
            writeValue(out, items[i], pretty, indent + 1);
        }
        if (pretty && !items.empty())
            out += '\n';
        pad();
        out += ']';
        break;
    }
    case Value::Type::Object: {
        out += '{';
        const auto& members = v.members();
        size_t i = 0;
        for (const auto& [key, val] : members) {
            if (i++)
                out += ',';
            padNl();
            appendEscapedString(out, key);
            out += (pretty ? ": " : ":");
            writeValue(out, val, pretty, indent + 1);
        }
        if (pretty && !members.empty())
            out += '\n';
        pad();
        out += '}';
        break;
    }
    }
}

} // namespace

std::string Value::serialize(bool pretty) const {
    std::string out;
    out.reserve(64);
    writeValue(out, *this, pretty, 0);
    return out;
}

std::optional<Value> Value::parse(std::string_view text) {
    Parser parser(text);
    return parser.parseRoot();
}

} // namespace lrc::json
