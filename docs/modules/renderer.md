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
TextureHandle CreateTexture(const TextureDesc &desc);
MaterialHandle CreateMaterial(const MaterialDesc &desc);
void ReleaseSceneResources();                // every mesh, material and texture but the defaults

void DrawFrame(std::span<const RenderView> views);
void SetClearColor(const glm::vec4 &color);
void SetDirectionalLight(const glm::vec3 &direction, const glm::vec3 &color); // direction points *toward* the light
void SetAmbientColor(const glm::vec3 &color);
void SetFog(const glm::vec3 &color, float density);          // density <= 0 disables fog
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
| `DrawItem` | `renderer/Renderer.h` | `{mesh, model, normalMatrix, tint, material}` — `model` is the world transform, not premultiplied by any camera |
| `RenderView` | `renderer/VulkanRenderer.h` | `{rect, items, viewProj, cameraPos}` — one camera's `DrawItem`s, clipped to a screen rect; `cameraPos` only the sky pass reads |
| `Viewport`, `ViewportId` | `renderer/RenderSystem.h` | `RenderSystem`'s per-viewport state: rect plus a World `Entity`, an external camera, or neither (fallback) |
| `MeshRenderer`, `Camera` | `renderer/components/` | Handles, and `fovY`/`near`/`far`, declared here rather than in `core` |
| `MeshBounds` | `renderer/components/MeshBounds.h` | `{mLocalMin, mLocalMax}`, a mesh's own local-space AABB; attached by `ResolveSceneMeshes` (see [app.md](app.md)), read by the editor's viewport click-picking (see [editor.md](editor.md)) — not used for rendering at all |
| `Ray`, `MakeCameraRay`, `RayIntersectsAABB` | `renderer/CameraMath.h` | World-space camera ray from a click, and a ray/AABB slab test — the same file `MakeViewProjection` lives in, so both stay in the same Vulkan-convention math |
| `RenderSystem` | `renderer/RenderSystem.h` | The ECS-to-renderer bridge, `SystemPhase::Render` |
| `Shapes.h` | `renderer/Shapes.h` | `MakeCube()` and friends — plain vectors, no Vulkan |
| `RenderSettings` | `renderer/RenderSettings.h` | Project Settings' `"rendering"` section (`rayTracing: bool`, `fog: bool`); `LoadFile`/`SaveFile`, a World resource loaded in `App::Initialize` |

## Rules

**Vulkan 1.4 with `dynamicRendering`, `synchronization2`, `timelineSemaphore` and
`shaderDrawParameters`.** A device missing any of them is rejected during
selection, with a log line naming what was missing.

The floor is roughly 2020 hardware with a current driver. That is acceptable for
a project that is not shipping to consumers.

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

1. **Per-handle destroy for meshes and materials.** `ReleaseSceneResources` frees
   everything at once. Nothing can free a single mesh, and a stale handle is not
   detected.
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

## Changed

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
