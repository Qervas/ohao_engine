# The source tree's structure: dead physics gone, modules named for what they hold

**Spec:** none (Frank, 2026-11-02: "a good project needs a good structure"; the review is in the session that made
these commits). **Status:** done, between sub-project 13's steps 1 and 2.

## 1. Summary

Four mechanical changes, one commit each, every one built and tested before the next; plus `tools/check.py --changed`,
which runs only the tests a change reaches (Frank: a small change should not pay for the whole gate).

## 2. What changed

| Commit | Change | Lines |
|---|---|---|
| `feat(check)` | `check.py --changed [REF]`: `tools/check_changed.py` maps changed paths to suites by reach (a saved type's files select every suite that records the schema hash; renderer and shader files the `gpu` label; specs and the frozen site nothing; an unmapped path the default set; a deleted test only `check_tests`) | +220 |
| `refactor(physics)` | the pre-Jolt physics deleted: forces, a rigid body, shapes, materials, profiles, the force debugger, `PhysicsWorld`, reached only by their own tests; `tests/force_generators`, `tests/python` (off), `tests/shadow_system` (off, no longer compiled). The live code's one use, `PhysicsWorldConfig`, is the backend's `BackendConfig` | −10,349 |
| `refactor(asset)` | `ohao/scene` (only the model loaders, in `scene/asset/`) is `ohao/asset`, target `ohao_asset`, includes `"asset/..."` | ±75 |
| `refactor(render)` | `VulkanRenderer` and its dispatch, device, environment and pipeline setup moved from `gpu/vulkan/` to `render/renderer/vulkan_renderer*` in `ohao_renderer`. `ohao_gpu_vulkan` had compiled it while `ohao_renderer` linked `ohao_gpu_vulkan`: the low layer included the high one's headers (the path tracer, frame resources). `gpu/vulkan` is now the Vulkan layer under the renderer | ±53 |
| `refactor(ecs)` | `physics_scene.cpp` (2,041 lines, every `Impl` member defined in the class) split: `physics_scene_impl.hpp` (state and declarations, private), `physics_scene.cpp` (bodies, sync, snapshots, the public API, 839), `_joints.cpp` (194), `_step.cpp` (144), `_players.cpp` (611). Moved by a script, not retyped | +2,500 / −2,041 |

ARCHITECTURE.md's module table (the renderer's library, `asset/`, the physics backend's role) and the README's subsystem
map (stale since the Godot era) are measured from the tree.

## 3. Results

- Each part ran the suites it reaches (`--changed`'s rules, by hand before the tool covered every path): the physics
  removal 7 suites (physics, engine, ecs, combat, api, check_tests), the rename 3, the renderer move 7 (GPU sample, the
  CLI's captures), the split 10 (every replay suite and the CLI and serve `world.hash` expectations: bit for bit).
- Closing `tools/check.py`: section 5.

## 4. Known gaps

- `ohao/ecs` still mixes the ECS core, subsystem bridges and gameplay (combat, enemy AI, the HUD, view model, audio
  schedule); gameplay moves to a game module in roadmap row 14.
- `.claude/skills/verify/SKILL.md` (installed; sessions cannot write there) still lists `force_generator_tests`; the
  source copy in `docs/agents/skills/verify/` is fixed. Reinstall the skills (`docs/agents/skills/README.md`).
- `tests/scenes`, `tests/reference_scenes` and `tests/assets` overlap in purpose; not merged here.

## 5. Gate

Closing `tools/check.py`: 56 of 57 (one suite fewer: `force_generator_tests` is gone). The one failure is
`check_docs` on `.claude/skills/verify/SKILL.md:22`, the installed skill's `force_generator_tests` line (section 4);
it passes once the skills are reinstalled from `docs/agents/skills/`.
