"""Writes tests/assets/layers_textures.gltf: one textured triangle whose material has
KHR_materials_clearcoat and KHR_materials_sheen with all four of their textures, each a 2x2 PNG of
its own colour (data URIs), so scene_tests can tell which image the loader put where.

    python tests/assets/make_layers_textures.py
"""
import base64
import json
import pathlib
import struct
import zlib

# RGBA of each image: clearcoatTexture, clearcoatRoughnessTexture, sheenColorTexture,
# sheenRoughnessTexture (scene_tests sp17_t2_gltf_layer_textures reads them back).
COLOURS = [(200, 10, 20, 255), (30, 150, 40, 255), (250, 60, 70, 255), (5, 6, 7, 120)]


def png(rgba, size=2):
    raw = b''.join(b'\x00' + bytes(rgba) * size for _ in range(size))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    ihdr = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')


def main():
    positions = [0, 0, 0, 1, 0, 0, 0, 1, 0]
    normals = [0, 0, 1] * 3
    uvs = [0, 0, 1, 0, 0, 1]
    blob = struct.pack('<9f', *positions) + struct.pack('<9f', *normals) + struct.pack('<6f', *uvs) + struct.pack('<3H', 0, 1, 2)
    blob += b'\x00' * (-len(blob) % 4)
    gltf = {
        'asset': {'version': '2.0', 'generator': 'tests/assets/make_layers_textures.py (OHAO scene_tests)'},
        'extensionsUsed': ['KHR_materials_clearcoat', 'KHR_materials_sheen'],
        'scene': 0,
        'scenes': [{'nodes': [0]}],
        'nodes': [{'name': 'layered', 'mesh': 0}],
        'meshes': [{'name': 'triangle', 'primitives': [
            {'attributes': {'POSITION': 0, 'NORMAL': 1, 'TEXCOORD_0': 2}, 'indices': 3, 'material': 0}]}],
        'materials': [{
            'name': 'layered',
            'pbrMetallicRoughness': {'baseColorFactor': [0.5, 0.5, 0.5, 1], 'metallicFactor': 0, 'roughnessFactor': 0.5},
            'extensions': {
                'KHR_materials_clearcoat': {
                    'clearcoatFactor': 0.8, 'clearcoatTexture': {'index': 0},
                    'clearcoatRoughnessFactor': 0.5, 'clearcoatRoughnessTexture': {'index': 1}},
                'KHR_materials_sheen': {
                    'sheenColorFactor': [1, 1, 1], 'sheenColorTexture': {'index': 2},
                    'sheenRoughnessFactor': 0.6, 'sheenRoughnessTexture': {'index': 3}},
            },
        }],
        'textures': [{'source': i} for i in range(4)],
        'images': [{'uri': 'data:image/png;base64,' + base64.b64encode(png(c)).decode()} for c in COLOURS],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [0, 0, 0], 'max': [1, 1, 0]},
            {'bufferView': 1, 'componentType': 5126, 'count': 3, 'type': 'VEC3'},
            {'bufferView': 2, 'componentType': 5126, 'count': 3, 'type': 'VEC2'},
            {'bufferView': 3, 'componentType': 5123, 'count': 3, 'type': 'SCALAR'},
        ],
        'bufferViews': [
            {'buffer': 0, 'byteOffset': 0, 'byteLength': 36, 'target': 34962},
            {'buffer': 0, 'byteOffset': 36, 'byteLength': 36, 'target': 34962},
            {'buffer': 0, 'byteOffset': 72, 'byteLength': 24, 'target': 34962},
            {'buffer': 0, 'byteOffset': 96, 'byteLength': 6, 'target': 34963},
        ],
        'buffers': [{'byteLength': len(blob), 'uri': 'data:application/octet-stream;base64,' + base64.b64encode(blob).decode()}],
    }
    out = pathlib.Path(__file__).with_name('layers_textures.gltf')
    out.write_text(json.dumps(gltf, indent=1) + '\n', encoding='utf-8', newline='\n')
    print('wrote', out)


if __name__ == '__main__':
    main()
