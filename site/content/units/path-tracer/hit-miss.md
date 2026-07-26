---
module: path-tracer
id: hit-miss
title: Hit and miss shaders
standard: v2
---

## Everything a hit knows must fit in one payload

The path tracer's bindless texture array is declared for closest-hit, any-hit and
miss only — no raygen stage appears in binding 12's mask, and none of the three
raygen profiles declares a sampler of its own.

{{cite ohao/render/rt/path_tracer_descriptors.cpp "bindings[12].stageFlags = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR"}}

Every material fact the integrator needs is therefore squeezed through a single
`RayPayload`, redeclared field-for-field in five files: the three raygen profiles,
`pt_closesthit.rchit` and `pt_miss.rmiss`. There is no shared header, so a field
added to one copy and not the others does not fail to compile — it reinterprets
the neighbouring floats.

One field is already write-only. The closest-hit publishes the instance's custom
index, which here holds a running global triangle offset rather than an actor id,
and nothing reads it back:

{{cite shaders/rt/pt_closesthit.rchit "payload.hitInstance = gl_InstanceCustomIndexEXT;"}}

A single payload also imposes an ordering rule. Shadow rays reuse miss index 0 —
the same `pt_miss.rmiss` — so a visibility test that *escapes* overwrites
`payload.color`, `payload.hitDist` and `payload.envPdf`. An occluded one runs no
shader at all: the shadow trace carries `TerminateOnFirstHit | Opaque |
SkipClosestHitShader`, so a hit invokes nothing and `payload.hitDist` keeps the
`999.0` sentinel the raygen wrote one line earlier — which is precisely how the
caller tells the two outcomes apart.

{{cite shaders/rt/pt_raygen.rgen "hitPos + N * 0.01, 0.001, L, shadowDist, 0);"}}

Either way the raygen must snapshot the hit into locals before it fires anything:

{{cite shaders/rt/pt_raygen.rgen "vec3 emissive = payload.color;"}}

Move a shadow ray above that line and the first escaping one turns the surface's
own emission into the sky.

## Rebuilding a surface from three indices

The closest-hit receives barycentrics and a primitive id, nothing else. It resolves
the triangle through the scene-wide flattened index buffer, interpolates UVs and
vertex normals, and rotates the result to world space. When the interpolated normal
is degenerate — squared length under 1e-4, what a mesh carrying no vertex normals
produces — it falls back to a box normal built from the sign of the dominant
object-space coordinate:

{{cite shaders/rt/pt_closesthit.rchit "else localN = vec3(0, 0, sign(hitLocal.z));"}}

A box normal is right for an axis-aligned quad and wrong for anything curved,
which is why the two-sided flip is gated on that same branch. In the shipped
scenes neither ever runs. Each Cornell wall is handed an explicit inward unit
normal that `addQuad` copies into all four of its vertices, the primitive sphere
uses its normalised position, and `rt_build.cpp` copies `Vertex::normal` verbatim
into the RT normal buffer — so the squared length is 1, the interpolated branch
always wins, and the fallback is dead code on the reference scene rather than the
thing that makes it work.

{{cite examples/cornell_box.cpp "v.normal = normal;"}}
{{cite ohao/scene/component/component_factory.cpp "vertex.normal = {x, y, z};"}}
{{cite ohao/gpu/vulkan/rt_build.cpp "normals[i] = glm::vec4(verts[i].normal, 0.0f);"}}

The OBJ and glTF loaders substitute `(0,1,0)` when a file carries none, so they
cannot reach the fallback either.

{{cite ohao/scene/asset/model.cpp "vertex.normal = glm::vec3(0.0f, 1.0f, 0.0f);"}}
{{cite ohao/scene/asset/model_gltf.cpp "v.normal = glm::vec3(0.0f, 1.0f, 0.0f);"}}

The one live path that can still produce a zero normal is FBX: `ufbx` writes the
vertex normal only when the mesh has a normal layer, so a mesh without one leaves
`Vertex::normal` at its zero-initialised default.

{{cite ohao/scene/asset/model_fbx.cpp "if (mesh->vertex_normal.exists)"}}

Real meshes therefore get no flip, and every TLAS instance disables triangle
culling, so a hit from behind keeps its outward normal.

{{cite ohao/render/rt/rt_acceleration_structure.cpp "vkInst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;"}}

Nothing goes negative — every NEE branch clamps `N·V` to 0.001 — but `N·L` is
clamped at zero, so every light on the far side contributes exactly nothing and
the back face renders black rather than wrongly lit.

{{cite shaders/rt/pt_raygen.rgen "float NdotL = max(dot(N, L), 0.0);"}}

From the same three vertex normals the shader derives a curvature proxy: the total
angular disagreement between them, scaled by eight and clamped.

$$\kappa = \operatorname{clamp}\!\Big(8\sum_{i<j}\big(1-\hat{n}_i\cdot\hat{n}_j\big),\,0,\,1\Big)$$

$\hat{n}_0,\hat{n}_1,\hat{n}_2$ are the triangle's normalised vertex normals and
the sum runs over their three distinct pairs. Read literally, that is the spread
of one triangle's normals — edge length times curvature — so it tracks tessellation
as much as shape. The comment above it calls the value near zero on smoothly-curved
regions; the engine's own sphere contradicts that. At 32 sectors by 16 stacks
adjacent vertex normals differ by $\pi/16 \approx 0.196$ rad, so on an equatorial
face the three pair terms sum to about $0.077$ and $\kappa \approx 0.6$ —
arithmetic on those two constants, not a measurement.

{{cite ohao/scene/component/component_factory.cpp "const int stacks = 16;"}}
{{cite shaders/rt/pt_closesthit.rchit "curv = clamp(curv * 8.0, 0.0, 1.0);"}}

It fails the other direction too: a hard crease authored with split per-face
normals presents three identical vertex normals, giving $\kappa = 0$ exactly. So
what the closest-hit computes is a tessellation-weighted smoothness estimate, not
the corner detector the comment describes — a usable knob, not geometry.

The value rides in `payload.attenuation.z`, computed once in the closest-hit for
all three profiles, and has exactly one reader — the offline profile's subsurface
term, which uses it to widen the diffusion kernel:

{{cite shaders/rt/pt_raygen_offline.rgen "float curvature = clamp(payload.attenuation.z, 0.0, 1.0);"}}

The shared decoder every raygen runs on that payload touches `.x` and `.y` only,
so under the default and realtime profiles the field is written and never read.

{{cite shaders/rt/includes/pbr_unpack.glsl "void unpackHitPbr(vec3 att, out float roughness, out float metallic) {"}}

## A tangent frame that does not know about UVs

The RT vertex stream carries no tangent, so normal mapping in the closest-hit
builds an orthonormal basis from the shading normal alone — Frisvad's
$a = 1/(1+n_z)$ construction, which buys its speed by avoiding a normalisation and
a square root, not a branch. The branch is unavoidable: the form is singular at
$n_z = -1$, and the shader guards it explicitly.

{{cite shaders/rt/pt_closesthit.rchit "if (worldNormal.z < -0.9999) {"}}

The tangent-space normal is then applied in that basis:

{{cite shaders/rt/pt_closesthit.rchit "worldNormal = normalize(T * mapN.x + B * mapN.y + worldNormal * mapN.z);"}}

The engine already documents why that basis is poor when direction matters:
`ggx_aniso.glsl` rejected Frisvad for its anisotropy tangent for exactly this
reason.

{{cite shaders/includes/material/ggx_aniso.glsl "varies discontinuously with N — neighbouring pixels on a sphere see totally"}}

The rasteriser has the real thing — a per-vertex tangent with handedness,
Gram-Schmidt orthogonalised against the shading normal:

{{cite shaders/core/gbuffer.frag "T = normalize(fragTangent.xyz - N * dot(N, fragTangent.xyz));"}}

So one normal map perturbs in a UV-aligned frame under the deferred pipeline and in
an arbitrary, N-dependent frame under the path tracer. The `mapN.z` component — the
bulk of most normal maps — survives either way, which is why RT renders still look
bumpy. Directional detail does not: brushed metal, scratches and woven cloth rotate
per pixel in the RT path and will not match the raster preview.

## One descriptor array, two image formats

The material row is three `vec4`s indexed by material id, with texture handles
smuggled through float bit patterns and `0xFFFFFFFF` meaning "none":

{{cite shaders/rt/pt_closesthit.rchit "vec4 matParams2 = matColorBuf.matColors[matID * 3u + 2u];"}}

Binding 3, the per-instance buffer carrying an older sign-and-magnitude encoding,
is still declared here and never indexed:

{{cite shaders/rt/pt_closesthit.rchit "layout(set = 0, binding = 3) readonly buffer MaterialBuffer"}}

Scene textures arrive as layers of one `R8G8B8A8_UNORM` array image exposed through
per-layer 2D views, which is why colour fetches need a manual transfer-function
decode an `_SRGB` view would have applied for free:

{{cite ohao/gpu/vulkan/rt_build.cpp "imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;"}}
{{cite shaders/rt/pt_closesthit.rchit "albedo *= pow(sampled, vec3(2.2));"}}

The HDR environment map sits in the *same* GLSL array as an `R32G32B32A32_SFLOAT`
image — legal because combined-image-sampler descriptors are independent — which is
how the miss shader reads values above 1.0 from a binding whose other entries are
clamped to `[0,1]`. The array has a single mip level and nothing in the RT path
computes ray differentials, so minified textures alias.

## The miss shader is both the sky and the shadow test

`pt_miss.rmiss` signals escape by writing a negative distance, and that one float
is the entire occlusion protocol:

{{cite shaders/rt/pt_miss.rmiss "payload.hitDist = -1.0;  // signal miss"}}

This is what lets environment next-event estimation cost one ray instead of two.
The raygen draws a direction from the environment CDF, traces a shadow ray along
it, and if the ray escapes it consumes the radiance the miss shader just wrote —
visibility and the light's emission resolved in a single traversal:

{{cite shaders/rt/pt_raygen.rgen "vec3 envContribution = envRadiance * brdf * NdotL_env * w / envPdf;"}}

The same economy is a cost on every other shadow ray that escapes: area-light NEE
traces through the identical miss shader, which fetches the environment texel and
then evaluates `pdfEnvMap` — an `acos`, an `atan2` and up to four indexed CDF loads — to
report a PDF that caller never reads. The expensive half is not shared: the two
binary searches live in `sampleEnvMap`, which only the raygen calls.

{{cite shaders/rt/pt_miss.rmiss "payload.envPdf = pdfEnvMap(dir, pc.control.w, uint(pc.tuning.y));"}}

:::why
With no environment map the miss returns black, not a constant ambient. A fake sky
term is the cheaper-looking choice and it is wrong for closed scenes: GI rays that
slip through a wall seam pick it up and carry sky light — and its grain — back into
a sealed room. Indoor scenes are lit only by their own lights.
{{cite shaders/rt/pt_miss.rmiss "payload.color = vec3(0.0);"}}
:::

## Two equirectangular conventions, one texture

The miss shader and the environment importance sampler both map directions to
equirectangular UVs, and they do not agree. For a unit direction $d$ the miss
shader uses

$$v_{\text{miss}} = \frac{\arcsin d_y}{\pi} + \frac{1}{2}$$

{{cite shaders/rt/pt_miss.rmiss "return vec2(phi / 6.2831853 + 0.5, theta / 3.1415926 + 0.5);"}}

while `env_sampling.glsl`, which turns CDF texels into directions and inverts the
map for MIS, uses

$$v_{\text{cdf}} = \frac{\arccos d_y}{\pi}$$

{{cite shaders/includes/rt/env_sampling.glsl "float theta = v * OHAO_PI;"}}

Since $\arccos x = \tfrac{\pi}{2} - \arcsin x$, the two are related by
$v_{\text{miss}} = 1 - v_{\text{cdf}}$ — an exact vertical mirror. The horizontal
axis agrees; both derive $u$ from $\operatorname{atan2}(d_z, d_x)$. They would still
be consistent if the CDF's rows ran opposite to the texture's, but `EnvCDF::build`
consumes the same `stbi_loadf` array that is memcpy'd unflipped into the image.

{{cite ohao/gpu/vulkan/light_upload.cpp "envCDF.build(hdrPixels, ew, eh);"}}

A direction drawn from the brightest row of the CDF is therefore looked up by the
miss shader in the mirrored row. Engine-wide the convention splits two against two,
not one outlier against the rest: the miss shader and `deferred_lighting.frag` both
build $0.5 + \theta/\pi$ from $\theta = \arcsin d_y$, while `env_sampling.glsl` and
the cinematic composite both land on $0.5 - \theta/\pi$. What matters is not the
census but which pair shares a texture — and the two that disagree here are the
sampler that picks the direction and the miss shader that shades it.

{{cite shaders/core/deferred_lighting.frag "vec2 envUV = vec2(phi / 6.2831853 + 0.5, theta / 3.1415926 + 0.5);"}}
{{cite shaders/rt/cinematic_composite.comp "vec2  uv    = vec2(phi / (2.0 * PI) + 0.5, 0.5 - theta / PI);"}}

MIS still combines two well-formed weights, but the estimator underneath is not
unbiased either. `sampleEnvMap` throws away the residual of $u_1,u_2$ after the two
searches and returns `equirectPixelToDir`, which resolves the texel *centre* — so
the procedure draws from a discrete set of $W \times H$ directions while reporting
a continuous solid-angle density.

{{cite shaders/includes/rt/env_sampling.glsl "float u = (float(x) + 0.5) / float(W);"}}

The env-NEE term is thus a midpoint Riemann sum over texel centres, evaluated
against the bilinearly filtered radiance the miss shader fetches: a systematic
quadrature error that no sample count removes, sitting on top of an importance
distribution the mirrored $v$ leaves anti-correlated with the radiance it exists to
track. Both are read from the code, not measured.

## The any-hit that traversal never calls

`pt_anyhit.rahit` implements a hard 0.5 cutout, and the shader itself is sound: it
takes alpha from the diffuse texture, so any textured cutout material would be
tested correctly if the stage ever ran.

{{cite shaders/rt/pt_anyhit.rahit "uint diffuseTexIdx = floatBitsToUint(matColor.a);"}}

It never runs. That is the whole of what makes it inert: every BLAS is built with
`VK_GEOMETRY_OPAQUE_BIT_KHR` and every `traceRayEXT` passes `gl_RayFlagsOpaqueEXT`,
either of which alone suppresses the stage.

A second, independent gap sits behind that one. The material row has no alpha slot
— the `.a` lane of the base-colour row is where the diffuse texture index lives —
so an untextured material with a transparent base colour could not be cut out even
with traversal fixed. `MaterialData` does carry an alpha mode, but only the glTF
loader ever writes it; the OBJ and FBX paths leave the default, and it reaches no
GPU buffer in any case:

{{cite ohao/scene/asset/model_gltf.cpp "mat.alphaMode = MaterialData::AlphaMode::MASK;"}}
{{cite ohao/scene/asset/model.hpp "enum class AlphaMode { OPAQUE, MASK, BLEND } alphaMode"}}

What ships instead is deletion at load time — the OBJ path drops alpha-card
triangles by material name:

{{cite ohao/scene/asset/model.cpp "// Filter alpha-card geometry that lacks proper alpha textures"}}

:::key
Read the any-hit as a specification, not as behaviour. Making it run takes three
independent changes — clear the geometry opaque bit, drop the per-ray opaque flag,
and add an alpha or alpha-mode field to the material row — and any one of them
alone changes nothing.
:::

## Contracts

- `RayPayload` is duplicated across `pt_closesthit.rchit`, `pt_miss.rmiss` and all three raygen profiles with no shared header. Edit one and you must edit all five; a mismatch is silent.
- Shadow rays use miss index 0, so one that escapes clobbers `payload.color`, `payload.hitDist` and `payload.envPdf`; an occluded one runs no shader and leaves the payload alone. Read albedo, normal, position and emission into locals before tracing anything.
- `payload.hitDist < 0.0` is the only occlusion signal. A miss that stops writing it turns every shadow ray into a hit and every surface black.
- `pt_miss.rmiss` declares a `vec4 jitter` it never reads, purely so its push-constant block size matches the layout the raygen defines. Trimming it invalidates the layout.
- Only degenerate-normal geometry is flipped two-sided, and no shipped scene produces one; culling is disabled TLAS-wide, so a real mesh hit from behind keeps its outward normal and renders black through the `N·L` clamp.
- Normal maps use a Frisvad basis in the path tracer and a UV-derived tangent basis in the GBuffer. Directional detail will not match between the two.
- The closest-hit's header comment still describes a two-`vec4` material layout; the code reads three. Binding 3 is declared here and never indexed.
