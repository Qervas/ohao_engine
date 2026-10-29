---
module: path-tracer
id: hit-miss
title: Hit and miss shaders
standard: v2
---

## Two hit-shader sets

The path tracer's pipeline holds four stages, and each profile brings its own four.
The offline profile's are entry points of `rt/path_tracer.slang` (`closestHit`,
`anyHit`, `miss`, beside `raygen`); the realtime profile's are the GLSL files
`pt_closesthit.rchit`, `pt_anyhit.rahit` and `pt_miss.rmiss`. Both sets read the same
descriptor set, so both resolve a hit from the same RenderScene tables. They differ
in what the hit hands back.

## The reference's payload: a whole surface

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

Hit shaders have no derivatives, so every fetch samples level 0. Nothing in either RT
path computes ray differentials, so minified textures alias.

### An emitter mesh

A light's emitter mesh (`kInstanceEmitter`) returns its radiance on the front side,
black behind, and its light index, which RenderScene stores in the instance's
`emitterLight.w`. The raygen uses the index to weigh a bounce ray's hit against
light sampling of the same light.

{{cite shaders/rt/path_tracer.slang "payload.light = uint(c.instanceRow9.w + 0.5);"}}
{{cite ohao/render/scene/render_scene.cpp "d.emitterLight = glm::vec4(want[i].radiance, static_cast<float>(rtIndex[i]));"}}

## The realtime payload: a few decoded numbers

The GLSL closest hit returns less: emission in `color`, roughness, metallic and a
curvature proxy in `attenuation`, position, normal, albedo and hit distance. The
`RayPayload` struct is written out in `pt_closesthit.rchit`, `pt_miss.rmiss` and
`pt_raygen_realtime.rgen` with no shared header, so a field added to one copy and not
the others reinterprets its neighbours.

{{cite shaders/rt/pt_closesthit.rchit "payload.attenuation = vec3(roughness, clamp(metallic, 0.0, 1.0), curv);"}}

It builds a normal map's frame from the normal alone (Frisvad's construction), not from
the tangents, so directional detail (brushed metal, scratches) rotates per pixel in
the realtime image and does not match the deferred frame or the reference.

{{cite shaders/rt/pt_closesthit.rchit "worldNormal = normalize(T * mapN.x + B * mapN.y + worldNormal * mapN.z);"}}

When the interpolated normal is degenerate it falls back to a box normal from the sign
of the dominant object-space coordinate, and only then flips it toward the ray.
Loaders fill normals a file lacks (`ModelLoader`), so a loaded mesh does not reach the
fallback.

{{cite shaders/rt/pt_closesthit.rchit "else localN = vec3(0, 0, sign(hitLocal.z));"}}

The curvature proxy, $\kappa = \operatorname{clamp}(8\sum_{i<j}(1-\hat n_i\cdot\hat n_j), 0, 1)$
over the triangle's three vertex normals, is written to `attenuation.z` and read by no
raygen.

## The any-hit: alpha

Both any-hit shaders implement the alpha modes from material row 3 (mode, cutoff,
opacity, base alpha). Coverage is the base alpha times opacity times the albedo
texture's alpha. `Mask` ignores a hit below the cutoff; `Blend` is stochastically
present, accepted with probability equal to the coverage by a hash of the launch
pixel, the ray and the triangle, so samples converge to the blend.

{{cite shaders/rt/path_tracer.slang "// Blended: stochastically present, by a hash of the ray and the triangle."}}
{{cite shaders/rt/pt_anyhit.rahit "if (mode == kAlphaMask) {"}}

The stage runs only for instances the TLAS marks non-opaque (an alpha material), and
only while rays trace without forcing opacity. The light buffer's header word 3 says
whether any such instance exists; when none does, every ray forces opacity and no
any-hit runs.

{{cite shaders/rt/path_tracer.slang "uint traceFlags() { return alphaRays() ? RAY_FLAG_NONE : RAY_FLAG_FORCE_OPAQUE; }"}}

## The miss

The reference's miss shader only clears the hit flag; the raygen looks up the
environment itself (`envRadiance`) and evaluates its density (`envPdf`) when it needs
them. That keeps a shadow ray's miss to one store.

{{cite shaders/rt/path_tracer.slang "payload.flags = 0u;"}}

The realtime miss shader does more. It signals escape by a negative distance, fetches
the environment, and evaluates `pdfEnvMap` for the escaping direction, so an
environment shadow ray that escapes is also the radiance fetch:

{{cite shaders/rt/pt_miss.rmiss "payload.hitDist = -1.0;  // signal miss"}}
{{cite shaders/rt/pt_miss.rmiss "payload.envPdf = pdfEnvMap(dir, pc.control.w, uint(pc.tuning.y));"}}

Every other escaping shadow ray pays for that density too: an `acos`, an `atan2` and up
to four CDF loads it never reads.

:::why
With no environment map both misses return black, not a constant ambient. A fake sky
term is wrong for closed scenes: rays that slip through a wall seam would carry sky
light, and its grain, into a sealed room. Indoor scenes are lit by their own lights.
{{cite shaders/rt/pt_miss.rmiss "payload.color = vec3(0.0);"}}
:::

## One equirectangular convention

The miss shaders, the environment sampler and every other environment lookup map a
direction to equirectangular UVs with row 0 at the zenith, the way `stbi_loadf` loads
the file. The realtime miss writes

$$v = \frac{1}{2} - \frac{\arcsin d_y}{\pi}$$

{{cite shaders/rt/pt_miss.rmiss "return vec2(phi / 6.2831853 + 0.5, 0.5 - theta / 3.1415926);"}}

and `env_sampling.glsl`, which turns CDF texels into directions, uses
$v = \arccos(d_y)/\pi$. Since $\arccos x = \tfrac{\pi}{2} - \arcsin x$ these are the same
number. The reference's `envPdf` and `sampleEnv` use the $\arccos$ form against the
same CDF buffers.

{{cite shaders/includes/rt/env_sampling.glsl "float theta = v * OHAO_PI;"}}
{{cite shaders/rt/path_tracer.slang "const float theta = acos(clamp(dir.y, -1.0, 1.0));"}}

The two samplers differ in one respect. The realtime `sampleEnvMap` returns the centre
of the chosen texel, so it draws from $W \times H$ directions while reporting a
continuous density, a quadrature error no sample count removes. The reference's
`sampleEnv` draws a point uniformly inside the texel, which matches the piecewise
constant density `envPdf` reports.

{{cite shaders/includes/rt/env_sampling.glsl "float u = (float(x) + 0.5) / float(W);"}}
{{cite shaders/rt/path_tracer.slang "const float u = (float(x) + rng.next()) / float(W);"}}

:::key
The reference's closest hit does all the material work and hands the raygen a complete
OpenPBR surface; its miss does nothing but say "missed". The realtime set keeps the
older split: a small decoded payload, a Frisvad frame for normal maps, and a miss
shader that doubles as the environment lookup.
:::

## Contracts

- `PathPayload` is declared once in `path_tracer.slang`; `RayPayload` is written out in `pt_closesthit.rchit`, `pt_miss.rmiss` and `pt_raygen_realtime.rgen`. Edit one GLSL copy and you must edit all three; a mismatch is silent.
- The instance slot is the hit shaders' only handle on a hit: the TLAS custom index indexes the instance table, which carries the mesh and the material references. A TLAS built with any other custom index breaks every lookup.
- A material is `kMaterialRows` (7) `vec4`s. A shader that indexes the table with another stride reads the neighbouring material.
- An emitter instance's `emitterLight.w` is its light's index in the RT light buffer; the reference's MIS reads it through `payload.light`.
- Any-hit runs only for non-opaque instances and only while header word 3 is set; a new trace must not force opacity while it is, or it shadows through every cut-out.
- A realtime shadow ray's escape is `payload.hitDist < 0.0`; a reference shadow ray's is `kHit` cleared. Both depend on the closest hit being skipped and the caller presetting the payload.
- `pt_miss.rmiss` declares a `vec4 jitter` it never reads so its push-constant block matches the pipeline layout. Trimming it invalidates the layout.
