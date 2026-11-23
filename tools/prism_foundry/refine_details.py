"""Micro-refinement & architectural polish pass for Prism Foundry RTX Showcase.

Covers:
  1. Classical rooflines, parapets, corner turrets & central optical orrery on Zones 2, 3 & 1B
     (strictly preserving the black_concrete light-seal & all 4 skylights of Zone 3).
  2. Southeast coastal formal sculpture garden, 3-tier quartz fountain, harbor lighthouse (<= 8 lights),
     refined Zone 6 pavilion, and timber/stone pier with moored boats.
  3. Reinforced 3D cardinal gateway portals, Zone 1 colonnade trims, Zone 5 Gothic twin-tower facade,
     and cantilevered glass/copper sky deck supporting the `sky_overview` waypoint.
  4. Zone 4B natural terraced cliff skirts, cliffside trees, Roman aqueduct stone arches under the rapid flume,
     plus Zone 4D sunken shipwreck keel, coral reefs (waterlogged), seagrass & kelp forests.
  5. Twin transparent glass bubble-column elevators (Y=64 <-> Y=96) hugging the North piers of the central
     clocktower with zero water leakage.
  6. Zone 2A optical target pillars, Zone 2D four-material reflective corner columns & dais, Zone 3C shaft
     observation dais, and Zone 5 cathedral pews, copper candelabras & stepped altar trims.
  7. Global attachment polish pass ensuring zero floating signs, wall torches, or hanging lanterns.
"""

from __future__ import annotations

import math
import random
from typing import Dict, Tuple

from voxel_canvas import CANVAS_MIN_Y, MAIN_FLOOR_Y, MIN_X, MIN_Z, SEA_LEVEL, VoxelCanvas


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _turret(
    c: VoxelCanvas,
    cx: int,
    base_y: int,
    cz: int,
    body_h: int = 5,
    roof_h: int = 4,
    with_rod: bool = True,
) -> None:
    """Build a classical 5x5 stone & oxidized-copper corner turret starting at base_y."""
    # Square stone body (5x5 hollow-ish shell with corner pillars & iron-bar windows)
    for y in range(base_y, base_y + body_h):
        for dx in range(-2, 3):
            for dz in range(-2, 3):
                on_edge = abs(dx) == 2 or abs(dz) == 2
                on_corner = abs(dx) == 2 and abs(dz) == 2
                if on_corner:
                    c.set(cx + dx, y, cz + dz, "polished_deepslate")
                elif on_edge:
                    if y == base_y + body_h - 1:
                        c.set(cx + dx, y, cz + dz, "waxed_cut_copper")
                    elif (dx == 0 or dz == 0) and base_y + 1 <= y <= base_y + body_h - 2:
                        c.set(cx + dx, y, cz + dz, "iron_bars")
                    else:
                        c.set(cx + dx, y, cz + dz, "stone_bricks")
    # Stepped pyramidal oxidized-copper spire with fresh-copper corner ribs
    ry0 = base_y + body_h
    for step in range(roof_h):
        y = ry0 + step
        r = max(0, 2 - step)
        if r == 2:
            for dx in range(-2, 3):
                for dz in range(-2, 3):
                    if abs(dx) == 2 or abs(dz) == 2:
                        blk = "waxed_cut_copper" if (abs(dx) == 2 and abs(dz) == 2) else "waxed_oxidized_copper"
                        c.set(cx + dx, y, cz + dz, blk)
                    else:
                        c.set(cx + dx, y, cz + dz, "waxed_oxidized_copper")
        elif r == 1:
            for dx in range(-1, 2):
                for dz in range(-1, 2):
                    blk = "waxed_cut_copper" if (abs(dx) == 1 and abs(dz) == 1) else "waxed_oxidized_copper"
                    c.set(cx + dx, y, cz + dz, blk)
        else:
            c.set(cx, y, cz, "waxed_copper_block")
    top_y = ry0 + roof_h
    c.set(cx, top_y, cz, "gold_block")
    if with_rod:
        c.set(cx, top_y + 1, cz, "waxed_lightning_rod", facing="up", powered="false")


def _parapet_perimeter(c: VoxelCanvas, x1: int, x2: int, z1: int, z2: int, roof_y: int) -> None:
    """Add classical stone-brick/deepslate crenelated parapets & exterior cornice around a flat roof."""
    for x in range(x1, x2 + 1):
        for z, out_dz, face in ((z1, -1, "south"), (z2, 1, "north")):
            c.set(x, roof_y + 1, z, "polished_deepslate")
            if (x - x1) % 8 == 0 or (x2 - x) == 0:
                c.set(x, roof_y + 2, z, "polished_deepslate")
                c.set(x, roof_y + 3, z, "waxed_cut_copper_slab", type="bottom")
            elif (x - x1) % 2 == 0:
                c.set(x, roof_y + 2, z, "stone_bricks")
                c.set(x, roof_y + 3, z, "stone_brick_slab", type="bottom")
            else:
                c.set(x, roof_y + 2, z, "stone_brick_wall")
            # Subtle exterior cornice lip at roof_y
            if x1 < x < x2 and c.get_short(x, roof_y, z + out_dz) == "air":
                c.set(x, roof_y, z + out_dz, "deepslate_brick_stairs", facing=face, half="top", shape="straight")

    for z in range(z1, z2 + 1):
        for x, out_dx, face in ((x1, -1, "east"), (x2, 1, "west")):
            c.set(x, roof_y + 1, z, "polished_deepslate")
            if (z - z1) % 8 == 0 or (z2 - z) == 0:
                c.set(x, roof_y + 2, z, "polished_deepslate")
                c.set(x, roof_y + 3, z, "waxed_cut_copper_slab", type="bottom")
            elif (z - z1) % 2 == 0:
                c.set(x, roof_y + 2, z, "stone_bricks")
                c.set(x, roof_y + 3, z, "stone_brick_slab", type="bottom")
            else:
                c.set(x, roof_y + 2, z, "stone_brick_wall")
            if z1 < z < z2 and c.get_short(x + out_dx, roof_y, z) == "air":
                c.set(x + out_dx, roof_y, z, "deepslate_brick_stairs", facing=face, half="top", shape="straight")


def _in_zone3_skylight_guard(x: int, z: int, pad: int = 2) -> bool:
    """Return True if (x, z) is within `pad` blocks of any of Zone 3's 4 intentional skylights."""
    skylights = [
        (-53, -49, -51, -47),  # Cornell Box 1
        (-91, -87, -51, -47),  # Cornell Box 3
        (-72, -70, -96, -94),  # 3B Indirect Bounce shaft
        (-88, -87, -82, -81),  # 3C 20-block Deep Slit Shaft
    ]
    for xa, xb, za, zb in skylights:
        if xa - pad <= x <= xb + pad and za - pad <= z <= zb + pad:
            return True
    return False


# ---------------------------------------------------------------------------
# 1. Skyline & Classical Rooflines (Zone 2, Zone 3, Zone 1B)
# ---------------------------------------------------------------------------

def refine_skyline_roofs(c: VoxelCanvas) -> None:
    """Transform flat gray roofs on Zones 2, 3, and 1B into a sculpted RTX landmark skyline."""
    # === Zone 2 Roof (X in [-36..36], Z in [-102..-36], exterior deck Y=84, black_concrete seal at Y=83) ===
    r2 = 84
    _parapet_perimeter(c, -36, 36, -102, -36, r2)

    # Decorative geometric copper/quartz roof inlays on 2C (West) and 2B (East) wings at Y=84
    for x in range(-34, 35):
        for z in range(-100, -37):
            if z <= -60 and abs(x) >= 14:
                if abs(x) in (15, 33) or z in (-99, -62):
                    c.set(x, r2, z, "waxed_oxidized_cut_copper")
                elif (x + z) % 6 == 0 or (x - z) % 6 == 0:
                    c.set(x, r2, z, "polished_deepslate")
                elif abs(abs(x) - 24) <= 1 and abs(z + 80) <= 1:
                    c.set(x, r2, z, "waxed_cut_copper")

    # Four corner turrets framing Zone 2
    for tx, tz in ((-34, -100), (34, -100), (-34, -38), (34, -38)):
        _turret(c, tx, r2 + 1, tz, body_h=5, roof_h=4, with_rod=True)

    # Twin decorative pitched copper-ridge roofs over Zone 2A (Z in [-55..-41], X in [-27..27])
    # Leaves central promenade above X in [-3..3] slightly recessed with a stone-copper gable ridge
    for x in range(-28, 29):
        for step in range(5):
            y = r2 + 1 + step
            zn = -54 + step
            zs = -42 - step
            mat_s = "waxed_oxidized_cut_copper_stairs" if step % 2 == 0 else "waxed_weathered_cut_copper_stairs"
            c.set(x, y, zn, mat_s, facing="south", half="bottom", shape="straight")
            c.set(x, y, zs, mat_s, facing="north", half="bottom", shape="straight")
            # Fill backing underneath step so roof looks thick & crisp from side views
            if step > 0:
                c.fill(x, r2 + 1, zn + 1, x, y - 1, zn + 1, "deepslate_bricks")
                c.fill(x, r2 + 1, zs - 1, x, y - 1, zs - 1, "deepslate_bricks")
        # Fresh copper main ridge along Z=-48 (Y=r2+6)
        c.fill(x, r2 + 5, -49, x, r2 + 5, -47, "waxed_oxidized_copper")
        c.set(x, r2 + 6, -48, "waxed_cut_copper")
        if x % 6 == 0:
            c.set(x, r2 + 7, -48, "waxed_copper_bars")
            c.set(x, r2 + 8, -48, "gold_block")
    # Stone-brick gable end walls at X=-28 and X=28
    for gx in (-28, 28):
        for step in range(5):
            c.fill(gx, r2 + 1 + step, -54 + step, gx, r2 + 1 + step, -42 - step, "stone_bricks")
        c.set(gx, r2 + 6, -48, "chiseled_quartz_block")

    # Small transverse twin copper ridge dormers on West (X=-24) and East (X=24) wings of Zone 2 North
    for wx in (-24, 24):
        for z in range(-94, -66):
            for step in range(3):
                y = r2 + 1 + step
                c.set(wx - 4 + step, y, z, "waxed_oxidized_cut_copper_stairs", facing="east", half="bottom", shape="straight")
                c.set(wx + 4 - step, y, z, "waxed_oxidized_cut_copper_stairs", facing="west", half="bottom", shape="straight")
            c.fill(wx - 1, r2 + 3, z, wx + 1, r2 + 3, z, "waxed_weathered_copper")
            c.set(wx, r2 + 4, z, "waxed_cut_copper")
        for gz in (-94, -66):
            c.fill(wx - 3, r2 + 1, gz, wx + 3, r2 + 2, gz, "stone_bricks")
            c.fill(wx - 1, r2 + 3, gz, wx + 1, r2 + 3, gz, "polished_deepslate")

    # Central Grand Optical Orrery Pavilion over Zone 2D (center (0, 85, -80), radius 11)
    ox, oz = 0, -80
    for dx in range(-11, 12):
        for dz in range(-11, 12):
            d = math.hypot(dx, dz)
            if d <= 11.2:
                if d >= 10.0:
                    c.set(ox + dx, r2 + 1, oz + dz, "polished_deepslate")
                elif d >= 8.2:
                    c.set(ox + dx, r2 + 1, oz + dz, "smooth_quartz")
                    c.set(ox + dx, r2 + 2, oz + dz, "waxed_cut_copper_slab", type="bottom")
                else:
                    c.set(ox + dx, r2 + 1, oz + dz, "polished_deepslate")
                    c.set(ox + dx, r2 + 2, oz + dz, "smooth_quartz" if (dx + dz) % 2 == 0 else "waxed_oxidized_copper")
    # 8 radial quartz & copper columns supporting the elevated astronomical ring (Y=87..94)
    for k in range(8):
        ang = k * math.pi / 4.0
        px = ox + int(round(8.5 * math.cos(ang)))
        pz = oz + int(round(8.5 * math.sin(ang)))
        c.pillar(px, r2 + 3, r2 + 10, pz, "quartz_pillar", "polished_deepslate", "waxed_chiseled_copper")
    # Horizontal entablature ring at Y=95 (r2+11)
    for dx in range(-9, 10):
        for dz in range(-9, 10):
            d = math.hypot(dx, dz)
            if 7.6 <= d <= 9.4:
                blk = "gold_block" if (abs(dx) <= 1 or abs(dz) <= 1) else "waxed_oxidized_copper"
                c.set(ox + dx, r2 + 11, oz + dz, blk)
    # Sculpted 3-Axis Armillary Sphere / Optical Orrery centered at (0, 99, -80), radius 6
    cy = r2 + 15  # Y=99
    rad = 6.0
    for deg in range(0, 360, 4):
        th = math.radians(deg)
        ca, sa = math.cos(th), math.sin(th)
        # Ring 1: Vertical North-South in Gold
        c.set(ox, cy + int(round(rad * sa)), oz + int(round(rad * ca)), "gold_block")
        # Ring 2: Tilted 45 deg in Fresh Copper
        c.set(
            ox + int(round(rad * ca * 0.707)),
            cy + int(round(rad * sa)),
            oz + int(round(rad * ca * 0.707)),
            "waxed_copper_block",
        )
        # Ring 3: Opposite tilt 45 deg in Oxidized Copper
        c.set(
            ox + int(round(rad * ca * 0.707)),
            cy + int(round(rad * sa)),
            oz - int(round(rad * ca * 0.707)),
            "waxed_oxidized_copper",
        )
    # Central refractive prism core + polar axis spire (1 sea lantern only)
    c.fill(ox, r2 + 3, oz, ox, cy + 7, oz, "iron_chain", axis="y")
    c.set(ox, cy, oz, "sea_lantern")
    for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
        c.set(ox + dx, cy + dy, oz + dz, "glass")
    c.set(ox, cy + 7, oz, "gold_block")
    c.set(ox, cy + 8, oz, "waxed_lightning_rod", facing="up", powered="false")

    # === Zone 3 Roof (X in [-100..-40], Z in [-100..-40], exterior deck Y=80, black_concrete seal at Y=79) ===
    r3 = 80
    _parapet_perimeter(c, -100, -40, -100, -40, r3)

    # Geometric deck tiling (strictly skipping the 4 skylight wells!)
    for x in range(-98, -41):
        for z in range(-98, -41):
            if _in_zone3_skylight_guard(x, z, pad=1):
                continue
            if (x % 8 == 0 or z % 8 == 0) and c.get_short(x, r3, z) == "deepslate_tiles":
                c.set(x, r3, z, "polished_deepslate")
            elif (x + z) % 10 == 0 and c.get_short(x, r3, z) == "deepslate_tiles":
                c.set(x, r3, z, "waxed_oxidized_cut_copper")

    # Four staggered corner turrets on Zone 3 (slightly shorter than Zone 2 for terraced skyline!)
    for tx, tz in ((-98, -98), (-42, -98), (-98, -42), (-42, -42)):
        _turret(c, tx, r3 + 1, tz, body_h=4, roof_h=3, with_rod=True)

    # Sculpted optical skylight curbing around all 4 Zone 3 skylights (never obstructing vertical shafts!)
    skylight_boxes = [
        (-53, -49, -51, -47),  # Cornell Box 1
        (-91, -87, -51, -47),  # Cornell Box 3
        (-72, -70, -96, -94),  # 3B Orange Bounce
        (-88, -87, -82, -81),  # 3C 20-block Deep Slit Shaft
    ]
    for xa, xb, za, zb in skylight_boxes:
        for x in range(xa - 1, xb + 2):
            for z in (za - 1, zb + 1):
                c.set(x, r3 + 1, z, "polished_deepslate")
                c.set(x, r3 + 2, z, "waxed_cut_copper_slab", type="bottom")
        for z in range(za, zb + 1):
            for x in (xa - 1, xb + 1):
                c.set(x, r3 + 1, z, "polished_deepslate")
                c.set(x, r3 + 2, z, "waxed_cut_copper_slab", type="bottom")
        # Guarantee skylight core remains 100% open air down through roof
        c.fill(xa, r3, za, xb, r3 + 10, zb, "air")

    # Sculpted open-frame Gilded Sundial Frame rising around 3C Deep Slit Shaft (clear of [-88..-87, -82..-81])
    for px in (-90, -85):
        for pz in (-84, -79):
            c.pillar(px, r3 + 1, r3 + 6, pz, "polished_deepslate", "chiseled_deepslate", "gold_block")
    for x in range(-90, -84):
        c.set(x, r3 + 7, -84, "waxed_cut_copper")
        c.set(x, r3 + 7, -79, "waxed_cut_copper")
    for z in range(-84, -78):
        c.set(-90, r3 + 7, z, "waxed_cut_copper")
        c.set(-85, r3 + 7, z, "waxed_cut_copper")

    # Pitched dual-copper observatory gallery ridge over 3B mirror corridors (X in [-64..-50], Z in [-94..-66])
    # Safely clear of all 4 skylights!
    for z in range(-94, -65):
        for step in range(4):
            y = r3 + 1 + step
            c.set(-64 + step, y, z, "waxed_oxidized_cut_copper_stairs", facing="east", half="bottom", shape="straight")
            c.set(-50 - step, y, z, "waxed_oxidized_cut_copper_stairs", facing="west", half="bottom", shape="straight")
            if step > 0:
                c.fill(-63 + step, r3 + 1, z, -63 + step, y - 1, z, "deepslate_bricks")
                c.fill(-51 - step, r3 + 1, z, -51 - step, y - 1, z, "deepslate_bricks")
        c.fill(-60, r3 + 4, z, -54, r3 + 4, z, "waxed_weathered_copper")
        c.set(-57, r3 + 5, z, "waxed_cut_copper")
    for gz in (-94, -66):
        for step in range(4):
            c.fill(-64 + step, r3 + 1 + step, gz, -50 - step, r3 + 1 + step, gz, "stone_bricks")
        c.set(-57, r3 + 5, gz, "gold_block")
        c.set(-57, r3 + 6, gz, "waxed_lightning_rod", facing="up", powered="false")

    # === Zone 1B Indoor PBR Gallery Roof Polish (X in [32..97], Z in [5..32], ceiling at Y=76) ===
    r1b = MAIN_FLOOR_Y + 12  # 76
    _parapet_perimeter(c, 32, 97, 5, 32, r1b)
    # Decorative raised copper architectural ribs across the 1B roof (without touching recessed sea_lanterns at Y=76)
    for rx in range(34, 96, 8):
        for z in range(7, 31):
            c.set(rx, r1b + 1, z, "waxed_cut_copper_slab", type="bottom")
    for tx, tz in ((34, 7), (95, 7), (34, 30), (95, 30)):
        c.fill(tx - 1, r1b + 1, tz - 1, tx + 1, r1b + 3, tz + 1, "polished_deepslate")
        c.set(tx, r1b + 4, tz, "waxed_copper_block")
        c.set(tx, r1b + 5, tz, "waxed_lightning_rod", facing="up", powered="false")


# ---------------------------------------------------------------------------
# 2. Southeast Coastal Garden, Quartz Fountain, Lighthouse & Timber Pier
# ---------------------------------------------------------------------------

def refine_southeast_garden_lighthouse_pier(c: VoxelCanvas) -> None:
    """Replace the plain SE grass terrace (X=36..102, Z=38..102) with a formal garden, fountain, lighthouse & pier."""
    y0 = MAIN_FLOOR_Y  # 64
    sea = SEA_LEVEL    # 62

    # 1. Clear old placeholder trees on SE terrace while preserving Zone 6 Pavilion (X=41..57, Z=39..53)
    for x in range(36, 103):
        for z in range(38, 103):
            if 41 <= x <= 57 and 39 <= z <= 53:
                continue
            c.fill(x, y0 + 1, z, x, y0 + 18, z, "air")
            c.set(x, y0, z, "grass_block")

    # 2. Polish Zone 6 Entry Pavilion (X=42..56, Z=40..52, roof Y=71) with hipped oxidized-copper roof & arches
    py = y0 + 7  # 71
    for step in range(5):
        xa, xb = 42 + step, 56 - step
        za, zb = 40 + step, 52 - step
        if xa > xb or za > zb:
            break
        y = py + step
        for x in range(xa, xb + 1):
            c.set(x, y, za, "waxed_oxidized_cut_copper_stairs", facing="south", half="bottom", shape="straight")
            c.set(x, y, zb, "waxed_oxidized_cut_copper_stairs", facing="north", half="bottom", shape="straight")
        for z in range(za + 1, zb):
            c.set(xa, y, z, "waxed_oxidized_cut_copper_stairs", facing="east", half="bottom", shape="straight")
            c.set(xb, y, z, "waxed_oxidized_cut_copper_stairs", facing="west", half="bottom", shape="straight")
        for cx in (xa, xb):
            for cz in (za, zb):
                c.set(cx, y, cz, "waxed_cut_copper")
        if step < 4:
            c.fill(xa + 1, y, za + 1, xb - 1, y, zb - 1, "waxed_oxidized_copper")
    c.fill(46, py + 5, 46, 52, py + 5, 46, "waxed_cut_copper")
    c.set(49, py + 6, 46, "gold_block")
    c.set(49, py + 7, 46, "waxed_lightning_rod", facing="up", powered="false")
    # Arch chamfer trim on pavilion north & west portals + relocate previously floating sign onto solid pillar
    c.set(46, y0 + 5, 40, "polished_deepslate_stairs", facing="west", half="top", shape="straight")
    c.set(52, y0 + 5, 40, "polished_deepslate_stairs", facing="east", half="top", shape="straight")
    c.set(42, y0 + 5, 44, "polished_deepslate_stairs", facing="north", half="top", shape="straight")
    c.set(42, y0 + 5, 48, "polished_deepslate_stairs", facing="south", half="top", shape="straight")
    # Remove floating sign at (49, 66, 39) and mount cleanly on solid north-west pavilion pillar (45, 66, 39)
    c.set(49, y0 + 2, 39, "air")
    c.place_sign(45, y0 + 2, 39, ["[区6 · 极端复合地下城]", "6A 铜铁试炼锻造厅", "6B 冰火熔岩水洞", "高密多光源+金属压测"], facing="north", color="yellow")

    # 3. Perimeter coastal promenade & seawall balustrade
    c.fill(36, y0, 38, 102, y0, 40, "stone_bricks")
    c.fill(36, y0, 38, 38, y0, 102, "stone_bricks")
    c.fill(100, y0, 38, 102, y0, 102, "polished_andesite")
    c.fill(36, y0, 100, 102, y0, 102, "polished_andesite")
    for x in range(39, 100):
        c.set(x, y0, 39, "polished_andesite")
        c.set(x, y0, 101, "stone_bricks")
    for z in range(41, 100):
        c.set(37, y0, z, "polished_andesite")
        c.set(101, y0, z, "stone_bricks")
    # Coastal balustrade along East (X=102) and South (Z=102), leaving opening for Pier (X=67..71, Z=102) & Lighthouse plaza
    for z in range(40, 84):
        c.set(102, y0 + 1, z, "polished_deepslate" if z % 8 == 0 else "stone_brick_wall")
        if z % 8 == 0:
            c.set(102, y0 + 2, z, "waxed_cut_copper_slab", type="bottom")
    for x in range(38, 84):
        if 66 <= x <= 72:
            continue
        c.set(x, y0 + 1, 102, "polished_deepslate" if x % 8 == 0 else "stone_brick_wall")
        if x % 8 == 0:
            c.set(x, y0 + 2, 102, "waxed_cut_copper_slab", type="bottom")

    # 4. Symmetrical Formal Garden Paths & Central 3-Tier Quartz Fountain (center (69, 64, 74))
    fcx, fcz = 69, 74
    # Cross axes (N-S connects to South Pier at Z=102!)
    for z in range(41, 103):
        for dx in range(-2, 3):
            blk = "smooth_quartz" if dx == 0 else ("mossy_stone_bricks" if (z + dx) % 5 == 0 else "stone_bricks")
            c.set(fcx + dx, y0, z, blk)
    for x in range(39, 102):
        for dz in range(-2, 3):
            blk = "smooth_quartz" if dz == 0 else ("mossy_stone_bricks" if (x + dz) % 5 == 0 else "stone_bricks")
            c.set(x, y0, fcz + dz, blk)

    # Circular walkway ring (r in [8.5, 13.0]) & Central Quartz Fountain Basin (r <= 8.5)
    for dx in range(-13, 14):
        for dz in range(-13, 14):
            d = math.hypot(dx, dz)
            wx, wz = fcx + dx, fcz + dz
            if 8.2 < d <= 13.0:
                if abs(d - 12.6) < 0.55 or abs(d - 8.7) < 0.55:
                    c.set(wx, y0, wz, "polished_deepslate")
                else:
                    c.set(wx, y0, wz, "smooth_quartz" if (dx + dz) % 2 == 0 else "stone_bricks")
            elif d <= 8.2:
                if d >= 7.1:
                    # Raised quartz fountain rim
                    c.set(wx, y0, wz, "smooth_quartz")
                    c.set(wx, y0 + 1, wz, "quartz_slab", type="bottom")
                else:
                    # Shallow sunken basin (Y=63 prismarine/quartz floor, Y=64 still water -> 4 blocks above underground ceiling Y=58!)
                    c.set(wx, y0 - 1, wz, "smooth_quartz" if int(d) % 2 == 0 else "prismarine_bricks")
                    c.set(wx, y0, wz, "water", level="0")
    # Four cardinal quartz plinths on fountain rim
    for dx, dz in ((8, 0), (-8, 0), (0, 8), (0, -8)):
        c.set(fcx + dx, y0 + 1, fcz + dz, "chiseled_quartz_block")
        c.set(fcx + dx, y0 + 2, fcz + dz, "quartz_slab", type="bottom")
    # 3-Tier Central Quartz Lotus Pedestal & Fountain Cascade (only 1 sea_lantern at apex!)
    c.fill(fcx - 1, y0, fcz - 1, fcx + 1, y0 + 1, fcz + 1, "chiseled_quartz_block")
    c.fill(fcx, y0 + 1, fcz, fcx, y0 + 5, fcz, "quartz_pillar")
    # Mid dish (Y=67)
    for dx in range(-1, 2):
        for dz in range(-1, 2):
            if dx != 0 or dz != 0:
                c.set(fcx + dx, y0 + 3, fcz + dz, "quartz_slab", type="top")
    c.set(fcx, y0 + 5, fcz, "gold_block")
    c.set(fcx, y0 + 6, fcz, "sea_lantern")
    # 4 crisp falling water streams safely dropping straight inside the 13-block-wide basin!
    for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        c.fill(fcx + dx, y0 + 1, fcz + dz, fcx + dx, y0 + 5, fcz + dz, "water", level="8")
        c.set(fcx + dx, y0 + 5, fcz + dz, "water", level="1")
        # Ensure base block underneath stream is open water inside basin
        c.set(fcx + dx, y0, fcz + dz, "water", level="0")

    # 5. Formal Geometric Parterre Hedge Beds, Flowers & Sculptures
    flowers = [
        "red_tulip", "orange_tulip", "white_tulip", "pink_tulip",
        "cornflower", "blue_orchid", "allium", "oxeye_daisy",
    ]
    parterres = [
        # (x1, z1, x2, z2, statue_block)
        (74, 43, 96, 68, "waxed_copper_golem_statue"),           # NE formal bed
        (42, 79, 64, 97, "waxed_oxidized_copper_golem_statue"),  # SW formal bed
        (42, 55, 64, 69, "waxed_exposed_copper_golem_statue"),   # NW formal bed (south of pavilion)
    ]
    rng = random.Random(20261122)
    for px1, pz1, px2, pz2, statue in parterres:
        mcx, mcz = (px1 + px2) // 2, (pz1 + pz2) // 2
        for x in range(px1, px2 + 1):
            for z in range(pz1, pz2 + 1):
                if math.hypot(x - fcx, z - fcz) <= 13.5:
                    continue
                on_border = x in (px1, px2) or z in (pz1, pz2)
                if on_border:
                    # Leave clean entry gaps at midpoints of hedges
                    if abs(x - mcx) <= 1 or abs(z - mcz) <= 1:
                        c.set(x, y0, z, "stone_bricks")
                    else:
                        leaf = "flowering_azalea_leaves" if (x + z) % 3 == 0 else "azalea_leaves"
                        c.set(x, y0 + 1, z, leaf, persistent="true", distance="1")
                else:
                    if (x + z) % 4 == 0:
                        c.set(x, y0, z, "moss_block")
                    if abs(x - mcx) <= 2 and abs(z - mcz) <= 2:
                        c.set(x, y0, z, "smooth_quartz")
                    elif (x + z) % 2 == 0 and rng.random() < 0.42:
                        fblk = flowers[(abs(x - mcx) + abs(z - mcz)) % len(flowers)]
                        c.set(x, y0 + 1, z, fblk)
        # Central sculpture plinth inside each parterre
        c.fill(mcx - 1, y0 + 1, mcz - 1, mcx + 1, y0 + 1, mcz + 1, "quartz_slab", type="bottom")
        c.set(mcx, y0 + 1, mcz, "chiseled_quartz_block")
        c.set(mcx, y0 + 2, mcz, statue)

    # Ornamental diverse trees framing garden corners (clear of pavilion, paths, fountain & lighthouse)
    garden_trees = [
        (43, 94, "cherry_log", "cherry_leaves", 6, 2.9, 201),
        (95, 45, "cherry_log", "cherry_leaves", 6, 3.0, 202),
        (61, 95, "birch_log", "birch_leaves", 6, 2.5, 203),
        (78, 45, "birch_log", "birch_leaves", 6, 2.5, 204),
        (43, 62, "oak_log", "oak_leaves", 6, 2.7, 205),
    ]
    for tx, tz, log, leaves, th, cr, sd in garden_trees:
        c.set(tx, y0, tz, "grass_block")
        c.tree(tx, y0 + 1, tz, log, leaves, height=th, canopy_r=cr, seed=sd)

    # 6. Towering SE Harbor Lighthouse (center (93, 64, 93), height Y=64..108, strictly 4 sea_lanterns!)
    lcx, lcz = 93, 93
    # Paved octagonal lighthouse plaza
    for dx in range(-9, 10):
        for dz in range(-9, 10):
            wx, wz = lcx + dx, lcz + dz
            if wx > 102 or wz > 102:
                continue
            d = math.hypot(dx, dz)
            if d <= 9.5:
                c.set(wx, y0, wz, "polished_deepslate" if int(d) % 2 == 0 else "stone_bricks")

    # Octagonal stone base (Y=64..71, r=6.2 -> 5.4)
    for y in range(y0 + 1, y0 + 8):
        r = 6.0 - (y - y0 - 1) * 0.14
        for dx in range(-6, 7):
            for dz in range(-6, 7):
                d = math.hypot(dx, dz)
                if r - 1.35 <= d <= r:
                    blk = "polished_deepslate" if (y in (y0 + 1, y0 + 7) or abs(dx) == abs(dz)) else "stone_bricks"
                    c.set(lcx + dx, y, lcz + dz, blk)
                elif d < r - 1.35:
                    c.set(lcx + dx, y, lcz + dz, "air")
    # Ground floor inside lighthouse + NW arched doorway facing garden
    for dx in range(-5, 6):
        for dz in range(-5, 6):
            if math.hypot(dx, dz) <= 5.0:
                c.set(lcx + dx, y0, lcz + dz, "polished_deepslate")
    # Open doorway along NW diagonal (lcx-4..lcx-3, lcz-4..lcz-3, Y=65..68)
    for dy in range(1, 5):
        for dd in (3, 4, 5):
            c.set(lcx - dd, y0 + dy, lcz - dd, "air")
            c.set(lcx - dd + 1, y0 + dy, lcz - dd, "air")
            c.set(lcx - dd, y0 + dy, lcz - dd + 1, "air")

    # Tapered maritime red-and-white striped quartz & copper tower shaft (Y=72..94)
    for y in range(72, 95):
        t = (y - 72) / 22.0
        r = 4.9 - 1.35 * t
        band = ((y - 72) // 4) % 2  # alternating white smooth_quartz & bold red_concrete maritime stripes
        for dx in range(-5, 6):
            for dz in range(-5, 6):
                d = math.hypot(dx, dz)
                if r - 1.25 <= d <= r:
                    if y in (72, 83, 94):
                        blk = "waxed_cut_copper"
                    elif (dx == 0 or dz == 0) and (y % 6 in (2, 3)):
                        blk = "glass"
                    else:
                        blk = "smooth_quartz" if band == 0 else "red_concrete"
                    c.set(lcx + dx, y, lcz + dz, blk)
                elif d < r - 1.25:
                    c.set(lcx + dx, y, lcz + dz, "air")

    # Central service column + climbable ladder inside lighthouse from Y=65 up to balcony Y=96
    c.fill(lcx, y0 + 1, lcz, lcx, 96, lcz, "polished_deepslate")
    for y in range(y0 + 1, 97):
        c.set(lcx - 1, y, lcz, "ladder", facing="west")

    # Cantilevered Observation Gallery Deck at Y=95..96 (radius 5.2)
    for dx in range(-5, 6):
        for dz in range(-5, 6):
            d = math.hypot(dx, dz)
            if d <= 5.2:
                c.set(lcx + dx, 95, lcz + dz, "polished_deepslate" if d >= 4.0 else "waxed_cut_copper")
                if 4.3 <= d <= 5.2:
                    c.set(lcx + dx, 96, lcz + dz, "waxed_copper_bars")
    # Open ladder hatch at (lcx-1, 95, lcz)
    c.set(lcx - 1, 95, lcz, "ladder", facing="west")

    # Rotating Fresnel-Lens Glass Lantern Room (Y=96..101, radius 3.3, ONLY 4 sea_lanterns!)
    for y in range(96, 102):
        for dx in range(-3, 4):
            for dz in range(-3, 4):
                d = math.hypot(dx, dz)
                if 2.4 <= d <= 3.4:
                    if abs(dx) == 2 and abs(dz) == 2:
                        c.set(lcx + dx, y, lcz + dz, "waxed_copper_bars")
                    elif y == 96:
                        c.set(lcx + dx, y, lcz + dz, "waxed_cut_copper")
                    else:
                        c.set(lcx + dx, y, lcz + dz, "glass")
                elif d < 2.4 and y >= 97:
                    c.set(lcx + dx, y, lcz + dz, "air")
    # Helical rotating reflector optics core (4 sea_lanterns + 4 mirrored gold reflector wings -> rotating Fresnel silhouette!)
    c.fill(lcx, 96, lcz, lcx, 101, lcz, "waxed_copper_block")
    rot_dirs = ((1, 0), (0, 1), (-1, 0), (0, -1))
    for i, (rx, rz) in enumerate(rot_dirs):
        ly = 97 + i
        c.set(lcx + rx, ly, lcz + rz, "sea_lantern")
        c.set(lcx - rx, ly, lcz - rz, "gold_block")
        # Copper Fresnel prism struts
        c.set(lcx + rz, ly, lcz - rx, "waxed_copper_grate")

    # Conical Oxidized-Copper Roof & Lightning Spire (Y=102..108)
    for dy, rad in ((0, 3.6), (1, 2.8), (2, 2.0), (3, 1.2), (4, 0.4)):
        y = 102 + dy
        ri = int(math.ceil(rad))
        for dx in range(-ri, ri + 1):
            for dz in range(-ri, ri + 1):
                if math.hypot(dx, dz) <= rad:
                    blk = "waxed_cut_copper" if (dy in (0, 3) and (dx == 0 or dz == 0)) else "waxed_oxidized_copper"
                    c.set(lcx + dx, y, lcz + dz, blk)
    c.set(lcx, 107, lcz, "gold_block")
    c.set(lcx, 108, lcz, "waxed_lightning_rod", facing="up", powered="false")

    # 7. South Coastal Timber & Stone Pier + L-Dock + Moored Boats (X=67..71, Z=103..118)
    # Main pier deck at Y=64 extending into warm ocean (Z=103..118)
    for z in range(103, 119):
        for x in range(67, 72):
            if x in (67, 71):
                c.set(x, y0, z, "stripped_spruce_log", axis="z")
            else:
                c.set(x, y0, z, "spruce_planks" if (x + z) % 2 == 0 else "oak_planks")
        # Low timber fence railings (leave east gap at Z=110..113 for lower L-dock stairs)
        c.set(67, y0 + 1, z, "spruce_fence")
        if not (110 <= z <= 113):
            c.set(71, y0 + 1, z, "spruce_fence")
        # Stone & timber pilings reaching all the way to the sandy ocean floor (Y=35..63)
        if z in (104, 108, 112, 116, 118):
            for px in (67, 71):
                c.fill(px, 35, z, px, 61, z, "mossy_stone_bricks")
                c.fill(px, 62, z, px, 64, z, "stripped_spruce_log", axis="y")
                c.set(px, 65, z, "stripped_spruce_log", axis="y")
    # Pier entrance & end lantern posts (4 waxed_copper_lanterns total)
    for pz in (104, 118):
        for px in (67, 71):
            c.set(px, y0 + 2, pz, "spruce_fence")
            c.set(px, y0 + 3, pz, "waxed_copper_lantern", hanging="false")
    # Pier end timber cap
    c.fill(67, y0, 118, 71, y0, 118, "stripped_spruce_log", axis="x")

    # Lower L-shaped mooring pontoon branching East (X=72..83, Z=110..113, deck at Y=63 right above sea Y=62!)
    c.fill(72, y0, 110, 72, y0, 113, "spruce_stairs", facing="west", half="bottom", shape="straight")
    for x in range(73, 84):
        for z in range(110, 114):
            c.set(x, y0 - 1, z, "oak_planks" if (x + z) % 2 == 0 else "spruce_planks")
        if x in (74, 78, 82):
            for pz in (110, 113):
                c.fill(x, 35, pz, x, 62, pz, "stripped_spruce_log", axis="y")
                c.set(x, 63, pz, "stripped_spruce_log", axis="y")
                c.set(x, 64, pz, "spruce_fence")
    # Cargo crates & barrels stacked naturally on the dock
    c.place_chest(75, y0, 110, facing="south", block="chest")
    c.set(76, y0, 110, "barrel", facing="up", open="false")
    c.set(77, y0, 110, "barrel", facing="south", open="false")
    c.set(76, y0 + 1, 110, "barrel", facing="up", open="false")
    c.place_chest(82, y0, 112, facing="west", block="waxed_copper_chest")

    # Moored boats floating peacefully beside the pier & L-dock
    c.add_boat(76.5, sea + 0.6, 107.5, yaw=15.0, boat_id="oak_boat")
    c.add_boat(80.5, sea + 0.6, 116.0, yaw=165.0, boat_id="spruce_boat")
    c.add_boat(64.0, sea + 0.6, 113.5, yaw=95.0, boat_id="cherry_boat")


# ---------------------------------------------------------------------------
# 3. Reinforced Architectural Facades & Cantilevered Sky Overview Platform
# ---------------------------------------------------------------------------

def refine_facades_and_sky_platform(c: VoxelCanvas) -> None:
    """Upgrade plaza portals, Zone 1 facade, Zone 5 Gothic facade, and `sky_overview` cantilevered deck."""
    y0 = MAIN_FLOOR_Y  # 64

    # === 1. Central Plaza 4 Cardinal Gateway Portals (3-block deep triumphal arches + directional inlays) ===
    # North Gate (Z=-26, X=-5..5) -> Zone 2 Dark Hall
    for dz in (-27, -25):
        c.arch_x(-5, 5, y0 + 1, 7, dz, "deepslate_bricks", carve_air=False)
    for sx in (-6, 6):
        c.pillar(sx, y0 + 1, y0 + 10, -26, "polished_deepslate", "chiseled_deepslate", "waxed_cut_copper")
        c.set(sx, y0 + 11, -26, "waxed_copper_bars")
    c.set(-4, y0 + 7, -26, "polished_deepslate_stairs", facing="west", half="top", shape="straight")
    c.set(4, y0 + 7, -26, "polished_deepslate_stairs", facing="east", half="top", shape="straight")
    for z in range(-25, -14):
        c.set(-2, y0, z, "waxed_cut_copper")
        c.set(2, y0, z, "waxed_cut_copper")

    # South Gate (Z=26, X=-5..5) -> Zone 5 Cathedral
    for dz in (25, 27):
        c.arch_x(-5, 5, y0 + 1, 8, dz, "smooth_quartz", carve_air=False)
    for sx in (-6, 6):
        c.pillar(sx, y0 + 1, y0 + 11, 26, "quartz_pillar", "polished_deepslate", "gold_block")
        c.set(sx, y0 + 12, 26, "waxed_cut_copper_slab", type="bottom")
    c.set(-4, y0 + 8, 26, "quartz_stairs", facing="west", half="top", shape="straight")
    c.set(4, y0 + 8, 26, "quartz_stairs", facing="east", half="top", shape="straight")

    # East Gate (X=26, Z=-5..5) -> Zone 1 PBR Museum
    for dx in (25, 27):
        c.arch_z(-5, 5, y0 + 1, 8, dx, "waxed_oxidized_cut_copper", carve_air=False)
    for sz in (-6, 6):
        c.pillar(26, y0 + 1, y0 + 11, sz, "waxed_cut_copper", "polished_deepslate", "gold_block")
    c.set(26, y0 + 8, -4, "waxed_cut_copper_stairs", facing="north", half="top", shape="straight")
    c.set(26, y0 + 8, 4, "waxed_cut_copper_stairs", facing="south", half="top", shape="straight")
    for x in range(14, 26):
        c.set(x, y0, -2, "waxed_copper_block")
        c.set(x, y0, 2, "waxed_copper_block")

    # West Gate (X=-26, Z=-5..5) -> Zone 4 Water Optics Bay
    for dx in (-27, -25):
        c.arch_z(-5, 5, y0 + 1, 8, dx, "dark_prismarine", carve_air=False)
    for sz in (-6, 6):
        c.pillar(-26, y0 + 1, y0 + 11, sz, "prismarine_bricks", "polished_deepslate", "waxed_oxidized_copper")
    c.set(-26, y0 + 8, -4, "prismarine_brick_stairs", facing="north", half="top", shape="straight")
    c.set(-26, y0 + 8, 4, "prismarine_brick_stairs", facing="south", half="top", shape="straight")
    for x in range(-25, -14):
        c.set(x, y0, -2, "prismarine_bricks")
        c.set(x, y0, 2, "prismarine_bricks")

    # === 2. Zone 1 East Spine Colonnade Chamfer Trims & 1B Wall Pilasters ===
    for x in range(32, 96, 8):
        # Inverted quartz stair brackets framing every central spine arch
        c.set(x, y0 + 7, -4, "quartz_stairs", facing="north", half="top", shape="straight")
        c.set(x, y0 + 7, 4, "quartz_stairs", facing="south", half="top", shape="straight")
        c.set(x, y0 + 1, -6, "polished_deepslate_stairs", facing="south", half="bottom", shape="straight")
    # Decorative vertical pilasters on Zone 1B exterior south wall (Z=33) & west wall (X=31)
    for x in range(34, 96, 6):
        c.pillar(x, y0 + 1, y0 + 12, 33, "stone_bricks", "polished_deepslate", "waxed_cut_copper")
    for z in range(8, 32, 6):
        c.pillar(31, y0 + 1, y0 + 12, z, "stone_bricks", "polished_deepslate", "waxed_cut_copper")

    # === 3. Zone 5 Cathedral Grand Gothic North Portal & Twin Corner Belfry Spires ===
    # Protruding 2-block Gothic stepped entrance surround on North facade (Z=41..42, X=-8..8)
    for sx in (-8, -7, 7, 8):
        c.pillar(sx, y0 + 1, y0 + 16, 42, "quartz_pillar", "polished_deepslate", "waxed_chiseled_copper")
        c.set(sx, y0 + 17, 42, "waxed_oxidized_copper")
        c.set(sx, y0 + 18, 42, "waxed_cut_copper_slab", type="bottom")
    c.arch_x(-7, 7, y0 + 1, 9, 42, "smooth_quartz", carve_air=False)
    c.arch_x(-6, 6, y0 + 1, 9, 41, "quartz_pillar", carve_air=False)
    # Inverted stair arch fillets
    c.set(-5, y0 + 9, 42, "quartz_stairs", facing="west", half="top", shape="straight")
    c.set(5, y0 + 9, 42, "quartz_stairs", facing="east", half="top", shape="straight")
    c.set(-5, y0 + 9, 41, "quartz_stairs", facing="west", half="top", shape="straight")
    c.set(5, y0 + 9, 41, "quartz_stairs", facing="east", half="top", shape="straight")
    # Cathedral north parapet cornice & Twin Gothic corner towers at (-25, 44) and (25, 44)
    for x in range(-26, 27):
        if abs(x) > 8:
            c.set(x, y0 + 20, 43, "stone_brick_wall" if x % 2 == 0 else "polished_deepslate")
    for tx in (-25, 25):
        _turret(c, tx, y0 + 20, 44, body_h=6, roof_h=4, with_rod=True)
        _turret(c, tx, y0 + 20, 98, body_h=5, roof_h=4, with_rod=True)
    # Sloped stone caps on exterior flying buttresses (X=±27..28)
    for side_x, face in ((-26, "east"), (26, "west")):
        out_x = side_x + (1 if side_x > 0 else -1)
        out_x2 = out_x + (1 if side_x > 0 else -1)
        for wz in range(48, 95, 6):
            c.set(out_x2, y0 + 17, wz - 3, "deepslate_brick_stairs", facing=face, half="bottom", shape="straight")
            c.set(out_x, y0 + 18, wz - 3, "deepslate_brick_stairs", facing=face, half="bottom", shape="straight")

    # === 4. Cantilevered SE Sky Overview Deck at Y=96 (supports `sky_overview` at (23.5, 97.0, 23.5)!) ===
    ry = 96
    # Connect existing SE ring arc (dist 22..26 around (17,17)) seamlessly out to a diamond lookout at (23, 96, 23)
    for x in range(15, 28):
        for z in range(15, 28):
            # Diagonal diamond extending along x+z from 34..50 and |x-z| <= 4.5
            s = x + z
            diff = abs(x - z)
            if 34 <= s <= 50 and diff <= 4:
                is_edge = (s == 50) or (diff == 4)
                if 22 <= x <= 24 and 22 <= z <= 24:
                    c.set(x, ry, z, "waxed_cut_copper")
                elif is_edge:
                    c.set(x, ry, z, "waxed_cut_copper")
                else:
                    c.set(x, ry, z, "glass")
                # Clear any old outer ring glass_pane obstructing entry onto the cantilevered deck
                c.set(x, ry + 1, z, "air")
                c.set(x, ry + 2, z, "air")
                if is_edge and s >= 38:
                    c.set(x, ry + 1, z, "glass_pane")
    # Decorative copper under-strut framing beneath the cantilevered deck
    for step in range(4):
        c.set(19 + step, ry - 1 - (3 - step) // 2, 19 + step, "waxed_copper_bars")
    c.set(23, ry, 23, "gold_block")  # Center hero marker directly beneath `sky_overview`!


# ---------------------------------------------------------------------------
# 4. Zone 4 Water Bay Naturalization & Underwater Ecosystem
# ---------------------------------------------------------------------------

def refine_zone4_water_nature(c: VoxelCanvas) -> None:
    """Naturalize Zone 4B waterfall cliff & flume arches, and enrich Zone 4D seabed with reef, kelp & shipwreck."""
    sea = SEA_LEVEL  # 62

    # === 4B-1: Natural Terraced Coastal Cliff Skirts around Waterfall Tower (X in [-102..-82], Z in [-28..-10]) ===
    # Wrap north, west, south, and east-flanking faces with organic sloping rock from seabed Y=37 up to Y=78
    for x in range(-108, -76):
        for z in range(-34, -4):
            # Skip waterfall plunge stream (X=-81..-77, Z=-22..-16) & glass rapid flume (X=-84..-62, Z=-9..-5)
            if -82 <= x <= -76 and -22 <= z <= -16:
                continue
            if -85 <= x <= -61 and -9 <= z <= -5:
                continue
            # Distance outside original tower footprint [-102..-82, -28..-10]
            dx = max(-102 - x, 0, x - (-82))
            dz = max(-28 - z, 0, z - (-10))
            dist_out = math.hypot(dx, dz)
            if dist_out <= 0.01 or dist_out > 6.5:
                continue
            # Organic height profile tapering smoothly into the ocean
            noise = 2.6 * math.sin(x * 0.43 + z * 0.31) + 1.8 * math.cos(x * 0.27 - z * 0.52)
            cliff_top = int(round(81.0 - dist_out * 6.8 + noise))
            if cliff_top < 38:
                continue
            cliff_top = min(79, cliff_top)
            for y in range(37, cliff_top + 1):
                h_hash = (x * 31 + y * 17 + z * 13) & 15
                if y < sea - 3:
                    blk = "deepslate" if h_hash < 9 else "cobbled_deepslate"
                elif y <= sea + 2:
                    blk = "mossy_cobblestone" if h_hash < 6 else ("mossy_stone_bricks" if h_hash < 10 else "stone")
                elif y == cliff_top and cliff_top >= 68:
                    blk = "moss_block" if h_hash < 6 else "grass_block"
                else:
                    blk = ("stone", "andesite", "cobblestone", "tuff_bricks", "mossy_stone_bricks")[h_hash % 5]
                c.set(x, y, z, blk)

    # Natural mossy rock wing buttresses framing the waterfall cascade (Z=-23 and Z=-15, X=-82..-78)
    for wz in (-23, -15):
        for wx, h_top in ((-82, 79), (-81, 77), (-80, 74), (-79, 71), (-78, 66)):
            for y in range(44, h_top + 1):
                blk = "mossy_cobblestone" if (wx + y) % 3 == 0 else ("stone" if y % 2 == 0 else "andesite")
                c.set(wx, y, wz, blk)

    # Organic cliff-top meadow & trees around the upper reservoir (Y=80..81, avoiding water [-98..-85, -24..-14])
    for x in range(-102, -81):
        for z in range(-28, -9):
            if -98 <= x <= -84 and -24 <= z <= -14:
                continue
            if -84 <= x <= -80 and -22 <= z <= -16:
                continue
            top_blk = "moss_block" if (x * 3 + z * 5) % 4 == 0 else "grass_block"
            c.set(x, 81, z, top_blk)
            if (x * 7 + z * 11) % 9 == 0:
                c.set(x, 82, z, "short_grass")
    c.tree(-100, 82, -26, "spruce_log", "spruce_leaves", height=6, canopy_r=2.6, seed=401)
    c.tree(-100, 82, -12, "oak_log", "oak_leaves", height=5, canopy_r=2.5, seed=402)
    c.tree(-86, 82, -26, "spruce_log", "spruce_leaves", height=5, canopy_r=2.2, seed=403)

    # === 4B-2: Upgrade Solid Flume Base into Sculpted Roman Stone Aqueduct Arches ===
    # Original flume steps run X=-84..-67, Z=-9..-5, water bed fy = 71 - step (step=0..8)
    # Clear ONLY the solid stone_bricks wall that build_part_surface placed from Y=62..fy-1 (preserving the seabed & Z=-9 depth ruler!)
    for step in range(9):
        fx = -84 + step * 2
        fy = 71 - step
        for x in (fx, fx + 1):
            # Support bed immediately beneath channel (fy-1) in mossy_stone_bricks
            c.fill(x, fy - 1, -9, x, fy - 1, -5, "mossy_stone_bricks")
            # Open air vault arches between piers from sea+1 up to fy-2
            if fy - 2 >= sea + 1:
                c.fill(x, sea + 1, -9, x, fy - 2, -5, "air")
            # Replace only the Y=62 solid brick slice back to water between piers
            if fy - 2 >= sea:
                c.fill(x, sea, -9, x, sea, -5, "water", level="0")
    # Build 5 strong aqueduct piers down until hitting solid seabed/sandstone + arched spandrel fillets between them
    pier_xs = (-84, -80, -76, -72, -68)
    for px in pier_xs:
        step = (px - (-84)) // 2
        top_y = 71 - step - 1
        for z in range(-9, -4):
            # Preserve the Z=-9 depth ruler stripe on the seabed!
            for y in range(top_y, 36, -1):
                cur = c.get_short(px, y, z)
                if cur in ("sand", "sandstone", "smooth_quartz", "polished_deepslate") and y <= 50:
                    break
                c.set(px, y, z, "mossy_stone_bricks")
        # Stone plinth collar at waterline
        c.fill(px, sea - 1, -9, px, sea + 1, -5, "polished_deepslate")
        # Inverted stair arch springers on east & west faces of piers (where clear above water)
        if top_y - 1 >= sea + 1:
            if px > -84:
                c.fill(px - 1, top_y - 1, -9, px - 1, top_y - 1, -5, "mossy_stone_brick_stairs", facing="east", half="top", shape="straight")
            if px < -68:
                c.fill(px + 1, top_y - 1, -9, px + 1, top_y - 1, -5, "mossy_stone_brick_stairs", facing="west", half="top", shape="straight")

    # === 4D-1: Decayed Oak Shipwreck Keel right beside Sunken Vault & Visible from 4C Tunnel! ===
    # Located cleanly in X in [-98..-83], Z in [17..21], Y=38..51 (between 4C tunnel Z<=16 and 4D Vault Z>=22)
    # Main keel timber running E-W along Z=19, gently rising toward prow at East (-83)
    for i, x in enumerate(range(-97, -83)):
        ky = 38 + (1 if i >= 9 else 0)
        c.set(x, ky, 19, "oak_log", axis="x")
        c.set(x, ky, 18, "spruce_planks")
        c.set(x, ky, 20, "spruce_planks")
        # Curved broken hull ribs every 2 blocks
        if i % 2 == 0:
            rib_h = 3 if 2 <= i <= 11 else 2
            c.fill(x, ky + 1, 17, x, ky + rib_h, 17, "oak_log", axis="y")
            c.fill(x, ky + 1, 21, x, ky + max(1, rib_h - (1 if i == 6 else 0)), 21, "oak_log", axis="y")
            # Partial weathered hull planking
            if i not in (5, 6, 10):
                c.set(x, ky + 1, 17, "oak_planks")
                c.set(x, ky + 1, 21, "oak_planks")
        elif i in (3, 7, 11):
            # Mossy ballast stones inside exposed hull
            c.set(x, ky + 1, 19, "mossy_cobblestone")
    # Snapped oak mast rising dramatically from (-91, 39..51, 19) with broken yardarm
    c.fill(-91, 39, 19, -91, 50, 19, "stripped_oak_log", axis="y")
    c.set(-90, 51, 19, "stripped_oak_log", axis="y")
    c.fill(-91, 47, 17, -91, 47, 21, "oak_fence", waterlogged="true")
    # Sunken treasure chest tucked in shipwreck stern
    c.set(-95, 39, 19, "chest", facing="east", type="single", waterlogged="true")
    c.place_chest(-95, 39, 19, facing="east", block="chest")
    # Re-apply waterlogged=true on the underwater chest block state while preserving BE
    c.grid[((39 - CANVAS_MIN_Y) << 16) | ((19 - MIN_Z) << 8) | (-95 - MIN_X)] = c._id(
        "chest", {"facing": "east", "type": "single", "waterlogged": "true"}
    )

    # === 4D-2: Underwater Coral Reefs, Seagrass Meadows & Kelp Forests ===
    coral_clusters = [
        (-74, 38, 19, "tube_coral_block", "tube_coral", "tube_coral_fan", True),
        (-66, 38, 18, "brain_coral_block", "brain_coral", "brain_coral_fan", True),
        (-56, 38, 50, "fire_coral_block", "fire_coral", "fire_coral_fan", True),
        (-44, 38, 50, "bubble_coral_block", "brain_coral", "tube_coral_fan", False),
        (-87, 38, 4, "horn_coral_block", "fire_coral", "fire_coral_fan", True),
        (-76, 38, 4, "tube_coral_block", "tube_coral", "brain_coral_fan", False),
    ]
    rng = random.Random(404404)
    for rx, ry, rz, cblk, cplant, cfan, add_pickle in coral_clusters:
        for dx in range(-2, 3):
            for dz in range(-2, 3):
                if abs(dx) + abs(dz) <= 2 and rng.random() < 0.78:
                    h = 2 if (dx == 0 and dz == 0) else 1
                    for dy in range(h):
                        if c.get_short(rx + dx, ry + dy, rz + dz) == "water":
                            c.set(rx + dx, ry + dy, rz + dz, cblk)
                    top_y = ry + h
                    if c.get_short(rx + dx, top_y, rz + dz) == "water":
                        if add_pickle and dx == 0 and dz == 0:
                            c.set(rx, top_y, rz, "sea_pickle", pickles="2", waterlogged="true")
                        elif rng.random() < 0.55:
                            c.set(rx + dx, top_y, rz + dz, cfan if rng.random() < 0.5 else cplant, waterlogged="true")

    # Helper to keep scientific rulers, temple marble floor, tunnel & Snell target crystal clear
    def _seabed_excluded(x: int, z: int) -> bool:
        if -11 <= z <= -8:  # 4A depth ruler stripe
            return True
        if -99 <= x <= -28 and 7 <= z <= 16:  # 4C glass tunnel & rotunda
            return True
        if -63 <= x <= -39 and 21 <= z <= 49:  # 4D quartz temple colonnade floor
            return True
        if -80 <= x <= -64 and 23 <= z <= 43:  # 4D Snell platform
            return True
        if -103 <= x <= -80 and 21 <= z <= 49:  # 4D underwater dark vault
            return True
        if -99 <= x <= -82 and 16 <= z <= 22:  # shipwreck keel
            return True
        if -105 <= x <= -77 and -30 <= z <= -4:  # 4B waterfall cliff & flume
            return True
        return False

    for x in range(-103, -34):
        for z in range(-29, 52):
            if _seabed_excluded(x, z):
                continue
            # Find sand seabed surface under water
            bed_y = None
            for y in range(37, sea - 1):
                if c.get_short(x, y, z) == "sand" and c.get_short(x, y + 1, z) == "water":
                    bed_y = y
                    break
            if bed_y is None:
                continue
            water_depth = sea - bed_y
            roll = rng.random()
            if water_depth >= 12 and roll < 0.045:
                # Swaying kelp stalk (strictly staying >= 4 blocks below surface)
                kh = rng.randint(4, min(11, water_depth - 4))
                can_place = all(c.get_short(x, bed_y + 1 + dy, z) == "water" for dy in range(kh))
                if can_place:
                    for dy in range(kh - 1):
                        c.set(x, bed_y + 1 + dy, z, "kelp_plant")
                    c.set(x, bed_y + kh, z, "kelp", age="25")
            elif water_depth >= 3 and roll < 0.14:
                if c.get_short(x, bed_y + 2, z) == "water":
                    c.set(x, bed_y + 1, z, "tall_seagrass", half="lower")
                    c.set(x, bed_y + 2, z, "tall_seagrass", half="upper")
            elif water_depth >= 2 and roll < 0.30:
                c.set(x, bed_y + 1, z, "seagrass")


# ---------------------------------------------------------------------------
# 5. Twin Transparent Glass Bubble-Column Elevators (Y=64 <-> Y=96)
# ---------------------------------------------------------------------------

def refine_bubble_elevators(c: VoxelCanvas) -> None:
    """Build twin leak-proof transparent glass bubble elevators flanking the central clocktower's north piers."""
    y0 = MAIN_FLOOR_Y  # 64
    top_deck = 96

    # West shaft (Ascending): center (-4, 65..97, -7), bottom soul_sand at Y=64
    # East shaft (Descending): center (4, 65..96, -7), bottom magma_block at Y=64
    shafts = [
        (-4, "soul_sand", "false", top_deck + 1, "east", "▲ 上行至 Y=96", "直走进入极速升空"),
        (4, "magma_block", "true", top_deck, "west", "▼ 下行回地面", "顶层入井涡流缓降"),
    ]

    for sx, base_blk, drag_val, water_top_y, inward_dir, title_line, desc_line in shafts:
        sz = -7
        # 1. Seal shaft perimeter completely with transparent glass & copper structural ribs (Y=64..98)
        for y in range(y0, top_deck + 3):
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    if dx == 0 and dz == 0:
                        continue
                    is_corner = abs(dx) == 1 and abs(dz) == 1
                    if is_corner:
                        blk = "waxed_cut_copper" if (y % 6 == 0 or y in (y0, top_deck, top_deck + 2)) else "waxed_copper_bars"
                    else:
                        blk = "polished_deepslate" if y == y0 else "glass"
                    c.set(sx + dx, y, sz + dz, blk)

        # 2. Core bubble column + base actuator block
        c.set(sx, y0, sz, base_blk)
        for y in range(y0 + 1, water_top_y + 1):
            c.set(sx, y, sz, "bubble_column", drag=drag_val)
        for y in range(water_top_y + 1, top_deck + 3):
            c.set(sx, y, sz, "air")

        # 3. Ground-level North entry/exit at Z=-8 (Y=65..66) using open spruce fence gates -> 100% leak-proof!
        c.set(sx, y0 + 1, sz - 1, "spruce_fence_gate", facing="north", open="true", in_wall="false", waterlogged="false")
        c.set(sx, y0 + 2, sz - 1, "spruce_fence_gate", facing="north", open="true", in_wall="false", waterlogged="false")
        # Solid lintel & side pillars framing the ground portal
        c.set(sx - 1, y0 + 1, sz - 1, "polished_deepslate")
        c.set(sx - 1, y0 + 2, sz - 1, "polished_deepslate")
        c.set(sx + 1, y0 + 1, sz - 1, "polished_deepslate")
        c.set(sx + 1, y0 + 2, sz - 1, "polished_deepslate")
        c.fill(sx - 1, y0 + 3, sz - 1, sx + 1, y0 + 3, sz - 1, "waxed_cut_copper")
        c.place_sign(
            sx,
            y0 + 3,
            sz - 2,
            ["[时钟塔气泡电梯]", title_line, desc_line, "玻璃井道零溢水"],
            facing="north",
            wood="spruce",
            color="aqua" if drag_val == "false" else "gold",
        )

        # 4. Top-level Y=96 Sky Ring bridge connection (inward side faces center North sky bridge at X=±2)
        in_dx = 1 if sx < 0 else -1
        # Copper threshold step connecting elevator directly onto North Sky Bridge (X=±2, Y=96, Z=-7)
        c.set(sx + in_dx, top_deck, sz, "waxed_cut_copper")
        # Open the bridge railing at X=±2, Y=97..98, Z=-7 so player walks seamlessly onto bridge!
        bridge_x = -2 if sx < 0 else 2
        c.set(bridge_x, top_deck + 1, sz, "air")
        c.set(bridge_x, top_deck + 2, sz, "air")
        if drag_val == "false":
            # Ascending shaft has water at Y=97 -> seal exit with open fence gate at (sx+1, 97, -7), air at Y=98!
            c.set(sx + in_dx, top_deck + 1, sz, "spruce_fence_gate", facing="west", open="true", in_wall="false", waterlogged="false")
            c.set(sx + in_dx, top_deck + 2, sz, "air")
        else:
            # Descending shaft water ends at Y=96 -> Y=97..98 is completely dry open walk-in!
            c.set(sx + in_dx, top_deck + 1, sz, "air")
            c.set(sx + in_dx, top_deck + 2, sz, "air")

        # 5. Architectural glass-and-copper crown cap at Y=99 (top_deck+3)
        c.fill(sx - 1, top_deck + 3, sz - 1, sx + 1, top_deck + 3, sz + 1, "waxed_cut_copper_slab", type="bottom")
        c.set(sx, top_deck + 3, sz, "glass")
        c.set(sx, top_deck + 4, sz, "waxed_copper_lantern", hanging="false")


# ---------------------------------------------------------------------------
# 6. Zone 2, Zone 3 & Zone 5 Interior Micro-Refinements
# ---------------------------------------------------------------------------

def refine_dark_and_cathedral_interiors(c: VoxelCanvas) -> None:
    """Add Zone 2A optical targets, Zone 2D 4-material corner pillars, safe viewing daises & Zone 5 cathedral props."""
    y0 = MAIN_FLOOR_Y  # 64

    # === Zone 2A: Mid-corridor 3D Optical Inspection Target Pillars every ~10 blocks along Z=-48 ===
    # Chosen X coordinates safely clear of `2a_grate_hall` (-16.5, 65.0, -48.5) & its 3-block sightline!
    target_mats = ("gold_block", "iron_block", "waxed_copper_block")
    for idx, tx in enumerate((-28, -20, -10, 0, 10, 20, 28)):
        c.set(tx, y0 + 1, -48, "polished_deepslate")
        c.set(tx, y0 + 2, -48, "white_concrete")        # crisp diffuse grate-shadow slice
        c.set(tx, y0 + 3, -48, target_mats[idx % 3])    # vivid metallic specular glint
        c.set(tx, y0 + 4, -48, "quartz_slab", type="bottom")

    # === Zone 2D: 4 Floor-to-Ceiling Reflective Material Corner Columns (Gold, Iron, Fresh Copper, Obsidian) ===
    corner_specs = [
        (-11, -100, +1, +1, "gold_block"),         # NW: Pure Gold
        (11, -100, -1, +1, "iron_block"),          # NE: Pure Iron
        (-11, -60, +1, -1, "waxed_copper_block"),  # SW: Fresh Copper
        (11, -60, -1, -1, "obsidian"),             # SE: Obsidian Fresnel
    ]
    for cx, cz, sx, sz, mat in corner_specs:
        cells = ((cx, cz), (cx + sx, cz), (cx, cz + sz))
        for x, z in cells:
            c.set(x, y0 + 1, z, "polished_deepslate")
            c.fill(x, y0 + 2, z, x, y0 + 17, z, mat)
            c.set(x, y0 + 18, z, "chiseled_deepslate")

    # Safe elevated viewing dais at `2d_lantern_tower` (0.5, 66.0, -64.5) so player stands solidly at Y=66!
    c.fill(-1, y0 + 1, -65, 1, y0 + 1, -63, "polished_deepslate")
    c.set(0, y0 + 1, -64, "waxed_cut_copper")
    c.fill(-1, y0 + 1, -62, 1, y0 + 1, -62, "polished_deepslate_stairs", facing="north", half="bottom", shape="straight")
    c.fill(-1, y0 + 1, -66, 1, y0 + 1, -66, "polished_deepslate_stairs", facing="south", half="bottom", shape="straight")
    # Relocate the floating sign at (0, 66, -62) onto the east edge of the dais so sightline stays 100% pure
    c.set(0, y0 + 2, -62, "air")
    c.place_sign(1, y0 + 2, -63, ["[2D · 同心镂空灯塔]", "三层格栅/栏杆/锁链", "四角金铁铜曜反光柱", "体素光追核心地标"], wall=False, rotation=8, color="yellow")

    # === Zone 3C: Safe viewing dais at `3c_slit_shaft` (-88.0, 45.0, -74.5) ===
    c.fill(-90, 44, -76, -86, 44, -74, "polished_deepslate")
    c.set(-88, 44, -75, "waxed_cut_copper")
    c.fill(-89, 44, -73, -87, 44, -73, "polished_deepslate_stairs", facing="north", half="bottom", shape="straight")

    # === Zone 5 Cathedral Interior Polish (`5_cathedral` hero Path Tracing frame!) ===
    # 1. Remove floating center sign at (0, 66, 52) blocking the central axis; place standing plaque on platform corner!
    c.set(0, y0 + 2, 52, "air")
    c.place_sign(
        2,
        y0 + 2,
        51,
        ["[Ctrl+Home 路径追踪]", "256 SPP 基准机位", "正对信标/传送门/穹顶", "可与实时Lumen对比"],
        wall=False,
        rotation=9,
        wood="spruce",
        color="yellow",
    )

    # 2. Nave Gothic Pews flanking the reflective central aisle (facing South toward the altar!)
    for pz in (54, 57, 60, 63):
        # West nave pews (X=-10..-7)
        c.fill(-10, y0 + 1, pz, -7, y0 + 1, pz, "dark_oak_stairs", facing="north", half="bottom", shape="straight")
        c.set(-11, y0 + 1, pz, "dark_oak_trapdoor", facing="west", half="bottom", open="true")
        # East nave pews (X=7..10)
        c.fill(7, y0 + 1, pz, 10, y0 + 1, pz, "dark_oak_stairs", facing="north", half="bottom", shape="straight")
        c.set(11, y0 + 1, pz, "dark_oak_trapdoor", facing="east", half="bottom", open="true")

    # 3. Low Nave Plinths & Warm Copper Candelabras framing the central processional aisle (X=±6, only 6 lanterns!)
    for cz in (54, 60, 66, 80, 86):
        for cx in (-6, 6):
            c.set(cx, y0 + 1, cz, "chiseled_quartz_block")
            c.set(cx, y0 + 2, cz, "waxed_copper_bars")
            if cz in (54, 66, 80):
                c.set(cx, y0 + 3, cz, "waxed_copper_lantern", hanging="false")
            else:
                c.set(cx, y0 + 3, cz, "gold_block")

    # 4. Grand Stepped Altar Surround (around X=-7..7, Z=66..78, Y=65) + 4 Corner Sanctuary Pillars + Portal Frame
    for x in range(-7, 8):
        c.set(x, y0 + 1, 65, "quartz_stairs", facing="south", half="bottom", shape="straight")
        c.set(x, y0 + 1, 79, "quartz_stairs", facing="north", half="bottom", shape="straight")
    for z in range(66, 79):
        c.set(-8, y0 + 1, z, "quartz_stairs", facing="east", half="bottom", shape="straight")
        c.set(8, y0 + 1, z, "quartz_stairs", facing="west", half="bottom", shape="straight")
    for sx in (-7, 7):
        for sz in (66, 78):
            c.pillar(sx, y0 + 2, y0 + 7, sz, "quartz_pillar", "chiseled_quartz_block", "gold_block")
            c.set(sx, y0 + 8, sz, "waxed_cut_copper_slab", type="bottom")
    # Ornate Gothic obsidian & gold wings flanking the Nether Portal (Z=76, X=±4)
    for sx in (-4, 4):
        c.fill(sx, y0 + 2, 76, sx, y0 + 8, 76, "obsidian")
        c.set(sx, y0 + 9, 76, "gold_block")
        c.set(sx, y0 + 10, 76, "waxed_copper_bars")
    c.fill(-3, y0 + 10, 76, 3, y0 + 10, 76, "gold_block")
    c.set(0, y0 + 11, 76, "waxed_cut_copper")


# ---------------------------------------------------------------------------
# 7. Global Floating Attachment Polish Pass (Zero Unsupported Signs/Torches/Lanterns)
# ---------------------------------------------------------------------------

def fix_all_floating_attachments(c: VoxelCanvas) -> None:
    """Fix every floating wall_torch, hanging lantern, or wall_sign across the entire map."""
    y0 = MAIN_FLOOR_Y  # 64

    # 1. Zone 1D Grazing-Angle Wall Torches (X=39..67, Y=67, Z=-10 and Z=+10): add polished_deepslate dividing pier behind each!
    for z_sign in (-1, 1):
        inner_z = -9 if z_sign < 0 else 9
        for idx in range(8):
            bx = 36 + idx * 4
            c.fill(bx + 3, y0 + 1, inner_z, bx + 3, y0 + 5, inner_z, "polished_deepslate")

    # 2. Zone 2C Hanging Lanterns (add iron_chain straight up to Zone 2 ceiling Y=82!)
    for lx, ly, lz in ((-16, 66, -99), (-33, 69, -92), (-33, 69, -74)):
        c.fill(lx, ly + 1, lz, lx, 82, lz, "iron_chain", axis="y")

    # 3. Zone 4C Underwater Tunnel Sign: move off tunnel centerline (-36, 44, 12) onto glass north wall (-36, 44, 11)!
    if (-36, 44, 12) in c.block_entities:
        c.set(-36, 44, 12, "air")
        c.place_sign(
            -36,
            44,
            11,
            ["[干式海底玻璃隧道]", "贴玻璃水体面态", "隔窗仰望水底焦散", "与沉船龙骨柱廊"],
            facing="south",
            wood="dark_oak",
            color="white",
        )

    # 4. General Sweep: convert any remaining unsupported wall_sign to a clean standing sign or mounted plaque
    back_dir: Dict[str, Tuple[int, int, int]] = {
        "north": (0, 0, 1),
        "south": (0, 0, -1),
        "east": (-1, 0, 0),
        "west": (1, 0, 0),
    }
    facing_to_rot = {"south": 0, "west": 4, "north": 8, "east": 12}
    invalid_sup = {"air", "cave_air", "void_air", "water", "lava", "bubble_column"}

    # Snapshot keys since we may update block states in-place
    sign_coords = [pos for pos, be in c.block_entities.items() if be.get("id") == (8, "minecraft:sign")]
    for x, y, z in sign_coords:
        name, props = c.get(x, y, z)
        sh = name.removeprefix("minecraft:")
        if "wall_sign" not in sh:
            continue
        p = dict(props)
        facing = p.get("facing", "north")
        dx, dy, dz = back_dir[facing]
        sup = c.get_short(x + dx, y + dy, z + dz)
        if sup not in invalid_sup:
            continue

        # Save existing sign block entity NBT before adjusting block state
        saved_be = c.block_entities[(x, y, z)]
        below = c.get_short(x, y - 1, z)
        wood = sh.removesuffix("_wall_sign")
        if below not in invalid_sup:
            # Grounded directly on solid floor -> convert cleanly to standing sign
            c.set(x, y, z, f"{wood}_sign", rotation=str(facing_to_rot[facing]), waterlogged="false")
            c.block_entities[(x, y, z)] = saved_be
        else:
            # Elevated wall_sign with empty space behind & below -> add a sleek dark-stone backing post behind it!
            bx, by, bz = x + dx, y + dy, z + dz
            # Fill slender pillar down until hitting solid floor (max 6 blocks) so it never floats either
            for py in range(by, max(y0, by - 6), -1):
                if c.get_short(bx, py, bz) in invalid_sup:
                    c.set(bx, py, bz, "polished_deepslate")
                else:
                    break
            c.set(bx, by, bz, "polished_deepslate")


# ---------------------------------------------------------------------------
# Master Refinement Entrypoint
# ---------------------------------------------------------------------------

def apply_refinements(c: VoxelCanvas) -> None:
    """Apply all architectural trims, naturalization, elevators, lighting polish, and attachment fixes."""
    refine_skyline_roofs(c)
    refine_southeast_garden_lighthouse_pier(c)
    refine_facades_and_sky_platform(c)
    refine_zone4_water_nature(c)
    refine_bubble_elevators(c)
    refine_dark_and_cathedral_interiors(c)
    fix_all_floating_attachments(c)
