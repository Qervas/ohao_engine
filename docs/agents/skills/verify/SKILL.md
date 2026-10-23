---
name: verify
description: Run OHAO's gates (unit suites, CLI and doc checks, GPU tests, validation, render goldens, the scene benchmark) and judge pass/fail. Use before every commit and whenever a change touches shared code.
---

# Verify OHAO

## The one command

```console
$ python tools/check.py   # slow
```

It builds (shaders first, at most 6 jobs) and runs every CTest test labelled `quick`,
`gpu` or `docs`; `--all` adds the goldens, the benchmark gate and the slow checks. Read
the last line it prints (also `build/check/check.json`): `ok`, then per test `ok`, the
suite's own `passed`/`failed`/`failures`, and for a failure the last 40 lines of its
output in `tail`. Exit 0 = pass, 1 = a failure, 2 = the environment (the `error` field
says what). The sections below run single suites by hand.

Every suite exits 0 on success and nonzero on any failure; that is the pass criterion.
Every suite ends with one JSON line `{"suite", "passed", "failed", "failures"}`. Build
first (the `build` skill) when running suites by hand.

## 1. CPU gates (seconds; run always)

```console
$ build/Release/runtime_tests.exe > /dev/null
$ build/Release/log_tests.exe > /dev/null
$ build/Release/reflect_tests.exe > /dev/null
$ build/Release/scene_tests.exe > /dev/null
$ build/Release/api_tests.exe > /dev/null
$ build/Release/rpc_tests.exe > /dev/null
$ build/Release/systems_tests.exe > /dev/null
$ build/Release/physics_backend_tests.exe > /dev/null
$ build/Release/audio_system_tests.exe > /dev/null
$ build/Release/force_generator_tests.exe > /dev/null
$ build/Release/env_cdf_test.exe > /dev/null
$ build/Release/denoise_parse_test.exe > /dev/null
$ build/Release/sobol_test.exe > /dev/null
$ python tests/cli/check_cli.py --cpu-only > /dev/null
$ python tests/reflect/check_scene_schema.py > /dev/null
$ python tests/check/check_tests.py > /dev/null
$ python tests/golden/test_golden_diff.py > /dev/null
$ python tests/doctor/check_doctor.py > /dev/null
$ python tests/serve/check_serve.py > /dev/null
```

## 2. GPU gates (a minute or two; when rendering, ECS, scene or API code changed)

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ build/Release/ecs_tests.exe > /dev/null   # gpu
$ build/Release/renderer_test.exe > /dev/null   # gpu
$ build/Release/instantiate_gpu_test.exe > /dev/null   # gpu
$ build/Release/scene_view_gpu_test.exe > /dev/null   # gpu
$ build/Release/restore_render_gpu_test.exe > /dev/null   # gpu
$ build/Release/deferred_determinism_gpu_test.exe > /dev/null   # gpu
$ python tests/cli/check_cli.py > /dev/null   # gpu
$ python tests/validation/check_validation.py > /dev/null   # gpu
$ python tests/doctor/check_doctor.py --gpu > /dev/null   # gpu
$ OHAO_VALIDATION=1 build/Release/ohao_game.exe --frames 600 --resize-at 200 > /dev/null   # gpu
```

## 3. Long gates (minutes; before committing rendering-adjacent or codec changes)

```console
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
$ build/Release/scene_bench.exe --check tests/bench/scene_budgets.json > /dev/null   # slow
$ build/Release/serve_bench.exe --check tests/bench/serve_budgets.json > /dev/null   # slow
$ python tests/docs/check_docs.py --cpu-only > /dev/null   # slow
```

## Judging a failure

- **A golden** fails: look at `build/golden_diff/<name>.heat.png` (where and how much)
  and `<name>.crop.png` (expected | actual | heat of the worst region), or read the
  `regions` in `--json`; run it alone with `--only NAME`. Never widen a tolerance. The deferred scenes are exact: any
  difference there is a change. In the path-traced scenes, a change that perturbs the
  sample sequence moves every pixel by the noise; that is not proof of a regression, and
  `--update` is right only after you have looked.
- **Validation** reports messages: set `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` to see
  every report.
- **The bench** reports `OVER`: budgets change only by Frank's ruling; report the numbers.
  `serve_bench` exiting 2 (`noisy`) means the machine was loaded (it lists the busy
  processes) or slower than its calibration: `tools/check.py` reruns it at most twice
  and lists every attempt; report a run still noisy as noisy, not as a pass or a
  regression. A budget is met after five consecutive passes.
- **Validation** messages are stderr lines `W validation: <VUID> | <text>`; run with
  `OHAO_LOG_LEVEL=debug` for the engine's detail lines around them.
- **The CLI, serve or doc checks** show a stdout diff: if the change was deliberate (a
  new component or method changes `schemaHash`), rerun with `--update` (`check_cli.py`,
  `check_serve.py`) or fix the doc, and check that the diff is only what you meant.
- **Device lost (-4) everywhere** after a crash is a wedged GPU, not your change: reboot.
  `build/Release/ohao doctor` says so (`gpu.wedged`), and names any other environment
  problem (stale SPIR-V, missing assets, conda's cmake, missing Python modules) with its fix.
