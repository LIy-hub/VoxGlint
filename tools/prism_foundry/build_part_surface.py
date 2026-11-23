"""Main builder (Part 1): Island base, Zone 0 (Plaza), Zone 1 (PBR Gallery), Zone 4 (Water Optics), Zone 5 (Crystal Cathedral), Sky Ring."""

from __future__ import annotations

import math
from voxel_canvas import MAIN_FLOOR_Y, SEA_LEVEL, VoxelCanvas


def build_island_foundation(c: VoxelCanvas) -> None:
    """Build a sculptural terraced stone/deepslate island rising smoothly out of the Y=62 warm ocean."""
    # Biomes: plains on dry zones, warm_ocean on water zones and exterior
    c.set_biome_rect(-128, -128, 127, 127, "warm_ocean")
    c.set_biome_rect(-32, -32, 31, 31, "plains")
    c.set_biome_rect(32, -32, 111, 31, "plains")
    c.set_biome_rect(-32, 32, 31, 111, "plains")
    c.set_biome_rect(-47, -111, 47, -32, "plains")
    c.set_biome_rect(-111, -111, -32, -32, "plains")
    c.set_biome_rect(32, 32, 111, 111, "plains")

    # Core structural landmass footprints (Y=33..63 filled with stone/deepslate base, topped at Y=64)
    # Keep Zone 4 (west bay X in [-108, -32], Z in [-32, 54]) open water!
    footprints = [
        (-30, -30, 30, 30),      # Zone 0 central plaza
        (30, -34, 102, 34),      # Zone 1 east PBR museum
        (-38, -104, 38, -30),    # Zone 2 north dark voxel hall
        (-102, -102, -38, -38),  # Zone 3 northwest Lumen GI lab
        (-32, 30, 32, 104),      # Zone 5 south crystal cathedral
        (32, 34, 104, 104),      # Zone 6 southeast underground complex footprint
    ]
    for x1, z1, x2, z2 in footprints:
        # Subsea foundation (Y=34..61) in stone bricks / deepslate bricks
        c.fill(x1 - 2, 34, z1 - 2, x2 + 2, 59, z2 + 2, "stone")
        c.fill(x1 - 1, 60, z1 - 1, x2 + 1, 62, z2 + 1, "deepslate_bricks")
        # Quay / seawall trim at Y=63..64
        c.fill(x1, 63, z1, x2, 64, z2, "polished_andesite")
        # Clear air above deck
        c.fill(x1, 65, z1, x2, 130, z2, "air")

    # Sculpted seawall stone steps / retaining curb along water edges
    for x1, z1, x2, z2 in footprints:
        for x in range(x1, x2 + 1):
            for z in (z1, z2):
                if c.get_short(x, 62, z - 2) == "water" or c.get_short(x, 62, z + 2) == "water":
                    c.set(x, 64, z, "polished_deepslate")
        for z in range(z1, z2 + 1):
            for x in (x1, x2):
                if c.get_short(x - 2, 62, z) == "water" or c.get_short(x + 2, 62, z) == "water":
                    c.set(x, 64, z, "polished_deepslate")


def build_zone0_plaza(c: VoxelCanvas) -> None:
    """Zone 0: Central Sundial Plaza, 48-block Clocktower, Weather Dry/Wet Gallery, Foliage Garden, Anisotropic Avenue, Control Console."""
    y0 = MAIN_FLOOR_Y  # 64

    # 1. Circular grand stone/quartz sundial pavement (radius 27)
    for dx in range(-28, 29):
        for dz in range(-28, 29):
            dist = math.hypot(dx, dz)
            if dist <= 27.5:
                if abs(dist - 27.0) < 0.75 or abs(dist - 18.0) < 0.65 or abs(dist - 9.0) < 0.65:
                    blk = "polished_deepslate"
                elif abs(dx) <= 1 or abs(dz) <= 1:
                    blk = "quartz_block"
                elif abs(abs(dx) - abs(dz)) <= 0.8 and dist > 7:
                    blk = "waxed_cut_copper"
                elif (dx + dz) % 2 == 0 and dist < 18:
                    blk = "smooth_quartz"
                else:
                    blk = "stone_bricks"
                c.set(dx, y0, dz, blk)

    # Four cardinal gateway arches into Zones 1, 2, 4, 5
    # North gate (towards Zone 2 dark hall)
    c.arch_x(-5, 5, y0 + 1, 7, -26, "polished_deepslate")
    # South gate (towards Zone 5 cathedral / anisotropic avenue)
    c.arch_x(-5, 5, y0 + 1, 8, 26, "quartz_pillar")
    # East gate (towards Zone 1 PBR museum)
    c.arch_z(-5, 5, y0 + 1, 8, 26, "waxed_cut_copper")
    # West gate (towards Zone 4 water bay)
    c.arch_z(-5, 5, y0 + 1, 8, -26, "prismarine_bricks")

    # 2. Central Sundial Clocktower (X in [-5,5], Z in [-5,5], Y=64..116)
    # Open four-arch pedestal at ground level so light streams through
    c.fill(-5, y0, -5, 5, y0, 5, "polished_deepslate")
    c.fill(-3, y0, -3, 3, y0, 3, "smooth_quartz")
    c.set(0, y0, 0, "gold_block")
    for sx in (-4, 4):
        for sz in (-4, 4):
            c.fill(sx - 1, y0 + 1, sz - 1, sx + 1, y0 + 14, sz + 1, "quartz_pillar")
            c.fill(sx, y0 + 1, sz, sx, y0 + 36, sz, "waxed_copper_block")
            # Corner buttress trims
            c.set(sx, y0 + 1, sz, "chiseled_quartz_block")
            c.set(sx, y0 + 14, sz, "waxed_chiseled_copper")
    # Ground-level four arches
    for z in (-4, 4):
        c.arch_x(-3, 3, y0 + 1, 6, z, "smooth_quartz")
    for x in (-4, 4):
        c.arch_z(-3, 3, y0 + 1, 6, x, "smooth_quartz")

    # Mid-shaft (Y=78..100): open copper-grate & glass lantern belfry casting crisp rotating shadows
    for y in range(y0 + 15, y0 + 34):
        for dx in range(-4, 5):
            for dz in range(-4, 5):
                on_edge = abs(dx) == 4 or abs(dz) == 4
                on_corner = abs(dx) == 4 and abs(dz) == 4
                if on_corner:
                    c.set(dx, y, dz, "polished_deepslate")
                elif on_edge:
                    if y in (y0 + 15, y0 + 24, y0 + 33):
                        c.set(dx, y, dz, "waxed_cut_copper")
                    elif abs(dx) <= 1 or abs(dz) <= 1:
                        c.set(dx, y, dz, "waxed_copper_grate")
                    else:
                        c.set(dx, y, dz, "iron_bars")
    # Sky ring deck at Y=96 (y0 + 32)
    c.fill(-5, 96, -5, 5, 96, 5, "polished_deepslate")
    c.fill(-3, 96, -3, 3, 96, 3, "glass")
    # Spire & copper lightning crown (Y=98..116)
    c.dome(0, 98, 0, 4.4, "waxed_oxidized_copper", hollow=True, height_scale=1.6, rib_block="waxed_copper_block")
    c.fill(0, 105, 0, 0, 113, 0, "gold_block")
    c.set(0, 114, 0, "sea_lantern")
    c.set(0, 115, 0, "waxed_lightning_rod", facing="up")
    # Hanging beacon-like lantern inside the lower tower arch
    c.fill(0, y0 + 9, 0, 0, y0 + 13, 0, "iron_chain", axis="y")
    c.set(0, y0 + 8, 0, "sea_lantern")
    for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        c.set(dx, y0 + 8, dz, "waxed_copper_grate")

    # 3. Half-Roofed Weather & Porosity Dry/Wet Gallery (NE quadrant: X in [10, 26], Z in [-24, -10])
    # Shows sharp PrecipGrid dry/wet roof line & puddle contrast between porous & non-porous blocks
    strip_materials = [
        ("stone_bricks", "高孔隙石砖"),
        ("oak_planks", "多孔木板"),
        ("sandstone", "多孔砂岩"),
        ("white_wool", "极高孔隙羊毛"),
        ("polished_deepslate", "半抛光深板岩"),
        ("quartz_block", "抛光石英"),
        ("iron_block", "纯金属铁"),
        ("waxed_copper_block", "纯金属铜"),
    ]
    for i, (mat, _) in enumerate(strip_materials):
        x = 11 + i * 2
        c.fill(x, y0, -23, x + 1, y0, -11, mat)
    # Cover only the northern half (Z in [-23, -17]) with an overhanging glass/stone canopy at Y=70
    c.fill(10, y0 + 6, -24, 27, y0 + 6, -17, "polished_deepslate")
    c.fill(11, y0 + 6, -23, 26, y0 + 6, -18, "glass")
    for px in (10, 18, 27):
        c.pillar(px, y0 + 1, y0 + 5, -24, "polished_deepslate", "chiseled_deepslate", "waxed_cut_copper")
        c.pillar(px, y0 + 1, y0 + 5, -17, "polished_deepslate", "chiseled_deepslate", "waxed_cut_copper")
    # Subtle evening lanterns under roof edge
    c.set(14, y0 + 5, -17, "lantern", hanging="true")
    c.set(22, y0 + 5, -17, "waxed_copper_lantern", hanging="true")
    c.place_sign(10, y0 + 2, -16, ["[气象干湿对比廊]", "北半屋檐下干燥", "南半露天雨洼反射", "按中控台切换降雨"], facing="south")

    # 4. Foliage Subsurface & Cutout-Shadow Garden (SW quadrant: X in [-25, -10], Z in [10, 25])
    c.fill(-25, y0, 10, -10, y0, 25, "grass_block")
    # Raised stone border
    for x in range(-25, -9):
        c.set(x, y0, 10, "stone_bricks")
        c.set(x, y0, 25, "stone_bricks")
    for z in range(10, 26):
        c.set(-25, y0, z, "stone_bricks")
        c.set(-10, y0, z, "stone_bricks")
    # Diverse trees for two-sided foliage subsurface scattering
    c.tree(-21, y0 + 1, 14, "oak_log", "oak_leaves", height=6, canopy_r=2.9, seed=11)
    c.tree(-14, y0 + 1, 21, "cherry_log", "cherry_leaves", height=6, canopy_r=3.1, seed=22)
    c.tree(-21, y0 + 1, 22, "birch_log", "birch_leaves", height=5, canopy_r=2.5, seed=33)
    # CutoutNoShadow (grass, flowers, rails) vs Cutout (fences, trapdoors, ladders) right side-by-side
    for dx in range(-18, -11):
        c.set(dx, y0 + 1, 12, "oak_fence")
    c.set(-15, y0 + 1, 13, "oak_trapdoor", facing="south", half="bottom", open="true")
    c.set(-14, y0 + 1, 13, "iron_bars")
    for fx, fz, fl in (
        (-17, 15, "short_grass"),
        (-16, 15, "poppy"),
        (-15, 15, "dandelion"),
        (-14, 15, "cornflower"),
        (-13, 15, "blue_orchid"),
        (-17, 16, "fern"),
        (-16, 16, "lily_of_the_valley"),
        (-15, 16, "short_grass"),
        (-14, 16, "short_grass"),
    ):
        c.set(fx, y0 + 1, fz, fl)
    # Small rail strip to verify CutoutNoShadow
    for rz in range(14, 20):
        c.set(-12, y0 + 1, rz, "rail", shape="north_south")
    c.place_sign(-10, y0 + 2, 13, ["[树叶次表面 & 镂空]", "树叶:双面透光SSS", "栅栏:实体遮罩投影", "草花铁轨:无杂阴影"], facing="east")

    # 5. Southward Anisotropic Filtering & Mip Inspection Avenue (Z = 24..42, continuing through Zone 5)
    for z in range(24, 43):
        c.set(-4, y0, z, "polished_deepslate")
        c.set(-3, y0, z, "deepslate_bricks")
        c.set(-2, y0, z, "cyan_glazed_terracotta", facing="north")
        c.set(-1, y0, z, "stone_bricks")
        c.set(0, y0, z, "chiseled_stone_bricks" if z % 2 == 0 else "quartz_block")
        c.set(1, y0, z, "stone_bricks")
        c.set(2, y0, z, "cyan_glazed_terracotta", facing="south")
        c.set(3, y0, z, "deepslate_bricks")
        c.set(4, y0, z, "polished_deepslate")
        if z % 6 == 0:
            c.pillar(-5, y0 + 1, y0 + 5, z, "deepslate_brick_wall", "polished_deepslate", "waxed_cut_copper")
            c.pillar(5, y0 + 1, y0 + 5, z, "deepslate_brick_wall", "polished_deepslate", "waxed_cut_copper")
            c.set(-5, y0 + 6, z, "lantern", hanging="false")
            c.set(5, y0 + 6, z, "lantern", hanging="false")

    # 6. Central Showcase Control Console (Z = 16, X in [-8, 8], facing North toward player spawn at Z=12)
    c.fill(-8, y0 + 1, 17, 8, y0 + 2, 17, "polished_deepslate")
    c.fill(-8, y0 + 3, 17, 8, y0 + 3, 17, "waxed_cut_copper_slab", type="bottom")
    controls = [
        (-7, "function prism:time_dawn", ["时间控制", ">>> 黎明 <<<", "低角度金光", "长阴影大气"]),
        (-5, "function prism:time_noon", ["时间控制", ">>> 正午 <<<", "强水下焦散", "高对比天光"]),
        (-3, "function prism:time_sunset", ["时间控制", ">>> 黄昏 <<<", "燃橙大气云", "湖面暖反射"]),
        (-1, "function prism:time_midnight", ["时间控制", ">>> 午夜 <<<", "月光与星空", "全图体素光"]),
        (1, "function prism:weather_clear", ["天气控制", ">>> 晴朗 <<<", "标准干燥面", "清晰焦散"]),
        (3, "function prism:weather_rain", ["天气控制", ">>> 降雨 <<<", "多孔湿润", "屋檐干湿线"]),
        (5, "function prism:weather_thunder", ["天气控制", ">>> 雷暴 <<<", "分叉闪电", "水面瞬态光"]),
        (7, "function prism:guide", ["导览传送", ">>> 目录 <<<", "聊天栏点击", "瞬移各展区"]),
    ]
    for cx, cmd, sign_lines in controls:
        c.place_cmd(cx, y0 + 1, 18, cmd, facing="south")
        c.set(cx, y0 + 1, 16, "stone_button", face="wall", facing="north", powered="false")
        c.place_sign(cx, y0 + 2, 16, sign_lines, facing="north", wood="spruce", color="yellow")

    # Decorative copper & gold plinths framing the welcome plaza
    for sx in (-12, 12):
        c.fill(sx - 1, y0 + 1, 10, sx + 1, y0 + 1, 12, "polished_deepslate")
        c.set(sx, y0 + 2, 11, "waxed_copper_golem_statue")

    c.register_waypoint("0_plaza", 0.5, y0 + 1.0, 12.5, 180.0, -8.0, "区0 · 中央日晷广场与中控台")
    c.register_waypoint("0_weather", 18.5, y0 + 1.0, -8.5, 180.0, 8.0, "区0 · 气象干湿与孔隙对比廊")


def build_zone1_pbr_gallery(c: VoxelCanvas) -> None:
    """Zone 1: PBR Material & Metal Gradient Museum (X in [32, 98], Z in [-32, 32]).
    North wing = Open-air daylight terrace; South wing = Enclosed neutral-lit gallery (mirrored comparison).
    """
    y0 = MAIN_FLOOR_Y  # 64

    # Central Spine Corridor connecting Plaza East Gate (X=26..96, Z=-4..4)
    c.fill(26, y0, -4, 96, y0, 4, "smooth_quartz")
    for x in range(28, 96, 4):
        c.fill(x, y0, -4, x, y0, 4, "polished_deepslate")
    # Decorative spine colonnade
    for x in range(32, 96, 8):
        c.pillar(x, y0 + 1, y0 + 7, -5, "quartz_pillar", "polished_deepslate", "waxed_cut_copper")
        c.pillar(x, y0 + 1, y0 + 7, 5, "quartz_pillar", "polished_deepslate", "waxed_cut_copper")
        c.arch_z(-5, 5, y0 + 1, 6, x, "quartz_block", carve_air=False)
        c.set(x, y0 + 9, 0, "sea_lantern")

    # ---- Helper to build the mirrored PBR showcase inside either wing ----
    def build_pbr_wing(z_sign: int, enclosed: bool) -> None:
        # z_sign = -1 (North open-air, Z in [-31..-6]); z_sign = +1 (South enclosed, Z in [6..31])
        za, zb = (-31, -6) if z_sign < 0 else (6, 31)
        # Floor: alternating neutral matte gray concrete & polished deepslate walkway
        c.fill(33, y0, za, 96, y0, zb, "gray_concrete")
        walk_z1, walk_z2 = (-18, -14) if z_sign < 0 else (14, 18)
        c.fill(33, y0, walk_z1, 96, y0, walk_z2, "polished_deepslate")

        # Perimeter enclosure
        if enclosed:
            c.hollow_box(32, y0, za - 1, 97, y0 + 12, zb + 1, "polished_deepslate", floor=None, ceiling="deepslate_tiles")
            # Re-lay floor inside
            c.fill(33, y0, za, 96, y0, zb, "gray_concrete")
            c.fill(33, y0, walk_z1, 96, y0, walk_z2, "polished_deepslate")
            # Open broad doorways to central spine (Z = 5)
            for dx in (42, 62, 82):
                c.fill(dx - 3, y0 + 1, 5, dx + 3, y0 + 6, 5, "air")
            # Clean studio linear ceiling lights (sea lanterns recessed with iron bars alternating with open)
            for lx in range(38, 94, 8):
                for lz in (10, 16, 22, 28):
                    c.set(lx, y0 + 12, lz, "sea_lantern")
        else:
            # Open-air terrace with low stone parapet around outer edge
            c.fill(32, y0 + 1, za - 1, 97, y0 + 2, za - 1, "stone_brick_wall")
            c.fill(97, y0 + 1, za - 1, 97, y0 + 2, zb, "stone_brick_wall")
            c.fill(32, y0 + 1, za - 1, 32, y0 + 2, zb, "stone_brick_wall")

        outer_z = -29 if z_sign < 0 else 29
        inner_z = -9 if z_sign < 0 else 9

        # === 1A: Pure Conductors Mirror Arcade (Gold, Iron, Fresh Copper — M=1.0, R=0.12) ===
        # X in [35..51]
        metals = [("gold_block", "纯金 M1.0 R0.12"), ("iron_block", "纯铁 M1.0 R0.12"), ("waxed_copper_block", "鲜铜 M1.0 R0.12")]
        for idx, (mblk, label) in enumerate(metals):
            bx = 36 + idx * 5
            # 4x5 mirror wall + side return + overhead lintel forming a reflective alcove
            c.fill(bx, y0 + 1, outer_z, bx + 3, y0 + 6, outer_z, mblk)
            c.fill(bx, y0 + 1, outer_z - z_sign * 3, bx, y0 + 6, outer_z, mblk)
            c.fill(bx, y0 + 6, outer_z - z_sign * 3, bx + 3, y0 + 6, outer_z, mblk)
            # Reflective floor apron
            c.fill(bx, y0, outer_z - z_sign * 3, bx + 3, y0, outer_z - z_sign, mblk)
            # Opposite high-contrast calibration pillar (red/white/cyan + end rod) so mirror reflections pop!
            tz = outer_z - z_sign * 6
            c.set(bx + 1, y0 + 1, tz, "red_concrete")
            c.set(bx + 1, y0 + 2, tz, "white_concrete")
            c.set(bx + 1, y0 + 3, tz, "cyan_concrete")
            c.set(bx + 1, y0 + 4, tz, "end_rod", facing="up")
            c.place_sign(bx + 2, y0 + 2, outer_z - z_sign, [label, "导体镜面染色", "观察高光反射"], facing="south" if z_sign < 0 else "north")

        # === 1B: 4-Stage Copper Oxidation Gradient Wall (M=1.0->0.25, R=0.12->0.74) ===
        # X in [53..71]
        copper_stages = [
            ("waxed_copper_block", "waxed_cut_copper", "waxed_copper_grate", "waxed_copper_bars", "阶段1:鲜铜"),
            ("waxed_exposed_copper", "waxed_exposed_cut_copper", "waxed_exposed_copper_grate", "waxed_exposed_copper_bars", "阶段2:斑驳"),
            ("waxed_weathered_copper", "waxed_weathered_cut_copper", "waxed_weathered_copper_grate", "waxed_weathered_copper_bars", "阶段3:风化"),
            ("waxed_oxidized_copper", "waxed_oxidized_cut_copper", "waxed_oxidized_copper_grate", "waxed_oxidized_copper_bars", "阶段4:氧化"),
        ]
        for idx, (full_c, cut_c, grate_c, bars_c, lbl) in enumerate(copper_stages):
            bx = 54 + idx * 4
            c.fill(bx, y0 + 1, outer_z, bx + 3, y0 + 3, outer_z, full_c)
            c.fill(bx, y0 + 4, outer_z, bx + 3, y0 + 5, outer_z, cut_c)
            c.fill(bx, y0 + 6, outer_z, bx + 3, y0 + 6, outer_z, grate_c)
            c.fill(bx + 1, y0 + 1, outer_z - z_sign * 2, bx + 1, y0 + 4, outer_z - z_sign * 2, bars_c)
            c.fill(bx, y0, outer_z - z_sign * 2, bx + 3, y0, outer_z - z_sign, full_c)
            # Vertical light strip directly facing the copper panel so specular lobe broadening is unmistakable
            c.set(bx + 2, y0 + 2, outer_z - z_sign * 5, "end_rod", facing="up")
            c.set(bx + 2, y0 + 3, outer_z - z_sign * 5, "end_rod", facing="up")
            c.place_sign(bx + 2, y0 + 1, outer_z - z_sign, [lbl, "涂蜡铜梯度", "高光逐级展宽"], facing="south" if z_sign < 0 else "north")

        # === 1C: Dark Metals + Full Dielectric Roughness Ramp (R=0.025 -> 0.98) ===
        # X in [73..94] along outer wall & inner wall
        dielectrics = [
            ("netherite_block", "下界合金 M0.65"),
            ("obsidian", "黑曜石 R0.08"),
            ("blue_ice", "蓝冰 R0.08"),
            ("diamond_block", "钻石块 R0.25"),
            ("emerald_block", "绿宝石 R0.25"),
            ("amethyst_block", "紫水晶 R0.25"),
            ("purple_glazed_terracotta", "釉陶瓦 R0.40"),
            ("quartz_block", "石英块 R0.40"),
            ("oak_planks", "橡木板 R0.71"),
            ("stone_bricks", "石砖 R0.83"),
            ("white_wool", "白羊毛 R0.98"),
        ]
        for idx, (blk, lbl) in enumerate(dielectrics):
            bx = 73 + idx * 2
            if bx + 1 > 95:
                break
            c.fill(bx, y0 + 1, outer_z, bx + 1, y0 + 6, outer_z, blk)
            c.fill(bx, y0, outer_z - z_sign * 2, bx + 1, y0, outer_z - z_sign, blk)
            if blk == "amethyst_block":
                c.set(bx, y0 + 4, outer_z - z_sign, "amethyst_cluster", facing="south" if z_sign < 0 else "north")
            if blk == "netherite_block":
                c.set(bx, y0 + 1, outer_z - z_sign * 2, "anvil", facing="east")
            if idx % 2 == 0:
                c.place_sign(bx, y0 + 2, outer_z - z_sign, [lbl, "粗糙度序列"], facing="south" if z_sign < 0 else "north")

        # === 1D: Grazing-Angle Normal / Relief Wall along inner_z (X in [36..66]) ===
        relief_blocks = [
            "cobblestone", "deepslate_bricks", "bricks", "oak_log",
            "deepslate_gold_ore", "deepslate_diamond_ore", "raw_copper_block", "chiseled_tuff_bricks",
        ]
        for idx, rblk in enumerate(relief_blocks):
            bx = 36 + idx * 4
            c.fill(bx, y0 + 1, inner_z, bx + 2, y0 + 5, inner_z, rblk)
            # Grazing side light mounted right off the edge to rake across surface bumps / LabPBR normals
            c.set(bx + 3, y0 + 3, inner_z + z_sign, "wall_torch", facing="north" if z_sign < 0 else "south")

        # === 1E: Dynamic Entities & Copper Golem Statues Plinth (X in [72..92], near inner_z) ===
        c.fill(72, y0 + 1, inner_z, 92, y0 + 1, inner_z + z_sign * 3, "polished_deepslate")
        statues = [
            "waxed_copper_golem_statue",
            "waxed_exposed_copper_golem_statue",
            "waxed_weathered_copper_golem_statue",
            "waxed_oxidized_copper_golem_statue",
        ]
        for i, st in enumerate(statues):
            c.set(74 + i * 2, y0 + 2, inner_z + z_sign, st)
        # Chests (wood + copper)
        c.place_chest(84, y0 + 2, inner_z + z_sign, facing="north" if z_sign < 0 else "south", block="chest")
        c.place_chest(86, y0 + 2, inner_z + z_sign, facing="north" if z_sign < 0 else "south", block="waxed_copper_chest")

        # Armor stands wearing full Iron / Gold / Diamond / Netherite armor
        yaw_face = 180.0 if z_sign < 0 else 0.0
        armors = [
            (74.5, "iron_helmet", "iron_chestplate", "iron_leggings", "iron_boots", "iron_sword", "shield"),
            (78.5, "golden_helmet", "golden_chestplate", "golden_leggings", "golden_boots", "golden_sword", "shield"),
            (82.5, "diamond_helmet", "diamond_chestplate", "diamond_leggings", "diamond_boots", "diamond_sword", "shield"),
            (86.5, "netherite_helmet", "netherite_chestplate", "netherite_leggings", "netherite_boots", "netherite_sword", "shield"),
        ]
        for ax, h, ch, lg, ft, mh, oh in armors:
            c.add_armor_stand(ax, y0 + 2.0, inner_z + z_sign * 2.5, yaw=yaw_face, head=h, chest=ch, legs=lg, feet=ft, mainhand=mh, offhand=oh)

    build_pbr_wing(z_sign=-1, enclosed=False)
    build_pbr_wing(z_sign=+1, enclosed=True)

    c.place_sign(34, y0 + 2, -5, ["<<< 露天PBR露台", "直射日光/天光反射", "铜氧化/金属/法线", "盔甲与铜傀儡阵"], facing="west")
    c.place_sign(34, y0 + 2, 5, ["室内恒光PBR馆 >>>", "相同材质镜像对照", "低掠射侧光法线墙", "零天光干扰评估"], facing="west")

    c.register_waypoint("1a_pbr_outdoor", 46.5, y0 + 1.0, -16.5, 165.0, 4.0, "区1A · 露天PBR金属与铜氧化露台")
    c.register_waypoint("1b_pbr_indoor", 46.5, y0 + 1.0, 16.5, 15.0, 4.0, "区1B · 室内恒光PBR材质美术馆")


def build_zone4_water_optics(c: VoxelCanvas) -> None:
    """Zone 4: Comprehensive Water Optics Bay & Sunken Temple (X in [-106, -32], Z in [-32, 54], Y=36..84).
    Covers P1 (5 water face modes, shore wetness porosity, aeration waterfall, flow), P2 (caustics, Snell window,
    underwater volume scatter, half-submerged, bubble columns), P3 (ice wave damping, shallow ripples & boats).
    """
    y0 = MAIN_FLOOR_Y  # 64
    sea = SEA_LEVEL    # 62

    # Carve clean deep water basin across [-104..-34], [-30..52], down to Y=38
    c.fill(-104, 38, -30, -34, sea, 52, "water", level="0")
    c.fill(-104, sea + 1, -30, -34, 92, 52, "air")
    # Sand seabed at Y=37
    c.fill(-104, 35, -30, -34, 37, 52, "sand")

    # East Quay boardwalk connecting Plaza West Gate (X=-34..-28, Z=-30..52)
    c.fill(-33, 38, -30, -28, y0, 52, "stone_bricks")
    c.fill(-33, y0, -30, -28, y0, 52, "polished_andesite")
    for z in range(-28, 52, 6):
        c.set(-33, y0 + 1, z, "waxed_copper_bars")
        c.set(-33, y0 + 2, z, "waxed_copper_lantern", hanging="false")

    # === 4A: Stepped Depth Beach + Porous vs Non-Porous Shore Wetness + Ice Damping + P3 Boat Pool ===
    # North half of bay: X in [-74..-34], Z in [-28..6]
    # Stepped seabed from 1 block deep (Y=61) at X=-36 down to 16 blocks deep (Y=46) at X=-70
    steps = [
        (-34, -39, 61),  # 1 block deep
        (-40, -45, 60),  # 2 blocks deep
        (-46, -51, 58),  # 4 blocks deep
        (-52, -57, 54),  # 8 blocks deep
        (-58, -63, 50),  # 12 blocks deep
        (-64, -72, 46),  # 16 blocks deep
    ]
    for xa, xb, bed_y in steps:
        c.fill(xa, 38, -28, xb, bed_y, 6, "sandstone")
        c.fill(xa, bed_y, -28, xb, bed_y, 6, "sand")
        # High-contrast white quartz & polished deepslate depth ruler stripe along Z=-10..-9
        for x in range(min(xa, xb), max(xa, xb) + 1):
            c.set(x, bed_y, -10, "smooth_quartz" if (x % 2 == 0) else "polished_deepslate")
            c.set(x, bed_y, -9, "smooth_quartz" if (x % 2 == 0) else "polished_deepslate")

    # Shore wetness porosity comparison piers protruding into water along Z=-27..-25 (water at Y=62, pier top Y=63)
    shore_mats = [
        (-38, "sandstone", "多孔砂岩34cm"),
        (-42, "cobblestone", "多孔圆石"),
        (-46, "oak_log", "多孔原木"),
        (-50, "white_wool", "极多孔羊毛"),
        (-54, "polished_blackstone", "抛光黑石"),
        (-58, "waxed_copper_block", "非多孔铜13cm"),
        (-62, "iron_block", "非多孔铁"),
    ]
    for sx, smat, _ in shore_mats:
        c.fill(sx - 1, 61, -28, sx + 1, 63, -24, smat)
    c.place_sign(-34, y0 + 1, -24, ["[岸湿孔隙度对照]", "北岸由东向西:", "砂岩/木/羊毛高吸湿", "黑石/铜/铁低薄膜"], facing="west")

    # Ice wave-damping floe (McWaterIce) floating at Y=62
    c.fill(-54, sea, -20, -48, sea, -15, "packed_ice")
    c.fill(-53, sea, -19, -49, sea, -16, "blue_ice")
    c.set(-51, sea + 1, -17, "blue_ice")

    # P3 Interactive boats floating in shallow/mid bay
    c.add_boat(-41.5, sea + 0.6, -4.5, yaw=45.0, boat_id="oak_boat")
    c.add_boat(-47.5, sea + 0.6, 1.5, yaw=120.0, boat_id="spruce_boat")

    # === 4B: 18-Block Cliff Waterfall (Open Drop + Aeration) & Glass-Walled Rapid Flume (5 Water Face Modes) ===
    # Cliff tower at X in [-102..-80], Z in [-28..-6], rising to Y=82
    c.fill(-102, 38, -28, -82, 80, -10, "deepslate_bricks")
    c.fill(-101, 79, -27, -83, 81, -11, "stone_bricks")
    # Upper reservoir pool (Y=79..81)
    c.fill(-98, 79, -24, -85, 81, -14, "water", level="0")
    # Carve terraced front spillway on east face (X=-82..-78, Z=-22..-16)
    c.fill(-84, 80, -22, -80, 82, -16, "air")
    c.fill(-84, 80, -22, -82, 80, -16, "water", level="0")
    # Three-tier open waterfall plunge (level=8 falling columns into deep plunge pool at Y=62..44)
    c.fill(-81, 73, -22, -81, 80, -16, "water", level="8")
    c.fill(-81, 72, -22, -79, 72, -16, "polished_deepslate")  # mid cascade ledge
    c.fill(-80, 73, -22, -79, 73, -16, "water", level="1")
    c.fill(-78, 63, -22, -78, 73, -16, "water", level="8")    # main open drop hitting bay!
    # Glass-walled rapid flume along Z=-8..-4 descending smoothly from X=-84 (Y=72) to X=-62 (Y=63)
    for step in range(9):
        fx = -84 + step * 2
        fy = 71 - step
        c.fill(fx, 62, -9, fx + 1, fy - 1, -5, "stone_bricks")
        # Glass side walls (triggers P1 "water against glass" face mode!)
        c.fill(fx, fy, -9, fx + 1, fy + 2, -9, "glass")
        c.fill(fx, fy, -5, fx + 1, fy + 2, -5, "glass")
        # Flowing water inside channel (triggers P1 flow direction + exposed open sides)
        c.fill(fx, fy, -8, fx + 1, fy, -6, "water", level="0" if step == 0 else str(min(7, (step % 4) + 1)))
        if step > 0:
            c.fill(fx, fy + 1, -8, fx, fy + 1, -6, "water", level="8")

    # === 4C: Dry Glass Underwater Tunnel (Z in [9..15], X in [-92..-34], Y=42..47) ===
    # Dry stairs descending from east quay at X=-32..-29, Z=11..13
    for i in range(21):
        sy = y0 - i
        sx = -33 - i
        if sy < 43:
            break
        # Spiral/straight staircase inside dry shaft at X=-32..-29, Z=10..14
    # Build dry access shaft at east shore (X in [-35..-29], Z in [9..15], Y=42..64)
    c.hollow_box(-35, 42, 9, -29, y0 + 4, 15, "polished_deepslate", floor="polished_deepslate", ceiling="glass")
    c.fill(-29, y0 + 1, 11, -29, y0 + 3, 13, "air")  # entry from east walkway
    # Ladder + spiral step platforms down shaft
    for y in range(43, y0 + 1):
        c.set(-30, y, 12, "ladder", facing="west")
        if y % 6 == 0:
            c.set(-34, y, 12, "sea_lantern")
    # Glass-domed tunnel running west through the deep water (X in [-90..-35], Z in [10..14], Y=42..46)
    c.fill(-90, 42, 10, -35, 42, 14, "polished_deepslate")
    c.fill(-90, 42, 11, -35, 42, 13, "smooth_quartz")
    c.fill(-90, 43, 11, -35, 45, 13, "air")
    c.fill(-90, 43, 10, -35, 45, 10, "glass")
    c.fill(-90, 43, 14, -35, 45, 14, "glass")
    c.fill(-90, 46, 10, -35, 46, 14, "glass")
    # Structural stone ribs every 8 blocks with subtle copper trim
    for rx in range(-88, -36, 8):
        c.fill(rx, 43, 10, rx, 45, 10, "prismarine_bricks")
        c.fill(rx, 43, 14, rx, 45, 14, "prismarine_bricks")
        c.fill(rx, 46, 10, rx, 46, 14, "waxed_cut_copper")
        c.set(rx, 42, 12, "sea_lantern")
    # West end underwater observation rotunda (X in [-98..-90], Z in [8..16], Y=42..48)
    c.hollow_box(-98, 42, 8, -90, 48, 16, "glass", floor="polished_deepslate", ceiling="glass")
    c.fill(-90, 43, 11, -90, 45, 13, "air")
    c.place_sign(-36, 44, 12, ["[干式海底玻璃隧道]", "贴玻璃水体面态", "隔窗仰望水底焦散", "与沉没神殿柱廊"], facing="east")

    # === 4D: Immersive Sunken Temple (P2 Underwater Optics Core, Z in [18..50], depth 22-24 blocks) ===
    # 1. Underwater Caustics & God-Rays Colonnade (X in [-62..-40], Z in [22..48], floor Y=40)
    c.fill(-63, 38, 21, -39, 40, 49, "quartz_block")
    c.fill(-61, 40, 23, -41, 40, 47, "smooth_quartz")
    for px in (-60, -54, -48, -42):
        for pz in (24, 30, 36, 42, 46):
            if px in (-60, -42) or pz in (24, 46):
                c.pillar(px, 41, 53, pz, "quartz_pillar", "chiseled_quartz_block", "chiseled_quartz_block")
    # Open lattice pergola roof underwater (Y=54) so sun shafts slice through into god-rays & crisp caustics
    for px in range(-60, -41, 3):
        c.fill(px, 54, 24, px, 54, 46, "quartz_slab", type="top", waterlogged="true")
    for pz in range(24, 47, 6):
        c.fill(-60, 54, pz, -42, 54, pz, "waxed_cut_copper_slab", type="top", waterlogged="true")

    # 2. Snell's Window Viewing Platform (X in [-79..-65], Z in [24..42], deck at Y=52 -> 10 blocks under surface)
    # High-contrast Gold + Red + White mosaic below to reflect vividly via Total Internal Reflection outside Snell cone!
    c.fill(-79, 38, 24, -65, 51, 42, "prismarine_bricks")
    for x in range(-79, -64):
        for z in range(24, 43):
            ring = max(abs(x + 72), abs(z - 33))
            if ring % 3 == 0:
                blk = "gold_block"
            elif ring % 3 == 1:
                blk = "red_concrete"
            else:
                blk = "white_concrete"
            c.set(x, 52, z, blk)
    # Clear calm water above Snell platform all the way to Y=62
    c.fill(-79, 53, 24, -65, sea, 42, "water", level="0")

    # 3. Half-Submerged Viewing Steps at East Edge (X in [-39..-34], Z in [18..28])
    for i in range(6):
        sx = -34 - i
        sy = y0 - i
        c.fill(sx, 38, 19, sx, sy, 27, "smooth_quartz")
        if sy <= sea:
            c.fill(sx, sy + 1, 19, sx, sea, 27, "water", level="0")

    # 4. Underwater Dark Shipwreck Vault + Metals + Grate Lights + Dual Bubble Columns (X in [-102..-81], Z in [22..48], Y=38..50)
    # Thick opaque stone roof at Y=50 blocks sunlight -> dark underwater chamber!
    c.hollow_box(-102, 38, 22, -81, 50, 48, "deepslate_bricks", floor="polished_deepslate", ceiling="deepslate_bricks", interior="water")
    # Fill interior with source water cleanly
    c.fill(-101, 39, 23, -82, 49, 47, "water", level="0")
    # Broad underwater archway facing east toward Snell platform
    c.fill(-81, 39, 31, -81, 46, 39, "water", level="0")
    # Treasure mound: Gold, Fresh Copper, Iron, Oxidized Copper, Diamond blocks underwater
    c.fill(-96, 39, 32, -90, 40, 38, "gold_block")
    c.fill(-95, 41, 33, -91, 41, 37, "waxed_copper_block")
    c.fill(-94, 42, 34, -92, 42, 36, "iron_block")
    c.set(-93, 43, 35, "diamond_block")
    # Underwater voxel-grate lights (sea_lantern enclosed in waterlogged copper_grate & iron_bars!)
    for lz in (26, 35, 44):
        c.set(-100, 43, lz, "sea_lantern")
        c.set(-99, 43, lz, "waxed_copper_grate", waterlogged="true")
        c.set(-99, 44, lz, "iron_bars", waterlogged="true")
        c.set(-99, 42, lz, "iron_bars", waterlogged="true")
    # Dual Bubble Columns rising/sinking all the way from Y=39 up through openings to surface Y=62!
    # Carve two 3x3 vertical shafts in vault ceiling for dramatic underwater bubble vents
    # Soul Sand ascending bubble column at (-86, 39..62, 27)
    c.set(-86, 38, 27, "soul_sand")
    c.fill(-86, 39, 27, -86, sea, 27, "bubble_column", drag="false")
    c.fill(-87, 50, 26, -85, 50, 28, "water", level="0")
    # Magma Block descending bubble column at (-86, 39..62, 43)
    c.set(-86, 38, 43, "magma_block")
    c.fill(-86, 39, 43, -86, sea, 43, "bubble_column", drag="true")
    c.fill(-87, 50, 42, -85, 50, 44, "water", level="0")
    # Extra open-water twin bubble vents right near colonnade for daytime viewing
    c.set(-51, 40, 35, "soul_sand")
    c.fill(-51, 41, 35, -51, sea, 35, "bubble_column", drag="false")
    c.set(-45, 40, 35, "magma_block")
    c.fill(-45, 41, 35, -45, sea, 35, "bubble_column", drag="true")

    c.register_waypoint("4a_water_bay", -31.5, y0 + 1.0, -10.5, 90.0, 14.0, "区4A · 水陆光学湾与阶梯瀑布")
    c.register_waypoint("4c_tunnel", -42.5, 43.0, 12.5, 90.0, -8.0, "区4C · 干式海底玻璃观测隧道")
    c.register_waypoint("4d_underwater", -52.5, 44.0, 26.5, 25.0, -6.0, "区4D · 沉没神殿焦散柱廊与气泡柱")
    c.register_waypoint("4d_snell", -72.0, 53.5, 33.0, 0.0, -58.0, "区4D · 斯涅尔窗口全内反射仰望台")
    c.register_waypoint("4d_vault", -83.5, 42.5, 35.5, 90.0, 2.0, "区4D · 深水暗室金属与格栅光")


def build_zone5_cathedral(c: VoxelCanvas) -> None:
    """Zone 5: Crystal Refraction Dome & Stained Glass Cathedral (X in [-30, 30], Z in [42, 102], Y=64..102)."""
    y0 = MAIN_FLOOR_Y  # 64

    # Grand stone plinth & reflective dark/white nave floor
    c.fill(-28, y0, 42, 28, y0, 100, "polished_deepslate")
    # Checkerboard mirror-quartz & obsidian central nave aisle (continues the anisotropic avenue!)
    for z in range(42, 99):
        for x in range(-6, 7):
            if abs(x) == 6:
                c.set(x, y0, z, "waxed_cut_copper")
            elif (x + z) % 2 == 0:
                c.set(x, y0, z, "smooth_quartz")
            else:
                c.set(x, y0, z, "obsidian")

    # Outer cathedral walls with tall stained-glass Gothic lancet windows (Y=65..82)
    stained_palette = [
        "red_stained_glass", "orange_stained_glass", "yellow_stained_glass",
        "lime_stained_glass", "cyan_stained_glass", "blue_stained_glass",
        "purple_stained_glass", "magenta_stained_glass",
    ]
    # North facade (Z=43) with grand portal arch
    c.fill(-26, y0 + 1, 43, 26, y0 + 18, 43, "stone_bricks")
    c.arch_x(-6, 6, y0 + 1, 9, 43, "quartz_pillar", carve_air=True)
    # Rose window above north entrance (Z=43, center (0, 80))
    for dx in range(-4, 5):
        for dy in range(-4, 5):
            r = math.hypot(dx, dy)
            if r <= 4.2:
                ang = int(((math.atan2(dy, dx) + math.pi) / (2 * math.pi)) * len(stained_palette)) % len(stained_palette)
                blk = "waxed_copper_block" if abs(r - 4.0) < 0.55 else stained_palette[ang]
                c.set(dx, 80 + dy, 43, blk)

    # South apse wall (Z=99) with towering multi-hue stained glass screen
    c.fill(-26, y0 + 1, 99, 26, y0 + 18, 99, "stone_bricks")
    for idx, wx in enumerate(range(-21, 22, 6)):
        gblk = stained_palette[idx % len(stained_palette)]
        c.fill(wx - 1, y0 + 3, 99, wx + 2, y0 + 16, 99, gblk)
        c.arch_x(wx - 2, wx + 3, y0 + 3, 11, 99, "quartz_pillar", carve_air=False)

    # East & West side walls (X=-26 and X=26, Z=44..98) with tall lancets & buttresses
    for side_x in (-26, 26):
        c.fill(side_x, y0 + 1, 44, side_x, y0 + 18, 98, "stone_bricks")
        for idx, wz in enumerate(range(48, 95, 6)):
            gblk = stained_palette[idx % len(stained_palette)]
            c.fill(side_x, y0 + 3, wz - 1, side_x, y0 + 15, wz + 1, gblk)
            # Exterior flying buttress piers
            out_x = side_x + (1 if side_x > 0 else -1)
            c.fill(out_x, y0 + 1, wz - 3, out_x + (1 if side_x > 0 else -1), y0 + 16, wz - 3, "deepslate_bricks")

    # Roof & soaring central Crystal Refraction Dome (center (0, 71), Y=82..100, R=18)
    c.fill(-26, y0 + 19, 43, 26, y0 + 19, 99, "deepslate_tiles")
    # Open circular oculus under the dome
    for dx in range(-18, 19):
        for dz in range(-18, 19):
            if math.hypot(dx, dz) < 17.2:
                c.set(dx, y0 + 19, 71 + dz, "air")
    c.dome(0, y0 + 19, 71, 18.0, "glass", hollow=True, thickness=1.15, height_scale=0.95, rib_block="waxed_copper_block")
    # Clear 3x3 crown opening at apex for beacon beam!
    c.fill(-1, y0 + 34, 70, 1, y0 + 39, 72, "air")

    # Nave arcade columns supporting the dome
    for sx in (-12, 12):
        for sz in (53, 62, 71, 80, 89):
            c.pillar(sx, y0 + 1, y0 + 18, sz, "quartz_pillar", "polished_deepslate", "waxed_chiseled_copper")

    # === Multi-layer Glass, Tinted Glass & Ice Refraction Galleries (West & East side aisles) ===
    # West aisle (X in [-24..-14], Z in [52..90]): Layered Glass / Stained Glass / Tinted Glass / Ice before calibration grids
    refract_layers = [
        (55, "glass", "无色透明玻璃 IOR1.08"),
        (61, "tinted_glass", "遮光玻璃(透视隔光)"),
        (67, "light_blue_stained_glass", "淡蓝染色玻璃"),
        (73, "magenta_stained_glass", "品红染色玻璃"),
        (79, "ice", "普通冰晶折射"),
        (85, "blue_ice", "蓝冰光滑反射"),
    ]
    for lz, mat, lbl in refract_layers:
        # High-contrast calibration target wall behind the glass
        for dy in range(1, 7):
            for dx in range(-23, -19):
                c.set(dx, y0 + dy, lz + 2, "white_concrete" if (dx + dy) % 2 == 0 else "black_concrete")
        c.set(-21, y0 + 4, lz + 1, "end_rod", facing="south")
        # Glass / Ice refractive screen in front
        c.fill(-23, y0 + 1, lz - 1, -19, y0 + 6, lz - 1, mat)
        c.place_sign(-18, y0 + 2, lz - 1, [lbl, "观察折射扭曲", "与掠射角菲涅尔"], facing="east")

    # East aisle (X in [14..24], Z in [52..90]): Dynamic Texture & Particle Wall (Triple Gothic Water Curtain vs Lava Curtain!)
    # Water curtain box (Z in [56..68]): dark prismarine backing + single crisp glass-facing water sheet in 3 gothic bays
    c.fill(20, y0 + 1, 56, 24, y0 + 11, 68, "deepslate_bricks")
    c.fill(22, y0 + 1, 57, 23, y0 + 10, 67, "dark_prismarine")
    for wz_a, wz_b in ((57, 59), (61, 63), (65, 67)):
        c.fill(21, y0 + 1, wz_a, 21, y0 + 10, wz_b, "water", level="8")
        c.fill(21, y0 + 10, wz_a, 21, y0 + 10, wz_b, "water", level="0")
        c.fill(20, y0 + 1, wz_a, 20, y0 + 10, wz_b, "glass")
    for mullion_z in (60, 64):
        c.fill(20, y0 + 1, mullion_z, 21, y0 + 10, mullion_z, "polished_deepslate")
    # Lava curtain box (Z in [74..86]): blackstone backing + single crisp glass-facing lava sheet in 3 gothic bays (90 emitters vs 330!)
    c.fill(20, y0 + 1, 74, 24, y0 + 11, 86, "deepslate_bricks")
    c.fill(22, y0 + 1, 75, 23, y0 + 10, 85, "polished_blackstone_bricks")
    for lz_a, lz_b in ((75, 77), (79, 81), (83, 85)):
        c.fill(21, y0 + 1, lz_a, 21, y0 + 10, lz_b, "lava", level="8")
        c.fill(21, y0 + 10, lz_a, 21, y0 + 10, lz_b, "lava", level="0")
        c.fill(20, y0 + 1, lz_a, 20, y0 + 10, lz_b, "glass")
    for mullion_z in (78, 82):
        c.fill(20, y0 + 1, mullion_z, 21, y0 + 10, mullion_z, "obsidian")
    c.place_sign(19, y0 + 2, 62, ["[水幕动画对照]", "AtlasPatch实时帧", "三联贴玻璃水体"], facing="west")
    c.place_sign(19, y0 + 2, 80, ["[岩浆幕动画对照]", "炽橙热辐射自发光", "三联隔玻璃折射"], facing="west")

    # === Central Dynamic Texture & Particle Altar (Z = 67..78, X = -6..6) ===
    # Elevated altar dais
    c.fill(-7, y0 + 1, 66, 7, y0 + 1, 78, "polished_deepslate")
    c.fill(-5, y0 + 1, 68, 5, y0 + 1, 76, "gold_block")
    # Center Beacon firing straight through the dome oculus into the sky!
    c.place_beacon(0, y0 + 2, 71, pyramid_block="iron_block")
    # Lit Nether Portal right behind beacon (Z=76, X in [-3..3], Y=66..73)
    c.fill(-3, y0 + 2, 76, 3, y0 + 9, 76, "obsidian")
    c.fill(-2, y0 + 3, 76, 2, y0 + 8, 76, "nether_portal", axis="x")
    # Twin Campfires (Normal + Soul) with smoke particles + Enchanting Table flanked by bookshelves
    c.set(-5, y0 + 2, 69, "campfire", facing="north", lit="true", signal_fire="true", waterlogged="false")
    c.set(5, y0 + 2, 69, "soul_campfire", facing="north", lit="true", signal_fire="true", waterlogged="false")
    c.set(0, y0 + 2, 67, "enchanting_table")
    for bx in (-2, 2):
        c.fill(bx, y0 + 2, 67, bx, y0 + 3, 67, "bookshelf")

    # Path Tracing Benchmark Tripod Platform (Ctrl+Home 256 SPP hero camera)
    c.fill(-2, y0 + 1, 49, 2, y0 + 1, 51, "waxed_cut_copper")
    c.place_sign(0, y0 + 2, 52, ["[Ctrl+Home 路径追踪]", "256 SPP 基准机位", "正对信标/传送门/穹顶", "可与实时Lumen对比"], facing="north", color="yellow")

    c.register_waypoint("5_cathedral", 0.5, y0 + 2.0, 50.5, 0.0, -6.0, "区5 · 水晶折光穹顶与路径追踪机位")


def build_sky_overview_ring(c: VoxelCanvas) -> None:
    """Elevated glass & copper observation ring at Y=96 surrounding the central tower for panoramic vistas."""
    ry = 96
    for dx in range(-28, 29):
        for dz in range(-28, 29):
            dist = math.hypot(dx, dz)
            if 22.0 <= dist <= 26.0:
                if abs(dist - 22.3) < 0.6 or abs(dist - 25.7) < 0.6:
                    c.set(dx, ry, dz, "waxed_cut_copper")
                    c.set(dx, ry + 1, dz, "glass_pane")
                else:
                    c.set(dx, ry, dz, "glass")
    # Four radial sky bridges linking central clocktower deck (radius 5) to the outer ring (radius 22)
    for d in range(6, 23):
        for w in (-1, 0, 1):
            blk = "waxed_cut_copper" if w == 0 else "glass"
            c.set(d, ry, w, blk)
            c.set(-d, ry, w, blk)
            c.set(w, ry, d, blk)
            c.set(w, ry, -d, blk)
        for edge in (-2, 2):
            c.set(d, ry, edge, "waxed_cut_copper")
            c.set(d, ry + 1, edge, "iron_bars")
            c.set(-d, ry, edge, "waxed_cut_copper")
            c.set(-d, ry + 1, edge, "iron_bars")
            c.set(edge, ry, d, "waxed_cut_copper")
            c.set(edge, ry + 1, d, "iron_bars")
            c.set(edge, ry, -d, "waxed_cut_copper")
            c.set(edge, ry + 1, -d, "iron_bars")

    c.register_waypoint("sky_overview", 23.5, ry + 1.0, 23.5, 135.0, 26.0, "高空环廊 · 全图光棱圣殿鸟瞰")
