"""Writes kit2k, the level the asset and level budgets are measured on: 2,000 nodes over 40 meshes, about 1 M
triangles (docs/specs/2026-10-26-level-pipeline-design.md, "Performance budgets"; docs/plans/2026-11-11-step5-assets.md).

    python tools/level/kit_level.py [build/kit2k/kit2k.glb] [--nodes 2000] [--meshes 40] [--triangles 1000000]

A kit as a level designer places one: each mesh used many times, on a grid, turned and scaled, with no level hint
(so every node gets the default, a static mesh collider). The meshes are spheres pushed in and out by a few waves,
each with normals, UVs and a material of its own. Generated, never checked in, never a golden; the same arguments
give the same bytes.
"""
import json
import struct
import sys

import numpy as np


def mesh(index, triangles):
    """A lat-long sphere of about `triangles` triangles, displaced by waves chosen by `index`."""
    rows = max(4, int(round((triangles / 4) ** 0.5)))
    cols = 2 * rows
    v, u = np.meshgrid(np.linspace(0.0, 1.0, rows + 1), np.linspace(0.0, 1.0, cols + 1), indexing='ij')
    theta, phi = v * np.pi, u * 2.0 * np.pi
    direction = np.stack([np.sin(theta) * np.cos(phi), np.cos(theta), np.sin(theta) * np.sin(phi)], axis=-1)
    a, b = 2 + index % 5, 3 + index % 7
    radius = 0.5 * (1.0 + 0.15 * np.sin(a * phi) * np.sin(b * theta))
    position = direction * radius[..., None]
    # Normals from the grid's own tangents (the displacement tilts them off the sphere's).
    du = np.gradient(position, axis=1)
    dv = np.gradient(position, axis=0)
    normal = np.cross(du, dv)
    length = np.linalg.norm(normal, axis=-1, keepdims=True)
    normal = np.where(length > 1e-8, normal / np.maximum(length, 1e-8), direction)
    i = (np.arange(rows)[:, None] * (cols + 1) + np.arange(cols)[None, :]).reshape(-1)
    quads = np.stack([i, i + 1, i + cols + 1, i + 1, i + cols + 2, i + cols + 1], axis=-1)
    return (position.reshape(-1, 3).astype('<f4'), normal.reshape(-1, 3).astype('<f4'),
            np.stack([u, v], axis=-1).reshape(-1, 2).astype('<f4'), quads.reshape(-1).astype('<u4'))


def build(nodes, meshes, triangles):
    blob = bytearray()
    views, accessors, gltf_meshes, materials = [], [], [], []

    def add(data, target, kind, component, bounds=None):
        while len(blob) % 4:
            blob.append(0)
        views.append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': data.nbytes, 'target': target})
        blob.extend(data.tobytes())
        accessor = {'bufferView': len(views) - 1, 'componentType': component, 'type': kind,
                    'count': int(data.shape[0])}
        if bounds is not None:
            accessor['min'], accessor['max'] = [float(x) for x in bounds.min(0)], [float(x) for x in bounds.max(0)]
        accessors.append(accessor)
        return len(accessors) - 1

    for m in range(meshes):
        position, normal, uv, index = mesh(m, triangles // meshes)
        hue = m / meshes
        materials.append({'name': f'kit_{m:02d}', 'pbrMetallicRoughness': {
            'baseColorFactor': [0.3 + 0.6 * hue, 0.8 - 0.5 * hue, 0.4 + 0.3 * ((m * 7) % 10) / 10, 1.0],
            'metallicFactor': 0.0, 'roughnessFactor': 0.4 + 0.5 * ((m * 3) % 10) / 10}})
        gltf_meshes.append({'name': f'Kit{m:02d}', 'primitives': [{
            'attributes': {'POSITION': add(position, 34962, 'VEC3', 5126, position),
                           'NORMAL': add(normal, 34962, 'VEC3', 5126), 'TEXCOORD_0': add(uv, 34962, 'VEC2', 5126)},
            'indices': add(index, 34963, 'SCALAR', 5125), 'material': m}]})

    side = int(np.ceil(nodes ** 0.5))
    gltf_nodes = []
    for n in range(nodes):
        yaw = (n * 37 % 360) * np.pi / 180.0
        scale = 0.8 + 0.4 * ((n * 13) % 10) / 10
        gltf_nodes.append({'name': f'Piece{n:04d}', 'mesh': n % meshes,
                           'translation': [1.5 * (n % side - side / 2), 0.5 * scale, 1.5 * (n // side - side / 2)],
                           'rotation': [0.0, float(np.sin(yaw / 2)), 0.0, float(np.cos(yaw / 2))],
                           'scale': [scale, scale, scale]})
    gltf_nodes.append({'name': 'PlayerSpawn', 'translation': [0.0, 2.0, 0.0], 'extras': {'spawn': 'player'}})

    while len(blob) % 4:
        blob.append(0)
    doc = {'asset': {'version': '2.0', 'generator': 'ohao tools/level/kit_level.py'}, 'scene': 0,
           'scenes': [{'name': 'kit2k', 'nodes': list(range(len(gltf_nodes)))}], 'nodes': gltf_nodes,
           'meshes': gltf_meshes, 'materials': materials, 'accessors': accessors, 'bufferViews': views,
           'buffers': [{'byteLength': len(blob)}]}
    text = json.dumps(doc, separators=(',', ':')).encode()
    text += b' ' * (-len(text) % 4)
    total = 12 + 8 + len(text) + 8 + len(blob)
    return (struct.pack('<4sII', b'glTF', 2, total) + struct.pack('<I4s', len(text), b'JSON') + text +
            struct.pack('<I4s', len(blob), b'BIN\0') + bytes(blob))


def main(argv):
    out, opts = 'build/kit2k/kit2k.glb', {'--nodes': 2000, '--meshes': 40, '--triangles': 1000000}
    i = 0
    while i < len(argv):
        if argv[i] in opts and i + 1 < len(argv):
            opts[argv[i]] = int(argv[i + 1])
            i += 2
        else:
            out = argv[i]
            i += 1
    data = build(opts['--nodes'], opts['--meshes'], opts['--triangles'])
    import os
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out, 'wb') as f:
        f.write(data)
    print(json.dumps({'path': out, 'bytes': len(data), 'nodes': opts['--nodes'] + 1, 'meshes': opts['--meshes']}))


if __name__ == '__main__':
    main(sys.argv[1:])
