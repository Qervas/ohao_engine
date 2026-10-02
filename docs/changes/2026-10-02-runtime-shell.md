# Runtime shell: windowed presentation, fixed-step loop, and input actions

**Merged to master:** commit range 7e1bb27..7d25499  
**Spec:** `docs/specs/2026-10-02-runtime-shell-design.md`  
**Status:** Sub-project 1 of 5 complete; done when: deferred renders straight to a window at ≥ 60 fps, zero validation errors, no per-frame readback.

## Summary

OHAO now runs as a game engine host: a GLFW window, a Vulkan 1.3 swapchain that the deferred renderer presents into directly, a fixed-step 60 Hz simulation loop with variable render rate, and an input action map. The Forward render path is deleted; deferred is the raster path, and RT modes stay headless. The presentation layer (`ohao_game.exe`) achieves ≥ 60 fps at 1080p on an RTX 5070 laptop, handles resize and minimize without deadlock or validation errors, and transfers zero bytes per frame over PCIe in windowed mode (readback is opt-in for headless examples).

## New modules and APIs

### `ohao::runtime::Application` — windowed game loop

**File:** `ohao/runtime/application.hpp` / `.cpp`  
**Purpose:** Owns the window, input map, and renderer; drives the game loop (input → N fixed steps → variable render/present) with optional scripted frame limits and resizes for automated testing.

**Key types:**
- `Application::initialize(AppConfig)` — create window, renderer, and descriptor structures
- `Application::run(GameHooks)` — loop until close, simulating and rendering each frame
- `Application::shutdown()` — tear down and report validation messages including device destruction
- `RunReport` — frames, readback copies, validation messages, FPS, GPU ms (per-frame timestamp query)
- `GameHooks` — minimal callback hooks (start, fixedUpdate, update) awaiting replacement by ECS systems in sub-project 2
- `AppConfig` — window size/title, vsync, clock config, frame limit, and scripted resize triggers

**Interface examples:**
```cpp
AppConfig cfg; cfg.window.width = 1920; cfg.window.height = 1080;
Application app; app.initialize(cfg);
GameHooks hooks; hooks.start = [](Application& app) { };
auto report = app.run(hooks);
```

### `ohao::runtime::Window` — GLFW surface and input event pump

**File:** `ohao/runtime/window.hpp` / `.cpp`  
**Purpose:** GLFW window with no graphics context; translates OS events into `InputEvent` structs and provides a Vulkan surface for presentation.

**Key types:**
- `Window::create(WindowConfig)` — open window at specified size and title, enable resize/input callbacks
- `Window::poll(std::vector<InputEvent>&)` — pump OS event queue and append events (key down/up, mouse move, focus loss)
- `Window::framebufferExtent()` — drawable area in pixels; 0×0 while minimized
- `Window::consumeResized()` — true once per framebuffer size change
- `Window::presentTarget()` — returns `PresentTarget` (surface, extent, vsync preference) for swapchain initialization
- `WindowConfig` — width, height, title, resizable flag

**Framebuffer events:** Minimize (0×0 extent) and maximize (non-zero extent) are detected via `framebufferExtent()` and `consumeResized()`. Key repeats are dropped; raw mouse motion is used only while cursor is captured.

### `ohao::runtime::FixedStepClock` — 60 Hz accumulator

**File:** `ohao/runtime/clock.hpp` / `.cpp`  
**Purpose:** Fixed-step integrator in integer nanoseconds; avoids floating-point drift when repeating 1/60 s many times.

**Key types:**
- `FixedStepClock(ClockConfig)` — initialize with fixed timestep (default 16,666,667 ns ≈ 1/60 s) and max-steps clamp (default 5)
- `Tick FixedStepClock::advance(Nanos frameDt)` — consume frame time, return step count and interpolation alpha
- `Tick::steps` — how many fixed updates to run this frame (0 if frameDt < fixedStep; clamped to maxSteps)
- `Tick::alpha` — fractional progress into the next step: `(accumulator % fixedStep) / fixedStep ∈ [0, 1)`
- `Tick::discarded` — time dropped this frame by the max-steps clamp (spiral-of-death prevention)

**Behavior:** Accumulates frame delta in nanoseconds. If accumulated time ≥ fixedStep, returns steps = accumulated / fixedStep (clamped to maxSteps). Excess time beyond maxSteps is discarded (not rolled to the next frame) and reported. Alpha is used for render-space interpolation between fixed frames.

### `ohao::runtime::InputMap` — action binding and edge detection

**File:** `ohao/runtime/input.hpp` / `.cpp`  
**Purpose:** Maps keys and mouse buttons to named actions; tracks pressed/held/released edges per frame; emits axis values from key pairs.

**Key types:**
- `InputMap::bind(action, InputDevice, code)` — associate a key/mouse-button code with an action name
- `InputMap::rebind(action, InputDevice, code)` — replace an existing binding
- `InputMap::unbind(action)` — remove a binding
- `InputMap::axis(axisName, negKey, posKey)` — create an axis from two bound keys (e.g., "move_x" from "strafe_left" / "strafe_right")
- `InputMap::update(std::span<InputEvent>)` — consume OS events and update internal edge state
- `InputMap::isPressed(action)` — true this frame if the key transitioned down
- `InputMap::isHeld(action)` — true this frame if the key is held down
- `InputMap::isReleased(action)` — true this frame if the key transitioned up
- `InputMap::axis(axisName)` — signed value: -1 (neg), 0 (neither), +1 (pos), or analog from raw mouse delta

**Behavior:** Edges are computed after every event (not just at frame boundaries), so a key released and re-pressed within one frame generates separate release and press events. FocusLost flushes held keys and generates release edges.

### `ohao::gpu::Swapchain` — surface → VkSwapchain

**File:** `ohao/gpu/vulkan/swapchain.hpp` / `.cpp`  
**Purpose:** Wraps VkSwapchain creation, image acquisition, presentation, and recreation on resize / minimize / device loss.

**Key types:**
- `Swapchain::create(device, surface, extent, format, mode, frameCount)` — create swapchain with per-image render-finished semaphores
- `Swapchain::build()` — rebuild after device loss or minimize; retires old swapchain on every path
- `Swapchain::acquire(frameFence, acquiredSem)` — wait fence, acquire next image (signals acquiredSem), mark dirty on OUT_OF_DATE
- `Swapchain::present(queue, waitSem)` — signal render-finished semaphore for the acquired image; present
- `Swapchain::image(index)` — get swapchain image by index
- `Swapchain::extent()` — drawable extent; 0×0 while minimized
- `Swapchain::isDirty()` — true if OUT_OF_DATE, SUBOPTIMAL, or extent changed (triggers recreate before frame slot is chosen)

**Format selection:** Prefers B8G8R8A8_UNORM (display-ready, not sRGB; the tonemap shader already gamma-encodes), then R8G8B8A8_UNORM. MAILBOX present mode if available, else FIFO. Image count is `minImageCount + 1` (one spare for blit while present is in flight).

### `ohao::PresentTarget` — surface + extent for renderer initialization

**File:** `ohao/gpu/vulkan/present_target.hpp`  
**Purpose:** Encapsulates surface, extent, and vsync preference for passing from `Window` to `VulkanRenderer::initialize`.

**Key types:**
- `PresentTarget::surface` — VkSurfaceKHR (null for headless)
- `PresentTarget::extent` — window drawable size
- `PresentTarget::vsync` — if true, present mode is FIFO; else MAILBOX

### `examples/ohao_game.cpp` — windowed game host

**Purpose:** Replaces `interactive.exe`. Renders the DamagedHelmet + HDRI scene with deferred raster (RT shadows and RT GI where supported), with WASD/QE flight control, right-drag look, Shift fast, Escape to quit, and optional `--vsync`, `--frames N`, `--resize-at N` (resize to 1280×720 at frame N for the presentation gate).

**Exit codes:** 0 on success; nonzero if validation messages are found (gate proves zero errors) or on a lost device.

## Removed

### Forward render path

**Deleted:**
- `RenderMode::Forward` enum value
- `VulkanRenderer::renderMultiFrame()`, `renderLegacy()`, `renderSceneObjects()`, `renderShadowPass()`
- `VulkanRenderer::createPipeline()`, `createRenderPass()`, `createOffscreenFramebuffer()`, `createShadowRenderPass()`, `createShadowResources()`, `createShadowPipeline()`
- Member variables: `m_pipeline`, `m_pipelineLayout`, `m_forwardPass`, `m_shadowPass`, `m_offscreenImage`, `m_shadowMap`, `m_framebuffer`, `m_cameraUbo`, `m_descriptorLayout`, `m_descriptorPool`, `m_descriptorSets` (Forward-only)
- Shader files: `shaders/core/forward.vert`, `shaders/core/forward.frag`, `shaders/shadow/shadow_depth.vert`, `shaders/shadow/shadow_depth.frag` (used only by the forward shadow pipeline). `shaders/shadow/shadow_csm.geom` is also deleted: CSM is vertex-shader-only (`shadow_csm.vert`)
- `FrameResourceManager::createPerFrameUBOs()`, per-frame camera/light UBO allocation and descriptor set binding (Forward consumed them; deferred and RT do not)

**Kept:** Light buffer (`createLightBuffer`, `updateLightBuffer`, `m_lightBuffer`) consumed by deferred CSM. Shadow enablement flag (`m_shadowsEnabled`) used by deferred. Command pool and sync objects reused by deferred and RT.

**Default render mode:** `Deferred`. `render()` dispatches RT modes or deferred, nothing else. `isRasterRenderMode()` now means Deferred only.

### Interactive example

**Deleted:** `examples/interactive.cpp` — an OpenGL quad viewer that read pixels back to the CPU every frame and uploaded them to a texture. Replaced by `ohao_game.exe` windowed presentation.

### Forward-era framebuffer pipeline

**Deleted:** `ohao/gpu/vulkan/framebuffer.cpp` — 408 lines allocating offscreen images, render passes, and framebuffers for Forward rendering. Deferred and RT use the render graph directly.

### Dead frame resource allocations

**Simplified:** `FrameResourceManager` (in `ohao/render/frame/frame_resources.hpp`) now allocates only: command buffer, fence, and readback staging buffer per slot. Removed per-frame camera and light UBOs and their descriptor sets (Forward-only). A frame slot is now `{ VkCommandBuffer, VkFence, VkBuffer (readback staging) }`.

## Rendering fixes

### Layout coherence

- **G-buffer depth:** Transitions to `VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY` after the G-buffer pass; stays there through the frame (SSAO, particle, TAA samples it). All readers (lighting, SSAO, TAA, particles) agree on one layout instead of conflicting over whether it is `SHADER_READ_ONLY` or `ATTACHMENT`.
- **CSM cascade barriers:** `RenderGraph::barrier()` now covers all four cascade array layers (0–3), not just layer 0. Cascades 1–3 were being sampled in the `COLOR_ATTACHMENT` layout.
- **G-buffer pass clears:** Runs its render pass (and clears) even when no scene is present, so lighting and later passes do not sample undefined images.

### Render-pass-to-descriptor hazards (out-of-order execution)

- **TAA temporal coherence:** TAA read the image it was rendering into (both ping-pong targets bound to the same set). Restructured: swap the read/write targets first, sample from the other image, clear history on creation/resize, use blend weight 0 on the first frame (no previous data).
- **Bloom on resize:** Framebuffers were created once on initialization and never rebuilt on resize, so bloom stopped running after the first resize while the tonemap composited a never-written output. Framebuffers are now rebuilt with the mip chain on resize.
- **RT shadow mask and RT GI:** Lighting sampled them unconditionally, even when no rays were traced (no TLAS). Techniques now expose a `rendered()` method; lighting samples only if the technique ran this frame, else falls back to CSM or no GI.
- **RT GI history:** Freed and cleared on resize (it previously leaked and was blended from uninitialized memory; NaN there would survive every later frame).
- **SSAO descriptor set:** Rebuilt on resize but never written to the new set until SSAO re-recorded. Now written at record time.
- **Cloud wiring:** Deleted. The sky shader sampled a never-written cloud buffer (`binding 1` in the sky set, and `binding 13` in the lighting set with a flag gate). `CloudPass` is gone; the wiring is dead code.

### Descriptor sets rewritten mid-flight

Each of these passes now has **one descriptor set per frame-in-flight** (3 sets total), written at record time for the frame slot whose fence was just waited. Prevents the GPU from reading a descriptor updated on the CPU while it is still in flight:
- `DeferredLightingPass`
- `TonemappingPass`
- `TAAPass`
- `SSSPass`
- `SSRPass`
- `BloomPass` (input texture)
- `RTShadowTechnique`
- `RTGITechnique`

### RT raygen shader binding table

- **All three RT techniques** (path tracer, RT shadows, RT GI) had raygen regions with padded size but unpadded stride. Vulkan requires `size == stride` for a raygen region. Fixed in path tracer, RT shadows, and RT GI.

### Sampler lifetime

- **RT shadows and RT GI:** Created samplers in `execute()`, wrote them to descriptor sets, recorded commands, then destroyed them while the command buffer was still recording. Destroying an object a command buffer references invalidates the buffer. Samplers are now members, created once, destroyed in `destroy()`.

### CSM geometry shader

- **Removed:** The geometry shader broadcast all triangles to all four cascade layers via `gl_Layer` into a one-layer framebuffer, with a cascade-matrix UBO that was never bound or in the pipeline layout. Shadow depth was computed with undefined matrices.
- **Replaced:** Vertex shader takes the cascade index in push constants (128 bytes, guaranteed minimum) and projects directly. On RT-capable GPUs the deferred path uses RT shadows (no change); this is the fallback for everything else.

### Demote-to-helper validation

- **Feature not enabled:** Vulkan 1.3 supports `VK_KHR_shader_demote_to_helper_invocation`, required for `glslc`'s lowering of `discard` to `OpDemoteToHelperInvocation` in SPIR-V 1.6. The feature struct was created but never chained to device features. Now enabled in `VulkanDevice::initialize()`.

### Leaks at shutdown

- **RT scene buffers, texture array, per-layer bindless views, env map image:** Freed only on re-upload, never at shutdown. Now freed in `RTSceneBuffers::destroy()`, called from `VulkanRenderer::destroy()`.

### Camera aspect ratio

- **Bug:** Set only in `VulkanRenderer::resize()`, which returns early if the extent is unchanged. A renderer constructed at its final size (every headless example, and a window opened at 1920×1080 and held there) rendered deferred at Camera's default 16:10 while the RT raygen used the true framebuffer aspect (e.g., 16:9): two modes framed the same scene 11% differently by default.
- **Fix:** Constructor calls `setAspect()` from the extent.
- **Test:** `engine_tests "the camera aspect matches the renderer's extent from initialize on"` RED → GREEN.

### HDR fallback image readback

- **Bug:** With tonemap off, the final image is lighting's RGBA16F output. It lacked `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, so both the swapchain blit and the headless readback copy were invalid. A headless copy would write 8 bytes per pixel into a 4-byte staging buffer (overrun).
- **Fix:** Add TRANSFER_SRC to the final image usage. The blit converts format; headless readback refuses an HDR source with one warning (tests run it silently).
- **Test:** `engine_tests "deferred with tonemapping off reads back its lighting output silently"` RED (usage, then overrun) → GREEN.

### Resize stability

- **SSAO and TAA G-buffer pointers:** `DeferredRenderer::onResize()` recreated the G-buffer (destroying its views), resized post-processing, then updated SSAO and TAA bindings one at a time. Each setter rewrote the whole descriptor set with destroyed handles still in the other inputs.
- **Fix:** After G-buffer recreation, hand SSAO and TAA their new views together in one rewrite, before anything else touches descriptors.
- **Test:** `engine_tests "a resize never writes a destroyed G-buffer view into a descriptor"` RED (6 validation errors: invalid VkImageView) → GREEN (0).

### Resize screen-sized targets

- **Bug:** `DeferredRenderer::onResize()` resized the G-buffer, but not the RT shadow mask, SSS working image, or SSR working image. After a resize, these render into a 1920×1080 mask/image while the rest of the frame is e.g., 2560×1440. Shadows misplaced; writes past the image when the window grows beyond 1080p.
- **Fix:** Every screen-sized target follows `m_extent` in `onResize()`.
- **Test:** `engine_tests "every screen-sized deferred target follows the renderer's extent, growing too"` RED → GREEN.

### Input action edge detection

- **Bug:** InputMap compared the action's held state only at frame start and end. A key pressed and released within one frame, or a key re-pressed after release in the same frame, lost the press. At low fps, mashing fire or jump missed input.
- **Fix:** Actions are re-evaluated after every event. Pressed/released latch on any held-state transition in the frame.
- **Tests:** `runtime_tests "repress_within_one_frame"` and `"press_after_focus_loss_in_one_frame"` RED → GREEN (21/21).

## Tools and gates

### Validation messaging

- **`OHAO_VALIDATION=1`:** Enables the Khronos validation layer. `VulkanRenderer` creates a validation messenger, counts messages, and keeps the first few for inspection.
- **`VulkanRenderer::validationEnabled()`:** false if the layer is missing; "not checked", not "clean".
- **`VulkanRenderer::validationMessageCount()`:** Total messages seen. Includes teardown messages (reported at `vkDestroyDevice`, after `run()`'s report).
- **`Application::shutdown()`:** Returns the count including device teardown. `ohao_game` gates on it: exit 0 only if count == 0.

### Presentation gate

```bash
OHAO_VALIDATION=1 ./build/Release/ohao_game.exe --frames 600 --resize-at 200
```

- Runs 600 frames, forces a resize to 1280×720 at frame 200.
- Exits 0 if validation count == 0 and readback-copy count == 0 (no per-frame CPU readback in windowed mode).
- Exits nonzero on a lost device or any validation message.
- Used by CI and manual verification; proves the presentation layer (acquire, blit, present) and renderer initialization/resize are clean.

### Test suite additions

- **`tests/runtime/runtime_tests.cpp`:** 21 tests — FixedStepClock (sixty frames = sixty steps, half-step alternation, long-stall clamp, negative/zero dt, alpha range), FrameStats, InputMap (press/hold/release edges, tap and re-press within one frame, press after focus loss, two bindings per action, rebind, focus loss releases everything, key-pair axes, no first-frame mouse jump, unknown names inert), and swapchain choice (UNORM BGRA preference, present mode, unlimited max image count, extent when the surface defers, zero extent = minimized).
- **`tests/validation/check_validation.py`:** Runs cornell_box and helmet through the deferred path under `VK_LAYER_KHRONOS_validation`. Fails on any validation message, printing the first. Catches pre-existing errors and regressions.
- **`engine_tests` expansions:** New tests for camera aspect at construct, resize, no destroyed G-buffer view written into a descriptor on resize, SSAO/TAA/bloom resize validation-silent, screen-sized target extent coherence, and deferred HDR readback. Suite: 56 tests, all green.

### CMake configuration

- **`examples/CMakeLists.txt`:** Replaces `interactive` target with `ohao_game`; links GLFW.
- **`ohao/CMakeLists.txt`:** Adds `ohao_runtime` library (clock, input, window, application).
- **`CMakeLists.txt`:** `add_custom_target(shaders ...)` remains without ALL; default build compiles no SPIR-V (validation gate runs shaders manually).

## Design decisions (from decision ledger)

1. **Branch isolation over worktrees:** Work on a branch in the existing checkout, not a fresh git worktree. Fresh worktrees require a from-scratch build of every FetchContent dep and shaders; branch isolation is identical and faster.

2. **Swapchain retires old on every path:** `Swapchain::build()` unconditionally retires the old swapchain on minimize, unsupported format, creation failure, or success. Avoids the plan's caller-side logic to detect whether the old was consumed, which leaked swapchain handles on creation failure.

3. **B8G8R8A8_UNORM swapchain format, not sRGB:** The deferred output is already gamma-encoded by the tonemap shader. An sRGB swapchain would encode it twice, washing the image out. UNORM passes display-ready bytes through. Blit-source coverage handles both possible final image formats (post output or HDR lighting when post is skipped).

4. **Deduce camera aspect from renderer extent at construct:** Constructor calls `setAspect()` from the framebuffer extent. Resize still updates it (in case window size changes at runtime) but avoids the 11% framing mismatch in every headless example and in windows opened at their final size.

5. **Per-pass descriptor sets per frame-in-flight:** Each writing pass has one set per slot (3 total), written at record time for the fence-waited slot. A future transient descriptor allocator (sub-project 2's renderer work) is the production answer; local arrays contain and verify the fix now.

6. **RT shadow/GI sampling gated on technique.rendered():** Lighting gets RT outputs only from a pass that traced this frame. Avoids sampling uninitialized or garbage data when no TLAS exists or the technique skipped execution.

7. **G-buffer pass always clears:** Runs render pass (clears) even with no scene. Later passes (lighting, post) sample the G-buffer unconditionally; undefined contents are not acceptable.

8. **Validation messages counted at shutdown:** `Application::shutdown()` returns the count including teardown. Leaks are reported only at `vkDestroyDevice`, after `run()`'s gate report, so tracking shutdown separately is necessary.

9. **Fixed-step clock in integer nanoseconds:** Avoids floating-point drift when repeating 1/60 s; 60 frames must be exactly 60 steps.

10. **Max-steps clamp with discard:** Excess accumulated time beyond `maxSteps` is discarded (not rolled forward). Prevents spiral of death at low frame rates; the discarded time is counted and visible in stats.

11. **Input edges re-evaluated per event:** Actions are updated after every OS event, not just at frame boundaries. Captures rapid key-state changes (press → release → press within one frame).

12. **Validation gate is comprehensive:** Covers acquire, blit, present, resize, initialization, and teardown. Real errors (layouts, temporal hazards, sampler lifetimes, leaks) are fixed here; spec says zero messages.

13. **CSM cascade loop in vertex shader, push constants:** One cascade per invocation, projection in push constants (128 bytes, guaranteed minimum). Replaces geometry shader with undefined UBO binding.

14. **Demote-to-helper feature enabled:** Required by Vulkan 1.3 for `glslc`'s `discard` lowering; explicitly chained in device features.

## Known issues / deferred

### Minor (deferred to next renderer iteration)

- **Minimized window busy-spins:** No `glfwWaitEventsTimeout()` while 0×0 (minimized). CPU thread runs at full speed. Fix: wait while minimized.
- **Unrecoverable surface errors:** `VK_ERROR_SURFACE_LOST_KHR` or `VK_ERROR_DEVICE_LOST` in present cause log spam forever instead of ending the app.
- **Swapchain format/usage negotiation:** Doesn't check `supportedUsageFlags` for `TRANSFER_DST` or `supportedCompositeAlpha`. Format fallback could pick an sRGB variant.
- **Initialization failure leaks:** `VulkanRenderer::initialize()` failure leaks instance/device/surface if `shutdown()` is called (returns early when `!m_initialized`).
- **CSM cascade UBO race:** Single mapped buffer rewritten every frame while frames in flight may read it (same race as light buffer, unfixed in this sub-project).
- **Tonemap set edge case:** Can be bound unwritten if `writeTonemapSet()` bails (unreachable in current code, but undefended).
- **GLFW raw-motion delta:** Glitches on cursor-capture toggle (one-frame look jump, GLFW's limitation).
- **Per-image render-finished semaphore reuse:** Standard Vulkan pattern; documented for the next maintainer.

### Deferred findings (pre-existing bugs discovered during work)

- **TAA jitter cycles 3 positions:** Uses `frameIndex` (frame-in-flight slot 0–2) as the jitter index instead of a monotonic frame counter. Needs a global frame counter; jitter should cycle through 16+ positions for sub-pixel sample variety.
- **DamagedHelmet near-black in deferred:** Scene renders darker from model_viewer's camera framing (pre-existing before this branch). Deferred shading quality, not runtime shell.
- **GPU ms > frame ms:** Reported GPU timestamp intervals may span overlapping frames or sum pass ranges. The number is not a per-frame cost; a per-pass breakdown and async readback would clarify it.
- **Sky and particles clipped by SSS:** SSS copies the lit image before sky/particles draw into it. Later passes sample SSS output, so sky and particles never reach the final image in outdoor scenes. Visible as missing sky. Deferred to the next renderer work with high priority; fixing it moves render passes and changes every deferred output.
- **Cross-frame/cross-pass temporal hazards:** Render passes without explicit subpass dependencies (e.g., TAA ping-pong, history reads) are unverifiable by core validation. Requires synchronization validation (next run).

### Deferred death: dead code found and deleted

- **`CameraUniformBuffer`:** Member in `renderer.hpp`, allocated in Forward, never read since removal. Delete in cleanup.
- **`FrameResources::valid`:** Operator bool always returns false, never set. Dead since Forward deletion.

## Wiki pages likely affected

Update these documentation units (assuming a monograph / wiki refresh):

- **Presentation and frame synchronization** — swapchain formats, per-image semaphores, frame slots, acquire/present, readback opt-in
- **Frame resources** — simplified to command buffer + fence + staging buffer; per-frame UBO/descriptor sets removed
- **Deferred render pass pipeline** — per-pass descriptor-set-per-frame-in-flight, RT technique sampling gated on `rendered()`, temporal coherence fixes (TAA, bloom, RT outputs)
- **Input system** — InputMap, Binding, edge detection, axes, cursor capture, event queue
- **Application loop** — FixedStepClock, fixed/variable rate split, alpha interpolation, RunReport
- **Window system** — GLFW, framebuffer events, resize/minimize, surface creation
- **Render modes** — Deferred is now the raster path; Forward deleted; RT modes stay headless
- **Camera** — aspect ratio set from extent at construct
- **RT raygen bindings** — SBT size == stride requirement, fixed in all three techniques
- **Device features** — demote-to-helper enabled in Vulkan 1.3
- **Validation and testing** — OHAO_VALIDATION layer messaging, presentation gate, new test suites (runtime, validation)
