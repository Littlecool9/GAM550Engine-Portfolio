/**
 * @file ComponentRegistration.cpp
 * @author Sumin Park
 * @brief Registers every component the script module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "script/ComponentRegistration.h"

#include "script/components/ScriptRef.h"

#include <core/ecs/ComponentRegistry.h>

namespace mir
{
    void RegisterScriptComponents()
    {
        ComponentRegistry::Instance().Register<ScriptRef>(kScriptRefFields);
    }
}
