"""Render fixed-exposure SectionGlass cases and compare the linear scene probes in UE."""

import argparse
from contextlib import contextmanager
import csv
import json
import math
import os
from pathlib import Path
import random
import socket
import statistics
import struct
import subprocess
import time

from bench_sky_lake import encode_env_v24
from bridge_v27_replay import BridgeReplay, frame, frames
from validate_glass_transport import CASES, DEFAULT_CONTRACTS, decode, ray_transmittance, verify
from validate_water_p2 import pose, request_capture

SCENE = 1
SCENE_READY = frame(15, struct.pack(">I", SCENE))
WALL_Z = 8.25
WALL_PROBE = (8.5, 4.5, WALL_Z)
WALL_CAMERA = (8.5, 4.5, 9.8)
WALL_VIEW = (WALL_CAMERA, WALL_PROBE, 52)
WHITE = (8, 4, 13, 255, 255, 255, 15, 0)
RED = (11, 4, 13, 255, 0, 0, 15, 0)
RED_CENTRE = (8, 4, 13, 255, 0, 0, 15, 0)
RECT = (8, 4, 13, 255, 255, 255, 15, 0xC4)  # North-facing covered side, native area light.
POINT_CENTRE = (8.5, 4.72, 13.5)
SUN_PROBE = (8.5, 2.75, 8.5)
SUN_VIEW = ((7.9, 4.65, 7.9), SUN_PROBE, 50)
SIDE_VIEW = ((8.7, 4.5, 9.75), (10.25, 4.5, 9.75), 65)
RED_TRANSMISSION_TOLERANCE = 0.0045
RED_LEAK_TOLERANCE = 0.0025
EFFECTIVE_SETTINGS = (
    "lumen_diffuse_indirect_allow", "view_uses_lumen_method", "hardware_lighting",
    "virtual_shadow_maps", "mega_lights", "fixed_exposure", "local_exposure_strength",
    "glass_ray_shadow_sources", "glass_vsm_shadow_sources", "glass_planar_grids",
    "exposure_min_ev100", "exposure_max_ev100", "exposure_span_ev100",
)
NIGHT = encode_env_v24(SCENE, 4.35, 0, 0, 0, 0, 0, 0x070B18, (0.04, 0.06, 0.12))
SUN = encode_env_v24(SCENE, 0, 0, 0, 0, 0, 0, 0x78A7FF, (0.72, 0.83, 0.98))
FACE_CORNERS = (
    ((0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)),
    ((0, 1, 0), (0, 1, 1), (1, 1, 1), (1, 1, 0)),
    ((1, 0, 0), (0, 0, 0), (0, 1, 0), (1, 1, 0)),
    ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)),
    ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)),
    ((1, 0, 1), (1, 0, 0), (1, 1, 0), (1, 1, 1)),
)


class SceneMesh:
    def __init__(self):
        self.buckets = [bytearray() for _ in range(6)]

    def cuboid(self, minimum, maximum, *, sides=range(6), tile=0, layer=0):
        bucket = self.buckets[layer]
        for face in sides:
            bucket.append(face)
            for corner in FACE_CORNERS[face]:
                point = [maximum[index] if high else minimum[index] for index, high in enumerate(corner)]
                bucket.extend(struct.pack(">5f5B", *point, (tile + 0.5) / 4, 0.5, 255, 255, 255, 255, 0))

    def packet(self):
        counts = [len(bucket) // 101 * 4 for bucket in self.buckets]
        return frame(2, struct.pack(">3i6I", 0, 0, 0, *counts) + b"".join(self.buckets) + bytes(8))


def mesh(boxes=(), *, opaque=False, floor=False, side=False, rect=False):
    result = SceneMesh()
    # All test geometry, sources and glass are in section (0,0,0). Cut planes coincide with block boundaries.
    if floor:
        result.cuboid((0.5, 2.25, 0.5), (15.5, 2.75, 15.5), sides=(1,))
    else:
        result.cuboid((0.5, 0.25, WALL_Z - 0.5), (15.5, 12.5, WALL_Z), sides=(3,))
    if opaque:
        # The compound tests put stone one or two cells farther along the ray than the glass.
        y, z = ((7 if boxes else 5), 8) if floor else (4, (12 if boxes else 10))
        lo, hi = (8, y, z), (9, y + 1, z + 1)
        result.cuboid(lo, hi)
    if side:
        result.cuboid((10.25, 1, WALL_Z), (10.75, 9, 10.55), sides=(4,))
        result.cuboid((9.1, 1, 11.0), (12, 9, 11.4))
    if rect:
        result.cuboid((8, 4, 13), (9, 5, 14), sides=(2,))
    for box in boxes:
        tile = 1 if box.transmittance[1] > box.transmittance[2] else 2
        result.cuboid(box.minimum, box.maximum, tile=tile, layer=5)
    return result.packet()


def atlases():
    colors = bytes((158, 158, 158, 255, 70, 155, 65, 95, 170, 74, 58, 95, 158, 158, 158, 255)) * 4
    normal, packed = bytes((255, 128, 128, 255)) * 16, bytes((0, 0, 235, 255)) * 16
    head = struct.pack(">6H", 0, 1, 4, 4, 0, 4)
    return b"".join(frame(kind, head + content) for kind, content in ((6, colors), (27, normal), (28, packed)))


def lights(records, scene=SCENE):
    data = bytearray(struct.pack(">I3iH", scene, 0, 0, 0, len(records)))
    for x, y, z, r, g, b, level, flags in records:
        data.extend(struct.pack(">H5B", x | (y << 4) | (z << 8), r, g, b, level, flags))
    return frame(25, data)


def publish_pose(stream, view):
    eye, target, fov = view
    angle = tuple(b - a for a, b in zip(eye, target))
    camera = bytearray(pose(eye, angle, fov))
    struct.pack_into(">I", camera, 5, stream.scene)
    stream.publish_camera(camera)


class Trial:
    def __init__(self, stream, output, contracts, enabled):
        self.stream, self.output = stream, output
        capabilities, = struct.unpack_from("<Q", stream.control, 16)
        if enabled and not capabilities & (1 << 6):
            raise RuntimeError(f"Renderer did not advertise SectionGlass: capabilities={capabilities:#x}")
        self.capabilities = capabilities
        self.data = {case: (contracts / f"{case}.bin").read_bytes() for case in CASES}
        self.glass = {name: decode(data).boxes for name, data in self.data.items()}
        self.probes = {}

    def update(self, variant, emitters=(), **options):
        glass_data = self.data[variant] if self.capabilities & (1 << 6) else b""
        self.stream.sendall(glass_data + mesh(self.glass[variant], **options) + lights(emitters))

    def begin(self, variant, emitters=(), *, view=WALL_VIEW, **options):
        video = frame(16, struct.pack(">BHBBB", 16, 0, 0, 0, 1))
        self.stream.sendall(frame(11, struct.pack(">Iii", SCENE, -64, 384)) + video + atlases() + NIGHT)
        publish_pose(self.stream, view)
        self.update(variant, emitters, **options)
        self.stream.sendall(SCENE_READY)
        self.stream.wait_until(lambda: self.stream.ready_scene == SCENE and not self.stream.terrain_leases)

    def capture(self, label, view, settle):
        publish_pose(self.stream, view)
        self.stream.wait_until(lambda: not self.stream.terrain_leases)
        until = time.monotonic() + settle
        while time.monotonic() < until:
            self.stream.receive(0.05)
        request_capture(self.output, label)
        png = self.output / f"{label}.png"
        linear = self.output / f"{label}.linear.json"
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            try:
                pixels = png.read_bytes()
                if pixels.endswith(b"IEND\xaeB\x60\x82"):
                    evidence = json.loads(linear.read_text(encoding="utf-8-sig"))
                    rgb = evidence["rgb"]
                    if len(rgb) != 3 or not all(math.isfinite(channel) and channel >= 0 for channel in rgb):
                        raise ValueError(f"Invalid RGB probe for {label}: {rgb}")
                    self.probes[label] = {
                        "rgb": rgb, "eye": view[0], "target": view[1], "vertical_fov": view[2],
                        "pre_exposure": evidence.get("pre_exposure"),
                        "sample_size": [evidence.get("sample_width"), evidence.get("sample_height")],
                        "view_size": [evidence.get("view_width"), evidence.get("view_height")],
                        "output_size": struct.unpack_from(">2I", pixels, 16),
                        "effective_settings": {key: evidence.get(key) for key in EFFECTIVE_SETTINGS},
                    }
                    print(f"{label}: {rgb}", flush=True)
                    return rgb
            except (FileNotFoundError, PermissionError, json.JSONDecodeError):
                pass
            self.stream.receive(0.08)
        raise TimeoutError(f"Timed out waiting for {png.name} and {linear.name} in {self.output}")


def delta(first, second):
    return [a - b for a, b in zip(first, second)]


def components_ratio(numerator, denominator):
    threshold = max(1e-8, 0.01 * max(denominator))
    return [a / b if b > threshold else None for a, b in zip(numerator, denominator)]


def verify_effective_settings(result, trial, args, *, gi):
    probes = trial.probes
    failures = result.setdefault("failures", [])
    if not probes:
        failures.append("No render settings were sampled.")
        return result
    expected = {
        "lumen_diffuse_indirect_allow": int(gi),
        "view_uses_lumen_method": gi and args.path == "hardware",
        "hardware_lighting": args.path == "hardware",
        "mega_lights": args.path == "hardware",
        "fixed_exposure": True,
        "local_exposure_strength": 0,
    }
    if args.path == "software":
        expected["virtual_shadow_maps"] = True
    seen = set()

    def problem(label, key, wanted, actual):
        item = (key, str(wanted), str(actual))
        if item not in seen:
            failures.append(f"{label}: {key} requested {wanted}, observed {actual}")
            seen.add(item)

    sampled_sizes = set()
    for label, probe in probes.items():
        effective = probe["effective_settings"]
        for key, value in expected.items():
            if effective.get(key) != value:
                problem(label, key, value, effective.get(key))
        exposure_span = effective.get("exposure_span_ev100")
        if exposure_span is not None and not (isinstance(exposure_span, (int, float)) and 0 <= exposure_span <= 0.0001):
            problem(label, "exposure_span_ev100", "0 to 0.0001", exposure_span)
        view_size, output_size = probe["view_size"], probe["output_size"]
        if not all(isinstance(v, int) and v > 0 for v in (*view_size, *output_size)):
            problem(label, "view/output dimensions", "positive size", (view_size, output_size))
            continue
        if tuple(args.size) != tuple(output_size):
            problem(label, "output size", args.size, output_size)
        sampled = tuple(round(100 * a / b, 4) for a, b in zip(view_size, output_size))
        sampled_sizes.add((tuple(view_size), tuple(output_size), sampled))
        if any(abs(value - args.render_percent) > 1.5 for value in sampled):
            problem(label, "render percentage", args.render_percent, sampled)
    settings_path = trial.output / "glass-qa-settings.json"
    try:
        startup = json.loads(settings_path.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError):
        startup = {}
    startup_gi = startup.get("lumen_diffuse_indirect_allow")
    if startup_gi != int(gi):
        problem("startup", "lumen_diffuse_indirect_allow", int(gi), startup_gi)

    shadow_probe = {"visual": "red-blue", "red-only": "red-blue", "lifecycle": "lifecycle-final-part",
                    "indirect": "indirect-green"}.get(args.mode)
    if shadow_probe in probes:
        effective = probes[shadow_probe]["effective_settings"]
        backend = "glass_ray_shadow_sources" if args.path == "hardware" else "glass_vsm_shadow_sources"
        count = effective.get(backend)
        if not isinstance(count, int) or count < 1:
            problem(shadow_probe, backend, "at least one", count)

    try:
        with (trial.output / "performance.csv").open(newline="", encoding="utf-8-sig") as source:
            presets = sorted({row["preset"] for row in csv.DictReader(source) if row.get("preset")})
    except (OSError, csv.Error):
        presets = []
    try:
        effective_line = (trial.output / "quality.txt.effective").read_text(encoding="utf-8-sig").splitlines()[0]
        effective_preset = effective_line.split()[0]
    except (OSError, IndexError, UnicodeError):
        effective_line = None
        effective_preset = None
    if (presets or args.mode == "perf") and presets != [args.quality]:
        problem("timing trace", "quality preset", args.quality, presets)
    if (effective_preset is not None or args.mode != "perf") and effective_preset != args.quality:
        problem("renderer effective status", "quality preset", args.quality, effective_preset)
    result["effective_settings"] = {
        "expected": expected,
        "startup": startup,
        "first_probe": next(iter(probes.values()))["effective_settings"],
        "view_and_output_sizes": sorted(sampled_sizes),
        "timing_trace_presets": presets,
        "renderer_effective_preset": effective_preset,
        "renderer_effective_status": effective_line,
        "probe_count": len(probes),
        "verified": not seen,
    }
    return result


def point_expected(boxes, view=WALL_VIEW):
    return ray_transmittance(boxes, view[1], POINT_CENTRE)


def rect_expected(boxes, probe):
    source_z = 13 - 0.015
    quarter_size = 0.94 / 4
    weighted_rgb, weights, samples = [0.0, 0.0, 0.0], [], []
    for x in (8.5 - quarter_size, 8.5 + quarter_size):
        for y in (4.5 - quarter_size, 4.5 + quarter_size):
            point = (x, y, source_z)
            distance = math.dist(point, probe)
            # Parallel lamp and wall. Their cosine factors are equal for each sample.
            weight = ((source_z - probe[2]) / distance ** 2) ** 2
            transmittance = ray_transmittance(boxes, probe, point)
            samples.append({"position": point, "transmittance": transmittance, "weight": weight})
            weights.append(weight)
            for channel in range(3):
                weighted_rgb[channel] += weight * transmittance[channel]
    return [value / sum(weights) for value in weighted_rgb], samples


def capture_red_blue(trial, args):
    trial.begin("clear")
    trial.capture("dark", WALL_VIEW, args.settle)
    trial.update("clear", (RED_CENTRE,))
    trial.capture("red-clear", WALL_VIEW, args.settle)
    trial.update("blue", (RED_CENTRE,))
    # Preserve the early first-filter frame and require two later captures of the same state.
    trial.capture("red-blue-first", WALL_VIEW, args.settle)
    trial.capture("red-blue", WALL_VIEW, max(2.5, args.settle))
    trial.capture("red-blue-repeat", WALL_VIEW, max(0.5, args.settle))


def red_blue_checks(values, expected, *, after_warm=False):
    dark = values["dark"]
    red_signal = values["red-clear"][0] - dark[0]
    failures, ratios = [], {}
    if red_signal <= 1e-5:
        failures.append(f"Red reference signal is too close to dark: {red_signal}")
    labels = ["red-blue-first", "red-blue", "red-blue-repeat"]
    if after_warm:
        labels += ["red-blue-warm", "red-blue-warm-repeat"]
    for name in labels:
        signal = delta(values[name], dark)
        transmitted = signal[0] / red_signal if red_signal > 1e-5 else None
        leakage = [abs(value) / red_signal for value in signal[1:]] if transmitted is not None else None
        expected_red = expected[0]
        red_ok = (transmitted is not None and transmitted > 0
                  and abs(transmitted - expected_red) <= RED_TRANSMISSION_TOLERANCE)
        leakage_ok = leakage is not None and max(leakage) <= RED_LEAK_TOLERANCE
        required = name != "red-blue-first"
        ratios[name] = {
            "measured": [transmitted, None, None],
            "expected": [expected_red, 0, 0],
            "linear_red_above_dark": signal[0],
            "unfiltered_red_above_dark": red_signal,
            "green_blue_leakage": leakage,
            "red_tolerance": RED_TRANSMISSION_TOLERANCE,
            "leakage_tolerance": RED_LEAK_TOLERANCE,
            "diagnostic_only": not required,
            "within_tolerance": red_ok and leakage_ok,
        }
        if required and not red_ok:
            failures.append(f"{name} red throughput {transmitted}, expected {expected_red:.6f} ± {RED_TRANSMISSION_TOLERANCE}")
        if required and not leakage_ok:
            failures.append(f"{name} G/B leakage {leakage}, limit {RED_LEAK_TOLERANCE} of unfiltered red")
    return ratios, failures


def red_only(trial, args):
    capture_red_blue(trial, args)
    trial.update("clear", (RED_CENTRE,))
    trial.capture("red-restored", WALL_VIEW, args.settle)
    trial.update("clear", (WHITE,))
    trial.capture("white-warm-reference", WALL_VIEW, args.settle)
    trial.update("blue", (WHITE,))
    trial.capture("white-blue-warm", WALL_VIEW, max(2.5, args.settle))
    trial.update("blue", (RED_CENTRE,))
    trial.capture("red-blue-warm", WALL_VIEW, max(2.5, args.settle))
    trial.capture("red-blue-warm-repeat", WALL_VIEW, max(0.5, args.settle))

    values = {name: row["rgb"] for name, row in trial.probes.items()}
    expected = point_expected(trial.glass["blue"])
    ratios, failures = red_blue_checks(values, expected, after_warm=True)
    restored = components_ratio(delta(values["red-restored"], values["dark"]),
                                delta(values["red-clear"], values["dark"]))[0]
    ratios["red-restored"] = {"measured": restored, "expected": 1}
    if restored is None or abs(restored - 1) > 0.05:
        failures.append(f"red-restored throughput {restored}, expected 1 ± 0.05")
    warm = components_ratio(delta(values["white-blue-warm"], values["dark"]),
                            delta(values["white-warm-reference"], values["dark"]))
    ratios["white-blue-warm"] = {"measured": warm, "expected": expected}
    if warm[2] is None or abs(warm[2] - expected[2]) > 0.06:
        failures.append(f"White warmup did not illuminate the wall through blue glass: {warm}")
    return {"probes": trial.probes, "comparisons": ratios, "failures": failures}


def visual(trial, args):
    view_seen = ((6.5, 4.8, 14.5), WALL_PROBE, 45)
    rect_view = ((7.55, 4.5, 9.8), (7.55, 4.5, WALL_Z), 40)
    capture_red_blue(trial, args)
    for name, variant, emitters, view, options in (
        ("white-clear", "clear", (WHITE,), WALL_VIEW, {}),
        ("white-green-offscreen", "green", (WHITE,), WALL_VIEW, {}),
        ("white-green-visible", "green", (WHITE,), view_seen, {}),
        ("white-green-stacked", "green-stacked", (WHITE,), WALL_VIEW, {}),
        ("white-green-pane", "green-pane", (WHITE,), WALL_VIEW, {}),
        ("white-green-removed", "clear", (WHITE,), WALL_VIEW, {}),
        ("white-green-replaced", "green", (WHITE,), WALL_VIEW, {}),
        ("red-offset-clear", "clear", (RED,), WALL_VIEW, {}),
        ("red-offset-green", "green", (RED,), WALL_VIEW, {}),
        ("white-and-red", "green", (WHITE, RED), WALL_VIEW, {}),
        ("white-opaque", "clear", (WHITE,), WALL_VIEW, {"opaque": True}),
        ("white-green-opaque", "green", (WHITE,), WALL_VIEW, {"opaque": True}),
        ("rect-dark", "clear", (), rect_view, {"rect": True}),
        ("rect-clear", "clear", (RECT,), rect_view, {"rect": True}),
        ("rect-partial-green", "green-rect-half", (RECT,), rect_view, {"rect": True}),
        ("rect-full-green", "green-rect-full", (RECT,), rect_view, {"rect": True}),
    ):
        trial.update(variant, emitters, **options)
        trial.capture(name, view, args.settle)
    trial.stream.sendall(SUN)
    for label, variant, options in (
        ("sun-clear", "clear", {}), ("sun-green", "green-sun", {}),
        ("sun-glass-miss", "green", {}),
        ("sun-opaque", "clear", {"opaque": True}), ("sun-green-opaque", "green-sun", {"opaque": True}),
        ("sun-restored", "clear", {}),
    ):
        trial.update(variant, (), floor=True, **options)
        trial.capture(label, SUN_VIEW, max(3.0, args.settle))

    values = {name: row["rgb"] for name, row in trial.probes.items()}
    ratios, failures = red_blue_checks(values, point_expected(trial.glass["blue"]))

    def check(name, baseline, expected, *, dark="dark", tolerance=0.075, channels=(0, 1, 2), comparison=None):
        ref = delta(values[baseline], values[dark])
        received = delta(values[name], values[dark])
        label = comparison or name
        ratios[label] = {"measured": components_ratio(received, ref), "expected": expected}
        for channel in channels:
            observed = ratios[label]["measured"][channel]
            if observed is None or abs(observed - expected[channel]) > tolerance:
                failures.append(f"{label} RGB[{channel}]: observed {observed}, expected {expected[channel]:.4f}")

    for label, variant in (("white-green-offscreen", "green"), ("white-green-stacked", "green-stacked"),
                           ("white-green-pane", "green-pane"), ("white-green-replaced", "green")):
        check(label, "white-clear", point_expected(trial.glass[variant]))
    check("white-green-removed", "white-clear", (1, 1, 1))
    check("red-offset-green", "red-offset-clear", (1, 1, 1), channels=(0,))
    check("white-opaque", "white-clear", (0, 0, 0), tolerance=0.16)
    check("white-green-opaque", "white-clear", (0, 0, 0), tolerance=0.16)
    sun_expected = ray_transmittance(trial.glass["green-sun"], SUN_PROBE, (8.5, 32.75, 8.5))
    check("sun-green", "sun-clear", sun_expected, dark="sun-opaque", tolerance=0.15)
    check("sun-glass-miss", "sun-clear", (1, 1, 1), dark="sun-opaque", tolerance=0.12)
    check("sun-green", "sun-glass-miss", sun_expected, dark="sun-opaque", tolerance=0.12,
          comparison="sun-green-vs-miss")
    check("sun-green-opaque", "sun-clear", (0, 0, 0), dark="sun-opaque", tolerance=0.16)
    check("sun-restored", "sun-clear", (1, 1, 1), dark="sun-opaque", tolerance=0.075)

    for label, variant in (("rect-partial-green", "green-rect-half"), ("rect-full-green", "green-rect-full")):
        expected, samples = rect_expected(trial.glass[variant], rect_view[1])
        check(label, "rect-clear", expected, dark="rect-dark", tolerance=0.1)
        ratios[label]["samples"] = samples
    measured_rect = ratios["rect-partial-green"]["measured"][0]
    full_rect = ratios["rect-full-green"]["measured"][0]
    if measured_rect is None or full_rect is None or not full_rect + 0.03 < measured_rect < 0.97:
        failures.append("rect-partial-green red channel does not show the expected fractional source visibility")

    white, red = values["white-green-offscreen"], values["red-offset-clear"]
    expected_sum = [a + b - c for a, b, c in zip(white, red, values["dark"])]
    mixed = values["white-and-red"]
    ratios["white-and-red"] = {"measured": mixed, "expected": expected_sum, "absolute_error": delta(mixed, expected_sum)}
    if max(abs(value) for value in delta(mixed, expected_sum)) > max(1e-6, 0.075 * max(expected_sum)):
        failures.append("simultaneous white and red is not the sum of their separate filtered contributions")
    return {"probes": trial.probes, "comparisons": ratios, "failures": failures}


def lifecycle(trial, args):
    trial.begin("clear")
    trial.capture("lifecycle-dark", WALL_VIEW, args.settle)
    trial.update("clear", (WHITE,))
    trial.capture("lifecycle-initial", WALL_VIEW, args.settle)

    parts = [frame(kind, body) for kind, body in frames(trial.data["transaction"])]
    trial.stream.sendall(parts[0])
    trial.capture("lifecycle-first-part", WALL_VIEW, args.settle)
    trial.stream.sendall(parts[1])
    trial.capture("lifecycle-final-part", WALL_VIEW, args.settle)

    geometry_and_light = mesh() + lights((WHITE,))
    trial.stream.sendall(frame(3, struct.pack(">2i", 0, 0)) + geometry_and_light)
    trial.capture("lifecycle-after-drop", WALL_VIEW, args.settle)
    trial.stream.sendall(trial.data["green"])
    trial.capture("lifecycle-before-world", WALL_VIEW, args.settle)

    next_scene = 2
    night_next = NIGHT[:5] + struct.pack(">I", next_scene) + NIGHT[9:]
    trial.stream.sendall(frame(11, struct.pack(">Iii", next_scene, -64, 384)) + atlases() + night_next)
    publish_pose(trial.stream, WALL_VIEW)
    trial.stream.sendall(mesh() + lights((WHITE,), next_scene) + frame(15, struct.pack(">I", next_scene)))
    trial.stream.wait_until(lambda: trial.stream.ready_scene == next_scene and not trial.stream.terrain_leases)
    trial.capture("lifecycle-after-world", WALL_VIEW, args.settle)

    expected = point_expected(trial.glass["green"])
    dark = trial.probes["lifecycle-dark"]["rgb"]
    clear = delta(trial.probes["lifecycle-initial"]["rgb"], dark)
    comparisons, failures = {}, []
    for label, target in (("first-part", (1, 1, 1)), ("final-part", expected), ("after-drop", (1, 1, 1)),
                          ("before-world", expected), ("after-world", (1, 1, 1))):
        measured = components_ratio(delta(trial.probes[f"lifecycle-{label}"]["rgb"], dark), clear)
        comparisons[label] = {"measured": measured, "expected": target}
        if any(value is None or abs(value - reference) > 0.075 for value, reference in zip(measured, target)):
            failures.append(f"{label} observed {measured}, expected {target}")
    return {"probes": trial.probes, "comparisons": comparisons, "failures": failures}


def read_performance(path, batches):
    try:
        with path.open(newline="", encoding="utf-8-sig") as stream:
            group = {}
            for row in csv.DictReader(stream):
                if row.get("gpu_ms"):
                    group.setdefault(row["batch"], []).append(row)
    except (FileNotFoundError, PermissionError):
        return None
    complete = [rows for rows in group.values() if len(rows) == 240]
    if len(complete) < batches:
        return None
    rows = [row for batch in complete[:batches] for row in batch]
    dimensions = sorted({(int(row["input_width"]), int(row["input_height"]),
                          int(row["output_width"]), int(row["output_height"])) for row in rows})
    gpu = [float(row["gpu_ms"]) for row in rows]
    if not all(math.isfinite(value) and value > 0 for value in gpu):
        raise RuntimeError(f"GPU trace contained unavailable measurements in {path}")
    return {"frames": len(gpu), "sizes": dimensions, "gpu_mean_ms": statistics.mean(gpu),
            "presets": sorted({row["preset"] for row in rows}),
            "gpu_median_ms": statistics.median(gpu), "gpu_p95_ms": sorted(gpu)[math.ceil(len(gpu) * 0.95) - 1]}


def performance(trial, args):
    seed = 1138
    rng = random.Random(seed)
    positions = rng.sample([(x, y, z) for z in range(12, 16) for x in range(3, 14) for y in range(2, 10)], args.lights)
    emitters = [(*p, *rng.choice(((255, 245, 223), (240, 228, 255), (228, 250, 255), (255, 229, 248))),
                 15, 0) for p in positions]
    view = ((8.5, 5.25, 16.25), (8.5, 5.25, WALL_Z), 49)
    trial.begin("multi-light", emitters, view=view)
    csv_path = trial.output / "performance.csv"
    deadline = time.monotonic() + args.perf_timeout
    while time.monotonic() < deadline:
        sample = read_performance(csv_path, args.batches)
        if sample is not None:
            # Read back settings after the timing window so the PNG/GPU copy cannot affect the samples.
            trial.capture("perf-settings", view, 0.0)
            return {**sample, "seed": seed, "lights": len(emitters), "positions": positions, "view": view,
                    "settings_probe": trial.probes["perf-settings"]}
        trial.stream.receive(0.08)
    raise TimeoutError(f"Did not receive {args.batches} complete GPU timing batches in {csv_path}")


def indirect(trial, args):
    trial.begin("clear", view=SIDE_VIEW, side=True)
    settle = max(args.settle, 5)
    for label, variant, sources, view in (
        ("indirect-control-dark", "clear", (), WALL_VIEW),
        ("indirect-dark", "clear", (), SIDE_VIEW),
        ("indirect-clear", "clear", (WHITE,), SIDE_VIEW),
        ("indirect-green", "green", (WHITE,), SIDE_VIEW),
        ("indirect-control-direct", "green", (WHITE,), WALL_VIEW),
    ):
        trial.update(variant, sources, side=True)
        trial.capture(label, view, settle)
    values = {name: row["rgb"] for name, row in trial.probes.items()}
    clear = delta(values["indirect-clear"], values["indirect-dark"])
    green = delta(values["indirect-green"], values["indirect-dark"])
    direct = delta(values["indirect-control-direct"], values["indirect-control-dark"])
    return {"probes": trial.probes, "clear_minus_dark": clear, "green_minus_dark": green,
            "green_minus_clear": delta(green, clear), "direct_control_minus_dark": direct}


def attribute_indirect(gi_off, gi_on):
    failures = [*gi_off["failures"], *gi_on["failures"]]
    effective = gi_on.get("effective_settings", {}).get("first_probe", {})
    if effective.get("view_uses_lumen_method") is not True:
        failures.append("The GI-on view did not use Lumen, so indirect color cannot be attributed to it.")
    direct = max(gi_off["direct_control_minus_dark"])
    # The bright directly lit control keeps the floor meaningful across color and exposure changes.
    limit = max(0.00025, min(0.001, 0.001 * direct))
    if direct < 0.02 or max(gi_on["direct_control_minus_dark"]) < 0.02:
        failures.append("The directly lit control was too dark to verify the indirect fixture.")
    for state in ("clear", "green"):
        signal = gi_off[f"{state}_minus_dark"]
        if any(abs(value) > limit for value in signal):
            failures.append(f"GI-off {state} changed the occluded wall by {signal}; attribution limit {limit:.6g}")
    observed_off = max(abs(channel) for state in ("clear", "green") for channel in gi_off[f"{state}_minus_dark"])
    attribution = {"validated": not failures, "gi_off_max_delta": limit, "gi_off_observed_max_delta": observed_off,
                   "clear_linear_rgb": None, "green_linear_rgb": None, "glass_change_linear_rgb": None}
    if not failures:
        clear = delta(gi_on["clear_minus_dark"], gi_off["clear_minus_dark"])
        green = delta(gi_on["green_minus_dark"], gi_off["green_minus_dark"])
        attribution.update(clear_linear_rgb=clear, green_linear_rgb=green,
                           glass_change_linear_rgb=delta(green, clear),
                           indirect_signal_detected=any(abs(value) > limit for value in clear + green))
    return attribution, failures


def renderer_flags(args, output, port, *, enabled, gi):
    quality = output / "quality.txt"
    quality.write_text(args.quality + "\n", encoding="ascii")
    hardware = int(args.path == "hardware")
    details = ["exposureMode=0", "exposureBias=20", "localExposure=0", "directionalShadows=1",
               f"renderScale={args.render_percent}",
               f"hardwareLighting={hardware}", f"megaLights={hardware}",
               f"fallbackLightBudget={min(args.lights, 128)}", f"glassLighting={int(enabled)}"]
    if not hardware:
        details.append("hardwareGi=0")
    (output / "minecraftue-graphics.txt").write_text("\n".join(details) + "\n", encoding="ascii")
    width, height = args.size
    flags = [f'-BridgeQualityFile="{quality}"', f"-BridgeQuality={args.quality}", f"-BridgePort={port}",
             f'-BridgeGlassCaptureDir="{output}"', f'-BridgeQualityTraceCsv="{output / "performance.csv"}"',
             f'-AbsLog="{output / "renderer.log"}"', f'-UserDir="{output / "profile"}"',
             "-game", "-RenderOffscreen", "-unattended", "-multiprocess", "-windowed",
             f"-ResX={width}", f"-ResY={height}", "-ForceRes", "-d3d12", "-sm6",
             "-NoSound", "-NoSplash", "-NoLiveCoding", "-NoCrashDialog", "-NoScreenMessages",
             "-NoPauseWhenUnfocused", "-BridgeTrace", "-BridgeQualityTrace", "-BridgeHideDebugPanel",
             "-ini:Engine:[/Script/Engine.Engine]:GameViewportClientClassName=/Script/Engine.GameViewportClient",
             f"-BridgeGlassQADiffuseGI={int(gi)}",
             f"-ini:Engine:[SystemSettings]:r.Lumen.DiffuseIndirect.Allow={int(gi)}",
             f'-ExecCmds="r.ScreenPercentage {args.render_percent},r.VSync 0,t.MaxFPS 0,r.Lumen.DiffuseIndirect.Allow {int(gi)}"']
    if args.path == "software":
        flags += ["-BridgeNoHardwareRayTracing", "-noraytracing",
                  "-ini:Engine:[/Script/Engine.RendererSettings]:r.RayTracing=0",
                  "-ini:Engine:[/Script/Engine.RendererSettings]:r.PathTracing=0"]
    if not enabled:
        flags.append("-BridgeGlassDisabled")
    return flags + args.renderer_arg


@contextmanager
def launch(args, output, *, enabled, gi):
    output.mkdir(parents=True, exist_ok=False)
    # Confirm that connecting below cannot land on an unrelated renderer.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as available:
        available.bind(("127.0.0.1", args.port))
        port = available.getsockname()[1]
    prefix = [str(args.renderer)] + ([str(args.project)] if args.project is not None else [])
    command = subprocess.list2cmdline(prefix) + " " + " ".join(renderer_flags(args, output, port, enabled=enabled, gi=gi))
    (output / "launch.txt").write_text(command + "\n", encoding="utf-8")
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = subprocess.SW_HIDE
    process = subprocess.Popen(command, startupinfo=startup)
    stream = BridgeReplay(port, timeout=40)
    connected = False
    try:
        deadline = time.monotonic() + args.startup_timeout
        while time.monotonic() < deadline and process.poll() is None:
            try:
                stream.__enter__()
            except ConnectionRefusedError:
                time.sleep(0.20)
            else:
                connected = True
                break
        if not connected:
            raise RuntimeError(f"Renderer failed to acknowledge the replay. See {output / 'renderer.log'}")
        yield Trial(stream, output, args.contracts, enabled)
    finally:
        if connected:
            stream.__exit__(None, None, None)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def write_json(path, report):
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main():
    single_trials = {"visual": visual, "lifecycle": lifecycle, "red-only": red_only}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--renderer", required=True, type=Path, help="Windows Shipping renderer or UnrealEditor.exe")
    parser.add_argument("--project", type=Path, help="Pass the project's .uproject when using UnrealEditor")
    parser.add_argument("--contracts", type=Path, default=DEFAULT_CONTRACTS)
    parser.add_argument("--mode", choices=(*single_trials, "perf", "indirect"), default="visual")
    parser.add_argument("--path", choices=("hardware", "software"), default="hardware")
    parser.add_argument("--quality", choices=("High", "Ultra"), default="High")
    parser.add_argument("--size", type=int, nargs=2, default=(854, 480), metavar=("WIDTH", "HEIGHT"))
    parser.add_argument("--render-percent", type=int, default=100)
    parser.add_argument("--port", type=int, default=0, help="Use this loopback port instead of letting Windows select one")
    parser.add_argument("--settle", type=float, default=1.2)
    parser.add_argument("--lights", type=int, default=48)
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--batches", type=int, default=3)
    parser.add_argument("--perf-timeout", type=float, default=150)
    parser.add_argument("--startup-timeout", type=float, default=150)
    parser.add_argument("--renderer-arg", action="append", default=[])
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("The replay uses the Windows shared-memory transport.")
    if not 1 <= args.lights <= 128 or min(args.rounds, args.batches, *args.size) < 1 or not 50 <= args.render_percent <= 100:
        parser.error("Use 1–128 lights, positive sizes/rounds/batches, and a render percentage from 50 to 100.")
    args.output = args.output.resolve()
    args.renderer = args.renderer.resolve(strict=True)
    args.project = args.project.resolve(strict=True) if args.project else None
    if args.project is None and args.renderer.name.lower() == "minecraftueviewer.exe":
        actual_executable = args.renderer.parent / "MinecraftUEViewer/Binaries/Win64/MinecraftUEViewer-Win64-Shipping.exe"
        if actual_executable.is_file():
            args.renderer = actual_executable
    verify(args.contracts)
    report = {"mode": args.mode, "requested_path": args.path, "quality": args.quality, "renderer": str(args.renderer),
              "fixed_exposure": True, "output_size": args.size, "render_percent": args.render_percent}
    if args.mode in single_trials:
        with launch(args, args.output / args.mode, enabled=True, gi=False) as trial:
            report.update(verify_effective_settings(single_trials[args.mode](trial, args), trial, args, gi=False))
    elif args.mode == "perf":
        rounds, failures = [], []
        for index in range(args.rounds):
            pair = {}
            for enabled in ((True, False) if index % 2 == 0 else (False, True)):
                key = "on" if enabled else "off"
                with launch(args, args.output / f"round-{index + 1}-{key}", enabled=enabled, gi=True) as trial:
                    stats = performance(trial, args)
                    pair[key] = verify_effective_settings(stats, trial, args, gi=True)
                failures.extend(f"round {index + 1} {key}: {item}" for item in pair[key]["failures"])
                for width, height, out_width, out_height in pair[key]["sizes"]:
                    if min(width, height, out_width, out_height) <= 0 or (out_width, out_height) != tuple(args.size) or any(
                            abs(100 * a / b - args.render_percent) > 1.5
                            for a, b in ((width, out_width), (height, out_height))):
                        failures.append(f"round {index + 1} {key}: unexpected render dimensions {pair[key]['sizes']}")
                        break
            if pair["on"]["sizes"] != pair["off"]["sizes"]:
                failures.append(f"round {index + 1}: enabled and disabled trials reported different render sizes.")
            pair["mean_delta_ms"] = pair["on"]["gpu_mean_ms"] - pair["off"]["gpu_mean_ms"]
            rounds.append(pair)
        report["rounds"] = rounds
        report["average_delta_ms"] = None if failures else statistics.mean(pair["mean_delta_ms"] for pair in rounds)
        report["failures"] = failures
    else:
        for enabled in (False, True):
            key = "gi-on" if enabled else "gi-off"
            with launch(args, args.output / key, enabled=True, gi=enabled) as trial:
                report[key] = verify_effective_settings(indirect(trial, args), trial, args, gi=enabled)
        attribution, failures = attribute_indirect(report["gi-off"], report["gi-on"])
        report["indirect_attribution"], report["failures"] = attribution, failures
        report["change_with_gi"] = attribution["glass_change_linear_rgb"]
    write_json(args.output / "result.json", report)
    for failure in report.get("failures", ()):
        print(failure)
    return int(bool(report.get("failures")))


if __name__ == "__main__":
    raise SystemExit(main())
