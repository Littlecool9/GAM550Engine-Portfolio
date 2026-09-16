/**
 * @file JoltRuntime.cpp
 * @author Jackey Cen
 * @brief Reference counted setup and teardown of Jolt's process-wide state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "JoltRuntime.h"

#include <core/log/Log.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/RegisterTypes.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace
{
    std::mutex gRuntimeMutex;
    int gRuntimeRefCount = 0;

    void TraceImpl(const char *format, ...)
    {
        char buffer[1024];

        va_list args;
        va_start(args, format);
        std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        MIR_LOG_TRACE("Jolt: {}", buffer);
    }

#ifdef JPH_ENABLE_ASSERTS
    // Returning false keeps Jolt out of the debugger: a failed Jolt assert is a
    // content problem, and the logged error is already enough to fail a smoke
    // test or a CI run without hanging the process on a breakpoint.
    bool AssertFailedImpl(const char *expression, const char *message, const char *file, JPH::uint line)
    {
        MIR_LOG_ERROR("Jolt assert at {}:{}: ({}) {}",
            file, line, expression, message != nullptr ? message : "");
        return false;
    }
#endif
}

namespace mir::detail
{
    JoltRuntimeRef::JoltRuntimeRef()
    {
        const std::lock_guard<std::mutex> lock(gRuntimeMutex);
        if (gRuntimeRefCount++ > 0)
            return;

        // Order matters: the allocator has to be in place before the Factory
        // allocates anything, and RegisterTypes needs the Factory.
        JPH::RegisterDefaultAllocator();

        JPH::Trace = &TraceImpl;
        JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &AssertFailedImpl;)

        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    JoltRuntimeRef::~JoltRuntimeRef()
    {
        const std::lock_guard<std::mutex> lock(gRuntimeMutex);
        if (--gRuntimeRefCount > 0)
            return;

        JPH::UnregisterTypes();

        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}
