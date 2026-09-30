# renderer

The whole Vulkan path — instance through swapchain, meshes, materials, depth and
per-frame recording — plus the render-facing components and the one system that
drives it from the ECS.

`mir::renderer` · depends `mir::core`, `volk`, `glm` (public); `VMA`, `imgui`, `mir::serial` (private) · API `modules/renderer/include/renderer/` · maintainer Sumin Park · reviewed 2026-09-18

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

Each view's opaque items are followed by one fullscreen-triangle sky draw,
depth-tested (not written) against the far clear value so it only shows
through the background. It's a Nishita-style ray-marched atmosphere reading
the same sun as scene lighting — see the set-1 UBO rule below and
`sky.slang`.

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
bool DestroyMesh(MeshHandle handle);         // false if handle is null/stale; frees that mesh alone
TextureHandle CreateTexture(const TextureDesc &desc);
MaterialHandle CreateMaterial(const MaterialDesc &desc);
bool DestroyMaterial(MaterialHandle handle); // same contract; always false on the default material
void ReleaseSceneResources();                // every mesh, material and texture but the defaults

void DrawFrame(std::span<const RenderView> views);
void TraceFrame(std::span<const RenderView> views);  // opt-in RT path alongside DrawFrame - see Rules
bool RayTracingSupported() const;                     // detected once at device selection, never a rejection reason
void SetClearColor(const glm::vec4 &color);
void SetDirectionalLight(const glm::vec3 &direction, const glm::vec3 &color); // direction points *toward* the light
void SetAmbientColor(const glm::vec3 &color);
void SetFog(const glm::vec3 &color, float density);          // density <= 0 disables fog
void SetImGuiDrawData(ImDrawData *drawData);
VkExtent2D SwapchainExtent() const; 

bool InitImGuiVulkanBackend();               // called by imguiruntime, not App or game code
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
| `DrawItem` | `renderer/Renderer.h` | `{mesh, model, normalMatrix, tint, material}` — `model` is the world transform, not premultiplied by any camera |
| `RenderView` | `renderer/VulkanRenderer.h` | `{rect, items, viewProj, cameraPos}` — one camera's `DrawItem`s, clipped to a screen rect; `cameraPos` only the sky pass reads |
| `Viewport`, `ViewportId` | `renderer/RenderSystem.h` | `RenderSystem`'s per-viewport state: rect plus a World `Entity`, an external camera, or neither (fallback) |
| `MeshRenderer`, `Camera` | `renderer/components/` | Handles, and `fovY`/`near`/`far`, declared here rather than in `core` |
| `MeshBounds` | `renderer/components/MeshBounds.h` | `{mLocalMin, mLocalMax}`, a mesh's own local-space AABB; attached by `ResolveSceneMeshes` (see [app.md](app.md)), read by the editor's viewport click-picking (see [editor.md](editor.md)) — not used for rendering at all |
| `Ray`, `RayIntersectsAABB`, `WorldRay`, `MakeCameraRay`, `RayIntersectsBox` | `renderer/CameraMath.h` | `WorldRay` is a camera ray with a double `WorldPosition` origin and a float direction; `RayIntersectsBox` places a `WorldPose` box relative to that origin in double, then runs the local ray/AABB slab test — the same file `MakeViewProjection` lives in, so both stay in the same Vulkan-convention math |
| `RenderSystem` | `renderer/RenderSystem.h` | The ECS-to-renderer bridge, `SystemPhase::Render` |
| `Shapes.h` | `renderer/Shapes.h` | `MakeCube()` and friends — plain vectors, no Vulkan |
| `RenderSettings` | `renderer/RenderSettings.h` | Project Settings' `"rendering"` section (`rayTracing: bool`, `fog: bool`); `LoadFile`/`SaveFile`, a World resource loaded in `App::Initialize` |

## Rules

**Vulkan 1.4 with `dynamicRendering`, `synchronization2`, `timelineSemaphore` and
`shaderDrawParameters`.** A device missing any of them is rejected during
selection, with a log line naming what was missing.

The floor is roughly 2020 hardware with a current driver. That is acceptable for
a project that is not shipping to consumers.

**Ray tracing (`VK_KHR_acceleration_structure` + `VK_KHR_ray_tracing_pipeline`)
is detected, never a rejection gate.** `FindPhysicalDevice` probes the winning
device for the 3 extensions (the two above plus their
`VK_KHR_deferred_host_operations` dependency) and the two feature bits,
storing the result in `mRayTracingSupported` and logging either way - a GPU
lacking them stays fully usable for raster, same floor as today.
`CreateLogicalDevice` only requests the 3 extensions and splices
`VkPhysicalDeviceAccelerationStructureFeaturesKHR`/
`RayTracingPipelineFeaturesKHR` onto the `pNext` chain when
`mRayTracingSupported` is true; false, the chain is byte-for-byte what it
was before ray tracing existed. Every RT object downstream (BLAS on
`GpuMesh`, the TLAS/SBT/pipeline/output-image state on `VulkanRenderer`
itself) follows the same shape: null/empty when unsupported, and
`Initialize`/`CreateMesh` skip creating it outright rather than creating and
never using it.

**TLAS, its scratch buffer, the instance buffer it's built from, and the RT
output image are each one array of `kFramesInFlight`, not one shared
instance** - same reasoning as `mCameraBuffers`/`mDepthImages`: two frames'
GPU work can be in flight at once, and a shared, every-frame-rewritten
resource would let frame N's rebuild stomp on data frame N-1's GPU commands
haven't finished reading yet. The TLAS itself is created once, sized for
`kMaxRtInstances` (a fixed cap - see Backlog), and every `TraceFrame` call
rebuilds it *in place* with however many `DrawItem`s that frame actually has
a BLAS for (`RecordTlasBuild`) rather than recreating the acceleration
structure object each time.

**A mesh's BLAS is built eagerly in `CreateMesh`, in the same one-shot
command buffer as its vertex/index upload**, immediately after the copies
that write those buffers (with a copy-write -> AS-build-read barrier
between them - see `BuildBLAS`). Same "everything happens at load time"
posture `CreateMaterial`'s eager pipeline build already has. A BLAS build
failure is logged and leaves the mesh's BLAS null; it does not fail mesh
creation; `RecordTlasBuild` simply skips any `DrawItem` whose mesh has no
BLAS. `BuildBLAS`'s scratch buffer is *not* freed by `BuildBLAS` itself - the
command buffer it built into hasn't been submitted yet when it returns, so
`CreateMesh` frees it after its own submit+`vkQueueWaitIdle`, the same way
it already frees its staging buffer.

**The shader binding table's buffer is allocated with
`vmaCreateBufferWithAlignment`, not the shared `CreateBuffer` helper** -
every SBT region's device address must itself be a multiple of
`shaderGroupBaseAlignment`, which an ordinary allocation does not guarantee.
Three regions (raygen/miss/hit), one shader-group handle each, all
individually rounded up to that same alignment for simplicity (a fourth,
callable, stays zeroed/unused - nothing traces callable shaders here).

**`TraceFrame` is a second, independent entry point next to `DrawFrame`, not
a parameter on it.** It shares `DrawFrame`'s acquire/wait/submit/present
skeleton (duplicated, not extracted - see `RecordRtFrame` vs
`RecordCommands`) so that "no change to raster behavior when RT is off"
holds by construction: `DrawFrame` itself is untouched by this feature.
`RecordRtFrame` traces `views[0]` only, clipped to that viewport's own rect
the same way `RecordView` clips raster (`ResolveViewRect`, shared by both -
a zero-extent `RenderView::rect` means "the whole swapchain," anything else
gets clamped to it). `vkCmdTraceRaysKHR`'s dispatch width/height is the
rect's extent, not the swapchain's, so an editor's docked scene panel (or
one half of a split screen) only costs rays over its own pixels. Ray-gen
writes into `mRtOutputImages[mFrameIndex]` (`R8G8B8A8_UNORM`, `STORAGE_BIT`
- the swapchain's own images carry no storage usage), which is always
swapchain-sized regardless of the rect, so `RtPushData::outputOffset` (a
push constant, `rect.offset`) tells ray-gen where in it this dispatch's
`(0,0)` actually lands - `raytrace.slang` computes NDC from
`DispatchRaysIndex()`/`DispatchRaysDimensions()` exactly as before (both
already scoped to the rect by the dispatch call itself, so no shader-side
rect math beyond the output offset), but writes `gOutput[pixel +
outputOffset]` instead of `gOutput[pixel]`. `RecordRtFrame` first
`vkCmdClearColorImage`s the *whole* swapchain image to `mClearColor` -
matching `RecordCommands`' `LOAD_OP_CLEAR` over its whole render area -
before **blitting**, not copying, only that same rect region from the
output image over it: a raw `vkCmdCopyImage` assumes identical texel
layout, and the swapchain's preferred format (`ChooseSurfaceFormat` picks
`B8G8R8A8_SRGB` when available) has its R/B channels swapped relative to
the output image's `R8G8B8A8`; a blit performs that conversion, a copy
would not. Without the whole-image clear first, every pixel outside the
traced rect - an editor's other docked panels, any gap between them -
would be left as undefined memory rather than the ordinary clear color,
since the blit only ever touches its own rect. A collapsed viewport
(`rect.extent` zero on either axis) skips the trace and the blit outright
rather than issuing a zero-sized dispatch - the clear alone still runs, so
the window still shows the clear color instead of stale/undefined content.
`RenderSystem::OnUpdate` calls `TraceFrame` instead of `DrawFrame` when
`RenderSettings::rayTracing` is on *and* `RayTracingSupported()` - the
`rayTracing` Project Settings checkbox existed already (see Changed on
2026-09-17) but was wired to nothing until now. On unsupported hardware
with the checkbox on, it falls back to `DrawFrame` with a one-shot warning,
the same pattern `mWarnedNoCamera` already uses.

**`closestHitMain` gets no interpolated vertex attributes for free the way
rasterization does - it fetches them itself, off the mesh's own vertex/
index buffers, through native Slang pointers, the same buffers `BuildBLAS`
already made `SHADER_DEVICE_ADDRESS`-capable.** First attempt used
`vk::RawBufferLoad<T>(uint64_t address)`, the HLSL-for-Vulkan idiom - it
does not exist in this Slang toolchain (`error[E30027]: member not found`
on `vk::RawBufferLoad`). Slang's own maintainers' documented answer for
this exact situation (buffer-device-address data in Vulkan) is to declare
the field itself as a native pointer type (`T*`), not a `uint64_t` cast at
the call site - `vkGetBufferDeviceAddress`'s 64-bit value and a Slang
pointer are the same bit pattern, so the C++ side (`GetBufferAddress`)
needed no change at all, only the shader-side field types
(`RtInstanceInfo::vertexAddress`/`indexAddress` are `float*`/`uint*`, not
`uint64_t`) and how they're read (`ptr[i]`, ordinary pointer-array
indexing in units of the pointee - `float`, so vertex `i`'s data starts at
`ptr[i * 9]`, 9 floats being `Vertex`'s 36-byte stride - not
`vk::RawBufferLoad`'s manual byte-offset arithmetic).
`RecordTlasBuild` writes a second, parallel per-frame array alongside the
`VkAccelerationStructureInstanceKHR` one it already built - one
`RtInstanceInfo` (vertex/index buffer addresses, tint, the same 3-column
normal matrix `PushData` uses for raster, and a lit/unlit flag) per traced
instance, same index. `VkAccelerationStructureInstanceKHR::instanceCustomIndex`
is what ties the two together - it's set to that same index now (was
hardcoded `0`, harmless while nothing read it, not once `InstanceID()`
became how the hit shader finds its `RtInstanceInfo`). The lit/unlit flag
comes from `GpuMaterial::mUnlit`, set once in `CreateMaterial` from
`desc.shaderName == "unlit"` - the only reason a `MaterialHandle` couldn't
answer "which shader was this built from" before now is that nothing
needed to ask; the shader name was read once, at `BuildPipeline` time, and
discarded. `RecordTlasBuild` resolves each item's material the same way
`RecordView` already does (`FindMaterial`, falling back to
`mDefaultMaterial` on a miss) rather than inventing a second fallback
rule. `closestHitMain` fetches the hit triangle's 3 indices, then either
its 3 vertex colors (unlit - `Vertex::color`, `renderer/Mesh.h`, offset 3
floats into its 16-float stride) or its 3 normals (lit, offset 6),
barycentric-interpolates, and runs the exact same math its raster
counterpart does:
`vertexColor * tint` for unlit, `default.slang`'s
ambient+diffuse+Blinn-Phong (down to the same `kAlbedo`/`kSpecularStrength`/
`kShininess` constants) for lit, both finished off with the same
exponential-fog blend both raster shaders already apply -
`RayTCurrent()` (the ray's own hit distance) standing in for raster's
`length(cameraPosWS - worldPos)`, since the ray origin already *is* the
camera and its direction is already normalized.

**`missMain` renders `sky.slang`'s real Nishita atmosphere, not a flat
vertical-gradient placeholder — issue #80.** `Atmosphere()`/`AtmosphereHeight()`/
`RaySphere()`, their constants, and a shared `TonemapReinhard()` moved into
`AtmosphereCommon.slang`, `#include`d by both `sky.slang` and `raytrace.slang`
rather than duplicated between them. `missMain` computes the same
planet-relative ray origin `sky.slang`'s `fragmentMain` does from the same
`cameraPosWS`, calls `Atmosphere()` with `WorldRayDirection()` in place of
raster's screen-space `rayDir`, and applies the same tonemap - same sky,
either path. This needed `cmake/Shaders.cmake`'s dependency tracking to
learn about `#include`, since `engine_add_shaders`'s `DEPENDS` previously
only named the one source file passed to it: `_engine_scan_shader_includes`
now greps each source (and transitively, each file it includes) for
`#include "..."` lines at configure time and adds every hit as an extra
`DEPENDS`, so Ninja rebuilds every shader that includes a shared file the
moment that file's mtime changes, with no reconfigure needed.

**`RenderSettings` owns only `project_settings.json`'s `"rendering"` key**,
the same way `InputMap` (see the `input` module) owns only `"input"`. Each
type's `SaveFile` goes through `UpdateJsonFile` (see
[serial.md](serial.md)), which rewrites only its own key and leaves every
sibling key on disk untouched, so the two can be saved independently without
one clobbering the other.

`RenderSettings::LoadFile` returns `std::optional`. It is `nullopt` only when
the file cannot be read as a JSON object at all. A readable file whose
`"rendering"` key is absent, or holds something other than an object, loads as
defaults instead — a file from before sections existed has no `"rendering"` key
and is the ordinary case. `App::Initialize` turns the `nullopt` into defaults
with `value_or`, so no system has to check.

The key is read with `find` rather than nlohmann's `value`. `value` hands back
the raw node when the key exists, and reading `"rayTracing"` out of a
`"rendering"` that is `null` or a number throws `type_error 306`, which nothing
on the startup path catches.

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
layout carries the same set-0 layout: binding 0 is the base color texture,
binding 1 is the normal map, both combined image samplers read in the
fragment stage.

**Set 1 is the camera, a dynamic uniform buffer bound once per `RenderView`.**
One buffer per frame-in-flight, sliced into up to `kMaxViewsPerFrame` views;
`RecordView` writes its `RenderView::viewProj` into its slice and binds set 1
with a dynamic offset before its per-item loop, so several viewports/cameras
can share one command buffer without colliding. Only `DrawItem::model` (the
world transform) and the normal matrix still travel per-item, in the push
constant.

Set 1's binding `stageFlags` gained `VK_SHADER_STAGE_RAYGEN_BIT_KHR` and
`VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR` (`mRayTracingSupported`-gated, same
as everything else RT) alongside the existing vertex/fragment flags —
`raytrace.slang`'s `rayGenMain` and `closestHitMain` both read `CameraData`
(`invViewProj`/`cameraPosWS`, same fields `sky.slang` reads, plus
`lightDirWS`/`ambientColor`/`fogColor` for `closestHitMain`'s shading).
Missing either flag failed `vkCreateRayTracingPipelinesKHR` outright, one
stage at a time as each shader started actually reading the buffer
(`VUID-VkRayTracingPipelineCreateInfoKHR-layout-07988` — a shader's stage
must be in the binding's `stageFlags`, not just declared as a set/binding
number match) - broadening `stageFlags` is free at runtime for the
vertex/fragment shaders that already use it, since the flag only says who's
*allowed* to read the binding, not who does.

**The same set-1 buffer also carries the scene's one directional light and
flat ambient term**, read in both the vertex and fragment stages now
(`VkDescriptorSetLayoutBinding::stageFlags` covers both). There is no
separate light buffer or per-light entity yet, so `SetDirectionalLight`/
`SetAmbientColor` set renderer-wide state that `WriteCameraData` copies into
every view's slice, duplicated across views like the rest of that struct.

**`CameraData`'s field order is append-only.** `viewProj`/`lightDirWS`/
`lightColor`/`ambientColor` must stay first, in that order — inserting a
field before that prefix silently corrupts what it *does* read (this broke
lit shading once, while adding the sky pass — see Changed). `default.slang`
and `sky.slang` both declare the full struct - `default.slang` for
`cameraPosWS` (its specular view vector), `sky.slang` for both
`invViewProj` and `cameraPosWS` (reconstructing a world-space ray per
pixel) - so anything new still goes after `cameraPosWS`, never in the
middle. `unlit.slang` also now declares the full struct (it needs
`cameraPosWS` for fog distance and `fogColor` itself), so every current
material shader reads the complete layout; a future shader that truly
needs nothing past `viewProj` can still declare only that leading prefix.

**The sky is its own pipeline with its own (smaller) layout** — set 0 is
`mCameraSetLayout` and nothing else, no material set, no push constants —
built by `CreateSkyPipeline` alongside `CreatePipelineLayout` rather than
going through `CreateMaterial`/`MaterialDesc`, since it isn't a per-mesh
material and needs no vertex buffer (its vertex shader synthesizes a
fullscreen triangle from `SV_VertexID`). Its depth-stencil state is
`LESS_OR_EQUAL`/no-write against the same depth image opaque items wrote, so
early-z skips the fragment shader — a ~128-tap ray march — everywhere
opaque geometry already covered.

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

**`DestroyMesh`/`DestroyMaterial` free one handle at a time, unlike
`ReleaseSceneResources`, and unlike it they *do* bump the slot's generation**
— immediately, at destroy, not when the slot is later reused — so every
outstanding copy of that handle fails `FindMesh`/`FindMaterial` from the
moment it's destroyed, not just once something else happens to occupy its
slot. The freed index goes on a small free-list (`mFreeMeshIndices`/
`mFreeMaterialIndices`) that the next `CreateMesh`/`CreateMaterial` checks
before growing the vector, so a create/destroy/create cycle reuses the slot
rather than growing forever. Both wait for the device to go idle first, same
reason as `ReleaseSceneResources` — an in-flight frame may still be drawing
from the buffers/pipeline about to be freed. **Same caller-vouches-for-
handles rule as `ReleaseSceneResources`**, just at one-handle granularity: a
`MeshRenderer`/material reference that outlives its handle's destroy isn't
prevented, it just starts missing in `FindMesh`/`FindMaterial` and silently
stops drawing (`RecordView`'s existing null-mesh skip / default-material
fallback), rather than crashing. `DestroyMaterial` additionally always
refuses on `mDefaultMaterial` — `CreateMaterial`'s own invalid-handle
fallback needs it alive for the process lifetime, the same reason
`ReleaseSceneResources` special-cases it. Textures have no per-handle
destroy yet (see Backlog); `FindMaterial` now checks `GpuMaterial::mGeneration`
too, matching the mesh/texture slots — it previously only bounds-checked,
since nothing before this could make a stale material index possible.

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
`RenderView`, because ImGui manages its own per-window clip rects. The draw data
comes from `ImGuiRuntime::EndFrame`, which `App` passes through.

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

1. **Per-handle destroy for textures.** Meshes and materials have it
   (`DestroyMesh`/`DestroyMaterial` — see Changed); textures still only go
   away in bulk, via `ReleaseSceneResources`/`DestroyTextures`. Lower
   priority than meshes/materials were: nothing today creates or discards
   textures at a rate that makes the gap felt.
2. **Draw-item sorting and batching.** There is one pipeline bind per item today,
   so a frame scales with the number of distinct materials rather than with draw
   count.
3. **Lighting beyond one hardcoded directional light + flat ambient.** No
   point/spot lights, no shadows, no per-entity `Light` component yet — the
   direction/color/ambient are renderer-wide state set via
   `SetDirectionalLight`/`SetAmbientColor`, not scene data.
4. **Per-config shader output paths.** Release currently ships unoptimised
   shaders, and the debug flags cannot become conditional while both
   configurations write to a path that carries no configuration component.
5. **A real renderer interface.** It is the prerequisite for a headless mode, and
   therefore for testing anything in this module or in `app`.
6. **A second viewport actually on screen at once.** The mechanism (`AddViewport`)
   exists and is tested by `App` using only viewport 0; nothing yet adds a second
   one for, say, an editor-camera view alongside the game view during a paused
   Play session.
7. **No cubemap/gradient sky option, and no time-of-day driver.** The sky is
   Nishita-only — there's no `MaterialDesc`-style switch to a static cubemap
   asset or a cheap two-color gradient for scenes that don't want the
   ray-march cost. Sun elevation is whatever direction `SetDirectionalLight`
   is holding; nothing animates it over time yet (a script/system driving
   `SetDirectionalLight` on a timer would get a day/night cycle for free,
   since sky and lit shading already read the same UBO fields — see the
   set-1 rules above). There's also no moon/stars, so full night is flat
   black rather than lit by anything but `SetAmbientColor`.
8. **RT still isn't full parity with raster — issues #55 and #80 closed the
   material-shading and sky gaps, not the rest.** `kMaxRtInstances` (256) is
   a fixed cap with no overflow handling beyond silently dropping the rest;
   the TLAS rebuilds fully every `TraceFrame` call with no dirty-tracking
   (see the Rules section above - correct, just not efficient); only the
   two shaders that exist (`default`/`unlit`) get RT parity, so a third
   custom material shader would trace with whichever of those two
   `GpuMaterial::mUnlit` happens to pick, not its own look; and
   `TraceFrame` only ever traces `views[0]` - a second viewport on screen
   at once is backlog item 6 regardless of raster/RT, and RT inherits that
   gap rather than fixing it.

## Changed

*2026-09-29* — **Camera rays keep a double origin, and the projection names
its clip convention.** Was: `MakeCameraRay(cameraWorld, viewProj, ndc)` returned
a float `Ray` whose origin was the camera's float world position, and
`MakeViewProjection` called `glm::perspective` under a file-local
`GLM_FORCE_DEPTH_ZERO_TO_ONE`. `Editor.cpp` instantiated the same inline
template without the macro, and in the Debug `MjolnirEngine.exe` the linker kept
that -1..1 copy for both, so the renderer's near plane sat at about twice
`nearZ`. Now: `MakeCameraRay(cameraPose, lens, aspect, ndc)` returns a
`WorldRay` (double `WorldPosition` origin, float direction) and
`RayIntersectsBox` subtracts that origin from the box's `WorldPose` in double
before the slab test; `MakeViewProjection` calls `glm::perspectiveRH_ZO`
outright and the macro is gone.

*2026-09-27* — **RT's `missMain` renders the real sky, and vertex-stride
drift between raster and RT got fixed along the way — issues #55 and #80.**
`mir::Vertex` grew from 9 floats (pos/color/normal) to 16 (uv/alpha/tangent
added for texturing and skinned models) without `raytrace.slang`'s
`closestHitMain` noticing - its hardcoded `kVertexStrideFloats` was still 9,
so it read color/normal off the wrong byte offsets for any mesh with more
than one vertex; now 16, matching `mir::Vertex` (`BuildBLAS`'s own
`vertexStride = sizeof(Vertex)` had already tracked the change correctly, so
only the shader-side constant was stale). Separately: `Atmosphere()`,
`AtmosphereHeight()`, `RaySphere()`, their constants, and a new shared
`TonemapReinhard()` moved out of `sky.slang` into `AtmosphereCommon.slang`,
which both `sky.slang` and `raytrace.slang` now `#include`; `missMain`
computes the same planet-relative ray origin `sky.slang`'s `fragmentMain`
does and calls the same `Atmosphere()`, so RT and raster render the same sky
from the same light direction instead of RT falling back to a flat
vertical-gradient placeholder. Getting there needed
`cmake/Shaders.cmake` to understand `#include` at all:
`_engine_scan_shader_includes` greps a shader source (and, transitively,
anything it includes) for `#include "..."` lines at configure time and adds
every hit to the custom command's `DEPENDS`, so editing a shared include now
rebuilds every `.spv` that includes it, not just the file passed as
`SOURCE` - verified by touching only `AtmosphereCommon.slang` and watching
both `sky`'s and `raytrace`'s shader targets rebuild with no `cmake`
reconfigure. `default`/`unlit`/`model`'s shader build path is untouched.
Issues #55, #80.

*2026-09-22* — **RT closest-hit shading reads real material data — issue
#55.** Was: `closestHitMain` wrote a flat barycentric tint regardless of
what was hit; `missMain` wrote one flat color. Now: `RecordTlasBuild`
writes a second per-instance array (`RtInstanceInfo` - vertex/index buffer
addresses, tint, normal matrix, a lit/unlit flag) alongside the
`VkAccelerationStructureInstanceKHR` one it already built, indexed by
`instanceCustomIndex` (now actually set to the instance's index, was
hardcoded `0`); a new set-0 binding 2 (`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`,
closest-hit stage only) exposes it. `GpuMaterial` gained `mUnlit`
(`CreateMaterial`, from `desc.shaderName == "unlit"`) so `RecordTlasBuild`
can tell the two apart per item, same `FindMaterial`-with-fallback-to-
default lookup `RecordView` already does. `closestHitMain` fetches its
triangle's vertices off the mesh's own buffers through native Slang
pointers (`float*`/`uint*` struct fields, not `vk::RawBufferLoad` - see
Rules for why), the same `SHADER_DEVICE_ADDRESS`-capable buffers
`BuildBLAS` already needed, barycentric-interpolates color or normal
depending on the flag,
and runs the same math `unlit.slang`/`default.slang` do (down to
`default.slang`'s exact Blinn-Phong constants), finished with the same
exponential fog both raster shaders apply. `missMain` got a cheap vertical
gradient instead of one flat color - still not `sky.slang`'s atmosphere,
see Rules/Backlog for why that's a separate piece of work. Criteria 2
(BLAS/TLAS rebuild on mesh add/remove) and 3 (runtime raster/RT toggle)
were already satisfied by #50's design - full-every-frame TLAS rebuild is
a superset of "rebuild on change," and `RenderSystem::OnUpdate` already
re-read the toggle every frame - re-verified manually rather than
re-implemented. Issue #55.

*2026-09-22* — **`TraceFrame` clips to the viewport rect, same as raster.**
Was: `RecordRtFrame` traced and blitted the whole swapchain regardless of
`RenderView::rect`, which is harmless for a standalone game window
(viewport 0 *is* the whole window there) but broke the editor the moment
the `rayTracing` checkbox was flipped on - the trace painted over every
docked panel, not just the scene viewport, while click-to-select and
everything else still assumed raster's own rect-clipped output. Now:
`ResolveViewRect` (new, shared with `RecordView`) computes the same clamped
rect either path uses; `vkCmdTraceRaysKHR` dispatches only that rect's
extent; a new push constant (`RtPushData::outputOffset` in
`raytrace.slang`, `VkPushConstantRange` on `mRtPipelineLayout`) tells
ray-gen where that dispatch's `(0,0)` lands inside the still-swapchain-sized
`mRtOutputImages`; and `RecordRtFrame` clears the whole swapchain image
first (matching `RecordCommands`' `LOAD_OP_CLEAR`) before blitting only the
rect region over it, so pixels outside the traced viewport are the ordinary
clear color instead of undefined memory. A collapsed viewport (rect extent
zero) skips the trace/blit rather than issuing a zero-sized dispatch.

Caught during the user's own manual test pass, the same way the four
validation bugs below were - toggling the checkbox in-editor made
selection, sky, fog and materials all look broken at once, which was
actually one root cause (RT painting over the whole window) plus the
already-known "RT replaces raster entirely, doesn't composite with it"
limitation (still true, still Backlog item 8) working together.

*2026-09-22* — **Four validation-layer bugs in the RT baseline above, found
by actually running it.** All four were `MIR_DEBUG_BREAK()`-on-validation-
error crashes (see `DebugCallback`), one per run, each fixed and re-tested
in turn:
1. `mCameraSetLayout`'s binding `stageFlags` didn't include
   `VK_SHADER_STAGE_RAYGEN_BIT_KHR`, so `vkCreateRayTracingPipelinesKHR`
   failed (`VUID-VkRayTracingPipelineCreateInfoKHR-layout-07988`) the moment
   `raytrace.slang`'s `rayGenMain` was found reading `CameraData` from a set
   whose layout didn't admit the raygen stage. Fixed by adding the flag
   (`mRayTracingSupported`-gated, like everything else RT touches here).
2. `BuildBLAS`'s copy-write→build-read barrier used
   `VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR` for the vertex/index
   buffers. Wrong category: that access type is for a shader reading an
   acceleration structure *object* (correct elsewhere, e.g. the TLAS
   build→trace barrier); reading geometry *input data* during a build is
   `VK_ACCESS_2_SHADER_READ_BIT`. The validation message names the correct
   flag directly.
3. The imgui-compositing barrier (`TRANSFER_DST_OPTIMAL` ->
   `COLOR_ATTACHMENT_OPTIMAL`) only granted `COLOR_ATTACHMENT_WRITE_BIT`,
   but that render pass uses `LOAD_OP_LOAD` (keeping the blit's pixels),
   which reads - needed `COLOR_ATTACHMENT_READ_BIT` too.
4. The swapchain image's first barrier each frame
   (`UNDEFINED`->`TRANSFER_DST_OPTIMAL`) used `VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT`
   as `srcStageMask`. `TraceFrame`'s submit waits on the acquire semaphore
   at `COLOR_ATTACHMENT_OUTPUT_BIT` (same as `DrawFrame`'s), and a
   semaphore wait only gates commands at the stage(s) it names - a
   `TOP_OF_PIPE` barrier isn't gated by it at all, so this barrier's write
   to the freshly-acquired image could execute before the acquire actually
   completed. `RecordCommands`' own equivalent barrier already used
   `COLOR_ATTACHMENT_OUTPUT_BIT` for exactly this reason; `RecordRtFrame`
   just didn't copy that part.

*2026-09-22* — **RT baseline: acceleration structures + a ray tracing
pipeline, opt-in alongside raster.** Was: no RT code at all; the `rayTracing`
Project Settings checkbox (see the 2026-09-17 entry below) existed but was
wired to nothing. Now: `FindPhysicalDevice` detects
`VK_KHR_acceleration_structure`/`VK_KHR_ray_tracing_pipeline` support
(`mRayTracingSupported`, never a rejection reason) and
`CreateLogicalDevice` conditionally requests them; `CreateMesh` builds each
mesh's BLAS eagerly (`BuildBLAS`, same command buffer as its vertex/index
upload) when supported, and `DestroyMesh`/`DestroyMeshes` free it
(`DestroyBLAS`); a new `VulkanRenderer::TraceFrame`, structurally parallel
to `DrawFrame` (see `RecordRtFrame` vs `RecordCommands`), rebuilds a
per-frame-in-flight TLAS from that frame's `DrawItem`s every call
(`RecordTlasBuild`), runs a new 3-shader RT pipeline
(`assets/shaders/raytrace.slang`: `rayGenMain`/`missMain`/`closestHitMain`,
registered in the root `CMakeLists.txt`) that writes into a per-frame RT
output image, then blits that into the swapchain image.
`RenderSystem::OnUpdate` now calls `TraceFrame` instead of `DrawFrame` when
`RenderSettings::rayTracing` is on and `RayTracingSupported()`, falling
back to raster (once-warned) otherwise. See the Rules section above for
the full shape of this (device-gating, per-frame-in-flight duplication,
BLAS/SBT lifetime) — deliberately a proof of concept per issue #50, not a
usable render mode; see Backlog. Textures are untouched. Issue #50.

*2026-09-22* — **`DestroyMesh`/`DestroyMaterial`: per-handle destroy for
meshes and materials.** Was: only `ReleaseSceneResources` could free
anything, all meshes/materials at once, and a stale handle was never
detected for materials at all (`FindMaterial` only bounds-checked; `GpuMaterial`
had no generation field). Now: both destroy one handle, validate it first
(null/out-of-range/stale generation all rejected, logged, `false` returned),
`vkDeviceWaitIdle` before touching any Vulkan object (same in-flight-frame
concern `ReleaseSceneResources` already had), then free the buffers/pipeline
+ descriptor set, bump the slot's generation so every outstanding copy of
the handle is immediately stale, and push the index onto a small free-list
(`mFreeMeshIndices`/`mFreeMaterialIndices`) that `CreateMesh`/`CreateMaterial`
now check before growing their vector — a create/destroy/create cycle
reuses the slot instead of growing it forever. `GpuMaterial` gained
`mGeneration` (same `0 = never filled` convention as `GpuMesh`/`GpuTexture`),
and `CreateMaterial`'s returned handle now carries its slot's real
generation instead of a hardcoded `0`; `ReleaseSceneResources`'s
default-material-repositioning line was fixed to match (`MaterialHandle{0,
defaultMaterial.mGeneration}`, not `{0, 0}`) since a hardcoded `0` there
would have started rejecting `mDefaultMaterial` itself the moment
`CreateMaterial` began returning real generations. Caller-vouches-for-
handles, same decision `ReleaseSceneResources` already made, now
documented for the one-handle case too (see Rules). Textures untouched —
out of scope for this pass, tracked in Backlog. Issue #36.

*2026-09-22* — **Distance fog gained a Project Settings toggle.** Was:
`SetFog` had no caller, so fog never showed regardless of build. Now:
`RenderSettings` gained `fog: bool` (own `"rendering"` key, same
load/save/round-trip rules as `rayTracing`), `ProjectSettingsPanel::DrawRendering`
exposes it as a "Distance Fog" checkbox next to "Ray Tracing", and
`RenderSystem::OnUpdate` reads `RenderSettings::fog` off the World each
frame and calls `mRenderer.SetFog(...)`, a fixed hazy-blue color at a fixed
density when on, density 0 when off. No color/density UI yet - those still
only exist for callers to set directly through `VulkanRenderer::SetFog`;
this toggle just picks between "off" and one fixed on-state.

*2026-09-22* — **Distance fog.** Was: geometry rendered at full color all the
way to the far plane, then vanished the instant it crossed it - a hard clip
rather than a fade. Now: `CameraData` gained `fogColor` (rgb = color, a =
density), appended after `cameraPosWS` per the append-only rule; `VulkanRenderer::SetFog(color, density)`
is renderer-wide state `WriteCameraData` copies in, same pattern as
`SetDirectionalLight`/`SetAmbientColor`, and defaults to density 0. Both
`default.slang` and `unlit.slang` blend their shaded color toward
`fogColor.rgb` by `1 - exp(-density * distance)` (distance = camera to
world-space fragment), so surfaces fade into whatever color the fog is set
to - typically the sky/horizon color - instead of disappearing at the far
plane. `unlit.slang` needed two additions to do this: a `worldPos` varying
(it previously only carried vertex color) and the full `CameraData` struct
declared in place of just `viewProj`, since reading `fogColor` means every
field ahead of it in the buffer must be declared too. Density 0 makes the
blend factor exactly 0 for every fragment, so this is a no-op against
pre-fog rendering - nothing calls `SetFog` yet, matching `SetDirectionalLight`/
`SetAmbientColor`, which also have no caller outside their own default
member values (see the lighting backlog item).

*2026-09-21* — **`CameraMath.h` gained a camera-ray builder and a ray/AABB
test, and meshes gained a `MeshBounds` component.** Was: the file only
built `viewProj`; there was no unprojection/ray helper anywhere, and no
bounding-box concept at all. Now: `MakeCameraRay(cameraWorld, viewProj,
ndc)` unprojects a screen click into a world-space `Ray` (same technique as
`sky.slang`'s CPU-side counterpart), and `RayIntersectsAABB` is a standard
slab test. `MeshBounds` (`renderer/components/MeshBounds.h`) is a mesh's
local-space min/max, attached by `SceneResolve.cpp`'s `ResolveSceneMeshes`
(app module) right where a fresh mesh's CPU vertex data is briefly
available before `CreateMesh` uploads and discards it - the only place in
the engine CPU-side positions still exist even momentarily. None of this
is renderer/draw-path code; it exists so the editor can ray-pick objects in
the viewport (see [editor.md](editor.md)) without the editor needing to
know `VulkanRenderer` exists at all, or the renderer needing to retain
mesh geometry permanently just to support picking.

*2026-09-21* — **The default material is a flat grey, non-metallic
Blinn-Phong shader, `assets/shaders/default.slang`.** Was:
`triangle.slang`, `MaterialDesc::shaderName`'s default, tinted each mesh's
own baked vertex colors by the scene light — the multi-coloured cube in the
HelloWorld sample was this material picking up its mesh's per-vertex
colours. Now: `default.slang` (the renamed, rewritten file) ignores
`input.color` entirely and shades a fixed mid-grey albedo (tinted by
`pushData.tint`, same as before) with Lambertian diffuse + ambient plus a
fixed-intensity Blinn-Phong specular highlight, deliberately similar to
Unity's default Standard/Lit material ball. `MaterialDesc::shaderName`'s
default changed from `"triangle"` to `"default"` to match. Needs
`cameraPosWS` for the specular view vector, so it's the second shader (after
`sky.slang`) that declares `CameraData`'s full field list rather than just
the leading prefix — see the append-only rule above. `unlit.slang` is
unchanged and is now the only shader that still reads vertex colour.

*2026-09-21* — **The sky covers the full sphere around the camera, not just
the upper hemisphere.** Was: `Atmosphere` in `sky.slang` clamped the primary
ray to a `kPlanetRadius` ground intersection and broke out of the secondary
(sun) ray march the moment a sample dipped underground, so any view ray
with a meaningful downward component-common from an elevated/angled
camera-collapsed to near-zero step size or an outright occluded sun and
rendered flat black, like a hard-edged hemisphere rather than a skybox. Now:
there is no ground/planet occlusion at all; `AtmosphereHeight` clamps
sample height to `max(length(pos) - kPlanetRadius, 0.0)` instead, so
"underground" samples just read as sea-level density. The primary ray
integrates all the way to the outer atmosphere-sphere exit (`hit.y`,
unclamped), and the attenuation term's own exponential falloff keeps the
result bounded (dominated by the first step or two for steep-down rays)
without needing an explicit cutoff. Closer to how Unity's procedural
skybox covers a full sphere than to a physically-grounded planet renderer.

*2026-09-21* — **A Nishita-style procedural sky renders behind opaque
geometry.** Was: the background was whatever `SetClearColor` cleared to,
nothing else. Now: `CreateSkyPipeline` builds a dedicated fullscreen-triangle
pipeline (own layout: set 0 = `mCameraSetLayout` only) drawn once per view
after its opaque items (`RecordView`), depth-tested `LESS_OR_EQUAL`/no-write
against the far clear value so early-z limits its ray-marched fragment
shader (`sky.slang`, Rayleigh+Mie single scattering, ported from the
public-domain wwwtyro/glsl-atmosphere reference) to actual background
pixels. `CameraData` (set 1, binding 0) gained `invViewProj` and
`cameraPosWS`, appended after the existing light fields rather than
inserted before them, so `triangle.slang`/`unlit.slang`'s shorter cbuffer
declarations keep reading the right bytes (see the append-only rule above —
getting this wrong the first time round briefly corrupted lit shading,
caught by re-running the app and comparing against the previous screenshot).
The sky's sun is the scene's existing directional light — no separate sun
state — so `SetDirectionalLight`'s direction drives both at once; sweeping
it over time is what gets a day/night cycle, verified by rebuilding with a
few different elevations (noon, sunset, sun below horizon) and confirming
the sky and the lit cube both responded.

*2026-09-21* — **`triangle.slang` shades per-pixel from a UBO light instead of
hardcoding a Gouraud-shaded directional light.** Was: `kLightDir`, `kAmbient`
and `kDiffuse` were `static const` in the shader, and shading was computed
once per vertex and baked into `VSOutput::color`. Now: the set-1 camera UBO
(`CameraData`, both C++ and shader-side) gained `lightDirWS`/`lightColor`/
`ambientColor` fields after `viewProj`, its descriptor binding's
`stageFlags` gained `VK_SHADER_STAGE_FRAGMENT_BIT`, the vertex shader passes
an unnormalized world-space normal through `VSOutput` instead of a shaded
color, and the fragment shader normalizes it and computes
`ambientColor + lightColor * max(dot(N, L), 0)` per pixel.
`VulkanRenderer::SetDirectionalLight`/`SetAmbientColor` set the values
(defaults match the old constants); `WriteCameraData` copies them into every
view's slice alongside `viewProj`. `unlit.slang` is unchanged — it never
reads past `viewProj` in the same buffer.

*2026-09-19* — **Descriptor sets and a per-frame camera UBO replaced the
premultiplied view-projection push constant.** Was: `RenderSystem` baked
`viewProj * world.Matrix()` into `DrawItem::model`, so the camera rode along
in every push constant and the pipeline layout carried only the set-0
material (texture) layout. Now: `DrawItem::model` is the plain world
transform, `RenderView` gained a `viewProj` field, and `VulkanRenderer` added
set 1 — a `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC` binding, one buffer per
frame-in-flight sliced into up to `kMaxViewsPerFrame` (16) views.
`RecordView` writes its view's `viewProj` into its slice and binds set 1 with
a dynamic offset once per view, ahead of the existing per-item set-0
(material) and push-constant (model + normal + tint) binds. `triangle.slang`
and `unlit.slang` now read `viewProj` from a `[[vk::binding(0, 1)]] cbuffer`
and multiply it by the push-constant model matrix in the vertex shader
instead of consuming an already-combined matrix.

*2026-09-18* — **`RenderSettings` file IO moved to `mir::serial`.** Was:
`RenderSettings.cpp` held its own parse, merge and dump cycle. Now it calls
`ReadJsonFile`/`UpdateJsonFile` from [serial.md](serial.md). The `mir::assets`
and `nlohmann_json` dependencies are gone with it, since nothing else in the
module used either.

`LoadFile` returns `std::optional<RenderSettings>` in the same change, so a
missing or unreadable file is distinguishable from one whose `"rendering"`
section simply is not written yet.

`ReadSpirv` now reads through `core/fs/FileIO.h` as well. It copies into a
`std::vector<uint32_t>` rather than reinterpreting the byte buffer, because
`vkCreateShaderModule` requires `pCode` to be four-byte aligned and a
`std::vector<std::byte>` is aligned to one.

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
`App::Initialize`, alongside `InputMap`.

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
