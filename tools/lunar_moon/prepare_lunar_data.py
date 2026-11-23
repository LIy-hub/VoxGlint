"""Convert the NASA CGI Moon Kit downloads into Unreal texture and mesh inputs."""

import argparse
import math
import struct
from pathlib import Path

import numpy as np
from PIL import Image


WIDTH, HEIGHT = 8192, 4096
LONGITUDE_STEPS, LATITUDE_STEPS = 128, 64
LOLA_DATUM = 20_000  # Unsigned TIFF value at the 1,737.4 km reference radius.
HALF_METRES_PER_RADIUS = 3_474_800
MODEL_RADIUS_CM = 1000.0


def bilinear(field, u, v):
    """Sample a pixel-centered global map with longitude wrap and latitude clamp."""
    height, width = field.shape
    x = u * width - 0.5
    y = np.clip(v * height - 0.5, 0, height - 1)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    blend_x = (x - x0).astype(np.float32)
    blend_y = (y - y0).astype(np.float32)
    y1 = np.minimum(y0 + 1, height - 1)
    x1 = (x0 + 1) % width
    x0 %= width
    top = field[y0, x0] * (1 - blend_x) + field[y0, x1] * blend_x
    bottom = field[y1, x0] * (1 - blend_x) + field[y1, x1] * blend_x
    return top * (1 - blend_y) + bottom * blend_y


def bake_relief(dem, output):
    height, width = dem.shape
    latitude = math.pi * (0.5 - (np.arange(height, dtype=np.float32) + 0.5) / height)
    # Central differences measured in metres per metre along the reference sphere.
    # Fade the tangent perturbation inside the last two source rows at each pole.
    metres_per_radian = 1_737_400.0
    cos_lat = np.cos(latitude)[:, None]
    metres_east = metres_per_radian * math.tau / width * np.maximum(cos_lat, 2 * math.pi / height)
    metres_south = metres_per_radian * math.pi / height
    east = (np.roll(dem, -1, axis=1) - np.roll(dem, 1, axis=1)) * (0.25 / metres_east)
    south = np.gradient(dem, axis=0) * (0.5 / metres_south)
    polar_fade = np.clip(cos_lat / (2 * math.pi / height), 0, 1)
    east *= polar_fade
    south *= polar_fade

    u = (np.arange(WIDTH, dtype=np.float32)[None, :] + 0.5) / WIDTH
    elevation = np.empty((HEIGHT, WIDTH), dtype=np.uint16)
    normals = np.empty((HEIGHT, WIDTH, 3), dtype=np.uint8)
    for begin in range(0, HEIGHT, 128):
        end = min(begin + 128, HEIGHT)
        v = (np.arange(begin, end, dtype=np.float32)[:, None] + 0.5) / HEIGHT
        elevation[begin:end] = np.rint(bilinear(dem, u, v)).astype(np.uint16)
        x = -bilinear(east, u, v)
        y = -bilinear(south, u, v)
        z = 1.0 / np.sqrt(1.0 + x * x + y * y)
        unit_normal = np.stack((x * z, y * z, z), axis=-1)
        normals[begin:end] = np.rint((unit_normal + 1.0) * 127.5).astype(np.uint8)

    Image.fromarray(elevation).save(output / "T_LunarElevation_16bit.png", compress_level=3)
    normal_image = Image.fromarray(normals)
    normal_image.save(output / "T_LunarNormal_8K.png", compress_level=3)
    normal_image.resize((WIDTH // 2, HEIGHT // 2), Image.Resampling.BOX).save(
        output / "T_McMoonNormal_4K.png", compress_level=3)


def bake_mesh(dem, output):
    vertices = []
    triangles = []
    # The equator at longitude zero faces +X, north is +Z. From +X, east is screen-right.
    u_ring = np.arange(LONGITUDE_STEPS + 1, dtype=np.float64) / LONGITUDE_STEPS
    v_ring = np.arange(1, LATITUDE_STEPS, dtype=np.float64) / LATITUDE_STEPS
    samples = bilinear(dem, u_ring[None, :], v_ring[:, None])

    def add_vertex(u, v, height):
        lon = math.tau * (u - 0.5)
        lat = math.pi * (0.5 - v)
        normal = (math.cos(lat) * math.cos(lon), -math.cos(lat) * math.sin(lon), math.sin(lat))
        radius = MODEL_RADIUS_CM * (1 + (height - LOLA_DATUM) / HALF_METRES_PER_RADIUS)
        vertices.append((*[radius * x for x in normal], *normal, u, v))

    stride = LONGITUDE_STEPS + 1
    for row, v in enumerate(v_ring):
        for col, u in enumerate(u_ring):
            add_vertex(u, v, float(samples[row, col]))

    # Unreal's front faces use clockwise winding. Normals still point radially outward.
    for row in range(LATITUDE_STEPS - 2):
        for col in range(LONGITUDE_STEPS):
            a = row * stride + col
            b = a + 1
            c = a + stride
            d = c + 1
            triangles.extend(((a, c, b), (b, c, d)))

    for v, edge_row in ((0.0, 0), (1.0, LATITUDE_STEPS - 2)):
        height = float(dem[0 if v == 0 else -1].mean())
        for col in range(LONGITUDE_STEPS):
            pole = len(vertices)
            add_vertex((col + 0.5) / LONGITUDE_STEPS, v, height)
            a = edge_row * stride + col
            triangles.append((pole, a, a + 1) if v == 0 else (pole, a + 1, a))

    with (output / "lunar_mesh.bin").open("wb") as mesh:
        mesh.write(struct.pack("<II", len(vertices), len(triangles)))
        mesh.write(np.asarray(vertices, dtype="<f4").tobytes())
        mesh.write(np.asarray(triangles, dtype="<i4").tobytes())
    print(f"Mesh: {len(vertices):,} vertices, {len(triangles):,} triangles")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Directory with the two CGI Moon Kit TIFFs")
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with Image.open(args.source / "ldem_16_uint.tif") as height_image:
        if height_image.size != (5760, 2880):
            raise ValueError(f"Unexpected LOLA map size {height_image.size}")
        dem = np.asarray(height_image, dtype=np.float32)
    print(f"LOLA relief: {(dem.min() - LOLA_DATUM) * 0.5:,.1f} to {(dem.max() - LOLA_DATUM) * 0.5:,.1f} m")
    bake_mesh(dem, args.output)
    bake_relief(dem, args.output)
    with Image.open(args.source / "lroc_color_16bit_srgb_8k.tif") as albedo:
        if albedo.size != (WIDTH, HEIGHT):
            raise ValueError(f"Unexpected LROC color map size {albedo.size}")
        color = albedo.convert("RGB")
    color.save(args.output / "T_LunarAlbedo_8K.png", compress_level=3)
    color.resize((WIDTH // 2, HEIGHT // 2), Image.Resampling.LANCZOS).save(
        args.output / "T_McMoonAlbedo_4K.png", compress_level=3)


if __name__ == "__main__":
    main()
