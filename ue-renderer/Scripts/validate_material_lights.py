"""Replay Fabric's two-colour lamp records in UE and measure direct illumination on a neutral wall."""
import argparse
import io
import json
import math
import struct
import time
from pathlib import Path
from zipfile import ZipFile

from PIL import Image, ImageStat
from bridge_v27_replay import BridgeReplay, frame
from validate_v23_renderer import section
from validate_water_p2 import capture, env, pose


def flat_atlas(kind, bgra):
    return frame(kind, struct.pack(">6H", 0, 1, 4, 4, 0, 4) + bytes(bgra) * 16)


def surface(face, vertices):
    return bytes([face]) + b"".join(struct.pack(">5f5B", *p, 0.5, 0.5, 255, 255, 255, 255, 0) for p in vertices)


def channels(image, fx, fy):
    x, y = round(fx * image.width), round(fy * image.height)
    return [round(v, 2) for v in ImageStat.Stat(image.crop((x-8, y-8, x+9, y+9))).mean[:3]]


def check_vanilla_probes(output):
    # Centres of stock model texels in the fixed 1024x576 composition, away from their filtered borders.
    points = {"tip": (253, 247), "wood": (254, 343), "window": (505, 346), "lid": (496, 284),
              "handle": (496, 228), "vein": (820, 198), "crust": (806, 228), "floor": (475, 414)}
    probes = {}
    for mode in ("vanilla-emission", "vanilla-emission-off", "vanilla-lit"):
        with Image.open(output / (mode + ".png")) as image:
            values = {}
            for key, (x, y) in points.items():
                px, py = round(x * image.width / 1024), round(y * image.height / 576)
                region = image.crop((px-2, py-2, px+3, py+3))
                values[key] = [round(value, 2) for value in ImageStat.Stat(region).mean[:3]]
            probes[mode] = values
    (output / "vanilla-probes.json").write_text(json.dumps(probes, indent=2), encoding="utf-8")
    on, off, lit = (probes[m] for m in ("vanilla-emission", "vanilla-emission-off", "vanilla-lit"))
    for key in ("tip", "window", "vein"):
        assert on[key][0] - off[key][0] > 90, (key, probes)
    for key in ("wood", "lid", "handle", "crust"):
        assert max(abs(a - b) for a, b in zip(on[key], off[key])) < 30, (key, probes)
    assert lit["floor"][0] > on["floor"][0] + 50, probes


def vanilla_materials(jar, masks, contracts, port, output):
    names = ("magma", "lantern", "torch")
    albedo = Image.new("RGBA", (128, 16), (120, 120, 120, 255))
    packed = Image.new("RGBA", albedo.size, (195, 0, 0, 255))
    models = []
    with ZipFile(jar) as assets:
        for index, name in enumerate(names, 1):
            raw = assets.read("assets/minecraft/textures/block/" + name + ".png")
            tile = Image.open(io.BytesIO(raw)).convert("RGBA").crop((0, 0, 16, 16))
            mask = Image.open(masks / (name + "-mask.png")).getchannel("R")
            tex = Image.merge("RGBA", (Image.new("L", tile.size, 195), Image.new("L", tile.size), mask,
                                       Image.new("L", tile.size, 255)))
            for slot in (index, index + 3):
                albedo.paste(tile, (16 * slot, 0))
            packed.paste(tex, (16 * index, 0))
            if name == "magma":
                models.append([{"from": [0,0,0], "to": [16,16,16],
                                "faces": {d: {"uv":[0,0,16,16]} for d in FACE_NAMES}}])
            else:
                model = json.loads(assets.read("assets/minecraft/models/block/template_" + name + ".json"))
                models.append(model["elements"])
    def rows(kind, texture):
        return frame(kind, struct.pack(">6H", 0, 1, 128, 16, 0, 16) + texture.tobytes("raw", "BGRA"))

    def mesh(disable):
        solid, masked = bytearray(), bytearray()
        for index, elements in enumerate(models, 1):
            geometry = solid if index == 1 else masked
            for element in elements:
                for side, face in element["faces"].items():
                    corners = BOX_CORNERS[FACE_NAMES.index(side)]
                    left, top, right, bottom = face["uv"]
                    uvs = [(left,bottom), (right,bottom), (right,top), (left,top)]
                    geometry.append(FACE_NAMES.index(side))
                    for corner, (u,v) in zip(corners, uvs):
                        point = [element["to" if k else "from"][i] for i,k in enumerate(corner)]
                        rot = element.get("rotation")
                        if rot:
                            # The two hanging loops in the vanilla standing lantern rotate about Y.
                            assert rot["axis"] == "y"
                            ox, _, oz = rot["origin"]
                            x, z, angle = point[0]-ox, point[2]-oz, math.radians(rot["angle"])
                            point[0] = ox + x*math.cos(angle) + z*math.sin(angle)
                            point[2] = oz + z*math.cos(angle) - x*math.sin(angle)
                        xyz = (6 + index + point[0]/16, 1 + point[1]/16, 8 + point[2]/16)
                        slot = index + (3 if disable else 0)
                        geometry.extend(struct.pack(">5f5B", *xyz, (16*slot+u)/128, v/16, 255,255,255,255,0))
        floor_uv = (0.05, 0.5)
        solid.append(1)
        for p in [(5,1,5),(5,1,13),(12,1,13),(12,1,5)]:
            solid.extend(struct.pack(">5f5B", *p, *floor_uv, 255,255,255,255,0))
        return section(0, 0, solid, masked)

    eye, look = (8.47, 1.82, 4.65), (0, -0.105, 1)
    with BridgeReplay(port, timeout=60) as stream:
        stream.sendall(frame(11, struct.pack(">Iii", 1, -64, 384)) +
                       rows(6, albedo) + rows(27, Image.new("RGBA", albedo.size, (128,128,255,255))) +
                       rows(28, packed) + env("Night") + pose(eye, look, 34) + mesh(False) +
                       frame(15, struct.pack(">I", 1)))
        stream.wait_until(lambda: stream.ready_scene == 1 and not stream.terrain_leases)
        time.sleep(3)
        for label, disable in [("vanilla-emission",False), ("vanilla-emission-off",True)]:
            stream.sendall(mesh(disable))
            capture(stream, output, label, eye, look, 34, 0, 0, "Night", False, 1.2)
        stream.sendall(mesh(False) + (contracts / "vanilla-lights.bin").read_bytes())
        capture(stream, output, "vanilla-lit", eye, look, 34, 0, 0, "Night", False, 1.5)
    check_vanilla_probes(output)


FACE_NAMES = ("down","up","north","south","west","east")
BOX_CORNERS = (
    ((0,0,0),(1,0,0),(1,0,1),(0,0,1)), ((0,1,0),(0,1,1),(1,1,1),(1,1,0)),
    ((1,0,0),(0,0,0),(0,1,0),(1,1,0)), ((0,0,1),(1,0,1),(1,1,1),(0,1,1)),
    ((0,0,0),(0,0,1),(0,1,1),(0,1,0)), ((1,0,1),(1,0,0),(1,1,0),(1,1,1)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--contracts", type=Path, default=Path(__file__).resolve().parents[2] /
                        "fabric-bridge/build/tmp/verifySectionLighting")
    parser.add_argument("--vanilla-jar", type=Path, help="Also render stock models with the verified Fabric masks")
    args = parser.parse_args()
    lamp, mirror = [(args.contracts / name).read_bytes() for name in ("lamp.bin", "lamp-mirror.bin")]
    # The emissive geometry is outside this test. The receiving wall has no emissive texels or MC fill.
    wall = surface(3, ((4, -1, 7.75), (13, -1, 7.75), (13, 5, 7.75), (4, 5, 7.75)))
    eye, direction = (8.5, 1.5, 9.9), (0, 0, -1)
    fov = 34
    empty = frame(25, struct.pack(">I3iH", 1, 0, 0, 0, 0))
    with BridgeReplay(args.port, timeout=60) as stream:
        stream.sendall(frame(11, struct.pack(">Iii", 1, -64, 384)) +
                       flat_atlas(6, (158, 158, 158, 255)) + flat_atlas(27, (255, 128, 128, 255)) +
                       flat_atlas(28, (0, 0, 210, 255)) + env("Night") + pose(eye, direction, fov) +
                       section(0, 0, wall, b"") + empty + frame(15, struct.pack(">I", 1)))
        stream.wait_until(lambda: stream.ready_scene == 1 and not stream.terrain_leases)
        time.sleep(3)
        values = {}
        for label, light in [("original",lamp), ("mirrored",mirror), ("removed",empty)]:
            stream.sendall(light)
            capture(stream, args.output, label, eye, direction, fov, 0, 0, "Night", False, 1.2)
            with Image.open(args.output / (label + ".png")) as image:
                values[label] = [channels(image, x, 0.5) for x in (.39, .61)]
        # The left/right orientation on screen depends on the camera convention; mirrored UVs must swap both colours.
        print(json.dumps(values, indent=2))
        (args.output / "wall-probes.json").write_text(json.dumps(values, indent=2), encoding="utf-8")
        delta = lambda v: v[0] - v[2]
        a, b = map(delta, values["original"])
        c, d = map(delta, values["mirrored"])
        assert min(abs(a), abs(b), abs(c), abs(d)) > 12 and a*b < 0 and a*c < 0 and b*d < 0, values
        assert all(max(v) > max(off) + 20 for v, off in zip(values["original"], values["removed"])), values
    if args.vanilla_jar:
        vanilla_materials(args.vanilla_jar, args.contracts.parent / "verifyTerrainAtlas/material-lights",
                         args.contracts, args.port, args.output)


if __name__ == "__main__":
    main()
