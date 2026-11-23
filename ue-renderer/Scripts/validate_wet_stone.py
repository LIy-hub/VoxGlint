"""Capture a fixed stone-brick courtyard with an exposed path, a roof and a reflection pool."""

import argparse
import hashlib
from io import BytesIO
import json
import math
from pathlib import Path
import re
import struct
import time
from zipfile import ZipFile

from PIL import Image, ImageChops, ImageFilter, ImageStat

from bench_sky_lake import (
    TILE_GRASS, TILE_LANTERN, TILE_STONE, TILE_WOOD, box_quads, build_atlases,
    encode_chunk_precip, encode_env_v24, encode_lights, encode_section, encode_water_grid, tile_uv, water_column,
)
from bridge_v27_replay import BridgeReplay, frame, frames
from validate_water_p2 import pose, request_capture, wait_image
from validate_water_v26 import Fixture, MIN_Y, SCENE


STONE = "assets/minecraft/textures/block/stone_bricks.png"
COOL_LAMP = 10
EYE = (0.5, 65.85, -6.6)
LOOK = (0.3, 63.8, 2.6)
FOV = 48.0
EDGE_EYE = (1.75, 64.74, 0.3)
EDGE_LOOK = (0.15, 63.99, 3.65)
EDGE_FOV = 48.0
FOOT_LANE = 2.03
FLAT_EYE = (0.45, 65.35, -4.6)
FLAT_LOOK = (0.85, 64.0, 3.2)
FLAT_CLOSE_EYE = (0.22, 64.72, 1.2)
FLAT_CLOSE_LOOK = (0.82, 64.0, 3.22)


def atlases(jar):
    with ZipFile(jar) as minecraft:
        stone_png = minecraft.read(STONE)
    with Image.open(BytesIO(stone_png)) as raw:
        stone = raw.convert("RGBA")
    if stone.size != (16, 16):
        raise ValueError(f"Expected vanilla 16x16 stone bricks in {jar}; got {stone.size}")
    textures = {kind: bytearray(body) for kind, body in frames(build_atlases())}

    def put(kind, tile, x, y, channels):
        tx, ty = tile % 4 * 32, tile // 4 * 32
        index = 12 + ((ty + y) * 128 + tx + x) * 4
        textures[kind][index:index + 4] = bytes(channels)

    def luma(x, y):
        r, g, b, _ = stone.getpixel((x % 16, y % 16))
        return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0

    for y in range(32):
        for x in range(32):
            u, v = x // 2, y // 2
            r, g, b, alpha = stone.getpixel((u, v))
            put(6, TILE_STONE, x, y, (b, g, r, alpha))
            # The vanilla sprite has albedo only. Keep its visual grain in the substrate's relief,
            # with rougher mortar. The coat normal is authored separately in the material.
            dx = (luma(u - 1, v) - luma(u + 1, v)) * 2.0
            dy = (luma(u, v - 1) - luma(u, v + 1)) * 2.0
            reciprocal_length = 1.0 / math.sqrt(1.0 + dx * dx + dy * dy)
            put(27, TILE_STONE, x, y, (round(255 * reciprocal_length), round(128 + 127 * dy * reciprocal_length),
                                        round(128 + 127 * dx * reciprocal_length), round(255 * 0.65)))
            mortar = min(max((0.40 - luma(u, v)) * 4.0, 0.0), 1.0)
            put(28, TILE_STONE, x, y, (0, 0, round(255 * (0.83 + 0.10 * mortar) ** 2), 255))
            center = int(6 <= x < 26 and 6 <= y < 26)
            put(6, COOL_LAMP, x, y, (188 + 67 * center, 147 + 65 * center, 42 + 44 * center, 255))
            put(27, COOL_LAMP, x, y, (255, 128, 128, 0))
            put(28, COOL_LAMP, x, y, (60 + 195 * center, 0, 40, 255))
    return b"".join(frame(kind, payload) for kind, payload in textures.items()), hashlib.sha256(stone_png).hexdigest()


def environment(rain, clock, sun):
    message = bytearray(encode_env_v24(SCENE, sun, rain, 0.0, rain, 0.0, 0.0,
                                       0x92B7E0, (0.67, 0.78, 0.91), game_time=clock))
    message[12] = 3 | (4 if rain > 0 else 0)
    return bytes(message)


def smooth_ramp(value):
    amount = min(max(value, 0.0), 1.0)
    return amount * amount * (3.0 - 2.0 * amount)


def depression_height(x, z):
    left = x / (0.55 + 0.09 * math.sin(z * 1.85))
    right = (4.0 - x) / (0.53 + 0.08 * math.cos(z * 1.5 + 0.2))
    front = (z + 2.0) / (0.60 + 0.12 * math.cos(x * 1.65))
    return 64.0 - 0.125 * smooth_ramp(min(left, right, front))


def add_depression_surface(scene, x, z):
    cx, cz = x >> 4, z >> 4
    buckets, _, _ = scene.sections.setdefault((cx, 3, cz), ([bytearray() for _ in range(6)], [], []))
    u0, v0 = tile_uv(TILE_STONE)[0]
    u1, v1 = tile_uv(TILE_STONE)[2]
    xs = (0.0, 0.23, 0.5, 0.77, 1.0) if x in (0, 3) else (0.0, 1.0)
    zs = (0.0, 0.23, 0.5, 0.77, 1.0) if z == -2 or x in (0, 3) else (0.0, 1.0)
    for left, right in zip(xs, xs[1:]):
        for front, back in zip(zs, zs[1:]):
            corners = ((left, front), (left, back), (right, back), (right, front))
            heights = [depression_height(x + u, z + v) for u, v in corners]
            buckets[0].append(1 if max(heights) == min(heights) else 6)
            for (u, v), height in zip(corners, heights):
                # Subdivide geometry, with one unchanged vanilla texture mapped across each full block.
                buckets[0].extend(struct.pack(">5f5B", (x & 15) + u, height - 48.0, (z & 15) + v,
                                              u0 + (u1 - u0) * u, v0 + (v1 - v0) * v, 255, 255, 255, 255, 0xF0))


def courtyard(jar, sun, flat=False):
    scene = Fixture()
    tops = {}
    lights = {}

    def box(x, y, z, height, width, tile, faces="UDNSWE"):
        cx, cy, cz = math.floor(x) >> 4, math.floor(y) >> 4, math.floor(z) >> 4
        lx, ly, lz = x - cx * 16, y - cy * 16, z - cz * 16
        buckets, _, _ = scene.sections.setdefault((cx, cy, cz), ([bytearray() for _ in range(6)], [], []))
        buckets[0].extend(box_quads(lx, ly, lz, lx + width, ly + height, lz + width, tile, tile, faces=faces))
        cell = math.floor(x), math.floor(z)
        tops[cell] = max(tops.get(cell, MIN_Y), math.ceil(y + height))

    # Only the rear of the localized depression keeps a discrete step. The other edges slope to the patio.
    pool = {(x, z) for x in range(5, 9) for z in range(3, 9)}
    depression = set() if flat else {(x, z) for x in range(4) for z in range(-2, 3)}
    dry_edge = {(-1, z) for z in range(1, 7)}
    for x in range(-18, 19):
        for z in range(-16, 28):
            porch = -7 <= x <= 9 and -9 <= z <= (13 if flat else 11)
            depressed = (x, z) in depression
            if (x, z) in pool:
                box(x, 62, z, 1, 1, TILE_STONE, "U")
                scene.surface(x, 63, z, height=0.875)
                tops[x, z] = 64
            elif depressed:
                add_depression_surface(scene, x, z)
                tops[x, z] = 64
            else:
                faces = "U" if porch else "UD"
                faces += "".join(face for dx, dz, face in ((1, 0, "E"), (-1, 0, "W"), (0, 1, "S"), (0, -1, "N"))
                                 if (x + dx, z + dz) in pool or ((x + dx, z + dz) in depression and z == 3))
                box(x, 63, z, 1, 1, TILE_STONE if porch else TILE_GRASS, faces)

    for x in range(-7, -1):
        for z in range(-3, 8):
            box(x, 67, z, 0.75, 1, TILE_WOOD)
    for x, z in ((-7, -3), (-2, -3), (-7, 7), (-2, 7)):
        box(x + 0.27, 64, z + 0.27, 3.03, 0.46, TILE_WOOD)
    for x in range(-7, 10):
        box(x, 64, 13 if flat else 10, 0.75, 1, TILE_STONE)

    lamps = ((-5, 1, TILE_LANTERN, (255, 184, 97)),
             (0 if flat else 1, 10 if flat else 4, TILE_LANTERN, (255, 184, 97)),
             (1 if flat else 3, 10 if flat else 5, COOL_LAMP, (116, 187, 255)))
    for x, z, lamp, color in lamps:
        box(x + 0.35, 64, z + 0.35, 1.20, 0.30, TILE_WOOD)
        box(x + 0.215, 65.2, z + 0.215, 0.77, 0.57, lamp)
        lights.setdefault((x >> 4, 4, z >> 4), []).append((x & 15, 1, z & 15, *color, 12, 0))

    texture_data, digest = atlases(jar)
    binary = bytearray(frame(11, struct.pack(">Iii", SCENE, MIN_Y, 384)) + texture_data)
    binary.extend(frame(16, struct.pack(">BHBBB", 10, 0, 0, 2, 4)))
    binary.extend(environment(0, 100.0, sun))
    for (cx, cy, cz), (buckets, water, shores) in sorted(scene.sections.items()):
        binary.extend(encode_section(cx, cy, cz, buckets, water=water, shores=shores))
        binary.extend(encode_lights(SCENE, cx, cy, cz, lights.get((cx, cy, cz), ())))
    for cx, cz in sorted({(x >> 4, z >> 4) for x, z in tops}):
        precip, water = [], []
        for i in range(256):
            position = cx * 16 + i % 16, cz * 16 + i // 16
            if position in tops:
                biome_bits = (1 << 11) if position in dry_edge else (1 << 12)
                precip.append((1 << 14) | biome_bits | (tops[position] - MIN_Y))
            else:
                precip.append(0)
            water.append(water_column(63.875, 1) if position in pool else water_column())
        binary.extend(encode_chunk_precip(SCENE, cx, cz, precip))
        binary.extend(encode_water_grid(SCENE, cx, cz, water))
    binary.extend(frame(15, struct.pack(">I", SCENE)))
    return bytes(binary), digest


def capture(directory, name):
    request_capture(directory, name)
    path = directory / f"{name}.png"
    digest = wait_image(path)
    with Image.open(path) as image:
        # A completed but empty/default color target does not count as a usable sample.
        lo, hi = image.convert("L").getextrema()
        assert image.width >= 800 and image.height >= 450 and hi - lo > 40, (path, image.size, lo, hi)
    print(f"SHOT {name} {digest}", flush=True)
    return digest


def point(text):
    values = tuple(map(float, text.split(",")))
    if len(values) != 3:
        raise argparse.ArgumentTypeError("Supply x,y,z.")
    return values


def player_pose(camera_pose, feet):
    message = bytearray(camera_pose)
    struct.pack_into(">B3d", message, 69, 2, *feet)  # World/local-player body rendering, with the eye kept fixed.
    return bytes(message)


def walk_dry(stream, camera_pose):
    for step in range(12):
        stream.publish_camera(player_pose(camera_pose, (FOOT_LANE, 63.875, -1.0 + 0.24 * step)), movement_flags=3)
        time.sleep(0.055)


def capture_steps(stream, camera_pose, output, prefix="step", flat=False):
    frames_captured = []
    if flat:
        foot_path = ((0.85, 64.0, 1.8 + min(n, 7) * 0.34) for n in range(16))
    else:
        foot_path = ((FOOT_LANE, 63.875, -1.0 + min(n, 7) * 0.42) for n in range(16))
    start = time.monotonic()
    for number, feet in enumerate(foot_path):
        stream.publish_camera(player_pose(camera_pose, feet), movement_flags=3)
        # Keep each pose available long enough for one normal render tick before requesting a sample.
        time.sleep(0.035)
        label = f"{prefix}-{number:02d}"
        requested_at = time.monotonic() - start
        digest = capture(output, label)
        frames_captured.append({"label": label, "feet_mc": feet, "requested_after_seconds": requested_at,
                                "png_sha256": digest})
    assert len(set(frame["png_sha256"] for frame in frames_captured)) >= 10, frames_captured
    return frames_captured


def push_flat_settings(directory, *, depression, waves):
    path = directory / "minecraftue-graphics.txt"
    setting_names = {"puddleDepressions", "waterWaves"}
    lines = [line for line in path.read_text(encoding="utf-8-sig").splitlines()
             if line.partition("=")[0] not in setting_names]
    lines.extend((f"puddleDepressions={int(depression)}", f"waterWaves={int(waves)}"))
    pending = path.with_suffix(".wet.tmp")
    pending.write_text("\n".join(lines) + "\n", encoding="ascii")
    until = time.monotonic() + 8.0
    while True:
        try:
            pending.replace(path)
            break
        except PermissionError:
            if time.monotonic() >= until:
                raise
            time.sleep(0.025)
    effective = directory / "quality.txt.effective"
    status = ""
    until = time.monotonic() + 8.0
    words = (f"PuddleDepressions={int(depression)}", f"WaterWaves={int(waves)}")
    while time.monotonic() < until:
        try:
            status = effective.read_text(encoding="utf-8")
            if all(word in status for word in words):
                time.sleep(0.24)
                return
        except (FileNotFoundError, PermissionError):
            pass
        time.sleep(0.035)
    raise TimeoutError(f"Missing effective values {words}: {status}")


def flat_screen_coord(world, size, fov, eye, look):
    x, y, z = (b - a for a, b in zip(eye, look))
    length = math.sqrt(x * x + y * y + z * z)
    fx, fy, fz = x / length, y / length, z / length
    plane = math.hypot(fx, fz)
    rx, rz = -fz / plane, fx / plane
    ux, uy, uz = -rz * fy, rz * fx - rx * fz, rx * fy
    dx, dy, dz = (a - b for a, b in zip(world, eye))
    depth = dx * fx + dy * fy + dz * fz
    width, height = size
    lens = height / (2 * math.tan(math.radians(fov * 0.5)))
    return (round(width / 2 + lens * (dx * rx + dz * rz) / depth),
            round(height / 2 - lens * (dx * ux + dy * uy + dz * uz) / depth))


def flat_toggle_metrics(directory, fov, *, prefix="flat", eye=FLAT_EYE, look=FLAT_LOOK, require_difference=True):
    images = {}
    for key in ("off", "on", "off-again"):
        with Image.open(directory / f"{prefix}-{key}.png") as image:
            images[key] = image.convert("RGB")

    def box(points):
        projected = [flat_screen_coord(point, images["off"].size, fov, eye, look) for point in points]
        xs, ys = zip(*projected)
        width, height = images["off"].size
        return max(0, min(xs)), max(0, min(ys)), min(width, max(xs) + 1), min(height, max(ys) + 1)

    def texture(images_, region):
        edges = {key: value.crop(region).convert("L").filter(ImageFilter.FIND_EDGES) for key, value in images_.items()}
        return {key: ImageStat.Stat(ImageChops.difference(edges["off"], edges[key])).mean[0]
                for key in ("on", "off-again")}

    wet = box(((0.44, 64, 2.4), (1.38, 64, 4.2), (1.38, 64, 2.4), (0.44, 64, 4.2)))
    dry = box(((-0.72, 64, 3.1), (-0.13, 64, 4.2), (-0.72, 64, 4.2), (-0.13, 64, 3.1)))
    wet_difference, dry_difference = texture(images, wet), texture(images, dry)
    if require_difference:
        assert (wet_difference["on"] > wet_difference["off-again"] + 0.1 and
                wet_difference["on"] > dry_difference["on"] + 0.1), (wet, dry, wet_difference, dry_difference)
    return {"eye_mc": eye, "look_mc": look, "wet_texture_region": wet, "dry_control_region": dry,
            "high_frequency_rgb_luma_difference": {"wet": wet_difference, "dry": dry_difference},
            "source_surface_y": 64.0, "material_writes_pixel_depth_offset": False}


def trace_steps(directory):
    log = directory / "renderer.log"
    trace = re.findall(r"puddleSteps=(\d+) activeFootsteps=(\d+)", log.read_text(encoding="utf-8", errors="replace"))
    return [(int(total), int(active)) for total, active in trace]


def verify_flat_source(world_frames):
    expected = {(x, z) for x in (0, 1) for z in range(1, 5)}
    observed = set()
    for kind, body in frames(world_frames):
        if kind != 2:
            continue
        cx, cy, cz, solid_count = struct.unpack_from(">3iI", body)
        for index in range(solid_count // 4):
            start = 36 + index * 101
            quad = [struct.unpack_from(">3f", body, start + 1 + vertex * 25) for vertex in range(4)]
            column = (cx * 16 + math.floor(min(position[0] for position in quad)),
                      cz * 16 + math.floor(min(position[2] for position in quad)))
            if column not in expected:
                continue
            assert body[start] == 1 and all(cy * 16 + position[1] == 64 for position in quad), (column, quad)
            observed.add(column)
    assert observed == expected, (expected, observed)


def motion_timing(stream, camera_pose, output, sun, rain_seconds, batches, *, motion_off):
    push_flat_settings(output, depression=True, waves=True)
    stream.sendall(environment(0.95, 300.0, sun))
    time.sleep(max(rain_seconds, 10.0))
    csv = output / "frame-timings.csv"

    def rows():
        try:
            return csv.read_bytes().splitlines()[1:]
        except (FileNotFoundError, PermissionError):
            return []

    # Discard the first batch completed after starting the motion, which can include stationary warmup.
    start_group = len(rows()) // 240 + 1
    stop_group = start_group + batches
    before = trace_steps(output)
    started = time.monotonic()
    elapsed = 0.0
    next_check = 0.0
    current = []
    while elapsed < max(75, 10 * batches):
        cycle = elapsed % 5.4
        travel = (smooth_ramp(cycle / 1.6) if cycle < 1.6 else
                  1.0 if cycle < 2.7 else 1.0 - smooth_ramp((cycle - 2.7) / 1.6))
        stream.publish_camera(player_pose(camera_pose, (0.85, 64.0, 1.92 + 1.80 * travel)), movement_flags=3)
        if elapsed >= next_check:
            current = rows()
            if len(current) // 240 >= stop_group:
                break
            next_check = elapsed + 0.25
        time.sleep(0.05)
        elapsed = time.monotonic() - started
    assert len(current) // 240 >= stop_group, (len(current), stop_group)
    selected = current[start_group * 240:stop_group * 240]
    batch_ids = sorted({int(row.split(b",", 1)[0]) for row in selected})
    assert len(selected) == batches * 240 and len(batch_ids) == batches, batch_ids
    trail = trace_steps(output)
    first_count = before[-1][0] if before else 0
    assert trail and (trail[-1][0] == first_count if motion_off else trail[-1][0] > first_count), trail
    result = {"csv": csv.name, "batch_ids": batch_ids, "frames": len(selected), "motion_enabled": not motion_off,
              "scene_rain": 0.95, "depression_enabled": True, "camera_mc": FLAT_EYE,
              "step_path_x": 0.85, "step_path_y": 64.0, "step_path_z": (1.92, 3.72),
              "steps_before": first_count, "steps_after": trail[-1][0]}
    (output / "motion-performance.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"MOTION_TIMING_PASS batches={batch_ids} frames={len(selected)} steps={trail[-1][0] - first_count}", flush=True)


def run_flat_toggle(stream, output, sun, rain_seconds, fov, *, motion_off=False):
    shots = {}
    push_flat_settings(output, depression=False, waves=False)
    shots["flat-dry"] = capture(output, "flat-dry")
    stream.sendall(environment(0.95, 300.0, sun))
    time.sleep(rain_seconds)
    stream.sendall(environment(0.0, 300.0, sun))
    time.sleep(2.4)
    def capture_toggle(prefix):
        for label, depression in (("off", False), ("on", True), ("off-again", False)):
            push_flat_settings(output, depression=depression, waves=False)
            name = f"{prefix}-{label}"
            shots[name] = capture(output, name)

    capture_toggle("flat")
    metrics = {"wide": flat_toggle_metrics(output, fov, require_difference=False)}
    close_pose = pose(FLAT_CLOSE_EYE, tuple(b - a for a, b in zip(FLAT_CLOSE_EYE, FLAT_CLOSE_LOOK)), fov)
    stream.publish_camera(close_pose)
    time.sleep(0.35)
    capture_toggle("flat-close")
    metrics["close"] = flat_toggle_metrics(output, fov, prefix="flat-close", eye=FLAT_CLOSE_EYE, look=FLAT_CLOSE_LOOK)
    movement = {}
    for label, depression in (("off", False), ("on", True)):
        stream.publish_camera(close_pose)
        push_flat_settings(output, depression=depression, waves=True)
        stream.sendall(environment(0.95, 300.0, sun))
        time.sleep(5.0)
        stream.sendall(environment(0.0, 300.0, sun))
        time.sleep(1.0)
        idle = [capture(output, f"flat-{label}-idle-{i}") for i in range(3)]
        before = trace_steps(output)
        captures = capture_steps(stream, close_pose, output, prefix=f"flat-{label}-walk", flat=True)
        time.sleep(1.15)
        after = trace_steps(output)
        start_count = before[-1][0] if before else 0
        assert after, output
        if motion_off:
            assert after[-1] == (start_count, 0), (before, after)
        else:
            assert after[-1][0] > start_count and after[-1][1] == 0, (before, after)
        movement[label] = {"idle_sha256": idle, "steps_before": start_count,
                           "steps_after": after[-1][0], "active_after": after[-1][1], "frames": captures}
    return {"captures": shots, "toggle_metrics": metrics, "motion": movement}


def edge_evidence(image_path, world_frames):
    forward = tuple(b - a for a, b in zip(EDGE_EYE, EDGE_LOOK))
    length = math.sqrt(sum(value * value for value in forward))
    fx, fy, fz = (value / length for value in forward)
    rx, rz = -fz / math.hypot(fx, fz), fx / math.hypot(fx, fz)
    up = (-rz * fy, rz * fx - rx * fz, rx * fy)
    samples = []
    with Image.open(image_path) as source:
        image = source.convert("RGB")
        lens = image.height / (2 * math.tan(math.radians(EDGE_FOV * 0.5)))

        def sample_rgb(world_point):
            dx, dy, dz = (a - b for a, b in zip(world_point, EDGE_EYE))
            distance = dx * fx + dy * fy + dz * fz
            px = round(image.width * 0.5 + lens * (dx * rx + dz * rz) / distance)
            py = round(image.height * 0.5 - lens * (dx * up[0] + dy * up[1] + dz * up[2]) / distance)
            assert 0 <= px < image.width and 0 <= py < image.height, (world_point, px, py)
            return {"mc": world_point, "pixel": (px, py), "rgb": image.getpixel((px, py))}

        # This line lies wholly on flat y=64 stone, past the small rear step in the fixture.
        for x in (-0.16, -0.06, 0.0, 0.025, 0.05, 0.075, 0.10, 0.12, 0.18, 0.26):
            samples.append(dict(sample_rgb((x, 64.0, 3.55)), expected_wet_fade=smooth_ramp(x / 0.075),
                                expected_puddle_fade=smooth_ramp(x / 0.12)))

        normal_frame = next(frame_body for kind, frame_body in frames(world_frames) if kind == 27)
        tile_x, tile_y = TILE_STONE % 4 * 32, TILE_STONE // 4 * 32

        def slope(x, y, channel):
            return (normal_frame[12 + ((tile_y + y) * 128 + tile_x + x) * 4 + channel] - 128) / 127

        # Find a convex and a concave reference on the same flat brick face from its actual normal texture.
        # This finite difference labels the source texture. It is not a readback of shader-space curvature.
        references = []
        for y in range(7, 25):
            for x in range(9, 26):
                signed = (slope(x - 1, y, 2) - slope(x + 1, y, 2) +
                          slope(x, y - 1, 1) - slope(x, y + 1, 1)) * 0.5
                references.append((signed, x, y))
        relief = {}
        for name, (signed, x, y) in (("groove", max(references)), ("crown", min(references))):
            local_x = (x + 0.5 - 1.5) / 29.0
            local_z = (y + 0.5 - 1.5) / 29.0
            relief[name] = dict(sample_rgb((local_x, 64.0, 3 + local_z)), source_normal_curvature=signed)
        assert relief["groove"]["source_normal_curvature"] > 0.0 > relief["crown"]["source_normal_curvature"]

    assert samples[2]["expected_wet_fade"] == samples[2]["expected_puddle_fade"] == 0
    assert samples[5]["expected_wet_fade"] == 1 and samples[5]["expected_puddle_fade"] < 1
    assert samples[7]["expected_puddle_fade"] == 1
    return {"eye_mc": EDGE_EYE, "look_mc": EDGE_LOOK, "fov": EDGE_FOV, "samples": samples,
            "normal_texture_relief_samples": relief}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--port", type=int, default=40716)
    parser.add_argument("--jar", type=Path, required=True)
    parser.add_argument("--dry-only", action="store_true")
    parser.add_argument("--footsteps", action="store_true")
    parser.add_argument("--edge-view", action="store_true")
    parser.add_argument("--flat-toggle", action="store_true")
    parser.add_argument("--motion-off", action="store_true")
    parser.add_argument("--perf-batches", type=int, choices=range(0, 9), default=0)
    parser.add_argument("--eye", type=point, default=EYE)
    parser.add_argument("--look", type=point, default=LOOK)
    parser.add_argument("--fov", type=float, default=FOV)
    parser.add_argument("--sun", type=float, default=0.93)
    parser.add_argument("--rain-seconds", type=float, default=8.0)
    args = parser.parse_args()
    if args.flat_toggle:
        args.eye, args.look = FLAT_EYE, FLAT_LOOK
    world, stone_hash = courtyard(args.jar, args.sun, flat=args.flat_toggle)
    if args.flat_toggle:
        verify_flat_source(world)
    direction = tuple(b - a for a, b in zip(args.eye, args.look))
    camera_pose = pose(args.eye, direction, args.fov)
    print(f"VANILLA_STONE {stone_hash} {args.jar}:{STONE}", flush=True)
    evidence = {"minecraft_jar": str(args.jar), "stone_sprite_sha256": stone_hash, "eye_mc": args.eye,
                "look_mc": args.look, "vertical_fov": args.fov, "sun_angle": args.sun, "captures": {},
                "scene_frame_sha256": hashlib.sha256(world).hexdigest()}
    with BridgeReplay(args.port, timeout=30) as stream:
        stream.publish_camera(camera_pose)
        stream.sendall(world)
        stream.wait_until(lambda: stream.ready_scene == SCENE and not stream.terrain_leases)
        time.sleep(1.4)
        if args.flat_toggle:
            if args.perf_batches:
                motion_timing(stream, camera_pose, args.output, args.sun, args.rain_seconds, args.perf_batches,
                              motion_off=args.motion_off)
                return
            evidence.update(run_flat_toggle(stream, args.output, args.sun, args.rain_seconds, args.fov,
                                            motion_off=args.motion_off))
            (args.output / "wet-stone-captures.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
            print("WET_STONE_FLAT_TOGGLE_PASS", flush=True)
            return
        shots = evidence["captures"]
        shots["01-dry"] = capture(args.output, "01-dry")
        if args.footsteps:
            walk_dry(stream, camera_pose)
            shots["01b-dry-walk"] = capture(args.output, "01b-dry-walk")
            stream.publish_camera(camera_pose)
        if not args.dry_only:
            stream.sendall(environment(0.9, 101.0, args.sun))
            time.sleep(args.rain_seconds)
            shots["02-rain"] = capture(args.output, "02-rain")
            stream.sendall(environment(0, 102.0, args.sun))
            time.sleep(0.55)
            shots["03-after-rain"] = capture(args.output, "03-after-rain")
            assert len({shots[name] for name in ("01-dry", "02-rain", "03-after-rain")}) == 3, shots
            if args.footsteps:
                evidence["footsteps"] = capture_steps(stream, camera_pose, args.output)
            if args.edge_view:
                edge_direction = tuple(b - a for a, b in zip(EDGE_EYE, EDGE_LOOK))
                stream.publish_camera(pose(EDGE_EYE, edge_direction, EDGE_FOV))
                time.sleep(0.3)
                shots["04-wet-edge"] = capture(args.output, "04-wet-edge")
                evidence["edge"] = edge_evidence(args.output / "04-wet-edge.png", world)
    (args.output / "wet-stone-captures.json").write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print("WET_STONE_CAPTURE_PASS", flush=True)


if __name__ == "__main__":
    main()
