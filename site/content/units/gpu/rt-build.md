---
module: gpu
id: rt-build
title: RT build (BLAS/TLAS)
standard: v2
---

## One id resolves a material, and nothing is walked

A path-traced hit resolves its material through four lookups and no per-scene walk.
The TLAS instance carries the render-world instance slot as its custom index. The
closest-hit shader uses that slot to find the instance, the instance to find the mesh,
and `gl_PrimitiveID` to find the triangle's submesh slot in the mesh's per-triangle
stream. The slot is then resolved through the instance's material-reference list to a
row of the material table:

{{cite shaders/rt/pt_closesthit.rchit "uint slot = triSlot[mesh.triangleSlotOffset + uint(gl_PrimitiveID)];"}}
{{cite shaders/rt/pt_closesthit.rchit "uint matID = OHAO_MATERIAL_INDEX(inst, slot, materialRefs);"}}

The resolution is a macro shared by the raster and RT shaders, and it is total: a slot
past the instance's reference list falls back to material 0, the default.

{{cite shaders/includes/scene/scene_data.glsl "((slot) < (inst).materialRefCount ? (refs)[(inst).materialRefOffset + (slot)] : 0u)"}}

Nothing here depends on iteration order. Every table is keyed by an identity the
instance carries — slot, mesh index, material index — instead of by position in a list
that several loops must walk identically, so there is no visibility predicate to keep
in step across files. The cost is moved to the edges, where the identities are
assigned. A reference that names a material the table does not hold is clamped to 0 on
the host when the instance is written, so the shader never indexes past the table:

{{cite ohao/render/scene/instance_table.cpp "refs[i] = (h.valid() && h.index < materialCount) ? h.index : 0u;"}}

and the custom index has only 24 bits, which is the ceiling on live instance slots:

{{cite ohao/render/scene/render_scene.cpp "vi.instanceCustomIndex = slot & 0xFFFFFFu;   // 24 bits"}}

## One BLAS per mesh, built at registration

Each mesh gets exactly one bottom-level structure, built when the mesh is registered,
against the shared vertex and index arenas. The geometry is a single triangle list over
the mesh's slice, so submeshes do not become separate geometries: the per-material
information rides in the triangle-slot stream, indexed by primitive id, rather than in
geometry indices. A build that cannot happen — no ray tracing, or fewer than three
indices — leaves the mesh without a BLAS:

{{cite ohao/render/scene/mesh_registry.cpp "if (!m_ctx.rt || !m_ctx.rt->isSupported() || mesh.indexCount < 3) return;"}}

The instance that references such a mesh is still created and still rasterises; in the
TLAS its acceleration-structure reference is zero, which Vulkan defines as an inactive
instance:

{{cite ohao/render/scene/render_scene.cpp "// A reference of 0 makes the instance inactive (no BLAS: failed build)."}}

BLASes are built for fast trace and cannot be updated, so a BLAS never references its
inputs after the build and survives the arenas being replaced by larger ones.

{{cite ohao/render/scene/mesh_registry.cpp "// A built BLAS (FAST_TRACE, no ALLOW_UPDATE) does not reference its inputs, so"}}

{{cite ohao/render/rt/rt_acceleration_structure.cpp "buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;"}}

The structures belong to the `RenderScene`. It owns its own `RTAccelerationStructure`
and decides itself whether ray tracing is available; the renderer asks it.

## A TLAS that is built or refitted inside the frame

The TLAS is not rebuilt per frame; it is built, refitted or left alone, decided from
what changed. Each instance slot has a 64-byte `VkAccelerationStructureInstanceKHR` kept
in a host mirror and a device-local buffer, and an edit to the slot recomputes its
mirror entry. A change that alters the instance's *mask* — visibility — or its BLAS
reference, or that creates or destroys the slot, is structural. A change to the
transform alone is not.

{{cite ohao/render/scene/render_scene.cpp "if (structural || cur.mask != vi.mask ||"}}

The recording step reads that flag: a structural change, or no TLAS yet, builds; a
transform-only change refits in `UPDATE` mode; nothing changed, nothing recorded.

{{cite ohao/render/scene/render_scene.cpp "const bool build = tlasStructural || rtAccel->getTLAS() == VK_NULL_HANDLE;"}}

Destroying an instance does not remove its slot from the TLAS. The slot's flags are
zeroed so the GPU copy reads as invisible, and the instance's mask becomes 0 — it stays
in the structure and no ray can hit it until a new instance reuses the slot:

{{cite ohao/render/scene/instance_table.cpp "m_gpu[slot].flags = 0;   // the GPU copy reads as invisible until the slot is reused"}}

Slots are reused lowest-first, and a reused slot never refits — creation is always
structural — because the custom index and mesh binding changed under the old structure.

If the allocation for a build fails, the dirty entries have already been uploaded, so the
failure marks the next build structural rather than keeping the instance data dirty:

{{cite ohao/render/scene/render_scene.cpp "tlasStructural = true;   // full build next frame covers this frame's changes"}}

The TLAS storage is replaced when the instance count outgrows it, so the handle a pass
read last frame can differ from this one. The dispatcher therefore binds the scene
buffers to the path tracer only after `recordUpdates` has run:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// After recordUpdates: table growth and TLAS (re)creation replace handles."}}

and the replaced storage goes to the retire queue and is destroyed only after
`MAX_FRAMES_IN_FLIGHT` frames, because earlier frames may still be tracing against it.
The barrier after the build names the stages that read the structure; compute and
fragment stages are included only when the device enabled ray query, since naming a
stage whose extension was not enabled is invalid usage:

{{cite ohao/render/scene/render_scene.cpp "if (info.rayQuery) consumers |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;"}}

:::why
The alternative is a full TLAS rebuild every frame from an instance list rebuilt by
walking the world — simple, and it needs no dirty tracking, but it pays the build cost
for scenes where only a few objects move. Refit is much cheaper than a rebuild and
degrades quality only gradually; the structural/refit split keeps the cheap path for
the common case, a few transforms changing, and takes the rebuild exactly when the
topology of what can be hit changed. What it costs is a flag that must be set by every
edit that changes the topology: forget one and the refit silently traces a stale
structure.
:::

## Base color, applied twice, on purpose

The material row reserves a slot for a path-tracer-only fallback texture. A material with
no albedo map is given a 1×1 pixel holding its base colour, transfer-encoded to sRGB,
uploaded as `UNORM` and shared by name between every material of the same encoded
colour:

{{cite ohao/render/scene/model_upload.cpp "const std::array<uint8_t, 4> texel{linearToSRGB(baseColor.r), linearToSRGB(baseColor.g),"}}
{{cite ohao/render/scene/model_upload.cpp "std::span<const uint8_t>(texel), 1, 1, VK_FORMAT_R8G8B8A8_UNORM, BindlessTextureType::Albedo,"}}

It is attached only when the material has no albedo texture and has not already been
given one:

{{cite ohao/render/scene/model_upload.cpp "if (m.albedoTex == kNoTexture && m.legacyRtTintTex == kNoTexture)"}}

The closest-hit shader seeds `albedo` with the material's base colour and multiplies in
the albedo texture when there is one — the glTF convention. For the fallback it
multiplies in the tint pixel decoded by an explicit power:

{{cite shaders/rt/pt_closesthit.rchit "albedo *= pow(textureLod(textures[nonuniformEXT(legacyTintTexIdx)], texUV, 0.0).rgb, vec3(2.2));"}}

For a real texture that is right. For the fallback the pixel *is* the factor. Writing
$c$ for the material's linear base colour and $\mathrm{srgb}(\cdot)$ for the 8-bit
transfer encode applied at upload:

$$\text{albedo} \;=\; c \cdot \big(\mathrm{srgb}(c)\big)^{2.2} \;\approx\; c^{2}$$

because the 2.2-power decode inverts the encode to within 8-bit quantisation. A base
colour of 0.73 shades as roughly 0.53. The raster path applies the colour once, so an
untextured surface is darker in the path tracer than in the G-buffer. The deviation is
deliberate and labelled in the shader — the fallback exists so the path-traced golden
corpus keeps matching — and it is gated: a material with an albedo texture takes the
first branch of the shader, an untextured material without a tint takes neither, and
only the tinted case squares.

{{cite shaders/rt/pt_closesthit.rchit "if (diffuseTexIdx != 0xFFFFFFFFu) {"}}

:::key
The diffuse sentinel is live: a material can have no albedo and no tint, and then the
base colour applies once. The squaring is confined to the materials `uploadModel`
tinted, and anyone who removes the tint must also re-baseline the path-traced goldens,
because their pixels encode the squared value.
:::

## Textures at their native size, base level only

Textures reach the hit shaders as separate bindless images, each at its own resolution
and format, so there is no common extent to resample to. What the hit shaders cannot do
is choose a mip: ray-tracing stages have no implicit derivatives, so the shaders sample
the base level.

{{cite shaders/rt/pt_closesthit.rchit "Hit shaders have no derivatives: sample the base level (native resolution)."}}

The images carry mip chains for the raster passes' benefit and the path tracer never
reads them, so minification aliasing is a real source of noise on high-frequency
textures; it is not something sampler state can recover.

The alpha test is inert. The any-hit shader samples the albedo alpha and ignores
intersections below 0.5, but every BLAS geometry is built opaque and every `traceRayEXT`
in the raygens passes `gl_RayFlagsOpaqueEXT`, so the pipeline never invokes it.

{{cite shaders/rt/pt_anyhit.rahit "ignoreIntersectionEXT;"}}

## What an edit costs, and the one word RT GI still needs

There is no material-patching entry point: the material table is append-only, and a
material is immutable once registered. The edits a caller can make are on the instance,
and each has a distinct cost:

- A **transform** change uploads one `InstanceGpu` row and refits the TLAS.
- A **visibility** or **mesh** change uploads the row and forces a full TLAS build.
- A **material list** change uploads the instance's 64-entry reference row and records
  no TLAS work, because the TLAS entry is byte-identical.

RT global illumination still needs one value the material table does not hold. Its
closest-hit reads the bounce albedo from the instance, not the material row, as the RGB
of a legacy word on `InstanceGpu`:

{{cite shaders/rt/rt_gi.rchit "vec3 albedo = instances[gl_InstanceCustomIndexEXT].giAlbedoPacked.rgb;"}}

Its alpha is a sign-and-magnitude packing — sign carries metallic, magnitude carries
roughness, and a ±10 bias tags meshes with more than 100 vertices — produced on the
host and uploaded to an instance word that nothing in `shaders/` reads. A comment on
the struct marks it for deletion when RT GI is reworked.

{{cite ohao/render/scene/model_upload.cpp "float packed = metallic > 0.5f ? -(roughness + 0.001f) : roughness;"}}

## Contracts

- Every TLAS instance's custom index is its render-world slot; the hit shaders index the instance table with it. Slots above 2^24 − 1 alias.
- The triangle-slot stream is read with `gl_PrimitiveID`, so a mesh's BLAS must be a single geometry over exactly the mesh's index range. Splitting a mesh into several geometries makes `gl_PrimitiveID` restart per geometry and breaks the slot lookup.
- A change to an instance's mask (visibility), mesh or existence must reach the TLAS as a structural change; only a transform-only change may refit.
- The path tracer must be handed the scene buffers after `recordUpdates` each frame, never before: table growth and TLAS storage replacement change the handles.
- Replaced TLAS storage, like replaced tables, is destroyed by the retire queue after `MAX_FRAMES_IN_FLIGHT` frames. Destroying it eagerly can free a structure an earlier frame is still tracing.
- The tint texture is attached only to materials without an albedo map, and it squares the base colour in the path tracer. Removing it changes path-traced output and invalidates the goldens.
- All BLAS geometry is opaque and every raygen trace passes the opaque flag; an alpha-tested material will not cut out until both change.
