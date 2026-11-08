"""Which tests a change reaches: the rules behind `python tools/check.py --changed [REF]`.

A changed path is matched against RULES top to bottom; every rule that matches adds its targets
(test names, or `label:<label>` for every test with that label). A path no rule matches selects
the whole default set (quick, gpu, docs): an unmapped change is never tested by nothing. Paths that
cannot change a test's result (docs other than the ones `check_docs` runs, the frozen site) select
nothing.

The rules are by reach, not by directory: a saved component moves the schema hash, so the files
that define saved types select every suite that records one.
"""
from __future__ import annotations

import fnmatch
import subprocess
from pathlib import Path

# Files that define what a scene file, the schema or world.hash holds.
SCHEMA = ["reflect_tests", "scene_tests", "api_tests", "check_cli_cpu", "check_cli", "check_scene_schema",
          "check_serve", "check_docs"]

PHYSICS = ["physics_backend_tests", "level_physics_tests", "combat_tests", "api_tests", "ecs_tests"]

# (glob, targets). fnmatch globs on '/'-separated paths; '*' crosses directories.
RULES: list[tuple[str, list[str]]] = [
    # Nothing a test reads.
    ("site/*", []),
    ("docs/specs/*", []), ("docs/plans/*", []), ("docs/changes/*", []), ("docs/images/*", []),
    ("docs/media/*", []), ("docs/bugs_solved/*", []), ("docs/ROADMAP.md", []), ("docs/render_pipelines.md", []),
    ("README.md", []), ("CHANGELOG.md", []), ("LICENSE", []), (".gitignore", []),
    # Documents whose commands check_docs runs.
    ("AGENTS.md", ["check_docs"]), ("CLAUDE.md", []), ("docs/ARCHITECTURE.md", ["check_docs"]),
    ("docs/agents/*", ["check_docs"]), (".claude/skills/*", ["check_docs"]),
    # The gate itself.
    ("tools/check.py", ["check_tests"]), ("tools/check_changed.py", ["check_tests"]), ("tests/check/*", ["check_tests"]),
    # Saved types: the schema and every hash.
    ("ohao/ecs/components.hpp", SCHEMA), ("ohao/ecs/scene_types.cpp", SCHEMA), ("ohao/ecs/scene_writers.*", SCHEMA),
    ("ohao/ecs/scene_io.*", SCHEMA), ("ohao/ecs/world_hash.*", SCHEMA), ("ohao/reflect/*", SCHEMA),
    ("shaders/shared/layout.json", ["check_layout", "label:gpu"]),
    # Physics, levels, combat.
    ("ohao/physics/*", PHYSICS + ["physics_bench"]),
    ("ohao/ecs/physics_scene*", PHYSICS + ["check_cli", "check_serve", "physics_bench"]),
    ("ohao/level/*", PHYSICS + ["check_level_assets", "check_cli"]),
    ("ohao/ecs/combat*", ["combat_tests", "api_tests", "audio_schedule_tests", "vfx_tests", "physics_bench"]),
    ("ohao/ecs/enemy_*", ["combat_tests", "anim_tests", "api_tests"]),
    ("ohao/ecs/view_model.*", ["combat_tests", "light_emitter_gpu_test"]),
    ("ohao/ecs/arms_anim.*", ["combat_tests", "anim_tests"]),
    # Animation, audio, vfx.
    ("ohao/anim/*", ["anim_tests", "combat_tests", "skinning_gpu_test"]),
    ("ohao/ecs/animation.*", ["anim_tests", "combat_tests", "skinning_gpu_test", "cull_parity_gpu_test"]),
    ("ohao/audio/*", ["audio_system_tests", "audio_schedule_tests", "grenade_spatial", "audio_bench"]),
    ("ohao/ecs/audio_schedule.*", ["audio_schedule_tests", "api_tests", "grenade_spatial"]),
    ("ohao/vfx/*", ["vfx_tests", "vfx_gpu_test"]),
    ("ohao/ecs/vfx*", ["vfx_tests", "vfx_gpu_test", "api_tests"]),
    # The rest of the ECS.
    ("ohao/ecs/*", ["ecs_tests", "scene_tests", "api_tests", "systems_tests", "level_physics_tests", "combat_tests",
                    "check_cli_cpu"]),
    # The agent surface.
    ("ohao/api/*", ["api_tests", "check_cli_cpu", "check_cli", "check_serve", "e2e_mcp"]),
    ("ohao/rpc/*", ["rpc_tests", "check_serve", "e2e_mcp"]),
    ("tools/ohao/*", ["check_cli_cpu", "check_cli", "check_serve", "check_doctor"]),
    ("ohao/runtime/*", ["runtime_tests", "check_live", "ohao_game_presentation"]),
    ("ohao/core/log.*", ["log_tests", "reflect_tests"]),
    # The renderer: every GPU suite.
    ("ohao/render/*", ["label:gpu"]), ("ohao/gpu/*", ["label:gpu"]), ("shaders/*", ["label:gpu", "check_doctor"]),
    ("ohao/asset/*", ["label:gpu", "scene_tests", "anim_tests"]),
    ("ohao/render/material/*", ["material_tests", "openpbr_tables_current", "label:gpu"]),
    # Content.
    ("assets/scenes/*", ["check_cli_cpu", "check_cli", "check_scene_schema"]),
    ("assets/levels/*", ["check_level_assets", "level_physics_tests"]),
    ("assets/characters/*", ["check_level_assets", "anim_tests", "combat_tests"]),
    ("assets/anim/*", ["anim_tests", "combat_tests"]),
    ("assets/audio/*", ["audio_system_tests", "audio_schedule_tests"]),
    ("assets/vfx/*", ["vfx_tests", "vfx_gpu_test", "check_level_assets"]),
    # Tests and their expectations: the suite they belong to.
    ("tests/physics/*", ["physics_backend_tests"]), ("tests/level/*", ["level_physics_tests", "check_level_assets"]),
    ("tests/combat/*", ["combat_tests"]), ("tests/anim/*", ["anim_tests", "skinning_gpu_test"]),
    ("tests/audio/*", ["audio_system_tests", "audio_schedule_tests"]), ("tests/vfx/*", ["vfx_tests", "vfx_gpu_test"]),
    ("tests/scene/*", ["scene_tests"]), ("tests/reflect/*", ["reflect_tests", "check_scene_schema"]),
    ("tests/api/*", ["api_tests"]), ("tests/rpc/*", ["rpc_tests"]), ("tests/runtime/*", ["runtime_tests"]),
    ("tests/log/*", ["log_tests"]), ("tests/ecs/*", ["ecs_tests"]), ("tests/systems/*", ["systems_tests"]),
    ("tests/engine/*", ["engine_tests"]), ("tests/material/*", ["material_tests"]),
    ("tests/cli/*", ["check_cli_cpu", "check_cli"]), ("tests/serve/*", ["check_serve", "check_live", "e2e_mcp"]),
    ("tests/doctor/*", ["check_doctor", "check_doctor_gpu"]), ("tests/layout/*", ["check_layout"]),
    ("tests/golden/*", ["test_golden_diff", "render_golden"]), ("tests/validation/*", ["check_validation"]),
    ("tests/renderer/*", ["label:gpu"]), ("tests/docs/*", ["check_docs"]),
    ("tests/bench/physics_budgets.json", ["physics_bench"]), ("tests/bench/render_budgets.json", ["render_bench"]),
    ("tests/bench/*", ["check_tests"]),
    ("tools/bench/physics_bench.cpp", ["physics_bench"]), ("tools/bench/render_bench.cpp", ["render_bench"]),
    ("tools/audio/*", ["grenade_spatial", "audio_bench"]), ("tools/gen_layout.py", ["check_layout"]),
    ("tools/level/*", ["check_level_assets"]), ("tools/anim/*", ["check_level_assets"]),
    ("tools/vfx/*", ["check_level_assets"]),
    # The path tracer's sampling modules and their CPU twins.
    ("shaders/sampling/*", ["sobol_test", "vmf_test"]), ("tools/sampling/*", ["sobol_test", "vmf_test"]),
    ("tests/renderer/sobol_test.cpp", ["sobol_test"]), ("tests/renderer/vmf_test.cpp", ["vmf_test"]),
    # Caustics (sub-project 24): the casters on the CPU, the pass on the GPU.
    ("ohao/render/rt/caustic_*", ["caustic_casters_test", "check_caustics", "check_caustics_showcase"]),
    ("tests/renderer/caustic_casters_test.cpp", ["caustic_casters_test"]),
    # Caustics step 2: the manifold solver and its CPU twin, the casters' triangles on the GPU.
    ("shaders/rt/manifold*", ["manifold_tests", "check_caustics"]), ("tools/caustics/*", ["manifold_tests"]),
    ("tests/renderer/manifold_tests.cpp", ["manifold_tests"]),
    ("ohao/render/rt/manifold_*", ["caustic_casters_test", "check_caustics", "check_caustics_showcase"]),
    # check_caustics is `slow` (out of label:gpu): the path tracer's own files and its test select it by name.
    ("shaders/rt/path_tracer.slang", ["check_caustics"]), ("shaders/rt/pt_hit.slang", ["check_caustics"]),
    ("shaders/rt/caustics.slang", ["check_caustics"]), ("ohao/render/rt/path_tracer*", ["check_caustics"]),
    ("tests/renderer/check_caustics.py", ["check_caustics"]),
    ("ohao/render/rt/pipeline_disk_cache.*", ["pipeline_cache_files_test", "label:gpu"]),
    ("tests/renderer/pipeline_cache_files_test.cpp", ["pipeline_cache_files_test"]),
]


def changed_paths(root: Path, ref: str) -> list[str]:
    """Tracked files that differ from `ref` (staged or not), and untracked ones not ignored."""
    def git(*args: str) -> list[str]:
        r = subprocess.run(["git", *args], capture_output=True, text=True, cwd=root)
        if r.returncode != 0:
            raise RuntimeError(f"git {' '.join(args)}: {r.stderr.strip()}")
        return [l for l in r.stdout.splitlines() if l]
    return sorted(set(git("diff", "--name-only", ref)) | set(git("ls-files", "--others", "--exclude-standard")))


def select(paths: list[str], labels_of: dict[str, list[str]], default_labels: list[str], root: Path | None = None):
    """(test names, why: {path: targets or 'unmapped'}). Unknown names in the rules are dropped. With
    `root`, a deleted file under tests/ selects only check_tests (a removed suite cannot fail; its
    registration can)."""
    names: set[str] = set()
    why: dict[str, object] = {}
    unmapped = False
    for p in paths:
        if root is not None and p.startswith("tests/") and not (root / p).exists():
            why[p] = ["check_tests"]
            names.add("check_tests")
            continue
        hit = [targets for glob, targets in RULES if fnmatch.fnmatch(p, glob)]
        if not hit:
            unmapped = True
            why[p] = "unmapped"
            continue
        targets = sorted({t for ts in hit for t in ts})
        why[p] = targets
        for t in targets:
            if t.startswith("label:"):
                lab = t[len("label:"):]
                names |= {n for n, ls in labels_of.items() if lab in ls}
            else:
                names.add(t)
    if unmapped:
        names |= {n for n, ls in labels_of.items() if any(l in default_labels for l in ls)}
    return sorted(n for n in names if n in labels_of), why
