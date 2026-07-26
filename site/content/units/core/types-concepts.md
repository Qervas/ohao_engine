---
module: core
id: types-concepts
title: Types and concepts
standard: v2
---

## The predicate that is checked at the struct, not the cast

`ohao/core/` contains no Vulkan and compiles in isolation, which makes it easy to
read `concepts.hpp` and `common_types.hpp` as housekeeping. They are not. One
concept in `concepts.hpp` is what ten structs across the engine assert
themselves against before they go anywhere near device memory, and the byte
constants paired with it are the only thing standing between a C++ struct and a
GLSL block that disagree by four bytes.

`GpuPod` is two standard traits and nothing else:

{{cite ohao/core/concepts.hpp "concept GpuPod = std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>;"}}

Trivially copyable means the object's bytes can be memcpy'd in and out and the
result is still a valid object. Standard layout forbids virtual functions and
virtual bases, and forbids non-static data members spread across more than one
access-control block or across both a base and its derived class — the conditions
under which every member has a fixed declaration-order offset the compiler is not
free to move.

Together they are the minimum that makes a byte-level view of a struct defensible,
and the engine spends them at the point of declaration. `SimpleVertex`,
`CameraUniformBuffer`, `Vertex`, `PBRMaterialParams` and `FrameCameraUBO` each
carry a bare assertion directly under their own fields:

{{cite ohao/gpu/vulkan/renderer.hpp "static_assert(GpuPod<CameraUniformBuffer>"}}

and the five `OHAO_ASSERT_GPU_LAYOUT` sites — `MeshBufferInfo`,
`ObjectPushConstants`, `LightData`, `GPULight`, `PTPushConstants` — expand to the
same check plus a size check. Without it, someone adds a `std::string` to
`CameraUniformBuffer`, the write below still compiles, and the failure is a garbage
matrix on the GPU plus a heap pointer sitting in device memory where the shader
expects a float:

{{cite ohao/gpu/vulkan/buffer_setup.cpp "memcpy(frame.cameraBufferMapped, &ubo, sizeof(ubo));"}}

With the assertion, the failure is a compile error in the header that declares the
struct.

## The typed-view layer that nothing calls

Declaration site rather than cast site matters, because nothing checks at the cast.
`vk_utils.hpp` does contain the design where something would — a `GpuPod`-guarded
adapter from a mapped pointer to a typed span:

{{cite ohao/gpu/vulkan/vk_utils.hpp "return std::span<T>{static_cast<T*>(mapped), count};"}}

Exactly three functions call it: `GpuAllocation::asSpan`, the only link in the
chain that touches VMA at all, via `info.pMappedData`; and the per-frame
`cameraMappedAs` / `lightMappedAs` accessors. `GpuBuffer::mappedAs` sits one level
further out, forwarding to `asSpan`.

{{cite ohao/render/frame/frame_resources.hpp "return as_mapped_span<T>(cameraBufferMapped, count);"}}

Nothing in `ohao/`, `examples/` or `tests/` calls any of those four — nor
`GpuAllocator::createBufferFromSpan`, the remaining `GpuPod`-constrained entry
point. The chain is dead end to end, and `FrameCameraUBO` with it. What runs
instead is `memcpy` on the raw `void*`, as above — and those frame buffers are not
even VMA allocations, they come from `vkAllocateMemory` and are persistently mapped
with `vkMapMemory` — plus unguarded casts on the way back out:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "LightUniformBuffer* ld = static_cast<LightUniformBuffer*>(m_lightBufferMapped);"}}

`rt_build.cpp` and both shader-binding-table writers cast the same way. The concept
therefore holds on these paths only because the structs declared it themselves —
and `LightUniformBuffer`, the type in that cast, is one that never did. Its
`LightData` members carry the layout assertion; the enclosing struct carries
nothing.

## Why a concept is not a layout

:::key
`GpuPod` is permission to reinterpret bytes. It is not a promise about *which*
bytes. `is_standard_layout` says nothing about padding, alignment or size, and
nothing at all about std140/std430 — so a struct can satisfy the concept and still
be the wrong size for the shader block it feeds.
:::

That gap is why the header ships a second, blunter tool. `OHAO_ASSERT_GPU_LAYOUT`
re-checks the concept and adds an exact byte count, firing both as `static_assert`s
in whichever header owns the struct:

{{cite ohao/core/concepts.hpp "#define OHAO_ASSERT_GPU_LAYOUT(Type, Bytes)"}}

Four of the five macro sites take their number from one header,
`ohao/gpu/layout_meta.hpp`, which gives each one a name: 240 bytes for
`ObjectPushConstants` (three `mat4`s plus three `vec4`s), 128 for `LightData`, 80
for `GPULight` (five `vec4`s of std430), 256 for the path tracer's push block. The
same header carries the 48-byte three-`vec4` material row, which has no owning C++
struct and is asserted in place.

{{cite ohao/render/rt/gpu_light.hpp "OHAO_ASSERT_GPU_LAYOUT(GPULight, layout::kGPULightBytes);"}}

The fifth site is not allowed to reach that header. `MeshBufferInfo` lives in
`core/`, `layout_meta.hpp` lives in `gpu/`, and no header in `core/` includes
anything outside `core/` and the standard library — so its count is a bare `16`
written at the struct. Either spelling is caught the same way: what fires when
someone adds a field is the `static_assert`, not the shared header. What the shared
header buys is that two owners of the same ABI cannot independently write two
different numbers for it.

The 256 is not a stylistic budget — it is the device ceiling, and the renderer
visibly lives against it. `PTPushConstants` is full at 256 bytes, so the
per-frame sample count could not be given a field of its own. It is packed into the
high 16 bits of `params.w` beside `maxBounces` in the low 16, and each raygen
variant either unpacks it or masks it off:

{{cite ohao/render/rt/path_tracer_render.cpp "256-byte device maxPushConstantsSize, so we cannot append a field"}}

The device limit is the cause of the bit-packing; the macro from `core/` is what
turns overrunning it into a compile error at the struct rather than a runtime
validation failure at pipeline-layout creation.

The header does contain the obvious unification — a concept that folds the size
check into `GpuPod` — and it has no users anywhere in the tree:

{{cite ohao/core/concepts.hpp "concept GpuLayoutBytes = GpuPod<T> && sizeof(T) == ExpectedBytes;"}}

:::why
Unused *concept*, very much used *predicate*: `GpuLayoutBytes` and the macro test
exactly the same thing, and the macro is what ships. It won because a
`static_assert` carries a message that names the type and the expected size, while
a failed concept check on a template parameter produces a constraint-substitution
error at some instantiation far from the struct you actually broke. The split also
keeps `GpuPod` weak enough to constrain a `T` with no shader counterpart at all —
staging, readback, mapped spans — so only structs with a real GLSL block behind
them pay for the ABI assertion.
:::

## The concept that cannot check itself

`ComponentType` is the entity system's gate, and it is nominal rather than
structural — derivation from `Component`, not a member checklist:

{{cite ohao/core/concepts.hpp "concept ComponentType = std::derived_from<T, Component>;"}}

It constrains all four of `Actor::addComponent` / `getComponent` / `hasComponent` /
`removeComponent`, the `isComponentType` / `componentCast` RTTI helpers, and the
variadic `ComponentPack` that applies a whole component set to an actor in one fold
expression — and folds the constrained `hasComponent` over the pack to test one:

{{cite ohao/scene/component/component_pack.hpp "return (actor->hasComponent<Components>() && ...);"}}

The sharp edge is a dependency inversion the header cannot resolve.
`concepts.hpp` only forward-declares `class Component;`, so it compiles standalone —
but `std::derived_from` needs a complete type wherever the concept is *checked*.
Any translation unit naming `ComponentType` must already have included
`scene/component/component.hpp`; `concepts.hpp` will not pull it in, and must not,
because `core/` may not depend on `scene/`.

`class Command;` is forward-declared beside it and then never referenced.
`CommandLike` is structural — it tests for `execute()`, `undo()` and
`getDescription()` on `T` — which is why `LambdaCommand` can `static_assert` against
it while the undo stack itself stores type-erased `Command` pointers.

## Sixteen bytes that never reach the GPU

The only struct in `common_types.hpp` is a mesh slice: four `uint32_t`s naming where
one actor's geometry lives inside the engine's single combined vertex/index buffer.
It is asserted under the GPU-layout macro:

{{cite ohao/core/common_types.hpp "OHAO_ASSERT_GPU_LAYOUT(MeshBufferInfo, 16);"}}

No shader reads it. Every consumer is CPU-side — `scene_upload.cpp` fills the map at
upload time and feeds `indexOffset`/`indexCount` straight to `vkCmdDrawIndexed`, the
GBuffer and CSM passes read the same map for their own draws, and `rt_build.cpp`
turns it into BLAS offsets. The 16-byte assertion is therefore discipline against a
future serialization or GPU-driven-draw path, not a shader ABI anyone can point at
today — worth knowing before someone "improves" the struct with a `std::string name`.

The contract that actually bites is units. The offsets are element counts, not
bytes, and the RT builder is the only place that converts:

{{cite ohao/gpu/vulkan/rt_build.cpp "VkDeviceSize vertexByteOffset = meshInfo.vertexOffset * sizeof(Vertex);"}}

The raster path never multiplies, because `vkCmdDrawIndexed` takes element offsets
directly. Changing either convention yields a BLAS built over the wrong triangles —
a wrong picture, not a validation error.

## What did not land

- `as_span` has zero callers anywhere, the test binary included. Its two neighbours
  `as_const_span` and `span_covers_image` are reached only from one test block —
  the one whose label still advertises `as_span`:

{{cite tests/engine/engine_tests.cpp "auto s = as_const_span<float>(v);"}}

  `ContiguousRangeOf` exists only to constrain the first two. This is the
  *range*-to-span family, distinct from the *pointer*-to-span chain in `vk_utils.hpp`
  above: different functions, the same concept guarding them, and neither reached
  from engine code.

- `GpuLayoutBytes`, the `Predicate` concept, and `layout::gpu_sizeof` have no users
  engine-wide.
- The "strong-ish aliases" lost outright. `ObjectId`, `FrameIndex`, `SampleIndex`
  and their sentinel constants have zero users:

{{cite ohao/core/common_types.hpp "using ObjectId = std::uint64_t;"}}

  The identifier type the engine actually runs on is spelled with a capital D,
  declared independently twice — once in `scene_object.hpp`, again in `actor.hpp` —
  and the mesh buffer map keys on a bare `uint64_t` regardless:

{{cite ohao/scene/scene_object.hpp "using ObjectID = uint64_t;"}}

- `core.hpp` is an umbrella over six headers with exactly one includer, the test
  binary — which is precisely what its own docstring asks for, so this one is not a
  gap.

## Contracts

- Any struct written into mapped device memory must assert `GpuPod` at its own declaration. Nothing checks at the cast — `buffer_setup.cpp` `memcpy`s onto the raw `void*` and `render_dispatch.cpp` casts it back unguarded — so a struct that skips the assertion and later gains a `std::string`, a virtual function, or mixed-access data members compiles and corrupts silently. `LightUniformBuffer` is already in that position.
- `GpuPod` does not imply the shader layout matches. Structs with a GLSL counterpart must also carry `OHAO_ASSERT_GPU_LAYOUT`: against a `layout::` constant if the owning header may include `gpu/layout_meta.hpp`, against a literal if it may not, as in `core/`. Adding a field without updating the number is a compile error; updating the number without updating the GLSL is not caught by anything.
- `PTPushConstants` sits at the 256-byte ceiling. New per-frame path-tracer state has to be packed into an existing field or moved into a descriptor-bound buffer.
- `MeshBufferInfo` offsets are in elements. Only the RT BLAS path converts them to bytes.
- Any TU that names `ComponentType` must include `scene/component/component.hpp` first; `concepts.hpp` only forward-declares `Component`, and `std::derived_from` requires the complete type.
