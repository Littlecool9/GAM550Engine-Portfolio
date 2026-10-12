# renderer

The whole Vulkan path — instance through swapchain, meshes, materials, depth and
per-frame recording — plus the render-facing components and the one system that
drives it from the ECS.

`mir::renderer` · depends `mir::core`, `volk`, `glm` (public); `VMA`, `imgui`, `mir::serial` (private) · API `modules/renderer/include/renderer/` · maintainer Sumin Park · reviewed 2026-09-18

## Model

`VulkanRenderer` is the module's public face, and it coordinates rather than
owns. Its Vulkan state lives in subsystems under `src/renderer/vulkan/`, in
namespace `mir::vk`. The public header only forward-declares them and holds
each through a `std::unique_ptr`, so no Vulkan internals reach `app` or game
code. Each subsystem owns its objects and stores the `Device` it was given in
`Initialize`. Anything else it needs arrives as a call parameter, so no
subsystem holds a pointer to the facade or to another subsystem.

| Subsystem | Owns |
|---|---|
| `Device` | instance, debug messenger, surface, physical and logical device, queue, VMA allocator, cached device properties |
| `Swapchain` | swapchain images and views, one depth image per frame in flight, render-complete semaphores, acquire and present |
| `FrameSync` | command pools and buffers, image-acquired semaphores, the frame timeline with `kFramesInFlight = 2` |
| `ResourceStore` | meshes, textures, samplers and the three built-in textures (white, flat normal, error magenta), behind slot/generation handles |
| `MaterialStore` | materials and their pipelines, set 0, the pipeline layout every material shares, the sky pipeline |
| `CameraBuffer` | set 1: per-view camera data and light list, plus the directional light, ambient, fog and atmosphere |
| `ModelDescriptors` | set 2: per-draw model data, reset each frame |
| `ShadowMaps` | set 3: Sun, spot and point shadow maps, their pipelines and casters |
| `RayTracer` | a BLAS per mesh slot, a TLAS per frame, the RT pipeline, SBT and output images |
| `FrameCapture` | the `CaptureNextFrame` buffer and the PNG it writes |
| `ImGuiOverlay` | ImGui's Vulkan backend and the draw data it overlays |
| `MeshUploader` | the persistent staging ring, queued mesh uploads and their BLAS builds, and retired GPU objects until their last frame (#127) |

The facade's own code is `VulkanRenderer.cpp`, which holds initialize, shutdown,
the resource calls and the setters, and `VulkanRendererFrame.cpp`, which holds the
frame loop and pass recording. Pass recording stays on the facade because one
pass reads six subsystems at once.

A frame starts in `BeginFrame`. It recreates the swapchain when the window
changed size, or when `RequestSwapchainRecreate` asked for it, then `FrameSync::Begin` waits for the frame slot, acquires an image
and begins the command buffer. `MeshUploader` then marks the queued meshes of
completed frames Ready, frees retired objects whose last frame completed, and
records this frame's mesh copies and BLAS builds. `DrawFrame` records the shadow passes, then each
view, then the ImGui overlay and any capture, and `FrameSync::End` submits and
presents. `TraceFrame` goes through the same `BeginFrame` and `End`, with
`RayTracer::Record` and a blit to the swapchain image in between.

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

Some draws depend on the viewport: terrain picks its level of detail per
camera. Such a producer is an `IViewDrawSource` registered in the
`ViewDrawSources` World resource (`renderer/ViewDrawSource.h`). `RenderSystem`
resolves every viewport's camera first and hands the list (`ViewCamera`: the
viewport id, pose, lens, pixel extent and camera-relative view-projection) to
each source's `Prepare`, then adds `AppendViewDraws(view)` to that view's world
draws, and for the sun, spot and point shadow passes adds
`AppendVolumeDraws(0, origin, viewProj)`: the source's draws inside that pass's
volume at the primary view's detail (#125).

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
scene, `ReleaseSceneResources` frees them all again, except the default and error materials.

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
TextureHandle ErrorTexture() const;          // 1x1 magenta, for a caller whose texture failed to load
MaterialHandle CreateMaterial(const MaterialDesc &desc);
bool DestroyMaterial(MaterialHandle handle); // same contract; always false on the default and error materials
MaterialHandle ErrorMaterial() const;        // solid magenta, for a caller whose CreateMaterial failed
void ReleaseSceneResources();                // every mesh, material and texture but the built-ins

// Asynchronous meshes (#127): no call waits on the GPU
bool ConfigureMeshStreaming(const MeshStreamingDesc &desc);  // staging ring and per-frame bytes, from the caller's data
MeshHandle QueueMesh(std::vector<Vertex> vertices, std::vector<uint32_t> indices, const MeshUploadOptions &options);
MeshUploadState MeshState(MeshHandle handle) const;          // Stale, Pending, Uploading, Ready or Failed
bool RetireMesh(MeshHandle handle);                          // stale at once, freed after its last frame
uint64_t MeshFootprint(uint32_t vertexCount, uint32_t indexCount, const MeshUploadOptions &options) const;
MeshStreamingStats MeshStreaming() const;                    // queue, uploads, retirement, idle waits, skipped draws

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
| `MeshStreamingDesc`, `MeshUploadOptions`, `MeshUploadState`, `MeshStreamingStats` | `renderer/MeshStreaming.h` | the staging ring and per-frame byte budget; whether a queued mesh gets a BLAS; where a queued mesh is; the counters (bytes queued, uploading and retiring, idle waits, skipped draws, highest generation) |
| `StagingRing`, `MeshUploadQueue`, `FrameRetireList` | `renderer/MeshStreaming.h` | the CPU bookkeeping behind them, tested without a device: ring space by frame, admission under the frame budget, objects kept until their last frame completes |
| `ViewDrawSources`, `IViewDrawSource`, `ViewCamera` | `renderer/ViewDrawSource.h` | World resource of producers whose draws depend on the viewport (app's terrain): `Prepare` with every viewport's camera, then `AppendViewDraws` per view and `AppendVolumeDraws` per shadow volume at a view's detail |
| `TextureDesc`, `TextureHandle` | `renderer/Texture.h` | `{width, height, pixels, srgb}`, tightly-packed RGBA8; `kNullTexture` falls back to a default in `CreateMaterial` |
| `MaterialDesc`, `MaterialHandle` | `renderer/Material.h` | `{shaderName, cullMode, polygonMode, baseColorTexture, normalTexture}`; `kNullMaterial` means the default |
| `DrawItem` | `renderer/Renderer.h` | `{mesh, model, normalMatrix, tint, material}` — `model` is the world transform, not premultiplied by any camera |
| `RenderView` | `renderer/VulkanRenderer.h` | `{rect, items, viewProj, cameraPos}` — one camera's `DrawItem`s, clipped to a screen rect; `cameraPos` only the sky pass reads |
| `Viewport`, `ViewportId` | `renderer/RenderSystem.h` | `RenderSystem`'s per-viewport state: rect plus a World `Entity`, an external camera, or neither (fallback) |
| `MeshRenderer`, `Camera` | `renderer/components/` | Handles, and `fovY`/`near`/`far` (`far` 0 = no far plane), declared here rather than in `core` |
| `MeshBounds` | `renderer/components/MeshBounds.h` | `{mLocalMin, mLocalMax}`, a mesh's own local-space AABB; attached and kept current by `app`'s `MeshResolveSystem` (see [app.md](app.md)), read by the editor's viewport click-picking (see [editor.md](editor.md)) — not used for rendering at all |
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
was before ray tracing existed. Every RT object downstream lives in
`RayTracer` and follows the same shape: `Initialize` never initializes
`RayTracer` on an unsupported device, and `CreateMesh` never asks it for a
BLAS there.

**TLAS, its scratch buffer, the instance buffer it's built from, and the RT
output image are each one array of `kFramesInFlight`, not one shared
instance** - same reasoning as `CameraBuffer`'s buffers and `Swapchain`'s
depth images: two frames'
GPU work can be in flight at once, and a shared, every-frame-rewritten
resource would let frame N's rebuild stomp on data frame N-1's GPU commands
haven't finished reading yet. The TLAS itself is created once, sized for
`RayTracer::kMaxInstances` (a fixed cap - see Backlog), and every `TraceFrame` call
rebuilds it *in place* with however many `DrawItem`s that frame actually has
a BLAS for (`RecordTlasBuild`) rather than recreating the acceleration
structure object each time.

**A mesh's BLAS is built eagerly in `CreateMesh`, in its own submit right
after the mesh upload.** `ResourceStore::CreateMesh` uploads the vertex and
index buffers and returns the handle. The facade then calls
`RayTracer::BuildMeshBlas` with that handle's slot, which records the build
into a second `vk::SubmitAndWait`. The build keeps its copy-write ->
AS-build-read barrier, whose first scope covers the earlier upload submit on
the same queue. The BLAS lives in `RayTracer`, indexed by mesh slot, so
`ResourceStore` never sees ray tracing. Same "everything happens at load time"
posture `CreateMaterial`'s eager pipeline build already has. A BLAS build
failure is logged and leaves the slot's BLAS null; it does not fail mesh
creation; `RecordTlasBuild` simply skips any `DrawItem` whose mesh has no
BLAS. The scratch buffer outlives `BuildBLAS` because the command buffer has
not been submitted when `BuildBLAS` returns, so `BuildMeshBlas` frees it after
the submit and wait.

**A queued mesh never waits: it uploads at the start of a frame and is Ready
once that frame completes (#127).** `QueueMesh` reserves a slot (state
Pending, no buffers) and keeps the vertex and index vectors. At the start of
each recorded frame, after `FrameSync::Begin`'s wait, `MeshUploader` takes
queued meshes in order while they fit `MeshStreamingDesc::frameBytes` (vertex
and index bytes plus the acceleration structure, the first mesh of a frame
even past it) and the persistent staging ring, creates their buffers, copies
through the ring (a mesh larger than the ring gets a staging buffer of its
own), builds a BLAS when `MeshUploadOptions::rayTracing` asks and the device
supports it (all of the frame's builds in one call after one copy-to-build
barrier, on one shared scratch buffer, `RayTracer::RecordMeshBlases`), and ends
with one barrier so later commands on the one queue read the copies and the
BLAS. The mesh is Uploading with that frame's
timeline value and becomes Ready when a later frame start sees the timeline
past it; ring space comes back at the same time. `FindMesh` returns Ready
meshes only, so draws, shadow passes and the TLAS skip anything else, and a
skipped non-null handle counts in `MeshStreamingStats::skippedDraws`.
`RetireMesh` takes the buffers and BLAS out of the slot and moves its
generation on at once; the objects are freed when the timeline passes the
last frame issued at the time of the call (`FrameSync::LastIssuedValue`), and
a mesh still Pending is dropped from the queue. `MeshStreamingStats` counts
queued, uploading and retiring bytes, and every `vkDeviceWaitIdle` and
`vkQueueWaitIdle`, which all go through `Device::WaitIdle` and
`WaitQueueIdle`. `CreateMesh` and `DestroyMesh` keep their synchronous
behaviour, BLAS included.

**Terrain tiles are traced like any other mesh.** Each drawn unit (a tile at
a LOD or a Region) gets its own BLAS and one TLAS instance per frame, so the
project's `rayTracing` setting alone (with device support and a single view)
picks the path, terrain or not; a traced frame holds that single view's
terrain selection. Terrain meshes are queued with a BLAS only while the
project's `rayTracing` is on, so a raster-only project builds none, even on a
device that could.
`RayTracer::kMaxInstances` is 16384 because one 4096 m chunk is already 256 tiles. Two
limits stay open: a tile edge is shared by two instances, and ray/instance
intersection is not watertight across them, so a seam may leak a rare pixel
that raster's `translationGrid` closes; BLAS bytes count against terrain's
GPU budget (#127).

**`ModelDrawItems` is one list; view draw sources answer per view.**
`ModelSystem` rebuilds the model list every frame, and `RenderSystem` appends
it after the `MeshRenderer` query for every view. A `ViewDrawSources` producer
(app's terrain) answers each view and each shadow volume separately, after
`Prepare` has seen every camera of the frame, so two viewports may draw the
same ground at different detail. A source stays registered while it lives and
removes itself before it dies.

**A `translationGrid` makes separate draws meet bit for bit.** Plain
narrowing rounds each draw's camera-relative translation to float on its own,
and two translations of different magnitude round on different steps. Two
terrain tiles that share an edge vertex then compute it one float step apart,
which drops single pixels along the seam. With a nonzero grid (a power of two
in meters), `BuildViewDrawItems` rounds the double offset to that grid instead;
every grid draw then shares one step, and grid-aligned vertices add to it
exactly while the translation is within 2^23 steps (past that it falls back to
plain float rounding). The terrain glue uses 1/4096 m: tiles stay watertight
within 2048 m of the camera, at the price of up to 0.12 mm of shift.

**The shader binding table's buffer is allocated with
`vmaCreateBufferWithAlignment`, not the shared `CreateBuffer` helper** -
every SBT region's device address must itself be a multiple of
`shaderGroupBaseAlignment`, which an ordinary allocation does not guarantee.
Three regions (raygen/miss/hit), one shader-group handle each, all
individually rounded up to that same alignment for simplicity (a fourth,
callable, stays zeroed/unused - nothing traces callable shaders here).

**`TraceFrame` is a second entry point next to `DrawFrame`, not a parameter
on it.** Both go through `BeginFrame` and `FrameSync::End` for the
wait/acquire/submit/present loop. Only the recording between them differs:
`RecordCommands` for raster, `RecordRtFrame` for ray tracing.
`RecordRtFrame` traces `views[0]` only, clipped to that viewport's own rect
the same way `RecordView` clips raster (`ResolveViewRect`, shared by both -
a zero-extent `RenderView::rect` means "the whole swapchain," anything else
gets clamped to it). `vkCmdTraceRaysKHR`'s dispatch width/height is the
rect's extent, not the swapchain's, so an editor's docked scene panel (or
one half of a split screen) only costs rays over its own pixels. Ray-gen
writes into the frame's `RayTracer` output image (`R8G8B8A8_UNORM`, `STORAGE_BIT`
- the swapchain's own images carry no storage usage), which is always
swapchain-sized regardless of the rect, so `RtPushConstants::outputOffset` (a
push constant, `rect.offset`) tells ray-gen where in it this dispatch's
`(0,0)` actually lands - `raytrace.slang` computes NDC from
`DispatchRaysIndex()`/`DispatchRaysDimensions()` exactly as before (both
already scoped to the rect by the dispatch call itself, so no shader-side
rect math beyond the output offset), but writes `gOutput[pixel +
outputOffset]` instead of `gOutput[pixel]`. `RecordRtFrame` first
`vkCmdClearColorImage`s the *whole* swapchain image to the clear color -
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
normal matrix `RasterPushConstants` uses for raster, and a lit/unlit flag) per traced
instance, same index. `VkAccelerationStructureInstanceKHR::instanceCustomIndex`
is what ties the two together - it's set to that same index now (was
hardcoded `0`, harmless while nothing read it, not once `InstanceID()`
became how the hit shader finds its `RtInstanceInfo`). The lit/unlit flag
comes from `GpuMaterial::mUnlit`, set once in `CreateMaterial` from
`desc.shaderName == "unlit"` - the only reason a `MaterialHandle` couldn't
answer "which shader was this built from" before now is that nothing
needed to ask; the shader name was read once, at `BuildPipeline` time, and
discarded. `RecordTlasBuild` resolves each item's material through
`MaterialStore::FindOrDefault`, the same call `RecordView` makes, so raster
and RT share one fallback rule. `closestHitMain` fetches the hit triangle's 3 indices, then either
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
`CameraBuffer` keeps one buffer per frame-in-flight, sliced into up to
`kMaxViewsPerFrame` views. `RecordView` has `CameraBuffer::WriteView` fill the
view's slice and binds set 1
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
separate buffer for it, so `SetDirectionalLight`/`SetAmbientColor` store
scene-wide state in `CameraBuffer`, which copies it into every view's slice,
duplicated across views like the rest of that struct.

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
`CameraBuffer`'s set layout and nothing else, no material set, no push
constants. `MaterialStore` builds it in `Initialize`, through the same
`CreateGraphicsPipeline` its material pipelines use, rather than through
`CreateMaterial`/`MaterialDesc`. The sky isn't a per-mesh material and needs no
vertex buffer, since its vertex shader synthesizes a fullscreen triangle from
`SV_VertexID`. Its depth-stencil state is `GREATER_OR_EQUAL`/no-write against
the same reverse-Z depth image opaque items wrote, so early-z skips the
fragment shader — a ~128-tap ray march — everywhere opaque geometry already
covered.

**A null material falls back to the default, and a broken one falls back to
the error material.** A null `MaterialHandle` means nothing was authored, so
the draw uses the default material. A non-null handle that does not resolve
means something failed, so `FindOrDefault` gives the draw the error material
instead. A failed `CreateMaterial` returns null, so its callers store
`ErrorMaterial()` in its place; `MeshResolveSystem` (app) and `ModelSystem`
(animation) both do. The error material runs `error.slang`, which draws solid magenta and
ignores the draw's tint and textures. It has no culling, because one error
material stands in for both a model primitive and its mirrored twin. Either
way, one bad handle does not blank the frame.

The same split applies one level down. `CreateMaterial` resolves
`MaterialDesc::baseColorTexture` and `normalTexture` through `CreateTexture`'s
table. A null or stale texture handle falls back to a flat white base color or
a flat normal map, rather than failing material creation. `ErrorTexture()` is
the texture for the broken case: a caller whose texture failed to load passes
it instead of null, so the failure shows as magenta rather than as white.

**`ReleaseSceneResources` is all or nothing, and the caller vouches for the
handles.** It waits for the device to go idle, destroys every mesh, every
material except the default and error materials, and every texture except the
three built-ins, and drops every queued, uploading and retired mesh. The next
`CreateMesh` reuses index 0. The next
`CreateMaterial` reuses index 2, since the default and error materials keep 0
and 1. The next `CreateTexture` reuses index 3, since the built-in textures
keep 0 to 2. Every mesh slot's generation moves on (#127), so a mesh handle
kept across the call misses instead of naming the next mesh in its slot;
texture generations are not bumped, so a texture handle kept across the call
names whatever is created next in its slot, with no error. `App` calls it only
right after unloading a scene, when no `MeshRenderer` is left.

**`DestroyMesh`/`DestroyMaterial` free one handle at a time, unlike
`ReleaseSceneResources`, and unlike it they *do* bump the slot's generation**
— immediately, at destroy, not when the slot is later reused — so every
outstanding copy of that handle fails the store's lookup from the moment
it's destroyed, not just once something else happens to occupy its slot. The
freed index goes on the owning store's free-list, which the next
`CreateMesh`/`CreateMaterial` checks before growing the vector, so a
create/destroy/create cycle reuses the slot rather than growing forever.
`Slots.h` holds that bookkeeping once for both stores. Both wait for the device to go idle first, same
reason as `ReleaseSceneResources` — an in-flight frame may still be drawing
from the buffers/pipeline about to be freed. **Same caller-vouches-for-
handles rule as `ReleaseSceneResources`**, just at one-handle granularity: a
`MeshRenderer`/material reference that outlives its handle's destroy isn't
prevented, it just starts missing in the store's lookup and silently
stops drawing (`RecordView`'s existing null-mesh skip) or draws magenta (the
error-material fallback), rather than crashing. `DestroyMaterial` additionally
always refuses on the default and error materials. Draws fall back to them for
the process lifetime, which is the same reason `ReleaseSceneResources` keeps
them. Textures have no public per-handle
destroy yet (see Backlog). Material lookups check the generation the same
way mesh and texture lookups do.

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

**`ImGuiOverlay`'s backend starts and stops with the app's ImGui context, not
with the renderer.** `ImGui_ImplVulkan_Init` needs the app's context, and
`ImGui_ImplVulkan_Shutdown` has to run before that context is destroyed. So
`imguiruntime` calls `InitImGuiVulkanBackend`/`ShutdownImGuiVulkanBackend`, and
the renderer's own `Initialize`/`Shutdown` never touch the overlay.

**Subsystems initialize in dependency order, and `Device` shuts down last.**
`MaterialStore` and the sky pipeline borrow set layouts from `CameraBuffer`,
`ModelDescriptors` and `ShadowMaps`, so those three initialize first, and the
borrowers shut down before them. `Device::Shutdown` logs VMA's live bytes before
it destroys the allocator, so every other subsystem has to be gone by then for
that count to report leaks.

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

`engine_renderer_tests` covers the parts that need no device: camera math,
render settings, view submission and the asynchronous mesh bookkeeping
(`MeshStreaming`: staging ring, frame budget, cancellation, retirement by frame).
Everything that needs a device is verified by the smoke tests and
`TerrainRenderProbe`.

## Backlog

1. **Per-handle destroy for textures.** Meshes and materials have it
   (`DestroyMesh`/`DestroyMaterial` — see Changed); textures still only go
   away in bulk, via `ReleaseSceneResources` and `ReleaseModelResources`.
   `ResourceStore::DestroyTexture` exists, but the public API does not expose
   it. Lower
   priority than meshes/materials were: nothing today creates or discards
   textures at a rate that makes the gap felt.
2. **Draw-item sorting and batching.** There is one pipeline bind per item today,
   so a frame scales with the number of distinct materials rather than with draw
   count.
3. **Lighting beyond flat ambient, one Sun, and `kMaxLights` (64) Point/Spot/
   Area.** Sun/Point/Spot now cast real shadows in both pipelines (see
   Changed) — Area still doesn't, and still doesn't need to: it's Point in a
   trenchcoat, same range-based falloff, no real rectangular-light
   integration; its `mAreaSize` is authored and inspectable but nothing reads
   it yet. `SetAmbientColor` is still the only ambient input; no per-entity
   control there either. Raster's shadow caps are deliberately small and
   separately capped per light type — `kMaxSpotShadows` (4) and
   `kMaxPointShadows` (2), beyond `kMaxLights` itself — since each caster is
   a real render target and draw pass, not just a buffer slot; a light
   beyond its cap simply casts no shadow (same "nearest to the primary
   camera wins" selection as everything else that's capped here). No PCF
   yet either — single-tap sampling, soft-edge filtering is a documented
   follow-up, not built.
4. **Per-config shader output paths.** Release currently ships unoptimised
   shaders, and the debug flags cannot become conditional while both
   configurations write to a path that carries no configuration component.
5. **A real renderer interface.** It is the prerequisite for a headless mode, and
   therefore for testing anything in this module or in `app`.
6. **A second viewport actually on screen at once.** The mechanism (`AddViewport`)
   exists and is tested by `App` using only viewport 0; nothing yet adds a second
   one for, say, an editor-camera view alongside the game view during a paused
   Play session.
7. **No cubemap/gradient sky option, and no time-of-day driver.** The Sky
   entity (#97) is Nishita-only — `SkyMode` has one value; there's no
   `MaterialDesc`-style switch to a static cubemap asset or a cheap
   two-color gradient for scenes that don't want the ray-march cost. Sun
   elevation is whatever a Sun-type Light's or the Sky entity's rotation is
   holding (#98 supersedes #97's own rotation the moment a Sun Light
   exists); nothing animates either over time yet (a script/system rotating
   one on a timer would get a day/night cycle for free — see Rules). There's
   also no moon/stars, so full night is flat black rather than lit by
   anything but `SetAmbientColor`.
8. **RT still isn't full parity with raster.** Shadows are now one gap that's
   *closed* rather than open (RT's occlusion rays and raster's shadow maps
   agree on what's lit, by construction - see Changed); what's left:
   `RayTracer::kMaxInstances` (256) is a fixed cap with no overflow handling beyond
   silently dropping the rest; the TLAS rebuilds fully every `TraceFrame`
   call with no dirty-tracking (see the Rules section above - correct, just
   not efficient; shadow occlusion rays add up to `1 + lightingMeta.x` extra
   `TraceRay` calls per shaded pixel on top of that, bounded by the view's
   own active light count); only the two shaders that exist
   (`default`/`unlit`) get RT parity, so a third custom material shader
   would trace with whichever of those two `GpuMaterial::mUnlit` happens to
   pick, not its own look; `closestHitMain` samples no textures, so its colour
   is the vertex colour or albedo times the draw's tint, a textured model
   traces untextured, and the error texture and error material trace as the
   draw's tint rather than magenta; and `TraceFrame` only ever traces `views[0]` - a
   second viewport on screen at once is backlog item 6 regardless of
   raster/RT, and RT inherits that gap rather than fixing it.

## Changed

*2026-10-11* — **Asynchronous meshes (#127).** Was: the only way to make or
free a mesh was `CreateMesh` (a staging buffer, a submit and a queue wait, then
the same again for the BLAS on any RT device) and `DestroyMesh` or
`ReleaseModelResources` (a device wait); `ReleaseSceneResources` restarted mesh
generations. Now: `ConfigureMeshStreaming`, `QueueMesh`, `MeshState`,
`RetireMesh`, `MeshFootprint` and `MeshStreaming` upload through a persistent
staging ring at the start of each frame within a per-frame byte budget, build
a BLAS only when asked, report readiness from the frame timeline and free
retired meshes after their last frame, all without a wait (`MeshUploader`,
`renderer/MeshStreaming.h`); `FindMesh` returns Ready meshes only; every
device and queue wait is counted; `ReleaseSceneResources` moves every mesh
slot's generation on; BLAS and TLAS scratch buffers are aligned to
`minAccelerationStructureScratchOffsetAlignment` (with many small buffers
sharing memory blocks, a plain allocation failed validation). `CreateMesh` and
`DestroyMesh` behave as before. Because:
streamed terrain replaced a level 0 Region by 256 tile meshes in one frame,
each with two queue waits.

*2026-10-10* — **Draws chosen per viewport (#125).** Was: terrain tile draws
sat in a persistent `TerrainDrawItems` resource that every view and shadow pass
drew whole. Now: `RenderSystem` resolves every viewport's camera before
anything else, hands the cameras to each `IViewDrawSource` in the
`ViewDrawSources` resource, and asks it for each view's draws and for the
shadow volumes' casters at viewport 0's detail. The shadow camera is viewport
0's resolved camera, as before. `TerrainDrawItems` is gone.

*2026-10-08* — **Error texture and error material.** Was: the renderer had one
fallback per level. A null or stale texture became white or a flat normal, and
a null or stale material became the default grey. A failed load therefore
looked the same as an untextured or unmaterialed draw. Now: `ResourceStore`
creates a third built-in texture, a 1x1 magenta exposed as `ErrorTexture()`.
`MaterialStore` creates a second built-in material on `error.slang`, exposed as
`ErrorMaterial()`. `FindOrDefault` sends a stale non-null handle to the error
material, and a null handle still goes to the default. Release keeps the
built-ins in the first slots, and `DestroyMaterial` refuses both built-in
materials. Because: issue #19, which asks a missing texture to fall back
rather than fail the model, and a failure is easier to spot in magenta.

*2026-10-05* — **`RequestSwapchainRecreate`.** Was: only a size change or an
out-of-date acquire or present recreated the swapchain. Now: a caller can force
it for the next frame, through the same `RecreateSwapchain` path. `App` does on a
fullscreen switch, which need not change the window's size. Because: issue #44.

*2026-10-05* — **`VulkanRenderer` became a facade over subsystems.** Was: one
class held every Vulkan object, about 4700 lines in one `.cpp` before the
file split, and `DrawFrame` and `TraceFrame` each carried their own copy of
the wait/acquire/submit/present loop. Now: the state lives in the eleven
`mir::vk` subsystems listed in Model, and both frame paths share
`FrameSync::Begin`/`End`. The public API did not change, apart from the setters
and `CaptureNextFrame` moving out of line with the same signatures. Behaviour
changed in six places. The set-2 frame reset runs on ray traced frames too. A
failed queue submit or timeline wait stops rendering for good, where it used to
hang two frames later. A failed swapchain or RT-output recreate retries on the
next frame. A capture finishes after present, and a frame that was never
submitted writes no PNG. `ReleaseModelResources` frees each released mesh's
BLAS, which it used to leak. Each BLAS is built in its own submit after its
mesh's upload. Because: with one class owning everything, a change to one
concern touched unrelated state, and the frame loop had to be fixed twice.

*2026-10-02* — **Terrain draws (#14).** Was: `RenderSystem` drew
`MeshRenderer` entities and `ModelDrawItems` only, and the TLAS held at most
256 instances. Now: a persistent `TerrainDrawItems` resource is merged with the
other draws and traced like them; `kMaxInstances` is 16384; and
`WorldDrawItem::translationGrid` lets `BuildViewDrawItems` round a draw's
translation to a fixed grid so neighbouring terrain tiles leave no pixel
cracks. Other draws narrow as before.

*2026-10-01* — **Sun/Point/Spot cast real shadows, in both pipelines.** Was:
falloff/cone attenuation but no occlusion test - a light shone straight
through geometry that should block it (backlog item 3). Now: `GpuLight.color.w`
(previously-dead padding) holds a CPU-assigned `shadowSlot` (`-1` = unshadowed)
that both pipelines gate on, so they agree on *what's* shadowed even though
*how* differs completely. `RenderSystem` ranks Point/Spot by distance to the
primary camera into `kMaxPointShadows`(2)/`kMaxSpotShadows`(4) slots; Area
never competes for one. RT fires one occlusion `TraceRay` per shadowed light
per pixel (reusing the existing hit group, `RAY_FLAG_SKIP_CLOSEST_HIT_SHADER`,
recursion depth 1→2). Raster renders real shadow maps each frame before the
main pass: Sun gets one non-cascaded orthographic map fitted to the primary
rendered viewport's frustum, including external editor cameras, up to 512 m
(`kSunShadowDistance`), with extra sun-axis depth for off-frustum casters; Spot a
`Texture2DArray`; Point a distance cube array (each face stores
`length(worldPos - lightPos)` in a color attachment, not hardware depth, so
shading samples by direction with no per-face matrices). Cost: up to 17 extra
raster depth passes/frame worst case (backlog item 3); RT pays up to
`1 + lightingMeta.x` extra `TraceRay`s/pixel (backlog item 8).

Default and model materials share `RasterLighting.slang` for scene lighting,
Sun/Spot/Point shadow lookups and fog. Model draws pass their deformed,
camera-relative world position to that lighting code. RT diagnostic images
are prepared after any swapchain recreation and before frame acquisition.
`TerrainRenderProbe --regressions --out <directory>` checks actual raster
receiver shadows for both materials, near and distant geometry, an external
camera at large world coordinates, and diagnostic captures across two resizes.

*2026-09-30* — **Depth is reverse-Z, and a lens can have no far plane.** Was:
`MakeViewProjection(cameraWorld, fov, near, far, aspect)` mapped near to depth 0
and far to 1, the depth buffer cleared to 1 and tested `LESS` (sky `LEQUAL` at
depth 1), transparent draws sorted by `z / w`, and ray tracing stopped at a
hard-coded `TMax` of 10 km whatever the camera said. With a far plane of 120 km
that mapping could not separate surfaces a meter apart beyond about 1 km. Now:
`MakeViewProjection(cameraWorld, lens, aspect)` maps near to 1 and far to 0 on
the same D32F buffer, which clears to 0 and tests `GREATER` (sky `GEQUAL` at
depth 0). `Camera::mFarZ == 0` (`HasFarPlane()` false) means no far plane:
depth is `near / distance`. The transparent sort uses clip w, the view depth.
`RenderView` carries `nearZ`/`farZ` into a new `CameraData::clipPlanes`, and
`raytrace.slang` ends its rays at the far plane, or not at all without one.
The sky and ray-generation shaders unproject at depth 1, now the near plane.

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

*2026-09-27* — **RT gets the Point/Spot/Area light list too, and the
Inspector edits a `Light`'s color as a color instead of three raw floats
— #98 follow-up, from the user's own testing.** The RT gap wasn't a bug -
`closestHitMain` genuinely never read `gLights`, matching the light list's
own binding being fragment-stage-only. Now: `CreateCameraResources`
conditionally adds `VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR` to that binding's
`stageFlags` when RT is supported (same descriptor set and buffer raster
already uses, so nothing new to allocate), and `closestHitMain` runs the
identical falloff/cone loop `default.slang`'s fragment shader does, off
`hitWorldPos`/`N` instead of `input.worldPos`/`input.normal`. Separately:
`range`/`intensity`/`areaSize`/`spotAngleDegrees` going negative, or `range`
to exactly 0 (an easy slip with a bare `DragFloat`, no min/max), silently
zeroes or degenerates a light's contribution with no visual cue why - the
Inspector now clamps each to its own sensible range (`range` > 0, since 0
means "affects nothing, ever"; `intensity`/`areaSize` >= 0, since 0 is a
legitimate off/degenerate value for those two; `spotAngleDegrees` to
`[1, 179]`, since 0 or 180+ isn't a cone) with `ImGuiSliderFlags_AlwaysClamp`
- without that flag, `DragFloat`'s min/max only bound the drag gesture,
never a typed-in value (double-click to type bypassed them entirely).
`color` draws as `ImGui::ColorEdit3` instead of a raw `DragFloat3`, and its
combo/color-picker special-case widgets now honor `FieldDesc::ReadOnly()`
the same way the generic per-field loop already did (they didn't, at
first). Reduced `Light`'s default `mRange` (10 → 2.5) and `mAreaSize`
(1 → 0.5) to match HelloWorld-scale scenes - the gizmo math was already
1:1 world units (`DrawLightGizmos` passes `light.mRange` straight through
as a screen-projected radius), the old defaults were just enormous relative
to a ~1-2-unit scene; `UpdateViewportPicking`'s light pick radius now scales
with `mRange`/`mAreaSize` too, so clicking anywhere on a light's visible
gizmo (not just within a fixed 0.3-unit dot at its center) selects it.
Two correctness fixes surfaced along the way: `SetLights` was writing
straight into `mLightMapped[mFrameIndex]` from `RenderSystem::OnUpdate`,
before `DrawFrame`/`TraceFrame`'s own `vkWaitSemaphores` had confirmed the
GPU was done reading that frame-in-flight slot's *previous* contents - a
race `WriteCameraData` never had, since it only ever runs from inside
`RecordCommands`/`RecordRtFrame`, after that wait. `SetLights` now only
stages into `mPendingLights`; a new `UploadLights()`, called from
`RecordCommands`/`RecordRtFrame` right after their waits, does the actual
write. Separately, a Sky entity's own path called `SetSunDirection`
(direction only) rather than `SetDirectionalLight` (direction + color) -
harmless while a Sun Light had never existed, but once one existed and was
then removed or retyped, `mLightColor` would keep that stale Sun's color
forever, since nothing on the Sky path ever touched it again. The Sky path
now always calls `SetDirectionalLight`, with a `kDefaultLightColor` fallback
(`VulkanRenderer`'s own original default) when no Sun Light claims the
direction; `SetSunDirection` is gone, nothing else called it.
`kSunReferenceLocal` was hand-duplicated in both `RenderSystem.h` and
`Editor.cpp`'s gizmo code - moved to `Light.h` as `kSunReferenceLocalDirection`,
one definition both now share, so the Sun gizmo can't silently point somewhere
the light doesn't. A second Sun-type Light could be created with no guard
(unlike `Sky`'s `CanCreateSky`) even though `RenderSystem` only ever honors
the first one `Query::ForEach` happens to visit - order that isn't guaranteed,
so which Sun "won" could change frame to frame; a `CanCreateSunLight` guard,
same shape as `CanCreateSky`, closes that. The four `Light/*` entity-type
factories were near-identical copy-pasted lambdas differing only in
`LightType` and a name prefix - collapsed into one `CreateLightEntity<Type>`
template, so a wrong-type copy-paste slip can't happen. `HierarchyPanel`'s
Create-menu grouping required same-prefix entity types (`"Light/Sun"`,
`"Light/Point"`, ...) to be registered contiguously to nest into one submenu
correctly - true today, but unenforced and silently wrong the moment it
isn't; rewritten to group by name via a small index instead of scanning for
runs of matching neighbors, so registration order no longer matters. The
`GpuLight` struct and falloff/cone-attenuation loop were copy-pasted verbatim
between `default.slang` and `raytrace.slang` (the review that found this also
noticed the loop's `dist2` was actually `length()`, not a squared distance -
a misleading name given a "2" suffix usually means squared); both now
`#include` a new `LightingCommon.slang` (same sharing pattern
`AtmosphereCommon.slang` already established for sky rendering) holding the
struct, the `gLights` binding, and one `AccumulatePunctualLights` function -
future falloff/shadow changes only need to happen once. `DrawLightGizmos`'s
Area case only ever drew the flat `mAreaSize` square, with nothing showing
`mRange` - the field that actually drives its falloff today (Area shades
identically to Point); it now also draws the same range circle Point does,
dimmer, so the square doesn't imply `mAreaSize` controls reach it doesn't yet
control.

*2026-09-27* — **Entities can add themselves now: Light and Sky entity
types, a data-driven Sky, and Sun/Point/Spot/Area lights with viewport
gizmos — issues #96, #97, #98.** Was: no way to create a non-mesh entity at
all from the editor; the sky was `VulkanRenderer`'s own always-on hardcoded
Nishita constants and direction, invisible as scene data; lighting was one
hardcoded directional light + ambient, `SetDirectionalLight`/
`SetAmbientColor` never actually called by anything. Now: `EntityPresetRegistry`
(new, `core/ecs/EntityPresetRegistry.h`, sibling to `ComponentRegistry`) holds
a name → factory list a module registers into (`RegisterRendererEntityTypes`,
alongside `RegisterRendererComponents`); `HierarchyPanel`'s right-click
"Create" menu lists it and selects whatever it makes (#96). `Sky` gained
real fields (`SkyMode`, planet/atmosphere radius, Rayleigh/Mie coefficients
and scale heights, `mieG`, sun intensity, all in slider-friendly display
units) that `VulkanRenderer::SetAtmosphere` converts to the physical units
`AtmosphereCommon.slang`'s `Atmosphere()` now takes as a parameter struct
instead of `static const` globals; the first Sky entity found (Camera's
fallback idiom) drives `lightDirWS` from its `Transform` rotation and gates
the raster sky-dome draw entirely — no Sky entity, no dome, clear color
shows through (#97). A `CanCreateSky` guard on its entity-type registration
logs an error and refuses a second one; HelloWorld's own sky, previously
invisible renderer state, is now `Sky_Entity6` in its scene file, visible
and editable like anything else. `Light` gained `LightType`
(Sun/Point/Spot/Area), color, intensity, range, spot angle, and area size;
`RenderSystem` resolves the first Sun-type Light (if any) to
`SetDirectionalLight` directly, taking priority over a Sky entity's own
rotation, and packs every Point/Spot/Area into a new per-frame list
(`VulkanRenderer::SetLights`, `LightItem`, `kMaxLights` = 64, same
silently-drop-the-rest cap style as `kMaxRtInstances`) that lives in the
camera descriptor set's new binding 1 (a `StructuredBuffer`) - one more set
member, not a new set, so no other pipeline layout changed. `default.slang`
reads it (falloff to 0 at range; Spot narrows that with a single `>=`
cone-cosine test that Point/Area's `-1` sentinel always passes, so one loop
serves all three with no per-type branch); `unlit.slang` intentionally
does not - it stays unlit, same as its RT closest-hit counterpart already
does for `GpuMaterial::mUnlit`. Each Light entity draws its own wireframe
gizmo (sun rays, a 3-great-circle sphere at `range`, a cone, or a flat
square) every frame regardless of selection, brighter when selected, using
the same world→clip→screen technique `DrawSelectionHighlights` already used
for `MeshBounds`; `UpdateViewportPicking` gained a second pass testing
closest-approach-within-a-fixed-radius against the ray, since a Light has
neither `MeshRenderer` nor `MeshBounds` for the existing AABB test to find.

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
extent; a new push constant (`RtPushConstants::outputOffset` in
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
