"""Drive a fixed Minecraft lake + shore + overhanging cabin scene for sky/water/weather before/after benchmarks."""

import argparse
from contextlib import ExitStack
import math
import re
import socket
import struct
import time
from pathlib import Path


def frame(kind, body=b""):
    return struct.pack(">IB", len(body) + 1, kind) + body


# 4x4 tile atlas (128x128, 32x32 per tile)
TILE_GRASS = 0
TILE_DIRT = 1
TILE_STONE = 2
TILE_WOOD = 3
TILE_LEAVES = 4
TILE_WATER = 5
TILE_GLASS = 6
TILE_METAL = 7
TILE_LANTERN = 8
TILE_SAND = 9


def tile_uv(tile_id):
    tx = tile_id % 4
    ty = tile_id // 4
    pad = 1.5 / 128.0
    u0 = tx * 0.25 + pad
    u1 = (tx + 1) * 0.25 - pad
    v0 = ty * 0.25 + pad
    v1 = (ty + 1) * 0.25 - pad
    return ((u0, v0), (u0, v1), (u1, v1), (u1, v0))


def quad(face, vertices, tile_id, color=(255, 255, 255, 255), light=0xF0):
    uvs = tile_uv(tile_id)
    return bytes([face]) + b"".join(
        struct.pack(">5f5B", *point, *uv, *color, light)
        for point, uv in zip(vertices, uvs)
    )


def box_quads(x0, y0, z0, x1, y1, z1, top_tile, side_tile, *, color=(255, 255, 255, 255), light=0xF0, faces="UDNSWE"):
    out = []
    if "D" in faces:
        out.append(quad(0, ((x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)), side_tile, color, light))
    if "U" in faces:
        out.append(quad(1, ((x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (x1, y1, z0)), top_tile, color, light))
    if "N" in faces:
        out.append(quad(2, ((x1, y0, z0), (x0, y0, z0), (x0, y1, z0), (x1, y1, z0)), side_tile, color, light))
    if "S" in faces:
        out.append(quad(3, ((x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)), side_tile, color, light))
    if "W" in faces:
        out.append(quad(4, ((x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)), side_tile, color, light))
    if "E" in faces:
        out.append(quad(5, ((x1, y0, z1), (x1, y0, z0), (x1, y1, z0), (x1, y1, z1)), side_tile, color, light))
    return b"".join(out)


def build_atlases():
    size = 128
    albedo = bytearray(size * size * 4)
    normal = bytearray(size * size * 4)
    pbr = bytearray(size * size * 4)

    def put(px, py, bgra_a, bgra_n, bgra_p):
        idx = (py * size + px) * 4
        albedo[idx:idx + 4] = bytes(bgra_a)
        normal[idx:idx + 4] = bytes(bgra_n)
        pbr[idx:idx + 4] = bytes(bgra_p)

    for tile_id in range(16):
        tx = (tile_id % 4) * 32
        ty = (tile_id // 4) * 32
        for ly in range(32):
            for lx in range(32):
                h = ((lx * 374761393 + ly * 668265263 + tile_id * 1442695041) & 0xFFFFFFFF)
                h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
                n = ((h >> 16) & 255) / 255.0 - 0.5
                # Defaults: BGRA order!
                # Normal: (B=Z, G=Y, R=X, A=porosity or 255)
                # PBR: (B=emissive, G=metallic, R=roughness, A=ao)
                r, g, b, a = 180, 180, 180, 255
                nx, ny, nz, porosity = 128, 128, 255, 180
                rough, metal, emis, ao = 225, 0, 0, 255

                if tile_id == TILE_GRASS:
                    r, g, b = int(78 + n * 18), int(124 + n * 24), int(56 + n * 14)
                    rough, porosity = 242, 235
                elif tile_id == TILE_DIRT:
                    r, g, b = int(118 + n * 22), int(88 + n * 18), int(64 + n * 14)
                    rough, porosity = 236, 230
                elif tile_id == TILE_STONE:
                    mortar = (lx % 16 == 0) or (ly % 8 == 0)
                    base = 112 if mortar else int(146 + n * 20)
                    r = g = b = base
                    rough, porosity = 205 if not mortar else 235, 170
                    if lx % 16 == 1:
                        nx = 165
                    elif lx % 16 == 15:
                        nx = 90
                    if ly % 8 == 1:
                        ny = 165
                    elif ly % 8 == 7:
                        ny = 90
                    nz = 245
                elif tile_id == TILE_WOOD:
                    grain = math.sin(ly * 0.9 + n * 0.8) * 10
                    seam = (ly % 8 == 0)
                    r = int((128 if seam else 156) + grain)
                    g = int((94 if seam else 118) + grain * 0.8)
                    b = int((62 if seam else 78) + grain * 0.6)
                    rough, porosity = 180, 175
                elif tile_id == TILE_LEAVES:
                    hole = ((lx * 3 + ly * 5) % 11 == 0) and (n > 0.05)
                    r, g, b = int(52 + n * 24), int(112 + n * 30), int(44 + n * 18)
                    a = 0 if hole else 255
                    rough, porosity = 230, 140
                elif tile_id == TILE_WATER:
                    r, g, b, a = 42, 128, 172, 255
                    rough, porosity = 5, 0
                elif tile_id == TILE_GLASS:
                    border = lx < 2 or lx > 29 or ly < 2 or ly > 29
                    r, g, b, a = (210, 230, 240, 220) if border else (185, 220, 238, 72)
                    rough, porosity = 6, 0
                elif tile_id == TILE_METAL:
                    r, g, b = int(196 + n * 12), int(202 + n * 12), int(210 + n * 12)
                    rough, metal, porosity = 32, 255, 0
                elif tile_id == TILE_LANTERN:
                    r, g, b = 255, 206, 120
                    rough, emis, porosity = 110, 255, 0
                elif tile_id == TILE_SAND:
                    r, g, b = int(208 + n * 18), int(192 + n * 16), int(146 + n * 14)
                    rough, porosity = 245, 240

                clamp = lambda v: max(0, min(255, int(v)))
                put(
                    tx + lx, ty + ly,
                    (clamp(b), clamp(g), clamp(r), clamp(a)),
                    (clamp(nz), clamp(ny), clamp(nx), clamp(porosity)),
                    (clamp(emis), clamp(metal), clamp(rough), clamp(ao)),
                )

    def pack_atlas(kind, data):
        return frame(kind, struct.pack(">6H", 0, 1, size, size, 0, size) + bytes(data))

    return pack_atlas(6, albedo) + pack_atlas(27, normal) + pack_atlas(28, pbr)


def terrain_height(wx, wz):
    """Return integer surface Y for the scenic lake basin, near shore, and distant mountains."""
    # Lake centered around (-42, 0), elongated along X and Z
    # Shore near X = +24 on the east, distant mountains starting around X = -104
    lake_nx = (wx + 42.0) / 68.0
    lake_nz = wz / 82.0
    r = math.sqrt(lake_nx * lake_nx + lake_nz * lake_nz)
    shore_warp = 0.08 * math.sin(wz * 0.075) + 0.05 * math.cos(wx * 0.06 - wz * 0.05)
    d = r + shore_warp

    if wx > 20 and -16 <= wz <= 26:
        # Flat grassy bluff for camera and lakeside cabin
        base = 65.0 + max(0.0, (wx - 28.0) * 0.14)
        return int(round(base))

    if d < 0.96:
        # Smooth lake bed dipping from 62 at shore down to 52 in center
        depth = (1.0 - d / 0.96) ** 1.35 * 11.0
        return int(round(62.5 - depth))

    # Rising banks and distant mountain range on the west horizon (wx < -92)
    bank = min(6.0, (d - 0.96) * 14.0)
    hills = 3.5 * math.sin(wx * 0.045 + 0.4) * math.cos(wz * 0.04) + 2.0 * math.sin((wx + wz) * 0.09)
    west_ridge = 0.0
    if wx < -95:
        t = min(1.0, (-95.0 - wx) / 55.0)
        peak1 = math.exp(-((wz - 18.0) / 38.0) ** 2) * 28.0
        peak2 = math.exp(-((wz + 42.0) / 34.0) ** 2) * 34.0
        peak3 = math.exp(-((wz - 78.0) / 40.0) ** 2) * 24.0
        west_ridge = (t ** 1.2) * (10.0 + peak1 + peak2 + peak3 + 5.0 * math.sin(wz * 0.13))
    return int(round(63.0 + bank + max(0.0, hills) + west_ridge))


def build_scene():
    """Build chunks covering MC X in [-160, 64), Z in [-112, 112), sea level Y=63."""
    sections = {}  # (cx, cy, cz) -> [bytearray for 6 buckets]
    heightmaps = {}  # (cx, cz) -> [256 ints: (precipType << 12) | valid | (y - minY)]
    lights_map = {}  # (cx, cy, cz) -> list of (lx, ly, lz, r, g, b, level, flags)

    def bucket(wx, wy, wz, layer):
        cx, cy, cz = wx >> 4, wy >> 4, wz >> 4
        key = (cx, cy, cz)
        arr = sections.get(key)
        if arr is None:
            arr = [bytearray() for _ in range(6)]
            sections[key] = arr
        return arr[layer], wx - (cx << 4), wy - (cy << 4), wz - (cz << 4)

    def add_quad(wx, wy, wz, layer, q):
        arr, _, _, _ = bucket(wx, wy, wz, layer)
        arr.extend(q)

    min_y = -64
    sea_level = 63

    # Precompute heights across X in [-160, 64), Z in [-112, 112)
    x_min, x_max = -160, 64
    z_min, z_max = -112, 112
    h_grid = {}
    for wx in range(x_min - 1, x_max + 1):
        for wz in range(z_min - 1, z_max + 1):
            h_grid[(wx, wz)] = terrain_height(wx, wz)

    # Record MOTION_BLOCKING heights (topmost non-air block Y+1)
    top_y = {}

    for cx in range(x_min >> 4, x_max >> 4):
        for cz in range(z_min >> 4, z_max >> 4):
            hm = [0] * 256
            heightmaps[(cx, cz)] = hm
            for lz in range(16):
                wz = (cz << 4) + lz
                for lx in range(16):
                    wx = (cx << 4) + lx
                    y = h_grid[(wx, wz)]
                    is_water = y < sea_level
                    top_tile = TILE_SAND if y <= sea_level + 1 else (TILE_STONE if y >= 79 else TILE_GRASS)
                    side_tile = TILE_SAND if y <= sea_level + 1 else (TILE_STONE if y >= 76 else TILE_DIRT)

                    # Top face of column
                    arr, bx, by, bz = bucket(wx, y, wz, 0)
                    arr.extend(quad(1, ((bx, by + 1, bz), (bx, by + 1, bz + 1), (bx + 1, by + 1, bz + 1), (bx + 1, by + 1, bz)), top_tile))

                    # Exposed cliff sides against lower neighbors
                    for dx, dz, face_id in ((0, -1, 2), (0, 1, 3), (-1, 0, 4), (1, 0, 5)):
                        ny = h_grid[(wx + dx, wz + dz)]
                        if ny < y:
                            for sy in range(ny + 1, y + 1):
                                s_arr, sbx, sby, sbz = bucket(wx, sy, wz, 0)
                                t = side_tile if sy < y or y >= 78 else top_tile
                                s_arr.extend(box_quads(sbx, sby, sbz, sbx + 1, sby + 1, sbz + 1, t, t, faces="UDNSWE"[face_id]))

                    if is_water:
                        # Water surface at Y = sea_level (63 -> top face at 63.88 like vanilla water)
                        w_arr, wbx, wby, wbz = bucket(wx, sea_level, wz, 4)
                        wy_top = wby + 0.88
                        w_arr.extend(quad(1, ((wbx, wy_top, wbz), (wbx, wy_top, wbz + 1),
                                              (wbx + 1, wy_top, wbz + 1), (wbx + 1, wy_top, wbz)),
                                          TILE_WATER, color=(70, 150, 200, 255)))
                        top_y[(wx, wz)] = sea_level + 1
                    else:
                        top_y[(wx, wz)] = y + 1

    # Add a lakeside timber & stone cabin with a wide 2-block overhanging porch roof at X in [21, 29], Z in [7, 15]
    # Floor at Y = 66 (stone patio extending out to X=19..30, Z=5..17 so we can clearly see dry under eaves vs wet open patio!)
    floor_y = 66
    for wx in range(19, 31):
        for wz in range(5, 18):
            arr, bx, by, bz = bucket(wx, floor_y, wz, 0)
            arr.extend(box_quads(bx, by, bz, bx + 1, by + 1, bz + 1, TILE_STONE, TILE_STONE, faces="UDNSWE"))
            top_y[(wx, wz)] = max(top_y.get((wx, wz), 0), floor_y + 1)

    # Cabin walls: X in [23, 29], Z in [8, 15], height Y = 67..70
    for wy in range(67, 71):
        for wx in range(23, 30):
            for wz in (8, 15):
                is_window = (wy in (68, 69)) and (wx in (25, 26, 27))
                layer = 5 if is_window else 0
                tile = TILE_GLASS if is_window else TILE_WOOD
                arr, bx, by, bz = bucket(wx, wy, wz, layer)
                arr.extend(box_quads(bx, by, bz, bx + 1, by + 1, bz + 1, tile, tile))
                top_y[(wx, wz)] = max(top_y.get((wx, wz), 0), wy + 1)
        for wz in range(9, 15):
            for wx in (23, 29):
                is_open = (wx == 23) and (wz in (11, 12)) and (wy in (67, 68))
                is_window = (wx == 23) and (wz in (9, 14)) and (wy == 68)
                if is_open:
                    continue
                layer = 5 if is_window else 0
                tile = TILE_GLASS if is_window else TILE_WOOD
                arr, bx, by, bz = bucket(wx, wy, wz, layer)
                arr.extend(box_quads(bx, by, bz, bx + 1, by + 1, bz + 1, tile, tile))
                top_y[(wx, wz)] = max(top_y.get((wx, wz), 0), wy + 1)

    # Porch posts at X=20, Z=7 and Z=16
    for wz in (7, 16):
        for wy in range(67, 71):
            arr, bx, by, bz = bucket(20, wy, wz, 0)
            arr.extend(box_quads(bx + 0.3, by, bz + 0.3, bx + 0.7, by + 1, bz + 0.7, TILE_WOOD, TILE_WOOD))

    # Overhanging pitched roof: X in [19, 30], Z in [6, 17] (covers porch X=19..22!)
    for step in range(4):
        ry = 71 + step
        for wx in range(19 + step, 31 - step):
            for wz in range(6 + step, 18 - step):
                arr, bx, by, bz = bucket(wx, ry, wz, 0)
                arr.extend(box_quads(bx, by, bz, bx + 1, by + 1, bz + 1, TILE_WOOD, TILE_STONE))
                top_y[(wx, wz)] = max(top_y.get((wx, wz), 0), ry + 1)

    # Warm glowing lantern hanging under the porch eave at (21, 69, 10) + one on the dock pier
    for lpos in ((21, 69, 10), (20, 66, 2)):
        lx, ly, lz = lpos
        arr, bx, by, bz = bucket(lx, ly, lz, 0)
        arr.extend(box_quads(bx + 0.3, by + 0.15, bz + 0.3, bx + 0.7, by + 0.8, bz + 0.7, TILE_LANTERN, TILE_LANTERN))
        sec_key = (lx >> 4, ly >> 4, lz >> 4)
        lights_map.setdefault(sec_key, []).append((lx & 15, ly & 15, lz & 15, 255, 196, 118, 15, 0))

    # Wooden pier jutting into the lake at X in [14, 24], Z in [-2, 3], Y = 64
    for wx in range(14, 25):
        for wz in range(-2, 3):
            arr, bx, by, bz = bucket(wx, 64, wz, 0)
            arr.extend(box_quads(bx, by + 0.65, bz, bx + 1, by + 1.0, bz + 1, TILE_WOOD, TILE_WOOD))
            top_y[(wx, wz)] = max(top_y.get((wx, wz), 0), 65)

    # A few trees on the near bank and distant west shore
    trees = [
        (28, -12), (24, -22), (31, 22), (18, 25),
        (-106, -18), (-112, 6), (-108, 28), (-118, -36), (-115, 42),
    ]
    for tx, tz in trees:
        base_y = h_grid.get((tx, tz), 65)
        if base_y < sea_level:
            continue
        trunk_h = 5
        for ty in range(base_y + 1, base_y + 1 + trunk_h):
            arr, bx, by, bz = bucket(tx, ty, tz, 0)
            arr.extend(box_quads(bx, by, bz, bx + 1, by + 1, bz + 1, TILE_WOOD, TILE_WOOD))
            top_y[(tx, tz)] = max(top_y.get((tx, tz), 0), ty + 1)
        for dy in range(trunk_h - 1, trunk_h + 3):
            rad = 2 if dy < trunk_h + 1 else 1
            for dx in range(-rad, rad + 1):
                for dz in range(-rad, rad + 1):
                    if abs(dx) == rad and abs(dz) == rad and dy > trunk_h - 1:
                        continue
                    wx, wy, wz = tx + dx, base_y + 1 + dy, tz + dz
                    arr, bx, by, bz = bucket(wx, wy, wz, 3)  # Foliage bucket
                    arr.extend(box_quads(bx, by, bz, bx + 1, by + 1, bz + 1, TILE_LEAVES, TILE_LEAVES))
                    top_y[(wx, wz)] = max(top_y.get((wx, wz), 0), wy + 1)

    # Fill precipitation heightmaps (with biome test strip: Z < -45 -> SNOW, Z > 50 -> NONE/DESERT, middle -> RAIN)
    for (cx, cz), hm in heightmaps.items():
        for lz in range(16):
            wz = (cz << 4) + lz
            ptype = 2 if wz < -45 else (0 if wz > 50 else 1)
            for lx in range(16):
                wx = (cx << 4) + lx
                h_rel = max(0, min(2047, top_y.get((wx, wz), 64) - min_y))
                hm[lz * 16 + lx] = (1 << 14) | (ptype << 12) | h_rel

    return sections, heightmaps, lights_map


def water_face(role=0, motion=0, flow=(0, 0), strength=0, disturbance=0, exposure=1,
               edges=0, corners=(56, 56, 56, 56), drop=0, impact=0, from_lip=0, to_toe=0, amount=8):
    mode = role | (motion << 2) | (16 if motion == 0 else 0) | (32 if motion == 2 else 0)
    return struct.pack(">BBbb12B", mode, amount, *flow, strength, disturbance, exposure, edges,
                       *corners, drop, impact, from_lip, to_toe)


def encode_section(cx, cy, cz, buckets, protocol=26, water=None, shores=()):
    counts = [len(b) // 101 * 4 for b in buckets]
    body = struct.pack(">3i6I", cx, cy, cz, *counts) + b"".join(buckets)
    if protocol >= 26:
        water_records = water if water is not None else [water_face()] * (counts[4] // 4)
        assert len(water_records) == counts[4] // 4 and all(len(r) == 16 for r in water_records)
        body += struct.pack(">II", len(water_records), len(shores)) + b"".join(water_records)
        for bucket, index, start, end in shores:
            body += struct.pack(">BI6h", bucket, index, *(round(c * 64) for c in (*start, *end)))
    return frame(2, body)


def water_column(y=None, depth=0, *, sky=True, obstacle=False, min_y=-64):
    known = 1 << 26
    if y is None:
        return known
    base, frac = divmod(round((y - min_y) * 64), 64)
    return known | (1 << 27) | (int(sky) << 28) | (int(obstacle) << 30) | (min(31, depth) << 17) | frac << 11 | base


def encode_water_grid(scene, cx, cz, cells):
    return frame(37, struct.pack(">Iii256I", scene, cx, cz, *cells))


def encode_lights(scene, cx, cy, cz, records):
    body = struct.pack(">I3iH", scene, cx, cy, cz, len(records))
    for lx, ly, lz, r, g, b, lvl, flags in records:
        body += struct.pack(">H5B", lx | (ly << 4) | (lz << 8), r, g, b, lvl, flags)
    return frame(25, body)


def encode_chunk_precip(scene, cx, cz, cells):
    return frame(35, struct.pack(">Iii", scene, cx, cz) + struct.pack(">256H", *cells))


def encode_lightning(scene, entity_id, x, y, z, seed, remaining=3.0):
    return frame(36, struct.pack(">II3dIf", scene, entity_id, x, y, z, seed, remaining))


def encode_env_v23(scene, sun_angle, rain, snow, sky_rgb, fog_rgb):
    return frame(24, struct.pack(">IBB3fI3f", scene, 0, 0, sun_angle, rain, snow, sky_rgb, *fog_rgb))


def encode_env_v24(scene, sun_angle, global_rain, thunder, local_rain, local_snow, sky_flash, sky_rgb, fog_rgb,
                   moon_phase=0, game_time=120.0):
    return frame(24, struct.pack(
        ">IBBBB6fI3ff",
        scene, 0, 0, moon_phase, 0,
        sun_angle, global_rain, thunder, local_rain, local_snow, sky_flash,
        sky_rgb, *fog_rgb, game_time,
    ))


def camera_frame(scene, protocol):
    # Camera on east shore looking west (-X) over the lake, cabin eaves on the right, mountains & sun ahead.
    pos = (33.5, 70.2, -5.0)
    fx, fy, fz = -1.0, 0.045, 0.095
    flen = math.sqrt(fx * fx + fy * fy + fz * fz)
    fx, fy, fz = fx / flen, fy / flen, fz / flen
    # Compute orthogonal up with +Y pointing upward: right = normalize(forward x (0, 1, 0)) = normalize(-fz, 0, fx)
    rx, ry, rz = -fz, 0.0, fx
    rlen = math.sqrt(rx * rx + rz * rz)
    rx, rz = rx / rlen, rz / rlen
    # up = right x forward (yields positive uy)
    ux = ry * fz - rz * fy
    uy = rz * fx - rx * fz
    uz = rx * fy - ry * fx
    body = struct.pack(">I3d8fI", scene, *pos, fx, fy, fz, ux, uy, uz, 68.0, 16.0 / 9.0, 0)
    if protocol >= 25:
        body += struct.pack(">B3d", 0, 0, 0, 0)
    return frame(5, body)


class Log:
    def __init__(self, path):
        self.path = path

    def wait(self, expression, *, after=0, timeout=120):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if self.path.exists():
                with self.path.open("rb") as stream:
                    stream.seek(after)
                    data = stream.read().decode(encoding="utf-8", errors="replace")
                if re.search(expression, data):
                    line = next(line for line in data.splitlines() if re.search(expression, line))
                    print(line, flush=True)
                    return line
            time.sleep(0.15)
        raise TimeoutError(f"No {expression!r} after {after} in {self.path}")

    def end(self):
        return self.path.stat().st_size if self.path.exists() else 0


def update_quality(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(value + "\n", encoding="ascii")
    temporary.replace(path)


def send_env(stream, protocol, scene, state):
    if protocol == 23:
        stream.sendall(encode_env_v23(
            scene, state["sun"], state["local_rain"], state["local_snow"], state["sky"], state["fog"]
        ))
    else:
        stream.sendall(encode_env_v24(
            scene, state["sun"], state["global_rain"], state["thunder"],
            state["local_rain"], state["local_snow"], state.get("flash", 0.0),
            state["sky"], state["fog"], state.get("moon_phase", 0), state.get("time", 120.0),
        ))


STATES = [
    ("NoonClear", dict(sun=0.52, global_rain=0.0, thunder=0.0, local_rain=0.0, local_snow=0.0,
                       sky=0x78A7FF, fog=(0.72, 0.83, 0.98), time=180.0)),
    ("Sunset",    dict(sun=1.47, global_rain=0.0, thunder=0.0, local_rain=0.0, local_snow=0.0,
                       sky=0xC87A52, fog=(0.76, 0.48, 0.34), time=360.0)),
    ("Rain",      dict(sun=0.72, global_rain=0.92, thunder=0.35, local_rain=0.92, local_snow=0.0,
                       sky=0x5C6878, fog=(0.45, 0.50, 0.56), time=520.0)),
    ("Night",     dict(sun=4.35, global_rain=0.0, thunder=0.0, local_rain=0.0, local_snow=0.0,
                       sky=0x070B18, fog=(0.04, 0.06, 0.12), moon_phase=0, time=720.0)),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("quality", type=Path)
    parser.add_argument("shots", type=Path)
    parser.add_argument("--port", type=int, default=39782)
    parser.add_argument("--protocol", type=int, choices=(23, 24, 25, 26, 27, 28), default=28)
    parser.add_argument("--presets-only", action="store_true")
    parser.add_argument("--high-only", action="store_true")
    parser.add_argument("--rounds", type=int, choices=range(1, 6), default=3)
    args = parser.parse_args()

    log = Log(args.log)
    reset = frame(1, struct.pack(">IIQ", 0x4D435545, args.protocol, 0x534B5931))
    sections, heightmaps, lights_map = build_scene()

    with ExitStack() as resources:
        if args.protocol >= 27:
            from bridge_v27_replay import BridgeReplay
            stream = camera = resources.enter_context(BridgeReplay(args.port, version=args.protocol))
        else:
            stream = resources.enter_context(socket.create_connection(("127.0.0.1", args.port), timeout=15))
            camera = resources.enter_context(socket.create_connection(("127.0.0.1", args.port + 1), timeout=15))
            stream.sendall(reset)
            camera.sendall(reset)
        stream.sendall(frame(11, struct.pack(">Iii", 1, -64, 384)))
        stream.sendall(build_atlases())
        stream.sendall(frame(16, struct.pack(">BHBBB", 12, 0, 0, 2, 4)))
        send_env(stream, args.protocol, 1, STATES[0][1])
        camera.sendall(camera_frame(1, args.protocol))
        for (cx, cy, cz), buckets in sections.items():
            metadata = None
            if args.protocol >= 26:
                metadata = []
                for start in range(0, len(buckets[4]), 101):
                    vx, _, vz = struct.unpack_from(">3f", buckets[4], start + 1)
                    x, z = cx * 16 + math.floor(vx + 0.01), cz * 16 + math.floor(vz + 0.01)
                    edges = sum((terrain_height(x + dx, z + dz) >= 63) << edge
                                for edge, (dx, dz) in enumerate(((-1, 0), (1, 0), (0, -1), (0, 1))))
                    metadata.append(water_face(edges=edges))
            mesh = encode_section(cx, cy, cz, buckets, args.protocol, water=metadata)
            if args.protocol >= 27:
                mesh += encode_lights(1, cx, cy, cz, lights_map.get((cx, cy, cz), ()))
            stream.sendall(mesh)
        for (cx, cy, cz), records in lights_map.items():
            lights = encode_lights(1, cx, cy, cz, records)
            if args.protocol < 27:
                stream.sendall(lights)
            elif (cx, cy, cz) not in sections:
                stream.sendall(encode_section(cx, cy, cz, [b""] * 6, args.protocol) + lights)
        if args.protocol >= 24:
            for (cx, cz), cells in heightmaps.items():
                stream.sendall(encode_chunk_precip(1, cx, cz, cells))
                if args.protocol >= 26:
                    water = []
                    for i, precip in enumerate(cells):
                        x, z = cx * 16 + i % 16, cz * 16 + i // 16
                        floor = terrain_height(x, z)
                        water.append(water_column(63.88, min(31, 63 - floor), sky=(precip & 2047) == 128)
                                     if floor < 63 else water_column())
                    stream.sendall(encode_water_grid(1, cx, cz, water))
        stream.sendall(frame(15, struct.pack(">I", 1)))

        def collect_shot(preset, tag, before_files):
            # A screenshot queued for a previous preset can reach disk after its metrics.
            raw_name = re.compile(fr"\d+-{re.escape(preset)}\.png")
            target = args.shots / f"{tag}-{preset}.png"
            deadline = time.monotonic() + 10.0
            while time.monotonic() < deadline:
                for shot in sorted(set(args.shots.glob("*.png")) - before_files):
                    if raw_name.fullmatch(shot.name):
                        try:
                            shot.replace(target)
                        except PermissionError:
                            continue
                        print(f"SHOT {target.name}", flush=True)
                        return
                time.sleep(0.2)
            raise TimeoutError(f"Missing screenshot for {tag}-{preset}")

        def capture_tagged(preset, tag):
            start = log.end()
            before_files = set(args.shots.glob("*.png"))
            update_quality(args.quality, preset)
            log.wait(fr"BridgeQualityMetrics {preset} input=\d+x\d+ output=\d+x\d+", after=start, timeout=180)
            collect_shot(preset, tag, before_files)

        # First wait for initial Baseline warmup so everything is settled
        log.wait(r"BridgeQualityMetrics Baseline input=\d+x\d+ output=\d+x\d+", timeout=180)
        # Clear initial Baseline screenshot if emitted
        for existing in list(args.shots.glob("*.png")):
            existing.unlink()

        if args.high_only:
            for tag, state in STATES:
                update_quality(args.quality, "Baseline")
                time.sleep(1.4)
                send_env(stream, args.protocol, 1, state)
                time.sleep(4.0 if tag == "Rain" else 1.0)
                capture_tagged("High", tag)
                for _ in range(args.rounds - 1):
                    log.wait(r"BridgeQualityMetrics High input=\d+x\d+ output=\d+x\d+",
                             after=log.end(), timeout=150)
            print("BENCH_PASS", flush=True)
            return

        # 1) Capture 4 presets on Sunset (the primary hero + performance benchmark view)
        send_env(stream, args.protocol, 1, STATES[1][1])
        time.sleep(1.0)
        for preset in ("Performance", "Balanced", "High", "Ultra"):
            capture_tagged(preset, "02-Sunset")

        if not args.presets_only:
            # 2) Capture NoonClear, Rain, Night on Ultra (and High for NoonClear)
            # Bounce via High -> Ultra so Revision always increments
            send_env(stream, args.protocol, 1, STATES[0][1])
            time.sleep(1.0)
            capture_tagged("High", "01-NoonClear")
            capture_tagged("Ultra", "01-NoonClear")

            if args.protocol >= 24:
                # 02b: Low-angle fiery golden dusk (2.6 deg sun elevation)
                dusk = dict(sun=1.525, global_rain=0.0, thunder=0.0, local_rain=0.0, local_snow=0.0,
                            sky=0xB85838, fog=(0.72, 0.36, 0.22), time=410.0)
                send_env(stream, args.protocol, 1, dusk)
                time.sleep(0.6)
                capture_tagged("High", "02b-GoldenDusk")
                capture_tagged("Ultra", "02b-GoldenDusk")

            send_env(stream, args.protocol, 1, STATES[2][1])
            time.sleep(6.0)
            capture_tagged("High", "03-Rain")
            capture_tagged("Ultra", "03-Rain")

            if args.protocol >= 24:
                # Thunder + real Minecraft LightningBolt strike across the lake
                storm = dict(sun=1.22, global_rain=1.0, thunder=1.0, local_rain=1.0, local_snow=0.0,
                             flash=0.85, sky=0x384252, fog=(0.25, 0.30, 0.36), time=580.0)
                send_env(stream, args.protocol, 1, storm)
                # Switch quality first, then fire bolt right as screenshot triggers
                capture_tagged("High", "05-ThunderStorm")
                # Fire a persistent 4.5s bolt over the lake ridge for the Ultra hero storm frame
                stream.sendall(encode_lightning(1, 9001, -92.0, 68.0, 4.0, 0x7A31C9, 4.8))
                time.sleep(0.1)
                start = log.end()
                before_files = set(args.shots.glob("*.png"))
                update_quality(args.quality, "Ultra")
                # Re-fire bolt near the end of warmup+sampling so the captured frame catches peak arc + cloud glow
                time.sleep(6.5)
                stream.sendall(encode_lightning(1, 9002, -92.0, 68.0, 4.0, 0x7A31C9, 4.5))
                log.wait(r"BridgeQualityMetrics Ultra input=\d+x\d+ output=\d+x\d+", after=start, timeout=180)
                collect_shot("Ultra", "05-ThunderLightning", before_files)

            send_env(stream, args.protocol, 1, STATES[3][1])
            time.sleep(2.0)
            capture_tagged("High", "04-Night")
            capture_tagged("Ultra", "04-Night")

        if args.quality.exists():
            args.quality.unlink()
        print("BENCH_PASS", flush=True)


if __name__ == "__main__":
    main()
