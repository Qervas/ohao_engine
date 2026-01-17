# Bug #002: Shadow Pass Using Wrong Descriptor Set in Multi-Frame Mode

**Date Solved:** 2026-01-17
**Severity:** High
**Symptom:** Shadows rotated 90 degrees on XOY plane

## Problem

Shadows appeared rotated 90 degrees from their correct orientation. Instead of casting in the light direction, shadows appeared as horizontal lines across objects.

## Root Cause

In multi-frame rendering mode, the shadow pass and main render pass were using different descriptor sets:

```cpp
// Shadow pass (WRONG - used legacy descriptor set)
vkCmdBindDescriptorSets(..., &m_descriptorSet, ...);

// Main pass (CORRECT - used per-frame descriptor set)
vkCmdBindDescriptorSets(..., &frame.descriptorSet, ...);
```

Each descriptor set contains a light uniform buffer with the `lightSpaceMatrix`. When these don't match:
- Shadow map is rendered using matrix A (from `m_descriptorSet`)
- Shadow sampling uses matrix B (from `frame.descriptorSet`)
- Result: Shadow coordinates are transformed incorrectly

The 90° rotation occurred because the light space matrices had different orientations due to being updated at different times or containing stale data.

## Solution

Changed `renderShadowPass()` to accept the descriptor set as a parameter:

```cpp
// Before
void renderShadowPass(VkCommandBuffer cmd);

// After
void renderShadowPass(VkCommandBuffer cmd, VkDescriptorSet descriptorSet);
```

Updated callers to pass the correct descriptor set:

```cpp
// Multi-frame path
renderShadowPass(cmd, frame.descriptorSet);

// Legacy path
renderShadowPass(m_commandBuffer, m_descriptorSet);
```

## Files Modified

- `src/renderer/offscreen/offscreen_renderer.hpp` - Updated function signature
- `src/renderer/offscreen/offscreen_scene_render.cpp` - Accept and use descriptor set parameter
- `src/renderer/offscreen/offscreen_renderer.cpp` - Pass correct descriptor set from both render paths

## Key Insight

In multi-frame rendering, ALL passes that access shared uniform data (camera, lights) must use the SAME per-frame descriptor set. Mixing legacy and per-frame descriptor sets causes data inconsistencies because:
- Per-frame buffers are updated each frame with current data
- Legacy buffers may contain stale data from previous frames

## Verification

After fix, shadows are correctly oriented and match the light direction. The shadow map and shadow sampling now use consistent light space matrices.
