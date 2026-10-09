/**
 * @file PhysicsBindings.cpp
 * @author Jackey Cen
 * @brief Extends the World usertype with RigidBody velocity, force and impulse calls and world:raycast.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "PhysicsBindings.h"

#include "BindingHelpers.h"

#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <glm/geometric.hpp>

#include <sol/sol.hpp>

#include <cmath>
#include <optional>
#include <string_view>

namespace mir
{
    namespace
    {
        // Jolt's hit fraction is a float, so the hit point resolves to ~6e-8 of this (0.06 mm).
        constexpr float kRaycastDefaultMaxDistance = 1000.0f;

        sol::table Vec3ToLua(sol::state_view lua, const glm::vec3 &value)
        {
            sol::table out = lua.create_table();
            out["x"] = value.x;
            out["y"] = value.y;
            out["z"] = value.z;
            return out;
        }
        bool ToVec3(const sol::object &value, glm::vec3 &out)
        {
            if (!value.is<sol::table>())
                return false;

            const sol::table t = value.as<sol::table>();
            out = glm::vec3(t.get_or("x", 0.0f), t.get_or("y", 0.0f), t.get_or("z", 0.0f));
            return true;
        }

        bool Given(const sol::optional<sol::object> &argument)
        {
            return argument.has_value() && argument->valid();
        }

        // Helper to ensure the handle is looked up on every lua call
        PhysicsWorld *ResolveBody(World &world, Entity entity, BodyHandle &outBody)
        {
            PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
            if (physics == nullptr)
                return nullptr;

            const RigidBody *rigidBody = world.GetComponent<RigidBody>(entity);
            if (rigidBody == nullptr || !rigidBody->body.IsValid())
                return nullptr;

            outBody = rigidBody->body;
            return physics;
        }

        using BodyRead = glm::vec3 (PhysicsWorld::*)(BodyHandle) const;
        using BodyWrite = void (PhysicsWorld::*)(BodyHandle, const glm::vec3 &);
        using BodyWriteAtOffset = void (PhysicsWorld::*)(BodyHandle, const glm::vec3 &, const glm::vec3 &);

        sol::table ReadVector(World &world, Entity entity, sol::this_state state, BodyRead read)
        {
            BodyHandle body{};
            const PhysicsWorld *physics = ResolveBody(world, entity, body);

            return Vec3ToLua(sol::state_view(state),
                             physics != nullptr ? (physics->*read)(body) : glm::vec3(0.0f));
        }

        bool WriteVector(World &world, Entity entity, std::string_view name, const sol::object &value,
                         BodyWrite write)
        {
            glm::vec3 vector{};
            if (!ToVec3(value, vector))
            {
                MIR_LOG_ERROR("script: world:{} needs a table with x, y and z", name);
                return false;
            }

            BodyHandle body{};
            PhysicsWorld *physics = ResolveBody(world, entity, body);
            if (physics == nullptr)
                return false;

            (physics->*write)(body, vector);
            return true;
        }

        bool WriteVectorAtOffset(World &world, Entity entity, std::string_view name, const sol::object &value,
                                 const sol::optional<sol::object> &offset, BodyWrite write,
                                 BodyWriteAtOffset writeAtOffset)
        {
            if (!Given(offset))
                return WriteVector(world, entity, name, value, write);

            glm::vec3 vector{};
            glm::vec3 localOffset{};
            if (!ToVec3(value, vector) || !ToVec3(*offset, localOffset))
            {
                MIR_LOG_ERROR("script: world:{} needs tables with x, y and z", name);
                return false;
            }

            BodyHandle body{};
            PhysicsWorld *physics = ResolveBody(world, entity, body);
            if (physics == nullptr)
                return false;

            (physics->*writeAtOffset)(body, vector, localOffset);
            return true;
        }

        sol::table WorldLinearVelocity(World &world, Entity entity, sol::this_state state)
        {
            return ReadVector(world, entity, state, &PhysicsWorld::BodyLinearVelocity);
        }

        sol::table WorldAngularVelocity(World &world, Entity entity, sol::this_state state)
        {
            return ReadVector(world, entity, state, &PhysicsWorld::BodyAngularVelocity);
        }

        bool WorldSetLinearVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "set_linear_velocity", velocity,
                               &PhysicsWorld::SetBodyLinearVelocity);
        }

        bool WorldSetAngularVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "set_angular_velocity", velocity,
                               &PhysicsWorld::SetBodyAngularVelocity);
        }

        bool WorldAddLinearVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "add_linear_velocity", velocity,
                               &PhysicsWorld::AddLinearVelocity);
        }

        bool WorldAddAngularVelocity(World &world, Entity entity, const sol::object &velocity)
        {
            return WriteVector(world, entity, "add_angular_velocity", velocity,
                               &PhysicsWorld::AddAngularVelocity);
        }

        // The force is world space; only the offset is in body space. A script
        // that wants a body-local thrust rotates it by Transform.rotation first.
        bool WorldApplyForce(World &world, Entity entity, const sol::object &force,
                             sol::optional<sol::object> offset)
        {
            return WriteVectorAtOffset(world, entity, "apply_force", force, offset, &PhysicsWorld::AddForce,
                                       &PhysicsWorld::AddForceAtOffset);
        }

        bool WorldApplyTorque(World &world, Entity entity, const sol::object &torque)
        {
            return WriteVector(world, entity, "apply_torque", torque, &PhysicsWorld::AddTorque);
        }

        bool WorldApplyImpulse(World &world, Entity entity, const sol::object &impulse,
                               sol::optional<sol::object> offset)
        {
            return WriteVectorAtOffset(world, entity, "apply_impulse", impulse, offset,
                                       &PhysicsWorld::AddImpulse, &PhysicsWorld::AddImpulseAtOffset);
        }

        bool WorldApplyAngularImpulse(World &world, Entity entity, const sol::object &impulse)
        {
            return WriteVector(world, entity, "apply_angular_impulse", impulse,
                               &PhysicsWorld::AddAngularImpulse);
        }

        bool IsFinite(const glm::vec3 &v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        sol::table WorldRaycast(World &world, const sol::object &origin, const sol::object &direction,
                                sol::optional<sol::object> maxDistance, sol::this_state state)
        {
            sol::state_view lua(state);
            sol::table result = lua.create_table();
            result["hit"] = false;

            const std::optional<WorldVec3> from = WorldVec3FromLua(origin);
            if (!from)
            {
                MIR_LOG_ERROR("script: world:raycast needs an origin table with finite x, y and z in world range");
                return result;
            }

            glm::vec3 dir{};
            const float length = ToVec3(direction, dir) ? glm::length(dir) : 0.0f;
            if (!IsFinite(dir) || !(length > 0.0f) || !IsFinite(dir / length))
            {
                MIR_LOG_ERROR("script: world:raycast needs a finite, non-zero direction table with x, y and z");
                return result;
            }

            float distance = kRaycastDefaultMaxDistance;
            if (Given(maxDistance))
            {
                const double given = maxDistance->get_type() == sol::type::number ? maxDistance->as<double>() : 0.0;
                distance = static_cast<float>(given);
                if (!std::isfinite(distance) || !(distance > 0.0f))
                {
                    MIR_LOG_ERROR("script: world:raycast needs a finite, positive maxDistance");
                    return result;
                }
            }

            const PhysicsWorld *physics = world.TryResource<PhysicsWorld>();
            if (physics == nullptr)
                return result;

            const std::optional<WorldPosition> position = WorldPosition::FromMeters(*from);
            if (!position)
                return result;

            const std::optional<RayHit> hit = physics->RayCast(*position, dir / length, distance);
            if (!hit)
                return result;

            result["hit"] = true;
            result["point"] = WorldVec3ToLua(lua, hit->mPoint.Meters());
            result["normal"] = Vec3ToLua(lua, hit->mNormal);
            result["distance"] = hit->mDistance;
            // A terrain collider's tag can name an entity destroyed during Play.
            const Entity entity = physics->BodyEntity(hit->mBody);
            if (!entity.IsNull() && world.IsAlive(entity))
                result["entity"] = entity;
            return result;
        }
    }

    void RegisterPhysicsBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("linear_velocity", &WorldLinearVelocity);
        worldType.set_function("angular_velocity", &WorldAngularVelocity);
        worldType.set_function("set_linear_velocity", &WorldSetLinearVelocity);
        worldType.set_function("set_angular_velocity", &WorldSetAngularVelocity);
        worldType.set_function("add_linear_velocity", &WorldAddLinearVelocity);
        worldType.set_function("add_angular_velocity", &WorldAddAngularVelocity);
        worldType.set_function("apply_force", &WorldApplyForce);
        worldType.set_function("apply_torque", &WorldApplyTorque);
        worldType.set_function("apply_impulse", &WorldApplyImpulse);
        worldType.set_function("apply_angular_impulse", &WorldApplyAngularImpulse);
        worldType.set_function("raycast", &WorldRaycast);
    }
}
