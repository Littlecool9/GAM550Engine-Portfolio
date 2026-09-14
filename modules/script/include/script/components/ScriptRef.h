#pragma once

#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>

#include <cstddef>
#include <cstdint>

namespace mir
{
    struct ScriptRef
    {
        int32_t instanceRef = -1; // IScriptHost-owned handle, -1 = none
        bool started = false;     // OnStart has run for this instance

        // Name the script was registered under (IScriptHost::LoadScriptSource),
        // not a runtime handle - unlike instanceRef, this is what a scene file
        // actually saves: instanceRef is only meaningful within one run's live
        // ScriptHost, so LoadScene re-derives it from this name instead of
        // trying to restore the handle itself (see ResolveSceneScripts, run by
        // App::LoadScene).
        char scriptName[kFieldStringCapacity] = {};
    };

    MIR_ASSERT_COMPONENT(ScriptRef);

    // Fields for scripts, inspectors, and scene serialization. instanceRef and
    // started are deliberately left out - they're this run's live state, not
    // anything a scene file should round-trip.
    inline constexpr FieldDesc kScriptRefFields[] = {
        {"scriptName", FieldKind::String, offsetof(ScriptRef, scriptName)},
    };
}
