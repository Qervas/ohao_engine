---
module: gpu
id: buffers-alloc
title: Buffers and allocator
standard: v2
---

## The allocator that is linked but never called

OHAO compiles the whole Vulkan Memory Allocator into the engine. `gpu_allocator.cpp`
is the translation unit that defines `VMA_IMPLEMENTATION`, so every OHAO binary
carries VMA's suballocator and memory-type heuristics.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "#define VMA_IMPLEMENTATION"}}

None of it allocates anything the renderer draws with. `GpuAllocator` is
constructed exactly once in the tree, and that once is a smoke test:

{{cite tests/renderer/renderer_pipeline_tests.cpp "GpuAllocator allocator;"}}

The two subsystems that hold a `GpuAllocator*` — `RenderGraph` and
`BindlessTextureManager` — are handed a null pointer by the code that builds them
(the render graph through a defaulted argument in `deferred_renderer.cpp`), and
neither ever dereferences it:

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

Counting call sites settles the shape: 84 raw `vkAllocateMemory` calls across 29
`.cpp` files under `ohao/`, against exactly two `vmaCreateBuffer`/`vmaCreateImage`
calls, both inside `gpu_allocator.cpp` itself. Every buffer and image the engine
renders with owns a private `VkDeviceMemory`.

:::why
The obvious cleanup — route the engine through `GpuAllocator` — does not work as
the class stands. VMA must be told at creation time that it may return memory
usable with `vkGetBufferDeviceAddress`; the flag that says so is in the source and
commented out.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp@223ff7f "// allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;"}}

Every BLAS/TLAS build input and every scratch buffer is created with
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` and allocated with
`VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` chained into the allocate info —
`rt_build.cpp` for the device-local vertex and index copies,
`rt_acceleration_structure.cpp` for scratch and instance storage. So is every shader
binding table, though no SBT is built in either of those files: there is one each in
`path_tracer_pipeline.cpp`, `rt_shadow_technique.cpp` and `rt_gi_technique.cpp`.

{{cite ohao/render/rt/path_tracer_pipeline.cpp "VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |"}}

Today's `GpuAllocator` would hand all of them memory it cannot take an address into.
:::

A second reason it would not pay for itself yet: the class forces a dedicated
`VkDeviceMemory` block for anything over 256 KB, and unconditionally for images.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "if (size > 256 * 1024) { // 256KB threshold"}}

Suballocating large resources out of shared blocks is most of what an allocator is
for; with that branch, `GpuAllocator` returns one allocation per non-trivial
resource — the hand-rolled topology, plus a dependency.

## The leak counter both sides must agree on

`AllocationStats` is maintained by hand, and its two sides must use the same number.
`destroyBuffer` debits `VmaAllocationInfo::size`, what VMA actually reserved:

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "VkDeviceSize size = buffer.allocation.getSize();"}}

VMA documents that field as possibly *greater* than the requested size — the
allocation absorbs alignment padding. `currentUsage` is an unsigned `std::size_t`, so a
credit of the caller's requested size would let each padded buffer drift the running
total downward, and the first debit that exceeded what was left — destroying the last
live buffer would do it — would wrap the subtraction to a value near $2^{64}$. So the
buffer path credits the VMA allocation size, the same quantity the debit uses and the
same one `createImage` credits:

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "const VkDeviceSize accountedSize = result.allocation.getSize();"}}

Nothing polls the counter mid-run: `hasLeaks()` and `liveBytes()` are defined on
`AllocationStats` and called from nowhere in `ohao/`, `tests/` or `examples/`. The only
reader is teardown, where `shutdown()` reads `currentUsage` directly and prints a leak
warning when it is nonzero — which is where any future asymmetry would surface, as a
nonsense byte count on a run that freed everything.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "if (m_stats.currentUsage > 0) {"}}

## Host-visible for the rasteriser, staged for the ray tracer

Those 84 allocations are scattered: GBuffer render targets in `gbuffer_pass.cpp`, the
cascade array in `csm_pass.cpp`, the bloom mip chain in `bloom_pass.cpp`, device-local
RT geometry in `rt_build.cpp`. What the renderer facade itself feeds the deferred path
is narrower — the light uniform buffer in `buffer_setup.cpp` and the scene's vertex and
index buffers in `scene_upload.cpp`, all built the same way — `vkCreateBuffer`,
`findMemoryType` over `HOST_VISIBLE | HOST_COHERENT`, `vkAllocateMemory`, bind. The
light buffer is mapped once at init and never unmapped:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "vkMapMemory(m_device, m_lightBufferMemory, 0, bufferSize, 0, &m_lightBufferMapped);"}}

`updateSceneBuffers` builds the vertex and index buffers host-visible too — map,
`memcpy`, unmap:

{{cite ohao/gpu/vulkan/scene_upload.cpp "memcpy(data, combinedVertices.data(), vertexBufferSize);"}}

Nothing stages them for the draw itself: on a discrete GPU, vertex and index fetches
read host memory across PCIe instead of local VRAM. In exchange there is no transfer
queue, no ownership transfer and no upload barrier, and a geometry edit is a `memcpy` —
a fair trade for headless renders whose frames cost seconds, and the first thing to
revisit now that a windowed host draws deferred frames against a frame budget.

They are staged, just not for raster. Both buffers carry
`VK_BUFFER_USAGE_TRANSFER_SRC_BIT`, which the raster pipeline has no use for:

{{cite ohao/gpu/vulkan/scene_upload.cpp "VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;"}}

Before it returns, `updateSceneBuffers` calls
`buildAccelerationStructures()`. On a device with ray tracing that path allocates a
`DEVICE_LOCAL` twin of each buffer and fills it with a one-shot `vkCmdCopyBuffer` on
the graphics queue:

{{cite ohao/gpu/vulkan/rt_build.cpp "vkCmdCopyBuffer(cmd, srcBuf, buf, 1, &copyRegion);"}}

A loaded mesh is therefore resident twice on any RT-capable card — host-visible for the
rasteriser, device-local for BLAS builds and the closest-hit shader's vertex and index
fetch.

## The far plane the cascades are told

`renderDeferred` hands `DeferredRenderer` the camera's own view and projection, which
it uses for the GBuffer, the lighting pass and SSR. The camera is default-constructed
and nothing in the tree calls `setPerspectiveProjection`, so its clip planes stay at the
constructor's 0.1 and 100.

{{cite ohao/render/camera/camera.hpp "float farPlane{100.0f};"}}

The `0.1f, 1000.0f` passed alongside the matrix are not clip planes and never reach a
projection. They land in `m_nearPlane`/`m_farPlane`, whose only consumer is the
cascaded shadow map:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "m_deferredRenderer->setCameraData(view, proj, camPos, 0.1f, 1000.0f);"}}

{{cite ohao/render/deferred/deferred_renderer.cpp "m_csmPass->setCameraData(m_view, m_proj, m_nearPlane, m_farPlane);"}}

The real defect is there. `calculateCascadeSplits` blends a logarithmic and a linear
split across $[n, f]$, and `calculateLightViewProj` then walks the camera's own frustum
corners by the fraction $(d - n)/(f - n)$. Both halves must use the same $f$ or they
describe different volumes — and they do not: the splits run out to 1000, the corners
come from a projection that clips at 100.

{{cite ohao/render/deferred/csm_pass.cpp "float d = m_splitLambda * log + (1.0f - m_splitLambda) * linear;"}}

With the shipped `m_splitLambda` of 0.95 and four cascades the splits land near 13.5,
34.5, 132.5 and 1000. `selectCascade` in `deferred_lighting.frag` compares view depth
against those, so a fragment 30 units out picks cascade 1 — whose matrix, rescaled over
the 100-unit frustum, actually covers depth 1.4 to 3.5. The three near cascades are
compressed into the first ~13 world units of a 100-unit view, and most of what is
routed to them falls outside their shadow maps.

## Eight slots, two type encodings

Lights are a fixed array, not a growable list. `LightData` is four `vec4`s plus a
`mat4` light-space matrix — 128 bytes — and the uniform block holds eight:

{{cite ohao/gpu/vulkan/renderer.hpp "constexpr uint32_t MAX_LIGHTS = 8;"}}

Fill order is scene `LightComponent`s first, then cached emissive-mesh lights
appended, both capped at eight, so a scene with eight explicit lights drops every
emissive light without a warning. Exactly one light in the buffer may cast a
shadow — a latch takes the first qualifying one and every later light gets a
shadow index of −1 and an identity light-space matrix:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "int shadowCasterIndex = -1;  // Index of first shadow-casting light"}}

Which light qualifies is where two enumerations collide. `LightData.position.w`
carries the *shader* type (0 = directional, 1 = point, 2 = spot), and a two-way ternary
remaps into it — `Sphere` to 1, `Directional` to 0, anything else to 2. Further down
the same iteration the shadow test re-reads the raw `LightComponent` enum and accepts
types 0 and 2 — where 0 is `Sphere`:

{{cite ohao/scene/component/light_component.hpp "Directional = 1,  // sun/moon (parallel rays)"}}

An authored directional light therefore never claims the shadow slot, while a
sphere light does — and `calculateLightSpaceMatrix`, dispatching on the
already-remapped `position.w = 1`, matches neither its directional nor its spot
branch and returns identity. The synthesized fallback sun is unaffected: it is
written directly as shader type 0 and takes the orthographic branch. That is why
the defect hides — shadows look right until someone adds a light to the scene.

The ternary's "anything else" arm hides a second collision. `LightType` has a fourth
enumerator, `AreaRect = 3`, with no branch of its own, so the raster light buffer emits
it as a spot light carrying the component's default 15°/30° cone and its default
straight-down direction. It is not a hypothetical enumerator — `turntable.cpp` authors
a ceiling area panel for its Cornell mode, and the RT path reads the same component
correctly, packing both edge vectors and the derived area into `GPULight`:

{{cite examples/turntable.cpp "al->setLightType(LightType::AreaRect);"}}

{{cite ohao/gpu/vulkan/light_upload.cpp "if (lc->getLightType() == LightType::AreaRect) {"}}

The two pipelines therefore disagree about what that actor is: a rectangle to the path
tracer, a cone to the rasteriser.

## The default light and its $\pi$

If the scene contributed nothing, a default directional light is synthesized with
intensity $\pi$ and a warm tint:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "defaultLight.color = glm::vec4(1.0f, 0.98f, 0.95f, glm::pi<float>());"}}

The $\pi$ is not decoration; it calibrates against the $1/\pi$ in an
energy-conserving diffuse lobe. Take the Lambertian idealisation first: a BRDF of
$\rho/\pi$ gives, for a light of intensity $I$,

$$L_o = I \cdot \frac{\rho}{\pi} \cos\theta$$

with $\rho$ the base colour and $\theta$ the angle between surface normal and light
direction. Setting $I = \pi$ makes a white albedo return $L_o = 1$ head-on — a clean
reference white — instead of $1/\pi \approx 0.318$, which is $\log_2 \pi \approx 1.65$
stops underexposed.

The shipping lobe is not Lambert. The deferred lighting pass, the one consumer of
this buffer, calls `evaluateBRDF`, whose diffuse term is Burley/Disney scaled by $k_D = (1 - F)(1 - m)$
for Fresnel $F$ and metalness $m$:

{{cite shaders/includes/brdf/brdf_common.glsl "return diffuseColor * INV_PI * lightScatter * viewScatter;"}}

{{cite shaders/includes/brdf/brdf_ggx.glsl "vec3 kD = (vec3(1.0) - F) * (1.0 - surface.metallic);"}}

Burley's two scatter factors both go to 1 at normal incidence, so it does collapse to
$\rho/\pi$ exactly where the calibration is aimed — but $k_D$ trims 4% off a dielectric
at $F_0 = 0.04$, and the Kulla-Conty multi-scatter term adds a little back. $I = \pi$
lands near reference white, not on it.

## Emissive meshes flattened into point lights

`cacheEmissiveLights` runs once per scene upload and converts emissive materials
into `LightData` — the cheap stand-in for real mesh-light sampling. For the first
emissive-textured material on an actor it walks the texels, keeps those above a
Rec. 709 luminance of 0.05, and averages the survivors into a colour:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "float lum = pr * 0.2126f + pg * 0.7152f + pb * 0.0722f;"}}

The 8-bit texels are divided by 255 with no transfer-function decode, so threshold
and average are computed in whatever encoding the loader stored, not in linear
radiance. Intensity is the summed luminance scaled by 0.1 and clamped:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "float intensity = std::min(totalPower * 0.1f, 20.0f);"}}

Because `totalPower` accumulates per texel, the same lamp authored at 4K and at
256² yields very different intensities — brightness scales with texture resolution,
not emitting area. The light is also placed at the centre of the *whole model's*
bounding box rather than the emissive material's triangles, and the scan stops
after one hit:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "break;  // one light per actor"}}

A character with an emissive visor and emissive boot strips gets one light,
floating at the character's centroid.

## vk_utils: a formula that got inlined instead

`vk_utils.hpp` is the thin C++20 layer over Vulkan, and its reach is narrower than
its surface: `vk_failed` and `vk_result_name` have one production call site each,
both inside `gpu_allocator.cpp`, plus one assertion apiece in
`tests/engine/engine_tests.cpp`; `vk_check` has neither. The helper that carries
weight is `as_mapped_span`, which turns a persistently mapped `void*` into a typed
`std::span` behind a `GpuPod` (trivially copyable, standard layout) constraint;
its one caller is `GpuBuffer::mappedAs`, which belongs to the allocator nothing in
the renderer allocates through.

`mip_levels_for` needs a more careful verdict than "unused". Its integer shift loop
has no production callers — only that same test file, which pins its 1×1 and 256×128
answers — but the identical quantity, $\lfloor \log_2 \max(w,h) \rfloor + 1$, is
computed in floating point at both places that actually build mip chains, the bindless
texture uploader and the bloom pass:

{{cite ohao/gpu/vulkan/bindless_texture_manager.cpp "std::floor(std::log2(std::max(width, height)))"}}

{{cite tests/engine/engine_tests.cpp "mip_levels_for(256, 128) == 9u"}}

The function is unreachable from the renderer; the formula is not. Deleting the helper
breaks the `engine_tests` build, and concluding that the engine does not compute mip
counts would be wrong on top of that.

:::key
Nothing in the shipping renderer allocates through VMA. `GpuAllocator` and most of
`vk_utils.hpp` are infrastructure written ahead of its callers; the live memory model
is one hand-rolled `vkAllocateMemory` per resource — host-visible and permanently
mapped for everything the CPU rewrites each frame, device-local for render targets and
the RT geometry copies. Judge changes against that, not against the allocator's
interface.
:::

## Contracts

- `cacheEmissiveLights()` must run before `updateLightBuffer()` or emissive lights are missing from the frame. Scene upload calls it once; the per-frame update only reads the cache.
- Two light-type encodings coexist inside one loop: `LightComponent::LightType` (Sphere 0, Directional 1, Spot 2, AreaRect 3) and the shader type packed into `position.w` (directional 0, point 1, spot 2). The remap is right for the first three and silently folds AreaRect into spot; the shadow-caster test reads the source encoding where the rest of the loop reads the packed one. Any edit here must state which encoding it is in.
- `MAX_LIGHTS` is 8 in four places: `renderer.hpp` for the C++ side, the `#define` hard-coded into `deferred_lighting.frag` — the one shader that reads the buffer, whose comment points at `offscreen_renderer.hpp`, a file that does not exist — an `#ifndef`-guarded copy in `includes/common/constants.glsl`, and `includes/common/types.glsl`, which no shader includes. Change the C++ value without the lighting shader's and the uniform block silently truncates or overruns.
- The light buffer is one host-visible allocation, not one per frame slot. `renderDeferred` rewrites it with `memcpy` after waiting on only the current slot's fence, so up to two earlier frames may still be reading it while it changes. A light that moves every frame needs a per-slot buffer.
- `GpuAllocator::createBuffer` and `destroyBuffer` must both account `allocation.getSize()`; crediting requested bytes on one side drifts unsigned `currentUsage` downward until a debit underflows it, and `shutdown()` reports the wrapped value as a leak.
- Enabling `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT` is a prerequisite for any RT buffer moving to `GpuAllocator`, not an optimisation.
