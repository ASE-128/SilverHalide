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
}

TEST_CASE("Json: exponent and negative numbers", "[json]") {
    auto v = Value::parse("-1.5e2");
    REQUIRE(v.has_value());
    REQUIRE(v->asNumber() == Approx(-150.0));
}
