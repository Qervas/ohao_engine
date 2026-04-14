# Animation System Infrastructure Redesign

## Status: DESIGN PHASE
**Author**: Claude + Frank  
**Date**: 2026-04-14  
**Branch**: `animation`

---

## 1. Problem Statement

The current animation system works but is fragile. Every new model requires manual tweaking:
- Blender conversion (GLB → FBX) for animation to work
- Hardcoded material roughness by name ("skin" → 0.9)
- Camera/light setup tuned for one model size
- Two separate animation code paths (ufbx vs GLTF native) with different bugs
- Dynamic BLAS pipeline has 5+ files of complex synchronization code

**Goal**: Load ANY animated model (FBX, GLB, GLTF) and render it correctly with zero manual intervention.

---

## 2. Current Architecture (What Exists)

```
Model Loading:
  OBJ  → model.cpp (native parser)
  GLTF → model_gltf.cpp (tinygltf) — skeleton works, animation BROKEN
  FBX  → model_fbx.cpp (ufbx) — skeleton + animation WORKS

Animation:
  ufbx path:  ufbx_evaluate_scene() → jointMatrices (correct for FBX)
  GLTF path:  AnimationClip::sample() → computeJointMatrices() (BROKEN)
  
Skinning:
  Raster: gbuffer_skinned.vert (bone UBO, set 1)
  Shadow: shadow_csm_skinned.vert (bone UBO, set 0)
  RT:     skinning.comp → GPU compute → skinned position buffer → BLAS

Material:
  FBX:  name-based roughness heuristic (hardcoded)
  GLTF: reads PBR values from tinygltf (works)
  
RT Pipeline:
  Static BLAS (initial build) + Dynamic BLAS (per-frame) + TLAS rebuild
  Material albedo buffer must be synced with TLAS instance order
  Driver workaround for RTX 5070 mask bug
```

### Known Bugs
1. GLTF skeleton: `computeJointMatrices()` doesn't produce identity at bind pose
2. ufbx can't read GLB/GLTF — only FBX
3. Material roughness hardcoded by name
4. RTGI material buffer gets stale when TLAS instance order changes
5. RTX 5070 ignores `traceRayEXT` cull mask
6. `updateSceneBuffers()` called twice (setScene + explicit) — double registration

---

## 3. Redesigned Architecture

### 3.1 Unified Model Loader

```
                    loadModel(path)
                         │
              ┌──────────┼──────────┐
              ▼          ▼          ▼
          .fbx/.FBX   .glb/.gltf   .obj
              │          │          │
           ufbx       Assimp     native
              │          │          │
              └──────────┼──────────┘
                         ▼
              UnifiedModelData {
                vertices[]
                indices[]
                materials[]     ← PBR from model, NOT heuristic
                skeleton?       ← node tree + joints
                animations[]    ← clips with channels
                textures[]      ← embedded or resolved paths
              }
```

**Key change**: Assimp handles GLB/GLTF (not tinygltf). ufbx handles FBX. Both produce the same `UnifiedModelData`. No more two separate skeleton paths.

**File**: `ohao/scene/asset/model_loader.hpp` (new)

### 3.2 Single Animation Backend

```
AnimationEvaluator {
  // For ufbx models: uses ufbx_evaluate_scene()
  // For Assimp models: walks node tree, same algorithm
  
  evaluate(float time) → std::vector<glm::mat4> jointMatrices
}
```

Both paths produce `jointMatrices` in the same format. The skeleton stores:
- Full node tree (for correct intermediate node handling)
- Per-joint inverse bind matrices  
- Animation data (keyframes OR ufbx scene pointer)

**Key change**: ONE `evaluate()` method, TWO internal implementations.

**File**: `ohao/animation/animation_evaluator.hpp` (new)

### 3.3 Material System

```
MaterialData {
  vec3 baseColor
  float roughness      ← from model PBR, NOT name heuristic
  float metallic       ← from model PBR
  bool hasSkin = false  ← auto-detected from texture/name
  
  TextureHandle albedo
  TextureHandle normal
  TextureHandle roughnessMetallic
  TextureHandle emissive
  TextureHandle ao
}
```

**Rules**:
1. Always use model's PBR values first
2. If model has NO PBR data (roughness=0, metallic=0): apply sensible defaults (roughness=0.5)
3. Name-based heuristics ONLY as last resort, and logged as warnings
4. SSS skin detection uses material flag, not roughness+albedo hack

**File**: `ohao/scene/component/material_component.hpp` (modified)

### 3.4 Simplified RT Pipeline

```
AnimatedRTManager {
  // Called once per frame, handles everything:
  // 1. Compute skinning (all animated meshes)
  // 2. Create fresh BLASes
  // 3. Rebuild TLAS with correct instance order
  // 4. Update material buffer to match
  // 5. Apply driver workarounds
  
  update(scene, commandBuffer)
}
```

**Key change**: ONE class manages the entire animated RT pipeline. No more scattered code across render_dispatch.cpp, rt_build.cpp, gpu_skinning.cpp, deferred_renderer.cpp.

**File**: `ohao/render/rt/animated_rt_manager.hpp` (new)

### 3.5 Auto-Framing

```
SceneFramer {
  // Analyzes model bounds, sets camera + lights automatically
  // Works for ANY model size (mm, cm, m, arbitrary units)
  
  frame(model, camera, lights, roomSize)
}
```

**File**: `ohao/render/camera/scene_framer.hpp` (new)

---

## 4. Implementation Phases

### Phase 1: Unified Loader (Assimp for GLB + ufbx for FBX)
**Goal**: Any animated model loads and plays without conversion  
**Estimate**: 2-3 hours  
**Test**: `walking.glb` plays animation directly (no Blender conversion)

#### Tasks:
1. Create `model_loader.cpp` — tries ufbx first, falls back to Assimp
2. Port the Assimp skeleton/animation code from old `model_fbx.cpp` (pre-ufbx)
3. Assimp path builds node tree (same as ufbx) for correct joint matrices
4. Assimp path uses `ufbx_evaluate_scene` equivalent: walk node tree per frame
5. Remove the `loadFromFBX`/`loadFromGLTF` split in model_viewer
6. Test: Fox.glb, walking.glb, walking_woman.fbx all play animation

**Verification**:
```bash
./build/model_viewer model.glb output.png 1 deferred  # any GLB
./build/model_viewer model.fbx output.png 1 deferred  # any FBX
# Both should show animated pose, correct textures, no manual tweaking
```

### Phase 2: Material System Cleanup
**Goal**: PBR values from model data, not name heuristics  
**Estimate**: 1-2 hours  
**Test**: No hardcoded roughness values in the codebase

#### Tasks:
1. Read PBR roughness/metallic from ufbx `mat->pbr.roughness.value_real`
2. Read PBR from Assimp `AI_MATKEY_ROUGHNESS_FACTOR`
3. Remove ALL name-based roughness overrides from model_fbx.cpp
4. Add sensible defaults ONLY when model has zero values (roughness=0→0.5)
5. SSS skin detection: use material flag or texture name, not roughness range
6. Log warnings when falling back to defaults

**Verification**:
```bash
# Render with model's actual PBR values
# Compare DamagedHelmet (has PBR) vs walking woman (no PBR)
# Helmet should look identical to before
# Woman should look reasonable with default values
```

### Phase 3: AnimatedRTManager
**Goal**: One class handles all animated RT complexity  
**Estimate**: 2-3 hours  
**Test**: RT shadows + GI work on animated models without wall projection

#### Tasks:
1. Create `AnimatedRTManager` class
2. Move compute skinning code from render_dispatch.cpp
3. Move BLAS creation/destruction from render_dispatch.cpp
4. Move TLAS rebuild + material buffer sync from render_dispatch.cpp
5. Move driver workaround (closest-hit alpha check) documentation
6. Expose single `update(cmd)` method
7. render_dispatch.cpp calls `m_animatedRT->update(cmd)` — one line

**Verification**:
```bash
# Same visual output as current, but cleaner code
# render_dispatch.cpp should be < 100 lines for animation handling
```

### Phase 4: Auto-Framing
**Goal**: Any model renders at correct size with good camera/lights  
**Estimate**: 1 hour  
**Test**: Fox, DamagedHelmet, walking woman, walking man all frame correctly

#### Tasks:
1. Compute model AABB after loading
2. Scale model to fit a normalized size (e.g., height = 8 units)
3. Position camera at 2x model height distance
4. Place key light at 1.5x model height, 45 degrees up
5. Place fill light opposite side, 0.5x intensity
6. Adjust room size to 3x model bounds

**Verification**:
```bash
# All models should be visible, well-lit, properly framed
# No manual camera/light adjustment needed
```

### Phase 5: Cleanup & Polish
**Goal**: Remove dead code, fix double-registration, update docs  
**Estimate**: 1-2 hours

#### Tasks:
1. Remove old `loadFromGLTF` skeleton code (broken, replaced by Assimp)
2. Remove double `updateSceneBuffers()` call
3. Remove `AnimationClip::sample()` GLTF path (replaced by node tree)
4. Update CLAUDE.md with new architecture
5. Update memory with current state
6. Clean up debug prints throughout codebase

---

## 5. File Map

### New Files
```
ohao/scene/asset/model_loader.hpp      # Unified loader (tries ufbx → Assimp)
ohao/scene/asset/model_loader.cpp      # Implementation
ohao/animation/animation_evaluator.hpp  # Single animation backend
ohao/animation/animation_evaluator.cpp  # ufbx + Assimp evaluation
ohao/render/rt/animated_rt_manager.hpp  # Simplified RT animation pipeline
ohao/render/rt/animated_rt_manager.cpp  # Compute skin + BLAS + TLAS + materials
ohao/render/camera/scene_framer.hpp     # Auto camera/light/room setup
ohao/render/camera/scene_framer.cpp     # Implementation
```

### Modified Files
```
examples/model_viewer.cpp              # Uses unified loader + auto-framing
ohao/gpu/vulkan/render_dispatch.cpp    # Delegates to AnimatedRTManager
ohao/gpu/vulkan/rt_build.cpp           # Simplified (no animation-specific code)
ohao/scene/asset/model_fbx.cpp         # ufbx-only, no Assimp code
ohao/scene/asset/model.hpp             # Add loadModel() unified method
ohao/render/deferred/gbuffer_pass.cpp  # Uses unified skeleton data
```

### Deleted Files (Phase 5)
```
# None deleted — old code stays as fallback until new code is verified
# Then remove dead paths in cleanup phase
```

---

## 6. Testing Matrix

| Model | Format | Skeleton | Animation | Textures | Expected |
|-------|--------|----------|-----------|----------|----------|
| DamagedHelmet | GLB | No | No | PBR | Static, correct PBR |
| Fox | GLB | Yes (24j) | Yes (3 clips) | Color | Animated walk |
| walking.glb | GLB | Yes (65j) | Yes (1 clip) | PBR+Normal | Animated walk |
| walking_woman | FBX | Yes (120j) | Yes (ufbx) | Diffuse | Animated walk |
| BrainStem | GLB | Yes (18j) | Yes (1 clip) | Color | Animated |
| cornell_box | OBJ | No | No | None | Static |

Each phase must pass ALL models in this matrix before proceeding.

---

## 7. Risk Register

| Risk | Impact | Mitigation |
|------|--------|-----------|
| Assimp GLB skeleton produces wrong matrices | High | Test against ufbx FBX output for same model |
| Material PBR removal makes some models look worse | Medium | Keep name heuristics as `--legacy-materials` flag |
| AnimatedRTManager refactor breaks existing renders | High | A/B test: old vs new code, pixel-compare |
| Auto-framing doesn't work for very small/large models | Low | Clamp scale to reasonable range |
| Double updateSceneBuffers removal causes crash | Medium | Remove one call at a time, test between |

---

## 8. Success Criteria

After all 5 phases:
1. `./build/model_viewer model.glb out.png 1 deferred` works for ANY model
2. No Blender conversion needed
3. No hardcoded material names in the codebase
4. render_dispatch.cpp animation section is < 5 lines
5. All 6 test models render correctly
6. Walking animation plays with correct textures
7. RT shadows + GI work on animated models (no wall projection)
