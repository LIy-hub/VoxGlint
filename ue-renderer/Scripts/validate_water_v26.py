"""Create and replay the v26 mixed-interface and stacked-water fixtures."""

import argparse
import math
import socket
import struct
import time
from pathlib import Path

from bench_sky_lake import (
    Log, STATES, TILE_GLASS, TILE_STONE, TILE_WATER, TILE_WOOD, box_quads,
    build_atlases, encode_chunk_precip, encode_env_v24, encode_section, encode_water_grid,
    frame, water_column, water_face,
)

SCENE = 1
MIN_Y = -64
CAMERAS = {
    "glass-fall": ((-4.0, 69.4, -7.0), (3.6, 66.0, 3.0)),
    "stacked-water": ((-0.5, 72.3, -8.5), (3.8, 66.5, 4.0)),
}


def packet_camera(name, offset=0.0):
    eye, target = CAMERAS[name]
    eye = (eye[0] + offset, eye[1], eye[2])
    dx, dy, dz = (b - a for a, b in zip(eye, target))
    length = math.sqrt(dx * dx + dy * dy + dz * dz)
    dx, dy, dz = dx / length, dy / length, dz / length
    horizontal = math.hypot(dx, dz)
    rx, rz = -dz / horizontal, dx / horizontal
    up = (-rz * dy, rz * dx - rx * dz, rx * dy)
    return frame(5, struct.pack(">I3d8fIB3d", SCENE, *eye, dx, dy, dz, *up,
                                47.0, 16 / 9, 0, 0, 0.0, 0.0, 0.0))


class Fixture:
    def __init__(self):
        self.sections = {}
        self.columns = {}

    def box(self, x, y, z, faces, tile, *, layer=0, height=1.0, metadata=(), shore=None):
        cx, cy, cz = x >> 4, y >> 4, z >> 4
        key = cx, cy, cz
        buckets, water, shores = self.sections.setdefault(key, ([bytearray() for _ in range(6)], [], []))
        lx, ly, lz = x & 15, y & 15, z & 15
        first = len(buckets[layer]) // 101
        tint = (70, 150, 200, 255) if layer == 4 else (255, 255, 255, 255)
        data = box_quads(lx, ly, lz, lx + 1, ly + height, lz + 1, tile, tile, color=tint, faces=faces)
        buckets[layer].extend(data)
        assert len(metadata) * 101 == len(data) if layer == 4 else not metadata
        water.extend(metadata)
        if shore is not None:
            assert len(data) == 101
            shores.append((layer, first, *shore))

    def surface(self, x, y, z, *, exposed=True, impact=0, edges=0, height=0.875, flow=(0, 0), strength=0):
        self.box(x, y, z, "U", TILE_WATER, layer=4, height=height,
                 metadata=(water_face(0, bool(strength), flow=flow, strength=strength, exposure=int(exposed),
                                      impact=impact, edges=edges, corners=(round(height * 64),) * 4),))
        old = self.columns.get((x, z))
        surface = y + height
        if old is None or surface > old[0]:
            self.columns[x, z] = surface, exposed

    def world(self, weather=0.0):
        reset = frame(1, struct.pack(">IIQ", 0x4D435545, 26, 0x574154455231))
        output = bytearray(reset + frame(11, struct.pack(">Iii", SCENE, MIN_Y, 384)) + build_atlases())
        output.extend(frame(16, struct.pack(">BHBBB", 8, 0, 0, 2, 4)))
        noon = STATES[0][1]
        output.extend(encode_env_v24(SCENE, noon["sun"], weather, 0.0, 0.0, 0.0, 0.0,
                                     noon["sky"], noon["fog"]))
        for (x, y, z), (buckets, water, shores) in sorted(self.sections.items()):
            output.extend(encode_section(x, y, z, buckets, water=water, shores=shores))
        for cx in range(-1, 2):
            for cz in range(-1, 2):
                precip, water = [], []
                for i in range(256):
                    x, z = cx * 16 + i % 16, cz * 16 + i // 16
                    surface = self.columns.get((x, z))
                    water.append(water_column(surface[0], 1, sky=surface[1]) if surface else water_column())
                    top = math.ceil(surface[0]) + (0 if surface[1] else 4) if surface else 64
                    precipitation = 1 if z >= 0 else 0
                    precip.append((1 << 14) | (precipitation << 12) | (top - MIN_Y))
                output.extend(encode_chunk_precip(SCENE, cx, cz, precip))
                output.extend(encode_water_grid(SCENE, cx, cz, water))
        output.extend(frame(15, struct.pack(">I", SCENE)))
        return bytes(output)


def glass_fall():
    scene = Fixture()
    for x in range(-6, 15):
        for z in range(-9, 15):
            scene.box(x, 63, z, "U", TILE_STONE)
    for y in range(64, 74):
        scene.box(2, y, 3, "E", TILE_GLASS, layer=5)
        loft, toe = 73 - y, y - 64
        record = water_face(1, 2, amount=8, drop=10, from_lip=loft, to_toe=toe)
        contact = water_face(2, 2, amount=8, drop=10, from_lip=loft, to_toe=toe)
        # Vanilla side winding faces north/west. Glass shares the west plane.
        scene.box(3, y, 3, "NW", TILE_WATER, layer=4, metadata=(record, contact))
    scene.surface(3, 73, 3)
    scene.surface(6, 65, 5)
    scene.box(6, 64, 5, "NW", TILE_WATER, layer=4,
              metadata=(water_face(1, 2, drop=1), water_face(1, 2, drop=1)))
    for x in range(2, 10):
        for z in range(0, 3):
            impact = max(0, 255 - abs(x - 3) * 65 - (2 - z) * 90)
            scene.surface(x, 64, z, impact=impact, flow=(127, 0), strength=165)
    return scene


def stacked_water():
    scene = Fixture()
    for x in range(-5, 15):
        for z in range(-7, 14):
            scene.box(x, 63, z, "U", TILE_STONE)
    for x in range(2, 7):
        for z in range(0, 7):
            scene.surface(x, 64, z, exposed=z < 3, edges=2 if x == 6 else 0, height=0.125 if z < 3 else 0.875)
            if z >= 3:
                scene.box(x, 67, z, "UD", TILE_STONE)
                scene.surface(x, 68, z, edges=2 if x == 6 else 0)
    for z in range(0, 7):
        for y in (64, 65, 68, 69):
            ly, lz = y & 15, z & 15
            water_y = (64.125 if z < 3 else 64.875) if y < 68 else 68.875
            fixed = water_y - (y & ~15)
            line = ((7.0, fixed, lz), (7.0, fixed, lz + 1))
            scene.box(7, y, z, "W", TILE_WOOD, shore=line if ly <= fixed + 0.35 else None)
            scene.box(7, y, z, "E", TILE_WOOD)
    return scene


def translate_world(world, x_offset, z_offset):
    shifted = bytearray(world)
    start = 0
    while start < len(shifted):
        size, kind = struct.unpack_from(">IB", shifted, start)
        if kind == 2:
            x, y, z = struct.unpack_from(">3i", shifted, start + 5)
            struct.pack_into(">3i", shifted, start + 5, x + x_offset // 16, y, z + z_offset // 16)
        elif kind in (35, 37):
            x, z = struct.unpack_from(">2i", shifted, start + 9)
            struct.pack_into(">2i", shifted, start + 9, x + x_offset // 16, z + z_offset // 16)
        start += size + 4
    assert start == len(shifted)
    return shifted


def replay(name, source, port, log_path, screenshot_dir, x_offset=0, z_offset=0, check_v25=False, protocol=28):
    log = Log(log_path)
    world = source.read_bytes()
    camera_pose = bytearray(packet_camera(name))
    reset_length = struct.unpack_from(">I", world)[0] + 4
    if x_offset or z_offset:
        world = translate_world(world, x_offset, z_offset)
        x, y, z = struct.unpack_from(">3d", camera_pose, 9)
        struct.pack_into(">3d", camera_pose, 9, x + x_offset, y, z + z_offset)
    if check_v25:
        previous = log.end()
        with socket.create_connection(("127.0.0.1", port), timeout=15) as old_client:
            old_client.sendall(frame(1, struct.pack(">IIQ", 0x4D435545, 25, 0x574154455231)))
            log.wait(fr"protocol mismatch: Fabric=25, renderer={protocol}", after=previous, timeout=15)
            print(f"PASS protocol mismatch Fabric=25, renderer={protocol}", flush=True)
    def wait_for_shot():
        log.wait(r"BridgeQualityMetrics High input=\d+x\d+ output=\d+x\d+", timeout=150)
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            shots = sorted(screenshot_dir.glob("*-High.png"))
            if shots and shots[-1].stat().st_size > 0:
                print(f"PASS {name} {shots[-1]}", flush=True)
                return
            time.sleep(0.2)
        raise TimeoutError(f"UE did not capture {name}")

    if protocol >= 27:
        from bridge_v27_replay import BridgeReplay
        with BridgeReplay(port, version=protocol) as stream:
            stream.publish_camera(camera_pose)
            stream.sendall(world)
            stream.wait_until(lambda: stream.ready_scene == 1 and not stream.terrain_leases)
            wait_for_shot()
    else:
        with (
            socket.create_connection(("127.0.0.1", port), timeout=15) as stream,
            socket.create_connection(("127.0.0.1", port + 1), timeout=15) as camera,
        ):
            stream.sendall(world[:reset_length])
            camera.sendall(world[:reset_length])
            stream.sendall(world[reset_length:])
            camera.sendall(camera_pose)
            wait_for_shot()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--replay", choices=CAMERAS)
    parser.add_argument("--port", type=int, default=39926)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--shots", type=Path)
    parser.add_argument("--offset-x", type=int, default=0)
    parser.add_argument("--offset-z", type=int, default=0)
    parser.add_argument("--protocol", type=int, choices=(26, 27, 28), default=28)
    parser.add_argument("--check-v25", action="store_true")
    args = parser.parse_args()
    if args.offset_x % 16 or args.offset_z % 16:
        parser.error("Offsets must be multiples of 16 blocks")
    args.output.mkdir(parents=True, exist_ok=True)
    for name, builder in (("glass-fall", glass_fall), ("stacked-water", stacked_water)):
        path = args.output / f"{name}.bin"
        if args.replay is None:
            path.write_bytes(builder().world())
            print(f"{path} {path.stat().st_size} bytes")
        elif name == args.replay:
            replay(name, path, args.port, args.log, args.shots, args.offset_x, args.offset_z, args.check_v25, args.protocol)


if __name__ == "__main__":
    main()

