---
module: gpu
id: layout-meta
title: Layout contracts
standard: v2
---

## A boundary with no type checker

Between a GPU buffer write and a GLSL block there is no type. The driver copies
bytes; the shader casts them to whatever its own declaration says they are. Nothing in
the Vulkan API, the SPIR-V validator or the C++ compiler compares the two, and a
mismatch yields neither a validation error nor a crash — it yields a plausible wrong
image. `layout_meta.hpp` converts the one part of that risk that is mechanically
checkable, the byte size of the C++ side, into a compile error. Five canonical
constants live there. Four are bare literals — `kGPULightBytes` is `80` with
`// 5 * vec4` beside it, `kPTPushConstantsBytes` a bare `256` — so the number and its
justification can drift apart unwatched. The fifth is the arithmetic the material
pack implies:

{{cite ohao/gpu/layout_meta.hpp "kMaterialVec4s * sizeof(glm::vec4);"}}

Nor is the header purely declarative: `MaterialGpuPack` adds two constexpr accessors
for the seven-`vec4` material stride and pins them with static asserts against
hand-computed answers, so an edit to `kMaterialVec4s` fails here rather than in a
shader.

## What the two assertions actually prove

`OHAO_ASSERT_GPU_LAYOUT(Type, Bytes)` expands to a pair of static asserts. The
second is the obvious one:

{{cite ohao/core/concepts.hpp "    static_assert(sizeof(Type) == (Bytes),"}}

The first requires `GpuPod`, which is `is_trivially_copyable_v` **and**
`is_standard_layout_v`. Trivial copyability makes the `memcpy` into mapped device
memory defined behaviour instead of a habit. Standard layout is the half people
skip, and the one doing structural work: it forbids reordering members declared
across different access-control sections, guarantees the first member sits at
offset 0, and is the precondition for `offsetof` being defined at all. Without it
a struct could satisfy the size check while the field order the shader assumes
does not formally exist.

Neither assertion says anything about the other side of the boundary — both are
statements about C++ types, evaluated by the C++ compiler, and no build step
reflects the SPIR-V. The macro catches *someone added a float to the struct* and
is blind to *someone reordered the GLSL block*. Three structs carry it:
`LightData` at 128 bytes, `GPULight` at 80 and `PTPushConstants` at 256. The first
is the light table's host array, which reaches the GPU only as the 64-byte `LightGpu`
records `LightClusters` copies out of it; the second rides in the path tracer's light
SSBO; only the third crosses a push-constant boundary.

The structs that make up the render world's tables are guarded by plain
`static_assert(sizeof(...) == N)` instead, which proves the size and nothing about
`GpuPod`:

{{cite ohao/render/scene/render_scene_types.hpp "static_assert(sizeof(InstanceGpu) == 160);"}}

`InstanceGpu` (160), `MeshGpu` (16), `Vertex` (100, which the RT shaders read as 25
floats) and the G-buffer's `CameraUBO` (128) are pinned this way. The practical
difference is small — each is a plain aggregate — but it means the macro's
trivially-copyable half is applied to three of the structs the GPU reads and not to
the rest.

## The one block above the guaranteed floor

Vulkan guarantees only 128 bytes of push-constant space; anything above that is a
device capability, not a promise. Exactly one contract sits above the floor — the
path tracer's 256 — and no code queries `maxPushConstantsSize`; across `ohao/`,
`shaders/` and `examples/` the identifier occurs only in comments, each noting that
128 is the floor. On a minimum-spec device the failure surfaces at pipeline-layout
creation, not at compile time. The raster blocks are far below the line: the G-buffer pushes 16 bytes and the
shadow pass 64.

The path-tracer block is also the one with no headroom: three matrices and four
`uvec4`/`vec4` lanes fill the 256 bytes exactly.

{{cite ohao/render/rt/path_tracer.hpp "};  // total = 256 bytes"}}

The bounce limit, the sample index, the running mean's length and the motion-vector
flag all share lanes with other values. A size contract that is exactly satisfied has
stopped being a check and become a design constraint: a new per-frame value has to
find a spare word, not a new field.

## The per-draw block that became a record

The raster draws carry no per-draw push constant. Indirect commands cannot change one
between draws, so everything a draw needs is in `RenderScene`'s draw table, one
`DrawRecord` per draw, which the vertex shaders index with `firstInstance`. The record
is declared once, in `shaders/shared/layout.json`, and generated into GLSL, Slang and
C++; the C++ side pins its size:

{{cite ohao/render/deferred/gpu_culling.cpp "static_assert(sizeof(shared::DrawRecord) == 64);"}}

Four `uint4`/`float4` rows: the index range, vertex offset and instance slot; the
material slot and the one-sided flag; the local box's two corners. The material slot is
not a material: the fragment shader resolves it through the instance's reference list,
so the contract that matters is the table layout below. What is left in the G-buffer's
push block is per frame — the SSS tile grid across and down, and two words of padding
that keep the range at 16 bytes:

{{cite ohao/render/deferred/gbuffer_pass.hpp "static_assert(sizeof(DrawPushConstant) == 16);"}}

`gbuffer.frag` declares the same four words in the same order:

{{cite shaders/includes/scene/gbuffer_body.glsl "layout(push_constant) uniform PushConstants { uint tilesX; uint tilesY; uint pad0; uint pad1; } pc;"}}

The size assertion pins the C++ struct, not the GLSL block; those two match by hand.
The SSS constants the shader also needs — the tile size, the header length, the list
capacity — come from `shaders/shared/layout.json` through the generated
`shared/layout.glsl`, the same file the C++ side reads as `gpu/shared_layout.hpp`, so
those cannot drift.

The instance table is the boundary that carries the weight. `InstanceGpu` is declared
once in C++ and once in `scene_data.glsl`, whose header names its twin:

{{cite shaders/includes/scene/scene_data.glsl "// Matches ohao/render/scene/render_scene_types.hpp (std430)."}}

Six shaders include that file — both G-buffer stages, the shadow vertex shader, and
the path tracer's closest-hit, any-hit and GI closest-hit — so a layout edit has to
be made in exactly two places, and only the C++ half is size-checked. The
declaration is shared across shaders but not across the language boundary.

## One packer, two readers

The material rows are where the deferred and path-traced pipelines meet. The G-buffer
pass, the transparent pass and both path tracers read the same rows — seven `vec4`s, 112
bytes a material — out of one storage buffer, and one function writes them:

{{cite ohao/render/scene/material_table.cpp "d.roughness, d.metallic, bits(d.normalTex), bits(d.emissiveTex),"}}

The size of a row is asserted against `MaterialGpuPack::kBytes`:

{{cite ohao/render/scene/material_table.hpp "static_assert(sizeof(Pack) == layout::MaterialGpuPack::kBytes);"}}

but which float is which lane is not, and each reader restates it in a comment beside
its own decode — in `gbuffer.frag`:

{{cite shaders/includes/scene/gbuffer_body.glsl "vec4 row1 = matRows[m * kMaterialRows + 1u];   // roughness, metallic, normalTex, emissiveTex"}}

and in the path tracer's closest-hit shader:

{{cite shaders/rt/path_tracer.slang "float roughness = row1.x, metalness = row1.y;"}}

The transparent pass decodes the same lanes again by hand. They agree today. Because there is a single packer the
pipelines cannot disagree about lane order, but the agreement is hand-written copies of one
diagram in several shaders, with the size assert as the only mechanical witness. A reordered
row needs coordinated edits in every reader and fails only as wrong pixels.

## Two sentinel decoders in one shader

Inside `gbuffer.frag` the "no texture" value is decoded two different ways. Albedo and
normal compare against the sentinel; roughness-metallic and emissive compare
against a magnitude instead:

{{cite shaders/includes/scene/gbuffer_body.glsl "    if (roughMetalTexIdx < 4096u) {"}}

4096 is not arbitrary — it is the bindless array's capacity as the renderer
constructs the manager:

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

The two tests agree on every value the packer produces, so this is not a live bug;
it is a hazard in two directions. `< 4096u` rejects a garbage lane and
`!= 0xFFFFFFFFu` does not — so if a lane ever carries an arbitrary bit pattern, the
albedo and normal lookups pass the sentinel test and index `textures[]` with it,
while the other two reject it. And the bound is a literal, so raising `maxTextures`
past 4096 hides the high descriptors from two of the four lookups and leaves them
visible to the other two. The path tracer's closest hit shares the literal: it tests
every index against 4096, the size its bindless array is declared at.

{{cite shaders/rt/path_tracer.slang "if (normalTex < 4096u)"}}

## What reaches the GPU from a material

The GPU-facing description of a material is `render::MaterialDesc`: OpenPBR's base
inputs (base colour and weight, metalness, diffuse roughness, specular weight, colour,
roughness and IOR, emission colour and strength), seven bindless texture indices
(albedo, normal, roughness-metallic, emissive, occlusion, specular, specular colour),
a subsurface strength, the alpha mode, its cutoff, an opacity and the base colour's
alpha. `MaterialTable::pack` writes them into seven `vec4` rows, every lane used:

{{cite ohao/render/scene/material_table.cpp "bits(d.roughMetalTex), d.emissiveStrength, d.subsurface, bits(d.occlusionTex),"}}

{{cite ohao/render/scene/material_table.cpp "d.specularIor, d.diffuseRoughness, d.baseWeight, bits(d.specularTex),"}}

Nothing else in the engine is a GPU material: the scene's `Material` component is
resolved into a `MaterialDesc` on its way to the render world, and the table is the
only buffer a shader reads materials from.

:::why
Sizes are pinned from a constants header rather than derived from the shaders. The
alternative — one declaration shared by both sides, either a header the GLSL
`#include`s or SPIR-V reflection at pipeline creation — makes *offsets* checkable,
not just totals. The toolchain already supports the include half, and the render
world uses it: `scene_data.glsl` is included by three shader sources. A third route goes
further: `shaders/shared/layout.json` describes a struct once and `tools/gen_layout.py`
generates its GLSL, Slang and C++ copies, the C++ one with an assert on every
field's offset. The deferred light record, `LightGpu`, is declared there, and both
its readers, the shared light loop `clustered_lights.slang` (deferred lighting and the
forward passes) and `light_cluster.slang`, take it from the generated modules. The cost of sharing declarations is that every C++ struct becomes
downstream of shader preprocessing or a generator, and layout questions become
build-step failures. The older structs on this page took the cheap half — one place to
edit a size, review for everything else.
:::

{{cite ohao/gpu/shared_layout.hpp "static_assert(offsetof(LightGpu, direction) == 16);"}}

:::key
`OHAO_ASSERT_GPU_LAYOUT` proves two things about a C++ type: that it can be
memcpy'd, and that it is N bytes. It cannot see the GLSL that reads those bytes,
so every defect that leaves the total unchanged — a reordered field, a lane that
changed meaning, a shader block written against an older struct — passes it.
:::

## Contracts

- `PTPushConstants` sits exactly at the 256-byte limit the code assumes, so no field can be appended; a new per-frame value must reuse a spare word.
- Nothing queries `maxPushConstantsSize`, and the path tracer's block exceeds Vulkan's guaranteed 128 bytes. On a minimum-spec device the failure is a rejected pipeline layout, not a failed build.
- Three structs carry `OHAO_ASSERT_GPU_LAYOUT` (`LightData`, `GPULight`, `PTPushConstants`); `InstanceGpu`, `MeshGpu`, `Vertex`, `CameraUBO` and `DrawPushConstant` are size-checked with a bare `static_assert`. A layout edit must update the C++ struct, its GLSL twin and the assert together.
- `InstanceGpu` exists once in C++ and once in `scene_data.glsl`; three shader sources read the GLSL copy, and the path tracer reads the buffer as raw `float4` rows. Only the C++ side is size-checked.
- `MaterialTable::pack` is the only writer of the seven-`vec4` material row; `gbuffer_body.glsl`, `rt/path_tracer.slang` and the transparent pass each decode it by hand. The row size is asserted, the lane order is not.
- `gbuffer.frag` decodes one sentinel two ways — `!= 0xFFFFFFFFu` for albedo and normal, `< 4096u` for roughness-metallic and emissive. That literal must track the texture manager's `maxTextures` by hand, and so must the path tracer's, which declares its array at 4096.
- A new material input reaches a shader only through `MaterialDesc`, `MaterialTable::pack` and `kMaterialRows`; every hand decoder of the rows must change with it.
