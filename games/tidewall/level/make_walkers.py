"""Writes a yard scene with N walkers (Tidewall, part N1): the yard scene's entities, and N characters that walk to the
player on the navmesh, started on a grid of the yard's walkable ground.

    python games/tidewall/level/make_walkers.py N OUT.scene.json [--ohao build/Release/ohao.exe]

The starting points are the grid's points (2.5 m apart, inside the yard's walls) that the engine finds on the navmesh
at ground level and reachable from the player's spawn (nav.nearest and nav.reach), taken in a fixed order spread over
the yard; the same build and arguments give the same file.
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
YARD = 'assets/tidewall/scenes/yard.scene.json'
GAME = 'build/Release/ohao_tidewall.dll' if os.name == 'nt' else 'build/Release/libohao_tidewall.so'


def candidates(ohao):
    grid = [(x * 2.5, z * 2.5) for z in range(-6, 7) for x in range(-10, 11)]
    script = [{'method': 'nav.reach', 'params': {'from': [-18, 0, 23], 'points': [[x, 0, z] for x, z in grid[i:i + 256]]}}
              for i in range(0, len(grid), 256)]
    script += [{'method': 'nav.nearest', 'params': {'point': [x, 0, z], 'extents': [0.05, 0.3, 0.05]}} for x, z in grid]
    path = os.path.join(ROOT, 'build', 'tidewall', 'walkers_probe.jsonl')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        for line in script:
            f.write(json.dumps(line) + '\n')
    out = subprocess.run([ohao, 'run', YARD, '--game', GAME, '--script', path], cwd=ROOT, capture_output=True, text=True)
    results = [json.loads(line) for line in out.stdout.splitlines() if line.startswith('{"method":"nav.')]
    reach = [p['reachable'] for r in results if r['method'] == 'nav.reach' for p in r['result']['points']]
    near = [r['result']['found'] for r in results if r['method'] == 'nav.nearest']
    return [g for g, a, b in zip(grid, reach, near) if a and b]


def walker(i, x, z):
    base = 0x7d0a110000000100 + i
    return [
        {'id': f'{base:016x}', 'Name': {'value': f'Walker{i:02d}'},
         'Transform': {'position': [x, 0, z]},
         'PlayerController': {'walkSpeed': 3.0, 'spawn': '', 'input': False},
         'NavAgent': {}, 'Walker': {}},
        {'id': f'{base + 0x100:016x}', 'Name': {'value': f'Walker{i:02d} Body'},
         'Transform': {'position': [0, 0.9, 0]}, 'Parent': {'entity': f'{base:016x}'},
         'MeshSource': {'source': {'CylinderPrim': {'radius': 0.3, 'height': 1.8}}}},
    ]


def main():
    args = sys.argv[1:]
    ohao = os.path.join(ROOT, 'build', 'Release', 'ohao.exe' if os.name == 'nt' else 'ohao')
    if '--ohao' in args:
        k = args.index('--ohao')
        ohao = args[k + 1]
        del args[k:k + 2]
    n, out = int(args[0]), args[1]
    free = candidates(ohao)
    if len(free) < n:
        raise SystemExit(f'make_walkers: only {len(free)} free points for {n} walkers')
    # Spread: every k-th free point.
    step = len(free) / n
    chosen = [free[int(i * step)] for i in range(n)]
    with open(os.path.join(ROOT, YARD), encoding='utf-8') as f:
        scene = json.load(f)
    for i, (x, z) in enumerate(chosen):
        scene['entities'] += walker(i, x, z)
    for c in ('MeshSource', 'NavAgent', 'Walker'):
        scene['components'][c] = 1
    scene['components'] = dict(sorted(scene['components'].items()))
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(scene, f, indent=2)
        f.write('\n')
    print(f'make_walkers: {n} walkers of {len(free)} free points -> {out}')


if __name__ == '__main__':
    main()
