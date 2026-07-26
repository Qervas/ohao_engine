---
module: deferred
id: pass-base
title: RenderPassBase
standard: v2
---

## A vtable and a toolbox, not a framework

Eleven classes in `ohao/render/deferred/` derive from `RenderPassBase` — GBuffer,
CSM, DeferredLighting, Sky, SSAO, SSR, SSS, Bloom, TAA, Gizmo, and
PostProcessingPipeline (which is itself a pass that owns other passes). What they
inherit is a four-function vtable (`initialize`, `cleanup`, `execute`, `getName`,
with `onResize` and `reloadShader` defaulted) and a protected toolbox of Vulkan
boilerplate. What they do *not* inherit is lifetime management, ordering, or state
tracking. The base makes exactly one call back down the vtable, and it is a debug
print — `reloadComputeShader` naming the pass in its success log:

{{cite ohao/render/deferred/render_pass_base.cpp "reloadComputeShader: success for "}}

It allocates no command buffers and submits nothing; its one recording helper,
`transitionImage`, is `static` and writes a barrier into a buffer the caller owns
and submits.

The consequence lands on the device handle. `m_device` is a protected member of
the base, but `initialize` is pure virtual, so the base has no opportunity to fill
it in. All eleven derived `initialize` bodies open by assigning it by hand:

{{cite ohao/render/deferred/gbuffer_pass.cpp "m_device = device;"}}

Every helper below — `findMemoryType`, `loadShaderModule`, `safeDestroy`,
`createRenderTarget` — dereferences `m_device` or `m_physicalDevice` without
checking either. A pass that calls a helper before that first line does not
assert; it hands `VK_NULL_HANDLE` to a Vulkan entry point.

The same field doubles as the initialised flag:

{{cite ohao/render/deferred/render_pass_base.hpp "bool isInitialized() const noexcept { return m_device != VK_NULL_HANDLE; }"}}

So `isInitialized()` reports "`initialize()` was entered", not "`initialize()`
succeeded". `GBufferPass::initialize` sets the device, then can fail in any of
five sub-steps and return `false` with the flag reading true. No `cleanup()` in
the deferred tree clears `m_device` back to null either, so it also stays true
after teardown; each `cleanup()` instead nulls its own handles as it destroys
them and uses the device handle purely as a never-initialised early-out. Nobody is
misled by it yet: `RenderPassBase::isInitialized` has no callers anywhere in the
repo — `DeferredRenderer::reloadShaderForPass`, the one site that reports a pass as
"not initialized", tests its `unique_ptr` for null instead.

:::key
The base is a helper library bolted to a vtable. It enforces no ordering, tracks
no state, and will not stop a pass that skips a step. A subclass owes it two
assignments before any helper call, not one: `m_device`, for everything that
creates or destroys a handle, and `m_physicalDevice`, which `findMemoryType` reads
and no other line in the base does.

{{cite ohao/render/deferred/render_pass_base.cpp "vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &memProperties);"}}
:::

## One shader search root for the whole renderer

SPV location is not a per-pass concern here. It is a `static inline` string on the
base class, shared by every pass and defaulted to a relative path:

{{cite ohao/render/deferred/render_pass_base.hpp "static inline std::string s_shaderBasePath"}}

`VulkanRenderer`'s constructor probes seven candidate directories for
`core_forward.vert.spv` and, during `initialize()`, pushes the winner into the
base before the deferred renderer — and therefore any pass — is constructed:

{{cite ohao/gpu/vulkan/renderer.cpp "RenderPassBase::setShaderBasePath(m_shaderBasePath);"}}

That ordering is a real contract: the setter must run before the first pass
`initialize()`, because `loadShaderModule` reads the static at call time and each
pass loads its modules inside `initialize`. It is also a soft contract, because
`loadShaderModule` does not trust the root. It builds three candidate paths — the
configured root, then two hardcoded build-tree locations — and opens the first one
that exists:

{{cite ohao/render/deferred/render_pass_base.cpp "build/Release/bin/shaders/"}}

The fallbacks are not decoration. `VulkanRenderer::m_shaderBasePath` is a
default-constructed `std::string`, so if none of the seven probes hit, the setter
overwrites the `"bin/shaders/"` default with an empty string and the first
candidate degenerates to a bare filename relative to the working directory.

:::why
Putting the search root on the base as mutable global state buys one thing: no
pass constructor, and no `initialize` signature, has to carry a path. The rejected
alternative — threading a config object through eleven constructors — costs
plumbing but keeps the dependency visible. The price paid instead is that the root
is reachable from outside the hierarchy — the getter is a public static on the base:

{{cite ohao/render/deferred/render_pass_base.hpp "static const std::string& getShaderBasePath() { return s_shaderBasePath; }"}}

`ParticleSystem`, which does not derive from `RenderPassBase` at all, calls it
through that qualified name twice — once for its compute SPVs, once for its render
SPVs — and prepends the result to each filename:

{{cite ohao/render/particles/particle_system.cpp "basePath + "particles_particle_emit.comp.spv","}}
:::

## Two error channels in one class

The helpers disagree about how failure is reported. `createShaderModule`,
`findMemoryType`, and `loadShaderModule` throw:

{{cite ohao/render/deferred/render_pass_base.cpp "Failed to open shader file: "}}

while `createRenderTarget` and `createSampler` report failure by returning a
struct or handle that is `VK_NULL_HANDLE`. Since `createRenderTarget` internally
calls `findMemoryType`, one helper carries both channels at once: a missing memory
type throws past the caller, an image-view failure returns quietly.

The practical effect is that a pass's `bool initialize()` contract can be bypassed.
A missing SPV inside `GBufferPass::createPipeline` does not produce `false` — it
unwinds through `DeferredRenderer::initialize` and `initializeDeferredRenderer`
into the `try`/`catch` inside `VulkanRenderer::initialize`, the only one anywhere
in that chain and the only thing that converts it back into a boolean. Getting
there costs the caller its own recovery: `initializeDeferredRenderer` *returning*
`false` is handled as non-fatal and forward rendering carries on, but a throw goes
straight past that branch and fails all of `VulkanRenderer::initialize`.

{{cite ohao/gpu/vulkan/renderer.cpp "if (!initializeDeferredRenderer()) {"}}

## The helpers most passes route around

`createRenderTarget` bundles image, memory, and view creation into one call and
returns a `RenderTarget` aggregate. It has exactly two callers, both inside
`SSAOPass`:

{{cite ohao/render/deferred/ssao_pass.cpp "RenderTarget rt = createRenderTarget(VK_FORMAT_R16_SFLOAT, m_width, m_height,"}}

`GBufferPass` builds all five of its targets with the same image → memory → view
sequence written out by hand, driven by its own local `{format, usage, aspect}`
table; `DeferredLightingPass` does the same for its `R16G16B16A16_SFLOAT` HDR
output. Both then call `RenderTarget::destroy` for teardown, which frees in the
one order that is safe — view, image, memory last. So the aggregate is adopted as
a lifetime bundle far more widely than as a factory result. Its convenience
members go the other way: both factory callers immediately split the returned
struct into three loose members and test `.image == VK_NULL_HANDLE` directly,
leaving `valid()`, `explicit operator bool`, and `extent()` with no callers in the
tree.

There is a mechanical reason the compute-pipeline helper is under-used, and it is
a C++ one. `SSRPass` and `SSSPass` each declare a private zero-argument member
with the same name as the base's five-argument helper:

{{cite ohao/render/deferred/ssr_pass.hpp "[[nodiscard]] bool createComputePipeline();"}}

Name lookup stops at the first scope that contains the name, so inside those two
classes the base overload is hidden outright — an unqualified call cannot reach
it, and there is no `using RenderPassBase::createComputePipeline;` anywhere in the
tree to bring it back. `SkyPass::createSampler()` hides the base's sampler helper
the same way. Each of the three then re-implements what it hid.

Only the sampler case would rebind silently if the derived member were renamed.
The base's `createSampler` defaults both of its parameters, so a bare
`createSampler()` inside `SkyPass` would still compile and would quietly mean the
base helper instead:

{{cite ohao/render/deferred/render_pass_base.hpp "VkSamplerAddressMode addressMode = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);"}}

The base's `createComputePipeline` takes five parameters and defaults none of them,
so renaming `SSRPass::createComputePipeline` or `SSSPass::createComputePipeline`
turns their zero-argument call sites into a hard compile error instead. Two of the
three hiding cases fail loudly; one does not.

## transitionImage only speaks colour

The barrier helper is `static` and takes stages, access masks, and layouts as
parameters — but not the subresource range, which it hardcodes:

{{cite ohao/render/deferred/render_pass_base.cpp ".aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,"}}

with `levelCount` and `layerCount` both fixed at 1. That excludes most of the
deferred pipeline's interesting images: the G-Buffer's `D32_SFLOAT` depth target
needs `VK_IMAGE_ASPECT_DEPTH_BIT`, and the CSM shadow map is created with
`arrayLayers = CASCADE_COUNT` (4), so a correct barrier for it needs four layers. Its
only callers are `SSAOPass`'s four transitions, on a single-layer `R16_SFLOAT` AO
target and a 4×4 noise image — both colour, both mip- and layer-count one.

## The hot-reload wire that is not connected

`onResize` and `reloadShader` are the two virtuals with non-abstract defaults, and
the defaults fare very differently. Ten of the eleven subclasses replace the empty
`onResize`; the one that does not is `CSMPass`, whose target is sized by a
`static constexpr uint32_t SHADOW_MAP_SIZE = 2048` and so never tracks the
swapchain. The other default is a refusal:

{{cite ohao/render/deferred/render_pass_base.hpp "virtual bool reloadShader(std::string_view spvPath) { (void)spvPath; return false; }"}}

`DeferredRenderer::reloadShaderForPass` looks up a pass by name in a four-entry
table and forwards to it:

{{cite ohao/render/deferred/deferred_renderer.cpp "m_skyPass.get()"}}

The table lists GBuffer, CSM, DeferredLighting, and SkyPass. None of those four
override `reloadShader`, so every one of them returns the base's `false` and the
dispatcher prints a hot-reload-failed message. The only pass in the tree that does
override it is `SSAOPass`, which forwards to the base's `reloadComputeShader`:

{{cite ohao/render/deferred/ssao_pass.cpp "return reloadComputeShader(spvPath, m_descriptorLayout, sizeof(SSAOParams),"}}

and `SSAOPass` is not in the table — it is owned by `PostProcessingPipeline`, one
level below the passes `DeferredRenderer` holds directly, so a name lookup on
`DeferredRenderer` cannot reach it. `reloadShaderForPass` itself has no callers in
the tree. So the feature is unreachable end to end — and the machinery behind it
is not ready to be reached. `reloadComputeShader` gets two things right: it opens
the SPV by absolute path with no root prepended, and it calls `vkDeviceWaitIdle`
before touching the live pipeline. Then it destroys the live pipeline and layout
*before* the replacement exists:

{{cite ohao/render/deferred/render_pass_base.cpp "// Destroy old pipeline and layout"}}

Both later failure paths — `vkCreatePipelineLayout` and `vkCreateComputePipelines`
— return `false` with those handles already nulled and nothing restoring them, so a
driver rejection at either point leaves the pass permanently pipeline-less.

Bad SPIR-V is the one input that does *not* reach that state, and it escapes for the
wrong reason. `createShaderModule` throws when `vkCreateShaderModule` fails, and the
load sits above the two `safeDestroy` calls, so the stack unwinds with the old
pipeline still live. The damage moves rather than disappearing: the null test written
directly after the load can never fire —

{{cite ohao/render/deferred/render_pass_base.cpp "if (shaderModule == VK_NULL_HANDLE) {"}}

— and the exception leaves through a `[[nodiscard]] bool` API into a stack with
nothing to absorb it. There is no `catch` anywhere in `ohao/render/deferred/`, and
the `try` in `VulkanRenderer::initialize` that rescues the missing-SPV throw above
is not on a hot-reload stack at all.

None of this can bite today — nothing in the tree reaches the helper. All of it
would bite the moment the dispatch table is fixed, which is the wrong order to
discover it in.

## Declarations with no consumers

Three things in the header are documentation wearing code's clothes. `AttachmentInfo`
has no references outside its own definition. The `GBufferAttachment` enum names the
five G-Buffer slots and their channel meanings, but `GBufferPass` indexes its array
with literal 0–4 in every accessor and never mentions the enum — and the two have
already drifted. The enum still documents slot 1 as three channels of normal plus
roughness in alpha:

{{cite ohao/render/deferred/render_pass_base.hpp "Normal = 1,      // RGB: Encoded normal, A: Roughness"}}

The shader packs an octahedral pair into `rg` — which the comment still describes
correctly — and then roughness into `b` and emissive luminance into `a`. Two of the
four channels have moved out from under their documentation, and nothing failed to
compile when they did:

{{cite shaders/core/gbuffer.frag "outGBuffer1 = vec4(encodedNormal, roughness, emissiveLuminance);"}}

And the `RenderPassLike` concept restates the virtual interface:

{{cite ohao/render/deferred/render_pass_base.hpp "concept RenderPassLike = requires(T& t, VkDevice d, VkPhysicalDevice pd,"}}

Nothing is constrained by it — there is no `template<RenderPassLike T>` in the
tree — which the header's own comment concedes when it calls the concept a way to
document the surface while keeping dispatch virtual.

`CascadeData` is the header's other live struct, alongside the `RenderTarget`
above. `CSMPass` `memcpy`s it into a UBO it created at exactly
`sizeof(CascadeData)`, and `DeferredLightingPass` binds that buffer at binding 12,
substituting a 512-byte dummy whenever the cascade buffer is not yet set. The 512
is not sized around `CascadeData`: the same dummy also backs the fallback SSBO at
binding 5, and 512 was chosen to cover both uses at once.

{{cite ohao/render/deferred/deferred_lighting_pass.cpp "CascadeData is 288 bytes, so 512 is sufficient for both uses."}}

Its layout is 4 × `mat4` + `vec4` + 3 floats + `uint32` = 288 bytes. The cascade
count appears twice in it, once as a fixed array bound (`viewProj[4]`) and once as
a runtime `cascadeCount` field, so the two can only ever agree at 4;
`CSMPass::CASCADE_COUNT` is likewise a
`static constexpr uint32_t` of 4. There is no path in the struct that grows.
