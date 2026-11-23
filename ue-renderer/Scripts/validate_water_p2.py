"""Replay protocol 27 water optics with explicit eye poses and aligned reference captures."""

import argparse
import hashlib
import math
from pathlib import Path
import re
import struct
import time

from bench_sky_lake import (
    Log, STATES, TILE_GLASS, TILE_LANTERN, TILE_STONE, TILE_WATER, TILE_WOOD,
    encode_chunk_precip, encode_env_v24, encode_lights, encode_water_grid,
    frame, quad, water_column, water_face,
)
from bridge_v27_replay import BridgeReplay
from validate_water_v26 import Fixture, MIN_Y, SCENE, glass_fall, stacked_water, translate_world

PLANE = 80.875
FLOOR = 64.0
FOV = 70.0
TINTS = {
    "ocean": (63, 118, 228, 255),
    "warm": (67, 213, 238, 255),
    "swamp": (97, 123, 100, 255),
}


def pose(eye, direction, fov, dx=0, dz=0):
    length = math.sqrt(sum(v * v for v in direction))
    fx, fy, fz = (v / length for v in direction)
    horizontal = math.hypot(fx, fz)
    rx, rz = (-fz / horizontal, fx / horizontal) if horizontal > 1e-8 else (-1.0, 0.0)
    return frame(5, struct.pack(">I3d8fIB3d", SCENE, eye[0] + dx, eye[1], eye[2] + dz,
                                fx, fy, fz, -rz * fy, rz * fx - rx * fz, rx * fy, fov, 16 / 9,
                                0, 0, 0, 0, 0))


def env(state="NoonClear", wet=False):
    values = next((item for title, item in STATES if title.lower() == state.lower()), STATES[0][1])
    message = bytearray(encode_env_v24(SCENE, values["sun"], values.get("global_rain", 0.0),
        values.get("thunder", 0.0), values.get("local_rain", 0.0), 0.0, 0.0, values["sky"], values["fog"]))
    message[10] = int(wet)
    return bytes(message)


def section(scene, x, y, z):
    cx, cy, cz = x >> 4, y >> 4, z >> 4
    buckets, water, shores = scene.sections.setdefault((cx, cy, cz), ([bytearray() for _ in range(6)], [], []))
    return buckets, water, x & 15, y & 15, z & 15


def top(scene, x, z, slope, color):
    buckets, records, lx, ly, lz = section(scene, x, int(PLANE), z)
    h0 = PLANE - int(PLANE)
    h1 = h0 + (slope if x == 0 else 0.0)
    vertices = [(lx, ly+h0, lz+1), (lx+1, ly+h1, lz+1),
                (lx+1, ly+h1, lz), (lx, ly+h0, lz)]
    buckets[4].extend(quad(6, vertices, TILE_WATER, color))
    buckets[4].extend(quad(6, list(reversed(vertices)), TILE_WATER, color))
    record = water_face(corners=(round(h0*64), round(h1*64), round(h0*64), round(h1*64)))
    records.extend((record, record))


def pool_fixture(kind, biome):
    scene = Fixture()
    roof = set()
    bed = 78.0 if kind == "shallow" else FLOOR
    for x in range(-24, 24):
        for z in range(-24, 24):
            scene.box(x, int(bed)-1, z, "U", TILE_STONE)
            if kind != "dry":
                top(scene, x, z, -0.125 if kind == "slope" else 0.0, TINTS[biome])
                scene.columns[(x, z)] = PLANE, True
            if kind == "occlusion":
                if -7 <= x <= 2 and 0 <= z <= 10:
                    scene.box(x, 84, z, "UD", TILE_WOOD)
                    roof.add((x, z))
                if 4 <= x <= 7 and 0 <= z <= 9:
                    scene.box(x, 76, z, "UDNSWE", TILE_STONE)
                if x == -8 and 0 <= z <= 10:
                    for y in range(65, 79):
                        scene.box(x, y, z, "EW", TILE_STONE)
    # A bright, tall marker remains behind an upwards-pointing camera below the surface.
    for x in range(2, 6):
        for y in range(64, 69):
            scene.box(x, y, -7, "NEWSU", TILE_WOOD)
    if kind == "tunnel":
        for z in range(-12, 11):
            for y in range(74, 77):
                scene.box(0, y, z, "W", TILE_GLASS, layer=5)
                scene.box(1, y, z, "E", TILE_GLASS, layer=5)
                scene.box(-1, y, z, "E", TILE_WATER, layer=4, metadata=(water_face(2),))
                scene.box(2, y, z, "W", TILE_WATER, layer=4, metadata=(water_face(2),))
            for x in (0, 1):
                scene.box(x, 76, z, "U", TILE_GLASS, layer=5)
    if kind == "reflection-marked":
        # This marker is behind the direct frustum of the upward-looking camera.
        # Reflected rays at 51–60 degrees reach it from below the surface.
        for x in range(14, 18):
            for y in range(73, 77):
                for z in range(-2, 3):
                    if (x == 14 or z in (-2, 2)) and (y + z) % 3 != 0:
                        scene.box(x, y, z, "NEWSUD", TILE_LANTERN)
    payload = bytearray(scene.world())
    ready_bytes = len(frame(15, struct.pack(">I", SCENE)))
    del payload[-ready_bytes:]
    for cx in range(-2, 2):
        for cz in range(-2, 2):
            water, precip = [], []
            for i in range(256):
                p = (cx * 16 + i % 16, cz * 16 + i // 16)
                present = p in scene.columns
                optical = {"swamp": 1, "warm": 2, "ocean": 3}[biome]
                column_bed = 77.0 if kind == "tunnel" and p[0] in (0, 1) and -12 <= p[1] <= 10 else bed
                water.append((water_column(PLANE, math.ceil(PLANE - column_bed), sky=p not in roof) | optical << 22)
                             if present else water_column())
                altitude = 85 if p in roof else math.ceil(PLANE) if present else 64
                precip.append((1 << 14) | (1 << 12) | (altitude - MIN_Y))
            payload.extend(encode_water_grid(SCENE, cx, cz, water))
            payload.extend(encode_chunk_precip(SCENE, cx, cz, precip))
    if kind == "occlusion":
        payload.extend(encode_lights(SCENE, 0, 4, 0, [(3, 11, 1, 108, 230, 255, 15, 0)]))
    payload.extend(frame(15, struct.pack(">I", SCENE)))
    return bytes(payload)


def wait_image(path):
    end = time.monotonic() + 22.0
    while time.monotonic() < end:
        try:
            data = path.read_bytes()
        except (FileNotFoundError, PermissionError):
            pass  # Unreal may still hold its Windows screenshot handle.
        else:
            if data.endswith(b"IEND\xaeB\x60\x82"):
                return hashlib.sha256(data).hexdigest()
        time.sleep(0.08)
    raise TimeoutError(f"Timed out waiting for {path}")


def request_capture(output, label):
    temp = output / "capture-request.tmp"
    target = output / "capture-request.txt"
    temp.write_text(label, encoding="ascii")
    deadline = time.monotonic() + 5
    while True:
        try:
            temp.replace(target)
            return
        except PermissionError as error:
            # UE briefly holds the previous request open without FILE_SHARE_DELETE on Windows.
            if getattr(error, "winerror", None) not in (5, 32, 33) or time.monotonic() >= deadline:
                raise
            time.sleep(.025)


def capture(stream, output, label, eye, direction, fov, dx, dz, weather, wet, settle):
    stream.publish_camera(pose(eye, direction, fov, dx, dz))
    stream.sendall(env(weather, wet))
    time.sleep(settle)
    request_capture(output, label)
    digest = wait_image(output / f"{label}.png")
    print(f"{label}\t{eye}\t{direction}\t{fov}\t{int(wet)}\t{digest}", flush=True)


def views(name, motion=False):
    if name == "tunnel":
        return [
            ("glass-tunnel-dry", (0.12, 75.2, -2.75), (0, 0, 1), FOV),
            ("glass-tunnel-wet", (-0.12, 75.2, -2.75), (0, 0, 1), FOV),
        ]
    if name.startswith("reflection-"):
        return [("offscreen-tir", (0.1, PLANE-6.0, 0.2), (0, 1, 0), 100.0)]
    if name == "dry":
        return [
            ("dry-ground", (0.5, 87.0, -6.0), (0.18, -0.39, 0.84), FOV),
            # Face the pillar at x=2..5, z=-7 above the dry pool bed.
            ("terrain-depth", (3.5, 66.5, -13.0), (0, 0, 1), FOV),
        ]
    if name == "shallow":
        return [(f"caustic-{i:02d}", (0.4 + i * 0.09, 80.02, -8.0), (0.2, -0.43, 1.0), FOV)
                for i in range(25)] if motion else [
                    ("caustic-underwater", (0.4, 80.02, -8.0), (0.2, -0.43, 1.0), FOV),
                    ("caustic-from-air", (0.4, 82.2, -8.0), (0.2, -0.45, 1.0), FOV),
                ]
    if motion and name == "flat":
        waterline = [(f"line-motion-{i:03d}", (0.42, PLANE - 0.100 + 0.004 * i, -3.8), (0, 0.01, 1), FOV)
                     for i in range(61)]
        snell = [(f"snell-motion-{i:03d}", (0.04 + i * 0.13, PLANE - 6, 0.2), (0.014 * i, 1, 0.1), 100.0)
                 for i in range(39)]
        return waterline + snell
    if name == "flat":
        result = [(f"waterline-{i:02d}", (0.42, PLANE + dy, -3.8), (0, 0, 1), FOV)
                  for i, dy in enumerate((0.10, 0.04, 0.025, 0.010, 0.0, -0.010, -0.025, -0.04, -0.10))]
        result += [(f"plate-{index}-{prefix}", (0.3, FLOOR + depth, -4.5), direction, FOV)
                   for index, depth in enumerate((0.5, 2.0, 5.0, 12.0))
                   for prefix, direction in (("down", (0.0, -1.0, 0.0)), ("oblique", (0.65, -0.75, 0.0)))]
        result += [(f"snell-{depth}", (0.1, PLANE - depth, 0.2), (0, 1, 0), 100.0) for depth in (6, 15)]
        return result
    if name == "slope":
        return [(f"slope-{i:02d}", (0.51, PLANE - 0.063 + dy, -2.3), (0.25, 0, 1), 70.0)
                for i, dy in enumerate((0.06, 0.024, 0.01, 0, -0.01, -0.024, -0.06))]
    if name == "occlusion":
        if motion:
            return [(f"canyon-{i:02d}", (-6.0 + i * 0.27, 76.5, -9.0), (0.23, 0.13, 1.0), 70.0)
                    for i in range(43)]
        return [(f"canyon-{i:02d}", (x, 76.5, -9.0), (0.23, 0.13, 1.0), 70.0)
                for i, x in enumerate((-7.2, -5.2, -3.2, -1.2, 0.8, 2.8, 4.8, 6.8))]
    if name == "stacked-water":
        return [
            ("lower-wet", (3.25, 64.84, 4.4), (0.0, 0.0, 1.0), 70.0),
            ("dry-between", (3.25, 65.85, 4.4), (0.0, 0.0, 1.0), 70.0),
            ("upper-wet", (3.25, 68.84, 4.4), (0.0, 0.0, 1.0), 70.0),
        ]
    return [("fall", (-4.0, 69.4, -7.0), (0.58, -0.26, 0.77), 47.0)]


def verify_dry_terrain_depth(path):
    from PIL import Image

    # P2 debug view 3 encodes near opaque depth in red. Sample away from silhouettes.
    with Image.open(path) as image:
        red = image.convert("RGB").getchannel("R")
        w, h = image.size
        for name, u, v, foreground in (("pillar", 0.5, 0.5, True), ("ground", 0.25, 0.85, True),
                                       ("sky", 0.15, 0.15, False)):
            x, y = int(u * w), int(v * h)
            lo, hi = red.crop((x - 2, y - 2, x + 3, y + 3)).getextrema()
            if (foreground and lo <= 32) or (not foreground and hi > 8):
                raise AssertionError(f"{name}: unexpected opaque depth, red range {lo}..{hi} in {path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--fixture", default="flat", choices=("flat", "slope", "occlusion", "glass-fall", "stacked-water", "dry", "shallow", "reflection-marked", "reflection-clear", "tunnel"))
    parser.add_argument("--biome", default="ocean", choices=tuple(TINTS))
    parser.add_argument("--port", type=int, default=39835)
    parser.add_argument("--x-offset", type=int, default=0)
    parser.add_argument("--z-offset", type=int, default=0)
    parser.add_argument("--weather", default="NoonClear")
    parser.add_argument("--capture-regex", default=".*")
    parser.add_argument("--perf-rounds", type=int, default=0)
    parser.add_argument("--bubbles", action="store_true")
    parser.add_argument("--motion", action="store_true")
    parser.add_argument("--verify-dry-depth", action="store_true")
    args = parser.parse_args()
    if args.x_offset % 16 or args.z_offset % 16:
        parser.error("Offsets must be section aligned")
    base = {"glass-fall": glass_fall, "stacked-water": stacked_water}.get(args.fixture)
    payload = base().world() if base else pool_fixture(args.fixture, args.biome)
    if args.x_offset or args.z_offset:
        payload = translate_world(payload, args.x_offset, args.z_offset)
    (args.output / "world.bin").write_bytes(payload)
    scene_views = [view for view in views(args.fixture, args.motion) if re.fullmatch(args.capture_regex, view[0])]
    if not scene_views:
        parser.error("No matching camera poses")
    start = scene_views[0]
    log = Log(args.output / "renderer.log")
    with BridgeReplay(args.port, timeout=120) as stream:
        stream.publish_camera(pose(start[1], start[2], start[3], args.x_offset, args.z_offset))
        stream.sendall(payload)
        stream.wait_until(lambda: stream.ready_scene == SCENE and not stream.terrain_leases, timeout=140)
        if args.bubbles:
            if not (struct.unpack_from("<Q", stream.control, 16)[0] & 1):
                raise RuntimeError("The renderer did not advertise v27 bubble columns")
            bottom, height = 65 - MIN_Y, 15
            chunk_z = args.z_offset // 16 - 1
            chunk_x = args.x_offset // 16
            runs = [(5 << 4, 0, bottom, height), ((5 << 4) | 2, 1, bottom, height)]
            header = struct.pack(">Iii", SCENE, chunk_x, chunk_z)
            data = b"".join(struct.pack(">BBHH", *run) for run in runs)
            stream.sendall(frame(39, header + struct.pack(">HH", len(runs), 0) + data))
            log.wait(r"WaterBubbles.*runs=2")
            capture(stream, args.output, "bubble-columns", (0.95, 74.8, -15.8), (0, 0, 1), FOV,
                    args.x_offset, args.z_offset, "NoonClear", True, 1.6)
            stream.sendall(frame(39, header + struct.pack(">HH", 0, 0)))
            log.wait(r"WaterBubbles.*runs=0")
            capture(stream, args.output, "bubble-columns-cleared", (0.95, 74.8, -15.8), (0, 0, 1), FOV,
                    args.x_offset, args.z_offset, "NoonClear", True, 1.4)
        elif args.perf_rounds:
            stream.sendall(env(args.weather, start[1][1] < PLANE))
            last = 0
            for _ in range(args.perf_rounds):
                log.wait(r"BridgeQualityMetrics ", after=last, timeout=150)
                last = log.end()
        else:
            for label, eye, direction, fov in scene_views:
                # Geometry must determine the waterline even when the camera-medium event lags a frame.
                wet = eye[1] < PLANE and args.fixture not in ("dry", "glass-fall", "stacked-water", "tunnel")
                capture(stream, args.output, label, eye, direction, fov, args.x_offset, args.z_offset, args.weather, wet, 0.18 if args.motion else 0.9)
                if args.verify_dry_depth and label == "terrain-depth":
                    verify_dry_terrain_depth(args.output / f"{label}.png")
    if re.search(r"Failed to compile Material|Fatal error|Assertion failed", log.path.read_text(encoding="utf-8", errors="replace")):
        raise RuntimeError(f"Rendering errors in {log.path}")


if __name__ == "__main__":
    main()
