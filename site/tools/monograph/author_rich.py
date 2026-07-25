"""Author rich markdown for every design unit by probing sources + domain knowledge.

Run: python3 -c "from monograph.author_rich import author_all; author_all()"
  (from site/tools with PYTHONPATH) or via generate after import.
"""
from __future__ import annotations

from pathlib import Path

from .code_probe import probe
from .paths import CONTENT, ROOT
from .tree_data import TREE

# Domain knowledge keyed by "module/id" — overrides / seeds What/How/Why
KNOWLEDGE: dict[str, dict] = {
    "architecture/request-path": {
        "what": [
            "The request path is the single mental model for every OHAO binary: a thin driver builds a Scene, hands it to VulkanRenderer, chooses a RenderMode, and either presents a swapchain or reads pixels.",
            "Examples (interactive, cornell_box, turntable, model_viewer) share this path; they differ only in scene construction and CLI flags.",
        ],
        "how": [
            "Parse CLI (model path, HDR env, --mode, --denoise, spp).",
            "Construct Scene; loaders fill Mesh/Material/Light components without touching Vulkan.",
            "VulkanRenderer(w,h).initialize() creates instance, device, queues, frame ring, bindless pool.",
            "setScene uploads geometry and materials, then buildBLASTLAS when RT is needed.",
            "setRenderMode selects Forward, Deferred, RTRealtime, or RTOffline; ensureRTRenderer lazily constructs PathTracer.",
            "render() records and submits; getPixels copies staging after fence wait for offline/goldens.",
        ],
        "why": "One API surface means tests, demos, and tools never fork renderer semantics. Lazy RT avoids dual 4K path-tracer OOM when you only needed deferred.",
        "contracts": [
            "Scene must not include vulkan.h",
            "initialize before setScene",
            "wait fence before staging readback",
            "TLAS instance order matches material rows after upload",
        ],
    },
    "architecture/invariants": {
        "what": [
            "Cross-module invariants are product rules that break pixels silently if violated — not style preferences.",
            "They couple scene upload, hit shaders, denoisers, and the frame ring into one coherent system.",
        ],
        "how": [
            "Documented in STATUS and chapter contracts; enforced by layout asserts, golden images, and careful descriptor writes.",
            "Upload path packs materials in the same order BLAS instances are built.",
            "NRD packing happens in raygen via nrd_frontend.glsl before REBLUR sees the image.",
        ],
        "why": "Silent pink, black frames, and OOM are worse than a loud assert. Invariants make failure modes diagnosable.",
        "contracts": [
            "TLAS instance order == material row order",
            "Bindless 0xFFFFFFFF = no texture",
            "Frame ring: wait fence before staging readback",
            "NRD: YCoCg + norm hit-dist (not linear RGB)",
            "Lazy PathTracer profiles (no dual 4K OOM)",
            "Same PBR language for deferred and PT",
        ],
    },
    "architecture/module-map": {
        "what": [
            "Ownership map: which directory may depend on which. Scene never sees Vulkan; GPU owns devices and uploads; render/* owns techniques; shaders mirror GPU bindings.",
        ],
        "how": [
            "ohao/core — types, EventBus, Result (no Vulkan)",
            "ohao/scene — actors, components, loaders (no Vulkan)",
            "ohao/gpu — device, bindless, upload, dispatch",
            "ohao/render/* — deferred, RT, denoise, graph, camera",
            "ohao/physics, ohao/audio — facades with pluggable backends",
            "shaders/ — SPIR-V sources matching set/binding contracts",
        ],
        "why": "Forbidden edges prevent the classic 'everything includes renderer.hpp' collapse that freezes refactors.",
        "contracts": [
            "scene → core only (plus glm)",
            "gpu may include scene types for upload but not inverse",
            "Public monograph omits non-product research trees",
        ],
    },
    "core/types-concepts": {
        "what": [
            "Shared C++20 types and concepts used across modules. GpuPod constrains GPU-shared structs to trivially copyable layouts so CPU packs match GLSL std430/std140.",
        ],
        "how": [
            "common_types.hpp holds enums/aliases (RenderMode-adjacent flags, mesh ids).",
            "concepts.hpp defines GpuPod and related requires-clauses used by static_assert on camera UBO, SimpleVertex, GPULight.",
            "core.hpp is the umbrella include for consumers that only need core.",
        ],
        "why": "Without layout discipline, deferred and path tracer silently disagree on material rows — pink metals and NaN lights.",
        "contracts": ["GpuPod types are trivially copyable", "No Vulkan types in core"],
    },
    "core/event-bus": {
        "what": [
            "Thread-safe publish/subscribe bus for decoupling systems (physics contacts, UI, tools) without hard module links.",
        ],
        "how": [
            "subscribe(eventType, handler) → SubscriptionId; unsubscribe by id.",
            "subscribeTyped<T> wraps any_cast and skips bad casts.",
            "ScopedSubscription is move-only RAII unsubscribe.",
            "EventBus::instance() is a process-wide convenience; inject a bus in tests.",
        ],
        "why": "Physics and audio should not #include each other; events carry typed payloads across boundaries.",
        "contracts": ["string_view keys at API; owned strings in map", "ScopedSubscription moves only"],
    },
    "core/command-result": {
        "what": [
            "Command history supports undo/redo editor-style operations; Result<T,E> is the fallible return type for loaders and init paths that should not throw on expected failures.",
        ],
        "how": [
            "Command interface execute/undo; history stack in command.cpp.",
            "Result exposes ok/err accessors and monadic helpers without exceptions on the hot path.",
        ],
        "why": "Load failures and missing files are expected; exceptions are for bugs. Undo is optional tooling, not render-critical.",
    },
    "core/console-widget": {
        "what": [
            "Optional in-engine console / log surface for debug tooling. Not on the hot render path.",
        ],
        "how": [
            "ConsoleWidget buffers log lines and presents a simple UI surface for tools/examples.",
            "Stays in core so gpu/ remains free of UI dependencies.",
        ],
        "why": "Debuggability without dragging ImGui or similar into every library target.",
    },
    "scene/actors": {
        "what": [
            "Actor is the scene graph node: identity, name, parent/children, lifecycle, and a bag of components. Transform is always present.",
        ],
        "how": [
            "construct → addComponent → initialize → start → update → destroy",
            "Hierarchy walks update world transforms before mesh/physics consumers read them.",
            "Actor::Ptr is shared ownership from Scene maps.",
        ],
        "why": "Composition over deep inheritance: renderers iterate MeshComponents, not class hierarchies.",
        "contracts": ["Transform always present on Actor", "Scene owns actor maps by id and name"],
    },
    "scene/components": {
        "what": [
            "Components are pure data + light behavior: Mesh, Material, Light, Transform, Physics. Factories and packs spawn common kits.",
        ],
        "how": [
            "component.hpp base; specialized headers for mesh/material/light/transform.",
            "component_factory / component_pack create primitives with sensible defaults for demos.",
        ],
        "why": "Upload and physics query components by type; no virtual draw() on actors.",
    },
    "scene/loaders": {
        "what": [
            "Asset loaders parse GLTF (tinygltf), FBX (Assimp), OBJ, and procedural primitives into Mesh/Material components — never Vulkan objects.",
        ],
        "how": [
            "Detect format → parse meshes/materials/textures → up-axis fix → fill components.",
            "model_loader.hpp is the facade; model_gltf.cpp / model_fbx.cpp / model.cpp specialize.",
            "primitive_mesh_generator builds cornell-style boxes and spheres without files.",
        ],
        "why": "Keeping loaders Vulkan-free means offline tools and tests can load scenes without a GPU.",
        "contracts": ["No VkDevice in scene/asset", "Textures referenced by path/index for later bindless upload"],
    },
    "scene/scene-api": {
        "what": [
            "Scene owns actors, registries, optional PhysicsWorld, tags, and descriptor metadata for serialization hooks.",
        ],
        "how": [
            "createActor / createActorWithComponents / addActor / removeActor",
            "find by id, name, or tag; forEach helpers for non-hot iteration",
            "update(dt) walks actors then steps physics if present",
        ],
        "why": "One ownership root prevents dangling components and double physics worlds.",
    },
    "scene/transform": {
        "what": [
            "TRS transform math: local matrix from translation/rotation/scale; world matrix via parent chain. Upload and TLAS read world only.",
        ],
        "how": [
            "Dirty flags recompute world matrices on demand during update or upload.",
            "Used by MeshComponent world poses and physics sync.",
        ],
        "why": "TLAS instances need stable world matrices; recomputing every frame without dirty flags is free performance loss.",
    },
    "gpu/renderer-facade": {
        "what": [
            "VulkanRenderer is the public facade: initialize, setScene, setRenderMode, ensureRTRenderer, setDenoiseMode, render, getPixels/present.",
            "RenderMode: Forward | Deferred | RTRealtime | RTOffline with isRTRenderMode helpers.",
        ],
        "how": [
            "renderer.hpp is the API; renderer.cpp + renderer_impl.hpp split implementation weight.",
            "PathTracer is lazy so deferred-only sessions skip dual 4K RT allocations.",
            "CameraUniformBuffer and SimpleVertex are GpuPod-asserted.",
        ],
        "why": "One facade for every example and golden test; modes share upload and AS.",
        "contracts": [
            "ensureRTRenderer before RT render",
            "setDenoiseMode after RT init for DLSS/NRD resource needs",
        ],
    },
    "gpu/init": {
        "what": [
            "Device init chain: instance → physical device → logical device with RT/descriptor indexing/BDA features.",
        ],
        "how": [
            "Vulkan 1.3 instance; prefer discrete GPU + graphics queue.",
            "Enable accelerationStructure, rayTracingPipeline, bufferDeviceAddress, descriptorIndexing.",
            "Optional DLSS device extensions when built with NGX.",
            "Command pool, sync objects, frame resources allocated after device.",
        ],
        "why": "Missing RT features fail at pipeline create, not first draw — early, loud, fixable.",
    },
    "gpu/bindless": {
        "what": [
            "BindlessTextureManager: variable-count sampled image array with UPDATE_AFTER_BIND so materials store integer indices instead of descriptor sets.",
        ],
        "how": [
            "Allocate large array binding; write descriptor for each uploaded texture.",
            "Shaders sample via nonuniformEXT(index); 0xFFFFFFFF means unbound.",
        ],
        "why": "Per-material descriptor thrash kills multi-texture GLB load times and bind cost.",
        "contracts": ["Never sample index 0xFFFFFFFF", "Update-after-bind set flags must match layout"],
    },
    "gpu/buffers-alloc": {
        "what": [
            "GpuAllocator and buffer setup helpers for device-local buffers, staging, and images used by the graph and path tracer.",
        ],
        "how": [
            "Staging for CPU→GPU mesh/material uploads; ring-friendly lifetimes with frame resources.",
            "vk_utils.hpp: create helpers, debug names, barrier one-liners.",
        ],
        "why": "Central allocation avoids ad-hoc vkAllocateMemory copies and leaks on resize.",
    },
    "gpu/scene-upload": {
        "what": [
            "Scene upload walks MeshComponents into VB/IB maps, packs material rows, dual-packs lights for deferred vs path tracer, and prepares env handles.",
        ],
        "how": [
            "scene_upload.cpp builds interleaved geometry and MeshBufferInfo map.",
            "light_upload.cpp packs LightComponents into GPULight SSBO and deferred LightData.",
            "No Vulkan types remain in scene/ — this boundary owns the conversion.",
        ],
        "why": "Upload is the only place scene becomes GPU state; keeping it explicit makes TLAS/material lockstep enforceable.",
    },
    "gpu/rt-build": {
        "what": [
            "BLAS per mesh, TLAS over instances with world transforms. Instance order must match material table order for hit shaders.",
        ],
        "how": [
            "rt_build.cpp drives RTAccelerationStructure build/update.",
            "Rebuild on topology change; refit/update when only transforms dirty.",
        ],
        "why": "Wrong instance→material mapping is the classic 'random materials on meshes' bug.",
        "contracts": ["instanceId indexes material rows", "world matrix from Transform only"],
    },
    "gpu/dispatch": {
        "what": [
            "Render dispatch branches on RenderMode: deferred graph vs PathTracer::render, with staging readback on the frame ring.",
        ],
        "how": [
            "Wait fence on ring slot → record → submit → advance frame.",
            "getPixels memcpy only after wait — never overwrite in-flight staging.",
        ],
        "why": "MAX_FRAMES_IN_FLIGHT=3 hides latency; violating fence order races readback.",
    },
    "gpu/layout-meta": {
        "what": [
            "layout_meta.hpp is the single source of truth for GPU struct sizes/offsets shared with GLSL. Material and MaterialInstance are CPU authoring types that pack into SSBO rows.",
        ],
        "how": [
            "OHAO_ASSERT_GPU_LAYOUT fails the build if C++ and shader disagree.",
            "MaterialInstance holds texture indices and factors before pack.",
        ],
        "why": "Silent layout drift is undebuggable; assert at compile time.",
    },
    "gpu/pipeline-fb": {
        "what": [
            "Legacy forward pipeline and offscreen framebuffer helpers for simple demos and regression paths.",
        ],
        "how": [
            "pipeline.cpp creates forward graphics pipelines; framebuffer.cpp allocates color/depth for present/readback.",
        ],
        "why": "Forward remains for smoke tests; deferred is the production raster path.",
    },
    "materials/pbr-model": {
        "what": [
            "Metallic-roughness PBR: baseColor, metallic, roughness map to diffuse lobe + specular F0. ORM packing follows glTF (occlusion/roughness/metal).",
        ],
        "how": [
            "pbr_unpack.glsl expands packed rows/textures into shading parameters.",
            "Dielectric F0 ≈ 0.04; metals use baseColor as F0 with zero diffuse.",
            "Roughness floor kills zero-width specular fireflies in path tracing.",
        ],
        "why": "One material language for deferred and PT means artist assets look consistent across modes.",
        "math": [
            "F_{0,\\mathrm{dielectric}} \\approx 0.04",
            "F_0 = \\mathrm{lerp}(0.04,\\, c_{\\mathrm{base}},\\, m)",
        ],
    },
    "materials/ggx": {
        "what": [
            "GGX/Trowbridge-Reitz microfacet BRDF with Schlick Fresnel and Smith correlated geometry — shared by deferred lighting and path-tracer lobes.",
        ],
        "how": [
            "brdf_ggx.glsl: D, F, G terms; brdf_common helpers; ggx_aniso for anisotropic roughness.",
            "material_sampling uses VNDF/cosine for importance sampling in PT.",
        ],
        "why": "Two BRDFs = two lookdevs. One GGX is a product decision.",
        "math": [
            "f_r = \\frac{D F G}{4(n\\cdot w_i)(n\\cdot w_o)}",
        ],
    },
    "materials/pack": {
        "what": [
            "GPU material pack: tight vec4 rows; texture indices bit-cast into floats for SSBO friendliness.",
        ],
        "how": [
            "layout_meta defines pack; upload writes rows in instance order.",
            "Shaders unpack with floatBitsToUint style casts where needed.",
        ],
        "why": "std430-friendly packs beat pointer-chasing material structs on GPU.",
        "contracts": ["0xFFFFFFFF = missing texture", "row order == TLAS instance order"],
    },
    "materials/advanced": {
        "what": [
            "Advanced material includes: multi-lobe helpers, sampling routines, material type constants used by hit and raygen.",
        ],
        "how": [
            "material_sampling.glsl importance samples GGX VNDF / cosine hemisphere.",
            "material_types.glsl shares enum-like constants.",
            "advanced_brdf.glsl extends beyond simple metal-rough.",
        ],
        "why": "Keep lobe math out of the giant raygen file so offline/realtime variants share code.",
    },
    "materials/lights": {
        "what": [
            "Dual light packing: GPULight 64-byte SSBO for path tracer (sphere/dir/spot/area) vs compact deferred LightData UBO.",
        ],
        "how": [
            "gpu_light.hpp defines GPULight; light_upload.cpp packs from LightComponents.",
            "Sphere lights store radius in dirAndParam.w; NEE must use distance for shadow Tmax, not radius.",
        ],
        "why": "PT needs analytic area sampling; deferred needs a tight multi-light loop — one component model, two packs.",
        "contracts": [
            "NEE shadow ray uses lightDist for Tmax, not light radius",
            "lightCount heads the SSBO",
        ],
    },
    "deferred/orchestrator": {
        "what": [
            "DeferredRenderer owns the AAA raster stack: GBuffer, CSM, SSAO, lighting, sky, post, gizmo, optional RT shadow/GI inject.",
        ],
        "how": [
            "initialize(device, phys); setScene; setCameraData; set lights/geometry buffers.",
            "render(cmd, frameIndex) runs passes; RenderGraph inserts barriers while passes own VkRenderPass objects.",
            "onResize reallocates targets.",
        ],
        "why": "Orchestrator pattern keeps each pass small; hybrid RT plugs in without rewriting lighting.",
        "workflow": ["CSM", "GBuffer", "SSAO", "Lighting (+ optional RT)", "Sky", "Bloom/TAA/Tonemap", "Gizmo"],
    },
    "deferred/gbuffer": {
        "what": [
            "GBuffer pass rasterizes scene into MRT: albedo, normals, material params, motion vectors, depth — fuel for deferred lighting and denoisers.",
        ],
        "how": [
            "gbuffer.vert: world pos, normal, tangent, dual UV, current+prev clip for velocity.",
            "gbuffer.frag: bindless textures, material packing, nonuniformEXT indices.",
        ],
        "why": "Decouple geometry bandwidth from light count; motion vectors unlock TAA and DLSS.",
    },
    "deferred/csm": {
        "what": [
            "Cascaded shadow maps into a depth array; splits from the camera frustum. Uses unjittered camera when TAA is active for stable cascades.",
        ],
        "how": [
            "csm_pass records depth from light's view per cascade; shadow_csm.glsl samples with PCF.",
        ],
        "why": "Single shadow map cannot cover large outdoor views without swimming or acne.",
    },
    "deferred/lighting": {
        "what": [
            "Fullscreen deferred lighting reconstructs world position from depth, evaluates GGX + IBL + CSM, multiplies SSAO, optionally samples RT shadow/GI textures.",
        ],
        "how": [
            "deferred_lighting.frag + deferred_lighting_pass.hpp; ibl.glsl for prefiltered env.",
        ],
        "why": "All shading in one pass over screen-sized buffers — light count scales better than forward.",
    },
    "deferred/ssao": {
        "what": [
            "Screen-space ambient occlusion: hemisphere samples in view space, blurred AO bound into lighting.",
        ],
        "how": ["ssao_pass + ssao.comp compute path."],
        "why": "Cheap contact darkening without full GI — complements hybrid RT and IBL.",
    },
    "deferred/ssr-sss": {
        "what": [
            "Experimental SSR (HiZ ray march) and SSS blur for skin-like materials — quality gated on the deferred hub.",
        ],
        "how": ["ssr.comp / sss_blur.comp with pass wrappers."],
        "why": "Optional quality; not required for cornell golden path.",
    },
    "deferred/sky": {
        "what": [
            "Sky pass composites HDR environment/atmosphere where GBuffer depth is sky.",
        ],
        "how": ["sky_pass + sky.frag after lighting."],
        "why": "Separate sky avoids lighting the background as geometry.",
    },
    "deferred/post": {
        "what": [
            "Post stack: bloom threshold/down/up, TAA resolve, tonemap (ACES family).",
        ],
        "how": [
            "PostProcessingPipeline owns order; bloom_pass and taa_pass share HDR/history targets.",
        ],
        "why": "Tone map last so bloom sees HDR peaks; TAA before display reduces shimmer.",
        "workflow": ["bloom threshold", "downsample", "upsample", "TAA", "tonemap"],
    },
    "deferred/gizmo": {
        "what": ["Debug/editor overlay meshes after the main stack."],
        "how": ["gizmo_pass + gizmo_meshes + overlay shaders."],
        "why": "Selection and force viz without polluting GBuffer.",
    },
    "deferred/pass-base": {
        "what": ["RenderPassBase: shared init/cleanup/shader path helpers."],
        "how": ["Each pass only declares attachments and record()."],
        "why": "DRY pipeline creation across a dozen passes.",
    },
    "path-tracer/host": {
        "what": [
            "PathTracer host lifecycle: images (accum RGBA32F, output, AOVs), RT pipeline, SBT, descriptors, optional denoise backends.",
            "Split TUs: pipeline, descriptors, images, render. ODR: NRD/DLSS members unconditional in header.",
        ],
        "how": [
            "init(device, phys, w, h) → allocate images → create pipeline/SBT → write descriptors",
            "setMaterialAlbedos / lights / env CDF before first render",
            "render(cmd, accel, view, proj, …); resetAccumulation on camera move",
            "resize reallocates images and rewrites descriptors",
        ],
        "why": "Full-frame GI without a light bake; progressive samples until denoise or offline converge.",
        "contracts": [
            "Header layout identical with/without OHAO_NRD for ODR",
            "Do not construct dual 4K PathTracers casually",
        ],
    },
    "path-tracer/profiles": {
        "what": [
            "RTRenderSettings profiles: kRealtime vs kOffline — spp, bounces, sampler type, denoise defaults.",
        ],
        "how": [
            "rt_settings.hpp / rt_meta.hpp traits; rt_profile_renderer bridges mode dispatch.",
        ],
        "why": "Interactive and reference share host code; only knobs change.",
    },
    "path-tracer/as": {
        "what": ["RTAccelerationStructure: BLAS/TLAS build, rebuild, update."],
        "how": ["rt_acceleration_structure.cpp used by both hybrid and full PT."],
        "why": "One AS owner avoids divergent instance layouts.",
    },
    "path-tracer/raygen": {
        "what": [
            "Raygen integrator family: pt_raygen.rgen (brute-force reference), realtime/offline variants with NEE, dual-lobe BRDF, MIS, AOV packing for denoisers.",
        ],
        "how": [
            "Primary ray from camera UBO; closest-hit payload carries throughput.",
            "NEE: sample light (sphere solid angle), shadow ray with Tmax = lightDist (not radius).",
            "BRDF sample next direction; Russian roulette / max bounce.",
            "Write beauty + AOVs; NRD path packs YCoCg + norm hit-dist via nrd_frontend.glsl.",
        ],
        "why": "One integrator family; profile swaps spp/sampler without forking the host API. Hub NEE walk anchors L305+ line pedagogy.",
        "workflow": [
            "Camera primary ray",
            "Closest-hit payload",
            "NEE + shadow ray",
            "BRDF sample bounce",
            "RR / max depth",
            "Accum + AOV writes",
        ],
        "math": [
            "L_o = L_e + \\int_{\\mathcal{H}} f_r L_i |n\\cdot w_i|\\,dw_i",
            "\\hat{L}_{\\mathrm{NEE}} = \\frac{f_r L_e |n\\cdot w_i|}{p_{\\mathrm{light}}} V",
        ],
        "contracts": [
            "Shadow Tmax uses distance to light sample, not sphere radius",
            "NRD inputs are YCoCg+hitDist, not linear RGB",
        ],
    },
    "path-tracer/hit-miss": {
        "what": [
            "closesthit: barycentrics → material → PBR payload; anyhit: alpha cutout; miss: environment contribution.",
        ],
        "how": [
            "pt_closesthit.rchit, pt_anyhit.rahit, pt_miss.rmiss share material unpack with raygen.",
        ],
        "why": "Split shader stages keep anyhit cheap and miss free of material state.",
    },
    "path-tracer/bindings": {
        "what": [
            "Descriptor set 0 map bindings 0–35: TLAS, accum, materials, lights, env CDF, AOVs, ReSTIR 29–34, DLSS hit-dist 35.",
        ],
        "how": [
            "path_tracer_descriptors.cpp writes all slots; GLSL layout(set=0,binding=N) must match exactly.",
        ],
        "why": "Wrong binding = silent black. Plate SVG documents the shipped map.",
        "contracts": [
            "0 TLAS, 1 accum, 2 output, 3 materials, 11 lights",
            "29–34 ReSTIR, 35 DLSS hit-dist when enabled",
        ],
    },
    "path-tracer/restir": {
        "what": [
            "ReSTIR GI reservoirs as set-0 storage images for realtime light sample reuse.",
        ],
        "how": [
            "Ping-pong planes on bindings 29–34; offline may skip and use Sobol spp instead.",
        ],
        "why": "Realtime GI needs temporal reuse; reservoirs keep it inside the same descriptor set as PT.",
    },
    "sampling/sampler-api": {
        "what": ["Unified next1D/next2D sampler API; backend selected by profile specialization."],
        "how": ["sampler_api.glsl dispatches; sampler_types.hpp mirrors host enum."],
        "why": "Integrator code should not branch on RNG brand at every sample site.",
    },
    "sampling/sobol": {
        "what": ["Sobol + Owen scramble for offline low-discrepancy sampling."],
        "how": ["Host tables + sampler_sobol.glsl; owen_scramble.cpp."],
        "why": "Lower discrepancy → faster offline convergence than white noise.",
    },
    "sampling/pcg": {
        "what": ["PCG hash RNG for interactive path tracing."],
        "how": ["sampler_pcg.glsl — cheap per-pixel streams."],
        "why": "Interactive budgets cannot afford full Sobol table traffic every frame.",
    },
    "sampling/mis": {
        "what": ["Multiple importance sampling balance/power heuristics for NEE vs BRDF samples."],
        "how": ["mis.glsl; weights use pdf_light and pdf_bsdf in the same measure."],
        "why": "Either strategy alone is high variance on glossy + small lights.",
        "math": [
            "w_{\\mathrm{bal}}(p_a,p_b)=\\frac{p_a}{p_a+p_b}",
        ],
        "contracts": ["pdf units must match — solid angle vs area mix biases results"],
    },
    "sampling/env-cdf": {
        "what": ["Environment map CDF for importance-sampled sky lighting."],
        "how": [
            "CPU builds marginal + conditional CDFs from HDR luminance (env_cdf.cpp).",
            "GPU sampleEnvMap / pdfEnvMap in env_sampling.glsl.",
        ],
        "why": "Uniform env sampling wastes paths on dark texels.",
    },
    "hybrid/shadow-technique": {
        "what": ["RT shadow technique: visibility rays from GBuffer toward lights into a mask for deferred lighting."],
        "how": ["rt_shadow_technique + rt_shadow.rgen/rmiss/rahit."],
        "why": "Soft RT shadows without full path tracing cost.",
    },
    "hybrid/gi-technique": {
        "what": ["One-bounce RT GI inject into deferred lighting using GBuffer normals and material albedos."],
        "how": ["rt_gi_technique + rt_gi.rgen/rchit/rmiss; shares TLAS with shadows."],
        "why": "Middle ground between IBL-only and full PT.",
    },
    "hybrid/visibility": {
        "what": ["Shared RenderTechnique pattern and visibility helpers for hybrid passes."],
        "how": ["init / resize / render / cleanup lifecycle."],
        "why": "Uniform lifecycle keeps DeferredRenderer orchestration simple.",
    },
    "denoise/types": {
        "what": ["DenoiseMode enum + traits declaring which AOV guides each backend needs."],
        "how": ["denoise_types.hpp; CLI --denoise=dlssrr|nrd|oidn|atrous|none."],
        "why": "Traits prevent calling DLSS without hit-dist or NRD without packing.",
    },
    "denoise/oidn": {
        "what": ["Intel Open Image Denoise for offline/reference quality."],
        "how": ["oidn_denoise after enough spp; guided by albedo/normal when available."],
        "why": "Best quality when latency is irrelevant.",
    },
    "denoise/nrd": {
        "what": [
            "NRD REBLUR realtime denoise: pack radiance as YCoCg + normalized hit-distance, dispatch, compose unpack, optional cinematic post.",
        ],
        "how": [
            "nrd_frontend.glsl pack in raygen; NrdDenoiser dispatch; nrd_compose.comp unpack; NrdCinematicPost optional.",
        ],
        "why": "Temporal denoise reuses AOVs already required for hybrid/PT. Wrong pack = magenta classic bug.",
        "contracts": [
            "IN_DIFF/SPEC are YCoCg+hitDist not linear RGB",
            "Compose must unpack before display",
        ],
        "workflow": ["pack AOVs", "NRD dispatch", "compose unpack", "optional cinematic"],
    },
    "denoise/dlss": {
        "what": ["NVIDIA DLSS Ray Reconstruction via NGX — guides include color, depth, motion, normal-roughness, specular hit-distance (binding 35)."],
        "how": [
            "dlss_rr.hpp/cpp; needs LD_LIBRARY_PATH to DLSS .so; interactive --denoise=dlssrr.",
            "dlss_tonemap.comp prepares display path.",
        ],
        "why": "Highest interactive quality on supported NVIDIA hardware.",
    },
    "denoise/atrous": {
        "what": ["In-engine edge-aware à-trous / SVGF-style denoise with zero external dependency."],
        "how": ["atrous_denoise + denoise_atrous.comp / rt_svgf_*.comp."],
        "why": "Fallback when NRD/DLSS unavailable.",
    },
    "denoise/cinematic": {
        "what": ["Cinematic RT post: bloom extract/blur, DoF, composite after denoise."],
        "how": ["cinematic_*.comp chain after NRD stability."],
        "why": "Bloom/DoF after denoise so temporal filters are not fighting post effects.",
    },
    "shaders/layout": {
        "what": ["Shader tree layout: core, rt, postprocess, compute, includes, shadow, particles. SPIR-V via CMake/compile script."],
        "how": ["PathTracer searches multiple relative SPIR-V paths; include root is shaders/."],
        "why": "Predictable layout beats hunting random .spv copies.",
    },
    "shaders/includes-common": {
        "what": ["Shared math, color, encoding, reconstruction, constants, noise."],
        "how": ["Used by both raster and RT; encoding holds octahedral pack conventions."],
        "why": "One luminance/normalize definition everywhere.",
    },
    "shaders/includes-lighting": {
        "what": ["IBL, attenuation, light types, phase, blinn_phong helpers."],
        "how": ["ibl.glsl samples prefiltered specular + irradiance + BRDF LUT."],
        "why": "Deferred and hybrid share light math with CPU packs.",
    },
    "shaders/includes-shadow": {
        "what": ["CSM/PCF sampling includes plus depth write shaders."],
        "how": ["shadow_csm.glsl, shadow_pcf.glsl, shadow_depth.*, shadow_csm.vert."],
        "why": "Sampling math must match cascade construction on the host.",
    },
    "shaders/includes-fx": {
        "what": ["Gerstner water and cloud density procedural includes."],
        "how": ["Optional; not on cornell golden path."],
        "why": "Keep FX math out of lighting shaders until needed.",
    },
    "shaders/postprocess-chain": {
        "what": ["Fullscreen post chain: bloom, TAA, tonemap, sky, SSR, SSS shaders."],
        "how": ["fullscreen.vert feeds fragment posts; compute for SSR/SSS."],
        "why": "Pass ownership in deferred post pipeline; shaders stay dumb.",
        "workflow": ["threshold", "downsample", "upsample", "TAA", "tonemap"],
    },
    "shaders/rt-includes": {
        "what": ["RT-specific includes: pbr_unpack, rt_masks, nrd_frontend."],
        "how": ["Included by raygen/hit; nrd_frontend is the YCoCg pack source of truth."],
        "why": "Wrong NRD pack is a one-line bug with a magenta symptom — keep pack code centralized.",
    },
    "shaders/compute": {
        "what": ["Compute suite: IBL bake, HiZ, light cull, skinning, composite, DoF, SSAO, denoise helpers."],
        "how": ["Dispatched from IBL processor, deferred passes, or PT post."],
        "why": "GPU prep work that is not full-screen fragment shading.",
    },
    "shaders/core-gbuffer-lighting": {
        "what": ["Core raster pair: gbuffer.vert/frag MRT contract and deferred_lighting.frag."],
        "how": ["Bindless nonuniformEXT; lighting reconstructs position from depth."],
        "why": "These two shaders are the deferred product surface.",
    },
    "shaders/forward": {
        "what": ["Legacy forward.vert/frag with limited lights."],
        "how": ["Used by RenderMode::Forward smoke paths."],
        "why": "Simple path for debugging without GBuffer.",
    },
    "shaders/particles-sh": {
        "what": ["Particle emit/update compute + render shaders."],
        "how": ["Owned by particle_system pass."],
        "why": "GPU particles stay off the deferred geometry path.",
    },
    "shaders/overlay-gizmo": {
        "what": ["Overlay gizmo vert/frag for debug draws."],
        "how": ["Depth-aware lines/solids after main stack."],
        "why": "Editor visibility without GBuffer pollution.",
    },
    "shaders/rt-shaders": {
        "what": [
            "Full RT program family: path tracer stages, hybrid shadow/GI, NRD compose, SVGF/à-trous, cinematic post.",
        ],
        "how": [
            "pt_* for full PT; rt_shadow_* / rt_gi_* for hybrid; compute post for denoise/cinematic.",
        ],
        "why": "One SPIR-V family documented together so binding/SBT changes stay coordinated.",
    },
    "shaders/disabled": {
        "what": ["shaders/_disabled archives retired experiments — not in the active build."],
        "how": ["Do not reference from CMake targets."],
        "why": "Keep history without shipping broken paths.",
    },
    "camera/camera": {
        "what": ["Camera view/projection and FPS/orbit controls used by examples."],
        "how": ["camera.hpp/cpp feed CameraUniformBuffer each frame."],
        "why": "Examples share one camera model so jitter/TAA conventions stay consistent.",
    },
    "camera/scene-framer": {
        "what": ["Auto-frame scene bounds for turntable/model viewer."],
        "how": ["scene_framer computes distance/FOV from AABB."],
        "why": "Demos should open on a well-framed model without manual tuning.",
    },
    "graph/render-graph": {
        "what": ["RenderGraph declares passes, resources, barriers; compiles execution order."],
        "how": [
            "Deferred uses graph for CSM→GBuffer→SSAO→Lighting barriers while passes still own VkRenderPass objects.",
        ],
        "why": "Explicit barriers beat tribal knowledge about image layouts.",
    },
    "graph/frame-resources": {
        "what": ["Per-frame ring: fences, staging, UBOs with MAX_FRAMES_IN_FLIGHT=3."],
        "how": ["frame_resources.hpp/cpp; dispatch waits before reuse."],
        "why": "CPU/GPU overlap without resource hazard.",
    },
    "graph/async-compute": {
        "what": ["Optional async compute queue path."],
        "how": ["async_compute_queue.hpp for overlapping work."],
        "why": "Headroom for culling/particles off the graphics queue.",
    },
    "graph/ibl-processor": {
        "what": ["IBL bake: equirect→cubemap, prefilter, BRDF LUT."],
        "how": ["ibl_processor dispatches compute shaders when env changes."],
        "why": "Runtime IBL without offline content pipeline dependency.",
    },
    "graph/particles": {
        "what": ["Particle system orchestration (CPU/GPU)."],
        "how": ["particle_system ties compute emit/update to draw."],
        "why": "Effects without mesh component spam.",
    },
    "graph/picking": {
        "what": ["Ray pick helpers for selection."],
        "how": ["picking_system + ray.hpp."],
        "why": "Tools need object pick independent of render mode.",
    },
    "graph/culling": {
        "what": ["CPU/GPU cull helpers (HiZ, light cull compute)."],
        "how": ["culling.hpp + gpu_cull.comp / hiz_generate.comp."],
        "why": "Scale deferred to denser scenes.",
    },
    "physics/backend": {
        "what": ["IPhysicsBackend plugin: factory picks Jolt or null; helpers convert shapes without leaking Jolt into scene headers."],
        "how": ["backend_factory.cpp; jolt_backend.*; jolt_helpers.hpp."],
        "why": "Scene/physics API stays backend-agnostic for tests and optional builds.",
    },
    "physics/world": {
        "what": ["PhysicsWorld: config (gravity, dt, substeps, CCD), SimulationState machine, body registry, profiles."],
        "how": ["step(dt); sync transforms to components; ProfileManager presets."],
        "why": "One world per scene; profiles tune quality without code forks.",
        "workflow": ["construct", "add bodies/forces", "step", "sync transforms"],
    },
    "physics/bodies": {
        "what": ["RigidBody + shape hierarchy (box/sphere/capsule/cylinder/plane/triangle mesh) + PhysicsComponent on actors."],
        "how": ["ShapeFactory builds shapes; component binds body to Actor."],
        "why": "Uniform body API over backend-specific types.",
    },
    "physics/forces": {
        "what": ["Force generators: gravity, spring, drag, volumes, fields, presets, registry."],
        "how": ["ForceRegistry maps generators→bodies each step; force_system_example.hpp is a usage sketch."],
        "why": "Gameplay forces without rewriting the integrator.",
    },
    "physics/materials-phys": {
        "what": ["Friction/restitution physics materials."],
        "how": ["physics_material.hpp/cpp applied at body create."],
        "why": "Artist-tunable contact response separate from render materials.",
    },
    "physics/math-constants": {
        "what": ["Shared physics scalars and math utils."],
        "how": ["physics_constants + physics_math."],
        "why": "Avoid magic numbers across forces and backend helpers.",
    },
    "physics/debug": {
        "what": ["Force debugger visualization hooks."],
        "how": ["force_debugger integrates with gizmo/draw paths."],
        "why": "Seeing forces beats guessing spring constants.",
    },
    "audio/system": {
        "what": [
            "AudioSystem facade over miniaudio: SFX/Music/Ambient buses, handles, 3D listener, volume clamps.",
        ],
        "how": [
            "initialize → loadSound(path, category) → play / play3D → setCategoryVolume / setMasterVolume → shutdown",
            "ma_engine/ma_sound forward-declared; SoundHandle 0 invalid.",
        ],
        "why": "Game-ready audio without OpenAL; one facade for tools and runtime.",
        "workflow": ["initialize", "loadSound", "play/play3D", "set volumes", "shutdown"],
    },
    "systems/build": {
        "what": ["CMake options gate NRD, OIDN, DLSS, Jolt, tests; shaders custom target compiles GLSL→SPIR-V."],
        "how": ["Top-level + ohao + examples + shaders CMakeLists."],
        "why": "Optional heavy deps must not break bare builds.",
    },
    "systems/examples": {
        "what": ["Example map: cornell_box, model_viewer, env_demo, interactive, turntable share example_cli.hpp flags."],
        "how": ["interactive is full dogfood (WASD, mode switch, --denoise=dlssrr)."],
        "why": "Thin drivers over one API — golden and demo paths stay aligned.",
    },
    "systems/tests": {
        "what": ["Unit suites + golden image gate for PT/deferred regressions."],
        "how": ["tests/engine, tests/renderer, tests/golden."],
        "why": "Visual regressions are the real product test for a renderer.",
    },
    "systems/status": {
        "what": ["STATUS.md evidence-based feature matrix; docs/bugs_solved root-cause archive."],
        "how": ["Update STATUS when a feature is proven, not when the PR lands."],
        "why": "Stops aspirational docs from lying about shipping quality.",
    },
    "systems/public-scope": {
        "what": ["Public face deliberately omits non-product research trees and disabled shader archives."],
        "how": ["Monograph tree covers ohao/ + active shaders only."],
        "why": "GH Pages is the engine product story, not every experiment.",
    },
}


def _auto_fill(module: str, child: dict) -> dict:
    """Fallback rich content from probe when no KNOWLEDGE entry."""
    pr = probe(child.get("files") or [])
    notes = pr["notes"]
    api = pr["api"]
    title = child["title"]
    summary = child["summary"]
    files = child.get("files") or []
    what = notes[:3] or [
        f"**{title}** — {summary}",
        f"Primary sources: {', '.join(files[:4]) if files else 'see hub'}.",
    ]
    how = child.get("workflow") or []
    if not how:
        how = [
            f"Read and modify the listed sources under the module's ownership rules.",
            f"Wire through the public types exposed in headers; keep shaders and layout_meta in lockstep when touching GPU structs.",
        ]
        if api:
            how.append("Key symbols: " + "; ".join(api[:6]))
    why = child.get("why") or (
        f"This unit exists so the {module} module can evolve without leaking its internals into unrelated layers. "
        f"Keeping {title.lower()} explicit documents the contract for the next change."
    )
    contracts = child.get("topics") or child.get("contracts") or []
    if not contracts and files:
        contracts = [f"Sources of truth: {f}" for f in files[:4]]
    design = child.get("design") or notes
    return {
        "what": what,
        "how": how if isinstance(how, list) and how and isinstance(how[0], str) and not how[0].startswith("Read") else (
            [f"Implementation steps for {title}:"] + list(how) if how else how
        ),
        "why": why,
        "contracts": contracts,
        "design": design,
        "workflow": child.get("workflow") or [],
        "math": child.get("math") or [],
    }


def write_unit_md(module: str, child: dict) -> Path:
    key = f"{module}/{child['id']}"
    data = {**_auto_fill(module, child), **KNOWLEDGE.get(key, {})}
    # KNOWLEDGE values should win field-by-field
    if key in KNOWLEDGE:
        base = _auto_fill(module, child)
        base.update({k: v for k, v in KNOWLEDGE[key].items() if v})
        data = base

    lines = [
        "---",
        f"module: {module}",
        f"id: {child['id']}",
        f"title: {child['title']}",
        "---",
        "",
        "## What",
        "",
    ]
    for p in data.get("what") or []:
        lines.append(p)
        lines.append("")
    lines += ["## How", ""]
    for p in data.get("how") or []:
        if p.startswith("Step:") or (data.get("workflow") and p in data["workflow"]):
            lines.append(f"- {p.replace('Step: ', '')}")
        else:
            lines.append(p)
            lines.append("")
    if data.get("workflow"):
        lines.append("")
        for s in data["workflow"]:
            if f"- {s}" not in lines:
                lines.append(f"- {s}")
        lines.append("")
    lines += ["## Why", ""]
    why = data.get("why") or ""
    if isinstance(why, list):
        why = " ".join(why)
    lines.append(why)
    lines.append("")
    lines += ["## Contracts", ""]
    for c in data.get("contracts") or []:
        lines.append(f"- {c}")
    lines.append("")
    if data.get("math"):
        lines += ["## Math", ""]
        for m in data["math"]:
            lines.append(m)
            lines.append("")
    if data.get("design"):
        lines += ["## Notes", ""]
        for d in data["design"]:
            lines.append(d)
            lines.append("")
    # file list as notes
    lines += ["## Notes", ""]
    lines.append("Source map:")
    for f in child.get("files") or []:
        lines.append(f"- `{f}`")
    lines.append("")

    out = CONTENT / module / f"{child['id']}.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")
    return out


def author_all() -> int:
    n = 0
    for mod in TREE:
        for c in mod["children"]:
            write_unit_md(mod["id"], c)
            n += 1
    print(f"Authored {n} rich markdown units under {CONTENT}")
    return n


if __name__ == "__main__":
    author_all()
