#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "utils/Json.h"

using namespace Catch;
using namespace lrc::json;

TEST_CASE("Json: scalar parse", "[json]") {
    auto v = Value::parse("42");
    REQUIRE(v.has_value());
    REQUIRE(v->isNumber());
    REQUIRE(v->asInt() == 42);
}

TEST_CASE("Json: object round-trip", "[json]") {
    const std::string text =
        R"({"version":1,"exposure":0.5,"name":"test","ok":true,"tags":["a","b"],"empty":null})";
    auto v = Value::parse(text);
    REQUIRE(v.has_value());
    REQUIRE(v->isObject());
    REQUIRE(v->contains("version"));
    REQUIRE(v->operator[]("version").asInt() == 1);
    REQUIRE(v->operator[]("exposure").asNumber() == Approx(0.5));
    REQUIRE(v->operator[]("name").asString() == "test");
    REQUIRE(v->operator[]("ok").asBool());
    REQUIRE(v->operator[]("tags").isArray());
    REQUIRE(v->operator[]("tags").size() == 2);
    REQUIRE(v->operator[]("empty").isNull());

    const auto parsed = Value::parse(v->serialize(false));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->serialize(false) == v->serialize(false));
}

TEST_CASE("Json: pretty output parses back identically", "[json]") {
    Value v = Value::object();
    v["a"] = 1.5;
    v["b"] = Value::array();
    v["b"].push_back(true);
    v["b"].push_back(Value("x"));
    const auto parsed = Value::parse(v.serialize(true));
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->serialize(false) == v.serialize(false));
}

TEST_CASE("Json: string escapes", "[json]") {
    auto v = Value::parse(R"("a\nb\t\"c\\")");
    REQUIRE(v.has_value());
    REQUIRE(v->asString() == "a\nb\t\"c\\");

    auto uni = Value::parse(R"("中文")");
    REQUIRE(uni.has_value());
    REQUIRE(uni->asString() == "\xe4\xb8\xad\xe6\x96\x87"); // 中文
}

TEST_CASE("Json: rejects malformed input", "[json]") {
    REQUIRE_FALSE(Value::parse("{").has_value());
    REQUIRE_FALSE(Value::parse("").has_value());
    REQUIRE_FALSE(Value::parse("[1,]").has_value());
    REQUIRE_FALSE(Value::parse("{\"a\" 1}").has_value());
    REQUIRE_FALSE(Value::parse("01").has_value());
    REQUIRE_FALSE(Value::parse("\"unterminated").has_value());
    // Additional malformed number / token shapes exercised by the
    // from_chars-based number parser.
    REQUIRE_FALSE(Value::parse("1.").has_value());
    REQUIRE_FALSE(Value::parse(".5").has_value());
    REQUIRE_FALSE(Value::parse("-").has_value());
    REQUIRE_FALSE(Value::parse("1e").has_value());
    REQUIRE_FALSE(Value::parse("1e+").has_value());
    REQUIRE_FALSE(Value::parse("+1").has_value());
    REQUIRE_FALSE(Value::parse("1.2.3").has_value());
    REQUIRE_FALSE(Value::parse("truex").has_value());
    REQUIRE_FALSE(Value::parse("42 43").has_value());
}

TEST_CASE("Json: exponent and negative numbers", "[json]") {
    auto v = Value::parse("-1.5e2");
    REQUIRE(v.has_value());
    REQUIRE(v->asNumber() == Approx(-150.0));
}

TEST_CASE("Json: variant type predicates are exclusive", "[json]") {
    const Value null;
    const Value boolean(true);
    const Value integral(7);
    const Value floating(2.5);
    const Value text("text");
    const Value arr = Value::array();
    const Value obj = Value::object();

    REQUIRE(null.type() == Value::Type::Null);
    REQUIRE(null.isNull());
    REQUIRE_FALSE(null.isNumber());

    REQUIRE(boolean.type() == Value::Type::Bool);
    REQUIRE(boolean.isBool());
    REQUIRE_FALSE(boolean.isNumber());

    REQUIRE(integral.type() == Value::Type::Number);
    REQUIRE(integral.isNumber());
    REQUIRE(integral.isInt());
    REQUIRE_FALSE(floating.isInt());
    REQUIRE(floating.isNumber());

    REQUIRE(text.type() == Value::Type::String);
    REQUIRE(text.isString());
    REQUIRE_FALSE(text.isNumber());

    REQUIRE(arr.type() == Value::Type::Array);
    REQUIRE(arr.isArray());

    REQUIRE(obj.type() == Value::Type::Object);
    REQUIRE(obj.isObject());

    // Type::Number keeps the historical floating-point zero (isInt() false).
    const Value numberPlaceholder(Value::Type::Number);
    REQUIRE(numberPlaceholder.isNumber());
    REQUIRE_FALSE(numberPlaceholder.isInt());
    REQUIRE(numberPlaceholder.asNumber() == 0.0);
}

TEST_CASE("Json: integer precision beyond 2^53", "[json]") {
    // 2^53 + 1 is not exactly representable as a double; the integer leg of
    // the variant must preserve it across a textual round trip.
    const long long id = 9007199254740993LL;
    Value v = Value::object();
    v["id"] = Value(id);
    v["nested"] = Value::array();
    v["nested"].push_back(Value(-9223372036854775807LL - 1));

    const auto round = Value::parse(v.serialize(false));
    REQUIRE(round.has_value());
    REQUIRE(round->operator[]("id").isInt());
    REQUIRE(round->operator[]("id").asInt64() == id);
    REQUIRE(round->operator[]("nested")[0].asInt64() ==
            -9223372036854775807LL - 1);
    // asNumber still exposes the (lossy) double view for legacy callers.
    REQUIRE(round->operator[]("id").asNumber() == static_cast<double>(id));
}

TEST_CASE("Json: integers too large for long long fall back to double", "[json]") {
    const auto big = Value::parse("9223372036854775808"); // 2^63
    REQUIRE(big.has_value());
    REQUIRE(big->isNumber());
    REQUIRE_FALSE(big->isInt());

    const auto huge = Value::parse("123456789012345678901234567890");
    REQUIRE(huge.has_value());
    REQUIRE(huge->isNumber());
    REQUIRE_FALSE(huge->isInt());
    REQUIRE(huge->asNumber() > 1e29);
}

TEST_CASE("Json: float numbers truncate when read as integers", "[json]") {
    const auto v = Value::parse("-2.75");
    REQUIRE(v.has_value());
    REQUIRE(v->isNumber());
    REQUIRE_FALSE(v->isInt());
    REQUIRE(v->asInt64() == -2);
    REQUIRE(v->asInt() == -2);
}

TEST_CASE("Json: empty object and array", "[json]") {
    const Value emptyObj = Value::object();
    REQUIRE(emptyObj.isObject());
    REQUIRE(emptyObj.size() == 0);
    REQUIRE(emptyObj.members().empty());
    REQUIRE(emptyObj.items().empty());
    REQUIRE(emptyObj.serialize(false) == "{}");

    const Value emptyArr = Value::array();
    REQUIRE(emptyArr.isArray());
    REQUIRE(emptyArr.size() == 0);
    REQUIRE(emptyArr.items().empty());
    REQUIRE(emptyArr.serialize(false) == "[]");

    const auto parsedObj = Value::parse("{}");
    REQUIRE(parsedObj.has_value());
    REQUIRE(parsedObj->isObject());
    REQUIRE(parsedObj->size() == 0);

    const auto parsedArr = Value::parse("[]");
    REQUIRE(parsedArr.has_value());
    REQUIRE(parsedArr->isArray());
    REQUIRE(parsedArr->size() == 0);
}

TEST_CASE("Json: non-matching accessors fall back", "[json]") {
    const Value num(3);
    REQUIRE(num.asString().empty());
    REQUIRE(num.asBool(true));
    REQUIRE(num.asNumber() == 3.0);
    REQUIRE(num.asInt(9) == 3);
    REQUIRE_FALSE(num.contains("missing"));
    REQUIRE(num["missing"].isNull());
    REQUIRE(num.members().empty());
    REQUIRE(num.items().empty());
    REQUIRE(num.size() == 0);

    const Value text("abc");
    REQUIRE(text.size() == 3);
    REQUIRE(text.asInt(11) == 11);
    REQUIRE(text.asNumber(0.5) == Approx(0.5));
}

TEST_CASE("Json: object/array write access grows on demand", "[json]") {
    Value arr = Value::array();
    arr[2] = Value("x");
    REQUIRE(arr.size() == 3);
    REQUIRE(arr[0].isNull());
    REQUIRE(arr[2].asString() == "x");

    Value obj;
    obj["a"] = 1;
    REQUIRE(obj.isObject());
    REQUIRE(obj.contains("a"));
    REQUIRE(obj["a"].asInt() == 1);
}

TEST_CASE("Json: string_view and literal keys", "[json]") {
    auto parsed = Value::parse(R"({"alpha":1,"beta":[1,2],"gamma":{"x":true}})");
    REQUIRE(parsed.has_value());
    Value& v = *parsed;

    const std::string key = "alpha";
    const std::string_view keyView = key;
    REQUIRE(v.contains(key));
    REQUIRE(v.contains(keyView));
    REQUIRE(v.contains(std::string_view("alpha")));
    REQUIRE_FALSE(v.contains(std::string_view("missing")));

    REQUIRE(v[keyView].asInt() == 1);
    REQUIRE(v[std::string_view("beta")].size() == 2);
    REQUIRE(v["gamma"]["x"].asBool());

    // Existing-key lookup through the non-const overload must not insert.
    REQUIRE(v.size() == 3);
    // A missing key still inserts, exactly like the old std::string overload.
    v[std::string_view("delta")] = 4;
    REQUIRE(v.size() == 4);
    REQUIRE(v["delta"].asInt() == 4);

    const Value& cv = v;
    REQUIRE(cv[std::string_view("missing")].isNull());
    REQUIRE(cv.members().size() == 4);
}

TEST_CASE("Json: inserted keys are owned copies", "[json]") {
    Value v = Value::object();
    {
        std::string temporary = "temporary-key";
        v[std::string_view(temporary)] = 1;
    } // temporary is destroyed here; the object must own its key.
    REQUIRE(v.contains("temporary-key"));
    REQUIRE(v["temporary-key"].asInt() == 1);
}

TEST_CASE("Json: parse borrows the input for the call only", "[json]") {
    // Temporary std::string and string_view sources are both fine: the decoded
    // Value owns its own strings and never points back into the input.
    const auto fromView = Value::parse(std::string_view("{\"k\":7}"));
    REQUIRE(fromView.has_value());
    REQUIRE(fromView->operator[]("k").asInt() == 7);

    const auto fromTemporary = Value::parse(std::string("{\"k\":8}"));
    REQUIRE(fromTemporary.has_value());
    REQUIRE(fromTemporary->operator[]("k").asInt() == 8);

    std::string buffer = "{\"k\":9}";
    const auto fromBuffer = Value::parse(buffer);
    buffer.assign("garbage");
    REQUIRE(fromBuffer.has_value());
    REQUIRE(fromBuffer->operator[]("k").asInt() == 9);
}
