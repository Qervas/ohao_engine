---
name: add-component
description: Add a new ECS component to OHAO that scene files, the API and agents can use - struct, registration with docs/units/limits, per-type writer, tests, corpus case, goldens. Use when a feature needs new per-entity data.
---

# Add a component

The full recipe, with the reasoning, is "Recipe: add a component" in
`docs/ARCHITECTURE.md`; it was followed end to end for `Spin`. Use `Spin` as the worked
example: these are the places a saved component touches (the spin system files are
there because Spin also has a system).

```console
$ git grep -l --untracked "Spin" -- ohao tests/scene tests/scenes tests/systems
ohao/ecs/components.hpp
ohao/ecs/scene_types.cpp
ohao/ecs/scene_types.hpp
ohao/ecs/scene_writers.cpp
ohao/ecs/scene_writers.hpp
ohao/ecs/spin_system.cpp
ohao/ecs/spin_system.hpp
ohao/ecs/world.cpp
tests/scene/golden/engine_catalogue.json
tests/scene/golden/engine_scene.schema.json
tests/scene/scene_tests.cpp
tests/scenes/invalid/19_spin.errors.json
tests/scenes/invalid/19_spin.scene.json
tests/systems/systems_tests.cpp
```

## Steps

1. **Struct** in `ohao/ecs/components.hpp`. Default member initializers are the defaults
   files omit.
2. **Register** in `registerSceneTypes` (`ohao/ecs/scene_types.cpp`): `.component()`,
   `.host(makeComponentOps<T>())`, docs on the type and every field, a unit from
   `knownUnits()` (`ohao/reflect/registry.cpp`; add one if needed) or `.unitless()`,
   limits. `.singleton()` / `.runtime()` where they apply.
3. **Writer** for a saved component: `writeFields` in `ohao/ecs/scene_writers.hpp/.cpp`,
   fields in registration order, each guarded by `!omit || !same(...)`.
4. **Tests** in `tests/scene/scene_tests.cpp`: `complete<T>` in t01, the name in
   `t01_authoring_and_runtime_split`, random values in `buildRandomWorld`.
5. **Corpus case**: `tests/scenes/invalid/NN_<name>.scene.json` with realistic mistakes
   and `NN_<name>.errors.json` with each expected `code` and `path`.
6. **Build** (`build` skill), then run the scene tests. Only the catalogue golden may
   fail; read its diff, then regenerate (`reflect_tests --update-golden` only if you
   added a unit):

```console
$ build/Release/scene_tests.exe --update-golden > /dev/null   # slow
$ build/Release/reflect_tests.exe --update-golden > /dev/null   # slow
$ python tests/cli/check_cli.py --cpu-only --update > /dev/null   # slow
```

   `git diff` the goldens: the new type, a new unit if you added one, and `schemaHash`,
   nothing else.
7. **Docs**: the component table in `docs/agents/ohao-cli.md`.
8. **Verify** (`verify` skill): the CPU gates at least.

Changing an existing saved component: bump `.version(n)`, add `.upgrade()`, and add an
old-version file to `tests/scenes/v1/` checked in `t08_v1_corpus_loads_with_expected_values`.
