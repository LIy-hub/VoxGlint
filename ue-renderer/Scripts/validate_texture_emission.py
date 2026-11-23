"""Replay actual Fabric atlas bytes, block states and animated light records under fixed UE exposure."""
import argparse
import csv
import json
import math
import struct
import time
from pathlib import Path

import numpy as np
from PIL import Image

from bridge_v27_replay import BridgeReplay, frame
from validate_material_lights import surface
from validate_v23_renderer import section
from validate_water_p2 import capture, env, pose


EYE = (8.0, 1.5, 4.2)
FOV = 40
TOWARD = (0, 0, 1)
EMPTY = frame(25, struct.pack(">I3iH", 1, 0, 0, 0, 0))


def scene_atlas(fixtures):
    return b"".join(frame(i, struct.pack(">6H", 0, 1, 128, 16, 0, 16)
                          + (fixtures / f"atlas-{i}.bgra").read_bytes()) for i in (6, 27, 28))


def models(fixtures, enabled):
    geometry = bytearray()
    flags = (fixtures / "face-flags.bin").read_bytes()
    levels = (fixtures / "face-levels.bin").read_bytes()
    for i, slot in enumerate((0, 1, 2, 2)):
        x = 6 + i
        geometry.append(2 | (flags[i] & 0x38 if enabled else 8) | (64 if enabled and levels[i] else 0))
        for wx, wy, u, v in ((1,0,0,1),(0,0,1,1),(0,1,1,0),(1,1,0,0)):
            geometry.extend(struct.pack(">5f5B", x+wx,1+wy,8,(slot+u)/8,v,255,255,255,255,levels[i] if enabled else 0))
    return section(0,0,geometry,b"")


def patch(fixtures, slot, name, index):
    payload = (fixtures / f"{name}-{index}.bgra").read_bytes()
    return frame(32,struct.pack(">IIH5H",1,1,1,0,slot*16,0,16,16) + payload)


def ready(stream, fixtures, mesh, eye, direction, fov, atlas=None):
    assert struct.unpack_from("<Q", stream.control,16)[0] & 160 == 160, "missing surface and mineral emission support"
    stream.sendall(frame(11,struct.pack(">Iii",1,-64,384)) + (scene_atlas(fixtures) if atlas is None else atlas) + env("Night")
                   + pose(eye,direction,fov) + mesh + EMPTY + frame(15,struct.pack(">I",1)))
    stream.wait_until(lambda: stream.ready_scene == 1 and not stream.terrain_leases)
    time.sleep(2)


def rgb(path):
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB")).copy()


def texels(pixels, block, y=1, eye=EYE, fov=FOV):
    height, width = pixels.shape[:2]
    scale = height / (2 * math.tan(math.radians(fov/2)) * (8-eye[2]))
    left = width/2 - (block + 1 - eye[0]) * scale
    top = height/2 - (y+1 - eye[1]) * scale
    return np.asarray([[pixels[round(top + (v+.5)*scale/16), round(left + (u+.5)*scale/16)]
                        for u in range(16)] for v in range(16)], dtype=float)


def verify_faces(output, fixtures):
    off = rgb(output/"materials-off.png")
    frames = [rgb(output/f"stock-{i}.png") for i in range(5)]
    packed = np.fromfile(fixtures/"atlas-28.bgra",dtype=np.uint8).reshape(16,128,4)
    mask = packed[:,:16,0]
    glow = (texels(frames[0],6) - texels(off,6)).mean(axis=2)
    sea = np.asarray([texels(im,7) for im in frames])
    border = np.ones((16,16),bool)
    border[2:-2,2:-2] = False
    change = np.ptp(sea.mean(axis=3),axis=0)
    ore = (texels(frames[0],8)-texels(off,8)).mean(axis=2)
    inactive = (texels(frames[0],9)-texels(off,9)).mean(axis=2)
    red_mask = packed[:,32:48,0]
    sea_delta = (sea[0] - texels(off,7)).mean(axis=2)
    values = {
        "glowstone_mean_bright_delta": float(glow[mask>150].mean()),
        "glowstone_mean_dark_delta": float(glow[mask==mask.min()].mean()),
        "sea_inner_90pct_frame_range": float(np.percentile(change[~border],90)),
        "sea_outer_median_frame_range": float(np.median(change[border])),
        "sea_outer_median_base_glow": float(np.median(sea_delta[border])),
        "redstone_lit_bright_delta": float(np.percentile(ore,94)),
        "redstone_inactive_grains": float(np.mean(inactive[red_mask>150])),
        "redstone_inactive_rock": float(np.median(np.abs(inactive[red_mask==0]))),
    }
    (output/"surface-probes.json").write_text(json.dumps(values,indent=2),encoding="utf-8")
    print(values,flush=True)
    assert values["glowstone_mean_bright_delta"] > values["glowstone_mean_dark_delta"] + 30, values
    assert values["glowstone_mean_dark_delta"] > 5 and values["sea_outer_median_base_glow"] > 5, values
    assert values["sea_inner_90pct_frame_range"] > values["sea_outer_median_frame_range"] + 8, values
    assert values["redstone_lit_bright_delta"] > values["redstone_inactive_grains"]+25, values
    assert values["redstone_inactive_grains"]>4 and values["redstone_inactive_rock"]<8, values


def ore_gallery(output, fixtures, port):
    with (fixtures/"ore-mask-coverage.csv").open(encoding="utf-8") as handle:
        slots = {row["texture"].split("/")[-1]:int(row["atlas_slot"]) for row in csv.DictReader(handle)}
    rows = (("diamond_ore","emerald_ore","lapis_ore","iron_ore","copper_ore"),
            ("deepslate_diamond_ore","deepslate_emerald_ore","deepslate_lapis_ore","deepslate_iron_ore","deepslate_copper_ore"),
            ("coal_ore","gold_ore","redstone_ore","nether_gold_ore","nether_quartz_ore"),
            ("deepslate_coal_ore","deepslate_gold_ore","deepslate_redstone_ore","gilded_blackstone","ancient_debris_side"))
    blocks = [(name,9-col,3-row) for row,line in enumerate(rows) for col,name in enumerate(line)]
    eye, fov = (7.5,2.0,2.3),48
    atlas = b"".join(frame(i,struct.pack(">6H",0,1,512,16,0,16)+(fixtures/f"ore-atlas-{i}.bgra").read_bytes())
                     for i in (6,27,28))
    def mesh(mode):
        data = bytearray()
        for name,x,y in blocks:
            data.append(2 | mode << 3)
            for wx,wy,u,v in ((1,0,0,1),(0,0,1,1),(0,1,1,0),(1,1,0,0)):
                data.extend(struct.pack(">5f5B",x+wx,y+wy,8,(slots[name]+u)/32,v,255,255,255,255,0))
        return section(0,0,data,b"")
    with BridgeReplay(port,timeout=60) as stream:
        ready(stream,fixtures,mesh(5),eye,TOWARD,fov,atlas)
        for label,mode in (("ore-glow",5),("ore-off",1)):
            stream.sendall(mesh(mode))
            capture(stream,output,label,eye,TOWARD,fov,0,0,"Night",False,1.2)
    on,off = rgb(output/"ore-glow.png"),rgb(output/"ore-off.png")
    packed = np.fromfile(fixtures/"ore-atlas-28.bgra",dtype=np.uint8).reshape(16,512,4)
    result = {}
    for name,x,y in blocks:
        slot = slots[name]
        gate = packed[:,16*slot:16*(slot+1),0]
        delta = texels(on,x,y,eye,fov)-texels(off,x,y,eye,fov)
        grains = delta[gate>100]
        bright = grains[np.argsort(grains.max(axis=1))[-8:]].mean(axis=0)
        rock = np.median(np.abs(delta[gate==0]))
        result[name] = {"grain_rgb_delta":np.round(bright,1).tolist(),"median_rock_delta":round(float(rock),2)}
    (output/"ore-probes.json").write_text(json.dumps(result,indent=2),encoding="utf-8")
    for name,reading in result.items():
        score=reading["grain_rgb_delta"]
        assert max(score)>4 and max(score)<155 and reading["median_rock_delta"]<8,(name,reading)
    diamond=result["diamond_ore"]["grain_rgb_delta"]
    emerald=result["emerald_ore"]["grain_rgb_delta"]
    lapis=result["lapis_ore"]["grain_rgb_delta"]
    assert min(diamond[1:])>diamond[0]+4 and emerald[1]>max(emerald[0],emerald[2])+4 and lapis[2]>lapis[0]+4,result


def run(args):
    fixtures, output = args.fixtures,args.output
    with BridgeReplay(args.port,timeout=60) as stream:
        ready(stream,fixtures,models(fixtures,True),EYE,TOWARD,FOV)
        for i in range(5):
            stream.sendall(patch(fixtures,1,"sea_lantern",i))
            capture(stream,output,f"stock-{i}",EYE,TOWARD,FOV,0,0,"Night",False,.8)
        stream.sendall(models(fixtures,False) + patch(fixtures,1,"sea_lantern",0))
        capture(stream,output,"materials-off",EYE,TOWARD,FOV,0,0,"Night",False,1.0)
    eye, direction = (8.5,1.5,9.9),(0,0,-1)
    # White receiver and off-camera lamp geometry are independent; only atlas rows change between red and blue.
    vertices = ((4,-1,7.72),(13,-1,7.72),(13,5,7.72),(4,5,7.72))
    wall = bytearray(surface(3 | 8,vertices))
    for i in range(4):
        struct.pack_into(">2f",wall,1+i*25+12,5.5/8,.5)
    values = {}
    with BridgeReplay(args.port,timeout=60) as stream:
        ready(stream,fixtures,section(0,0,wall,b""),eye,direction,34)
        stream.sendall((fixtures/"animated-lights.bin").read_bytes())
        for i,name in enumerate(("wall-red","wall-blue","wall-removed")):
            stream.sendall(patch(fixtures,7,"animated_lamp",i) if i<2 else EMPTY)
            capture(stream,output,name,eye,direction,34,0,0,"Night",False,1.2)
            im = rgb(output/(name+".png"))
            h,w = im.shape[:2]
            values[name] = np.round(im[h//2-9:h//2+9,w//2-9:w//2+9].mean(axis=(0,1)),2).tolist()
    (output/"animated-wall-probes.json").write_text(json.dumps(values,indent=2),encoding="utf-8")
    print(values,flush=True)
    red,blue,removed = values.values()
    assert red[0]>red[2]+20 and blue[2]>blue[0]+20 and min(red[0],blue[2])>max(removed)+20, values
    try:
        ore_gallery(output,fixtures,args.port)
    finally:
        verify_faces(output,fixtures)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output",type=Path)
    parser.add_argument("--port",required=True,type=int)
    parser.add_argument("--fixtures",type=Path,default=Path(__file__).resolve().parents[2] /
                        "fabric-bridge/build/tmp/verifySectionLighting/texture-emission")
    run(parser.parse_args())
