"""Render the bridge, waterfall, ice margin and rain on a synthetic protocol replay."""

import argparse
import math
from pathlib import Path
import re
import struct
import time

from bench_sky_lake import (
    Log, STATES, TILE_GLASS, TILE_STONE, TILE_WATER, TILE_WOOD, encode_chunk_precip,
    encode_env_v24, encode_lightning, encode_water_grid, frame, quad, water_column, water_face,
)
from bridge_v27_replay import BridgeReplay, frames
from validate_water_p2 import PLANE, TINTS, pose, section, wait_image
from validate_water_v26 import Fixture, MIN_Y, SCENE, translate_world


def site():
    fixture = Fixture()
    pillars = {(-4, 7), (-4, 8), (3, 7), (3, 8)}
    roof = {(x, z) for x in range(-11, -5) for z in range(-3, 7)}
    ice = {(x, z) for x in range(11, 14) for z in range(20, 26)}
    plant = {(0, 2), (0, 3)}
    waterfall = (8, 12)
    toes = {(7, 12), (8, 11)}
    obstacles = pillars | roof | ice | plant | toes | {waterfall}
    for x in range(-24, 24):
        for z in range(-16, 40):
            shallow_shelf = -13 <= x <= 6 and -7 <= z <= 11
            fixture.box(x, 77 if shallow_shelf else 74, z, "U", TILE_STONE)
            p = (x, z)
            if p in pillars:
                for y in range(77, 85):
                    fixture.box(x, y, z, "NEWSU", TILE_STONE)
                continue
            if p in ice:
                fixture.box(x, 80, z, "NEWSU", TILE_GLASS, layer=5)
                continue
            if p in roof:
                fixture.box(x, 85, z, "DU", TILE_WOOD)
            if p in plant:
                fixture.box(x, 80, z, "NSEW", TILE_WOOD, layer=1, height=0.64)
            if p == waterfall:
                for y in range(81, 91):
                    params = dict(drop=10, from_lip=90 - y, to_toe=y - 81)
                    record = water_face(1, 2, **params)
                    fixture.box(x, y, z, "NEWS", TILE_WATER, layer=4, metadata=(record,) * 4)
                    if y >= 86:
                        fixture.box(x + 1, y, z, "NWEU", TILE_STONE)
            y = 90 if p == waterfall else 80
            target, records, lx, ly, lz = section(fixture, x, y, z)
            h = PLANE % 1.0
            verts = [(lx, ly + h, lz + 1), (lx + 1, ly + h, lz + 1),
                     (lx + 1, ly + h, lz), (lx, ly + h, lz)]
            target[4].extend(quad(6, verts, TILE_WATER, TINTS["ocean"]))
            target[4].extend(quad(6, list(reversed(verts)), TILE_WATER, TINTS["ocean"]))
            record = water_face(0, impact=255 if p in toes else 0)
            records.extend((record, record))
            fixture.columns[p] = y + h, True
    for x in range(-12, 13):
        for z in (7, 8):
            fixture.box(x, 84, z, "NSEWUD", TILE_WOOD)

    world = bytearray()
    for kind, body in frames(fixture.world()):
        if kind not in (15, 35, 37):
            world += frame(kind, body)
    precip = bytearray()
    roof_override = bytearray()
    for cx in range(-2, 2):
        for cz in range(-1, 3):
            grid, normal_precip, blocked_precip = [], [], []
            for i in range(256):
                p = (cx * 16 + i % 16, cz * 16 + i // 16)
                water = fixture.columns.get(p)
                near = [(p[0] + dx, p[1] + dz) for dx, dz in ((-1, 0), (1, 0), (0, -1), (0, 1))]
                dry = p in pillars or p in ice or not water
                ice_rim = any(n in ice for n in near)
                obstacle = p in obstacles or any(n in obstacles for n in near)
                shallow_shelf = -13 <= p[0] <= 6 and -7 <= p[1] <= 11
                grid.append(water_column() if dry else water_column(water[0], 3 if shallow_shelf else 6, obstacle=obstacle)
                            | 3 << 22 | int(ice_rim) << 29)
                altitude = 86 if p in roof else 85 if p[1] in (7, 8) and -12 <= p[0] <= 12 else (
                    math.ceil(water[0]) if water else 75)
                normal_precip.append((1 << 14) | (1 << 12) | (altitude - MIN_Y))
                blocked_precip.append((1 << 14) | (1 << 12) | (99 - MIN_Y))
            world += encode_water_grid(SCENE, cx, cz, grid)
            precip += encode_chunk_precip(SCENE, cx, cz, normal_precip)
            roof_override += encode_chunk_precip(SCENE, cx, cz, blocked_precip)
    return bytes(world + precip + frame(15, struct.pack(">I", SCENE))), bytes(precip), bytes(roof_override)


def dynamic_box(mesh, half):
    x, y, z = half
    faces = (
        ((0, 1, 0), [(-x, y, -z), (-x, y, z), (x, y, z), (x, y, -z)]),
        ((-1, 0, 0), [(-x, -y, -z), (-x, -y, z), (-x, y, z), (-x, y, -z)]),
        ((1, 0, 0), [(x, -y, z), (x, -y, -z), (x, y, -z), (x, y, z)]),
        ((0, 0, -1), [(x, -y, -z), (-x, -y, -z), (-x, y, -z), (x, y, -z)]),
        ((0, 0, 1), [(-x, -y, z), (x, -y, z), (x, y, z), (-x, y, z)]),
    )
    data = bytearray()
    for normal, positions in faces:
        for p, uv in zip(positions, ((0.05, 0), (0.05, 0.2), (0.25, 0.2), (0.25, 0))):
            data += struct.pack(">5f4B3bB", *p, *uv, 210, 162, 103, 255, *(n * 127 for n in normal), 0xF0)
    return frame(30, struct.pack(">IBIIBBI", SCENE, 0, mesh, 1, 0, 4, len(faces) * 4) + data)


def instances(boat, item=None, dx=0, dz=0):
    values = [(1, 0x80000011, boat)]
    if item:
        values.append((2, 0x15, item))
    data = bytearray(struct.pack(">IBfH", SCENE, 0, 0.0, len(values)))
    for mesh, part, p in values:
        data += struct.pack(">II3d5f", mesh, part, p[0] + dx, p[1], p[2] + dz, 0, 0, 0, 1, 1)
    return frame(31, data)


def weather(storm=False, flash=0.0):
    baseline = STATES[0][1]
    intensity = float(storm)
    return encode_env_v24(SCENE, baseline["sun"], intensity, intensity, intensity, 0.0, flash,
                          baseline["sky"], baseline["fog"])


def run(args):
    output = args.output
    world, exposed, covered = site()
    if args.x_offset or args.z_offset:
        world = translate_world(world, args.x_offset, args.z_offset)
        exposed = translate_world(exposed, args.x_offset, args.z_offset)
        covered = translate_world(covered, args.x_offset, args.z_offset)
    log = Log(output / "renderer.log")
    (output / "world.bin").write_bytes(world)
    with BridgeReplay(args.port, timeout=120, version=args.protocol) as replay:
        def camera(eye, direction):
            replay.publish_camera(pose(eye, direction, 69.0, args.x_offset, args.z_offset))

        def shot(label):
            temp = output / "capture-request.tmp"
            temp.write_text(label, encoding="ascii")
            temp.replace(output / "capture-request.txt")
            print(f"{label} {wait_image(output / (label + '.png'))}", flush=True)

        motion_eye = (0.2, 82.3, 0.25)
        motion_direction = (0.0, -0.36, 1.0)
        camera(motion_eye, motion_direction)
        replay.sendall(world)
        replay.wait_until(lambda: replay.ready_scene == SCENE and not replay.terrain_leases, timeout=160)
        log.wait(r"WaterP3 .*falls=(?:[1-9]|1[0-2])", timeout=30)
        replay.sendall(dynamic_box(1, (0.61, 0.28, 0.88)) + dynamic_box(2, (0.12, 0.12, 0.12)))
        if args.scenario == "motion":
            camera((0.2, 84.6, 0.25), motion_direction)
            start = time.monotonic()
            while (elapsed := time.monotonic() - start) < 4.4:
                drop = max(0.0, elapsed - 1.1)
                y = 84.6 - min(2.75, 5.2 * drop * drop)
                x = 0.2 + min(2.0, max(0.0, elapsed - 1.6) * 1.7)
                z = 0.25 + max(0.0, elapsed - 2.15) * 0.98
                camera((x, y, z), motion_direction)
                boat = (-1.0, PLANE - 0.08, 2.0 + elapsed * 2.6)
                item = (1.0, max(PLANE - 0.2, 83.2 - 4.4 * drop * drop), 4.2)
                replay.sendall(instances(boat, item, args.x_offset, args.z_offset))
                replay.receive(0)
                time.sleep(0.024)
            shot("bridge-wake")
            log.wait(r"WaterP3 player entry", timeout=2)
            log.wait(r"WaterP3 entity entry", timeout=2)
            log.wait(r"WaterP3 .*hulls=1", timeout=2)
            camera((4.8, 83.2, 9.7), (0.79, -0.31, 0.57))
            time.sleep(0.8)
            shot("waterfall")
        else:
            camera((0.1, 84.0, -1.5), (0.0, -0.19, 1.0))
            replay.sendall(weather(True))
            log.wait(r"WaterP3 .* rain=[1-9][0-9]*", timeout=18)
            time.sleep(2.2)
            shot("storm")
            replay.sendall(weather(True, 1.0) + encode_lightning(
                SCENE, 51, 2.0 + args.x_offset, PLANE, 26.0 + args.z_offset, 23))
            shot("lightning")
            replay.sendall(weather(True) + covered)
            before = log.end()
            time.sleep(0.75)
            log.wait(r"WaterP3 .* rain=0", after=before, timeout=12)
            shot("roof-masked-rain")
            if args.perf_seconds:
                replay.sendall(exposed)
                time.sleep(args.perf_seconds)

    messages = log.path.read_text(encoding="utf-8", errors="replace")
    if re.search(r"Failed to compile Material|Fatal error|Assertion failed|GPU Crashed", messages):
        raise RuntimeError(f"Rendering errors in {log.path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--port", type=int, default=39844)
    parser.add_argument("--protocol", type=int, required=True)
    parser.add_argument("--scenario", choices=("motion", "storm"), default="motion")
    parser.add_argument("--x-offset", type=int, default=0)
    parser.add_argument("--z-offset", type=int, default=0)
    parser.add_argument("--perf-seconds", type=float, default=0)
    arguments = parser.parse_args()
    if arguments.x_offset % 16 or arguments.z_offset % 16:
        parser.error("Offsets must be section aligned")
    run(arguments)
