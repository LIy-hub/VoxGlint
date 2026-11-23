"""Drive a fixed scene over loopback without a Minecraft window or save."""

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


def quad(face, vertices, color=(255, 255, 255, 255), light=0xF0, masked=False):
    left, right = (0.53, 0.96) if masked else (0.03, 0.46)
    uvs = ((left, 0.05), (left, 0.95), (right, 0.95), (right, 0.05))
    return bytes([face]) + b"".join(struct.pack(">5f5B", *point, *uv, *color, light)
                                      for point, uv in zip(vertices, uvs))


def floor(x, z):
    return quad(1, ((x, 0, z), (x, 0, z + 1), (x + 1, 0, z + 1), (x + 1, 0, z)))


def wall(x, z, *, color=(255, 255, 255, 255), light=0xF0):
    return quad(2, ((x, 0, z), (x, 4, z), (x + 2, 4, z), (x + 2, 0, z)), color, light, True)


def section(x, z, solid, masked):
    return frame(2, struct.pack(">3i6I", x, 0, z, len(solid) // 101 * 4, len(masked) // 101 * 4, 0, 0, 0, 0)
                 + solid + masked + struct.pack(">II", 0, 0))


def atlas(kind):
    colors = bytearray()
    for y in range(32):
        for x in range(64):
            if kind == 6:
                rgb = (72, 124, 152) if x < 32 else (80, 179, 65)
                shade = 0 if (x // 4 + y // 4) % 2 else 34
                alpha = 255 if x < 32 or x % 9 < 3 or y % 15 < 3 else 0
                colors.extend((*[i + shade for i in rgb], alpha))
            elif kind == 27:
                colors.extend((255, 128, 128, 255))
            else:
                colors.extend((0, 0, 160, 255))
    return frame(kind, struct.pack(">6H", 0, 1, 64, 32, 0, 32) + colors)


def lights(z, *records):
    body = struct.pack(">I3iH", 1, 0, 0, z, len(records))
    for x, y, local_z, flags in records:
        body += struct.pack(">H5B", x | (y << 4) | (local_z << 8), 255, 168, 98, 15, flags)
    return frame(25, body)


def camera_frame(player_view=0, feet=(0, 0, 0)):
    tilt = 0.23
    return frame(5, struct.pack(">I3d8fIB3d", 1, 7.5, 9.0, -16.5, 0, -math.sin(tilt), math.cos(tilt),
                                0, math.cos(tilt), math.sin(tilt), 68, 16 / 9, 0, player_view, *feet))


def dynamic_mesh(space=0):
    positions = ((-0.3, 0.0, 0.0), (-0.3, 0.6, 0.0), (0.3, 0.6, 0.0), (0.3, 0.0, 0.0))
    uv = ((0.05, 0), (0.05, 1), (0.45, 1), (0.45, 0))
    return frame(30, struct.pack(">IBIIBBI", 1, space, 1, 1, 0, 4, 4)
                 + b"".join(struct.pack(">5f4B3bB", *p, *t, 218, 250, 255, 255, 0, 0, -127, 0xF0)
                            for p, t in zip(positions, uv)))


def instance(position=None, turn=0.0, *, space=0):
    count = 0 if position is None else 1
    rigid_part = 17 if space == 0 else 0
    return frame(31, struct.pack(">IBfH", 1, space, 0, count)
                 + (struct.pack(">II3d5f", 1, rigid_part, *position, 0, math.sin(turn * 0.5), 0, math.cos(turn * 0.5), 1)
                    if count else b""))


class Log:
    def __init__(self, path):
        self.path = path

    def wait(self, expression, *, after=0, timeout=45):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with self.path.open("rb") as stream:
                stream.seek(after)
                data = stream.read().decode(encoding="utf-8", errors="replace")
            if re.search(expression, data):
                line = next(line for line in data.splitlines() if re.search(expression, line))
                print(line, flush=True)
                return line
            time.sleep(0.1)
        raise TimeoutError(f"No {expression!r} after {after} in {self.path}")

    def end(self):
        return self.path.stat().st_size


def update_quality(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(value + "\n", encoding="ascii")
    temporary.replace(path)


def exercise(stream, camera, log, base_floor, base_wall, quality):
    start = log.end()
    stream.sendall(section(0, 0, base_floor, wall(6, 12, color=(223, 192, 255, 255), light=0x70) + wall(2, 15)))
    log.wait(r"TerrainRegion updates=1 layout=0 positions=0 shadowNotify=0 blas=0", after=start)
    start = log.end()
    stream.sendall(section(0, 0, base_floor, wall(6, 12, color=(223, 192, 255, 0)) + wall(2, 15)))
    log.wait(r"TerrainRegion updates=1 layout=0 positions=0 shadowNotify=1 blas=0", after=start)
    start = log.end()
    stream.sendall(section(0, 0, base_floor, wall(7, 12) + wall(2, 15)))
    log.wait(r"TerrainRegion updates=1 layout=0 positions=1 shadowNotify=1 blas=1", after=start)
    stream.sendall(section(0, 0, base_floor, base_wall))

    start = log.end()
    stream.sendall(lights(0, (7, 2, 15, 0x48)))
    log.wait(r"Point light .*X=7 Y=2 Z=15 face=3 kind=1 position=X=1620\.000", after=start)
    start = log.end()
    stream.sendall(lights(0, (7, 2, 15, 0x70)))
    log.wait(r"Point light .*X=7 Y=2 Z=15 face=4 kind=2 position=X=1550\.000 Y=-698\.500", after=start)
    log.wait(r"Point light .*X=7 Y=2 Z=15 face=5 kind=2 position=X=1550\.000 Y=-801\.500", after=start)
    log.wait(r"SceneLighting .*active=2 pool=3", after=start)
    start = log.end()
    stream.sendall(lights(0))
    log.wait(r"SceneLighting .*active=0 pool=3", after=start)
    stream.sendall(lights(0, (7, 2, 15, 0x62)))

    start = log.end()
    stream.sendall(dynamic_mesh() + instance((7, 1.5, -1)))
    log.wait(r"RigidDynamic .*parts=1 uploads=1 transformUpdates=0 rayTracingParts=1", after=start)
    start = log.end()
    for i in range(48):
        stream.sendall(instance((7 + i * 0.02, 1.5, -1), i * 0.08))
        time.sleep(1 / 30)
    log.wait(r"RigidDynamic .*parts=1 uploads=1 transformUpdates=[1-9]\d* rayTracingParts=1", after=start)
    # A displacement larger than four blocks must be a discontinuous update.
    start = log.end()
    stream.sendall(instance((7, 1.5, 9)))
    log.wait(r"RigidDynamic .*parts=1 uploads=1 .*rayTracingParts=1", after=start)
    start = log.end()
    stream.sendall(instance())
    log.wait(r"DynamicMeshes .*worldInstances=0", after=start)

    # The same player mesh can move between world and first-person shadow rendering without a new batch.
    stream.sendall(dynamic_mesh(space=2) + instance((0, 0, 0), space=2))
    for view, feet in ((2, (7.5, 7.38, -12.5)), (1, (7.5, 7.38, -16.5)), (0, (0, 0, 0))):
        start = log.end()
        camera.sendall(camera_frame(view, feet))
        log.wait(fr"DynamicMeshes .*worldInstances=0 .*playerBatches=1 playerInstances=1 playerMeshes=1 playerView={view}", after=start)
    start = log.end()
    stream.sendall(instance(space=2))
    log.wait(r"DynamicMeshes .*playerBatches=2 playerInstances=0", after=start)

    for preset in ("Performance", "Balanced", "High", "Ultra", "Baseline"):
        start = log.end()
        update_quality(quality, preset)
        log.wait(fr"BridgeQualityMetrics {preset} input=\d+x\d+ output=2560x1440", after=start, timeout=90)
    quality.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("quality", type=Path)
    parser.add_argument("--port", type=int, default=39682)
    parser.add_argument("--protocol", type=int, choices=(26, 27, 28), default=28)
    parser.add_argument("--one-preset", help="Measure this launch state without switching or editing geometry")
    args = parser.parse_args()
    log = Log(args.log)
    reset = frame(1, struct.pack(">IIQ", 0x4D435545, args.protocol, 0x254652))
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
        stream.sendall(atlas(6) + atlas(27) + atlas(28))
        stream.sendall(frame(16, struct.pack(">BHBBB", 8, 0, 0, 0, 1)))
        stream.sendall(frame(24, struct.pack(">IBBBB6fI3ff", 1, 0, 0, 0, 0, 0.58, 0.0, 0.0, 0.0, 0.0, 0.0, 0x88A9FF, 0.3, 0.45, 0.66, 120.0)))
        camera.sendall(camera_frame())
        base_floor = b"".join(floor(x, z) for x in range(16) for z in range(16))
        base_wall = wall(6, 12) + wall(2, 15)
        patterned = b"".join(wall(x, z) for x in (1, 7, 12) for z in (5, 12))
        for x in range(-4, 5):
            for z in range(-3, 8):
                stream.sendall(section(x, z, base_floor, base_wall if (x, z) == (0, 0) else patterned))
        stream.sendall(frame(15, struct.pack(">I", 1)))
        if args.one_preset:
            stream.sendall(lights(0, (7, 2, 15, 0x62)))
            log.wait(fr"BridgeQualityMetrics {args.one_preset} input=\d+x\d+ output=2560x1440", timeout=90)
        else:
            log.wait(r"BridgeQualityMetrics Baseline input=\d+x\d+ output=2560x1440", timeout=90)
            exercise(stream, camera, log, base_floor, base_wall, args.quality)
        print("PASS", flush=True)


if __name__ == "__main__":
    main()
