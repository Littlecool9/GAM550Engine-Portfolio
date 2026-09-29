/**
 * @file UIId.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UIId: stable hashing of a name and index, and ids
 *        that must not collide.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIId.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <unordered_set>

using namespace mir;

TEST_CASE("The same name and index give the same id every time", "[gameui]")
{
    static_assert(UIId("play") == UIId("play"));
    static_assert(UIId("slot", 7) == UIId("slot", 7));

    for (int frame = 0; frame < 3; ++frame)
    {
        CHECK(UIId("play") == UIId("play"));
        CHECK(UIId("slot", 7) == UIId("slot", 7));
    }
}

TEST_CASE("A UIId hashes the name's characters, not where they are stored", "[gameui]")
{
    // a label rebuilt every frame lives at a new address each time
    const std::string first = std::string("inventory_") + "slot";
    const std::string second = std::string("inventory") + "_slot";

    REQUIRE(first.data() != second.data());
    CHECK(UIId(first) == UIId(second));
    CHECK(UIId(first, 3) == UIId(second, 3));
}

TEST_CASE("The default UIId is null and no name gives the null id", "[gameui]")
{
    CHECK(UIId{}.IsNull());
    CHECK(UIId{}.Value() == 0);

    CHECK_FALSE(UIId("").IsNull());
    CHECK_FALSE(UIId("", 0).IsNull());
    CHECK_FALSE(UIId("play").IsNull());
}

TEST_CASE("Every index of one name is a different id", "[gameui]")
{
    std::unordered_set<uint64_t> ids;
    ids.insert(UIId("slot").Value());
    for (uint64_t index = 0; index < 10000; ++index)
        ids.insert(UIId("slot", index).Value());

    CHECK(ids.size() == 10001);
}

TEST_CASE("The index is mixed into the hash, not appended to the name", "[gameui]")
{
    CHECK(UIId("item", 1) != UIId("item1"));
    CHECK(UIId("item", 12) != UIId("item1", 2));
    CHECK(UIId("slot", 0) != UIId("slot"));
    CHECK(UIId("a", 1) != UIId("b", 1));
}

TEST_CASE("Different names are different ids", "[gameui]")
{
    std::unordered_set<uint64_t> ids;
    size_t names = 0;

    for (int i = 0; i < 10000; ++i)
    {
        ids.insert(UIId("button_" + std::to_string(i)).Value());
        ++names;
    }

    // one byte apart: case, order, a trailing space, a prefix of another
    for (const char *name : {"Play", "play", "Play ", "lPay", "Pla", "P", "", " "})
    {
        ids.insert(UIId(name).Value());
        ++names;
    }

    CHECK(ids.size() == names);
}
