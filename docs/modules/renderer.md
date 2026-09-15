# renderer

The whole Vulkan path — instance through swapchain, meshes, materials, depth and
per-frame recording — plus the render-facing components and the one system that
drives it from the ECS.

`mir::renderer` · depends `mir::core`, `volk`, `glm` (public); `VMA`, `imgui` (private) · API `modules/renderer/include/renderer/` · maintainer Sumin Park · reviewed 2026-09-06

## Model

`VulkanRenderer` is one class holding everything: instance, debug messenger,
surface, device, VMA allocator, swapchain, per-frame command pools and buffers, a
depth image per frame, an uploaded-mesh table, a pipeline-per-material table, and
frame pacing on a timeline semaphore with `kFramesInFlight = 2`.

`RenderSystem` is the only thing connecting the ECS to it. It is an `ISystem` in
`SystemPhase::Render` holding a `VulkanRenderer&`. Each frame it takes the first
`Camera` it finds, builds one view-projection matrix, builds one `DrawItem` per
`(WorldTransform, MeshRenderer)` pair, and calls `DrawFrame`.

Game code never calls Vulkan. It calls `CreateMesh` and `CreateMaterial` once at
scene-build time, stores the returned handles on a `MeshRenderer` component, and
`RenderSystem` takes over from there every frame after. When `App` unloads that
scene, `ReleaseSceneResources` frees them all again, except the default material.

The module targets Vulkan 1.3 and rejects any device below it. That removes a
lot of code: there are no render passes and no framebuffers, since
`vkCmdBeginRendering` is used throughout, there are only synchronization2
barriers, and frame pacing is one timeline semaphore rather than a fence pool.

## API

```cpp
bool Initialize(const RendererDesc &desc);   // takes an ISurfaceProvider, never a Window

MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);
MaterialHandle CreateMaterial(const MaterialDesc &desc);
void ReleaseSceneResources();                // every mesh and material but the default

void DrawFrame(std::span<const DrawItem> items);
void SetClearColor(const glm::vec4 &color);
void SetImGuiDrawData(ImDrawData *drawData);
void SetSceneViewport(VkRect2D rect);        // zero extent = full swapchain
float AspectRatio() const;                   // follows the scene viewport once one is set

bool InitImGuiVulkanBackend();               // called by editor, not App or game code
void ShutdownImGuiVulkanBackend();

void Shutdown();
```

| Type | Header | Role |
|---|---|---|
| `Vertex`, `MeshHandle` | `renderer/Mesh.h` | `{pos, color, normal}`; a `{index, generation}` handle |
| `MaterialDesc`, `MaterialHandle` | `renderer/Material.h` | `{shaderName, cullMode, polygonMode}`; `kNullMaterial` means the default |
| `DrawItem` | `renderer/Renderer.h` | `{mesh, model, normalMatrix, tint, material}` |
| `MeshRenderer`, `Camera` | `renderer/components/` | Handles, and `fovY`/`near`/`far`, declared here rather than in `core` |
| `RenderSystem` | `renderer/RenderSystem.h` | The ECS-to-renderer bridge, `SystemPhase::Render` |
| `Shapes.h` | `renderer/Shapes.h` | `MakeCube()` and friends — plain vectors, no Vulkan |

## Rules

**Vulkan 1.3 with `dynamicRendering`, `synchronization2`, `timelineSemaphore` and
`shaderDrawParameters`.** A device missing any of them is rejected during
selection, with a log line naming what was missing.

The floor is roughly 2020 hardware with a current driver. That is acceptable for
a project that is not shipping to consumers.

**`volk` is compiled from source in this module**, so the platform defines are
ours. VMA is configured with `VMA_STATIC_VULKAN_FUNCTIONS=0` and
`VMA_DYNAMIC_VULKAN_FUNCTIONS=1`, so it fetches its pointers through volk.

**The renderer takes an `ISurfaceProvider`, never a `Window`.** It re-queries the
size through that interface after a resize, which is why it is an interface
rather than a handle and two integers passed once.

**One `VkPipeline` per material, built eagerly at `CreateMaterial`.** The cost is
a shader module load plus `vkCreateGraphicsPipelines`, paid at load time.
Everything is created at scene-build time anyway, before the loop starts.

**An invalid material falls back to the default** rather than failing the draw,
so one bad handle does not blank the frame.

**`ReleaseSceneResources` is all or nothing, and the caller vouches for the
handles.** It waits for the device to go idle, destroys every mesh and every
material except the default, and the next `CreateMesh` or `CreateMaterial` reuses
index 0. Mesh generations are not bumped, so a handle kept across the call names
whatever is created next in its slot, with no error. `App` calls it only right
after unloading a scene, when no `MeshRenderer` is left.

**`RenderSystem` runs in `Render` because `TransformPropagateSystem` runs in
`PostUpdate`.** Placed any earlier, it would draw the previous frame's matrices.

**The renderer reaches `RenderSystem` by constructor reference**, not as a world
resource. A resource would be reachable from any script-bound system, and there
is no access-control story for that yet.

**`SetSceneViewport` clips the scene pass**, clamped to the swapchain, so the
editor's docked panels are never painted over.

ImGui's draw data composites on top afterwards, unclipped, because ImGui manages
its own per-window clip rects. `editor` is the only caller.

**RenderDoc object naming is applied under validation**, and shaders are built
`-g -O0` unconditionally, so a capture shows Slang source.

## State

*As of 2026-09-06.* Working: instance through swapchain, swapchain recreation on
resize, meshes, materials, depth, camera, ImGui compositing, and scene-viewport
clipping.

`Renderer.h` is still an empty abstraction placeholder, so `App` names
`VulkanRenderer` directly.

No tests. Everything here needs a device, and the module is verified by running.

## Backlog

1. **Descriptor sets and uniform buffers.** The view-projection is premultiplied
   on the CPU into each `DrawItem`'s push constant, which caps what else can
   travel with a draw call at the 128-byte guaranteed minimum.
2. **Per-handle destroy for meshes and materials.** `ReleaseSceneResources` frees
   everything at once. Nothing can free a single mesh, and a stale handle is not
   detected.
3. **Draw-item sorting and batching.** There is one pipeline bind per item today,
   so a frame scales with the number of distinct materials rather than with draw
   count.
4. **Lighting** beyond what the `unlit` and `triangle` shaders hardcode.
5. **Per-config shader output paths.** Release currently ships unoptimised
   shaders, and the debug flags cannot become conditional while both
   configurations write to a path that carries no configuration component.
6. **A real renderer interface.** It is the prerequisite for a headless mode, and
   therefore for testing anything in this module or in `app`.

## Changed

*2026-09-14* — **Scene resources can be released.** Was: every mesh and pipeline
lived until `Shutdown()`, so each scene reload or game switch uploaded another
copy. Now: `ReleaseSceneResources` destroys all of them except the default
material, and `App` calls it whenever it unloads a scene.

*2026-09-03* — **Render-facing components moved from `core` to `renderer`.** Was:
`TriangleRenderer`, a tag in `core`, with `App` driving the draw by hand. Now:
`MeshRenderer` and `Camera` in `renderer/include/renderer/components/`.

Real meshes needed a `MeshHandle` component that the renderer allocates,
validates and destroys, and `core` can do none of that without learning what
Vulkan is.

*2026-09-03* — **Materials arrived, and pipelines with them.** Was: one pipeline
built from one hardcoded shader. Now: one pipeline per `MaterialDesc`, built at
creation. A second look (`unlit.slang`) forced a decision about how a draw picks
its pipeline, and a different fragment shader is a different pipeline in Vulkan
rather than a parameter on an existing one.
