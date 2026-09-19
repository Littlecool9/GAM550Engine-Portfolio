# glTF asset pipeline

## Import and asset ownership

`AssetCooker` routes `.gltf` and `.glb` under its source roots through fastgltf (vcpkg supplies 0.9.0). The runtime does not parse glTF or load its original buffers/images. Other supported source files, including legacy `.mesh.json`, retain the raw-asset path.

The source path resolves to a **Model** asset. It references independently cooked **Mesh**, **Skeleton**, **AnimationClip**, **Material**, and **Texture** assets. Model stores scenes, node hierarchy, mesh instances, default morph weights, and skin bindings. A skin binding owns its joint-key palette and inverse bind matrices; a Skeleton preserves auxiliary nodes as well as joints. Clips target stable node keys. This separates reusable content from instance playback without introducing retargeting or an animation state machine.

Mesh payloads contain explicit vertex/index arrays, primitive topology, material and base-color texture IDs, variable-length joint influences, and position/normal/tangent morph deltas. Runtime converts the vertex fields to `renderer/Mesh.h`'s `mir::Vertex`; serialization never copies compiler-dependent struct padding.

`ModelAsset.h` defines payload versions by type. **Mesh contentVersion is 2**, replacing the old RAW!/1 mesh representation; other new types start at 1. The existing outer `AssetBlobHeader::formatVersion` stays at 1 because framing did not change. There is no second version field inside a typed payload. Readers check both type and content version, reject unsupported versions, and require recooking after incompatible schema changes. Legacy raw mesh JSON is still loaded through its existing reader.

## Supported first-version content

- Indexed/non-indexed geometry; all seven glTF primitive modes converted to point, line or triangle lists. Triangle strips preserve winding. Missing triangle normals generate flat normals and duplicate all associated skin/morph attributes consistently.
- Strided and sparse accessors, integer normalization, UV selection by the base-color texture's `texCoord`, normals, tangents and vertex colors. Engine coordinates remain glTF's right-handed, Y-up coordinates; matrices and hierarchy are preserved.
- External buffers/images confined to cooked roots, data URIs, and GLB buffer views. PNG/JPEG decode during cook to RGBA8. Base-color factor, vertex color, sRGB textures, sampler wrap/filter/mips, alpha opaque/mask/blend, double-sided materials and mirrored instances.
- Joint palettes, inverse bind matrices, auxiliary hierarchy nodes, and all paired JOINTS_n/WEIGHTS_n sets. Nonzero influences are retained and normalized together, without truncating to four.
- Translation, rotation, scale and morph-weight tracks with STEP, LINEAR and CUBICSPLINE interpolation. Quaternion linear interpolation follows the shortest path; cubic tangents are scaled by the interval duration and results normalized.
- Morph position/normal/tangent deltas, mesh/node default weights, animated weights and optional per-instance overrides. GPU vertex processing applies **morph, then skin, then instance/world transform**. A CPU reference evaluator is available to tests and tools.

Unsupported required extensions fail cook explicitly. Optional extensions use their core glTF fallback. Draco, meshopt, KTX2/BasisU, extension-specific materials/texture transforms, full metallic/roughness PBR, normal maps, cameras/lights from glTF, retargeting, blending/state machines, root motion and editor timeline tools are outside this implementation. Transparent draws sort by instance/node origin, so intersecting transparent surfaces are not order-independent.

## Identity and publication

Keep `<source>.import.json` in version control alongside its glTF. It stores allocated child keys and never recycles them. Unique nonempty names match across array reorder and content edits. Anonymous/duplicate-name objects use content or structural fingerprints; ambiguous matches fail with an instruction to assign unique names. Renaming named objects, editing anonymous objects, deleting metadata, or moving the source can change IDs. Assign unique names before importing assets that need stable references.

Every cooker invocation reimports glTF and reads its dependencies. CMake tracks files under cooked roots to trigger the cooker after edits. Dependency fingerprint caching is deferred.

Blobs use asset ID plus content hash in their filenames. The cooker writes blobs to temporary files and atomically replaces the manifest only after all imports succeed. A failed import leaves the previous manifest and its blobs usable. Unreferenced blobs are pruned after successful publication. Metadata is append-only and may advance if publication itself fails; that does not invalidate the old manifest. Concurrent cooker processes targeting the same output directory are unsupported. The output directory must be outside source roots.

## Using a model

Add a `ModelPlayer` component to an entity with a Transform. `modelName` is relative to the game's asset root, for example `models/flag.glb`. `clip` selects a clip (`-1` means rest pose); `scene = -1` selects the default scene. `time`, `speed`, `playing`, `loop` and `preview` control each instance. Set `playing = false` and `time` for a fixed pose. In Edit mode playback advances only when `preview` is true; Play pause freezes the clock. Model nodes remain an internal hierarchy beneath the entity, rather than separate ECS entities.

`ModelMorphOverrides::instances[PackEntity(entity)][nodeKey]` optionally replaces sampled weights for that instance. It is transient and cleared on scene unload/Play restore. Shared asset and GPU mesh data are immutable; time, pose and weights belong to the instance. Stop restores serialized playback state and rebuilds scene GPU resources.

The first renderer implementation uploads deformation buffers per draw/frame (including immutable morph data), uses at most 4096 model draw descriptor sets per frame, and checks the device storage-buffer limit. It is intended as a correct initial implementation; persistent GPU deformation data and allocation pooling are later optimizations.

## Build and verify

From Developer PowerShell with the normal repository prerequisites:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --test-dir builds/windows-msvc-debug --output-on-failure
```

`games/GltfShowcase` is an original, reproducible fixture containing the same checker-textured flag as external `.gltf` + `.bin` + PNG and embedded `.glb`. It has two joints, a morph target and two clips. `tools/AssetCooker/tests/generate_fixtures.py` regenerates the source fixtures; their `.import.json` files should be retained. Two instances run at different phases/speeds to expose accidentally shared playback state.

From `builds/windows-msvc-debug`, view and capture the GPU result:

```powershell
.\MjolnirEngine.exe --game games/GltfShowcase --frames 90 --no-editor --capture gltf-showcase.png
```

`--capture` requires `--frames`; the final frame is saved when the surface supports readback. Automated coverage includes glTF/GLB typed cook/load, texture references, topology conversion, normalized/interleaved and sparse accessors, dependency changes, named-node reorder, failed-import publication, deleted clips, malformed payloads, interpolation, hierarchy, combined morph/skin and independent instances. `smoke.gltf_lifecycle` runs two Play/Stop cycles, checks playback restoration and GPU draw reconstruction. Smoke tests require Vulkan/GPU and fail on logged errors. This is targeted regression coverage, not glTF conformance certification.
