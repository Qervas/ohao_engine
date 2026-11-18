# Caustics, step 2c: normal-mapped glints measured, and one exact solver for the manifold pass

**Spec:** `docs/specs/2026-11-04-caustics-step2-design.md` ("Step 2c", T1-T3, rulings M10, M11, M13); plan
`docs/plans/2026-11-08-caustics-step2c.md`
**Status:** built on branch `caustics-step2c-gpu` (2026-11-08 to 2026-11-18), not merged. Normal-mapped draws are not
owned by the manifold pass (measured, section 5); the light pass carries them.

## 1. Summary

Step 2c set out to solve, exactly, every path by which the sun glints off a normal-mapped surface onto a matte one, cell
by texel cell. It was built down to the cells, made complete and loss-free, and measured at equal time against step 1's
light pass, which already renders that light: about 49,000x the cost a pixel-spp for the same image. So mapped draws stay
with the light pass, and the code only they used is deleted. What the step left in the engine:
- MikkTSpace's normal in both renderers, and the tangent's handedness cooked right (the helmet's left half was flipped).
- The path tracer's normal map read as four texels blended in fp32.
- The manifold pass as rounds of bounded items in queues (no lane walks a scene-sized loop), with no item lost to a
  full queue.
- One exact solver for plain triangles (Bernstein subdivision and Krawczyk), in place of 2a's Newton, which missed
  about 2% of the roots.

## 2. What was built

- **MikkTSpace** (`material/material_layers.slang` `mikkTSpaceNormal`, `rt/pt_hit.slang` `mikkFrameAt`): the
  interpolated n, t and `b = w n x t` as they are, the result normalised; the bitangent at vertex location 10.
  `kCookerVersion` 4 (`w` from the bitangent's sign).
- **The fp32 map read** (`mapNormalAt`): four `Load`s at mip 0, blended in fp32, repeat addressing.
- **The rounds** (`ohao/render/rt/manifold_rounds.*`, `shaders/rt/path_tracer.slang`):
  - Stages: setup, three walk rounds, `manifoldSolvePlain`, `manifoldConnect` (raygens, `vkCmdTraceRaysIndirectKHR`
    from the queue counts), then resolve.
  - The frame runs in bands of rows, each in 12 passes. An item that does not fit is carried into the next pass with a
    resume index.
  - The sum is fixed point, by integer atomics.
- **The solver** (`shaders/rt/manifold_poly.slang`):
  - A plain triangle is solved on its (u, v) square, in 2a's frame interpolated, as degree-3 Bernstein grids.
  - Subdivision and Krawczyk on boxes grown 10%; each root proven, then polished.
  - The weight is `areaST |det dF/dsigma| / |det dF/d(s, t)|`.
  - The slope draw is untruncated GGX.
  - At most 96 tests an item a dispatch; an unfinished item's stack is carried, and splits in two when carried.
- **Built, measured and deleted** (at `7a693075`): a pyramid of tangent-normal balls per map, built on the GPU at
  upload; a side table of mapped triangles; the walk to cells with an exact uv clip, a hull-of-products bound and the
  sign tests; the cell solver on degree-5 grids.

## 3. Bugs found and fixed

- **The cooker's tangent handedness** was wrong, and the helmet's left half shaded inverted.
- **2a's lost roots.** Newton accepted a root at a tolerance of 1e-6, sigma's own rounding, and it started from one
  seed. On the caustics scene at 160x90 it found 1,984 roots where a dense double search finds 2,027.
- **A root lost to cancellation.** `y - p` was formed after rounding y at |y|. With a receiver 0.02 from a triangle
  2.3 from the origin, this moved a root 3.7e-5 of a side off its triangle. It is formed as `(p0 - p) + e1 u + e2 v`.
- **A queue's capacity was read once a pass.** Breadth-first rounds stacked in one region read at most `cap` items a
  pass. The two halves alternate by round now.
- **Two problems with degenerate uvs:**
  - Triangles with two equal uvs kept their whole uv box; they are clipped in their own coordinates.
  - A determinant of pure fp32 rounding passed a threshold of 1e-20; it is relative now (1e-5 of the sides' product).
- **Synchronization:** a write-after-write on the pyramid's directory, caught by synchronization validation.

## 4. Spec corrections

- **T1's count was not a bound.** T1's 47 cells a request counted cells whose corner slopes bracket the drawn slope,
  which is not a conservative test. The measured floor is 288 cells with a real root (171-391), and the walk needed
  532 to keep all of them.
- **Balls, not cones.** The pyramid holds balls of tangent normals, because a ball survives the slope's subtraction
  and a cone of directions does not. 2a's disc term is not conservative in the vertex tangent frame.
- **Step 2c's outcome:** owning mapped draws is refuted on cost (section 5). The spec's Step 2c records it.

## 5. Results

**Roots against dense double searches (plain triangles):** every root found, none false, none open.

| Scene | 160x90 | 640x360 |
|---|---|---|
| Caustics | 2,027 | 34,093 |
| Showcase | 8,686 | 139,331 |

**Cells against the floor, while mapped draws were built:** 1,373 of 1,373 path roots found. A float32 copy of the
bound pruned none of 273,691 planted roots.

**Bias, at equal time on the showcase** (paired seeds, mapped draws owned against the light pass): the grid-sum
difference was +0.0005 ± 0.0009 at 320x180 and −9e-5 ± 7e-5 at 640x360, and the per-pixel z RMS was 0.7.

**Cost at equal error:**
- Mapped draws owned: 9.2 ms a pixel-spp.
- The light pass: 0.19 µs a pixel-spp.
- The light pass reaches RMSE 0.00028 at 4,096 spp before an owned run finishes its first sample.

**`pt.manifold`, showcase:**

| Version | 640x360 | 1920x1080 |
|---|---|---|
| 2a, one lane a pixel | 92 ms | 644 ms |
| Rounds with Newton | 9.3 ms | 83 ms |
| The exact solver | 15.0 ms | 143 ms (the solve 60 ms) |

The caustics scene at 320x180 takes 2.9 ms. The manifold pipeline's cold compile is 1.8 s.

**The deletion:** 2,001 lines and 72.6 MB of device memory on the showcase, with the PFMs byte-identical.

**Gates:** `tools/check.py` passed, and `check_validation` under synchronization validation passed 21/21. No Xid
during the step after the power-off.

## 6. Known gaps

- **Cost:** the exact solver costs more than 2a's Newton did (1080p 83 -> 143 ms).
- **Capacity:** a scene denser than the showcase's 1080p peaks would carry past 12 passes and lose that light
  (counted, logged).
- **Glass:** step 2b's glass walk is still opt-in (`OHAO_MANIFOLD_GLASS=1`) and is not on the rounds.
- **Mapped casters:** they stay with the light pass, so their caustic is as noisy as its sample count.
