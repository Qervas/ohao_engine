---
module: materials
id: pack
title: GPU material pack
standard: v2
figures: [materials-pack-matrow]
---

## A struct that exists on only one side

There is no material struct on the GPU. The path tracer binds the table as a flat array
of `float4` and nothing else:

{{cite shaders/rt/path_tracer.slang "[[vk::binding(10, 0)]] StructuredBuffer<float4> materials;    // kMaterialRows a material"}}

The G-buffer fragment shader binds the *same buffer* in its own set under another name and
the same element type:

{{cite shaders/includes/scene/gbuffer_body.glsl "layout(set = 1, binding = 1, std430) readonly buffer Materials { vec4 matRows[]; };"}}
 A material is therefore a convention rather than a
type: seven consecutive vec4s, twenty-eight float lanes, and a meaning per lane that the
producer and each consumer have to agree on independently. The closest hit reassembles the
rows by hand, one indexed load per row, before it knows what any lane holds:

{{cite shaders/rt/path_tracer.slang "const float4 row0 = materials[m], row1 = materials[m + 1u], row2 = materials[m + 2u];"}}

`layout_meta.hpp` is where that convention is written down. The stride there is
derived from the slot count — `kMaterialBytes = kMaterialVec4s * sizeof(glm::vec4)`
— and the derived value is then pinned against a literal:

{{cite ohao/gpu/layout_meta.hpp "static_assert(MaterialGpuPack::kBytes == 112"}}

The producer is tied to it: `MaterialTable::Pack` is a `std::array<float, 28>` (seven rows since OpenPBR's
specular, IOR, diffuse roughness, base weight and emission colour joined), and a
`static_assert` equates its size with `MaterialGpuPack::kBytes`.

## Integers that have to travel as floats

Seven of the twenty-eight lanes hold bindless texture indices — uint32 values in an
array whose element type is float. `pack` reinterprets the bits instead of
converting the value:

{{cite ohao/render/scene/material_table.cpp "const auto bits = [](uint32_t u) { return std::bit_cast<float>(u); };"}}

The "no texture" case is the all-ones uint32, `kNoTexture`, and it goes through the
same cast. The path tracer reverses it with `asuint` and tests the result against the
bindless table's size, 4096, which the sentinel also fails:

{{cite shaders/rt/path_tracer.slang "if (albedoTex < 4096u) baseColor *= sampleTex(albedoTex, uv).rgb;"}}

A value conversion would not have worked: binary32 represents integers exactly
only up to $2^{24}$, and 0xFFFFFFFF has no exact float representation at all.
Reinterpretation is exact, but it parks every index in a numerically hostile
region. Let $i$ be the uint32 index and $\mathrm{bits}^{-1}(i)$ the binary32
number whose bit pattern equals $i$. For $0 < i < 2^{23}$ the exponent field is
all zeros, so the result is subnormal:

$$\mathrm{bits}^{-1}(i) = i \cdot 2^{-149}$$

Bindless indices are small, so *every nonzero texture index in the pack is a
subnormal float* (index 3 reads as $4.2 \times 10^{-45}$); index 0 reinterprets
as $+0.0$, and the sentinel is a NaN. All three survive only because nothing does
arithmetic on those lanes — they are memcpy'd in, loaded, bit-cast out. A pass
that multiplied, interpolated, or ran
under flush-to-zero there would not crash; it would silently repoint the scene's
textures.

## One writer, and the indices exist before the row does

The pack is assembled in exactly one place. `MaterialTable::pack` turns a
`MaterialDesc` into its twenty-eight lanes in a single expression, and `add` appends the
row to a host array and marks its index dirty:

{{cite ohao/render/scene/material_table.cpp "m_packs.push_back(pack(desc));"}}

That is possible because a `MaterialDesc` already holds finished bindless indices by
the time it is registered. `uploadModel` loads each model texture into the bindless
manager first — all albedo, then normal, rough/metal, emissive, one upload per
material — writes the returned index into the material's field, and only then
registers the material. A row is never written with sentinels and patched later, and
no lane has two writers:

{{cite ohao/render/scene/model_upload.cpp "mats[i].emissiveStrength = i < model.materialEmissionStrength.size() ? model.materialEmissionStrength[i] : 1.0f;"}}

The host array is the only CPU copy, and device memory is written only by copies the
frame records. `recordUploads` walks the dirty set, coalesces contiguous indices into
runs, and records one copy per run through the frame slot's staging buffer:

{{cite ohao/render/scene/material_table.cpp "batch.upload(m_buffer.buffer, VkDeviceSize{first} * sizeof(Pack), &m_packs[first],"}}

so a host write can never race a frame still reading the table. Row 0 is registered
at initialisation as the default `MaterialDesc{}`, and it is the fallback twice over:
an instance whose material list is shorter than a submesh's slot resolves to it, and
a registration into a full table (capacity 4096) logs once and returns it.

{{cite ohao/render/scene/material_table.hpp "static constexpr uint32_t kCapacity = 4096;"}}

Which row a hit uses is decided per instance, not per material: the closest-hit
shader reads the triangle's slot from the mesh's slot stream, then the instance's
material-reference list, with the same arithmetic as the G-buffer's `OHAO_MATERIAL_INDEX`
macro written out in `cornersOf`, so raster and RT agree about which material a triangle
has.

Row 2 holds the roughness/metal texture index, the emissive strength and the
subsurface strength, which only the raster G-buffer reads (to flag SSS tiles and sign
the normal target's fourth channel; neither tracer reads it). The last lane is the
glTF occlusion texture's index, bit-cast like the others; only the G-buffer reads it (AO
from its R channel, for the ambient terms), since a path tracer needs no baked occlusion.

{{cite ohao/render/scene/material_table.cpp "bits(d.roughMetalTex), d.emissiveStrength, d.subsurface, bits(d.occlusionTex),"}}

Row 3 holds the alpha mode (as a float), the alpha cutoff, the opacity and the base
colour's alpha. They are plain floats, read by the G-buffer's alpha-tested variant, the
path tracer's any-hit shader and the transparent pass:

{{cite ohao/render/scene/material_table.cpp "static_cast<float>(static_cast<uint32_t>(d.alphaMode)), d.alphaCutoff, d.opacity, d.baseAlpha,"}}

Rows 4 to 6 are OpenPBR's: specular colour and weight; specular IOR, diffuse roughness,
base weight and the specular-weight texture; emission colour and the specular-colour
texture. The path tracer's closest hit and the G-buffer both read all three,
and both form emission as row 6's colour times row 2's strength (emission_color ×
emission_luminance). The G-buffer stores the rest for deferred lighting in its two OpenPBR
targets and folds `base_weight` into the albedo:

{{cite ohao/render/scene/material_table.cpp "d.specularIor, d.diffuseRoughness, d.baseWeight, bits(d.specularTex),"}}

{{cite shaders/rt/path_tracer.slang "float3 emission = row6.rgb * row2.y;"}}

{{cite shaders/includes/scene/gbuffer_body.glsl "vec3 emissive = row6.rgb * row2.y;"}}

{{figure materials-pack-matrow "Conceptual byte map of one material: seven vec4 rows, 112 bytes, coloured by lane kind: scalar or bit-cast uint32 texture index. Lane names read from material_table.hpp and gbuffer_body.glsl; not a memory capture."}}

:::why
Interleaving the indices into the float rows keeps a whole material inside one
binding, and it lets raster and RT share it. The alternative — a parallel SSBO of
uints beside the colours — needs no bit-casting and no NaN/subnormal discipline, but
costs a second descriptor binding per consumer and a second buffer to keep in step.
It does not save a fetch: the closest-hit already issues a separate load per row before
it decodes a single index. OHAO pays for the one binding with the standing rule that
seven of the twenty-eight lanes are opaque bits.
:::

## The constants that are load-bearing, and the ones that are decoration

`MaterialGpuPack` offers `byteOffset()` and `vec4Count()` so nobody has to write
the stride by hand, and a canonical `kNoTexture`:

{{cite ohao/gpu/layout_meta.hpp "static constexpr uint32_t    kNoTexture = 0xFFFFFFFFu;"}}

Grep says otherwise. `byteOffset` and `vec4Count` are called only by their own
self-asserts and by `tests/engine/engine_tests.cpp`; `MaterialGpuPack::kNoTexture`
is referenced nowhere outside its own header. The renderer has a second
`kNoTexture` of its own in the scene types, with the same value, and that is the one
every producer uses:

{{cite ohao/render/scene/render_scene_types.hpp "inline constexpr uint32_t kNoTexture = 0xFFFFFFFFu;"}}

Every shader consumer takes the stride from `kMaterialRows`, a constant generated from
`shaders/shared/layout.json` — `matRows[m * kMaterialRows + 0u]` in the G-buffer,
`c.material * kMaterialRows` in the path tracer's closest hit. `kMaterialVec4s` repeats that number by hand
(its comment names `gpu::shared::kMaterialRows`), so the static asserts are a lock, not
a derivation: editing one without the other breaks the size checks on the C++ side
only.

## A header that only looks like padding

The light SSBO plays the same game with a sharper move. `GPULight` is five vec4s,
80 bytes, and binding 11 holds a header followed by the lights. Under std430 an
array of vec4-aligned structs cannot begin at offset 4 — it begins at 16, and the light
table names exactly that as its header size:

{{cite ohao/render/scene/light_table.hpp "static constexpr VkDeviceSize kHeaderBytes = 16;"}}

The 12 bytes between the count and the first light look like alignment slack.
They are not: they hold the environment-map index, the HDRI intensity and the
alpha-rays flag. The path tracer reads the binding as bare `float4`s, the header at
index 0 and light $i$ at rows $1+5i$ to $5+5i$:

{{cite shaders/rt/path_tracer.slang "const uint b = 1u + i * 5u;"}}

The header is written whole whenever the table is dirty: the light count, the environment index,
the intensity scale and the alpha-rays flag.

{{cite ohao/render/scene/light_table.cpp "const uint32_t header[4] = {static_cast<uint32_t>(m_rt.size()), m_envMapTexIdx,"}}

The environment index and intensity are stored in the light table itself
(`setEnvMapTextureIndex`, `setEnvIntensity`), and changing either marks the table
dirty, so a rebuild of the lights cannot lose them. What remains of the coupling is
its shape: environment state riding in a light list's header.

## What the layout assertion cannot see

Both packs are guarded by one macro:

{{cite ohao/render/rt/gpu_light.hpp "OHAO_ASSERT_GPU_LAYOUT(GPULight, layout::kGPULightBytes);"}}

It expands to two static asserts — that the type satisfies `GpuPod` (trivially
copyable, standard layout) and that `sizeof` equals the stated number. Both are
facts about C++. Nothing in the tree reflects the SPIR-V: the path tracer's `loadLight`
decodes the five rows by hand. The assertion catches a C++ edit and is blind to a shader
edit.

The same header carries a constant that reads like a hard limit and is not one:

{{cite ohao/render/rt/gpu_light.hpp "static constexpr uint32_t MAX_GPU_LIGHTS = 64;"}}

It is referenced: the light table sizes the
device buffer as the header plus 64 lights and truncates a longer list with a
one-time log. The path tracer clamps its light choice against the header's count,
which is already post-truncation. The deferred side bounds its table the same way but
at a different scale: up to `kLightCapacity` 65,536 `LightGpu` records, generated from
`shaders/shared/layout.json` into the C++, GLSL and Slang copies at once, so its shader
struct, unlike `GPULight`, cannot drift from the C++ one without `gen_layout.py --check`
failing. The deferred loop is bounded by the pixel's froxel list, not by the table:

{{cite shaders/shared/layout.json "{"name": "kLightCapacity", "type": "uint", "value": 65536,"}}

## The bill for an overloaded lane

Compactness is bought by giving one lane several meanings, and the light pack
shows the price. `dirAndParam.w` is a radius for sphere lights and an inner cone
angle in degrees for spot lights:

{{cite ohao/render/rt/gpu_light.hpp "w=param (sphere:radius, spot:innerAngle)"}}
{{cite ohao/render/scene/light_table.cpp "dirParam = d.innerConeDeg;"}}

The overload is resolved by hand in `loadLight`, which takes a spot's radius from
`extra2.y` and a sphere's from `dirAndParam.w`:

{{cite shaders/rt/path_tracer.slang "l.radius = max(l.type == 2u ? f.y : d.w, 0.01);"}}

The C++ comment on `dirAndParam` names both meanings and does not say that a spot's
radius moved; `extra2`'s comment does. A reader that took `dirAndParam.w` as the radius
for every kind would place a default spot's emitter on a sphere the size of its cone
angle:

{{cite ohao/ecs/components.hpp "float innerConeDeg{30.0f};"}}

30 *world units* around the light. Nothing would be out of range, no assertion would
fire, the pack would still be exactly 80 bytes, and the units would be wrong.

:::key
The static asserts prove that 64 is 64 and 80 is 80. Nothing proves that a lane
still means what its comment says, or that a shader's decoding of a struct still
matches the C++ one. Every hazard in this unit is semantic — a half-written
comment, a re-hardcoded stride, an angle that could be read as a radius — sitting
underneath a size contract that passes.
:::

## Contracts

- A `MaterialDesc` must carry finished bindless indices when it is registered. The table has no second pass that patches texture lanes in, so a texture uploaded after `registerMaterial` never reaches the row.
- Nothing may do arithmetic on the seven index lanes. Nonzero indices are subnormal floats, index 0 is $+0.0$, the sentinel is a NaN; all three are safe only as opaque bits.
- Row 0 is the default material and the fallback for a short material list and a full table. Code that registers materials must not assume a returned handle is distinct from 0.
- The light-buffer header is written by `LightTable::recordUploads`, whole, whenever the table is dirty. The environment index and intensity live in the table, so a light rebuild keeps them; anything else writing binding 11 would have to reproduce the 16-byte header.
- The shader `GPULight` lives in one hand-maintained decoder, the path tracer's `loadLight`. `OHAO_ASSERT_GPU_LAYOUT` does not see it, and no test reflects the SPIR-V.
- `MAX_GPU_LIGHTS` bounds the RT light list: the device buffer is sized for 64 and a longer list is truncated with a log.
