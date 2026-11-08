---
name: add-component
description: Add a new ECS component to OHAO that scene files, the API and agents can use - struct, registration with docs/units/limits, per-type writer, docs. Use when a feature needs new per-entity data.
---

# Add a component

The full recipe, with the reasoning, is "Recipe: add a component" in `docs/ARCHITECTURE.md`. Use `Spin`
as the worked example: `git grep -l "Spin" -- ohao` lists the places a saved component touches.

## Steps

1. **Struct** in `ohao/ecs/components.hpp`. Default member initializers are the defaults files omit.
2. **Register** in `registerSceneTypes` (`ohao/ecs/scene_types.cpp`): `.component()`,
   `.host(makeComponentOps<T>())`, docs on the type and every field, a unit from `knownUnits()`
   (`ohao/reflect/registry.cpp`; add one if needed) or `.unitless()`, limits. `.singleton()` /
   `.runtime()` where they apply.
3. **Writer** for a saved component: `writeFields` in `ohao/ecs/scene_writers.hpp/.cpp`, fields in
   registration order, each guarded by `!omit || !same(...)`.
4. **Docs**: the component table in `docs/agents/ohao-cli.md`.
5. **Check it by hand**: `build/Release/ohao schema -o build/schema` writes the schema (`scene.schema.json`)
   with the type, its docs and units; a scene that uses it round-trips (`ohao fmt` it, load it, the same
   `world.hash` after a save and reload).
6. **Verify** (`verify` skill). A new saved type hashes its name even with no instances, so it moves every
   `world.hash` in `tests/serve/expected`: look at the diff, then `python tests/serve/check_serve.py --update`.
   `replay_test` holds the gameplay replay with the new type in the hash.

Changing an existing saved component: bump `.version(n)` and add an `.upgrade()`, and load an old-version
file (`tests/scenes/v1/`) to see it upgrade.
