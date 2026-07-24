/**
 * OHAO monograph — shared jargon dictionary.
 * Keys are match strings (longest first when building the scanner).
 * Used by hover tooltips + the Glossary chapter page.
 */
window.OHAO_GLOSSARY = {
  // ── Light transport ─────────────────────────────────────────────
  NEE: {
    expand: "Next Event Estimation",
    def: "Explicitly sample a point on a light and cast a shadow ray, instead of waiting for a random bounce to hit the light by chance. Cuts noise on small bright lights.",
    group: "Light transport",
  },
  MIS: {
    expand: "Multiple Importance Sampling",
    def: "Blend two (or more) sampling strategies—e.g. “sample the light” and “sample the BRDF”—with weights from their PDFs so neither strategy alone creates huge fireflies.",
    group: "Light transport",
  },
  PDF: {
    expand: "Probability Density Function",
    def: "How likely a continuous random sample was. Monte Carlo divides contribution by the PDF so rare and common samples stay unbiased.",
    group: "Light transport",
  },
  BRDF: {
    expand: "Bidirectional Reflectance Distribution Function",
    def: "How much light reflects from an incoming direction toward an outgoing direction at a surface. OHAO uses Cook-Torrance with a GGX microfacet lobe + Lambert diffuse.",
    group: "Materials",
  },
  BSDF: {
    expand: "Bidirectional Scattering Distribution Function",
    def: "Generalization of the BRDF that can include transmission (refraction) as well as reflection.",
    group: "Materials",
  },
  GGX: {
    expand: "Trowbridge–Reitz / GGX normal distribution",
    def: "Industry-standard microfacet NDF used in modern PBR. Controls how spread-out specular highlights are as roughness increases. Implemented in brdf_ggx.glsl.",
    group: "Materials",
  },
  NDF: {
    expand: "Normal Distribution Function",
    def: "In microfacet theory: statistical distribution of tiny surface facet normals. GGX is OHAO’s NDF for specular highlights.",
    group: "Materials",
  },
  PBR: {
    expand: "Physically Based Rendering",
    def: "Shading that follows energy-aware material models (metallic-roughness, Fresnel, microfacet BRDFs) so assets look consistent under different lights.",
    group: "Materials",
  },
  F0: {
    expand: "Base reflectance at normal incidence",
    def: "How reflective a surface is when viewed head-on. Dielectrics ≈ 0.04; metals tint F0 toward base color. F₀ = lerp(0.04, baseColor, metallic).",
    group: "Materials",
  },
  ORM: {
    expand: "Occlusion / Roughness / Metallic map",
    def: "Packed texture: R = ambient occlusion, G = roughness, B = metallic (glTF convention). Multiplies material scalars in GBuffer and RT.",
    group: "Materials",
  },
  IBL: {
    expand: "Image-Based Lighting",
    def: "Lighting from an environment map (usually HDR equirectangular) instead of only analytic lights. Used in deferred reflections and path-tracer miss rays.",
    group: "Lighting",
  },
  HDRI: {
    expand: "High Dynamic Range Image (environment)",
    def: "Floating-point environment map that can store sun-bright values. Sampled for path-tracer miss/env MIS and deferred IBL.",
    group: "Lighting",
  },
  HDR: {
    expand: "High Dynamic Range",
    def: "Colors stored above the 0–1 display range (e.g. RGBA32F accumulation) before tonemapping compresses them for the screen.",
    group: "Imaging",
  },
  AOV: {
    expand: "Arbitrary Output Variable",
    def: "Extra render targets beside beauty—albedo, normal, depth, roughness, diffuse/specular radiance—used for denoising and debugging.",
    group: "Imaging",
  },
  // ── Acceleration / Vulkan RT ────────────────────────────────────
  TLAS: {
    expand: "Top-Level Acceleration Structure",
    def: "Vulkan RT structure of instances. Each instance points at a BLAS plus a transform. Shared by the path tracer and hybrid RT techniques.",
    group: "Ray tracing",
  },
  BLAS: {
    expand: "Bottom-Level Acceleration Structure",
    def: "Vulkan RT structure for one mesh’s triangles. Built once per static mesh; referenced by TLAS instances.",
    group: "Ray tracing",
  },
  SBT: {
    expand: "Shader Binding Table",
    def: "GPU table that maps ray-query geometry to raygen / miss / hit shader groups in a ray-tracing pipeline.",
    group: "Ray tracing",
  },
  AS: {
    expand: "Acceleration Structure",
    def: "Generic term for BLAS/TLAS used to accelerate ray–triangle tests on the GPU.",
    group: "Ray tracing",
  },
  // ── Pipelines / raster ──────────────────────────────────────────
  GBuffer: {
    expand: "Geometry Buffer",
    def: "Multiple render targets that store surface data (position, normal, albedo, …) so lighting can run as a full-screen pass without re-drawing meshes per light.",
    group: "Deferred",
  },
  "G-buffer": {
    expand: "Geometry Buffer",
    def: "Same as GBuffer: MRT surface data for deferred lighting.",
    group: "Deferred",
  },
  MRT: {
    expand: "Multiple Render Targets",
    def: "Writing several color attachments in one pass (e.g. GBuffer0–2 + velocity).",
    group: "Deferred",
  },
  CSM: {
    expand: "Cascaded Shadow Maps",
    def: "Split the view frustum into distance cascades, each with its own shadow map, so nearby shadows stay sharp without a huge single map.",
    group: "Deferred",
  },
  SSAO: {
    expand: "Screen-Space Ambient Occlusion",
    def: "Approximate contact shadows/occlusion from the depth (and normals) already in the G-buffer—cheap ambient darkening in crevices.",
    group: "Post",
  },
  SSR: {
    expand: "Screen-Space Reflections",
    def: "Trace reflections in screen space using depth; fast but misses off-screen geometry. Experimental in OHAO deferred.",
    group: "Post",
  },
  SSGI: {
    expand: "Screen-Space Global Illumination",
    def: "Approximate bounce lighting from on-screen data only. Experimental / look-dev in deferred.",
    group: "Post",
  },
  TAA: {
    expand: "Temporal Anti-Aliasing",
    def: "Blend the current frame with history using motion vectors to smooth edges and reduce shimmer. Needs sub-pixel jitter.",
    group: "Post",
  },
  ACES: {
    expand: "Academy Color Encoding System (tonemap)",
    def: "Film-style curve that compresses HDR values into displayable LDR. One of OHAO’s tonemap operators.",
    group: "Imaging",
  },
  // ── Denoise / upscale ──────────────────────────────────────────
  OIDN: {
    expand: "Open Image Denoise (Intel)",
    def: "CPU/GPU denoise library. OHAO’s default offline path: beauty guided by albedo + normal AOVs.",
    group: "Denoise",
  },
  NRD: {
    expand: "NVIDIA Real-time Denoisers",
    def: "Realtime denoise suite. OHAO uses REBLUR-style paths; inputs must be packed (YCoCg + normalized hit distance), not raw linear RGB.",
    group: "Denoise",
  },
  REBLUR: {
    expand: "NRD REBLUR denoiser",
    def: "NRD algorithm for noisy diffuse/specular radiance with hit-distance guidance. Demands correct AOV packing.",
    group: "Denoise",
  },
  YCoCg: {
    expand: "Luma / chroma color encoding",
    def: "Color space used when packing radiance for NRD: Y = brightness, Co/Cg = chroma. Feeding linear RGB here caused the magenta-helmet bug.",
    group: "Denoise",
  },
  "DLSS-RR": {
    expand: "DLSS Ray Reconstruction",
    def: "NVIDIA NGX path that denoises/reconstructs ray-traced signals with AI, using extended AOV guides (motion, hit distance, …).",
    group: "Denoise",
  },
  DLSS: {
    expand: "Deep Learning Super Sampling",
    def: "NVIDIA family of neural upscalers/reconstructors. In this repo, Ray Reconstruction (DLSS-RR) is the relevant mode when NGX is linked.",
    group: "Denoise",
  },
  NGX: {
    expand: "NVIDIA GameWorks / DLSS SDK host",
    def: "SDK layer that hosts DLSS/DLSS-RR. CMake enables OHAO_DLSS when the static NGX lib is present.",
    group: "Denoise",
  },
  ReSTIR: {
    expand: "Reservoir Spatio-Temporal Importance Resampling",
    def: "Resamples light candidates over space and time for efficient many-light / GI sampling. Reservoir planes live at set-0 bindings 29–34 in the path tracer.",
    group: "Light transport",
  },
  // ── Sampling ────────────────────────────────────────────────────
  Sobol: {
    expand: "Sobol quasi-Monte Carlo sequence",
    def: "Low-discrepancy sample sequence (often Owen-scrambled). Default offline path-tracer sampler for faster convergence than pure random.",
    group: "Sampling",
  },
  PCG: {
    expand: "Permuted Congruential Generator",
    def: "Fast hash-style RNG used for the realtime path-tracer profile when Sobol is too heavy.",
    group: "Sampling",
  },
  CDF: {
    expand: "Cumulative Distribution Function",
    def: "Running integral of a PDF. Env maps build marginal/conditional CDFs so bright texels are sampled more often.",
    group: "Sampling",
  },
  // ── Engine / GPU ────────────────────────────────────────────────
  SSBO: {
    expand: "Shader Storage Buffer Object",
    def: "Large GPU buffer readable/writable in shaders (lights, materials, CDFs). More flexible than a small UBO.",
    group: "Vulkan",
  },
  UBO: {
    expand: "Uniform Buffer Object",
    def: "Small, frequently updated GPU buffer for constants (camera, few lights). Deferred uses a tight light UBO budget.",
    group: "Vulkan",
  },
  Bindless: {
    expand: "Bindless textures",
    def: "Textures addressed by index in a large array (sampler2D textures[]) instead of rebinding descriptor sets per draw.",
    group: "Vulkan",
  },
  LOD: {
    expand: "Level of Detail",
    def: "Mip level or quality tier. Deferred IBL uses roughness to pick an env-map LOD.",
    group: "Imaging",
  },
  FPS: {
    expand: "Frames Per Second",
    def: "How many images the interactive path presents each second.",
    group: "Realtime",
  },
  spp: {
    expand: "Samples Per Pixel",
    def: "How many independent path samples contribute to one pixel before (or while) averaging. Higher spp → less noise, more cost.",
    group: "Light transport",
  },
  SPP: {
    expand: "Samples Per Pixel",
    def: "Same as spp: Monte Carlo samples averaged into one pixel.",
    group: "Light transport",
  },
  GI: {
    expand: "Global Illumination",
    def: "Light that has bounced at least once (indirect). Full path tracing estimates multi-bounce GI; hybrid RT often does one bounce only.",
    group: "Light transport",
  },
  PT: {
    expand: "Path Tracing / Path Tracer",
    def: "Monte Carlo method that follows light paths with random bounces. OHAO’s KHR ray-tracing offline/realtime profiles.",
    group: "Light transport",
  },
  RT: {
    expand: "Ray Tracing",
    def: "Casting rays through an acceleration structure. Includes full path tracing and cheaper hybrid queries (shadow/GI rays).",
    group: "Ray tracing",
  },
  KHR: {
    expand: "Khronos (Vulkan extension prefix)",
    def: "In this engine: VK_KHR_ray_tracing_pipeline / acceleration_structure—the official Vulkan ray-tracing APIs.",
    group: "Vulkan",
  },
  ECS: {
    expand: "Entity Component System (actor/component model)",
    def: "Scene objects (actors) hold components (mesh, material, light, physics) instead of deep inheritance trees.",
    group: "Scene",
  },
  TRS: {
    expand: "Translation / Rotation / Scale",
    def: "The three parts of a transform. Composed into a matrix for rendering and physics sync.",
    group: "Scene",
  },
  glTF: {
    expand: "GL Transmission Format",
    def: "Standard 3D asset format (often .glb). OHAO loads metallic-roughness PBR via tinygltf.",
    group: "Assets",
  },
  GLB: {
    expand: "Binary glTF",
    def: "Single-file binary package of a glTF scene (meshes, materials, textures).",
    group: "Assets",
  },
  FBX: {
    expand: "Autodesk FBX",
    def: "DCC interchange format. Loaded through Assimp for meshes/materials (animation path was renovation-scoped).",
    group: "Assets",
  },
  AABB: {
    expand: "Axis-Aligned Bounding Box",
    def: "Box aligned to world axes used for culling and broad-phase collision.",
    group: "Math",
  },
  NDC: {
    expand: "Normalized Device Coordinates",
    def: "Post-projection coordinates (roughly −1…1). Used for velocity (TAA) and camera ray construction.",
    group: "Math",
  },
  FOV: {
    expand: "Field of View",
    def: "Angular width of the camera frustum. Extracted from the inverse projection in the path tracer.",
    group: "Camera",
  },
  DoF: {
    expand: "Depth of Field",
    def: "Optical blur for out-of-focus regions. Available as an experimental post effect.",
    group: "Post",
  },
  SSS: {
    expand: "Subsurface Scattering",
    def: "Light that enters a surface and exits nearby (skin, wax). OHAO’s SSS pass is biased look-dev, not a full BSSRDF.",
    group: "Materials",
  },
  BSSRDF: {
    expand: "Bidirectional Scattering Surface Reflectance Distribution Function",
    def: "True subsurface transport model (entry and exit points differ). Heavier than the engine’s approximate SSS hacks.",
    group: "Materials",
  },
  Fresnel: {
    expand: "Fresnel reflectance",
    def: "Surfaces reflect more at grazing angles. Schlick approximation: F = F₀ + (1−F₀)(1−cosθ)⁵.",
    group: "Materials",
  },
  Lambert: {
    expand: "Lambertian diffuse",
    def: "Ideal matte reflector: f = albedo/π. Used for the diffuse lobe alongside GGX specular.",
    group: "Materials",
  },
  "Cook-Torrance": {
    expand: "Cook–Torrance microfacet specular",
    def: "Specular BRDF form D·G·F / (4 n·v n·l). OHAO evaluates D=GGX with height-correlated Smith G.",
    group: "Materials",
  },
  VMA: {
    expand: "Vulkan Memory Allocator",
    def: "Library that simplifies GPU memory allocation for buffers and images.",
    group: "Vulkan",
  },
  GLFW: {
    expand: "Graphics Library Framework",
    def: "Window/input library used by the interactive example viewer—not required by the core offscreen renderer.",
    group: "Host",
  },
  Jolt: {
    expand: "Jolt Physics",
    def: "Third-party rigid-body engine behind OHAO’s IPhysicsBackend implementation.",
    group: "Physics",
  },
  miniaudio: {
    expand: "miniaudio library",
    def: "Single-file audio library. AudioSystem is a thin facade over it (SFX/Music/Ambient, 3D listener).",
    group: "Audio",
  },
  SFX: {
    expand: "Sound effects category",
    def: "One-shot gameplay/UI sounds. One of three AudioSystem mix buses (with Music and Ambient).",
    group: "Audio",
  },
};
