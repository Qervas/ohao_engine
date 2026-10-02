# Sub-project 1 — Runtime shell

**Status:** approved design, 2026-10-02
**Roadmap position:** 1 of 5 toward the FPS vertical slice
(runtime shell → ECS world → level pipeline → player → combat).

## Goal

OHAO runs as a game engine host: a window, a Vulkan swapchain the deferred
renderer presents into directly, a fixed-step engine loop, and an input action
map. No per-frame GPU→CPU readback in windowed mode.

**Done when** `ohao_game` opens a window, renders the DamagedHelmet + HDRI scene
with the deferred path, flies a camera from the input action map, holds
≥ 60 fps at 1080p, survives resize and minimize, and finishes a scripted
600-frame run with zero validation errors.

## Non-goals

- ECS, gameplay API beyond two loop hooks (sub-project 2).
- Level loading (sub-project 3), physics in the loop (sub-project 4).
- Restructuring `VulkanRenderer` into context / scheduler / paths. That happens
  in sub-project 2, when render extraction changes what the renderer consumes.
- Presenting the RT render modes. They keep working headless; windowed
  presentation is deferred-only for now. (The blit is format-agnostic, so adding
  them later is small.)

## Architecture

```
ohao/runtime/                 NEW static library ohao_runtime
  window.{hpp,cpp}            GLFW window, GLFW_NO_API, VkSurfaceKHR creation,
                              framebuffer-size and minimize events
  input.{hpp,cpp}             action map: keys / mouse buttons → named actions,
                              axes from key pairs, raw mouse delta, cursor capture,
                              pressed / held / released edges per frame
  clock.{hpp,cpp}             fixed-step accumulator (default 60 Hz), max-steps
                              clamp, interpolation alpha, frame-time statistics
  application.{hpp,cpp}       owns Window, Input, VulkanRenderer; runs the loop;
                              calls GameHooks

ohao/gpu/vulkan/
  swapchain.{hpp,cpp}         NEW: surface → swapchain, image views not needed
                              (blit target), per-image render-finished semaphores,
                              recreate()
  renderer.*                  windowed initialize path, present output,
                              Forward path deleted

examples/ohao_game.cpp        NEW: windowed host for the helmet scene
examples/interactive.cpp      DELETED (GL-quad viewer; replaced by ohao_game)
```

Dependency direction: `ohao_runtime` → `ohao_renderer`/`ohao_gpu_vulkan`. The
renderer never includes runtime headers; it takes a `VkSurfaceKHR` and an
extent. GLFW is linked only by `ohao_runtime`.

### Game hooks

```cpp
struct GameHooks {
    std::function<void(Application&)>               start;
    std::function<void(Application&, float fixedDt)> fixedUpdate;
    std::function<void(Application&, float frameDt, float alpha)> update;
};
```

Deliberately minimal — sub-project 2 replaces these with ordered ECS systems.

### The loop

```
start()
while running:
    window.poll()                         → events, resize, close
    input.beginFrame(events)              → edges computed for this frame
    clock.tick()                          → frameDt, steps (≤ maxSteps), alpha
    repeat steps:  hooks.fixedUpdate(fixedDt)
    hooks.update(frameDt, alpha)
    if minimized:  continue               (simulation keeps running)
    renderer.render()                     → acquire · deferred · blit · submit · present
    stats.record(cpuMs, gpuMs)
```

Fixed step 1/60 s. `maxSteps` = 5; excess accumulated time is discarded and
counted (no spiral of death). `alpha = accumulator / fixedDt ∈ [0, 1)`.

## Swapchain and frame synchronisation

- **Instance:** add `glfwGetRequiredInstanceExtensions()` to the instance
  extensions when a window is requested. **Device:** add `VK_KHR_swapchain`, and
  require the graphics queue family to support present on the surface.
- **Format:** prefer `B8G8R8A8_UNORM`, then `R8G8B8A8_UNORM` (colour space `SRGB_NONLINEAR`), else the first offered.
  The deferred output is already gamma-encoded by the tonemap shader; an `_SRGB` swapchain would encode it a
  second time and wash the image out, so the blit copies display-ready bytes into a UNORM image.
- **Present mode:** `MAILBOX` if available, else `FIFO`. `--vsync` forces `FIFO`.
- **Image count:** `minImageCount + 1`, clamped to `maxImageCount`.
- **Usage:** `TRANSFER_DST` — the renderer blits into swapchain images; it never
  renders into them.
- **Frames in flight:** the existing 3-slot `FrameResources` ring (fence +
  command buffer per slot), plus one *image-acquired* semaphore per slot.
- **Render-finished semaphores: one per swapchain image**, indexed by the
  acquired image index. Per-slot render-finished semaphores are wrong whenever
  image count ≠ slot count.
- **Frame:**
  1. wait slot fence; `vkAcquireNextImageKHR` (signals slot's acquired semaphore)
  2. reset fence, record deferred into the slot's command buffer
  3. final deferred image `SHADER_READ_ONLY → TRANSFER_SRC`;
     swapchain image `UNDEFINED → TRANSFER_DST`;
     `vkCmdBlitImage` (linear filter, full extents of each);
     swapchain image `TRANSFER_DST → PRESENT_SRC`;
     final image back to `SHADER_READ_ONLY`
  4. submit: wait acquired @ `TRANSFER`, signal image's render-finished, fence
  5. `vkQueuePresentKHR` waiting on render-finished
- **Recreate** on `VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR`, or a
  framebuffer-size event: `vkDeviceWaitIdle`, rebuild swapchain, call
  `renderer.resize(w, h)` (its first production caller). `VulkanRenderer::resize`
  must forward to `DeferredRenderer::onResize` — today it does not, so a resize
  would leave the G-buffer and post targets at the old size. 0×0 extent = minimized:
  skip rendering, keep simulating, recreate on restore.
- **Readback is opt-in:** headless initialize keeps the staging-buffer copy and
  `getPixels()`; windowed initialize never records `copyDeferredOutputToPixelBuffer`.

## Forward removal

Delete `RenderMode::Forward`, `renderMultiFrame`, `renderLegacy`,
`renderSceneObjects`, `createPipeline`, `createRenderPass`,
`createOffscreenFramebuffer`, `createShadowRenderPass`, `createShadowResources`,
`createShadowPipeline`, `renderShadowPass`, the forward UBO, descriptor-set
layout / pool / sets, `m_pipeline`, `m_pipelineLayout` (forward),
`shaders/core/forward.{vert,frag}`, and `shaders/shadow/shadow_depth.{vert,frag}`
(loaded only by the forward shadow pipeline; the deferred CSM pass uses
`shadow_csm.{vert,geom}`). Keep `createLightBuffer` (deferred reads it), the
command pool, and sync objects that deferred or RT use.

`render()` becomes: RT mode → `renderRTPipeline`, otherwise → `renderDeferred`.
Default `m_renderMode` = `Deferred`. `isRasterRenderMode` now means Deferred only.
The shader-path probe in `renderer.cpp` that tests for `core_forward.vert.spv`
switches to a deferred shader.

## Verification

| Claim | Evidence |
|---|---|
| Headless output unchanged | golden corpus `max_abs 0` after each task; `renderer_test`, all C++ suites |
| Forward is gone | no `RenderMode::Forward` / `renderLegacy` symbols; build + suites green |
| Clock is correct | unit tests: exact step counts for synthetic frame-time sequences, `maxSteps` clamp and discarded-time counter, `alpha ∈ [0,1)` |
| Input is correct | unit tests: bind / rebind, pressed / held / released edges and axis values from a synthetic event stream |
| Presentation is correct | `ohao_game --frames 600 --resize-at 200` under `OHAO_VALIDATION=1`: zero validation messages, exit 0 |
| No windowed readback | that run asserts the readback-copy counter is 0 |
| Performance | stats printout for the helmet scene at 1080p: ≥ 60 fps; CPU ms and GPU ms (timestamp queries) reported separately |

Validation layers must be installed for the presentation check; if they are
absent the run reports that rather than passing silently.

## Risks

- **Present-capable queue:** assumed to be the graphics family (true on every
  desktop GPU this targets). If not, fail initialization with a clear error
  rather than add a separate present queue.
- **`DeferredRenderer::onResize` has never had a caller.** It is wired in this
  sub-project and exercised by the forced-resize run; if it is broken, fixing it
  is in scope.
- **Validation noise from existing passes** may surface once a window path is
  exercised under validation. Real errors get fixed here; they are in scope
  because the done-criterion is zero messages.
