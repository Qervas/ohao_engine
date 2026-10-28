"""The alpha-tested test card (sub-project 12, test 32's transparency scene): a 2 x 2 m quad whose
glTF material is alphaMode MASK (cutoff 0.5) over a procedural leaf texture, its alpha 255 inside
the leaves and 0 between them, so a ray or a pixel through a gap sees what is behind the card.

Pure Python, no seeds but fixed numbers: the same bytes every run (check_level_assets-style
rebuild). Writes assets/vfx/alpha_card.gltf with the geometry and the PNG embedded as data URIs.

    python tools/vfx/make_alpha_card.py [--out PATH] [--check]
"""
import argparse
import base64
import json
import math
import pathlib
import struct
import sys
import zlib

SIZE = 64          # texels a side
LEAVES = [         # (centre u, centre v, half length, half width, angle degrees)
    (0.30, 0.30, 0.22, 0.09, 35.0),
    (0.70, 0.28, 0.20, 0.08, -40.0),
    (0.32, 0.72, 0.21, 0.08, -30.0),
    (0.70, 0.70, 0.23, 0.09, 45.0),
    (0.50, 0.50, 0.16, 0.06, 90.0),
]


def leaf_alpha(u, v):
    for cu, cv, a, b, deg in LEAVES:
        r = math.radians(deg)
        du, dv = u - cu, v - cv
        x = du * math.cos(r) + dv * math.sin(r)
        y = -du * math.sin(r) + dv * math.cos(r)
        if (x / a) ** 2 + (y / b) ** 2 <= 1.0:
            return 255
    return 0


def png_rgba(width, height, pixels):
    raw = bytearray()
    for y in range(height):
        raw.append(0)   # filter: none
        for x in range(width):
            raw.extend(pixels[y * width + x])

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def texture():
    pixels = []
    for y in range(SIZE):
        for x in range(SIZE):
            u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
            a = leaf_alpha(u, v)
            g = 110 + int(60 * v)
            pixels.append((40, g, 30, a))
    return png_rgba(SIZE, SIZE, pixels)


def gltf():
    # A quad in the XY plane facing +Z, 2 m square, centred on the origin.
    positions = [(-1.0, -1.0, 0.0), (1.0, -1.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)]
    normals = [(0.0, 0.0, 1.0)] * 4
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    indices = [0, 1, 2, 0, 2, 3]
    blob = bytearray()
    for p in positions:
        blob += struct.pack("<3f", *p)
    for n in normals:
        blob += struct.pack("<3f", *n)
    for t in uvs:
        blob += struct.pack("<2f", *t)
    for i in indices:
        blob += struct.pack("<H", i)
    blob += b"\x00" * ((4 - len(blob) % 4) % 4)
    doc = {
        "asset": {"version": "2.0", "generator": "tools/vfx/make_alpha_card.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0, "name": "AlphaCard"}],
        "meshes": [{"name": "AlphaCard", "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "materials": [{
            "name": "Leaves",
            "alphaMode": "MASK",
            "alphaCutoff": 0.5,
            "doubleSided": True,
            "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.8},
        }],
        "textures": [{"source": 0, "sampler": 0}],
        "samplers": [{"magFilter": 9728, "minFilter": 9728}],   # nearest: the cut stays sharp
        "images": [{"uri": "data:image/png;base64," + base64.b64encode(texture()).decode("ascii")}],
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," +
                     base64.b64encode(bytes(blob)).decode("ascii")}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 48, "target": 34962},
            {"buffer": 0, "byteOffset": 48, "byteLength": 48, "target": 34962},
            {"buffer": 0, "byteOffset": 96, "byteLength": 32, "target": 34962},
            {"buffer": 0, "byteOffset": 128, "byteLength": 12, "target": 34963},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [-1, -1, 0], "max": [1, 1, 0]},
            {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
        ],
    }
    return (json.dumps(doc, indent=1, sort_keys=True) + "\n").encode("utf-8")


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(root / "assets/vfx/alpha_card.gltf"))
    ap.add_argument("--check", action="store_true", help="exit 1 unless the file on disk is what this writes")
    args = ap.parse_args()
    data = gltf()
    out = pathlib.Path(args.out)
    if args.check:
        same = out.exists() and out.read_bytes() == data
        print(f"{out}: {'same' if same else 'DIFFERS'}")
        return 0 if same else 1
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"wrote {out} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
