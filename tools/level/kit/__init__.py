"""The level kit (docs/specs/2026-11-18-tidewall-design.md, section 6): parametric blockout pieces built in Blender by
script, each with its level hints set here, never by hand (`col`, `surf`, `vis`, `trig`, `spawn`; the hint schema is
docs/specs/2026-10-26-level-pipeline-design.md, "Extras").

Every piece is given in the engine's frame, metres, Y up (x east, z south): `at` is the point the piece stands on,
`yaw` degrees about +Y, counter-clockwise seen from above, 0 facing -Z (north). The kit converts to Blender's Z up
(engine (x, y, z) is Blender (x, -z, y); a yaw about engine +Y is the same angle about Blender +Z), and the glTF
exporter's +Y up converts back. Each piece becomes flat objects named `<name>` or `<name>_<part>`, placed in world
space, in the order the layout lists them, so the same layout gives the same file.

Called by tools/level/build_level.py inside Blender (`bpy`).
"""
import math

import bmesh
import bpy

# name: (base colour, roughness, metallic). Flat blockout colours, one material per kind of surface.
MATERIALS = {
    'concrete': ((0.42, 0.42, 0.40), 0.9, 0.0),
    'asphalt': ((0.16, 0.16, 0.17), 0.95, 0.0),
    'block': ((0.55, 0.53, 0.50), 0.85, 0.0),
    'metal': ((0.30, 0.32, 0.35), 0.5, 0.8),
    'steel_grate': ((0.22, 0.24, 0.26), 0.6, 0.8),
    'pipe': ((0.45, 0.40, 0.30), 0.55, 0.6),
    'wood': ((0.55, 0.38, 0.22), 0.7, 0.0),
    'door': ((0.25, 0.30, 0.38), 0.6, 0.3),
    'container_blue': ((0.10, 0.22, 0.45), 0.6, 0.4),
    'container_red': ((0.50, 0.12, 0.08), 0.6, 0.4),
    'container_green': ((0.12, 0.35, 0.18), 0.6, 0.4),
    'container_orange': ((0.70, 0.35, 0.08), 0.6, 0.4),
    'container_grey': ((0.40, 0.42, 0.44), 0.6, 0.4),
    'barrier': ((0.62, 0.60, 0.56), 0.85, 0.0),
}

# What each material's surface sounds like underfoot (the `surf` hint, footsteps).
SURFACES = {
    'concrete': 'concrete', 'asphalt': 'concrete', 'block': 'concrete', 'barrier': 'concrete',
    'metal': 'metal', 'steel_grate': 'metal', 'pipe': 'metal', 'door': 'metal',
    'container_blue': 'metal', 'container_red': 'metal', 'container_green': 'metal',
    'container_orange': 'metal', 'container_grey': 'metal', 'wood': 'wood',
}

_materials = {}

CONTAINER_20 = (6.06, 2.59, 2.44)    # length (local x), height, width (local z): ISO 20 ft
CONTAINER_40 = (12.19, 2.59, 2.44)   # 40 ft


def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _materials.clear()


def material(name):
    if name not in _materials:
        rgb, roughness, metallic = MATERIALS[name]
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        bsdf = m.node_tree.nodes['Principled BSDF']
        bsdf.inputs['Base Color'].default_value = (*rgb, 1.0)
        bsdf.inputs['Roughness'].default_value = roughness
        bsdf.inputs['Metallic'].default_value = metallic
        _materials[name] = m
    return _materials[name]


def to_blender(p):
    """Engine (x, y, z), Y up, to Blender (x, -z, y), Z up."""
    return (p[0], -p[2], p[1])


def turn(offset, yaw_deg):
    """An engine-frame offset turned by yaw about +Y."""
    c, s = math.cos(math.radians(yaw_deg)), math.sin(math.radians(yaw_deg))
    x, y, z = offset
    return (x * c + z * s, y, -x * s + z * c)


def add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def _link(ob, hints):
    for k in sorted(hints):
        ob[k] = hints[k]
    bpy.context.collection.objects.link(ob)
    return ob


def mesh(name, verts, faces, mat, centre, yaw_deg=0.0, **hints):
    """A mesh object at `centre` (engine), turned by yaw; `verts` in the piece's engine frame about its centre."""
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    vs = [bm.verts.new(to_blender(v)) for v in verts]
    for f in faces:
        bm.faces.new([vs[i] for i in f])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(me)
    bm.free()
    me.materials.append(material(mat))
    ob = bpy.data.objects.new(name, me)
    ob.location = to_blender(centre)
    ob.rotation_euler = (0.0, 0.0, math.radians(yaw_deg))
    hints.setdefault('surf', SURFACES[mat])
    return _link(ob, hints)


BOX_FACES = [(0, 1, 2, 3), (4, 7, 6, 5), (0, 4, 5, 1), (1, 5, 6, 2), (2, 6, 7, 3), (3, 7, 4, 0)]


def box(name, centre, half, mat, yaw_deg=0.0, **hints):
    """A box of half extents `half` (engine x, y, z) about `centre`, turned by yaw. A box collider by default."""
    hx, hy, hz = half
    verts = [(-hx, -hy, -hz), (hx, -hy, -hz), (hx, -hy, hz), (-hx, -hy, hz),
             (-hx, hy, -hz), (hx, hy, -hz), (hx, hy, hz), (-hx, hy, hz)]
    hints.setdefault('col', 'box')
    return mesh(name, verts, BOX_FACES, mat, centre, yaw_deg, **hints)


def box_lohi(name, lo, hi, mat, **hints):
    """An axis-aligned box from lo to hi (engine)."""
    centre = tuple((a + b) / 2 for a, b in zip(lo, hi))
    half = tuple((b - a) / 2 for a, b in zip(lo, hi))
    return box(name, centre, half, mat, **hints)


def placed_box(name, at, yaw, local_centre, half, mat, **hints):
    """A box whose centre is `local_centre` in a piece's frame (at, yaw)."""
    return box(name, add(at, turn(local_centre, yaw)), half, mat, yaw, **hints)


def cylinder(name, a, b, radius, mat, sides=12, **hints):
    """A cylinder from a to b (engine, horizontal or vertical), a convex collider."""
    axis = tuple(y - x for x, y in zip(a, b))
    length = math.sqrt(sum(c * c for c in axis))
    d = tuple(c / length for c in axis)
    # Two directions across the axis.
    up = (0.0, 1.0, 0.0) if abs(d[1]) < 0.9 else (1.0, 0.0, 0.0)
    u = (d[1] * up[2] - d[2] * up[1], d[2] * up[0] - d[0] * up[2], d[0] * up[1] - d[1] * up[0])
    ul = math.sqrt(sum(c * c for c in u))
    u = tuple(c / ul for c in u)
    v = (d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0])
    half = tuple(c * length / 2 for c in d)
    verts = []
    for end in (-1.0, 1.0):
        for i in range(sides):
            t = 2.0 * math.pi * i / sides
            ring = tuple(radius * (math.cos(t) * uu + math.sin(t) * vv) for uu, vv in zip(u, v))
            verts.append(tuple(end * h + r for h, r in zip(half, ring)))
    faces = [tuple(range(sides)), tuple(range(2 * sides - 1, sides - 1, -1))]
    for i in range(sides):
        j = (i + 1) % sides
        faces.append((i, j, sides + j, sides + i))
    centre = tuple((x + y) / 2 for x, y in zip(a, b))
    hints.setdefault('col', 'convex')
    return mesh(name, verts, faces, mat, centre, **hints)


# ---- pieces: each takes the layout's entry (a dict) -----------------------------------------------------------------

def slab(p):
    """A slab or block from lo to hi: ground, floors, roofs, platforms."""
    box_lohi(p['name'], p['lo'], p['hi'], p.get('mat', 'concrete'))


def wall(p):
    """A straight wall from `from` to `to` ([x, z]) on `base`, `height` and `thick`, with `openings` (each `at`, its
    centre's distance from `from`, `width`, `height`, `sill`): segments between openings, a lintel above and a sill
    below each that needs one."""
    (x0, z0), (x1, z1) = p['from'], p['to']
    base, height, thick = p.get('base', 0.0), p.get('height', 3.0), p.get('thick', 0.3)
    mat = p.get('mat', 'block')
    length = math.hypot(x1 - x0, z1 - z0)
    # The wall's local x runs from `from` to `to`.
    yaw = math.degrees(math.atan2(-(z1 - z0), x1 - x0))
    at = (x0, base, z0)
    cuts = sorted(p.get('openings', []), key=lambda o: o['at'])
    start, k = 0.0, 0

    def segment(name, a, b, y0, y1):
        if b - a <= 1e-4 or y1 - y0 <= 1e-4:
            return
        placed_box(name, at, yaw, ((a + b) / 2, (y0 + y1) / 2, 0.0), ((b - a) / 2, (y1 - y0) / 2, thick / 2), mat)

    for o in cuts:
        lo, hi = o['at'] - o['width'] / 2, o['at'] + o['width'] / 2
        segment(f"{p['name']}_{k}", start, lo, 0.0, height)
        segment(f"{p['name']}_{k}_lintel", lo, hi, o['height'] + o.get('sill', 0.0), height)
        segment(f"{p['name']}_{k}_sill", lo, hi, 0.0, o.get('sill', 0.0))
        start, k = hi, k + 1
    segment(f"{p['name']}_{k}", start, length, 0.0, height)


def block(p):
    """A box of `size` [x, y, z] standing on `at`, turned by yaw."""
    sx, sy, sz = p['size']
    placed_box(p['name'], tuple(p['at']), p.get('yaw', 0.0), (0.0, sy / 2, 0.0), (sx / 2, sy / 2, sz / 2),
               p.get('mat', 'concrete'))


def cover_low(p):
    """Low cover, 1.0 m: crouch behind it (a concrete barrier), `length` along its local x."""
    length = p.get('length', 3.0)
    placed_box(p['name'], tuple(p['at']), p.get('yaw', 0.0), (0.0, 0.5, 0.0), (length / 2, 0.5, 0.3), 'barrier')


def cover_high(p):
    """High cover, 1.6 m: stand behind it."""
    length = p.get('length', 2.0)
    placed_box(p['name'], tuple(p['at']), p.get('yaw', 0.0), (0.0, 0.8, 0.0), (length / 2, 0.8, 0.2), 'block')


def container(p):
    """A shipping container (`size` "20" or "40" ft), its length along local x, on `at` (the stack's floor)."""
    length, height, width = CONTAINER_40 if str(p.get('size', '20')) == '40' else CONTAINER_20
    placed_box(p['name'], tuple(p['at']), p.get('yaw', 0.0), (0.0, height / 2, 0.0),
               (length / 2, height / 2, width / 2), 'container_' + p.get('color', 'blue'))


def stairs(p):
    """A flight of `steps` steps climbing `rise` toward local -z (the yaw's facing) from its foot at `at`, `width`
    wide, `tread` deep: solid blocks, each from the floor to its tread."""
    at, yaw = tuple(p['at']), p.get('yaw', 0.0)
    steps, rise, width, tread = p['steps'], p['rise'], p.get('width', 1.2), p.get('tread', 0.3)
    h = rise / steps
    for i in range(steps):
        top = h * (i + 1)
        placed_box(f"{p['name']}_{i:02d}", at, yaw, (0.0, top / 2, -tread * (i + 0.5)), (width / 2, top / 2, tread / 2),
                   p.get('mat', 'metal'))


def ramp(p):
    """A wedge `width` wide rising `rise` over `length` toward local -z from its foot at `at`: a triangle-mesh
    collider."""
    at, yaw = tuple(p['at']), p.get('yaw', 0.0)
    w, length, rise = p.get('width', 2.0) / 2, p['length'], p['rise']
    # About its centre: the foot edge at +z, the top edge at -z.
    zc = length / 2
    verts = [(-w, 0.0, zc), (w, 0.0, zc), (w, 0.0, -zc), (-w, 0.0, -zc), (-w, rise, -zc), (w, rise, -zc)]
    faces = [(0, 3, 2, 1), (0, 1, 5, 4), (1, 2, 5), (0, 4, 3), (2, 3, 4, 5)]
    centre = add(at, turn((0.0, 0.0, -zc), yaw))
    mesh(p['name'], verts, faces, p.get('mat', 'concrete'), centre, yaw, col='mesh')


def catwalk(p):
    """A raised walkway from `from` to `to` ([x, z]) at `height` (the deck's top), `width` wide: a grate deck, rails
    1.0 m high along both sides (its ends open: stairs or a drop meet them), posts every 3 m, and a navmesh link down off
    each end in `drops`."""
    (x0, z0), (x1, z1) = p['from'], p['to']
    height, width = p['height'], p.get('width', 1.5)
    length = math.hypot(x1 - x0, z1 - z0)
    yaw = math.degrees(math.atan2(-(z1 - z0), x1 - x0))
    at = (x0, 0.0, z0)
    name = p['name']
    placed_box(f'{name}_deck', at, yaw, (length / 2, height - 0.05, 0.0), (length / 2, 0.05, width / 2), 'steel_grate')
    for side, sz in (('l', -1.0), ('r', 1.0)):
        placed_box(f'{name}_rail_{side}', at, yaw, (length / 2, height + 0.5, sz * (width / 2 - 0.025)),
                   (length / 2, 0.5, 0.025), 'metal')
    # A drop off each open end named in `drops` ("from", "to"): a one-way navmesh link from the deck's edge to the
    # ground 1 m out (NavLink, in the drop's frame: its local x points out of the end).
    for end in p.get('drops', []):
        x, out = (0.0, -1.0) if end == 'from' else (length, 1.0)
        _empty(f'{name}_drop_{end}', add(at, turn((x - out * 0.3, height, 0.0), yaw)), yaw + (180.0 if out < 0 else 0.0),
               NavLink={'end': [1.3, -height, 0.0]})
    posts = max(2, int(length // 3.0) + 1)
    for i in range(posts):
        x = length * i / (posts - 1)
        x = min(max(x, 0.1), length - 0.1)
        for side, sz in (('l', -1.0), ('r', 1.0)):
            placed_box(f'{name}_post_{i:02d}{side}', at, yaw, (x, (height - 0.1) / 2, sz * (width / 2 - 0.1)),
                       (0.075, (height - 0.1) / 2, 0.075), 'metal')


def pipe_rack(p):
    """A pipe rack from `from` to `to` ([x, z]): pairs of posts every `bay` m, `width` apart, a beam across each pair
    at `height`, and `pipes` pipes along it on the beams. With `low`, the pipes lie on sleepers 0.3 m up instead, no
    posts: a run of pipes to crouch behind."""
    (x0, z0), (x1, z1) = p['from'], p['to']
    length = math.hypot(x1 - x0, z1 - z0)
    yaw = math.degrees(math.atan2(-(z1 - z0), x1 - x0))
    at = (x0, 0.0, z0)
    name, width, bay = p['name'], p.get('width', 1.2), p.get('bay', 5.0)
    pipes, radius = p.get('pipes', 3), p.get('radius', 0.15)
    bays = max(1, int(round(length / bay)))
    low = p.get('low', False)
    level = 0.3 + radius if low else p.get('height', 2.6) + 0.15 + radius
    for i in range(bays + 1):
        x = length * i / bays
        if low:
            placed_box(f'{name}_sleeper_{i:02d}', at, yaw, (x, 0.15, 0.0), (0.15, 0.15, width / 2 + 0.1), 'concrete')
            continue
        h = p.get('height', 2.6)
        for side, sz in (('l', -1.0), ('r', 1.0)):
            placed_box(f'{name}_post_{i:02d}{side}', at, yaw, (x, h / 2, sz * width / 2), (0.1, h / 2, 0.1), 'metal')
        placed_box(f'{name}_beam_{i:02d}', at, yaw, (x, h + 0.075, 0.0), (0.1, 0.075, width / 2 + 0.1), 'metal')
    for j in range(pipes):
        sz = (j - (pipes - 1) / 2) * (2 * radius + 0.1)
        a = add(at, turn((0.0, level, sz), yaw))
        b = add(at, turn((length, level, sz), yaw))
        cylinder(f'{name}_pipe_{j}', a, b, radius, 'pipe')
    if low:   # the top row, resting on the first two: low cover about 0.9 m high
        for j in range(pipes - 1):
            sz = (j + 0.5 - (pipes - 1) / 2) * (2 * radius + 0.1)
            y = level + 2 * radius * 0.87
            cylinder(f'{name}_pipe_top_{j}', add(at, turn((0.0, y, sz), yaw)), add(at, turn((length, y, sz), yaw)),
                     radius, 'pipe')


def door(p):
    """A closed door leaf (a box) `width` x `height`, in the opening at `at`, its width along local x: a navmesh obstacle
    (NavObstacle, its mesh's bounds), so the navmesh is baked without it and blocked while it is closed."""
    width, height, thick = p.get('width', 1.0), p.get('height', 2.1), p.get('thick', 0.06)
    placed_box(p['name'], tuple(p['at']), p.get('yaw', 0.0), (0.0, height / 2, 0.0),
               (width / 2 - 0.01, height / 2, thick / 2), 'door', NavObstacle={'active': True})


def _empty(name, at, yaw, **hints):
    ob = bpy.data.objects.new(name, None)
    ob.empty_display_type = 'ARROWS'
    ob.location = to_blender(at)
    # An empty's facing is its local -Z in the engine; Blender's exporter turns +Z up into +Y, so an empty turned by
    # yaw about Blender's Z faces the engine's yaw.
    ob.rotation_euler = (0.0, 0.0, math.radians(yaw))
    return _link(ob, hints)


def spawn(p):
    """A spawn point (SpawnPoint{tag}) at `at`, facing yaw."""
    _empty(p['name'], tuple(p['at']), p.get('yaw', 0.0), spawn=p['tag'])


def trigger(p):
    """A trigger volume from lo to hi: a sensor (physics.trigger_enter/exit), not drawn."""
    box_lohi(p['name'], p['lo'], p['hi'], 'concrete', trig=1, vis=0)


def view(p):
    """A camera node at `at` facing yaw, pitched by `pitch` degrees (up positive), `fov` degrees vertical: a fixed view
    for captures and benches (active only when the scene has no camera of its own)."""
    cam = bpy.data.cameras.new(p['name'])
    cam.lens_unit = 'FOV'
    cam.sensor_fit = 'VERTICAL'
    cam.angle = math.radians(p.get('fov', 60.0))
    ob = bpy.data.objects.new(p['name'], cam)
    ob.location = to_blender(tuple(p['at']))
    # A Blender camera looks down its -Z: turned 90 degrees about X it looks along Blender +Y (engine -Z), level.
    ob.rotation_euler = (math.radians(90.0 + p.get('pitch', 0.0)), 0.0, math.radians(p.get('yaw', 0.0)))
    bpy.context.collection.objects.link(ob)


PIECES = {
    'slab': slab, 'wall': wall, 'block': block, 'cover_low': cover_low, 'cover_high': cover_high,
    'container': container, 'stairs': stairs, 'ramp': ramp, 'catwalk': catwalk, 'pipe_rack': pipe_rack,
    'door': door, 'spawn': spawn, 'trigger': trigger, 'view': view,
}
