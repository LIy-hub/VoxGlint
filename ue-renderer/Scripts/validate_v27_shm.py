"""Check v27 mapped terrain replay and priority against an isolated UE -BridgeTrace process."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
from pathlib import Path
import re
import struct
import time

from bridge_v27_replay import BridgeReplay, MAX_SECTION, frame, frames
from validate_v23_renderer import Log, atlas, camera_frame, floor, lights, section


ATLAS = atlas(6) + atlas(27) + atlas(28)


def empty_lights(scene, mesh):
    return struct.pack(">I", scene) + mesh[:12] + bytes(2)


def validate_capture(path):
    stats = Counter()
    pending = {}
    output = bytearray()
    mesh_hash, lights_hash = hashlib.sha256(), hashlib.sha256()
    for kind, body in frames(path.read_bytes()):
        if kind == 1:
            assert struct.unpack_from(">II", body) == (0x4D435545, 26)
            continue
        if kind == 2:
            counts = struct.unpack_from(">6I", body, 12)
            assert sum(counts) <= 1048576 and all(count % 4 == 0 for count in counts)
            metadata = 36 + sum(counts) // 4 * 101
            water, shore = struct.unpack_from(">II", body, metadata)
            assert water == counts[4] // 4 and shore <= (sum(counts) - counts[4]) // 2
            assert len(body) == metadata + 8 + water * 16 + shore * 17
            for index in range(shore):
                layer, quad = struct.unpack_from(">BI", body, metadata + 8 + water * 16 + index * 17)
                assert layer < 6 and layer != 4 and quad < counts[layer] // 4
            assert body[:12] not in pending
            pending[body[:12]] = body
            mesh_hash.update(body)
            stats.update(meshes=1, empty=int(not sum(counts)), water=water, shore=shore, mesh_bytes=len(body))
        elif kind == 25:
            count, = struct.unpack_from(">H", body, 16)
            assert len(body) == 18 + count * 7 and count <= 4096
            seen = set()
            for packed, _, _, _, level, flags in struct.iter_unpack(">H5B", body[18:]):
                assert packed < 4096 and packed not in seen and 0 < level <= 15
                assert flags == 0 or ((flags & 0xC0) == 0x40 and (flags & 0x3F) != 0)
                seen.add(packed)
            output.extend(frame(2, pending.pop(body[4:16])))
            output.extend(frame(kind, body))
            lights_hash.update(body)
            stats.update(light_frames=1, emitters=count, light_bytes=len(body))
        else:
            output.extend(frame(kind, body))
            if kind == 11:
                output.extend(ATLAS)
    assert not pending and stats["meshes"] == stats["light_frames"] and stats["water"] and stats["shore"]
    print(f"CAPTURE {path.name} {dict(stats)} meshSha256={mesh_hash.hexdigest()} lightsSha256={lights_hash.hexdigest()}", flush=True)
    return output, stats["meshes"]


def replay_capture(port, log, path):
    payload, count = validate_capture(path)
    start = log.end()
    with BridgeReplay(port) as bridge:
        bridge.publish_camera(camera_frame(2, (7.5, 7.38, -12.5)))
        bridge.sendall(payload)
        bridge.wait_until(lambda: bridge.ready_scene == 1 and not bridge.terrain_leases, timeout=90)
        assert bridge.token == len(bridge.release_order) == count
        log.wait(fr"Live .*sections={count} .*cameras=[1-9]\d* .*meshes=[1-9]\d* .*pending=0", after=start)
        print(f"PASS {path.name} {count} mapped mesh/light pairs released, SceneReady=1", flush=True)
    log.wait("Minecraft bridge disconnected", after=start)


def sample_ue_time(line):
    match = re.match(r"\[([\d.\-:]+)\]\[\s*(\d+)\]", line)
    assert match, line
    stamp = datetime.strptime(match[1], "%Y.%m.%d-%H.%M.%S:%f").replace(tzinfo=timezone.utc)
    return int(match[2]), stamp.timestamp() * 1000


def verify_priority(port, log):
    start = log.end()
    with BridgeReplay(port) as bridge:
        bridge.publish_camera(camera_frame(1, (7.5, 7.38, -16.5)))
        environment = frame(24, struct.pack(">IBBBB6fI3ff", 1, 0, 0, 0, 0, 0.58, 0, 0, 0, 0, 0, 0x88A9FF,
                                            0.3, 0.45, 0.66, 120))
        bridge.sendall(frame(11, struct.pack(">Iii", 1, -64, 384)) + ATLAS + environment)
        tiles = b"".join(floor(x, z) for x in range(16) for z in range(16))
        batch = bytearray()
        for x in range(16, 112):
            body = section(x, 0, tiles, b"")[5:]
            descriptor = bridge.section(body, empty_lights(1, body))
            assert descriptor is not None
            batch.extend(descriptor)
        body = section(0, 0, floor(8, 8), b"")[5:]
        assert bridge.section(body, empty_lights(1, body)) is None, "bulk count must be bounded"
        older = bridge.token + 1
        batch.extend(bridge.section(body, empty_lights(1, body), urgent=True))
        edited = bridge.token + 1
        body = section(0, 0, floor(8, 8) + floor(9, 8), b"")[5:]
        batch.extend(bridge.section(body, lights(0, (8, 1, 8, 0x48), (9, 1, 8, 0))[5:], urgent=True))
        publish_ms = time.time_ns() / 1e6
        bridge.socket.sendall(batch + frame(15, struct.pack(">I", 1)))
        bridge.wait_until(lambda: bridge.ready_scene == 1 and not bridge.terrain_leases, timeout=90)
        releases = bridge.release_order
        assert len(releases) == len(set(releases)) == 98
        assert releases.index(older) < releases.index(edited) < releases.index(96)
        dispatch = log.wait(fr"V27 terrain near dispatch scene=1 token={edited} .*bulkPending=[1-9]\d*", after=start)
        commit = log.wait(fr"V27 terrain near commit scene=1 token={edited} ", after=start)
        assert f"near commit scene=1 token={older} " not in log.path.read_text(encoding="utf-8", errors="replace")
        assert sample_ue_time(commit)[1] >= sample_ue_time(dispatch)[1]
        print(f"PASS near coalesced tokens {older}->{edited}, returned at index {releases.index(edited)}, "
              f"96 bulk; send-to-commit={sample_ue_time(commit)[1] - publish_ms:.2f} ms "
              f"(sendall entry to UE log UTC, frame {sample_ue_time(commit)[0]})", flush=True)
        log.wait(r"SceneLighting .*lightSections=1 sources=2 active=2", after=start)
        log.wait(r"Live .*meshes=97 .*pending=0", after=start)
        removal_start = log.end()
        empty = section(0, 0, b"", b"")[5:]
        bridge.socket.sendall(bridge.section(empty, empty_lights(1, empty), urgent=True))
        bridge.wait_until(lambda: not bridge.terrain_leases)
        log.wait(r"SceneLighting .*lightSections=0 sources=0 active=0", after=removal_start)
        log.wait(r"Live .*meshes=96 .*pending=0", after=removal_start)
        print("PASS nonempty light list and empty-section geometry/light removal", flush=True)

        # Largest legal v26 section: before decoding, unload that column in the same reliable batch.
        quad_count = 262144
        shore = b"".join(struct.pack(">BI6H", 0, index, 0, 0, 0, 256, 0, 0) * 2 for index in range(quad_count))
        large = (struct.pack(">3i6I", 120, 0, 0, 4 * quad_count, 0, 0, 0, 0, 0)
                 + floor(0, 0) * quad_count + struct.pack(">II", 0, 2 * quad_count) + shore)
        assert len(large) == MAX_SECTION
        big = bridge.section(large, empty_lights(1, large))
        assert big is not None and bridge.section(large, empty_lights(1, large)) is None
        bridge.socket.sendall(big + frame(3, struct.pack(">ii", 120, 0)))
        bridge.wait_until(lambda: not bridge.terrain_leases)
        print(f"PASS large lane {len(large)} mesh bytes, drop returned lease {bridge.token}", flush=True)

        # Reuse the same coordinates in a new scene before the old work is dispatched.
        before = bridge.token
        stale = bridge.section(body, empty_lights(1, body), urgent=True)
        bridge.scene = 2
        newer = bridge.section(body, empty_lights(2, body), urgent=True)
        pose = bytearray(camera_frame(2, (7.5, 7.38, -12.5)))
        struct.pack_into(">I", pose, 5, 2)
        bridge.publish_camera(pose)
        bridge.socket.sendall(stale + frame(11, struct.pack(">Iii", 2, -64, 384)) + ATLAS
                              + newer + frame(15, struct.pack(">I", 2)))
        bridge.wait_until(lambda: bridge.ready_scene == 2 and not bridge.terrain_leases)
        log.wait(fr"V27 terrain near commit scene=2 token={before + 2} ", after=start)
        assert bridge.release_order[-2:] == [before + 1, before + 2]
        print("PASS World barrier returned stale work and applied the new scene", flush=True)
    log.wait("Minecraft bridge disconnected", after=start)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--captures", nargs="*", type=Path, default=[])
    args = parser.parse_args()
    log = Log(args.log)
    for path in args.captures:
        replay_capture(args.port, log, path)
    verify_priority(args.port, log)


if __name__ == "__main__":
    main()
