---
name: verify
description: Run OHAO's gates (unit suites, CLI and doc checks, GPU tests, validation, render goldens, the scene benchmark) and judge pass/fail. Use before every commit and whenever a change touches shared code.
---

# Verify OHAO

Every suite exits 0 on success and nonzero on any failure; that is the pass criterion.
Newer suites end with one JSON line `{"suite", "passed", "failed", "failures"}`. Build
first (the `build` skill), shaders included if `shaders/` changed.

## 1. CPU gates (seconds; run always)

```console
$ build/Release/runtime_tests.exe > /dev/null
$ build/Release/reflect_tests.exe > /dev/null
$ build/Release/scene_tests.exe > /dev/null
$ build/Release/api_tests.exe > /dev/null
$ build/Release/systems_tests.exe > /dev/null
$ build/Release/physics_backend_tests.exe > /dev/null
$ build/Release/audio_system_tests.exe > /dev/null
$ build/Release/env_cdf_test.exe > /dev/null
$ build/Release/denoise_parse_test.exe > /dev/null
$ build/Release/sobol_test.exe > /dev/null
$ python tests/cli/check_cli.py --cpu-only > /dev/null
$ python tests/reflect/check_scene_schema.py > /dev/null
```

## 2. GPU gates (a minute or two; when rendering, ECS, scene or API code changed)

```console
$ build/Release/engine_tests.exe > /dev/null   # gpu
$ build/Release/ecs_tests.exe > /dev/null   # gpu
$ build/Release/renderer_test.exe > /dev/null   # gpu
$ build/Release/instantiate_gpu_test.exe > /dev/null   # gpu
$ build/Release/scene_view_gpu_test.exe > /dev/null   # gpu
$ build/Release/restore_render_gpu_test.exe > /dev/null   # gpu
$ python tests/cli/check_cli.py > /dev/null   # gpu
$ python tests/validation/check_validation.py > /dev/null   # gpu
$ OHAO_VALIDATION=1 build/Release/ohao_game.exe --frames 600 --resize-at 200 > /dev/null   # gpu
```

## 3. Long gates (minutes; before committing rendering-adjacent or codec changes)

```console
$ python tests/golden/render_golden.py tests/golden/manifest.json > /dev/null   # gpu # slow
$ build/Release/scene_bench.exe --check tests/bench/scene_budgets.json > /dev/null   # slow
$ python tests/docs/check_docs.py --cpu-only > /dev/null   # slow
```

## Judging a failure

- **A golden** fails: run it alone (`--only NAME`) and look at the `.actual.png` written
  beside the golden. Never widen a tolerance. A change that perturbs the sample sequence
  moves every pixel by the noise; that is not proof of a regression, and `--update` is
  right only after you have looked.
- **Validation** reports messages: set `VK_LAYER_DUPLICATE_MESSAGE_LIMIT=10000` to see
  every report.
- **The bench** reports `OVER`: budgets change only by Frank's ruling; report the numbers.
- **The CLI or doc checks** show a stdout diff: if the change was deliberate (a new
  component changes `schemaHash`), rerun with `--update` (CLI) or fix the doc, and check
  that the diff is only what you meant.
- **Device lost (-4) everywhere** after a crash is a wedged GPU, not your change: reboot.
