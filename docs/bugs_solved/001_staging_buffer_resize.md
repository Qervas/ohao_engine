# Bug #001: Multi-Frame Staging Buffer Not Resized on Viewport Resize

**Date Solved:** 2026-01-17
**Severity:** Critical
**Symptom:** Viewport blinking/flickering in Godot editor

## Problem

When the OHAO viewport was resized, the multi-frame staging buffers (used for pixel readback) kept their original size while the framebuffer changed dimensions.

```
Initial:     Staging buffers = 64×64×4 bytes (256KB each)
After resize: Framebuffer = 1920×1080, staging still 64×64
Copy attempt: Trying to copy 8MB into 256KB buffer
Result:      Memory corruption, garbage pixels, blinking
```

The ring buffer architecture has 3 frames in flight, each with its own staging buffer. On resize:
- `m_pixelBuffer` was resized correctly
- `m_stagingBuffer` (legacy) was recreated
- `m_frameResources` staging buffers were NOT resized

## Root Cause

`OffscreenRenderer::resize()` did not call any method to resize the per-frame staging buffers in `FrameResourceManager`.

## Solution

1. Added `FrameResourceManager::resizeStagingBuffers(size_t newSize)` method:
   - Waits for GPU idle
   - Unmaps and destroys old staging buffers
   - Creates new staging buffers with correct size
   - Initializes to black (prevents garbage on first frames)

2. Updated `OffscreenRenderer::resize()` to call:
   ```cpp
   if (m_frameResources.isInitialized()) {
       m_frameResources.resizeStagingBuffers(width * height * 4);
       m_currentFrame = 0;  // Reset to avoid reading old-size data
   }
   ```

## Files Modified

- `src/renderer/frame/frame_resources.hpp` - Added `resizeStagingBuffers()` declaration
- `src/renderer/frame/frame_resources.cpp` - Added `resizeStagingBuffers()` implementation
- `src/renderer/offscreen/offscreen_renderer.cpp` - Call resize in `resize()` method

## Verification

After fix, viewport resize no longer causes blinking. Staging buffers are properly recreated at the new size.
