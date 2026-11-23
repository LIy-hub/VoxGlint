"""Replay Minecraft v28 weather frames and assert the public UE weather data interface."""

import argparse
from enum import IntEnum
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import time

from bench_sky_lake import camera_frame, encode_chunk_precip, encode_env_v24, encode_lightning
from bridge_v27_replay import BridgeReplay, VERSION, frame, frames


class Precipitation(IntEnum):
    UNKNOWN = 0
    NONE = 1
    RAIN = 2
    SNOW = 3


class Climate(IntEnum):
    UNKNOWN = 0
    TEMPERATE = 1
    DRY = 2
    COLD = 3
    HUMID = 4


class Condition(IntEnum):
    UNKNOWN = 0
    CLEAR = 1
    RAIN = 2
    THUNDERSTORM = 3


class Capability(IntEnum):
    UNKNOWN = 0
    SUPPORTED = 1
    DISABLED = 2


KNOWS_WEATHER = 1 << 0
ALLOWS_WEATHER = 1 << 1
RAINING = 1 << 2
THUNDERING = 1 << 3
WORLD_WEATHER = KNOWS_WEATHER | ALLOWS_WEATHER
MIN_Y = -64
HEIGHT = 384

WORLD_KEYS = ("has_world", "generation", "scene", "dimension", "condition", "capability", "global_precipitation",
              "thunder", "sky_flash", "is_raining", "is_thundering", "camera_rain", "camera_snow", "game_time")
GRID_KEYS = ("valid", "texture", "x", "z", "size")
STRIKE_KEYS = ("exists", "sequence", "generation", "scene", "entity", "seed", "reflash", "remaining",
               "mc_x", "mc_y", "mc_z", "ue_x", "ue_y", "ue_z")
SAMPLE_KEYS = ("source", "x", "z", "top_y", "covered", "climate", "column_type", "actual_type", "intensity",
               "temperate", "dry", "cold", "humid", "unknown")
WET_HISTORY_KEYS = ("valid", "rain_rate", "mode", "grounded", "steps", "active", "seconds")


def ue_position(position_mc):
    x, y, z = position_mc
    return 100 * z, -100 * x, 100 * y


def world(scene, min_y=MIN_Y, height=HEIGHT):
    return frame(11, struct.pack(">Iii", scene, min_y, height))


def camera(scene, position, feet=None, view=2):
    message = bytearray(camera_frame(scene, VERSION))
    struct.pack_into(">3d", message, 9, *position)
    if feet is not None:
        struct.pack_into(">B3d", message, 69, view, *feet)
    return bytes(message)


def environment(scene, tick, *, rain=0.0, thunder=0.0, flash=0.0, local_rain=0.0, local_snow=0.0,
                flags=WORLD_WEATHER, dimension=0):
    message = bytearray(encode_env_v24(scene, 0.52, rain, thunder, local_rain, local_snow, flash,
                                       0x7BA7FF, (0.67, 0.78, 0.98), game_time=tick))
    message[9], message[12] = dimension, flags
    return bytes(message)


def column(top, kind, climate, min_y=MIN_Y):
    profile = int(climate) - 1
    return (1 << 14) | ((int(kind) - 1) << 12) | ((profile & 1) << 11) | ((profile & 2) << 14) | (top - min_y)


def replay_columns(scene, replacements=None):
    replacements = replacements or {}
    encoded = bytearray()
    for cx in (-1, 0):
        cells = []
        for z in range(16):
            for lx in range(16):
                x = cx * 16 + lx
                if x < 0:
                    climate = Climate.TEMPERATE if z < 8 else Climate.HUMID
                    kind = Precipitation.RAIN
                else:
                    climate = Climate.COLD if z < 8 else Climate.DRY
                    kind = Precipitation.SNOW if z < 8 else Precipitation.NONE
                tops = {(-4, 4): 70, (-3, 4): 76, (3, 4): 320, (-7, 4): -64, (-9, 4): 321}
                cell = column(tops.get((x, z), 64), kind, climate)
                if (x, z) == (2, 4):
                    cell = 0
                if (x, z) == (-8, 4):
                    cell |= 3 << 12  # reserved kind, treated as unknown
                cells.append(replacements.get((x, z), cell))
        encoded.extend(encode_chunk_precip(scene, cx, 0, cells))
    return bytes(encoded)


POINTS = {
    "temperate": (-5.5, 64.5, 2.5),
    "snow": (0.5, 64.5, 2.5),
    "sand": (0.5, 64.5, 9.5),
    "wetland": (-0.5, 64.5, 9.5),
    "low_eave_below": (-3.5, 69.999, 4.5),
    "low_eave_surface": (-3.5, 70, 4.5),
    "high_eave_below": (-2.5, 70, 4.5),
    "high_eave_surface": (-2.5, 76, 4.5),
    "missing": (2.5, 77, 4.5),
    "cold_and_missing": (2.25, 77, 4.5),
    "out_of_range_top": (-8.5, 322, 4.5),
    "reserved_kind": (-7.5, 77, 4.5),
    "top_of_world": (3.5, 320, 4.5),
    "empty_column": (-6.5, -64, 4.5),
    "boundary_rain": (-0.001, 64.5, 2.5),
    "boundary_snow": (0, 64.5, 2.5),
    "four_climates": (0, 64.5, 8),
    "outside": (-16.0001, 64.5, 2.5),
    "chunk_edge": (-16, 64.5, 2.5),
}


def fields(keys, values, integer_fields):
    assert len(keys) == len(values), (keys, values)
    return {key: (int(value) if i in integer_fields else float(value))
            for i, (key, value) in enumerate(zip(keys, values))}


def parse_probe(text):
    report = {"samples": {}}
    for line in text.splitlines():
        label, *values = line.split("\t")
        if label in {"request", "teleport-epoch", "wet-history-uploads", "puddle-depressions"}:
            report[label] = int(values[0])
        elif label == "world":
            report[label] = fields(WORLD_KEYS, values, {0, 1, 2, 3, 4, 5, 9, 10})
        elif label == "grid":
            report[label] = fields(GRID_KEYS, values, set(range(5)))
        elif label == "gpu-grid":
            report[label] = tuple(map(int, values))
        elif label == "texel":
            report.setdefault("texels", {})[values[0]] = tuple(map(float, values[1:]))
        elif label == "wet-history":
            report[label] = fields(WET_HISTORY_KEYS, values, {0, 2, 3, 4, 5})
        elif label == "gpu-wet-history":
            report[label] = tuple(map(int, values))
        elif label in {"moisture", "wet-texel"}:
            report.setdefault(label, {})[values[0]] = tuple(map(float, values[1:]))
        elif label == "puddle-impact":
            report.setdefault("puddle-impacts", []).append(tuple(map(float, values)))
        elif label in {"camera", "player-feet"}:
            report[label] = tuple(map(float, values))
        elif label == "lightning":
            report[label] = fields(STRIKE_KEYS, values, set(range(7)))
        else:
            report["samples"][label] = fields(SAMPLE_KEYS, values, set(range(8)))
    return report


class WeatherProbe:
    def __init__(self, directory, read_gpu):
        self.directory = directory
        self.read_gpu = read_gpu
        self.serial = time.monotonic_ns()
        self.results = {}

    def query(self, points, read_gpu):
        self.serial += 1
        query = self.directory / "weather-query.tsv"
        pending = query.with_suffix(".tmp")
        lines = [f"{self.serial}\tgpu" if self.read_gpu and read_gpu else str(self.serial)]
        for label, point in points.items():
            coordinates = "\t".join(f"{value:.17g}" for value in ue_position(point))
            lines.append(f"{label}\t{coordinates}")
        pending.write_text("\n".join(lines) + "\n", encoding="ascii")
        pending.replace(query)

        response = self.directory / f"weather-response-{self.serial}.tsv"
        until = time.monotonic() + 8.0
        while time.monotonic() < until:
            try:
                report = parse_probe(response.read_text(encoding="utf-8"))
                if report.get("request") == self.serial:
                    assert report["samples"].keys() == points.keys(), report
                    try:
                        response.unlink(missing_ok=True)
                    except PermissionError:
                        pass
                    return report
            except (FileNotFoundError, PermissionError):
                pass
            time.sleep(0.02)
        raise TimeoutError(f"Renderer did not answer weather request {self.serial} in {self.directory}")

    def wait(self, label, stream, *, scene=None, tick=None, eye=None, points=POINTS, check=None, read_gpu=False):
        until = time.monotonic() + 12.0
        while time.monotonic() < until:
            if stream is not None:
                stream.receive()
            reply = self.query(points, read_gpu)
            state = reply["world"]
            if ((scene is None or state["scene"] == scene) and
                (tick is None or state["game_time"] == tick) and
                (eye is None or reply["camera"] == eye) and
                (check is None or check(reply))):
                self.results[label] = reply
                (self.directory / "weather-replay.json").write_text(
                    json.dumps(self.results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
                print(f"OBSERVED {label} scene={state['scene']} generation={state['generation']} "
                      f"state={Condition(state['condition']).name} strike={reply['lightning']['sequence']}", flush=True)
                return reply
        raise TimeoutError(f"Weather phase {label} did not settle: {reply}")

    def expect_texels(self, report, **expected):
        if not self.read_gpu:
            return
        assert report.get("gpu-grid", (0, 0, 0))[2] == 256, report
        grid = report["grid"]
        if grid["valid"]:
            assert report["gpu-grid"] == (grid["x"], grid["z"], grid["size"]), report
        for label, rgba in expected.items():
            assert report.get("texels", {}).get(label) == rgba, (label, rgba, report.get("texels"))
        print(f"D3D12_TEXELS_PASS {', '.join(expected)}", flush=True)

    def expect_moisture_texels(self, report, **expected):
        if not self.read_gpu:
            return
        assert report.get("gpu-wet-history", (0, 0, 0))[2] == 256, report
        if report["wet-history"]["valid"]:
            grid = report["grid"]
            assert report["gpu-wet-history"] == (grid["x"], grid["z"], grid["size"]), report
        for name, channels in expected.items():
            texel = report["wet-texel"][name]
            assert all(math.isclose(a, b, abs_tol=0.0006) for a, b in zip(texel, channels)), (name, texel, channels)
        print(f"D3D12_WET_TEXELS_PASS {', '.join(expected)}", flush=True)


def expect(sample, **expected):
    for field, value in expected.items():
        actual = sample[field]
        if isinstance(value, float):
            assert math.isclose(actual, value, abs_tol=1e-6), (field, value, actual, sample)
        else:
            assert actual == value, (field, value, actual, sample)


def check_rain(report):
    points = report["samples"]
    expect(points["temperate"], source=1, x=-6, z=2, top_y=64, covered=0, climate=Climate.TEMPERATE,
           column_type=Precipitation.RAIN, actual_type=Precipitation.RAIN, intensity=0.8)
    expect(points["snow"], source=1, top_y=64, climate=Climate.COLD, column_type=Precipitation.SNOW,
           actual_type=Precipitation.SNOW, intensity=0.8)
    expect(points["sand"], source=1, climate=Climate.DRY, column_type=Precipitation.NONE,
           actual_type=Precipitation.NONE, intensity=0)
    expect(points["wetland"], source=1, climate=Climate.HUMID, actual_type=Precipitation.RAIN, intensity=0.8)
    for label, top in (("low", 70), ("high", 76)):
        expect(points[f"{label}_eave_below"], source=1, top_y=top, covered=1,
               column_type=Precipitation.RAIN, actual_type=Precipitation.NONE, intensity=0)
        expect(points[f"{label}_eave_surface"], source=1, top_y=top, covered=0,
               actual_type=Precipitation.RAIN, intensity=0.8)
    for label in ("missing", "cold_and_missing", "out_of_range_top", "reserved_kind", "outside"):
        expect(points[label], source=0, climate=Climate.UNKNOWN, column_type=Precipitation.UNKNOWN,
               actual_type=Precipitation.UNKNOWN, intensity=0)
    expect(points["top_of_world"], source=1, top_y=320, covered=0, actual_type=Precipitation.SNOW)
    expect(points["empty_column"], source=1, top_y=-64, covered=0, actual_type=Precipitation.RAIN)
    expect(points["boundary_rain"], x=-1, actual_type=Precipitation.RAIN, temperate=0.501, cold=0.499)
    expect(points["boundary_snow"], x=0, actual_type=Precipitation.SNOW, temperate=0.5, cold=0.5)
    expect(points["four_climates"], temperate=0.25, dry=0.25, cold=0.25, humid=0.25, unknown=0)
    expect(points["cold_and_missing"], cold=0.25, unknown=0.75)
    expect(points["chunk_edge"], x=-16, actual_type=Precipitation.RAIN, temperate=0.5, unknown=0.5)
    for sample in points.values():
        assert math.isclose(sum(sample[key] for key in SAMPLE_KEYS[-5:]), 1.0, abs_tol=1e-6), sample


def expect_strike(report, sequence, scene, seed, position, *, reflash, entity=10437, remaining=0.125):
    strike = report["lightning"]
    expect(strike, exists=1, sequence=sequence, generation=report["world"]["generation"],
           scene=scene, entity=entity, seed=seed, reflash=int(reflash), remaining=remaining)
    assert tuple(strike[key] for key in STRIKE_KEYS[8:11]) == position, strike
    assert tuple(strike[key] for key in STRIKE_KEYS[11:14]) == ue_position(position), strike


def run_weather(stream, probe):
    scene = 17
    start_eye = (-5.5, 67, 2.5)
    dry_eye = (0.5, 67, 9.5)
    stream.sendall(world(scene) + replay_columns(scene))
    stream.sendall(camera(scene, start_eye))
    waiting = probe.wait("columns_before_environment", stream, scene=scene, eye=start_eye)
    expect(waiting["world"], has_world=1, condition=Condition.UNKNOWN)
    expect(waiting["samples"]["temperate"], column_type=Precipitation.RAIN, actual_type=Precipitation.UNKNOWN)

    stream.sendall(environment(scene, 10))
    sunny = probe.wait("clear", stream, scene=scene, tick=10)
    expect(sunny["world"], condition=Condition.CLEAR, capability=Capability.SUPPORTED, global_precipitation=0, is_raining=0)
    for point in sunny["samples"].values():
        expect(point, actual_type=Precipitation.NONE, intensity=0)
    expect(sunny["grid"], valid=1, texture=1, x=-144, z=-128, size=256)

    stream.sendall(environment(scene, 11, rain=0.8, local_rain=0.8, flags=WORLD_WEATHER | RAINING))
    rain = probe.wait("rain_and_snow", stream, scene=scene, tick=11)
    expect(rain["world"], condition=Condition.RAIN, is_raining=1, is_thundering=0, camera_rain=0.8)
    check_rain(rain)

    stream.sendall(camera(scene, dry_eye))
    stream.sendall(environment(scene, 12, rain=0.8, flags=WORLD_WEATHER | RAINING))
    cross = probe.wait("dry_camera_crossing", stream, scene=scene, tick=12, eye=dry_eye)
    expect(cross["world"], camera_rain=0, camera_snow=0)
    expect(cross["grid"], valid=1, x=-128, z=-128, size=256)
    check_rain(cross)

    storm_flags = WORLD_WEATHER | RAINING | THUNDERING
    stream.sendall(environment(scene, 13, rain=0.8, thunder=0.98, flash=0.75, flags=storm_flags))
    storm = probe.wait("thunder_and_sky_flash_without_entity", stream, scene=scene, tick=13)
    expect(storm["world"], condition=Condition.THUNDERSTORM, is_thundering=1, thunder=0.98, sky_flash=0.75)
    expect(storm["lightning"], exists=0, sequence=0)

    position = (-15.125, 68.875, 11.5)
    strike = encode_lightning(scene, 10437, *position, 0xFEDCBA98, 0.125)
    stream.sendall(strike + strike + environment(scene, 14, rain=0.8, thunder=0.98, flags=storm_flags))
    impact = probe.wait("first_lightning_and_duplicate", stream, scene=scene, tick=14)
    expect_strike(impact, 1, scene, 0xFEDCBA98, position, reflash=False)
    time.sleep(0.3)
    stream.sendall(strike + environment(scene, 15, rain=0.8, thunder=0.98, flags=storm_flags))
    expect_strike(probe.wait("duplicate_after_lifetime", stream, scene=scene, tick=15),
                  1, scene, 0xFEDCBA98, position, reflash=False)

    flash = encode_lightning(scene, 10437, *position, 0x18273645, 0.125)
    stream.sendall(flash + strike + flash + environment(scene, 16, rain=0.8, thunder=0.98, flags=storm_flags))
    expect_strike(probe.wait("same_entity_new_seed_and_old_replays", stream, scene=scene, tick=16),
                  2, scene, 0x18273645, position, reflash=True)

    stream.sendall(environment(scene, 17, rain=0.15, thunder=0.01))
    fade = probe.wait("no_rain_with_residual_interpolation", stream, scene=scene, tick=17)
    expect(fade["world"], condition=Condition.CLEAR, global_precipitation=0.15, is_raining=0)
    expect(fade["samples"]["temperate"], column_type=Precipitation.RAIN, actual_type=Precipitation.NONE)
    expect(fade["samples"]["snow"], column_type=Precipitation.SNOW, actual_type=Precipitation.NONE)

    stream.sendall(environment(scene, 18, rain=0.8, flags=WORLD_WEATHER | RAINING, dimension=3))
    custom = probe.wait("custom_dimension_weather_capable", stream, scene=scene, tick=18)
    expect(custom["world"], condition=Condition.RAIN, dimension=3, capability=Capability.SUPPORTED)
    check_rain(custom)

    stream.sendall(environment(scene, 19, rain=0.8, thunder=0.98, flags=KNOWS_WEATHER, dimension=3))
    dry = probe.wait("custom_dimension_weather_disabled", stream, scene=scene, tick=19)
    expect(dry["world"], condition=Condition.CLEAR, dimension=3, capability=Capability.DISABLED, global_precipitation=0.8)
    expect(dry["samples"]["temperate"], column_type=Precipitation.RAIN, actual_type=Precipitation.NONE)

    stream.sendall(environment(scene, 20, rain=0.8, flags=0, dimension=3))
    legacy_other = probe.wait("legacy_custom_dimension_unknown", stream, scene=scene, tick=20)
    expect(legacy_other["world"], condition=Condition.UNKNOWN, capability=Capability.UNKNOWN)
    expect(legacy_other["samples"]["temperate"], actual_type=Precipitation.UNKNOWN)
    stream.sendall(environment(scene, 21, rain=0.8, flags=0))
    legacy = probe.wait("legacy_overworld", stream, scene=scene, tick=21)
    expect(legacy["world"], condition=Condition.RAIN, capability=Capability.UNKNOWN)
    check_rain(legacy)

    stream.sendall(frame(3, struct.pack(">2i", -1, 0)) +
                   environment(scene, 22, rain=0.8, flags=WORLD_WEATHER | RAINING))
    partial = probe.wait("drop_first_chunk", stream, scene=scene, tick=22, read_gpu=True)
    expect(partial["grid"], valid=1, texture=1)
    expect(partial["samples"]["temperate"], source=0, actual_type=Precipitation.UNKNOWN)
    expect(partial["samples"]["snow"], source=1, actual_type=Precipitation.SNOW)
    probe.expect_texels(partial, temperate=(0.0, 0.0, 0.0, 0.0), snow=(6400.0, 1.0, 1.0, 1.0))
    stream.sendall(frame(3, struct.pack(">2i", 0, 0)) +
                   environment(scene, 23, rain=0.8, flags=WORLD_WEATHER | RAINING))
    dropped = probe.wait("drop_last_chunk", stream, scene=scene, tick=23, read_gpu=True)
    expect(dropped["grid"], valid=0, texture=0, size=0)
    expect(dropped["samples"]["snow"], source=0, actual_type=Precipitation.UNKNOWN, unknown=1)
    probe.expect_texels(dropped, snow=(0.0, 0.0, 0.0, 0.0))

    stream.sendall(replay_columns(scene) + environment(scene, 24))
    restored = probe.wait("restored_pre_reset", stream, scene=scene, tick=24, read_gpu=True)
    expect(restored["grid"], valid=1, texture=1)
    probe.expect_texels(restored, temperate=(6400.0, 0.5, 1.0, 1.0), snow=(6400.0, 1.0, 1.0, 1.0))
    next_scene = scene + 1
    # Stale rows and events must not repopulate a newly loaded world.
    stream.sendall(world(next_scene) + replay_columns(scene) + strike)
    reset = probe.wait("world_reset_and_stale_frames", stream, scene=next_scene, read_gpu=True)
    expect(reset["world"], generation=restored["world"]["generation"] + 1, condition=Condition.UNKNOWN)
    expect(reset["grid"], valid=0, texture=0)
    expect(reset["samples"]["temperate"], source=0, actual_type=Precipitation.UNKNOWN, unknown=1)
    expect(reset["lightning"], exists=0)
    probe.expect_texels(reset, temperate=(0.0, 0.0, 0.0, 0.0), snow=(0.0, 0.0, 0.0, 0.0))

    position = (-29999998.75, 92.125, 29999997.625)
    stream.sendall(encode_lightning(next_scene, 10437, *position, 0xFEDCBA98, 0.125) +
                   environment(next_scene, 25))
    replayed = probe.wait("reused_entity_new_world_and_large_coordinates", stream, scene=next_scene, tick=25)
    expect_strike(replayed, 3, next_scene, 0xFEDCBA98, position, reflash=False)


def run_production_frame(stream, probe, golden):
    data = golden.read_bytes()
    [(kind, payload)] = list(frames(data))
    scene, cx, cz = struct.unpack_from(">Iii", payload)
    assert (kind, len(data), scene, cx, cz) == (35, 529, 29, -2, 3), golden
    print(f"FABRIC_GOLDEN {hashlib.sha256(data).hexdigest()} {golden}", flush=True)
    eye = (cx * 16 + 4.5, 75, cz * 16 + 4.5)
    stream.sendall(world(scene) + data + environment(scene, 26, rain=0.8, flags=WORLD_WEATHER | RAINING))
    stream.sendall(camera(scene, eye))
    points = {
        "plains": (cx * 16 + 4.5, 64.0, cz * 16 + 4.5),
        "desert": (cx * 16 + 12.5, 64.0, cz * 16 + 4.5),
        "snow": (cx * 16 + 4.5, 64.0, cz * 16 + 12.5),
        "swamp_under_glass": (cx * 16 + 12.5, 70.999, cz * 16 + 12.5),
        "swamp_at_glass_top": (cx * 16 + 12.5, 71.0, cz * 16 + 12.5),
        "highest": (cx * 16 + 5.5, 320.0, cz * 16 + 4.5),
        "empty": (cx * 16 + 4.5, -64.0, cz * 16 + 5.5),
    }
    cells = struct.unpack_from(">256H", payload, 12)
    missing = next(index for index, cell in enumerate(cells) if not cell & (1 << 14))
    points["missing_native_biome"] = (cx * 16 + missing % 16 + 0.5, 75, cz * 16 + missing // 16 + 0.5)
    report = probe.wait("fabric_production_chunk", stream, scene=scene, tick=26, eye=eye, points=points, read_gpu=True)
    samples = report["samples"]
    expect(samples["plains"], source=1, top_y=64, climate=Climate.TEMPERATE, actual_type=Precipitation.RAIN)
    expect(samples["desert"], source=1, top_y=64, climate=Climate.DRY, actual_type=Precipitation.NONE)
    expect(samples["snow"], source=1, top_y=64, climate=Climate.COLD, actual_type=Precipitation.SNOW)
    expect(samples["swamp_under_glass"], source=1, top_y=71, climate=Climate.HUMID, covered=1,
           column_type=Precipitation.RAIN, actual_type=Precipitation.NONE)
    expect(samples["swamp_at_glass_top"], source=1, top_y=71, climate=Climate.HUMID, covered=0,
           actual_type=Precipitation.RAIN)
    expect(samples["highest"], source=1, top_y=320, covered=0, actual_type=Precipitation.RAIN)
    expect(samples["empty"], source=1, top_y=-64, covered=0, actual_type=Precipitation.RAIN)
    expect(samples["missing_native_biome"], source=0, actual_type=Precipitation.UNKNOWN, unknown=1)
    probe.expect_texels(report, plains=(6400.0, 0.5, 1.0, 1.0), desert=(6400.0, 0.0, 1.0, 1.0),
                        snow=(6400.0, 1.0, 1.0, 1.0), swamp_under_glass=(7100.0, 0.5, 1.0, 1.0),
                        highest=(32000.0, 0.5, 1.0, 1.0), empty=(-6400.0, 0.5, 1.0, 1.0),
                        missing_native_biome=(0.0, 0.0, 0.0, 0.0))
    expect(report["lightning"], exists=0)


def run_production_lightning(stream, probe, golden):
    data = golden.read_bytes()
    [(kind, payload)] = list(frames(data))
    scene, entity, x, y, z, seed, remaining = struct.unpack(">II3dIf", payload)
    assert (kind, len(data), scene, x, y, z) == (36, 45, 29, -4.125, 76.5, 5.875), golden
    print(f"FABRIC_LIGHTNING_GOLDEN {hashlib.sha256(data).hexdigest()} {golden}", flush=True)
    if stream.scene != scene:
        stream.sendall(world(scene))
    stream.sendall(data + data +
                   environment(scene, 27, rain=1, thunder=1, flags=WORLD_WEATHER | RAINING | THUNDERING))
    report = probe.wait("fabric_production_lightning", stream, scene=scene, tick=27)
    expect_strike(report, 4, scene, seed, (x, y, z), reflash=False, entity=entity, remaining=remaining)


def save_puddle_depressions(path, enabled, *, keep_mtime=False):
    previous = path.stat() if keep_mtime else None
    pending = path.with_suffix(".tmp")
    pending.write_text(f"puddleDepressions={int(enabled)}\n", encoding="ascii")
    if previous is not None:
        os.utime(pending, ns=(previous.st_atime_ns, previous.st_mtime_ns))
    until = time.monotonic() + 2.0
    while True:
        try:
            pending.replace(path)
            break
        except PermissionError:
            if time.monotonic() >= until:
                raise
            time.sleep(0.010)
    if previous is not None:
        written = path.stat()
        assert (written.st_size, written.st_mtime_ns) == (previous.st_size, previous.st_mtime_ns), (previous, written)


def run_wet_surface(stream, probe, graphics_file=None):
    scene = 39
    eye = (-7.5, 66, 10.5)
    dry_eye = (1.5, 66, 10.5)
    walk_y = 63.875
    lane_z = 10.5
    points = {
        "stone": (-5.5, 64, 10.5),
        "sand": (1.5, 64, 9.5),
        "snow": (0.5, 64, 2.5),
        "under_eave": (-3.5, 64, 4.5),
        "eave_surface": (-3.5, 70, 4.5),
        "unknown": (2.5, 64, 4.5),
        "empty": (-6.5, -64, 4.5),
        "edited_column": (-0.5, 64, 9.5),
        "new_roof": (-0.5, 70, 9.5),
    }
    zero = (0.0, 0.0)
    if graphics_file is not None:
        save_puddle_depressions(graphics_file, False)
    stream.sendall(world(scene) + replay_columns(scene) + environment(scene, 100))
    stream.sendall(camera(scene, eye))
    clear = probe.wait("surface_initially_dry", stream, scene=scene, tick=100, eye=eye, points=points, read_gpu=True)
    expect(clear["wet-history"], rain_rate=0, mode=2, steps=0)
    assert clear["puddle-depressions"] == 0, clear
    assert all(value == zero for value in clear["moisture"].values()), clear
    probe.expect_moisture_texels(clear, stone=zero, sand=zero, snow=zero, unknown=zero)
    roof_code = column(70, Precipitation.RAIN, Climate.HUMID)
    stream.sendall(replay_columns(scene, {(-1, 9): roof_code}) + environment(scene, 100.25))
    dry_edit = probe.wait("surface_dry_edit_skips_history_upload", stream, scene=scene, tick=100.25, points=points,
                          check=lambda report: report["samples"]["new_roof"]["top_y"] == 70)
    assert dry_edit["wet-history-uploads"] == clear["wet-history-uploads"], (clear, dry_edit)

    stream.sendall(replay_columns(scene) + environment(scene, 101, rain=1, local_rain=1, flags=WORLD_WEATHER | RAINING))
    film = probe.wait("surface_film_precedes_puddles", stream, scene=scene, tick=101, points=points,
                      check=lambda report: report["moisture"]["stone"][0] >= 0.08)
    assert film["moisture"]["stone"][1] == 0, film
    active = probe.wait("surface_deeper_puddles", stream, scene=scene, tick=101, points=points, read_gpu=True,
                        check=lambda report: report["moisture"]["stone"][1] >= 0.82)
    for name in ("sand", "snow", "under_eave", "unknown", "empty", "new_roof"):
        assert active["moisture"][name] == zero, active
    stone = active["moisture"]["stone"]
    assert active["moisture"]["eave_surface"][1] >= 0.82, active
    probe.expect_moisture_texels(active, stone=stone, sand=zero, snow=zero, unknown=zero, empty=zero,
                                 under_eave=active["moisture"]["eave_surface"])

    if graphics_file is not None:
        settings_stamp = graphics_file.stat()
        # Exercise the same-size, same-mtime atomic replacement on successive renderer updates.
        for enabled in (True, False):
            save_puddle_depressions(graphics_file, enabled, keep_mtime=True)
            label = "same_metadata_puddle_depressions_on" if enabled else "same_metadata_puddle_depressions_off"
            reloaded = probe.wait(label, stream, scene=scene, tick=101, eye=eye, points=points,
                                  check=lambda report: report["puddle-depressions"] == int(enabled))
            assert reloaded["world"]["generation"] == active["world"]["generation"], reloaded
            assert reloaded["moisture"]["stone"][1] >= stone[1], reloaded
        print(f"SAME_METADATA_SETTINGS_RELOAD_PASS {settings_stamp.st_size} bytes mtime_ns={settings_stamp.st_mtime_ns}",
              flush=True)

    stream.sendall(camera(scene, dry_eye) + environment(scene, 102, rain=1, flags=WORLD_WEATHER | RAINING))
    dry_camera = probe.wait("surface_retained_across_dry_border_and_recenter", stream, scene=scene, tick=102,
                            eye=dry_eye, points=points, read_gpu=True)
    assert dry_camera["grid"]["x"] == active["grid"]["x"] + 16, dry_camera
    expect(dry_camera["wet-history"], rain_rate=1)
    expect(dry_camera["world"], camera_rain=0)
    assert dry_camera["moisture"]["stone"][1] >= stone[1], dry_camera
    probe.expect_moisture_texels(dry_camera, stone=dry_camera["moisture"]["stone"], sand=zero)

    # The raw intensity can still be tapering after Minecraft has stopped its rain condition.
    stream.sendall(environment(scene, 103, rain=0.15))
    stopped = probe.wait("surface_rain_off_with_residual_moisture", stream, scene=scene, tick=103, points=points)
    expect(stopped["wet-history"], rain_rate=0)
    assert stopped["moisture"]["stone"][0] > 0.95 and stopped["moisture"]["stone"][1] > 0.77, stopped

    def walk(start, movement_flags, *, steps=23):
        for index in range(steps):
            position = (start + index * 0.08, walk_y, lane_z)
            stream.publish_camera(camera(scene, eye, feet=position), movement_flags=movement_flags)
            stream.receive(0.005)
            time.sleep(0.030)
        return position

    feet = walk(-8.3, 3)
    stepped = probe.wait("surface_wet_footsteps_after_rain", stream, scene=scene, tick=103, points=points,
                         check=lambda result: result["wet-history"]["steps"] >= 2)
    expect(stepped["wet-history"], rain_rate=0, grounded=1)
    impacts = stepped["puddle-impacts"]
    assert len(impacts) >= 2 and impacts[0][0] < impacts[1][0], impacts
    assert (impacts[0][2] - lane_z) * (impacts[1][2] - lane_z) < 0, impacts
    assert all(hit[1] == walk_y and 0 <= hit[3] < 1.1 for hit in impacts), impacts
    step_count = stepped["wet-history"]["steps"]

    if graphics_file is not None:
        save_puddle_depressions(graphics_file, True, keep_mtime=True)
        on = probe.wait("puddle_depressions_enabled_in_same_scene", stream, scene=scene, tick=103, eye=eye,
                        points=points, check=lambda report: report["puddle-depressions"] == 1)
        assert on["world"]["generation"] == stepped["world"]["generation"], on
        assert on["player-feet"] == feet and on["moisture"]["stone"][1] > 0.65, on
        expect(on["wet-history"], steps=step_count, rain_rate=0, grounded=1)

        feet = walk(feet[0], 3)
        still_walks = probe.wait("puddle_depressions_keep_footsteps", stream, scene=scene, tick=103, eye=eye,
                                 points=points, check=lambda report: report["wet-history"]["steps"] >= step_count + 2)
        assert still_walks["puddle-depressions"] == 1 and still_walks["player-feet"] == feet, still_walks
        assert all(hit[1] == walk_y for hit in still_walks["puddle-impacts"]), still_walks
        step_count = still_walks["wet-history"]["steps"]

        save_puddle_depressions(graphics_file, False, keep_mtime=True)
        off = probe.wait("puddle_depressions_disabled_in_same_scene", stream, scene=scene, tick=103, eye=eye,
                         points=points, read_gpu=True, check=lambda report: report["puddle-depressions"] == 0)
        assert off["world"]["generation"] == stepped["world"]["generation"], off
        assert off["player-feet"] == feet and off["moisture"]["stone"][1] > 0.6, off
        expect(off["wet-history"], steps=step_count, rain_rate=0, grounded=1)
        probe.expect_moisture_texels(off, stone=off["moisture"]["stone"], sand=zero)

    # Changing the third-person camera leaves the last explicit player feet in the same place.
    stream.publish_camera(camera(scene, dry_eye, feet=feet), movement_flags=3)
    decayed = probe.wait("surface_stationary_camera_move_and_footstep_decay", stream, scene=scene, tick=103,
                         eye=dry_eye, points=points, read_gpu=True,
                         check=lambda result: result["wet-history"]["active"] == 0)
    expect(decayed["wet-history"], steps=step_count)
    assert 0 < decayed["moisture"]["stone"][1] < stopped["moisture"]["stone"][1], decayed
    assert decayed["moisture"]["stone"][0] > decayed["moisture"]["stone"][1], decayed
    probe.expect_moisture_texels(decayed, stone=decayed["moisture"]["stone"], sand=zero)

    stream.publish_teleport_epoch(1)
    waiting_pose = probe.wait("surface_teleport_epoch_before_pose", stream, scene=scene, tick=103, points=points,
                              check=lambda report: report["teleport-epoch"] == 1)
    assert waiting_pose["camera"] == dry_eye, waiting_pose
    time.sleep(0.09)
    warp_eye = (-6.1, 66, 10.5)
    warped_feet = (feet[0] + 0.82, walk_y, lane_z)
    stream.publish_camera(camera(scene, warp_eye, feet=warped_feet), movement_flags=3)
    warped = probe.wait("surface_no_footstep_from_small_teleport", stream, scene=scene, tick=103,
                        eye=warp_eye, points=points)
    assert warped["moisture"]["stone"][1] > 0.5, warped
    expect(warped["wet-history"], steps=step_count, active=0, grounded=1)

    # Along the border, the first foot lies three cm from the dry column and the second 29 cm inside.
    for index in range(23):
        position = (-0.16, walk_y, 9.1 + index * 0.08)
        stream.publish_camera(camera(scene, eye, feet=position), movement_flags=3)
        stream.receive(0.005)
        time.sleep(0.030)
    edge = probe.wait("surface_footsteps_skip_wet_side_of_dry_edge", stream, scene=scene, tick=103,
                      eye=eye, points=points, check=lambda result: result["wet-history"]["steps"] > step_count)
    expect(edge["wet-history"], steps=step_count + 1, active=1)
    impact, = edge["puddle-impacts"]
    assert math.isclose(impact[0], -0.29, abs_tol=1e-5), edge
    step_count += 1
    probe.wait("surface_border_footstep_decays", stream, scene=scene, tick=103, points=points,
               check=lambda result: result["wet-history"]["active"] == 0)

    for name, start, flags in (("legacy_unknown", -8.3, 0), ("airborne", -8.3, 1), ("dry", 1.1, 3)):
        walk(start, flags)
        report = probe.wait(f"surface_no_footsteps_{name}", stream, scene=scene, tick=103, eye=eye, points=points)
        expect(report["wet-history"], steps=step_count, active=0, grounded=int(flags == 3))

    stream.sendall(replay_columns(scene, {(-1, 9): roof_code}) + environment(scene, 104))
    edited = probe.wait("surface_topology_change_discards_old_water", stream, scene=scene, tick=104, points=points, read_gpu=True)
    assert edited["moisture"]["edited_column"] == zero and edited["moisture"]["new_roof"] == zero, edited
    assert edited["moisture"]["stone"][0] > 0.65, edited
    probe.expect_moisture_texels(edited, edited_column=zero, new_roof=zero, stone=edited["moisture"]["stone"])

    stream.sendall(frame(3, struct.pack(">2i", -1, 0)) + replay_columns(scene) + environment(scene, 105))
    reloaded = probe.wait("surface_reload_same_coordinates_starts_dry", stream, scene=scene, tick=105, points=points, read_gpu=True)
    assert all(value == zero for value in reloaded["moisture"].values()), reloaded
    probe.expect_moisture_texels(reloaded, stone=zero, sand=zero)

    stream.sendall(environment(scene, 106, rain=1, flags=WORLD_WEATHER | RAINING))
    probe.wait("surface_wet_before_world_reset", stream, scene=scene, tick=106, points=points, read_gpu=True,
               check=lambda result: result["moisture"]["stone"][0] >= 0.08)
    stream.sendall(world(scene + 1) + replay_columns(scene))
    reset = probe.wait("surface_world_reset", stream, scene=scene + 1, points=points, read_gpu=True)
    expect(reset["wet-history"], valid=0, rain_rate=0, steps=0, active=0)
    assert all(value == zero for value in reset["moisture"].values()), reset
    probe.expect_moisture_texels(reset, stone=zero, sand=zero)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--port", type=int, default=40688)
    parser.add_argument("--fabric-golden", type=Path)
    parser.add_argument("--fabric-lightning-golden", type=Path)
    parser.add_argument("--graphics-file", type=Path, help="Allow the fixture to create and toggle an isolated graphics settings file")
    parser.add_argument("--read-gpu", action="store_true")
    options = parser.parse_args()
    if options.graphics_file and options.graphics_file.exists():
        parser.error("--graphics-file must name a new file in the renderer's test profile")
    probe = WeatherProbe(options.output, options.read_gpu)
    with BridgeReplay(options.port, timeout=20) as stream:
        run_weather(stream, probe)
        if options.fabric_golden:
            run_production_frame(stream, probe, options.fabric_golden)
        if options.fabric_lightning_golden:
            run_production_lightning(stream, probe, options.fabric_lightning_golden)
        run_wet_surface(stream, probe, options.graphics_file)
    disconnected = probe.wait("disconnect", None, check=lambda state: not state["world"]["has_world"])
    expect(disconnected["grid"], valid=0, texture=0)
    expect(disconnected["lightning"], exists=0)
    expect(disconnected["samples"]["temperate"], source=0, actual_type=Precipitation.UNKNOWN, unknown=1)
    print(f"WEATHER_DATA_PASS {len(probe.results)} stages", flush=True)


if __name__ == "__main__":
    main()
