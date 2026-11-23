"""Main builder (Part 2): Zone 2 (Voxel Light & Grate Hall), Zone 3 (Lumen GI & Blackbox Lab), Zone 6 (Extreme Stress Dungeon)."""

from __future__ import annotations

import math
from voxel_canvas import MAIN_FLOOR_Y, VoxelCanvas


def build_zone2_voxel_light_hall(c: VoxelCanvas) -> None:
    """Zone 2: Enclosed Dark Voxel-Light & Fence/Grate Shadow Hall (X in [-36, 36], Z in [-102, -36], Y=64..84)."""
    y0 = MAIN_FLOOR_Y  # 64

    # 1. Light-trap Z-shaped entry tunnel from Plaza North Gate (Z=-26..-36) so ZERO sunlight leaks inside
    c.hollow_box(-7, y0, -37, 11, y0 + 7, -26, "deepslate_bricks", floor="polished_deepslate", ceiling="deepslate_tiles")
    # Entry opening on south side (X in [-3..3], Z=-26)
    c.fill(-3, y0 + 1, -26, 3, y0 + 5, -26, "air")
    # Internal baffle wall forcing a dog-leg turn (Z=-31, blocks X in [-6..4], leaves east passage X in [5..10])
    c.fill(-6, y0 + 1, -31, 4, y0 + 6, -31, "black_concrete")
    # Second baffle wall (Z=-34, blocks X in [0..10], leaves west passage X in [-6..-1])
    c.fill(0, y0 + 1, -34, 10, y0 + 6, -34, "black_concrete")
    # Exit opening into Zone 2A hall at X in [-6..-1], Z=-37..-36
    c.fill(-6, y0 + 1, -37, -1, y0 + 5, -36, "air")
    # Subtle floor guiding end-rods inside light-trap
    c.set(8, y0 + 1, -29, "soul_lantern", hanging="false")
    c.set(-4, y0 + 1, -35, "soul_lantern", hanging="false")
    c.place_sign(-4, y0 + 3, -25, ["[区2 · 体素光遮罩大殿]", "Z型遮光暗道入内", "零天光纯人造光环境", "体验自动曝光过渡"], facing="south")

    # 2. Outer dark shell of Zone 2 (double-walled ceiling to guarantee zero skylight bleed)
    c.hollow_box(-36, y0, -102, 36, y0 + 19, -36, "deepslate_bricks", floor="polished_deepslate", ceiling="black_concrete")
    c.fill(-36, y0 + 20, -102, 36, y0 + 20, -36, "deepslate_tiles")  # exterior roof cap
    # Re-open entry doorway from light-trap
    c.fill(-6, y0 + 1, -37, -1, y0 + 5, -36, "air")

    # =========================================================================
    # 2A. Voxel Fence / Grate / Trapdoor Mask Shadow Corridor (Z in [-58, -37], X in [-35, 35], H=10)
    # =========================================================================
    # Separate 2A ceiling at Y=75 (y0+11) and partition wall at Z=-59 with wide central arch into 2B/2C/2D
    c.fill(-35, y0 + 11, -59, 35, y0 + 11, -37, "black_concrete")
    c.fill(-35, y0 + 1, -59, 35, y0 + 11, -59, "black_concrete")
    # Open central & wing portals northward at Z=-59
    for px in (-24, 0, 24):
        c.fill(px - 3, y0 + 1, -59, px + 3, y0 + 6, -59, "air")

    # Matte white concrete projection floor strips + polished deepslate center aisle so both shadow & reflection read clearly!
    c.fill(-34, y0, -58, 34, y0, -37, "white_concrete")
    c.fill(-34, y0, -50, 34, y0, -46, "polished_deepslate")

    # 10 recessed COVERED_FACE light bays on SOUTH wall (Z=-37, shining NORTH) + 10 on NORTH wall (Z=-58, shining SOUTH)
    # Every bay puts a full-block emitter at wall depth + a cover block immediately in front -> triggers RectLight COVERED_FACE!
    south_bays = [
        (-31, "sea_lantern", "iron_bars", {}, "铁栏杆 + 海晶灯"),
        (-24, "glowstone", "waxed_copper_bars", {}, "铜栏杆 + 萤石"),
        (-17, "pearlescent_froglight", "waxed_copper_grate", {}, "鲜铜格栅 + 蛙明灯"),
        (-10, "waxed_copper_bulb", "waxed_oxidized_copper_grate", {"_emit": {"lit": "true"}}, "氧化格栅 + 铜灯泡"),
        (3, "glowstone", "oak_trapdoor", {"facing": "north", "half": "bottom", "open": "true"}, "橡木窗格活板门"),
        (10, "redstone_lamp", "dark_oak_trapdoor", {"_emit": {"lit": "true"}, "facing": "north", "half": "bottom", "open": "true"}, "深色百叶活板门"),
        (17, "sea_lantern", "iron_trapdoor", {"facing": "north", "half": "bottom", "open": "true"}, "铁活板门孔阵"),
        (24, "ochre_froglight", "waxed_copper_trapdoor", {"facing": "north", "half": "bottom", "open": "true"}, "铜活板门窗格"),
        (31, "sea_lantern", None, {}, "对照组: 裸露无遮罩"),
    ]
    north_bays = [
        (-31, "jack_o_lantern", "oak_fence", {}, "橡木栅栏 + 南瓜灯"),
        (-24, "glowstone", "nether_brick_fence", {}, "下界砖栅栏投影"),
        (-17, "redstone_lamp", "oak_fence_gate", {"_emit": {"lit": "true"}, "facing": "south", "open": "false", "in_wall": "false"}, "木栅栏门投影"),
        (-10, "sea_lantern", "ladder", {"facing": "south"}, "梯子横档光栅"),
        (10, "glowstone", "deepslate_brick_stairs", {"facing": "north", "half": "top", "shape": "straight"}, "倒置楼梯狭缝光"),
        (17, "pearlescent_froglight", "iron_chain", {"axis": "y"}, "铁锁链垂帘投影"),
        (24, "verdant_froglight", "waxed_copper_chain", {"axis": "y"}, "铜锁链垂帘投影"),
        (31, "glowstone", None, {}, "对照组: 裸露萤石"),
    ]

    def build_masked_bay(bx: int, wall_z: int, inward_dz: int, emit_blk: str, cover_blk: str | None, opts: dict, title: str) -> None:
        emit_props = dict(opts.get("_emit", {}))
        cover_props = {k: v for k, v in opts.items() if k != "_emit"}
        # Matte white back-wall baffle dividers flanking each bay (width 5: bx-2..bx+2)
        for dx in (-3, 3):
            c.fill(bx + dx, y0 + 1, wall_z, bx + dx, y0 + 8, wall_z + inward_dz * 3, "gray_concrete")
        # 3x3 recessed emitter array (or 2x2 centered at y0+2..y0+4, bx-1..bx+1)
        # Keep only 1 exposed face (inward_dz) by sealing back/sides/top/bottom with black_concrete
        back_z = wall_z - inward_dz
        c.fill(bx - 2, y0 + 1, back_z, bx + 2, y0 + 7, back_z, "black_concrete")
        c.fill(bx - 2, y0 + 1, wall_z, bx + 2, y0 + 7, wall_z, "black_concrete")
        # Place emitter column (2x2 for crisp shadow & controlled light count = 4 emitters per bay)
        for ex in (bx, bx + 1):
            for ey in (y0 + 3, y0 + 4):
                c.set(ex, ey, wall_z, emit_blk, **emit_props)
                if cover_blk is not None:
                    c.set(ex, ey, wall_z + inward_dz, cover_blk, **cover_props)
        # Frame around the light opening in polished deepslate
        for fx in range(bx - 1, bx + 3):
            c.set(fx, y0 + 2, wall_z + inward_dz, "polished_deepslate")
            c.set(fx, y0 + 5, wall_z + inward_dz, "polished_deepslate")
        for fy in (y0 + 3, y0 + 4):
            c.set(bx - 1, fy, wall_z + inward_dz, "polished_deepslate")
            c.set(bx + 2, fy, wall_z + inward_dz, "polished_deepslate")
        # White projection target panel across from the opening on the divider & label sign
        sign_face = "north" if inward_dz < 0 else "south"
        c.place_sign(bx, y0 + 2, wall_z + inward_dz * 2, ["[体素光遮罩]", title, "COVERED_FACE", "RectLight光追投影"], facing=sign_face)

    for bx, eb, cb, opts, lbl in south_bays:
        build_masked_bay(bx, -37, -1, eb, cb, opts, lbl)
    for bx, eb, cb, opts, lbl in north_bays:
        build_masked_bay(bx, -58, +1, eb, cb, opts, lbl)

    # =========================================================================
    # Partition North Half (Z in [-101, -60]) into:
    #   2C West Wing (X in [-35, -13]): 8 Color Temperatures + RGB Colored Shadow Hall + Copper Bulb Dimmer
    #   2D Center Rotunda (X in [-12, 12]): Soaring 3-Tier Concentric Grate/Chain Lantern Tower (H=19!)
    #   2B East Wing (X in [13, 35]): Flush single-face IES Emitters & Directional Furnace Fronts
    # =========================================================================
    c.fill(-13, y0 + 1, -101, -13, y0 + 19, -60, "black_concrete")
    c.fill(13, y0 + 1, -101, 13, y0 + 19, -60, "black_concrete")
    # Arch doorways linking 2D Center Rotunda to 2C (West) and 2B (East)
    c.fill(-13, y0 + 1, -83, -13, y0 + 6, -77, "air")
    c.fill(13, y0 + 1, -83, 13, y0 + 6, -77, "air")
    # Also connect West wall of Zone 2C (X=-36, Z in [-82..-78]) to Zone 3 (Lumen GI Lab) via dark corridor!
    c.fill(-40, y0, -83, -36, y0 + 7, -77, "deepslate_bricks")
    c.fill(-40, y0 + 1, -82, -36, y0 + 5, -78, "air")
    c.fill(-40, y0, -82, -36, y0, -78, "polished_deepslate")

    # === 2B. Flush Single-Face IES & Directional Furnace Hall (East Wing: X in [14, 35], Z in [-101, -60]) ===
    c.fill(14, y0, -101, 35, y0, -60, "polished_deepslate")
    # Flush single-face floor emitters (each surrounded on 5 sides by opaque solid blocks -> pure FlushIes!)
    flush_strips = [
        (17, "magma_block", "岩浆块局部缝隙发光"),
        (21, "crying_obsidian", "哭泣黑曜石紫纹发光"),
        (25, "sea_lantern", "单面海晶灯掠射光"),
        (29, "lava", "平铺单面岩浆槽"),
    ]
    for fx, fblk, flbl in flush_strips:
        # Surround sides underneath with deepslate so strictly 1 top face is exposed
        c.fill(fx - 1, y0 - 1, -96, fx + 1, y0 - 1, -66, "deepslate_bricks")
        for fz in range(-94, -68, 4):
            c.set(fx, y0, fz, fblk, **({"level": "0"} if fblk == "lava" else {}))
            c.set(fx, y0, fz + 1, fblk, **({"level": "0"} if fblk == "lava" else {}))
        c.place_sign(fx, y0 + 1, -65, ["[FlushIes单面]", flbl, "暗壳锐利不洗白"], facing="south")
    # Directional Furnace / Blast Furnace / Smoker Wall along East Wall (X=34, Z in [-96..-66])
    c.fill(35, y0 + 1, -98, 35, y0 + 6, -64, "deepslate_bricks")
    furnace_kinds = ["furnace", "blast_furnace", "smoker"]
    for i, fz in enumerate(range(-94, -67, 3)):
        fk = furnace_kinds[i % 3]
        c.place_lit_furnace(34, y0 + 2, fz, kind=fk, facing="west")
        c.set(34, y0 + 1, fz, "polished_deepslate")
        c.set(34, y0 + 3, fz, "deepslate_bricks")
    # Projection floor apron in front of furnaces
    c.fill(31, y0, -96, 33, y0, -66, "smooth_quartz")
    c.place_sign(33, y0 + 4, -80, ["[定向炉口光]", "熔炉/高炉/烟熏炉", "仅朝向正面发光"], facing="west")

    # === 2C. 8 Color-Temperature Alcoves + RGB Colored Shadow Rotunda + Copper Bulb Dimmer (X in [-35, -14], Z in [-101, -60]) ===
    c.fill(-35, y0, -101, -14, y0, -60, "gray_concrete")
    # 8 separated color-temp booths along West Wall (X=-34..-30) and North Wall (Z=-100..-96)
    color_booths_west = [
        (-92, "soul_lantern", {"hanging": "true"}, "soul_torch", {}, "1.灵魂冰蓝 #63DFFF"),
        (-84, "sea_lantern", {}, "end_rod", {"facing": "up"}, "2.极冷青白 #D9F4FF"),
        (-74, "waxed_copper_lantern", {"hanging": "true"}, "copper_torch", {}, "3.铜翠荧光 #8DFFAC"),
        (-66, "crying_obsidian", {}, "respawn_anchor", {"charges": "4"}, "4.幽暗深紫 #9315FF"),
    ]
    for bz, top_l, top_p, bot_l, bot_p, lbl in color_booths_west:
        c.fill(-35, y0 + 1, bz - 3, -30, y0 + 7, bz - 3, "black_concrete")
        c.fill(-35, y0 + 1, bz + 3, -30, y0 + 7, bz + 3, "black_concrete")
        c.fill(-35, y0 + 1, bz - 2, -35, y0 + 7, bz + 2, "white_concrete")
        c.fill(-34, y0, bz - 2, -30, y0, bz + 2, "white_concrete")
        c.set(-33, y0 + 5, bz, top_l, **top_p)
        c.set(-33, y0 + 1, bz, bot_l, **bot_p)
        c.set(-32, y0 + 1, bz - 1, "iron_block")  # metallic specular swatch inside each color booth
        c.place_sign(-30, y0 + 2, bz + 2, ["[8色温谱系]", lbl, "白墙+金属色偏对照"], facing="east")

    color_booths_north = [
        (-31, "amethyst_cluster", {"facing": "up"}, "amethyst_block", {}, "5.水晶淡紫 #B58CFF"),
        (-26, "redstone_torch", {}, "redstone_ore", {}, "6.警报深红 #FF4433"),
        (-21, "campfire", {"lit": "true", "facing": "south", "signal_fire": "false", "waterlogged": "false"}, "magma_block", {}, "7.炽热橘橙 #FF692D"),
        (-16, "lantern", {"hanging": "true"}, "glowstone", {}, "8.经典暖金 #FFC474"),
    ]
    for bx, l1, p1, l2, p2, lbl in color_booths_north:
        c.fill(bx - 2, y0 + 1, -101, bx - 2, y0 + 7, -96, "black_concrete")
        c.fill(bx + 2, y0 + 1, -101, bx + 2, y0 + 7, -96, "black_concrete")
        c.fill(bx - 1, y0 + 1, -101, bx + 1, y0 + 7, -101, "white_concrete")
        c.fill(bx - 1, y0, -100, bx + 1, y0, -96, "white_concrete")
        c.set(bx, y0 + 1, -99, l2, **p2)
        c.set(bx, y0 + 2, -99, l1, **p1)
        c.set(bx + 1, y0 + 1, -98, "gold_block")
        c.place_sign(bx, y0 + 4, -96, ["[8色温谱系]", lbl], facing="south")

    # Central RGB Tri-Color Colored Shadow Sculpture (X=-23, Z=-78)
    c.fill(-27, y0, -83, -19, y0, -73, "white_concrete")
    # Central white quartz & gold obelisk that casts 3 distinct Red/Green/Blue tinted raytraced shadows!
    c.fill(-23, y0 + 1, -78, -23, y0 + 5, -78, "white_concrete")
    c.set(-23, y0 + 3, -78, "gold_block")
    # Tri-color light pedestals placed in an equilateral triangle around (-23, -78)
    c.set(-26, y0 + 1, -75, "polished_deepslate")
    c.set(-26, y0 + 2, -75, "redstone_torch")      # Red
    c.set(-20, y0 + 1, -75, "polished_deepslate")
    c.set(-20, y0 + 2, -75, "copper_torch")        # Green
    c.set(-23, y0 + 1, -82, "polished_deepslate")
    c.set(-23, y0 + 2, -82, "soul_torch")          # Blue
    c.place_sign(-23, y0 + 1, -72, ["[RGB三色阴影]", "红石+铜+灵魂火把", "MegaLights光追", "三色分离投影"], facing="south")

    # Interactive 4-Oxidation Copper Bulb Dimmer Wall (X=-14, Z in [-92..-85]) -> Levels 15, 12, 8, 4!
    bulbs = [
        (-92, "waxed_copper_bulb", "15级全亮"),
        (-90, "waxed_exposed_copper_bulb", "12级中亮"),
        (-88, "waxed_weathered_copper_bulb", "8级柔光"),
        (-86, "waxed_oxidized_copper_bulb", "4级微光"),
    ]
    for bz, bblk, blbl in bulbs:
        c.set(-14, y0 + 3, bz, bblk, lit="true", powered="false")
        c.set(-14, y0 + 2, bz, "polished_deepslate")
        c.set(-15, y0 + 2, bz, "lever", face="wall", facing="west", powered="false")
        c.place_sign(-15, y0 + 4, bz, [blbl, "拉杆实时开关"], facing="west")

    # === 2D. Soaring 3-Tier Concentric Grate & Chain Lantern Tower (X in [-12, 12], Z in [-101, -60], Y=64..83) ===
    tcx, tcz = 0, -80
    # Smooth white & obsidian concentric floor rings to catch intricate lattice shadows & reflections
    for dx in range(-11, 12):
        for dz in range(-18, 19):
            dist = math.hypot(dx, dz)
            if dist < 5.0:
                blk = "gold_block" if int(dist) % 2 == 0 else "polished_deepslate"
            elif dist < 10.5:
                blk = "white_concrete" if (dx + dz) % 2 == 0 else "smooth_quartz"
            else:
                blk = "polished_deepslate"
            c.set(tcx + dx, y0, tcz + dz, blk)
    # Matte white perimeter walls in 2D to showcase dramatic projected lattice shadow patterns across 18 blocks height!
    c.fill(-12, y0 + 1, -101, 12, y0 + 18, -101, "white_concrete")
    c.fill(-12, y0 + 1, -101, -12, y0 + 18, -60, "white_concrete")
    c.fill(12, y0 + 1, -101, 12, y0 + 18, -60, "white_concrete")
    # Re-open doorways
    c.fill(-13, y0 + 1, -83, -12, y0 + 6, -77, "air")
    c.fill(12, y0 + 1, -83, 13, y0 + 6, -77, "air")

    # Concentric 3-tier lantern tower at (0, -80), Y=65..81:
    # Inner core (r=0..1): vertical column of staggered multi-hue point lights (Sea Lantern, Soul Lantern, Copper Lantern, End Rods)
    c.fill(tcx, y0 + 1, tcz, tcx, y0 + 18, tcz, "iron_chain", axis="y")
    for ly, lblk, lprops in (
        (y0 + 3, "sea_lantern", {}),
        (y0 + 7, "waxed_copper_bulb", {"lit": "true", "powered": "false"}),
        (y0 + 11, "glowstone", {}),
        (y0 + 15, "pearlescent_froglight", {}),
    ):
        c.set(tcx, ly, tcz, lblk, **lprops)
    for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        c.set(tcx + dx, y0 + 5, tcz + dz, "soul_lantern", hanging="true")
        c.set(tcx + dx, y0 + 9, tcz + dz, "waxed_copper_lantern", hanging="true")
        c.set(tcx + dx, y0 + 13, tcz + dz, "lantern", hanging="true")
        c.set(tcx + dx, y0 + 10, tcz + dz, "iron_chain", axis="y")
        c.set(tcx + dx, y0 + 6, tcz + dz, "iron_chain", axis="y")
        c.set(tcx + dx, y0 + 14, tcz + dz, "iron_chain", axis="y")

    # Middle cage (r=2 square, Y=66..80): alternating waxed_copper_grate & iron_bars creating dense penumbra
    for y in range(y0 + 2, y0 + 17):
        for dx in range(-2, 3):
            for dz in range(-2, 3):
                if abs(dx) == 2 or abs(dz) == 2:
                    if abs(dx) == 2 and abs(dz) == 2:
                        c.set(tcx + dx, y, tcz + dz, "waxed_copper_block" if y % 3 == 0 else "waxed_copper_chain", **({} if y % 3 == 0 else {"axis": "y"}))
                    elif (dx + dz + y) % 2 == 0:
                        c.set(tcx + dx, y, tcz + dz, "waxed_copper_grate")
                    else:
                        c.set(tcx + dx, y, tcz + dz, "iron_bars")

    # Outer frame (r=4 rotated diamond/octagonal columns & hanging trapdoor ribs, Y=65..82)
    for dx, dz in ((4, 0), (-4, 0), (0, 4), (0, -4), (3, 3), (-3, 3), (3, -3), (-3, -3)):
        for y in range(y0 + 1, y0 + 18):
            if y % 4 == 0:
                c.set(tcx + dx, y, tcz + dz, "waxed_cut_copper")
            elif y % 2 == 0:
                c.set(tcx + dx, y, tcz + dz, "waxed_copper_bars")
            else:
                c.set(tcx + dx, y, tcz + dz, "iron_chain", axis="y")

    c.place_sign(0, y0 + 2, -62, ["[2D · 同心镂空灯塔]", "三层格栅/栏杆/锁链", "四壁交错软硬半影", "体素光追核心地标"], facing="south", color="yellow")

    c.register_waypoint("2a_grate_hall", -16.5, y0 + 1.0, -48.5, -78.0, 2.0, "区2A · 栅栏/铜格栅体素光投影长廊")
    c.register_waypoint("2b_flush_ies", 23.5, y0 + 1.0, -64.5, 180.0, 8.0, "区2B · 单面嵌入式IES与定向炉口光")
    c.register_waypoint("2c_colors", -23.5, y0 + 1.0, -66.5, 180.0, 4.0, "区2C · 8色温谱系与RGB彩色阴影")
    c.register_waypoint("2d_lantern_tower", 0.5, y0 + 2.0, -64.5, 180.0, -10.0, "区2D · 三层同心镂空灯笼塔大殿")


def build_zone3_lumen_gi_lab(c: VoxelCanvas) -> None:
    """Zone 3: Lumen GI Cornell Boxes, 90-deg Bounce Tunnel, Infinity Mirror Corridor, Deep Slit Shaft (X in [-100, -40], Z in [-100, -40])."""
    y0 = MAIN_FLOOR_Y  # 64

    # Outer dark concrete enclosure for Zone 3
    c.hollow_box(-100, y0, -100, -40, y0 + 15, -40, "deepslate_bricks", floor="polished_deepslate", ceiling="black_concrete")
    c.fill(-100, y0 + 16, -100, -40, y0 + 16, -40, "deepslate_tiles")
    # Re-open connecting corridor from Zone 2C at X=-40, Z in [-82..-78]
    c.fill(-41, y0 + 1, -82, -39, y0 + 5, -78, "air")

    # Central viewing concourse running East-West at Z in [-64..-59], X in [-98..-42] and North-South at X in [-45..-41], Z in [-96..-58]
    c.fill(-98, y0, -64, -41, y0, -59, "polished_deepslate")
    c.fill(-45, y0, -96, -41, y0, -59, "polished_deepslate")

    # =========================================================================
    # 3A. Three Classic Cornell Boxes along South Edge (Z in [-57..-42], X in [-96..-44])
    # =========================================================================
    boxes = [
        (-58, -44, "red_wool", "cyan_wool", True, "康奈尔盒1: 自然天窗"),
        (-77, -63, "red_wool", "lime_wool", False, "康奈尔盒2: 海晶面光"),
        (-96, -82, "orange_concrete", "blue_concrete", True, "康奈尔盒3: 橙蓝材质"),
    ]
    for x1, x2, left_w, right_w, skylight, title in boxes:
        cx = (x1 + x2) // 2
        # Box walls (Y=64..76): matte white floor/ceiling/back, saturated colored side walls
        c.hollow_box(x1, y0, -57, x2, y0 + 12, -42, "white_concrete", floor="white_concrete", ceiling="white_concrete")
        c.fill(x1, y0 + 1, -56, x1, y0 + 11, -43, left_w)
        c.fill(x2, y0 + 1, -56, x2, y0 + 11, -43, right_w)
        # Wide open viewing front on North wall (Z=-57)
        c.fill(x1 + 2, y0 + 1, -57, x2 - 2, y0 + 9, -57, "air")
        # Light source on ceiling (center 4x4)
        if skylight:
            # Punch straight through outer roof so real sunlight enters!
            c.fill(cx - 2, y0 + 12, -51, cx + 2, y0 + 18, -47, "air")
        else:
            c.fill(cx - 1, y0 + 12, -50, cx + 1, y0 + 12, -48, "sea_lantern")
        # Classic stepped blocks inside the Cornell Box: tall matte white prism + low specular gold/iron/obsidian cube!
        c.fill(cx - 4, y0 + 1, -48, cx - 2, y0 + 6, -46, "white_concrete")
        c.fill(cx + 1, y0 + 1, -51, cx + 3, y0 + 3, -49, "gold_block")
        c.fill(cx + 2, y0 + 1, -46, cx + 3, y0 + 4, -45, "iron_block" if skylight else "blue_ice")
        c.place_sign(cx, y0 + 2, -58, ["[Lumen GI色溢]", title, "白块染上两侧彩光", "切软件路径做对比"], facing="north")

    # =========================================================================
    # 3B. Multi-Bounce Mirror Corridor & 90-Degree Indirect-Only Bounce Tunnel (X in [-69..-46], Z in [-98..-66])
    # =========================================================================
    # 1) Three Parallel Infinity Mirror Corridors (Gold, Iron, Obsidian) running North-South (Z in [-97..-67])
    mirror_specs = [
        (-52, "gold_block", "金镜无限反射廊"),
        (-58, "iron_block", "铁镜无限反射廊"),
        (-64, "obsidian", "黑曜石菲涅尔廊"),
    ]
    for mx, mblk, mlbl in mirror_specs:
        # 5-block wide corridor centered at mx (walls at mx-2, mx+2; interior mx-1..mx+1, H=7)
        c.hollow_box(mx - 2, y0, -98, mx + 2, y0 + 7, -66, mblk, floor=mblk, ceiling=mblk)
        c.fill(mx - 1, y0 + 1, -66, mx + 1, y0 + 5, -66, "air")  # south entrance from concourse
        # Suspended luminous end-rods & colored markers down the corridor to count mirror bounce depth!
        for mz in range(-94, -70, 6):
            c.set(mx, y0 + 4, mz, "end_rod", facing="up")
            c.set(mx, y0 + 3, mz, "cyan_concrete" if (mz % 12 == 0) else "magenta_concrete")
        c.place_sign(mx, y0 + 6, -65, ["[反射反弹次数测试]", mlbl, "在光追菜单调反弹数"], facing="south")

    # 2) 90-Degree L-Shaped Indirect Sunlight Bounce Chamber (X in [-70..-47], tucked at Z in [-98..-84] West side -> move to X in [-76..-67], Z in [-98..-66])
    # Entry from south concourse at (-71, -66) -> goes north to Z=-88, turns WEST to X=-76.. actually let's place cleanly in X in [-74..-68]
    c.hollow_box(-74, y0, -98, -68, y0 + 9, -66, "black_concrete", floor="white_concrete", ceiling="black_concrete")
    c.fill(-72, y0 + 1, -66, -70, y0 + 5, -66, "air")
    # Bright sunlight shaft ONLY hits an angled bright orange/red bounce wall at far north Z=-97 (Y=65..72)
    c.fill(-73, y0 + 1, -97, -69, y0 + 8, -97, "orange_wool")
    c.fill(-72, y0 + 9, -96, -70, y0 + 17, -94, "air")  # skylight hits orange wool only
    # Baffle wall at Z=-86 forces 2nd bounce light to illuminate a white pedestal in the shadowed nook!
    c.fill(-73, y0 + 1, -86, -71, y0 + 8, -86, "black_concrete")
    c.fill(-73, y0 + 1, -82, -72, y0 + 4, -81, "white_concrete")  # illuminated purely by orange bounce!
    c.place_sign(-71, y0 + 6, -65, ["[二次反弹暗道]", "仅橙壁受直射阳光", "拐角白柱全靠GI反弹"], facing="south")

    # =========================================================================
    # 3C. 20-Block Deep Absolute-Darkness Slit Shaft with Redstone Piston Shutter (X in [-98..-77], Z in [-98..-66])
    # =========================================================================
    # Carve a dramatic 20-block deep shaft from Y=78 down to Y=44!
    c.hollow_box(-98, 43, -98, -77, 79, -66, "deepslate_bricks", floor="polished_deepslate", ceiling="black_concrete")
    c.fill(-98, 80, -98, -77, 80, -66, "deepslate_tiles")
    # Viewing balcony at Y=64 on south side + spiral staircase descending to shaft floor Y=44
    c.fill(-92, y0 + 1, -66, -84, y0 + 5, -66, "air")  # entry from south concourse
    c.fill(-97, y0, -72, -78, y0, -67, "polished_deepslate")
    c.fill(-97, y0 + 1, -72, -78, y0 + 1, -72, "iron_bars")
    # Descending stone stairs hugging East & North interior walls down from Y=64 to Y=44
    for step in range(20):
        sy = y0 - step
        sz = -73 - step
        if sz >= -96:
            c.fill(-80, sy, sz, -78, sy, sz, "deepslate_brick_stairs", facing="south", half="bottom", shape="straight")
            c.fill(-80, sy - 1, sz, -78, sy - 1, sz, "deepslate_bricks")
    # Shaft floor at Y=44: shallow reflective pool + gold treasure island directly beneath the 2x2 sky slit!
    scx, scz = -88, -82
    c.fill(-96, 43, -96, -81, 43, -73, "polished_deepslate")
    c.fill(scx - 4, 43, scz - 4, scx + 4, 43, scz + 4, "water", level="0")
    c.fill(scx - 1, 43, scz - 1, scx + 1, 44, scz + 1, "gold_block")
    # 2x2 Sky Slit directly overhead at Y=79..80 (currently OPEN for a piercing god-ray shaft!)
    c.fill(scx, 78, scz, scx + 1, 82, scz + 1, "air")
    # Sticky pistons positioned on the sides ready to close the slit + Command Block toggle on balcony!
    for pz in (scz, scz + 1):
        c.set(scx - 2, 79, pz, "sticky_piston", facing="east", extended="false")
        c.set(scx - 1, 79, pz, "black_concrete")
        c.set(scx + 3, 79, pz, "sticky_piston", facing="west", extended="false")
        c.set(scx + 2, 79, pz, "black_concrete")
    # Balcony shutter control buttons (Open Slit vs Seal Absolute Darkness!)
    c.set(-86, y0 + 1, -71, "polished_deepslate")
    c.place_cmd(-86, y0, -71, f"fill {scx} 79 {scz} {scx+1} 80 {scz+1} minecraft:air", facing="up")
    c.set(-86, y0 + 2, -71, "stone_button", face="floor", facing="north", powered="false")
    c.place_sign(-86, y0 + 1, -70, ["[开启天井狭缝]", "测试体积光柱", "与极限局部曝光"], facing="south", color="yellow")

    c.set(-90, y0 + 1, -71, "polished_deepslate")
    c.place_cmd(-90, y0, -71, f"fill {scx} 79 {scz} {scx+1} 80 {scz+1} minecraft:black_concrete", facing="up")
    c.set(-90, y0 + 2, -71, "stone_button", face="floor", facing="north", powered="false")
    c.place_sign(-90, y0 + 1, -70, ["[闭合天井黑盒]", "测试绝对漆黑", "零虚假天空漏光"], facing="south", color="red")

    c.register_waypoint("3a_cornell", -69.5, y0 + 1.0, -61.5, 0.0, 2.0, "区3A · 经典康奈尔盒Lumen色溢")
    c.register_waypoint("3b_mirrors", -52.0, y0 + 1.0, -64.5, 180.0, 2.0, "区3B · 金铁无限反射廊与反弹暗道")
    c.register_waypoint("3c_slit_shaft", -88.0, 45.0, -74.5, 180.0, -28.0, "区3C · 20格深狭缝天井与零漏光测试")


def build_zone6_stress_dungeon(c: VoxelCanvas) -> None:
    """Zone 6: Subterranean Extreme Composite Stress Complex (X in [36, 102], Z in [38, 102], Y=36..61).
    6A (West): Trial Forge Hall (dense copper/grates/bars/chains/furnaces/lava + ~150 lanterns).
    6B (East): Fire & Ice Cavern (lava falls + magma vs blue ice + deep pool + soul fire + gold suspension bridge).
    """
    y0 = MAIN_FLOOR_Y  # 64

    # 1. Surface Garden Pavilion over Zone 6 (Y=64) with grand staircase descending into the dark underground!
    c.fill(36, y0, 38, 102, y0, 102, "grass_block")
    c.fill(36, y0, 38, 102, y0, 40, "stone_bricks")
    c.fill(36, y0, 38, 38, y0, 102, "stone_bricks")
    # Decorative trees on surface terrace
    for tx, tz, sd in ((50, 72, 101), (82, 55, 102), (88, 86, 103), (58, 92, 104)):
        c.tree(tx, y0 + 1, tz, "spruce_log", "spruce_leaves", height=7, canopy_r=3.0, seed=sd)
    # Stone entry pavilion at (44..54, Z=42..50) leading down a sealed dog-leg staircase to Y=42
    c.hollow_box(42, y0, 40, 56, y0 + 7, 52, "deepslate_bricks", floor="polished_deepslate", ceiling="waxed_oxidized_copper")
    c.fill(46, y0 + 1, 40, 52, y0 + 5, 40, "air")  # north entrance facing Zone 1
    c.fill(42, y0 + 1, 44, 42, y0 + 5, 48, "air")  # west entrance facing Zone 5
    c.place_sign(49, y0 + 2, 39, ["[区6 · 极端复合地下城]", "6A 铜铁试炼锻造厅", "6B 冰火熔岩水洞", "高密多光源+金属压测"], facing="north", color="yellow")

    # Stairwell descending from (48..50, Y=64, Z=46) southward down to Y=46 at Z=64 (strictly enclosed underground)
    for step in range(18):
        sy = y0 - step
        sz = 46 + step
        c.fill(47, sy, sz, 51, sy + 4, sz, "air")
        c.fill(47, sy - 1, sz, 51, sy - 1, sz, "deepslate_bricks")
        c.fill(48, sy, sz, 50, sy, sz, "deepslate_brick_stairs", facing="north", half="bottom", shape="straight")

    # =========================================================================
    # 6A. Trial Forge Hall (X in [38, 68], Z in [42, 98], Y=38..58)
    # =========================================================================
    c.hollow_box(38, 38, 42, 68, 58, 98, "deepslate_bricks", floor="polished_deepslate", ceiling="deepslate_tiles")
    # Re-open staircase landing into upper mezzanine at Y=46, Z=64
    c.fill(47, 46, 62, 51, 51, 66, "air")

    # Segmented forge lava troughs with obsidian/copper-grate bridges & intermittent crucibles (X=45..46 and X=60..61)
    # Keeps dramatic multi-light stress feel while staying well within MegaLights 80-block budget!
    for lx in (45, 60):
        # Solid obsidian/deepslate trough bed and curbs
        c.fill(lx, 37, 45, lx + 1, 38, 95, "obsidian")
        c.fill(lx - 1, 39, 46, lx - 1, 39, 94, "waxed_copper_grate")
        c.fill(lx + 2, 39, 46, lx + 2, 39, 94, "waxed_copper_grate")
        # 6 glowing smelting crucibles per side (each 2x2 lava = 24/side -> 48 total instead of 196)
        for seg_idx, sz in enumerate(range(47, 94, 8)):
            c.fill(lx, 38, sz, lx + 1, 38, sz + 1, "lava", level="0")
            # Copper grate casting lid over alternate crucibles for crisp grate-masked upward glow
            if seg_idx % 2 == 0:
                c.fill(lx, 39, sz, lx + 1, 39, sz + 1, "waxed_copper_grate")
            # Obsidian & gilded blackstone cross-bridges between crucibles
            if sz + 4 <= 94:
                c.fill(lx, 38, sz + 3, lx + 1, 38, sz + 5, "polished_blackstone_bricks")
                c.set(lx, 38, sz + 4, "gilded_blackstone")
                c.fill(lx, 39, sz + 4, lx + 1, 39, sz + 4, "waxed_cut_copper_slab", type="bottom")

    # Central metallic forged walkway (X=48..58) inlaid with alternating copper stages & iron grates
    for z in range(44, 96):
        c.fill(49, 38, z, 57, 38, z, "polished_deepslate")
        if z % 4 == 0:
            c.fill(50, 38, z, 56, 38, z, "waxed_copper_block")
        elif z % 4 == 2:
            c.fill(50, 38, z, 56, 38, z, "waxed_oxidized_copper")

    # Massive 4-stage oxidation copper & deepslate pillars + double-tier copper grate mezzanine balconies (Y=46)
    pillar_stages = [
        "waxed_copper_block",
        "waxed_exposed_copper",
        "waxed_weathered_copper",
        "waxed_oxidized_copper",
    ]
    # Mezzanine catwalks along West (X=39..44) and East (X=62..67) at Y=46
    c.fill(39, 46, 44, 44, 46, 96, "waxed_copper_grate")
    c.fill(62, 46, 44, 67, 46, 96, "waxed_copper_grate")
    c.fill(44, 47, 44, 44, 47, 96, "iron_bars")
    c.fill(62, 47, 44, 62, 47, 96, "iron_bars")
    # North Hero Viewing Balcony (Z=45..50, X=45..61, Y=46) supporting the 6a_forge waypoint!
    c.fill(45, 46, 45, 61, 46, 50, "waxed_exposed_copper_grate")
    c.fill(50, 46, 46, 56, 46, 49, "polished_deepslate")
    c.fill(51, 46, 47, 55, 46, 49, "waxed_cut_copper")
    c.fill(45, 47, 50, 61, 47, 50, "waxed_copper_bars")
    # Clear railing openings connecting North Balcony to West & East catwalks
    c.fill(44, 47, 46, 44, 47, 49, "air")
    c.fill(62, 47, 46, 62, 47, 49, "air")
    # Cross-bridges spanning the hall at Z=56, 72, 88
    for bz in (56, 72, 88):
        c.fill(45, 46, bz - 1, 61, 46, bz + 1, "waxed_exposed_copper_grate")
        c.fill(45, 47, bz - 1, 61, 47, bz - 1, "waxed_copper_bars")
        c.fill(45, 47, bz + 1, 61, 47, bz + 1, "waxed_copper_bars")

    # Stairs down from mezzanine (Y=46) to forge floor (Y=38) along Z=66..73
    for s in range(8):
        c.fill(48, 45 - s, 66 + s, 50, 45 - s, 66 + s, "deepslate_brick_stairs", facing="north", half="bottom", shape="straight")
        c.fill(48, 44 - s, 66 + s, 50, 44 - s, 66 + s, "deepslate_bricks")

    # Pillars + dense multi-color suspended chains & lanterns (~90 varied point lights + ~30 furnaces + lava)
    for idx, pz in enumerate(range(48, 94, 6)):
        pmat = pillar_stages[idx % 4]
        for px in (44, 62):
            c.fill(px, 39, pz, px, 57, pz, pmat)
            # Overhead transverse iron/copper arch ribs
        c.fill(44, 56, pz, 62, 56, pz, "waxed_cut_copper")
        # Hanging chandeliers at X=48, 53, 58 with alternating normal / copper / soul lanterns
        for ci, cx in enumerate((48, 53, 58)):
            drop = 4 if cx == 53 else 6
            c.fill(cx, 57 - drop, pz, cx, 57, pz, "waxed_copper_chain" if ci == 1 else "iron_chain", axis="y")
            ltype = ("lantern", "waxed_copper_lantern", "soul_lantern")[(idx + ci) % 3]
            c.set(cx, 56 - drop, pz, ltype, hanging="true")
            # Copper grate sleeve around central chandelier for crisp volumetric shafts!
            if cx == 53 and idx % 2 == 0:
                c.set(cx - 1, 56 - drop, pz, "waxed_copper_grate")
                c.set(cx + 1, 56 - drop, pz, "waxed_copper_grate")

    # Blast Furnace Smelting Banks along West Wall (X=39) & Anvil Forging Stations
    for fz in range(46, 94, 4):
        c.place_lit_furnace(39, 39, fz, kind="blast_furnace", facing="east")
        c.place_lit_furnace(39, 40, fz, kind="blast_furnace", facing="east")
        c.set(42, 39, fz, "anvil", facing="north")
        # Mezzanine upper copper bulbs (lit) behind iron bars
        c.set(39, 49, fz, "waxed_copper_bulb", lit="true", powered="false")
        c.set(40, 49, fz, "iron_bars")

    # Broad archway on East Wall (X=68..70, Z in [66..74], Y=39..49) connecting 6A Forge to 6B Fire & Ice Cavern!
    c.fill(68, 39, 66, 70, 49, 74, "air")
    c.arch_z(66, 74, 39, 7, 69, "obsidian", carve_air=True)

    # =========================================================================
    # 6B. Fire & Ice Lava/Water Cavern (X in [70, 100], Z in [42, 98], Y=36..58)
    # =========================================================================
    c.hollow_box(70, 36, 42, 100, 58, 98, "deepslate", floor="deepslate", ceiling="deepslate")
    # Re-open connecting arch on west wall X=70
    c.fill(70, 39, 66, 70, 49, 74, "air")

    # Sculpt organic-feeling stepped cavern walls & ceiling stalactites
    # NORTH HALF (Z in [43..68]): Sculpted Basalt/Obsidian Volcanic Reefs, Winding Lava Rivers & Split Lava Falls!
    c.fill(71, 36, 43, 99, 37, 67, "obsidian")
    # Build tiered volcanic basalt & blackstone reef terrain (Y=38..40) so lava carves dramatic glowing channels
    for x in range(71, 100):
        for z in range(43, 68):
            # Organic height ripple using deterministic trigonometric ridges
            h_wave = math.sin(x * 0.37 + z * 0.21) + 0.6 * math.cos(x * 0.23 - z * 0.41)
            dist_edge = min(x - 71, 99 - x, z - 43, 67 - z)
            top_y = 37
            if dist_edge <= 2:
                top_y = 39 if h_wave > -0.2 else 38
            elif h_wave > 0.55:
                top_y = 39
            elif h_wave > -0.25:
                top_y = 38
            for yy in range(38, top_y + 1):
                if (x * 7 + z * 13 + yy * 5) % 11 == 0:
                    blk = "gilded_blackstone"
                elif (x + z + yy) % 3 == 0:
                    blk = "polished_basalt"
                elif (x + z) % 2 == 0:
                    blk = "blackstone"
                else:
                    blk = "smooth_basalt"
                c.set(x, yy, z, blk)

    # Carve a natural plunge pool under the North cliff lava falls + two winding lava rivers!
    lava_cells: set[tuple[int, int]] = set()
    # 1. Irregular plunge pool around (85, 47)
    for dx in range(-4, 5):
        for dz in range(-2, 4):
            if (dx * dx) / 14.0 + (dz * dz) / 7.5 <= 1.0:
                lava_cells.add((85 + dx, 47 + dz))
    # 2. West meandering lava river from plunge pool (82, 49) curving toward SW reef (74..80, 64)
    for t in range(18):
        rz = 49 + t
        if rz > 65:
            break
        rx = int(round(81.5 - 5.2 * math.sin(t * 0.26) - t * 0.22))
        lava_cells.add((rx, rz))
        if t % 3 != 0:
            lava_cells.add((rx + 1, rz))
    # 3. East meandering lava river from plunge pool (88, 49) curving toward SE reef (93..96, 63)
    for t in range(16):
        rz = 49 + t
        if rz > 65:
            break
        rx = int(round(88.5 + 5.5 * math.sin(t * 0.28 + 0.4) + t * 0.16))
        lava_cells.add((rx, rz))
        if t % 4 == 0:
            lava_cells.add((rx - 1, rz))
    # 4. Small glowing molten pool right beside the suspension bridge center (84..86, 62..64) for rich gold reflection
    for px in range(84, 87):
        for pz in range(62, 65):
            lava_cells.add((px, pz))

    for lx, lz in lava_cells:
        if 72 <= lx <= 98 and 45 <= lz <= 66:
            c.fill(lx, 38, lz, lx, 40, lz, "air")
            c.set(lx, 37, lz, "lava", level="0")

    # Sculpted jagged basalt crags & single-block FlushIes magma_block fissures flanking the lava rivers
    reef_crags = [
        (75, 49, 4), (79, 55, 3), (85, 56, 4), (92, 52, 4), (95, 59, 3), (76, 62, 2), (91, 64, 3),
    ]
    for cx, cz, ch in reef_crags:
        c.fill(cx, 38, cz, cx, 38 + ch, cz, "polished_basalt")
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if (cx + dx, cz + dz) not in lava_cells and 72 <= cx + dx <= 98 and 44 <= cz + dz <= 66:
                c.fill(cx + dx, 38, cz + dz, cx + dx, 38 + max(1, ch - 2), cz + dz, "blackstone")
    # 12 selective FlushIes magma_block volcanic fissures embedded flush in the obsidian shore
    for mx, mz in (
        (77, 51), (82, 53), (88, 53), (94, 54), (74, 58), (81, 59),
        (87, 59), (93, 61), (79, 64), (83, 65), (89, 65), (85, 51),
    ):
        if (mx, mz) not in lava_cells:
            c.set(mx, 38, mz, "magma_block")

    # Twin Split Lava Waterfalls cascading down jagged blackstone cliff (Z=43..45, X in [80..91])
    c.fill(80, 38, 43, 91, 56, 44, "blackstone")
    c.fill(82, 38, 44, 89, 55, 44, "polished_basalt")
    # Protruding obsidian boulder splitting the main waterfall mid-air at Y=46..47
    c.fill(83, 46, 44, 85, 47, 45, "obsidian")
    # Main twin fall: X=84 (Y=55..48), splits around boulder into X=83 and X=85 (Y=47..38)
    c.fill(84, 48, 44, 85, 55, 44, "lava", level="8")
    c.fill(84, 55, 44, 85, 55, 44, "lava", level="0")
    c.fill(83, 38, 45, 83, 47, 45, "lava", level="8")
    c.fill(85, 38, 45, 85, 46, 45, "lava", level="8")
    # Secondary slender fall at X=88 (Y=54..38)
    c.fill(88, 38, 44, 88, 54, 44, "lava", level="8")
    c.set(88, 54, 44, "lava", level="0")
    # Jagged basalt stalactites from ceiling on fire side (only 4 glowing magma tips)
    for sx, sz, slen in ((76, 48, 5), (84, 54, 4), (94, 48, 6), (92, 60, 5)):
        c.fill(sx, 57 - slen, sz, sx, 57, sz, "polished_basalt")
        c.set(sx, 56 - slen, sz, "magma_block")

    # SOUTH HALF (Z in [73..97]): Frozen Blue-Ice Stalactites + Deep Water Pool + Soul Fire + Shore Wetness!
    c.fill(71, 36, 73, 99, 36, 97, "deepslate_bricks")
    c.fill(72, 37, 74, 98, 40, 96, "water", level="0")  # 4-block deep cold cavern pool (Y=37..40)
    # Porous stone & sand shore islands protruding out of cold pool at Y=41 (triggers P1 shore wetness!)
    for ix, iz, mat in ((78, 84, "sandstone"), (92, 82, "cobblestone"), (85, 92, "packed_ice")):
        c.fill(ix - 2, 37, iz - 2, ix + 2, 41, iz + 2, mat)
        c.set(ix, 42, iz, "soul_campfire", facing="north", lit="true", signal_fire="false", waterlogged="false")
    # Massive Blue Ice & Packed Ice Stalactites / Frozen Pillars descending into water!
    for ix, iz, h in ((75, 90, 12), (85, 86, 15), (95, 90, 13), (80, 95, 10), (90, 95, 11)):
        c.fill(ix - 1, 57 - h, iz - 1, ix + 1, 57, iz + 1, "packed_ice")
        c.fill(ix, 55 - h, iz, ix, 57, iz, "blue_ice")
        c.set(ix, 54 - h, iz, "sea_lantern")
    # Underwater sea lanterns recessed under ice pillars
    c.set(85, 36, 86, "sea_lantern")
    c.set(78, 36, 89, "sea_lantern")
    c.set(92, 36, 89, "sea_lantern")

    # CENTRAL SUSPENSION BRIDGE separating Fire (North) & Ice (South) along Z in [68..72], X in [71..99], Y=42!
    # Obsidian + Gold Mirror Deck + Iron/Copper Bars Railing -> reflects burning orange lava on left & cold cyan ice/water on right!
    c.fill(71, 41, 68, 99, 41, 72, "obsidian")
    c.fill(71, 42, 69, 99, 42, 71, "gold_block")
    c.fill(71, 42, 70, 99, 42, 70, "iron_block")
    c.fill(71, 43, 68, 99, 43, 68, "iron_bars")
    c.fill(71, 43, 72, 99, 43, 72, "iron_bars")
    # Stepped approach from archway (X=71, Y=39 -> Y=42)
    for s in range(3):
        c.fill(71 + s, 39 + s, 69, 71 + s, 39 + s, 71, "polished_blackstone_stairs", facing="east", half="bottom", shape="straight")
    # Suspension chains up to cavern ceiling every 6 blocks
    for cx in range(74, 98, 6):
        c.fill(cx, 43, 68, cx, 57, 68, "iron_chain", axis="y")
        c.fill(cx, 43, 72, cx, 57, 72, "iron_chain", axis="y")
        c.set(cx, 45, 68, "lantern", hanging="false")
        c.set(cx, 45, 72, "soul_lantern", hanging="false")

    # Observation platform at east end of bridge (X=96..99, Z=67..73)
    c.fill(95, 42, 67, 99, 42, 73, "polished_deepslate")
    c.place_sign(99, 43, 70, ["[6B · 冰火熔岩水洞]", "北:熔岩河+双瀑礁石", "南:蓝冰+寒潭+灵魂火", "中:金铁悬桥双极反射"], facing="west", color="yellow")

    c.register_waypoint("6a_forge", 53.5, 47.0, 48.5, 0.0, 12.0, "区6A · 铜铁试炼锻造多光源压测厅")
    c.register_waypoint("6b_fire_ice", 76.5, 43.0, 70.5, -90.0, 2.0, "区6B · 冰火熔岩水洞悬桥终帧机位")
