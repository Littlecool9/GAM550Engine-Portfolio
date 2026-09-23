# animation

Model resource validation, clip sampling, pose evaluation, and per-instance
playback data. The module does not own the renderer, ECS update loop, or import
of source glTF files.

`mir::animation` · depends `mir::assets`, `mir::core` (public) · API `modules/animation/include/animation/` · review pending

## Model

`AssetCooker` turns a glTF package into a Model plus typed Mesh, Skeleton,
AnimationClip, Material, and Texture assets. `LoadModelResources` resolves the
Model's mesh, skeleton, and clip dependencies through `AssetCache`, then checks
their cross-references before a model can be evaluated. The source format is
never parsed here.

`ModelPlayer` is the ECS component for one instance. It stores the model and
animation references, clip and scene selection, and the playback clock. The
cooked assets are shared and immutable; `EvaluateModel` builds a fresh node
pose from rest values for each sample, so seeking or evaluating two instances
does not mutate shared data. The pose follows stable node keys and the Model's
parent hierarchy. A `ModelMorphOverrides` world resource can replace sampled
weights for one entity and node without changing the asset.

`ModelSystem` lives in `app`, where it can access the asset cache and Vulkan
renderer. It resolves each `ModelPlayer`, advances its clock only while Play is
simulating, evaluates a pose, and submits per-draw deformation data. The
renderer shader applies morph, then skin, then the instance transform;
`DeformPrimitive` provides the CPU reference for that order.

## API

| Type or function | Header | Role |
|---|---|---|
| `ModelPlayer`, `RegisterModelPlayer` | `animation/ModelPlayer.h` | Serializable per-entity selection and playback state |
| `ModelMorphOverrides` | `animation/ModelPlayer.h` | Transient weights keyed by entity and stable node key |
| `ModelResources`, `LoadModelResources` | `animation/ModelAnimation.h` | Typed model and validated dependencies |
| `SampleAnimationTrack`, `EvaluateModel`, `NodePose` | `animation/ModelAnimation.h` | Track interpolation and a fresh hierarchical pose |
| `ResolvePoseWorld`, `SkinMatrices`, `VisibleModelNodes` | `animation/ModelAnimation.h` | World transforms, skin palette, and selected glTF scene |
| `DeformPrimitive`, `BuildDeformationData` | `animation/ModelAnimation.h` | CPU reference and shader buffer encoding |

`ModelSystem` and its GPU resource lifetime are in `modules/app/src/app/ModelSystem.h`
and `ModelSystem.cpp`, rather than this library. See
[glTF asset pipeline](../gltf-importer.md) for importer identity and editor use.

## Rules

**Keep instance state separate from cooked assets.** `ModelPlayer` contains
the time, speed, playing and loop flags. Sampling starts from rest pose on
every call. This makes seeking deterministic and prevents one entity's morph
weights or animation time from affecting another entity.

**Use stable asset IDs after resolution.** `modelName` is a path relative to the
game's asset root and is useful in hand-authored scenes. `modelAsset` and
`animationAsset` store resolved hex IDs; when set, they take precedence over
the path and clip index. The runtime fills missing IDs after a successful
load, so subsequent scene saves retain references across source moves and
clip reordering. A missing referenced clip stops playback with a warning.
The Inspector provides a Model picker and hides these raw reference fields;
`clip` and `scene` remain editable.

**The playback clock is a Play-mode concern.** `ModelSystem` advances time only
when the world is simulating and `playing` is true. Edit mode and Play pause
hold the clock. A fixed pose can be authored with `playing = false` and an
explicit `time`. There is no serialized preview flag. Older scene files with
`preview` load without using that key, and the next save omits it.

**Scene serialization follows the component field table.**
`RegisterModelPlayer` supplies the fields consumed by both scene save/load and
generic scripting. Removing a field from that table changes what scene files
persist; hiding it in the Inspector alone does not.

**Validate dependencies before drawing.** A clip track must target a model
node with a compatible channel, a skin palette must match its skeleton, and
morph weights must match the mesh. `LoadModelResources` rejects mismatches;
it does not silently render a partial model.

## State

*As of 2026-09-23.* The module samples translation, rotation, scale, and
morph-weight tracks with STEP, LINEAR, and CUBICSPLINE interpolation. It
evaluates hierarchy, selected glTF scene visibility, skin matrices, and
optional per-instance morph overrides. `ModelSystem` shares loaded GPU
resources between instances and rebuilds them around import reloads and scene
changes. Tests in `modules/animation/tests/ModelAnimation.tests.cpp` cover
interpolation, deterministic seeking, hierarchy, morph-before-skin behavior,
and invalid typed assets. GPU draw behavior is outside this unit test target.

There is no animator graph, blending, retargeting, root motion, or animation
preview window. The Inspector edits the existing playback fields and selects
a model, but it has no timeline or named clip picker.

## Backlog

1. Add an animator layer for state transitions and blending when gameplay
   needs more than one selected clip per instance.
2. Design a dedicated animation preview window with a transient clock and
   selection state. Keep preview state out of scene JSON.
3. Add a clip picker and timeline authoring controls to replace manual clip
   indices and time edits.
4. Profile per-draw deformation buffer uploads before pooling or caching them.

## Changed

*2026-09-23* — **Preview removed from scene state.** `ModelPlayer.preview`
previously let Edit mode advance the playback clock and was saved by the
generic field table. Playback now advances only while simulating; a future
preview window will own its own transient state.
