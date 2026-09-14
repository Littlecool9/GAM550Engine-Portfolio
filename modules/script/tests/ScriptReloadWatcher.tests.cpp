/**
 * @file ScriptReloadWatcher.tests.cpp
 * @author Jackey Cen
 * @brief Tests for ScriptReloadWatcher::UnloadAll.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptHost.h>
#include <script/ScriptReloadWatcher.h>

#include <assets/AssetId.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

namespace
{
    constexpr std::string_view kEmptyScriptSource = R"LUA(
local T = {}
return T
)LUA";
}

TEST_CASE("UnloadAll unloads every tracked script from the host")
{
    mir::ScriptHost host;
    mir::ScriptReloadWatcher watcher(host);

    REQUIRE(host.LoadScriptSource("first", kEmptyScriptSource));
    REQUIRE(host.LoadScriptSource("second", kEmptyScriptSource));
    watcher.Track("first", mir::MakeAssetId("games/Test/assets/scripts/first.lua"));
    watcher.Track("second", mir::MakeAssetId("games/Test/assets/scripts/second.lua"));

    watcher.UnloadAll();

    CHECK(host.CreateInstance("first") == -1);
    CHECK(host.CreateInstance("second") == -1);
}

TEST_CASE("UnloadAll leaves scripts it never tracked loaded")
{
    mir::ScriptHost host;
    mir::ScriptReloadWatcher watcher(host);

    REQUIRE(host.LoadScriptSource("tracked", kEmptyScriptSource));
    REQUIRE(host.LoadScriptSource("untracked", kEmptyScriptSource));
    watcher.Track("tracked", mir::MakeAssetId("games/Test/assets/scripts/tracked.lua"));

    watcher.UnloadAll();

    CHECK(host.CreateInstance("tracked") == -1);

    const int32_t instance = host.CreateInstance("untracked");
    CHECK(instance >= 0);
    host.DestroyInstance(instance);
}
