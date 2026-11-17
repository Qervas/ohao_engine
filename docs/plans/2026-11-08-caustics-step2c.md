# Caustics step 2c (normal-mapped glints) Implementation Plan

**Spec:** `docs/specs/2026-11-04-caustics-step2-design.md`, "Step 2c: normal-mapped glints" (T1 and T2 measured;
rulings M10, M11, M11b, M13 accepted).

**Goal.** Step 2a's term (sun -> a smooth reflection -> X's rough part) for normal-mapped casters, every root of every
cell found, so the showcase's helmet glints stop being the camera side's dots (ruling M7 lifted).

## Global constraints

- Exact: every root of a cell is found or the cell is counted undecided (the closed forms must show none). No cap that
  drops roots, no filtering of the map.
- **Bounded work per lane per dispatch** (2b's lesson, 2026-11-08): a GPU dispatch is not preempted, so no thread may
  run an unbounded loop. Work that can be large (cells a request, boxes a cell) is split into items of bounded cost
  and scheduled across threads and dispatches, never looped over in one lane.
- One solver, two builds: Slang compiled to SPIR-V and to the C++ twin (`tools/caustics/manifold_kernel.slang`), tested
  on the CPU first (`manifold_tests`, t-numbers `m1x_*` as the spec's).
- fp32 on the GPU; the twin's tests compare against double precision.
- Each OpenPBR entry keeps one call site per kernel (AGENTS.md, "Every call site is compiled whole").

## Tasks

### Task 1: the cell's polynomial in Bernstein form (CPU twin) — built 2026-11-08

`shaders/rt/manifold_poly.slang`: on one cell (the square between four texel centres, (s, t) in [0, 1]^2) of a
triangle, MikkTSpace's mapped normal `m = tn.x vT + tn.y vB + tn.z vN` from the interpolated vectors as they are (vB the
interpolation of the per-vertex `sign N x T`), the drawn slope's microfacet normal `h = m - sigma.x vT - sigma.y vB`,
`d = p - X`, and the two constraints `F1 = (d.a)(h.h) - 2 (d.h)(h.a)`, `F2` with b (a, b perpendicular to L): tensor
Bernstein grids of degree (5, 5) built from the cell's corner values by Bernstein products (convex combinations only),
in fp32. Twin op 16 returns the grids and F at a point.
- `m11_poly_matches_map`: at random points of random cells, F from the grids (de Casteljau) equals the double
  formula within fp32's rounding of its terms, and `F = 0` exactly where the double reflection holds (r parallel to L).

### Task 2: every root of a cell: Bernstein subdivision and Krawczyk (CPU twin) — built 2026-11-08

A box stack over [0, 1]^2 (the stack holds boxes only: each box's grids are extracted from the cell's by two de
Casteljau splits a direction). A box is excluded when 0 is outside either grid's hull (with a margin of the grids' own
rounding) or Krawczyk's K misses it; proven when K lies inside it (Newton from the centre); else split along its wider
side. Roots kept inside the cell, the triangle and their box's half-open extent (no root twice), and physical (r . L
> 0, h on the normal's side). A box budget; undecided boxes counted.
- `m12_cells_complete` (the spec's m10): T2's random cells at 27 and 45 degrees: every root a dense double search finds
  is found, every root reported a root of the grids within mu of the double polynomial's, undecided 0; boxes a cell
  (median, mean, max) printed. Results: the spec's "Step 2c", T3.

### Task 3: MikkTSpace's normal in both renderers (ruling M13)

The G-buffer, the tracer's hit (`fillHitAt`), RT GI's and RT direct light's hits: `normalize(tn.x vT + tn.y vB + tn.z
vN)` from the interpolated vectors, vB interpolated from the vertices. Measured first: every normal-mapped golden's
move (`render_golden.py --json`), looked at, then updated. The tracer's lift (a normal below the geometric plane) is
kept or dropped by what the cell's polynomial can carry: decided here, with numbers.

*As built (2026-11-17).* `mikkTSpaceNormal` (material/material_layers.slang): per vertex the unit world normal, the unit
world tangent (through the world matrix, not the normal matrix: a tangent lies in the surface) and `w n x t`, interpolated
as they are, the result normalised; seen from behind the whole normal turns. The G-buffer's vertex stage writes the
bitangent as an interpolant; the tracer's hit (`mikkFrameAt`) builds the same three from the vertices. RT GI and RT
direct light read no map (the geometric normal; the G-buffer). The solver's `ManifoldMappedTri` carries the per-vertex
bitangents (it took vertex 0's sign); T1's rejected cell map (`CellSlopeMap`) is deleted.
- **Found: the tangent's handedness was never cooked.** The cooker set w from the node's mirror alone, dropping the
  importer's bitangent, so every UV-mirrored half was lit with its bitangent upside down. w is now the side of `n x t`
  the importer's bitangent lies on, after the node's transform (`kCookerVersion` 4).
- **The helmet's golden** (the only normal-mapped one) moved: 18.9% of the pixels, max 61, RMSE 1.57, the left half
  of the helmet (its UVs mirror the right). A/B: the formula with the old handedness moves 4.1% of the pixels, max 29,
  RMSE 0.16, symmetric; the rest is the handedness. Looked at, updated. The other four goldens exact.
- **The lift** stays in the tracer for now: the pass counts the roots whose normal it would lift (task 6), and the
  choice is made on that count.

### Task 4: the map read in fp32 (ruling M10)

Owned normal-mapped materials read their map as four `Load`s blended in fp32 in the tracer's hit and the pass (the
same expression), not the hardware's 1/256-quantised filter. `m13_fp32_normal`: the tracer's normal equals the twin's
to 1 ulp. Goldens with normal maps measured and updated after looking.

*As built (2026-11-17).* `mapNormalAt` (rt/pt_hit.slang): the four texels round uv at mip 0, each decoded by
`normalFromMap`, blended bilinearly in fp32, repeating as the sampler does; the pass calls the same function, so the two
agree by construction (no twin since 2026-11-09). Every normal map the tracer reads takes it, not owned materials only:
one path, and no golden path-traces a normal map (all five exact). Checked against the hardware filter on the helmet
(640x360, 64 spp, one seed): mean within 6e-6, mean absolute difference 6.5e-5, 0.5% of the pixels more than 1% apart;
a half-texel shift would move every bump.

### The pass as rounds (decided 2026-11-17, before tasks 5 and 6)

**Where it starts.** `manifoldConnect` (rt/path_tracer.slang) is one thread a pixel: for each request and light it
picks one owned draw by solid angle and walks that draw's whole hierarchy in the lane (groups, clusters, sub-clusters,
triangles; 2a's disc test at each), then for each candidate triangle Newton, a visibility ray, the retrace of y, the
connection, summed into the pixel. Its work is bounded only by the chosen draw's size: tolerable for 2a's spheres,
not once 2c owns Sponza's draws and the helmet (tens of thousands of triangles each, their cones widened by the map),
and not with cells (47 a request on the helmet, up to 425, each 27 to 620 Krawczyk boxes). Step 2b's in-lane loop of
up to 4,096 walks is the one that took the GPU's firmware down twice.

**Decided:** the pass becomes rounds of items of bounded cost, in queues, across dispatches; 2a's walk moves onto them
first, so the machinery is written once and 2c's cells are one more kind of item.
- **Setup** (raygen, a thread a request): retraces X, picks the draw and the slope (the same hashes as today), writes
  a request record and the draw's group items.
- **Walk** (compute, a thread an item): an item is (request record, light, node); it tests the node's children (at
  most 8 clusters, 8 sub-clusters or 16 triangles) and appends those that pass to the next round's queue. The rounds
  follow the levels: groups, clusters, sub-clusters, triangles; for a mapped triangle, its uv footprint's pyramid
  nodes down to cells.
- **Solve** (compute): a plain triangle, 2a's Newton (16 steps); a cell, `cellPolySolve` with a box budget an item,
  set by measurement; a cell at the budget is a counted miss. Roots go to the connect queue.
- **Connect** (raygen, a thread a root): the shadow ray, the retrace of y, X's surface again (its retrace from the
  request), the contribution added to the pixel as fixed-point integers (`InterlockedAdd`: the sum is the same in any
  order, so two captures stay identical; a clamp is counted), resolved into the accumulation by one pass.
- **Bounded:** each dispatch's work per thread is a small fixed maximum; queue capacities are set by measurement and
  an overflow is counted, never silent; dispatches are indirect from the queues' counts.
- **Proof for 2a's move:** the same scene and seed give the same candidates, roots and connections (counted exactly:
  the same tests on the same numbers) and the same image within the fixed point's rounding.



A pyramid of normal cones per normal map, built at upload; a triangle's cone composed with the pyramid's over its uv
box, 2a's disc test at each node down to cells. Proven conservative on random triangles and maps
(`t8_disc_conservative`'s method). Measured on the helmet: cells a request (T1 counted 47 at least, 0 to 425).

### Task 6: the pass on the GPU, bounded

Requests -> candidate cells -> cell solves as work items of bounded cost (a box budget an item), spread across threads
and dispatches with the leftover carried, never a loop over a request's cells in one lane. Owned bit (M7) drops "no
normal map". Cost per stage measured on the showcase (`render.capture {timings}`) before anything is tuned.

### Task 7: closed forms, showcase, docs

`m9_bumped_mirror` (the spec's: a sine height field's map on a mirror, the wall's light against a numpy enumeration
of every cell in double, 1%), the showcase's helmet dots at equal time, the change record.

## Order

1 -> 2 on the CPU (no GPU risk), then 3 -> 4 (goldens, one GPU job at a time), 5, 6, 7.
