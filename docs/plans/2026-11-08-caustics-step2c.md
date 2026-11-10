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

### Task 4: the map read in fp32 (ruling M10)

Owned normal-mapped materials read their map as four `Load`s blended in fp32 in the tracer's hit and the pass (the
same expression), not the hardware's 1/256-quantised filter. `m13_fp32_normal`: the tracer's normal equals the twin's
to 1 ulp. Goldens with normal maps measured and updated after looking.

### Task 5: pruning to cells (CPU twin, then GPU)

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
