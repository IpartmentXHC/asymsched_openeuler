#include "asymsched/json.h"
#include "test_framework.h"

#include <string>

TEST(json_roundtrip_preserves_structure) {
    const std::string text =
        R"({"a":[1,2,{"b":"text"}],"c":true,"d":null,"e":1.5})";
    const auto value = asymsched::json::parse(text);
    CHECK(value.is_object());
    CHECK(value.at("c").boolean());
    CHECK(value.at("d").is_null());
    CHECK_EQ(value.at("a").array().size(), 3);
    CHECK_EQ(value.at("a").array()[0].as_u64(), 1);
    CHECK_EQ(value.at("a").array()[2].at("b").string(), "text");
    CHECK_NEAR(value.at("e").number(), 1.5, 1e-12);

    const auto reparsed = asymsched::json::parse(asymsched::json::dump(value));
    CHECK(reparsed.at("a").array()[1].is_integer_literal());
    CHECK_EQ(reparsed.at("a").array()[2].at("b").string(), "text");
}

TEST(json_distinguishes_integer_literals) {
    CHECK(asymsched::json::parse("0").is_integer_literal());
    CHECK(!asymsched::json::parse("1.0").is_integer_literal());
    CHECK(!asymsched::json::parse("1e2").is_integer_literal());
    CHECK_THROWS(asymsched::json::parse("-1").as_u64());
    CHECK_THROWS(asymsched::json::parse("1.0").as_u64());
}

TEST(json_decodes_unicode_escapes) {
    CHECK_EQ(asymsched::json::parse(R"("\u4f60\u597d")").string(),
             std::string("\xe4\xbd\xa0\xe5\xa5\xbd"));
    CHECK_EQ(asymsched::json::parse(R"("\ud83d\ude00")").string(),
             std::string("\xf0\x9f\x98\x80"));
}

TEST(json_rejects_malformed_documents) {
    CHECK_THROWS(asymsched::json::parse(""));
    CHECK_THROWS(asymsched::json::parse("{"));
    CHECK_THROWS(asymsched::json::parse(R"({"a":1,})"));
    CHECK_THROWS(asymsched::json::parse("01"));
    CHECK_THROWS(asymsched::json::parse("1e999"));
    CHECK_THROWS(asymsched::json::parse("\"unterminated"));
    CHECK_THROWS(asymsched::json::parse(R"("\q")"));
}

TEST(json_dump_escapes_control_characters) {
    asymsched::json::Value::Object object;
    object["s"] = asymsched::json::Value::make_string("a\n\"b\"");
    const std::string text =
        asymsched::json::dump(asymsched::json::Value::make_object(object));
    CHECK_EQ(text, R"({"s":"a\n\"b\""})");
}
