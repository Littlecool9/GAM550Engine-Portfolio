/**
 * @file MeshRenderer.h
 * @author Rahul Nair
 * @brief Marks an entity as drawing a mesh.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"
#include "renderer/Material.h"
#include "renderer/Mesh.h"

#include <cstddef>
#include <glm/vec4.hpp>

namespace mir
{
    // the mesh render component
    // null mesh is skipped
    struct MeshRenderer
    {
        // Runtime-only, like ScriptRef::instanceRef: a GPU upload handle from
        // this run's VulkanRenderer, meaningless after a restart. A scene
        // file stores meshName/materialShader instead (see below) and a
        // post-load pass (ResolveSceneMeshes, run by App::LoadScene) turns those back
        // into these two - CreateMesh/CreateMaterial are what actually
        // upload, so nothing before that pass can fill them in.
        MeshHandle mesh;
        MaterialHandle material;

        glm::vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};

        // Asset path of the mesh to load, relative to the project's
        // assetsRoot (e.g. "meshes/cube.mesh.json") - what a scene file
        // actually names. Empty means "no mesh", same as mesh.IsNull().
        char meshName[kFieldStringCapacity] = {};

        // Shader name for CreateMaterial (MaterialDesc::shaderName). Empty
        // resolves to the renderer's default material, same as
        // material.IsNull() today.
        char materialShader[kFieldStringCapacity] = {};
    };

    MIR_ASSERT_COMPONENT(MeshRenderer);

    // Fields for scripts, inspectors, and scene serialization. mesh/material
    // are deliberately left out - see the comment on them above.
    inline constexpr FieldDesc kMeshRendererFields[] = {
        {"tint", FieldKind::Vec4, offsetof(MeshRenderer, tint)},
        {"meshName", FieldKind::String, offsetof(MeshRenderer, meshName)},
        {"materialShader", FieldKind::String, offsetof(MeshRenderer, materialShader)},
    };
}

// Table storage: dense like Transform, since most drawable entities carry
// one and RenderSystem sweeps every row.
