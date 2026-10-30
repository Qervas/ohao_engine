---
module: path-tracer
id: hit-miss
title: Hit and miss shaders
standard: v2
---

## Four entry points, one file

The path tracer's pipeline holds four stages, and both profiles take them from one
file: the entry points `raygen`, `closestHit`, `anyHit` and `miss` of
`rt/path_tracer.slang`. They resolve a hit from the RenderScene tables in the one
descriptor set.

## The payload: a whole surface

In `path_tracer.slang` the closest hit resolves every OpenPBR input the raygen needs
and hands it back in one `PathPayload`: position, distance, geometric and shading
normals, base colour and weight, specular colour, weight, roughness and IOR,
metalness, diffuse roughness, emission, and for an emitter mesh its light.

{{cite shaders/rt/path_tracer.slang "// What a ray brings back: a surface's place and its OpenPBR inputs (resolved from its material"}}

The struct is declared once, in the file that holds all four stages, so the stages
cannot disagree about its layout. The raygen turns it into `MaterialInputs` and builds
`OpenPBRBase` from that; nothing about the surface is decoded in the raygen.

The triangle is found through the instance slot (`InstanceID()`, the TLAS custom
index), the mesh table and the index arena; its material through the triangle's slot
and the instance's material references:

{{cite shaders/rt/path_tracer.slang "c.material = triSlot < info.z ? materialRefs[info.y + triSlot] : 0u;"}}

Both normals are built in world space and turned toward the ray. The geometric normal
comes from the triangle's corners by the inverse transpose; the shading normal is the
interpolated vertex normal, flipped to the geometric normal's side. Which side the
winding faces is kept, because an emitter emits from its front only.

{{cite shaders/rt/path_tracer.slang "const bool front = dot(geo, dir) < 0.0;   // the side the winding faces (emitters emit from it)"}}

### Normal maps in the tangents' frame

A normal map is applied in the frame of the vertex tangents, Gram-Schmidt
orthogonalised against the shading normal, with the bitangent's sign from the tangent's
`w` — the frame the G-buffer uses, so one normal map perturbs a surface the same way in
both renderers. A mesh without tangents falls back to a basis built from the normal
alone (Duff et al.).

{{cite shaders/rt/path_tracer.slang "T = normalize(tw - n * dot(n, tw));"}}

A mapped normal that ends up at or below the geometric surface is pushed back just
above it, so a BSDF sampled about it cannot point every direction into the surface.

{{cite shaders/rt/path_tracer.slang "if (dot(n, geo) < 1e-3) n = normalize(n + geo * (1e-3 - dot(n, geo)));"}}

### The material's rows

A material is `kMaterialRows` (7) `vec4`s. The closest hit reads rows 0, 1, 2, 4, 5
and 6: base colour, roughness and metalness, the texture handles, specular weight and
colour, specular IOR, diffuse roughness, base weight, and emission colour. Texture
handles are float bit patterns; an index at or past 4096 means none. Roughness and
metalness are multiplied by the G and B channels of the roughness-metallic texture,
the specular weight by the specular texture's alpha, the specular colour by its
texture, and emission is `emission_color × emission_luminance`, times the emissive
texture when there is one.

{{cite shaders/rt/path_tracer.slang "float3 emission = row6.rgb * row2.y;"}}

Hit shaders have no derivatives, so every fetch samples level 0. The path tracer
computes no ray differentials, so minified textures alias.

### An emitter mesh

A light's emitter mesh (`kInstanceEmitter`) returns its radiance on the front side,
black behind, and its light index, which RenderScene stores in the instance's
`emitterLight.w`. The raygen uses the index to weigh a bounce ray's hit against
light sampling of the same light.

{{cite shaders/rt/path_tracer.slang "payload.light = uint(c.instanceRow9.w + 0.5);"}}
{{cite ohao/render/scene/render_scene.cpp "d.emitterLight = glm::vec4(want[i].radiance, static_cast<float>(rtIndex[i]));"}}

## The any-hit: alpha

The any-hit shader implements the alpha modes from material row 3 (mode, cutoff,
opacity, base alpha). Coverage is the base alpha times opacity times the albedo
texture's alpha. `Mask` ignores a hit below the cutoff; `Blend` is stochastically
present, accepted with probability equal to the coverage by a hash of the launch
pixel, the ray and the triangle, so samples converge to the blend.

{{cite shaders/rt/path_tracer.slang "// Blended: stochastically present, by a hash of the ray and the triangle."}}

The stage runs only for instances the TLAS marks non-opaque (an alpha material), and
only while rays trace without forcing opacity. The light buffer's header word 3 says
whether any such instance exists; when none does, every ray forces opacity and no
any-hit runs.

{{cite shaders/rt/path_tracer.slang "uint traceFlags() { return alphaRays() ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE; }"}}

## The miss

The miss shader only clears the hit flag; the raygen looks up the environment itself
(`envRadiance`) and evaluates its density (`envPdf`) when it needs them. That keeps a
shadow ray's miss to one store, and an escaping shadow ray pays for no density it
never reads.

{{cite shaders/rt/path_tracer.slang "payload.flags = 0u;"}}

:::why
With no environment map a miss returns black, not a constant ambient: `envRadiance`
returns zero when the header names no environment texture. A fake sky term is wrong
for closed scenes: rays that slip through a wall seam would carry sky light, and its
grain, into a sealed room. Indoor scenes are lit by their own lights.
{{cite shaders/rt/path_tracer.slang "if (tex >= 4096u) return float3(0.0);"}}
:::

## One equirectangular convention

Every environment lookup maps a direction to equirectangular UVs with row 0 at the
zenith, the way `stbi_loadf` loads the file. `envRadiance` writes

$$v = \frac{1}{2} - \frac{\arcsin d_y}{\pi}$$

{{cite shaders/rt/path_tracer.slang "const float2 uv = float2(atan2(dir.z, dir.x) / kTwoPi + 0.5, 0.5 - asin(clamp(dir.y, -1.0, 1.0)) / kPi);   // row 0 the zenith"}}

and `envPdf` and `sampleEnv`, which work against the CDF buffers, use
$v = \arccos(d_y)/\pi$. Since $\arccos x = \tfrac{\pi}{2} - \arcsin x$ these are the
same number.

{{cite shaders/rt/path_tracer.slang "const float theta = acos(clamp(dir.y, -1.0, 1.0));"}}

`sampleEnv` draws a point uniformly inside the chosen texel, which matches the
piecewise constant density `envPdf` reports, so the environment estimator has no
quadrature error to converge past.

{{cite shaders/rt/path_tracer.slang "const float u = (float(x) + rng.next()) / float(W);"}}

:::key
The closest hit does all the material work and hands the raygen a complete OpenPBR
surface; the miss does nothing but say "missed".
:::

## Contracts

- `PathPayload` is declared once in `path_tracer.slang`, beside all four stages that use it.
- The instance slot is the hit shaders' only handle on a hit: the TLAS custom index indexes the instance table, which carries the mesh and the material references. A TLAS built with any other custom index breaks every lookup.
- A material is `kMaterialRows` (7) `vec4`s. A shader that indexes the table with another stride reads the neighbouring material.
- An emitter instance's `emitterLight.w` is its light's index in the RT light buffer; the raygen's MIS reads it through `payload.light`.
- Any-hit runs only for non-opaque instances and only while header word 3 is set; a new trace must not force opacity while it is, or it shadows through every cut-out.
- A shadow ray's escape is `kHit` cleared by the miss shader; it depends on the closest hit being skipped and the caller presetting the flag.
