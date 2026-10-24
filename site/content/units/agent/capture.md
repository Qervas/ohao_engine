---
module: agent
id: capture
title: Frame capture
standard: v2
---

## What an agent looks at

`render.capture` is how an agent sees the world: a PNG file, an inline preview image, or both. The session checks the whole request (path, mode, sizes, post effects, exposure, region) and then hands it to its `api::Host`. Two hosts implement it. One is the CLI's headless `host::RenderHost`, used by `ohao run --render` and headless `ohao serve`, which starts a renderer on first capture. The other is the window's `runtime::LiveHost`. Both end in one procedure, `runtime::captureFrame`. It applies the world's camera and environment, sets the mode, denoiser, post effects and exposure, does a temporal reset, renders `frames` frames each after `World::sync()`, and reads back the last one. The CLI unit describes the frame defaults and the reset. This unit covers the parts sub-project 4 added: readback on request, what a window may capture, and the image encoders.

Because the procedure is shared, a lockstep window capture and a headless capture of the same world at the same size are the same pixels by construction, not by a tolerance. `check_live.py` compares them byte for byte, with post effects off and on.

## Readback on request

A capture asks for its last frame and gets that frame. Every reader of pixels works this way (captures, the examples, the tests, `ohao doctor`), and no frame copies anything to the CPU unless it was asked for, headless or windowed. `requestReadback()` marks the next frame:

{{cite ohao/gpu/vulkan/renderer.hpp "void requestReadback() noexcept { m_readbackRequested = true; }"}}

The flag is consumed by the next frame that starts, whether or not that frame is actually rendered:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// A request belongs to this frame; a frame that is not rendered leaves no readback."}}

A frame skipped because the window is minimized or the swapchain is out of date leaves nothing to read, and `readback()` returns an empty span instead of an old frame. When the frame did copy, `readback()` waits on that frame's fence only, never on the queue or the device, and returns a view of the slot's persistently mapped buffer:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "if (!f.stagingBufferMapped || !m_frameResources.waitForFrame(m_readbackSlot)) return {};"}}

In a window the copy is recorded inside the swapchain blit, where the final image is already in `TRANSFER_SRC` layout, so it costs one extra copy command and no layout transitions:

{{cite ohao/gpu/vulkan/render_dispatch.cpp "// A requested readback: the same image, already a transfer source, into the"}}

A headless frame copies the same way, only when asked (`copyDeferredOutputToStaging`). The renderer counts the copies (`requestedReadbackCount()`); the window host counts its own requests beside them. The presentation gate (`ohao_game --frames N`), `check_live.py` and `e2e_mcp.py` all require the difference, the unrequested copies, to stay 0 through teardown.

{{cite ohao/runtime/live_host.cpp "return copies > m_readbackRequests ? copies - m_readbackRequests : 0;"}}

The CPU reads the staging buffers, so they are allocated host-cached where the device offers cached memory that is also coherent:

{{cite ohao/render/frame/frame_resources.cpp "// Readback buffers: the CPU reads them, so host-cached memory where the device offers it"}}

Reading one 1280x720 frame (3.7 MB) from uncached host-visible memory took about 20 ms. The spec's sub-project 4 step-9 note records a one-frame headless capture dropping from about 45 ms of CPU time to about 1 ms.

A path-traced capture under a CPU-side denoiser (OIDN, NRD) reads the same way. `readback()` denoises the requested frame's images itself, once its fence has signalled:

{{cite ohao/runtime/frame_capture.cpp "const std::span<const uint8_t> pixels = renderer.readback();"}}

## What a window may capture

`LiveHost::capture` refuses three requests before rendering anything. A minimized window has no frame (`E_STATE`). Another size is `E_COMMAND`: the window captures at its own size, and the agent leaves `width` and `height` out. Path tracing is `E_COMMAND` too, because the path tracer's frames have no swapchain path, so a window cannot present them:

{{cite ohao/runtime/live_host.cpp "a window host captures deferred frames; path tracing needs a headless "}}

Otherwise the result depends on the time mode. In lockstep (or paused), the host first renders once if a resize is still pending, so the capture is at the window's current size. It then saves the window's post settings, runs `captureFrame` with a reset every time, and restores the settings, leaving the capture's last frame on screen. In realtime the world is moving, and a reset or extra frames would show the agent something the player never saw. So the capture is the next frame the window presents, with the live settings:

{{cite ohao/runtime/live_host.cpp "// Realtime: the next frame the window presents, as is (no reset, the live settings)."}}

That result is marked `deterministic: false`. A lockstep capture of the same world always gives the same pixels. That holds after step 1 of the sub-project, which made every deferred configuration bit-identical run to run (see the hash and determinism unit).

## Region, preview and the image beside the result

`region: [x, y, w, h]` crops the frame before anything else, so an agent can zoom into a detail at full resolution without paying for the whole frame. A region outside the frame is `E_COMMAND` at `/region`. The crop then goes to the PNG file (`path`) if one was asked for, and to the preview (`inline: true`). The preview is scaled so its long side is at most `maxSize`, 768 px by default; the spec puts a 768x432 preview at about 440 tokens. The scale is an area average: each output pixel is the mean of the source pixels it covers, each weighted by how much of it falls inside:

{{cite ohao/runtime/image.cpp "const double cover = std::min<double>(b, k + 1) - std::max<double>(a, k);"}}

`fitWithin` resamples vertically first, so each output row is a weighted sum of whole source rows, a run of contiguous multiply-adds per tap, and then horizontally over only the rows that are left. The spec measures 3 ms for 1280x720 to 768x432. The preview is a JPEG at quality 85 through `stb_image_write` (about 3.5 ms for 768x432), or a PNG when `format: "png"` asks for exact pixels.

The encoded preview does not go into the result JSON. It travels beside it, as `api::CallOutcome::image`. Over MCP it becomes an `image` content block after the text block, which is the form a model can look at:

{{cite ohao/rpc/server.cpp "if (!failed && o.image) {   // render.capture inline: the image a model looks at"}}

Over native JSON-RPC, and through `Session::call(string)`, it is appended to the result as `"image"` (base64) and `"mimeType"`. The result itself reports `width`, `height`, `imageSize`, `frames`, the total `ms`, `readbackMs` (the fence wait and the copy) and `encodeMs` (crop, scale, file and preview), plus the validation message count when the layers are on.

## A PNG writer of its own

The spec planned `stb_image_write` for PNG files and named `fpng` as the fallback if the encode was over budget. It was over by two orders of magnitude:

{{cite ohao/runtime/image.cpp "// stb_image_write's PNG took 7.3 s for one 1280x720 Cornell frame (its deflate tries up to"}}

The budget for a whole PNG capture is 30 ms. `fpng` would have needed the network and a new dependency, so the ruling (ledgered in the spec) was a writer in `ohao/runtime/image.cpp`, designed around what rendered frames look like. Each row is filtered `Up`, meaning minus the row above it (the first row uses `Sub`). Flat and slowly varying regions, which a rendered frame has a lot of, filter to long runs of equal bytes. The filtered bytes are then deflated with the fixed Huffman codes and one kind of match only, a repeat of the previous byte:

{{cite ohao/runtime/image.cpp "a match at distance 1: the byte again"}}

There is no hash chain, no search and no lazy matching: one pass, a byte compare per byte, and table lookups. The image is cut into bands of rows, each deflated on its own thread. The band count depends on the image height and never on the machine, so the file's bytes depend on the image alone:

{{cite ohao/runtime/image.cpp "const uint32_t bands = std::max<uint32_t>(1, std::min<uint32_t>(4, height / 64));"}}

The bands are joined the way pigz joins its blocks. Every band but the last ends with an empty stored block, which pads it to a byte boundary, so the bands' bytes can be concatenated:

{{cite ohao/runtime/image.cpp "an empty stored block (BFINAL 0, BTYPE 00): aligns the band"}}

The zlib trailer's Adler-32 is combined from the bands' checksums with zlib's `adler32_combine` formula, so no thread has to read another band's data. One case defeats run-length coding: a one-frame RT GI render is mostly noise, which codes at about 8.5 bits a byte. For such a band, the writer falls back to stored blocks of raw bytes whenever they are smaller:

{{cite ohao/runtime/image.cpp "if (band.zn > n + blocks * 5) {"}}

On synthetic 1280x720 frames the spec measures 4.1 ms for a mixed image and 6.1 ms for pure noise. The files are larger than a full deflate would write. That is the trade: a capture file exists to be read back exactly, not archived. `sp4_t16_capture_png_round_trips_losslessly` decodes the output with `stb_image` and requires every byte back.

## Where the time goes

A clean `serve_bench` run (step 11) puts a 1280x720 PNG capture at 22.7 ms against its 30 ms budget, a window readback at 5.34 ms against 2 ms, and an inline capture at 34.3 ms against 20 ms. The record attributes both misses to the deferred frame's own GPU cost: one Cornell deferred 1280x720 frame takes 5.0 to 5.6 ms of GPU time, which a capture waits for. Frank ruled that both budgets stay exactly as they are and that the misses belong to sub-project 2b, which makes the frame cheaper. They are not to be closed by changing the budgets.

:::why
Readback on request, rather than a ring that is always copying, makes "the frame I just rendered" a direct question. The capture marks its last frame, waits for that frame's fence, and reads the copy, with no lag to compensate for and no guessing at `+3`. It also keeps a presenting window free of transfers: a frame that nobody captures never touches the staging buffer, and the gates check exactly that.
:::

:::key
One capture procedure serves both hosts, so a lockstep window capture equals a headless one bit for bit in deferred. The renderer copies a frame only on request, and the capture waits for that frame's fence alone, reading host-cached staging memory. A window host captures deferred frames only, at its own size. Previews are area-downscaled JPEGs that travel beside the result. PNG files use a one-pass, four-band, run-length deflate.
:::

## Contracts

- Call `requestReadback()` before the `render()` whose pixels you want, and `readback()` after it. The span is valid until the next `render()` or `resize()`.
- `readback()` returns an empty span for a frame that was not rendered. Callers check the size before using it.
- The PNG writer's output depends only on the image. Its band count must never follow the core count.
- A window host restores its live post settings after a lockstep capture. A capture never changes what the player sees afterwards.
