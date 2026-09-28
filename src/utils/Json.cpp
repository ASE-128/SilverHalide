#include "utils/Json.h"

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>

namespace lrc::json {

Value& Value::operator[](const std::string& key) {
    if (type_ != Type::Object)
        *this = Value(Value::Type::Object);
    return obj_[key];
}

const Value& Value::operator[](const std::string& key) const {
    static const Value kNull;
    if (type_ != Type::Object)
        return kNull;
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
}

bool Value::contains(const std::string& key) const {
    return type_ == Type::Object && obj_.count(key) != 0;
}

size_t Value::size() const {
    switch (type_) {
    case Type::Array: return arr_.size();
    case Type::Object: return obj_.size();
    case Type::String: return str_.size();
    default: return 0;
    }
}

void Value::push_back(Value v) {
    if (type_ != Type::Array)
        *this = Value(Value::Type::Array);
    arr_.push_back(std::move(v));
}

Value& Value::operator[](size_t index) {
    // Grows on demand, exactly like the object overload inserts a key. Growing
    // (instead of returning a shared mutable Null) removes the global mutable
    // state that made an out-of-range write a data race across threads.
    if (type_ != Type::Array)
        *this = Value(Value::Type::Array);
    if (index >= arr_.size())
        arr_.resize(index + 1);
    return arr_[index];
}

const Value& Value::operator[](size_t index) const {
    static const Value kNull;
    if (type_ != Type::Array || index >= arr_.size())
        return kNull;
    return arr_[index];
}

namespace {

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

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
    const std::string& text_;
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
        if (text_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return Value(true);
        }
        if (text_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return Value(false);
        }
        return std::nullopt;
    }

    std::optional<Value> parseNull() {
        if (text_.compare(pos_, 4, "null") == 0) {
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
        const std::string token = text_.substr(start, pos_ - start);
        // Integer tokens are kept exact (long long); anything with a fraction or
        // an exponent goes through strtod. A token too large for long long falls
        // back to double instead of losing the value.
        if (token.find_first_of(".eE") == std::string::npos) {
            errno = 0;
            char* intEnd = nullptr;
            const long long iv = std::strtoll(token.c_str(), &intEnd, 10);
            if (intEnd && *intEnd == '\0' && errno != ERANGE)
                return Value(iv);
        }
        char* end = nullptr;
        const double v = std::strtod(token.c_str(), &end);
        if (!end || *end != '\0')
            return std::nullopt;
        return Value(v);
    }
};

// ---------------------------------------------------------------------------
// Serializer
// ---------------------------------------------------------------------------

void escapeString(std::ostream& os, const std::string& s) {
    os << '"';
    for (char c : s) {
        switch (c) {
        case '"': os << "\\\""; break;
        case '\\': os << "\\\\"; break;
        case '\b': os << "\\b"; break;
        case '\f': os << "\\f"; break;
        case '\n': os << "\\n"; break;
        case '\r': os << "\\r"; break;
        case '\t': os << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                   << static_cast<unsigned>(static_cast<unsigned char>(c))
                   << std::dec << std::setfill(' ');
            else
                os << c;
        }
    }
    os << '"';
}

void writeValue(std::ostream& os, const Value& v, bool pretty, int indent) {
    const auto pad = [&]() {
        if (pretty)
            os << std::string(static_cast<size_t>(indent) * 2, ' ');
    };
    const auto padNl = [&](int delta) {
        if (pretty) {
            os << '\n';
            pad();
        }
    };
    switch (v.type()) {
    case Value::Type::Null:
        os << "null";
        break;
    case Value::Type::Bool:
        os << (v.asBool() ? "true" : "false");
        break;
    case Value::Type::Number: {
        if (v.isInt())
            os << v.asInt64();
        else
            os << std::setprecision(17) << v.asNumber();
        break;
    }
    case Value::Type::String:
        escapeString(os, v.asString());
        break;
    case Value::Type::Array: {
        os << '[';
        const auto& items = v.items();
        for (size_t i = 0; i < items.size(); ++i) {
            if (i)
                os << ',';
            padNl(indent + 1);
            writeValue(os, items[i], pretty, indent + 1);
        }
        if (pretty && !items.empty())
            os << '\n';
        pad();
        os << ']';
        break;
    }
    case Value::Type::Object: {
        os << '{';
        const auto& members = v.members();
        size_t i = 0;
        for (const auto& [key, val] : members) {
            if (i++)
                os << ',';
            padNl(indent + 1);
            escapeString(os, key);
            os << (pretty ? ": " : ":");
            writeValue(os, val, pretty, indent + 1);
        }
        if (pretty && !members.empty())
            os << '\n';
        pad();
        os << '}';
        break;
    }
    }
}

} // namespace

std::string Value::serialize(bool pretty) const {
    std::ostringstream os;
    writeValue(os, *this, pretty, 0);
    return os.str();
}

std::optional<Value> Value::parse(const std::string& text) {
    Parser parser(text);
    return parser.parseRoot();
}

} // namespace lrc::json
