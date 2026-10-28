"""Generates the VFX slice's textures: smoke and flash flipbooks, the spark, and the bullet and
scorch decals (albedo alpha, and the bullet's normal map). Ours (MIT), from fixed seeds: running it
twice writes the same bytes.

    python tools/vfx/make_flipbooks.py [--out DIR]     (default DIR: assets/vfx)
    python tools/vfx/make_flipbooks.py --check          (write to a temp dir and compare with assets/vfx)

Every PNG is 8-bit RGBA (decal normals RGB), zlib level 9, no metadata chunks, so the bytes depend only
on the arrays. The decal normal map is linear: the engine loads it as a normal map, never sRGB.
"""

import argparse
import os
import struct
import sys
import tempfile
import zlib

import numpy as np

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_OUT = os.path.join(ROOT, "assets", "vfx")


def png_bytes(rgba):
    """An 8-bit PNG of an (h, w, 3|4) uint8 array. Fixed encoder settings: the same array, the same bytes."""
    rgba = np.ascontiguousarray(rgba, dtype=np.uint8)
    h, w, c = rgba.shape
    colour = {3: 2, 4: 6}[c]
    raw = b"".join(b"\x00" + rgba[y].tobytes() for y in range(h))

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, colour, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


def value_noise(rng, size, cells):
    """A smooth noise field in [0, 1): a seeded lattice of `cells` x `cells`, bilinear to `size`."""
    lattice = rng.random((cells + 1, cells + 1))
    xs = np.linspace(0.0, cells, size, endpoint=False)
    x0 = np.floor(xs).astype(int)
    fx = xs - x0
    fx = fx * fx * (3.0 - 2.0 * fx)
    a = lattice[np.ix_(x0, x0)]
    b = lattice[np.ix_(x0, x0 + 1)]
    c = lattice[np.ix_(x0 + 1, x0)]
    d = lattice[np.ix_(x0 + 1, x0 + 1)]
    top = a * (1 - fx)[None, :] + b * fx[None, :]
    bottom = c * (1 - fx)[None, :] + d * fx[None, :]
    fy = fx[:, None]
    return top * (1 - fy) + bottom * fy


def fbm(rng, size, octaves=3, base=3):
    total = np.zeros((size, size))
    amp, norm = 1.0, 0.0
    for o in range(octaves):
        total += amp * value_noise(rng, size, base * (2 ** o))
        norm += amp
        amp *= 0.5
    return total / norm


def radius(size):
    """Distance from the centre, 0 at the middle and 1 at the cell's edge midpoints."""
    c = (np.arange(size) + 0.5) / size * 2.0 - 1.0
    x, y = np.meshgrid(c, c)
    return np.sqrt(x * x + y * y), x, y


def smoke_cells(seed, cells=8, size=64):
    """8x8 puffs: a soft disc broken up by fbm, a different seed per cell (the flipbook's frames)."""
    out = np.zeros((cells * cells, size, size, 4), dtype=np.uint8)
    for i in range(cells * cells):
        rng = np.random.default_rng(seed + i)
        r, _, _ = radius(size)
        n = fbm(rng, size)
        # The puff grows with the frame index: a small core early in the flipbook, a wide one late.
        spread = 0.45 + 0.4 * (i / (cells * cells - 1))
        falloff = np.clip(1.0 - r / spread, 0.0, 1.0) ** 1.6
        alpha = np.clip(falloff * (0.55 + 0.9 * (n - 0.5)), 0.0, 1.0)
        out[i, :, :, 0:3] = 255
        out[i, :, :, 3] = np.round(alpha * 255).astype(np.uint8)
    return out


def tile(cells, cols, rows):
    """Cells (n, h, w, 4) laid out row by row into one sheet."""
    n, h, w, c = cells.shape
    sheet = np.zeros((rows * h, cols * w, c), dtype=np.uint8)
    for i in range(n):
        y, x = divmod(i, cols)
        sheet[y * h:(y + 1) * h, x * w:(x + 1) * w] = cells[i]
    return sheet


def flash_cells(seed, count=4, size=64):
    """Four frames of a muzzle flash: a star that opens, peaks and fades."""
    out = np.zeros((count, size, size, 4), dtype=np.uint8)
    for i in range(count):
        rng = np.random.default_rng(seed + i)
        r, x, y = radius(size)
        ang = np.arctan2(y, x)
        spikes = 0.5 + 0.5 * np.cos(ang * 6.0 + 0.5 * rng.random())
        reach = (0.35 + 0.25 * i) if i < 3 else 0.5
        core = np.clip(1.0 - r / (0.25 + 0.1 * i), 0.0, 1.0) ** 2
        star = np.clip(1.0 - r / reach, 0.0, 1.0) ** 1.5 * (0.4 + 0.6 * spikes)
        fade = 1.0 - 0.25 * i
        alpha = np.clip(np.maximum(core, star) * fade, 0.0, 1.0)
        warm = np.clip(core * 1.0 + star * 0.5, 0.0, 1.0)
        out[i, :, :, 0] = 255
        out[i, :, :, 1] = np.round(255 * (0.75 + 0.25 * warm)).astype(np.uint8)
        out[i, :, :, 2] = np.round(255 * (0.4 + 0.5 * warm)).astype(np.uint8)
        out[i, :, :, 3] = np.round(alpha * 255).astype(np.uint8)
    return out


def spark(seed, size=64):
    """A hot dot with a short streak: additive, soft."""
    rng = np.random.default_rng(seed)
    r, x, y = radius(size)
    dot = np.clip(1.0 - r / 0.22, 0.0, 1.0) ** 2
    streak = np.clip(1.0 - np.abs(y) / 0.06, 0.0, 1.0) * np.clip(1.0 - np.abs(x) / 0.9, 0.0, 1.0) * 0.5
    grain = 0.9 + 0.1 * rng.random((size, size))
    alpha = np.clip(np.maximum(dot, streak) * grain, 0.0, 1.0)
    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[:, :, 0:3] = 255
    out[:, :, 3] = np.round(alpha * 255).astype(np.uint8)
    return out


def bullet_decal(seed, size=64):
    """A bullet's hole in albedo: a dark ring with a soft edge, broken up by fbm. Tinted by the set."""
    rng = np.random.default_rng(seed)
    r, _, _ = radius(size)
    n = fbm(rng, size, base=5)
    hole = np.clip((0.85 - r) / 0.15, 0.0, 1.0) * np.clip(0.7 + 0.6 * (n - 0.5), 0.0, 1.0)
    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[:, :, 0:3] = 255
    out[:, :, 3] = np.round(np.clip(hole, 0.0, 1.0) * 255).astype(np.uint8)
    return out


def bullet_normal(seed, size=64):
    """The hole's normal map: a shallow crater, its height from the same radius, linear RGB."""
    rng = np.random.default_rng(seed)
    r, _, _ = radius(size)
    n = fbm(rng, size, base=5)
    height = np.clip(1.0 - (r / 0.85) ** 2, 0.0, 1.0) * (0.85 + 0.3 * (n - 0.5))
    gy, gx = np.gradient(height, 2.0 / size)
    nx, ny, nz = -gx * 0.6, -gy * 0.6, np.ones_like(height)
    length = np.sqrt(nx * nx + ny * ny + nz * nz)
    out = np.zeros((size, size, 3), dtype=np.uint8)
    out[:, :, 0] = np.round((nx / length * 0.5 + 0.5) * 255).astype(np.uint8)
    out[:, :, 1] = np.round((ny / length * 0.5 + 0.5) * 255).astype(np.uint8)
    out[:, :, 2] = np.round((nz / length * 0.5 + 0.5) * 255).astype(np.uint8)
    return out


def scorch_decal(seed, size=128):
    """A burn mark: a dark, uneven disc with a soft edge (the grenade's, projected straight down)."""
    rng = np.random.default_rng(seed)
    r, _, _ = radius(size)
    n = fbm(rng, size, octaves=4, base=4)
    mark = np.clip((0.95 - r) / 0.25, 0.0, 1.0) * np.clip(0.55 + 0.9 * (n - 0.5), 0.0, 1.0)
    out = np.zeros((size, size, 4), dtype=np.uint8)
    out[:, :, 0:3] = 255
    out[:, :, 3] = np.round(np.clip(mark, 0.0, 1.0) * 255).astype(np.uint8)
    return out


def build(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    files = {
        "smoke_8x8.png": tile(smoke_cells(1000), 8, 8),
        "flash_4x1.png": tile(flash_cells(2000), 4, 1),
        "spark.png": spark(3000),
        "decal_bullet.png": bullet_decal(4000),
        "decal_bullet_n.png": bullet_normal(4000),
        "decal_scorch.png": scorch_decal(5000),
    }
    for name, arr in files.items():
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(png_bytes(arr))
    return sorted(files)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true", help="write to a temp dir and compare with the committed files")
    args = ap.parse_args(argv)
    if not args.check:
        for name in build(args.out):
            print(os.path.join(args.out, name))
        return 0
    with tempfile.TemporaryDirectory() as tmp:
        names = build(tmp)
        bad = []
        for name in names:
            with open(os.path.join(tmp, name), "rb") as a, open(os.path.join(DEFAULT_OUT, name), "rb") as b:
                if a.read() != b.read():
                    bad.append(name)
        if bad:
            print("differs from the committed files: " + ", ".join(bad))
            return 1
        print("ok: %d textures byte for byte" % len(names))
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
