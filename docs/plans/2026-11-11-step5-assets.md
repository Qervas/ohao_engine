# Step 5: assets

**Spec:** `docs/specs/2026-11-08-system-design.md`, 4.8 and 5 (step 5); `docs/ROADMAP.md` row 15 (which absorbs the
level pipeline's kit2k budgets). **Status:** planned 2026-11-11; built before step 4 (Frank, 2026-11-11).

Source files stop being parsed by the running engine. A cooker turns each source file into cooked data, cached by
content hash; the engine loads cooked data only, shares it by content, and gives it back when nothing uses it.

The cooked formats and the cooker come first, so the runtime's loader is written once, against cooked data, and no
from-source path is written to be thrown away.

## Where it starts

From a survey of the tree at `c536dafd` and runs on this machine (4060, 2026-11-11).

- **A cold start to a first frame** (`ohao run <scene> --render`, 640x360, the whole process, second run):

  | Scene | Wall | Peak resident |
  |---|---|---|
  | The play scene | 0.32 s | 256 MB |
  | Damaged Helmet | 0.48 s | 350 MB |
  | Sponza | 1.38 s | 597 MB |
  | flat42 (wide), Frank's kit | 0.56 s | 343 MB |

  Load time is not what hurts at this content's size; memory and the missing release are.
- **The baseline** (`tools/bench/asset_bench.py`, 5a: a new `ohao serve` a scene, the second run; GPU bytes from
  `render.stats`' `memory`):

  | Scene | Loaded | First frame | Textures | Meshes | BLAS | Resident |
  |---|---|---|---|---|---|---|
  | play | 16 ms | 0.28 to 0.30 s | 4, 0.0 MB | 1.4 MB | 22, 1.0 MB | 235 MB |
  | helmet | 7 ms | 0.41 s | 9, 106.7 MB | 1.8 MB | 13, 0.9 MB | 330 MB |
  | Sponza | 8 ms | 1.23 s | 77, 384.0 MB | 24.9 MB | 3, 14.0 MB | 576 MB |
  | flat42 | 8 ms | 0.47 s | 16, 112.0 MB | 1.5 MB | 7, 0.7 MB | 329 MB |
  | kit2k | 45.6 s | 47.9 s | 4, 0.0 MB | 70.8 MB | 40, 53.4 MB | 1,085 MB |

  Texture bytes are the pixels with their mips at 4 bytes each; each count includes the renderer's 4 built-in
  textures. A scene's first run after a build takes about 2 s longer (the driver's caches).
- **At a kit's size load time is the problem.** kit2k (`tools/level/kit_level.py`: 2,000 nodes over 40 meshes,
  998,560 triangles, a 28.6 MB GLB, every node a static mesh collider) loads headless in **43.6 s**
  (`ohao level build/kit2k/kit2k.glb`, the whole process, no GPU), at 916 MB resident; the budget is 400 ms. The
  file is imported whole by Assimp once for each of its 40 meshes.
- **Three glTF readers, and one file read many times.**
  - Assimp 5.4.3 for geometry, materials and textures (`asset/model_loader.cpp`); a simdjson reader for a level's
    structure (`level/gltf_structure.cpp`); another for skins and clips (`anim/gltf_anim.cpp`).
  - A level of U distinct meshes reads its file 2U + 1 times: `loadMesh` reads and parses the structure again and
    runs Assimp over the whole file for each mesh (the blockout: 13 imports for 13 meshes).
  - FBX is parsed twice (ufbx, then Assimp). OBJ has its own parser. No scene refers to an FBX or OBJ file.
- **Textures.**
  - Every texture is RGBA8, uncompressed: 32 bits a pixel where BC7 is 8.
  - A texture that k meshes share is decoded k times and uploaded k times.
  - Each upload is its own staging buffer, its own device allocation (not VMA), its own submit and wait, and a
    rewrite of every live slot of the bindless table.
  - Mips are a blit chain on the GPU at load.
  - A model's texture is never freed; `unloadTexture` waits for the whole device.
- **Meshes.** Each is its own upload and wait, then its own BLAS build and wait (two GPU waits a mesh), with
  meshlets built at load by `meshlet_builder`. Vertices are 100 bytes. A released mesh's range in the arena is
  never reused. Materials are never freed (a table of 4,096).
- **Names.** `AssetRef` is `{path, guid}`; the guid is never set and the resolver ignores it. One project root.
- **Caches** are keyed by path and live as long as their owner: the resolver's models (with their decoded pixels),
  the level structures, the animation cache. Only `level.reload` forgets anything.
- **No file watcher and no `asset.*` method.** `level.reload` and `game.reload` are the reloads there are.
- **Dependencies present:** simdjson, stb, Assimp, ufbx, miniaudio, nlohmann. **Absent:** any block compressor, KTX,
  meshoptimizer, zstd, a hash library. An on-disk cache by hash exists for RT pipelines
  (`render/rt/pipeline_disk_cache.cpp`, FNV-1a).
- **kit2k** had never been generated (the level pipeline planned it in Blender, which this machine does not have).
  It is now written by numpy alone; `level_bench` still prints its four budgets as not measured.

## Decisions

Each by AGENTS.md's "Deciding"; the ones that leave the spec are also under "Spec corrections".

- **Cook on demand, in the host.** A host asked for an asset whose cooked data is missing cooks it then and there
  (the cooker is a library, `ohao_cook`); `ohao cook`, cooking ahead, comes with the first build that ships without it. An agent never runs a second step, and never
  sees stale data: the cache key is the source's content.
  - *Refuted:* a ship build must not carry the importers. It will not: only `ohao_cook` links them, and a host
    built without it fails on a missing cooked asset by name. That build is not made in this step.
- **The cache key** is a hash of the source's bytes, the cooker's version and the cook settings. A cache of
  `(path, size, mtime) -> hash` saves rehashing unchanged files. The cache is `<root>/.ohao/cooked/`, git-ignored.
  - The hash is 128-bit, written here (two FNV-1a 64 streams are too weak to trust for content addressing; the
    choice is fixed in 5b after measuring its speed on the 135 MB tree).
- **Assimp stays, inside the cooker only.** The spec asks for one glTF reader in the cooker. Assimp's output is the
  baseline every golden holds; replacing it changes every vertex order and tangent at once. It leaves the runtime
  in 5c, and a reader of our own can replace it later by touching the importer alone.
- **FBX and OBJ are deleted** (ufbx, `model_fbx.cpp`, the OBJ and MTL parsers). No scene uses them; Frank's kit
  arrives as GLB through Blender (`tools/level/stage_flat42.py`). The four tracked OBJ files go with them.
- **A private cooked format, not KTX2.** Nothing outside the engine reads the cache; a header and the mip levels'
  bytes are read with one `mmap` and no library. KTX2 would need libktx or a reader of our own for a container
  whose other readers we do not have.
- **No GUIDs and no `.meta` files in this step.** Their purpose is surviving a rename. Scenes are text that agents
  write by path, and a rename is a search and replace an agent does; the cache is keyed by content, so a rename
  costs nothing there. `AssetRef.guid` stays in the format, unset. Built when a rename breaks something.
- **Audio and clips are not cooked in this step.** A bank's decode has its budget (`bank_load_ms`, 150 ms) and
  meets it; clips are read once per file. Cooked when a measurement asks.
- **Until step 4:** the streamer (5g) loads on threads of its own and uploads on the graphics queue between frames.
  4b's jobs and 4a's transfer queue replace both without changing what the asset manager offers.

## Needs Frank

- **A block compressor (5e).** BC7, BC5 and BC6H encoding is the one piece not written here: a good BC7 encoder is
  a research-grade search. Candidates, to be compared by PSNR and time on our textures before one is asked for:
  `bc7enc_rdo` (MIT, C++, no build tool), Intel's ISPC Texture Compressor (MIT, needs the `ispc` compiler),
  Compressonator (MIT, large). 5a to 5d and 5f need none of them.

## The referee

- **Identical where nothing should change.** 5b and 5c move the same bytes through a new path: the cooked mesh
  decodes to the vertices, indices and materials today's loader gives, compared bit for bit by a temporary tool over
  every scene's assets, and the five goldens hold. Mips move from a GPU blit to the cooker: a deliberate change,
  looked at.
- **Measured where it should.** `asset_bench` (5a) gives load time, texture and mesh bytes on the GPU, and resident
  memory, before and after each part.
- **Compression is judged by eye and by number:** each compressed texture's PSNR against its RGBA8, and the play
  scene, the helmet and Sponza rendered both ways (`docs/notes/renderer.md`, "The referee").

## Parts

Each is one or a few commits with the hooks green.

- **5a, the baseline.**
  - `tools/bench/asset_bench.py` (done): for the play scene, the helmet, Sponza, flat42 and kit2k, the time to the
    scene loaded and to a first frame, the GPU's texture, mesh and BLAS bytes, and resident memory.
  - The GPU's bytes by kind in `render.stats`' `memory` (done): what the bench reads, and what an agent reads.
  - `tools/level/kit_level.py`: kit2k, generated (done); `level_bench`'s four kit2k numbers measured as they stand.
- **5b, the cooker and the cache.**
  - `ohao/cook`: a source file imported once, whole, into cooked assets: one per mesh (vertex and index data, the
    submeshes, the material slots), one per material, one per texture (RGBA8 with its mips, the format a field), a
    level's structure, a file's skins and clips as they are read today.
  - The content hash and the cache directory.
  - Proven by the bit-for-bit comparison against today's loader, and by a planted change to a source file cooking
    again while an untouched one does not.
  - *As built, first piece (2026-11-11): a file is imported once.* `ModelLoader::loadMeshes` imports a glTF file
    once and makes every mesh of it from that one Assimp scene; the resolver keeps the file's meshes
    (`FileAssetResolver::m_files`). `loadMesh` is gone.
    - Identical: `model_hash` (temporary, `tools/bench/model_hash.cpp`) hashes everything the resolver hands the
      engine for each mesh (by index, by name, and the whole file) of the 18 tracked and local glTF files and of
      kit2k: 188 lines, the same before and after. A planted wrong primitive range fails it.
    - The 188 loads took 9.0 s + 29.3 s before and 6.4 s + 6.9 s after.
    - kit2k's load went from 45.6 s to 33.5 s only: the rest is its 2,000 mesh colliders, each built from its
      25,000 triangles (5f's shape cache). `blockout_load_ms` 1.24 ms (budget 20).
  - *As built, the cache (2026-11-11).*
    - `asset/content_hash.hpp`: a 128-bit hash written here (two lanes of a folded 64x64 multiply over 8-byte
      words). `asset/cooked.{hpp,cpp}`: the model file (a source's meshes: vertices, indices, a material slot a
      triangle, materials with their textures' names, joint names) and the texture file (RGBA8, level 0), read in
      place from a mapping and written through a temporary and a rename.
    - `ohao/cook` (`ohao_cook`, level 3): `cookedModel` names a source by its bytes, the bytes of every buffer and
      image a glTF names, what was asked (its meshes, or the whole file) and the versions, and cooks it when the
      cache has no such file. `FileAssetResolver` loads only through it, and makes a `Model` of the cooked mesh
      (`cook::toModel`, the bridge 5c removes).
    - **Mips stay the GPU's until 5e**, so 5b and 5c change no pixel: the texture file holds level 0.
    - **No `(path, size, mtime)` index:** a source is hashed at each load (Sponza's 108 MB with its images among
      the 0.64 s below). Built if a measurement asks.
    - **No `ohao cook` yet:** every host cooks on demand, and nothing ships without the cooker. Built with the
      first build that does.
    - Identical: `model_hash`'s 188 lines through the cache, cold and warm; the five goldens. A copy of the
      blockout cooks nothing (same content, another path); one byte of its buffer changed cooks once; the untouched
      copy and a second load cook nothing.
    - First frame, the cache warm (`asset_bench`; before in brackets): play 0.27 s (0.28 to 0.30), helmet 0.32 s
      (0.41), Sponza 0.64 s (1.23), flat42 0.33 s (0.47), kit2k 35.5 s (47.9; its colliders). The first load of
      each cooks: helmet 0.50 s, Sponza 1.51 s, flat42 0.54 s. `blockout_load_ms` 0.92 ms.
    - The cache of every scene here is 511 MB (uncompressed pixels, a texture once). It only grows: unused files
      stay until `.ohao/` is deleted, which is always safe.
- **5c, the runtime reads cooked data only.**
  - The asset manager replaces `FileAssetResolver`'s model cache: handles to cooked assets, mapped from the cache,
    cooked on demand. A texture is one asset by content: decoded never, uploaded once whoever uses it.
  - Uploads of a load go in one batch: one staging buffer and one submit for its textures, the bindless table
    written once; VMA for the images.
  - `ModelLoader`, Assimp and stb leave the runtime libraries for `ohao_cook`. FBX and OBJ are deleted.
  - *As built, first piece (2026-11-11): the engine reads cooked meshes.*
    - `AssetResolver::loadMesh` returns a `cooked::Mesh` (views into the file's mapping; the pointer keeps it
      mapped) and `texture(name)` a cooked texture. Instantiate, physics, animation and audio read it; a primitive
      is the same view over vectors made in code. `Model` is the importer's struct, inside `ohao/cook` with
      `ModelLoader`, Assimp and stb_image's use for models. `cook::toModel` is gone.
    - A texture is one table entry by content (`BindlessTextureManager::contentTexture`: the pixels' hash, the
      format, the kind, whether alpha is forced). The smallest value of each channel is found at upload (caustics
      read roughness's); it moves into the cooked texture with 5e, when that file changes anyway.
    - **Deleted:** the FBX and OBJ loaders, ufbx, the eight OBJ and MTL files; Assimp's other formats (dae, blend,
      3ds, ply) from what a `ModelAsset` accepts, untested and unused. The four example programs that pushed an
      in-memory `Model` at the renderer (`cornell_box`, `model_viewer`, `env_demo`, `turntable`: `ohao run` and
      `ohao serve` render any scene) with `model_spawn`, `SceneFramer`, `framing` and the `InstantiateBackend`
      interface, which had one implementation. `model_hash` too: its job ended with 5b.
    - **stb_image stays in the runtime** for the environment map and a vfx set's sprites, read from their source
      files until 5e cooks them.
    - Identical: an empty cache recooked gives the 12 model files of the five scenes byte for byte; the five
      goldens; gates 6 of 6.
    - Sponza's textures went from 384.0 MB (77 table entries) to 341.3 MB (69): eight uploads were of an image
      already uploaded. The helmet's and flat42's are unchanged (no image shared).
    - Resident memory after the first frame, the decoded copies gone: helmet 313.8 MB to 231.5, Sponza 566.8 to
      255.9, flat42 320.6 to 233.9 (the play scene, with no model textures, 233.1 to 232.1). First frame, warm: play 0.28 s, helmet 0.32 s, Sponza 0.59 s
      (0.64), flat42 0.33 s, kit2k 35.4 s.
- **5d, release.**
  - Textures, materials and meshes are counted and freed when unused, their GPU memory after the frames in flight
    (no device wait); the mesh arena reuses released ranges; the manager drops unmapped cooked data.
  - Proven by a load and unload loop that returns GPU and resident memory to its baseline, and a planted leak
    (a texture never released) that the loop catches.
- **5e, compressed textures** (after Frank's approval of the compressor).
  - BC7 for colour, BC5 for normals (the shader rebuilds z), BC6H for the environment, mips from the cooker.
  - Done when texture memory drops by the compression ratio (4x for BC7) and the renders pass the referee.
- **5f, cooked geometry.**
  - Meshlets built by the cooker. Vertex data split into the position stream (depth, RT) and the rest.
  - One upload and one BLAS build submit per load, not two waits a mesh; BLAS compaction if it measures a gain.
  - kit2k's four budgets met (load 400 ms, collision build 150 ms, a ray 5 µs, an idle sync 10 µs), with collision
    shapes cached per (mesh, shape, scale), the gap recorded in the level pipeline.
- **5g, streaming.**
  - Loads run off the main thread; a frame takes finished uploads under a byte budget; an asset not yet arrived
    draws its placeholder. A lockstep host and a capture wait for everything asked (deterministic frames).
  - Done when a scene loads with no device or queue wait on the main thread.
- **5h, hot reload and the agent's API.**
  - A watcher (inotify) on the sources in use: a changed file is cooked and swapped where it is used.
  - `asset.info` (what is loaded, its bytes, who uses it), `asset.reload`; `ohao doctor`'s `assets.present` covers
    every `AssetRef`, not three of the eight.
- **5i, close.** nlohmann leaves the libraries that no longer need it; the record; the docs; the temporary
  comparison tool deleted.

## Done when (the spec's criteria)

- The play scene loads from cooked data faster than from source (measured in 5a and after 5c).
- Texture memory drops by the compression ratio (5e).
- Loading has no `WaitIdle` (5g).
- A load and unload loop returns memory to its baseline (5d).
- Row 15: kit2k's budgets met (5f); a real kit (flat42) loads cooked with at least 4x less texture memory (5e).

## Spec corrections

To be made in the spec when the part that settles each is built:
- **4.8, the database:** no GUID and no `.meta` in step 5; scenes refer to paths, the cache to content.
- **4.8, the cooker:** a private cooked format in place of KTX2; cooked on demand by the host, with no
  `tools/cook` until a build ships without the cooker; "on jobs" waits for 4b.
- **4.8, the runtime:** the streamer's jobs and transfer queue wait for step 4.
- **4.8, one JSON reader:** not settled here. The scene reader is our own (`ecs::parseJson`,
  `reflect::JsonCursor`) and simdjson reads the other formats; 5i measures both on a scene file before either goes.
