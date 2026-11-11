# Step 5: assets (parts a to i)

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.8 and 5 (step 5); plan
`docs/plans/2026-11-11-step5-assets.md` (each part's As-built notes hold its detail). **Status:** 5a-5i built,
2026-11-11, on `step3-engine-game-boundary`, not pushed.

## 1. Summary

The engine loads cooked data only. A source file (a glTF, its buffers and images) is cooked the first time a host
asks for it, into files named by a hash of what they were made from, in `<root>/.ohao/cooked/`; after that the
engine maps those files and copies them to the GPU. Textures are block-compressed with their mips. What a scene
loaded is given back when the scene goes. A load is submitted to the GPU and not waited for.

## 2. What was built

- **5a, the baseline** (`6b8e4990`, `ae4b42e3`): kit2k (`tools/level/kit_level.py`: 2,000 nodes over 40 meshes,
  998,560 triangles, every node a mesh collider); `tools/bench/asset_bench.py`; `render.stats`' `memory`.
- **5b, the cooker and the cache** (`7eb6efea`, `58b198aa`): a glTF imported once for all its meshes;
  `asset/content_hash.hpp` (128 bits); the cooked model and texture files (`asset/cooked.hpp`), read in place from a
  mapping; `ohao/cook`, which names a source by its bytes and those of every buffer and image it names.
- **5c, the runtime reads cooked data** (`e3f30cf0`, `9f827d2b`): `AssetResolver::loadMesh` returns a
  `cooked::Mesh`; instantiate, physics, animation and audio read it. A texture is one table entry by content. A
  load's textures upload in batches of 64 MB. `Model`, Assimp and the importer are the cooker's alone.
  - Deleted: the FBX and OBJ loaders, ufbx, eight OBJ and MTL files, four example programs (`cornell_box`,
    `model_viewer`, `env_demo`, `turntable`) with `model_spawn`, `SceneFramer` and the `InstantiateBackend`
    interface.
- **5d, release** (`26dd42ad`): textures held by count and destroyed three frames after the last hold; material
  rows and mesh arena ranges reused; a replaced world drops its cooked mappings.
- **5e, compressed textures** (`b9ae3f23`): bc7enc and rgbcx vendored in `external/bc7enc` (Frank's approval).
  A cooked texture is one use of an image: colour and data BC7, normals BC5 with z rebuilt in the shader; every mip
  and each channel's smallest value in the file.
- **5f, cooked geometry** (`c55d6381`, `64fe2b88`): a mesh's collision shape made once at scale 1, cooked as the
  backend's bytes and shared by every body at its own scale; the broad phase rebuilt after static bodies are added;
  submeshes and meshlets cooked. `level_bench --only kit2k`.
- **5g, no wait for the device** (`e57ae494`): mesh uploads, BLAS builds and texture uploads go through
  `gpu::SubmittedWork` (a fence, never waited for on the load path).
- **5h, the agent's API** (`469845a0`): `asset.info`, `asset.reload`; `checkAssets` covers every component that
  names a file.

## 3. Bugs found and fixed

- **Three lookups of a mesh as `MeshHandle{index, 0}`** (5d): wrong once an index is reused. Three gates failed in
  the one-process harness; an A/B with reclaim off passed. They ask by table index (`infoAt`).
- **A ray before the first step cost 18.4 us on kit2k** (5f): static bodies added one at a time left the broad
  phase's tree unbuilt until a step. The physics sync rebuilds it.
- **The bridge set a texture's `materialIndex`, which the loader never did** (5b): found by the identity hash on
  every textured mesh.
- **An edit script wrote four CRLF files as LF** (`c55d6381`, `e3f30cf0`): put back in `149be4b5`.
- **A hand-written 32-lane minimum was slower than the plain loop** (160 ms against 70; 5c): dropped.

## 4. Spec corrections

The spec's 4.8 now describes what was built. Against what it asked:
- **No GUID and no `.meta`:** identity is content; scenes refer to paths.
- **A format of our own in place of KTX2;** cooked on demand by the host, no `tools/cook`.
- **Not built, each with its reason in the plan:** split vertex streams, one upload and one BLAS submit per load,
  BLAS compaction, BC6H, cooked clips and audio, a `(path, size, mtime)` index, preparing a load on another thread
  with a byte budget and placeholders (step 4b's jobs), the file watcher.
- **One JSON reader:** not settled.

## 5. Results

On this machine (RTX 4060 laptop), `asset_bench`, warm, before step 5 and after:

| Scene | First frame | Textures on the GPU | Resident memory |
|---|---|---|---|
| Play scene | 0.28-0.30 s to 0.23 s | none of its own | 233 MB to 233 MB |
| Damaged Helmet | 0.41 s to 0.22 s | 106.7 MB to 26.7 MB | 314 MB to 226 MB |
| Sponza | 1.23 s to 0.25 s | 384.0 MB to 85.3 MB | 567 MB to 251 MB |
| flat42 | 0.47 s to 0.22 s | 112.0 MB to 28.0 MB | 321 MB to 227 MB |
| kit2k | 47.9 s to 0.42 s | none of its own | 1,110 MB to 324 MB |

- **The first load of a source cooks it:** Sponza 5.6 s, the helmet 1.0 s, flat42 1.1 s, kit2k 1.2 s to loaded.
- **kit2k's budgets** (`level_bench --only kit2k`, gated): load 88 ms (400), collision build 12 ms (150), a ray
  0.34 us (5), an idle sync 0.07 us (10). Before: 33.3 s, 33.2 s, 18.4 us, 0.07 us.
- **The release loop** (`asset_bench.py --loop 6`: Sponza, the helmet, the Cornell box in turn): textures, meshes,
  BLASes, materials and their bytes are the first rest's exactly after every round; resident memory 237 MB from
  round 2. Before: 448 MB of textures kept, 26.8 MB of mesh arena and 28 material rows more a round.
- **A scene arriving in a running engine:** Sponza 60 ms, the helmet 28 ms; of Sponza's, 40 ms is the main thread
  naming the source and copying to staging.
- **The compressor** was chosen from five measured on our textures (the plan's table): bc7enc at 48.72 dB and
  41 megapixels a second; bc7e (ISPC) is about 1 dB better and needs a build tool.
- **Pixels:** 5b, 5c, 5d, 5f and 5g changed none (the five goldens identical; cooked files byte-identical through
  5c). 5e moved the helmet's golden: RMSE 1.19 of 255, looked at and updated. Sponza before and after 5e at
  1280x720: RMSE 2.30, 0.29% of the pixels off by more than 16.
- **Behaviour:** the fight's five `world.hash` checkpoints are the same before and after 5f; 3,696 of kit2k's
  10,000 fixed rays hit, as before shapes were shared.
- **Planted, each caught:** a wrong primitive range (the identity hash); instantiate without its flush (the
  helmet's golden); one texture in seven never released (the loop); a shared shape's scale ignored (the kit's
  rays, 3,792); a missing animation graph (`ohao doctor`).
- **Gates:** every commit passed the gates its files reach, through the pre-commit hook. No GPU fault was logged;
  the GPU stayed between 35 and 55 C.
- **The cache** of every scene here: 245 MB (511 MB before compression).

## 6. Known gaps

- **The cache only grows:** files nothing names any more stay until `.ohao/` is deleted, which is always safe.
- **A load still runs on the main thread** (the 40 ms above); 4b's jobs take it.
- **The environment map** is read from its source with stb_image, uncompressed, and its upload waits for the device.
- **bc7enc uses two of BC7's modes:** textures with real alpha are about 2 dB worse in alpha than bc7e's.
- **No file watcher:** an edited source is shown after `asset.reload`.
- **Animation's bone map is not rebuilt by `asset.reload`** when a reloaded file changes its skeleton.
- **`build/eval_compressors/`** (git-ignored) holds the three compressors and `ispc` downloaded for the comparison.
