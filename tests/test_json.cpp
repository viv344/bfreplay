#include <gtest/gtest.h>
#include <cmath>
#include "bfr/json.hpp"

using namespace bfr::json;

TEST(Json, ParsesStreamMessage) {
    Document d;
    ASSERT_TRUE(d.parse(R"({"op":"mcm","pt":1763075783036,"mc":[{"id":"1.25","rc":[{"ltp":5.2,"id":123},{"ltp":1.01,"id":456}]}]})"));
    auto r = d.root();
    EXPECT_EQ(r["op"].str(), "mcm");
    EXPECT_EQ(static_cast<int64_t>(r["pt"].number()), 1763075783036);
    int n = 0;
    for (auto mc : r["mc"]) {
        EXPECT_EQ(mc["id"].str(), "1.25");
        for (auto rc : mc["rc"]) {
            ++n;
            if (rc["id"].number() == 123) { EXPECT_DOUBLE_EQ(rc["ltp"].number(), 5.2); }
        }
    }
    EXPECT_EQ(n, 2);
}

TEST(Json, NestedObjectsBoolsNullsAndMissingKeys) {
    Document d;
    ASSERT_TRUE(d.parse(R"({"a":{"inPlay":true,"x":null,"runners":[]},"b":false})"));
    EXPECT_TRUE(d.root()["a"]["inPlay"].boolean());
    EXPECT_TRUE(d.root()["a"]["x"].is_null());
    EXPECT_FALSE(d.root()["b"].boolean());
    EXPECT_FALSE(d.root()["missing"]);
    EXPECT_TRUE(d.root()["missing"]["deeper"].is_null());
    int n = 0;
    for (auto v : d.root()["a"]["runners"]) { (void)v; ++n; }
    EXPECT_EQ(n, 0);
}

TEST(Json, InfinityAndNaNInStringsAndBareTokens) {
    Document d;
    ASSERT_TRUE(d.parse(R"({"bsp":"Infinity","x":NaN,"y":-Infinity,"z":"12.5"})"));
    EXPECT_TRUE(std::isinf(d.root()["bsp"].number()));
    EXPECT_TRUE(std::isnan(d.root()["x"].number()));
    EXPECT_TRUE(std::isinf(d.root()["y"].number()) && d.root()["y"].number() < 0);
    EXPECT_DOUBLE_EQ(d.root()["z"].number(), 12.5);
}

TEST(Json, EscapedStringsKeepRawContent) {
    Document d;
    ASSERT_TRUE(d.parse(R"({"name":"O\"Brien é","k":1})"));
    EXPECT_EQ(d.root()["name"].str(), R"(O\"Brien é)");
    EXPECT_EQ(d.root()["k"].number(), 1);
}

TEST(Json, RejectsMalformed) {
    Document d;
    EXPECT_FALSE(d.parse(R"({"a":1)"));
    EXPECT_FALSE(d.parse(R"({"a" 1})"));
    EXPECT_FALSE(d.parse(R"({"a":1,})"));
    EXPECT_FALSE(d.parse(R"({"a":tru})"));
    EXPECT_FALSE(d.parse(R"({"a":1} extra)"));
}

TEST(Json, ArenaReusedAcrossLines) {
    Document d;
    ASSERT_TRUE(d.parse(R"({"a":[1,2,3,4,5,6,7,8]})"));
    size_t big = d.size();
    ASSERT_TRUE(d.parse(R"({"b":1})"));
    EXPECT_LT(d.size(), big);
    EXPECT_FALSE(d.root()["a"]);
    EXPECT_EQ(d.root()["b"].number(), 1);
}
