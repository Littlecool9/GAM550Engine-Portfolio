# renderer

The whole Vulkan path — instance through swapchain, meshes, materials, depth and
per-frame recording — plus the render-facing components and the one system that
drives it from the ECS.

`mir::renderer` · depends `mir::core`, `volk`, `glm` (public); `VMA`, `imgui`, `mir::assets`, `nlohmann_json` (private) · API `modules/renderer/include/renderer/` · maintainer Sumin Park · reviewed 2026-09-12

## Model

`VulkanRenderer` is one class holding everything: instance, debug messenger,
surface, device, VMA allocator, swapchain, per-frame command pools and buffers, a
depth image per frame, an uploaded-mesh table, a pipeline-per-material table, and
frame pacing on a timeline semaphore with `kFramesInFlight = 2`.

`RenderSystem` is the only thing connecting the ECS to it. It is an `ISystem` in
`SystemPhase::Render` holding a `VulkanRenderer&`, and it owns a list of
`Viewport`s. Viewport 0 always exists(whole window, first `Camera` found),
and can add more with `AddViewPort`.

Each frame, for every viewport, `RenderSystem` resolves a camera (a specific
World `Entity` via `SetViewportCamera`, an externally-owned one via
`SetExternalCamera`, or that same first-match `Camera` as a fallback), builds
one view-projection matrix scaled to that viewport's own rect, builds one
`DrawItem` per `(WorldTransform, MeshRenderer)` pair, and packages the result as
a `RenderView`. All of a frame's `RenderView`s go to `VulkanRenderer::DrawFrame`
together.

`VulkanRenderer` draws every `RenderView` inside one render pass, swapping only the viewport/scissor rect between them (`RecordView`). This is split-screen-style clipping
within the one swapchain image, not separate offscreen render targets.

Game code never calls Vulkan. It calls `CreateMesh` and `CreateMaterial` once at
scene-build time, stores the returned handles on a `MeshRenderer` component, and
`RenderSystem` takes over from there every frame after. When `App` unloads that
scene, `ReleaseSceneResources` frees them all again, except the default material.

The module targets Vulkan 1.4 and rejects any device below it. That removes a
lot of code: there are no render passes and no framebuffers, since
`vkCmdBeginRendering` is used throughout, there are only synchronization2
barriers, and frame pacing is one timeline semaphore rather than a fence pool.

## API

```cpp
bool Initialize(const RendererDesc &desc);   // takes an ISurfaceProvider, never a Window

MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices);
TextureHandle CreateTexture(const TextureDesc &desc);
MaterialHandle CreateMaterial(const MaterialDesc &desc);
void ReleaseSceneResources();                // every mesh, material and texture but the defaults

void DrawFrame(std::span<const RenderView> views);
void SetClearColor(const glm::vec4 &color);
void SetImGuiDrawData(ImDrawData *drawData);
VkExtent2D SwapchainExtent() const; 

bool InitImGuiVulkanBackend();               // called by editor, not App or game code
void ShutdownImGuiVulkanBackend();

void Shutdown();
```

```cpp
// RenderSystem
ViewportId AddViewport(VkRect2D rect);
void RemoveViewport(ViewportId id);          // viewport 0 cannot be removed
void SetViewportRect(ViewportId id, VkRect2D rect);
void SetViewportCamera(ViewportId id, Entity camera);           // a World camera
void SetExternalCamera(ViewportId id, const glm::mat4 &cameraWorld, const Camera &camera); // one the World doesn't own
```

| Type | Header | Role |
|---|---|---|
| `Vertex`, `MeshHandle` | `renderer/Mesh.h` | `{pos, color, normal}`; a `{index, generation}` handle |
| `TextureDesc`, `TextureHandle` | `renderer/Texture.h` | `{width, height, pixels, srgb}`, tightly-packed RGBA8; `kNullTexture` falls back to a default in `CreateMaterial` |
| `MaterialDesc`, `MaterialHandle` | `renderer/Material.h` | `{shaderName, cullMode, polygonMode, baseColorTexture, normalTexture}`; `kNullMaterial` means the default |
| `DrawItem` | `renderer/Renderer.h` | `{mesh, model, normalMatrix, tint, material}` |
| `RenderView` | `renderer/VulkanRenderer.h` | `{rect, items}` — one camera's `DrawItem`s, clipped to a screen rect |
| `Viewport`, `ViewportId` | `renderer/RenderSystem.h` | `RenderSystem`'s per-viewport state: rect plus a World `Entity`, an external camera, or neither (fallback) |
| `MeshRenderer`, `Camera` | `renderer/components/` | Handles, and `fovY`/`near`/`far`, declared here rather than in `core` |
| `RenderSystem` | `renderer/RenderSystem.h` | The ECS-to-renderer bridge, `SystemPhase::Render` |
| `Shapes.h` | `renderer/Shapes.h` | `MakeCube()` and friends — plain vectors, no Vulkan |
| `RenderSettings` | `renderer/RenderSettings.h` | Project Settings' `"rendering"` section (`rayTracing: bool` so far); `LoadFile`/`SaveFile`, a World resource loaded in `App::Initialize` |

## Rules

**Vulkan 1.4 with `dynamicRendering`, `synchronization2`, `timelineSemaphore` and
`shaderDrawParameters`.** A device missing any of them is rejected during
selection, with a log line naming what was missing.

The floor is roughly 2020 hardware with a current driver. That is acceptable for
a project that is not shipping to consumers.

**`RenderSettings` owns only `project_settings.json`'s `"rendering"` key**,
the same way `InputMap` (see the `input` module) owns only `"input"`. Each
type's `SaveFile` reads the file first and rewrites only its own section, so
the two can be loaded and saved independently without one clobbering the
other. A file from before sections existed has no `"rendering"` key, which
`RenderSettings::LoadFile` reads the same as an empty one — defaults, not an
error.

`RenderSettings` reads and writes bytes through `assets/AssetFileIo.h`
(`ReadFileBytes`/`WriteFileBytes`) rather than opening its own file streams,
the same file-IO primitives `AssetManifest` and `AssetCache` use.

**`volk` is compiled from source in this module**, so the platform defines are
ours. VMA is configured with `VMA_STATIC_VULKAN_FUNCTIONS=0` and
`VMA_DYNAMIC_VULKAN_FUNCTIONS=1`, so it fetches its pointers through volk.

**The renderer takes an `ISurfaceProvider`, never a `Window`.** It re-queries the
size through that interface after a resize, which is why it is an interface
rather than a handle and two integers passed once.

**One `VkPipeline` and one `VkDescriptorSet` per material, built eagerly at
`CreateMaterial`.** The cost is a shader module load, `vkCreateGraphicsPipelines`
and a descriptor set write, paid at load time. Everything is created at
scene-build time anyway, before the loop starts. Every material's pipeline
layout carries the same one-set layout: binding 0 is the base color texture,
binding 1 is the normal map, both combined image samplers read in the
fragment stage.

**An invalid material falls back to the default** rather than failing the
draw, so one bad handle does not blank the frame. The same rule applies one
level down: `CreateMaterial` resolves `MaterialDesc::baseColorTexture` and
`normalTexture` through `CreateTexture`'s table, and a null or stale texture
handle falls back to a flat white base color / flat normal map rather than
failing material creation.

**`ReleaseSceneResources` is all or nothing, and the caller vouches for the
handles.** It waits for the device to go idle, destroys every mesh, every
material except the default, and every texture except the two defaults, and
the next `CreateMesh`/`CreateMaterial`/`CreateTexture` reuses index 0 (index 2
for textures, since the two defaults keep 0 and 1). Mesh and texture
generations are not bumped, so a handle kept across the call names whatever is
created next in its slot, with no error. `App` calls it only right after
unloading a scene, when no `MeshRenderer` is left.

**`RenderSystem` runs in `Render` because `TransformPropagateSystem` runs in
`PostUpdate`.** Placed any earlier, it would draw the previous frame's matrices.

**The renderer reaches `RenderSystem` by constructor reference**, not as a world
resource. A resource would be reachable from any script-bound system, and there
is no access-control story for that yet.

**`RenderSystem` owns the viewport list, not `Editor` or `App`.** Viewport
mechanics — shared meshes/materials/pipeline binds across every view, per-view
culling later, render-target lifetime — are a rendering concern, and a shipped
game with split-screen or a security-camera-in-a-monitor needs multiple
viewports with zero editor code compiled in. `Editor` is a caller of the API
(it calls `AddViewport`/`SetViewportRect` for its own scene panel), never the
owner of the list.

**Each `RenderView`'s rect clips its own viewport/scissor**, clamped to the
swapchain, so the editor's docked panels are never painted over and one
viewport's geometry never bleeds into another's rect.

ImGui's draw data composites on top afterwards, unclipped and after every
`RenderView`, because ImGui manages its own per-window clip rects. `editor` is
the only caller.

**A viewport's camera resolves in this order: external, then a specific
`Entity`, then the first `Camera` found.** `SetExternalCamera` is for a camera
the World doesn't own, such as the editor's free-fly camera — its transform is
a raw `glm::mat4`, not a `WorldTransform`, because `WorldTransform`'s matrix is
resolver-only and unconstructable outside the ECS hierarchy resolve.
`SetViewportCamera(id, kNullEntity)` clears both and falls back to the
first-match `Camera`, which is how a viewport hands itself back to whatever the
World considers its default camera.

**RenderDoc object naming is applied under validation**, and shaders are built
`-g -O0` unconditionally, so a capture shows Slang source.

## State

*As of 2026-09-12.* Working: instance through swapchain, swapchain recreation on
resize, meshes, materials, depth, ImGui compositing, and a `RenderSystem`
viewport list — one viewport by default, more via `AddViewport`, each with its
own rect and camera resolution, all drawn in one pass. `App` currently uses only
viewport 0, feeding it the editor's free-fly camera in Edit mode and falling
back to the World's default camera in Play.

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
7. **A second viewport actually on screen at once.** The mechanism (`AddViewport`)
   exists and is tested by `App` using only viewport 0; nothing yet adds a second
   one for, say, an editor-camera view alongside the game view during a paused
   Play session.

## Changed

*2026-09-17* — **`RenderSettings` added, owning `project_settings.json`'s
new `"rendering"` section.** Was: `project_settings.json` was `InputMap`'s
file alone, root-level `"actions"` and nothing else. Now: the root has an
`"input"` section (`InputMap`, unchanged in content) and a `"rendering"`
section (`RenderSettings`, `rayTracing: bool` so far); each type's
`LoadFile`/`SaveFile` reads and writes only its own key, and `SaveFile`
preserves whatever sibling section is already on disk. A pre-sections file
(root-level `"actions"`, no `"input"`/`"rendering"` keys) still loads via
`InputMap`'s back-compat path, and is migrated to the sectioned form the
next time it saves. `RenderSettings` is loaded into a World resource in
`App::Initialize`, alongside `InputMap`. Its file IO goes through
`assets/AssetFileIo.h`'s `ReadFileBytes`/`WriteFileBytes` rather than its own
streams, so this module now depends on `mir::assets`.

*2026-09-17* — **Device floor raised to Vulkan 1.4.** Was: instance/device
requested 1.3. Now: both request 1.4 (`VulkanRenderer::VulkanVersion`); the
rejection log line reports "need 1.4". Required features
(`dynamicRendering`, `synchronization2`, `timelineSemaphore`,
`shaderDrawParameters`) are unchanged — they validate the same way under 1.4 —
so this is a floor bump with no functional change to raster rendering.

*2026-09-14* — **Scene resources can be released.** Was: every mesh and pipeline
lived until `Shutdown()`, so each scene load and each Play session's Stop
uploaded another copy. Now: `ReleaseSceneResources` destroys all of them except
the default material, and `App` calls it whenever scene entities are discarded:
New, Load and Stop.

*2026-09-12* — **`RenderSystem` gained a viewport list, replacing one implicit
whole-window view.** Was: `VulkanRenderer::SetSceneViewport`/`AspectRatio`
tracked one clip rect renderer-side, and `RenderSystem` always drew the first
`Camera` found into it. Now: `RenderSystem` owns `std::vector<Viewport>`
(`AddViewport`/`SetViewportRect`/`SetViewportCamera`/`SetExternalCamera`),
`VulkanRenderer::DrawFrame` takes `std::span<const RenderView>` and draws each
into its own rect within one pass (`RecordView`), and `SwapchainExtent()`
replaces `AspectRatio()` since aspect is now per-viewport.

Driven by wanting multiple simultaneous camera views (an editor camera and a
game camera, later split-screen) without duplicating render state per view —
the renderer is the one place that can share meshes/materials/pipeline binds
across all of them, so it, not `Editor` or `App`, needed to own the list.

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
