---
module: gpu
id: buffers-alloc
title: Buffers and allocator
standard: v2
---

## The allocator that serves one subsystem

OHAO compiles the whole Vulkan Memory Allocator into the engine. `gpu_allocator.cpp`
is the translation unit that defines `VMA_IMPLEMENTATION`, so every OHAO binary
carries VMA's suballocator and memory-type heuristics.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "#define VMA_IMPLEMENTATION"}}

Exactly one subsystem allocates through it: the render world. `RenderScene` owns a
`GpuAllocator` as a member, and every buffer its tables hold — the mesh arenas, the
material, instance and light tables, the TLAS instance buffer, and the per-frame
staging buffers — comes from it.

{{cite ohao/render/scene/render_scene.cpp "GpuAllocator allocator;"}}

{{cite ohao/render/scene/mesh_registry.cpp "a.buf = m_ctx.allocator->createBuffer(elemSize * capacity, usage, AllocationUsage::GpuOnly);"}}

The other `GpuAllocator` in the tree is a smoke test:

{{cite tests/renderer/renderer_pipeline_tests.cpp "GpuAllocator allocator;"}}

The two other subsystems that hold a `GpuAllocator*` — `RenderGraph` and
`BindlessTextureManager` — are handed a null pointer by the code that builds them
(the render graph through a defaulted argument in `deferred_renderer.cpp`), and
neither ever dereferences it:

{{cite ohao/gpu/vulkan/renderer.cpp "m_textureManager->initialize(m_device, m_physicalDevice, nullptr, 4096,"}}

Counting call sites settles the shape: 73 raw `vkAllocateMemory` calls across 27
`.cpp` files under `ohao/`, against exactly two `vmaCreateBuffer`/`vmaCreateImage`
calls, both inside `gpu_allocator.cpp` itself. Render targets, bindless texture
images, shader binding tables, acceleration-structure storage and every readback
buffer still own a private `VkDeviceMemory`; the scene's geometry and tables do not.

:::why
Routing the engine through `GpuAllocator` depended on one flag. VMA must be told at
creation time that it may return memory usable with `vkGetBufferDeviceAddress`, and
the render world's arenas and TLAS instance buffer are created with
`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`. The allocator now sets it, with a comment
explaining what goes wrong without it — VMA silently skips the allocate flag in
release builds, and the later address query is invalid:

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;"}}

The flag tags every block, so the device must have been created with
`bufferDeviceAddress`, which the logical-device setup guarantees. The remaining raw
allocations are the ones that chain `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` into their
own allocate info — acceleration-structure scratch and storage in
`rt_acceleration_structure.cpp`, and the one shader binding table left in the tree,
the path tracer's in `path_tracer_pipeline.cpp` (RT shadows and RT GI are compute
passes of inline ray queries and have none).

{{cite ohao/render/rt/path_tracer_pipeline.cpp "VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR |"}}

Those would need their own review before moving over; the flag does not block it.
:::

The class forces a dedicated `VkDeviceMemory` block for anything over 256 KB, and
unconditionally for images.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "if (size > 256 * 1024) { // 256KB threshold"}}

Suballocating large resources out of shared blocks is most of what an allocator is
for, and the render world's biggest buffers all cross that line — the vertex arena
starts at a million vertices of 100 bytes — so for them `GpuAllocator` returns one
allocation per buffer. The saving is in the small tables and the staging buffers, and
in the accounting.

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
warning when it is nonzero. `RenderScene::shutdown` destroys its tables first and the
allocator last, so a buffer it forgot to release is exactly what that warning reports.

{{cite ohao/gpu/vulkan/gpu_allocator.cpp "if (m_stats.currentUsage > 0) {"}}

## Device-local for the scene, host-visible for the leftovers

The scene's geometry and tables are `GpuOnly`: nothing maps them, and every write is a
copy. The host-visible allocations that remain are the ones the CPU rewrites often or
reads back. Each frame slot owns one persistently mapped staging buffer in the render
world, grown when a frame's dirty bytes exceed it:

{{cite ohao/render/scene/render_scene.cpp "AllocationUsage::CpuToGpu, true);"}}

and the raster light tables the deferred lighting pass reads, one storage buffer per
frame slot, are raw `vkAllocateMemory` memory through `rtc::createHostBuffer`:
host-visible and coherent, device-local as well where the GPU exposes such a heap,
mapped once and replaced only when a slot's light count outgrows it:

{{cite ohao/render/rt/rt_compute.cpp "// Device-local memory the host can write (resizable BAR) when there is some, else"}}

`LightClusters` writes a slot's table only after that slot's fence wait, and only when
the light list's version differs from the one the slot already holds, so a still scene
uploads nothing:

{{cite ohao/render/deferred/light_clusters.cpp "if (version != 0 && s.version == version && s.count == count && s.lights.buffer) return;"}}

A mesh is therefore resident once, in device memory, whether it is drawn, traced or
used as a BLAS input; there is no host-visible twin. In exchange every change to the
scene costs a staged copy recorded into a frame's command buffer instead of a
`memcpy` through a mapping, which is why edits are batched into contiguous runs.

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

## A table of any size, two type encodings

`LightData` is four `vec4`s plus a `mat4` light-space matrix, 128 bytes, and the raster
array holds up to 65,536 of them, the GPU table's capacity:

{{cite ohao/render/scene/render_scene_types.hpp "constexpr uint32_t MAX_LIGHTS = 65536;"}}

`LightTable::set` builds the array once per light-list change: regular lights in
creation order first, then the emissive-mesh proxies appended, both under the one cap,
with a warning logged once when the list exceeds it. The GPU copy is not `LightData`:
`LightClusters` writes 64-byte `LightGpu` records, the four `vec4`s without the matrix.

{{cite ohao/render/scene/light_table.cpp "OHAO_LOG(Warn, render, "RenderScene: {} lights: the raster table holds {}", sorted.size(), MAX_LIGHTS);"}}

`LightData.position.w` carries the *shader* type (0 = directional, 1 = point, 2 = spot,
3 = rectangle), remapped from the source `LightKind`, whose first two are the other way
round:

{{cite ohao/render/scene/light_table.cpp "const int deferredType = d.kind == LightKind::Point         ? 1"}}

`params.z` is a point or spot light's sphere radius, the radius its emitter mesh is drawn
at (raised to the path tracer's 0.01 m), or 0 for a true point. The lighting pass gives
such a light the specular of a sphere, so that a mirror reflects the mesh:

{{cite ohao/render/scene/light_table.cpp "(d.kind == LightKind::Point || d.kind == LightKind::Spot) && d.radius > 0.0f ? std::max(d.radius, 0.01f) : 0.0f;"}}

`lightSpaceMatrix` is identity for every scene light: no shadow map reads it. Shadows come
from the RT visibility channels in `params.w`, or from the cascades for directional
lights.

`LightType`'s fourth enumerator, `AreaRect` (`turntable.cpp` authors a ceiling panel for
its Cornell mode), is a rectangle in both pipelines. The raster record repacks its four
`vec4`s for it: the rectangle's centre in `position`, its edges in `direction.xyz` and
`params.xyz`, and in `color.w` its radiance `I / (4 pi r^2)`, the path tracer's, instead
of an intensity:

{{cite ohao/ecs/components.hpp "enum class LightType : uint8_t { Point, Directional, Spot, AreaRect };"}}

{{cite examples/turntable.cpp "area.type = ecs::LightType::AreaRect;"}}

{{cite ohao/render/scene/light_table.cpp "light.position = glm::vec4(d.position + 0.5f * (d.areaEdge1 + d.areaEdge2), 3.0f);"}}

## A scene without lights

`setFrameLights` hands the deferred renderer the scene's raster lights and nothing else. A scene
that contributed none is lit by its environment, or the constant ambient without one, as the path
tracer lights it; no light is made up for it:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "m_deferredRenderer->setLights(lights, version, m_ambientOverride.value_or(0.35f));"}}

## Emissive meshes flattened into point lights

`emissiveProxy` runs once per model upload and converts an emissive material into a
point-light proxy — the cheap stand-in for real mesh-light sampling. For the first
emissive-textured material it walks the texels, keeps those above a Rec. 709 luminance
of 0.05, and averages the survivors into a colour:

{{cite ohao/render/scene/model_upload.cpp "const float lum = pr * 0.2126f + pg * 0.7152f + pb * 0.0722f;"}}

The 8-bit texels are divided by 255 with no transfer-function decode, so threshold
and average are computed in whatever encoding the loader stored, not in linear
radiance. Intensity is the summed luminance scaled by 0.1 and clamped:

{{cite ohao/render/scene/model_upload.cpp "light.intensity = std::min(totalPower * 0.1f, 20.0f);"}}

Because `totalPower` accumulates per texel, the same lamp authored at 4K and at
256² yields very different intensities — brightness scales with texture resolution,
not emitting area. The light is also placed at the centre of the *whole model's*
bounding box rather than the emissive material's triangles, and the scan stops
after one material:

{{cite ohao/render/scene/model_upload.cpp "return std::nullopt;   // one light per model: only the first emissive material counts"}}

A character with an emissive visor and emissive boot strips gets one light,
floating at the character's centroid. In the raster array it sits after the regular
lights, so it is the first thing a full light list drops.

## vk_utils: a formula that got inlined instead

`vk_utils.hpp` is the thin C++20 layer over Vulkan, and its reach is narrower than
its surface: `vk_failed` and `vk_result_name` have one production call site each,
both inside `gpu_allocator.cpp`, plus one assertion apiece in
`tests/engine/engine_tests.cpp`; `vk_check` has neither. The helper that carries
weight is `as_mapped_span`, which turns a persistently mapped `void*` into a typed
`std::span` behind a `GpuPod` (trivially copyable, standard layout) constraint;
its one route in is `GpuBuffer::mappedAs`, which no caller uses — the render world
reads its mapped staging memory through the raw pointer from `getMappedData()`.

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
The allocation model is split along who owns the lifetime. The render world's geometry,
tables and staging go through `GpuAllocator`, device-local and written only by copies;
everything else — render targets, texture images, acceleration-structure storage,
binding tables, the raster light tables — is one hand-rolled `vkAllocateMemory` per
resource, host-visible and permanently mapped for what the CPU rewrites each frame.
Judge a change against whichever side of that line the resource sits on.
:::

## Contracts

- Emissive proxies travel in the same light list as every other light and are appended after the regular ones; the raster array holds at most `MAX_LIGHTS`, so a full list drops proxies first. Setting the list again rebuilds both consumers' arrays.
- Two light-type encodings coexist inside one loop: the source `LightKind` (Point, Directional, Spot, AreaRect, EmissiveProxy) and the shader type packed into `position.w` (directional 0, point 1, spot 2). The remap is right for the first three and silently folds AreaRect into spot; the shadow-caster test reads the source encoding where the rest of the loop reads the packed one. Any edit here must state which encoding it is in.
- `MAX_LIGHTS` (`render_scene_types.hpp`) and `kLightCapacity` (`shaders/shared/layout.json`) are both 65,536 and must stay equal: the light table caps its array at the first, `LightClusters` its GPU table at the second, with its own warning.
- The raster light table is one host-visible allocation per frame slot. `DeferredRenderer::render` has `LightClusters` write only the current slot's buffer, after that slot's fence wait, and hands that buffer to the lighting pass, which binds it in the same slot's descriptor set. Writing any other slot's buffer races the frames still in flight; skipping the write when the version matches is safe only because each slot remembers the version it holds.
- Every buffer the render world holds is created `GpuOnly` or as a mapped staging buffer through its own `GpuAllocator`; anything it replaces goes through the retire queue, never an immediate `destroyBuffer`.
- `GpuAllocator::createBuffer` and `destroyBuffer` must both account `allocation.getSize()`; crediting requested bytes on one side drifts unsigned `currentUsage` downward until a debit underflows it, and `shutdown()` reports the wrapped value as a leak.
- The device-address flag on the VMA allocator requires the device to have enabled `bufferDeviceAddress`; an allocator built against a device without it tags every block with a flag the device cannot honour.
