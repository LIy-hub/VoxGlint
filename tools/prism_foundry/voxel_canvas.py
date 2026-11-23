"""High-speed 3D voxel canvas, architectural primitives, auto-connector, light baker & QA visualizer."""

from __future__ import annotations

import array
import json
import math
import os
import random
import uuid
import zipfile
from collections import deque
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple

from PIL import Image, ImageDraw

from nbt_anvil import (
    DATA_VERSION,
    MAX_SECTION_Y,
    MIN_SECTION_Y,
    TAG_COMPOUND,
    TAG_DOUBLE,
    TAG_FLOAT,
    TAG_STRING,
    WORLD_MIN_Y,
    block_emission_level,
    block_light_opacity,
    create_save_metadata,
    encode_chunk_nbt,
    encode_entity_chunk_nbt,
    is_motion_blocking,
    nbt_byte,
    nbt_compound,
    nbt_double,
    nbt_float,
    nbt_int,
    nbt_int_array,
    nbt_list,
    nbt_short,
    nbt_string,
    write_mca_file,
)

BlockState = Tuple[str, Tuple[Tuple[str, str], ...]]

# World canvas bounds: 16x16 chunks = 256x256 blocks, Y in [24, 151] (128 layers)
MIN_X, MAX_X = -128, 127
MIN_Z, MAX_Z = -128, 127
CANVAS_MIN_Y, CANVAS_MAX_Y = 24, 151
SIZE_X = MAX_X - MIN_X + 1  # 256
SIZE_Z = MAX_Z - MIN_Z + 1  # 256
SIZE_Y = CANVAS_MAX_Y - CANVAS_MIN_Y + 1  # 128

SEA_LEVEL = 62
MAIN_FLOOR_Y = 64


def norm_block(name: str, props: Optional[Dict[str, Any]] = None) -> BlockState:
    if ":" not in name:
        name = "minecraft:" + name
    if not props:
        return (name, ())
    items = tuple(sorted((str(k), str(v).lower() if isinstance(v, bool) else str(v)) for k, v in props.items()))
    return (name, items)


def _is_connectable_solid(short: str) -> bool:
    if short in ("air", "cave_air", "void_air", "water", "lava", "bubble_column", "barrier"):
        return False
    if (
        "torch" in short
        or "lantern" in short
        or "sign" in short
        or "button" in short
        or "lever" in short
        or "rail" in short
        or "grass" in short and short != "grass_block"
        or "fern" in short
        or "flower" in short
        or "coral" in short and "block" not in short
        or "kelp" in short
        or "carpet" in short
        or "trapdoor" in short
        or "door" in short
        or "ladder" in short
        or "chain" in short
        or "fence" in short
        or "bars" in short
        or "pane" in short
        or "slab" in short
        or "stairs" in short
        or "campfire" in short
        or "fire" in short
        or "portal" in short
        or "leaves" in short
        or "amethyst" in short and short not in ("amethyst_block", "budding_amethyst")
        or short in (
            "end_rod",
            "redstone_wire",
            "dandelion",
            "poppy",
            "blue_orchid",
            "cornflower",
            "lily_of_the_valley",
            "chest",
            "copper_chest",
            "waxed_copper_chest",
            "enchanting_table",
            "anvil",
            "sea_pickle",
            "lily_pad",
            "glow_lichen",
            "sculk_vein",
            "copper_golem_statue",
            "waxed_copper_golem_statue",
            "waxed_exposed_copper_golem_statue",
            "waxed_weathered_copper_golem_statue",
            "waxed_oxidized_copper_golem_statue",
            "lightning_rod",
            "waxed_lightning_rod",
        )
    ):
        return False
    return True


def _is_bars_or_pane(short: str) -> bool:
    return short.endswith("_bars") or short == "glass_pane" or short.endswith("_glass_pane")


def _is_fence(short: str) -> bool:
    return short.endswith("_fence")


def _is_wall(short: str) -> bool:
    return short.endswith("_wall") and "sign" not in short and "torch" not in short and "fan" not in short and "head" not in short


class VoxelCanvas:
    """Fast dense 3D voxel buffer over [-128..127] x [24..151] x [-128..127]."""

    def __init__(self) -> None:
        self.states: List[BlockState] = [norm_block("air")]
        self.state_to_id: Dict[BlockState, int] = {self.states[0]: 0}
        # Compact uint16 array indexed by (y - CANVAS_MIN_Y) * 65536 + (z - MIN_Z) * 256 + (x - MIN_X)
        self.grid = array.array("H", [0]) * (SIZE_X * SIZE_Y * SIZE_Z)
        self.block_entities: Dict[Tuple[int, int, int], Dict[str, Tuple[int, Any]]] = {}
        self.entities: List[Dict[str, Tuple[int, Any]]] = []
        self.chunk_biomes: Dict[Tuple[int, int], str] = {}
        self.waypoints: Dict[str, Tuple[float, float, float, float, float, str]] = {}
        self.last_sky: Optional[bytearray] = None
        self.last_blk: Optional[bytearray] = None

        # Pre-register bedrock/stone/sand/water default ocean floor so out-of-island matches flat generator
        self._init_ocean_canvas()

    def _id(self, name: str, props: Optional[Dict[str, Any]] = None) -> int:
        st = norm_block(name, props)
        idx = self.state_to_id.get(st)
        if idx is None:
            idx = len(self.states)
            self.states.append(st)
            self.state_to_id[st] = idx
        return idx

    def _init_ocean_canvas(self) -> None:
        """Initialize Y=24..32 stone, Y=33..35 sand, Y=36..62 water (matching flat warm ocean)."""
        stone_id = self._id("stone")
        sand_id = self._id("sand")
        water_id = self._id("water", {"level": "0"})
        plane_size = SIZE_X * SIZE_Z
        for y in range(CANVAS_MIN_Y, 33):
            off = (y - CANVAS_MIN_Y) * plane_size
            self.grid[off : off + plane_size] = array.array("H", [stone_id]) * plane_size
        for y in range(33, 36):
            off = (y - CANVAS_MIN_Y) * plane_size
            self.grid[off : off + plane_size] = array.array("H", [sand_id]) * plane_size
        for y in range(36, SEA_LEVEL + 1):
            off = (y - CANVAS_MIN_Y) * plane_size
            self.grid[off : off + plane_size] = array.array("H", [water_id]) * plane_size

    def in_bounds(self, x: int, y: int, z: int) -> bool:
        return MIN_X <= x <= MAX_X and CANVAS_MIN_Y <= y <= CANVAS_MAX_Y and MIN_Z <= z <= MAX_Z

    def set(self, x: int, y: int, z: int, block: str, **props: Any) -> None:
        if not self.in_bounds(x, y, z):
            return
        idx = ((y - CANVAS_MIN_Y) << 16) | ((z - MIN_Z) << 8) | (x - MIN_X)
        self.grid[idx] = self._id(block, props if props else None)
        if (x, y, z) in self.block_entities:
            del self.block_entities[(x, y, z)]

    def get(self, x: int, y: int, z: int) -> BlockState:
        if not self.in_bounds(x, y, z):
            return self.states[0]
        idx = ((y - CANVAS_MIN_Y) << 16) | ((z - MIN_Z) << 8) | (x - MIN_X)
        return self.states[self.grid[idx]]

    def get_short(self, x: int, y: int, z: int) -> str:
        return self.get(x, y, z)[0].removeprefix("minecraft:")

    def fill(
        self,
        x1: int,
        y1: int,
        z1: int,
        x2: int,
        y2: int,
        z2: int,
        block: str,
        **props: Any,
    ) -> None:
        xa, xb = max(MIN_X, min(x1, x2)), min(MAX_X, max(x1, x2))
        ya, yb = max(CANVAS_MIN_Y, min(y1, y2)), min(CANVAS_MAX_Y, max(y1, y2))
        za, zb = max(MIN_Z, min(z1, z2)), min(MAX_Z, max(z1, z2))
        if xa > xb or ya > yb or za > zb:
            return
        bid = self._id(block, props if props else None)
        w = xb - xa + 1
        row = array.array("H", [bid]) * w
        grid = self.grid
        for y in range(ya, yb + 1):
            y_off = (y - CANVAS_MIN_Y) << 16
            for z in range(za, zb + 1):
                start = y_off | ((z - MIN_Z) << 8) | (xa - MIN_X)
                grid[start : start + w] = row

    def replace_in_box(
        self,
        x1: int,
        y1: int,
        z1: int,
        x2: int,
        y2: int,
        z2: int,
        target_short: str,
        new_block: str,
        **props: Any,
    ) -> None:
        xa, xb = max(MIN_X, min(x1, x2)), min(MAX_X, max(x1, x2))
        ya, yb = max(CANVAS_MIN_Y, min(y1, y2)), min(CANVAS_MAX_Y, max(y1, y2))
        za, zb = max(MIN_Z, min(z1, z2)), min(MAX_Z, max(z1, z2))
        bid = self._id(new_block, props if props else None)
        full_target = "minecraft:" + target_short if ":" not in target_short else target_short
        for y in range(ya, yb + 1):
            y_off = (y - CANVAS_MIN_Y) << 16
            for z in range(za, zb + 1):
                row_off = y_off | ((z - MIN_Z) << 8)
                for x in range(xa, xb + 1):
                    idx = row_off | (x - MIN_X)
                    if self.states[self.grid[idx]][0] == full_target:
                        self.grid[idx] = bid

    def hollow_box(
        self,
        x1: int,
        y1: int,
        z1: int,
        x2: int,
        y2: int,
        z2: int,
        wall: str,
        floor: Optional[str] = None,
        ceiling: Optional[str] = None,
        interior: str = "air",
    ) -> None:
        xa, xb = min(x1, x2), max(x1, x2)
        ya, yb = min(y1, y2), max(y1, y2)
        za, zb = min(z1, z2), max(z1, z2)
        self.fill(xa, ya, za, xb, yb, zb, wall)
        if floor:
            self.fill(xa, ya, za, xb, ya, zb, floor)
        if ceiling:
            self.fill(xa, yb, za, xb, yb, zb, ceiling)
        if xb - xa >= 2 and yb - ya >= 2 and zb - za >= 2:
            self.fill(xa + 1, ya + 1, za + 1, xb - 1, yb - 1, zb - 1, interior)

    def cylinder(
        self,
        cx: int,
        y1: int,
        y2: int,
        cz: int,
        radius: float,
        block: str,
        hollow: bool = False,
        inner_radius: Optional[float] = None,
        **props: Any,
    ) -> None:
        r_int = int(math.ceil(radius))
        r2 = radius * radius
        ir = (radius - 1.15) if inner_radius is None else inner_radius
        ir2 = ir * ir if hollow else -1.0
        for dx in range(-r_int, r_int + 1):
            for dz in range(-r_int, r_int + 1):
                d2 = dx * dx + dz * dz
                if d2 <= r2 and (not hollow or d2 >= ir2):
                    self.fill(cx + dx, y1, cz + dz, cx + dx, y2, cz + dz, block, **props)

    def dome(
        self,
        cx: int,
        base_y: int,
        cz: int,
        radius: float,
        block: str,
        hollow: bool = True,
        thickness: float = 1.25,
        height_scale: float = 1.0,
        rib_block: Optional[str] = None,
        **props: Any,
    ) -> None:
        r_int = int(math.ceil(radius))
        h_int = int(math.ceil(radius * height_scale))
        r2 = radius * radius
        ir2 = max(0.0, (radius - thickness) ** 2)
        for dy in range(0, h_int + 1):
            ny = dy / max(height_scale, 0.01)
            for dx in range(-r_int, r_int + 1):
                for dz in range(-r_int, r_int + 1):
                    d2 = dx * dx + dz * dz + ny * ny
                    if d2 <= r2 and (not hollow or d2 >= ir2):
                        use = block
                        if rib_block and (abs(dx) <= 0 or abs(dz) <= 0 or abs(abs(dx) - abs(dz)) <= 0):
                            use = rib_block
                        self.set(cx + dx, base_y + dy, cz + dz, use, **props)

    def arch_x(
        self,
        x1: int,
        x2: int,
        base_y: int,
        spring_h: int,
        z: int,
        block: str,
        carve_air: bool = True,
        **props: Any,
    ) -> None:
        """Build a semicircular arch spanning along X at fixed Z."""
        xa, xb = min(x1, x2), max(x1, x2)
        cx = 0.5 * (xa + xb)
        r = 0.5 * (xb - xa)
        top_y = base_y + spring_h + int(math.ceil(r))
        for x in range(xa, xb + 1):
            for y in range(base_y, top_y + 1):
                if y < base_y + spring_h:
                    if x == xa or x == xb:
                        self.set(x, y, z, block, **props)
                    elif carve_air:
                        self.set(x, y, z, "air")
                else:
                    dx = x - cx
                    dy = y - (base_y + spring_h)
                    dist = math.hypot(dx, dy)
                    if r - 0.65 <= dist <= r + 0.65:
                        self.set(x, y, z, block, **props)
                    elif dist < r - 0.65 and carve_air:
                        self.set(x, y, z, "air")

    def arch_z(
        self,
        z1: int,
        z2: int,
        base_y: int,
        spring_h: int,
        x: int,
        block: str,
        carve_air: bool = True,
        **props: Any,
    ) -> None:
        """Build a semicircular arch spanning along Z at fixed X."""
        za, zb = min(z1, z2), max(z1, z2)
        cz = 0.5 * (za + zb)
        r = 0.5 * (zb - za)
        top_y = base_y + spring_h + int(math.ceil(r))
        for z in range(za, zb + 1):
            for y in range(base_y, top_y + 1):
                if y < base_y + spring_h:
                    if z == za or z == zb:
                        self.set(x, y, z, block, **props)
                    elif carve_air:
                        self.set(x, y, z, "air")
                else:
                    dz = z - cz
                    dy = y - (base_y + spring_h)
                    dist = math.hypot(dz, dy)
                    if r - 0.65 <= dist <= r + 0.65:
                        self.set(x, y, z, block, **props)
                    elif dist < r - 0.65 and carve_air:
                        self.set(x, y, z, "air")

    def pillar(
        self,
        x: int,
        y1: int,
        y2: int,
        z: int,
        shaft: str = "quartz_pillar",
        base: str = "chiseled_quartz_block",
        cap: str = "chiseled_quartz_block",
    ) -> None:
        self.fill(x, y1, z, x, y2, z, shaft)
        if base:
            self.set(x, y1, z, base)
        if cap:
            self.set(x, y2, z, cap)

    def tree(
        self,
        x: int,
        base_y: int,
        z: int,
        log: str = "oak_log",
        leaves: str = "oak_leaves",
        height: int = 6,
        canopy_r: float = 2.8,
        seed: int = 1,
    ) -> None:
        rng = random.Random(seed)
        self.fill(x, base_y, z, x, base_y + height - 1, z, log, axis="y")
        cy = base_y + height
        ri = int(math.ceil(canopy_r))
        for dy in range(-2, ri + 1):
            for dx in range(-ri, ri + 1):
                for dz in range(-ri, ri + 1):
                    d = math.sqrt(dx * dx + dz * dz + (dy * 1.15) ** 2)
                    if d <= canopy_r + rng.uniform(-0.25, 0.25):
                        if dx == 0 and dz == 0 and dy < 0:
                            continue
                        if self.get_short(x + dx, cy + dy, z + dz) == "air":
                            self.set(x + dx, cy + dy, z + dz, leaves, persistent="true", distance="1")

    def set_biome_rect(self, x1: int, z1: int, x2: int, z2: int, biome: str) -> None:
        if ":" not in biome:
            biome = "minecraft:" + biome
        cx1, cx2 = min(x1, x2) >> 4, max(x1, x2) >> 4
        cz1, cz2 = min(z1, z2) >> 4, max(z1, z2) >> 4
        for cx in range(cx1, cx2 + 1):
            for cz in range(cz1, cz2 + 1):
                self.chunk_biomes[(cx, cz)] = biome

    def place_sign(
        self,
        x: int,
        y: int,
        z: int,
        lines: Sequence[str],
        facing: str = "north",
        wall: bool = True,
        rotation: int = 0,
        wood: str = "dark_oak",
        color: str = "white",
        glowing: bool = True,
    ) -> None:
        if wall:
            self.set(x, y, z, f"{wood}_wall_sign", facing=facing, waterlogged="false")
        else:
            self.set(x, y, z, f"{wood}_sign", rotation=str(rotation), waterlogged="false")
        padded = [str(lines[i]) if i < len(lines) else "" for i in range(4)]
        empty = ["", "", "", ""]
        self.block_entities[(x, y, z)] = {
            "id": nbt_string("minecraft:sign"),
            "x": nbt_int(x),
            "y": nbt_int(y),
            "z": nbt_int(z),
            "keepPacked": nbt_byte(0),
            "is_waxed": nbt_byte(1),
            "front_text": nbt_compound(
                {
                    "color": nbt_string(color),
                    "has_glowing_text": nbt_byte(1 if glowing else 0),
                    "messages": nbt_list(TAG_STRING, padded),
                }
            ),
            "back_text": nbt_compound(
                {
                    "color": nbt_string(color),
                    "has_glowing_text": nbt_byte(0),
                    "messages": nbt_list(TAG_STRING, empty),
                }
            ),
        }

    def place_cmd(
        self,
        x: int,
        y: int,
        z: int,
        command: str,
        facing: str = "up",
        auto: bool = False,
    ) -> None:
        self.set(x, y, z, "command_block", facing=facing, conditional="false")
        self.block_entities[(x, y, z)] = {
            "id": nbt_string("minecraft:command_block"),
            "x": nbt_int(x),
            "y": nbt_int(y),
            "z": nbt_int(z),
            "keepPacked": nbt_byte(0),
            "Command": nbt_string(command),
            "auto": nbt_byte(1 if auto else 0),
            "powered": nbt_byte(0),
            "conditionMet": nbt_byte(0),
            "TrackOutput": nbt_byte(0),
            "UpdateLastExecution": nbt_byte(1),
            "SuccessCount": nbt_int(0),
        }

    def place_lit_furnace(
        self,
        x: int,
        y: int,
        z: int,
        kind: str = "blast_furnace",
        facing: str = "south",
    ) -> None:
        self.set(x, y, z, kind, facing=facing, lit="true")
        # Give a large burn time + coal/iron ore so the furnace stays lit cleanly in-game
        self.block_entities[(x, y, z)] = {
            "id": nbt_string(f"minecraft:{kind}"),
            "x": nbt_int(x),
            "y": nbt_int(y),
            "z": nbt_int(z),
            "keepPacked": nbt_byte(0),
            "lit_time_remaining": nbt_short(32000),
            "lit_total_time": nbt_short(32000),
            "cooking_time_spent": nbt_short(0),
            "cooking_total_time": nbt_short(200),
            "Items": nbt_list(
                TAG_COMPOUND,
                [
                    {
                        "Slot": nbt_byte(1),
                        "id": nbt_string("minecraft:coal_block"),
                        "count": nbt_int(64),
                    }
                ],
            ),
        }

    def place_chest(self, x: int, y: int, z: int, facing: str = "south", block: str = "chest") -> None:
        self.set(x, y, z, block, facing=facing, type="single", waterlogged="false")
        self.block_entities[(x, y, z)] = {
            "id": nbt_string("minecraft:chest"),
            "x": nbt_int(x),
            "y": nbt_int(y),
            "z": nbt_int(z),
            "keepPacked": nbt_byte(0),
            "Items": nbt_list(TAG_COMPOUND, []),
        }

    def place_beacon(self, x: int, y: int, z: int, pyramid_block: str = "iron_block") -> None:
        """Place a functioning beacon with a 3x3 metal base right underneath."""
        self.fill(x - 1, y - 1, z - 1, x + 1, y - 1, z + 1, pyramid_block)
        self.set(x, y, z, "beacon")
        self.block_entities[(x, y, z)] = {
            "id": nbt_string("minecraft:beacon"),
            "x": nbt_int(x),
            "y": nbt_int(y),
            "z": nbt_int(z),
            "keepPacked": nbt_byte(0),
            "Levels": nbt_int(1),
        }

    def add_armor_stand(
        self,
        x: float,
        y: float,
        z: float,
        yaw: float = 0.0,
        head: Optional[str] = None,
        chest: Optional[str] = None,
        legs: Optional[str] = None,
        feet: Optional[str] = None,
        mainhand: Optional[str] = None,
        offhand: Optional[str] = None,
        show_arms: bool = True,
    ) -> None:
        u = uuid.uuid5(uuid.NAMESPACE_DNS, f"prism-armor-{x:.2f}-{y:.2f}-{z:.2f}-{yaw:.1f}")
        u_ints = [int.from_bytes(u.bytes[i : i + 4], "big", signed=True) for i in range(0, 16, 4)]
        equip: Dict[str, Tuple[int, Any]] = {}
        for slot, item in (
            ("head", head),
            ("chest", chest),
            ("legs", legs),
            ("feet", feet),
            ("mainhand", mainhand),
            ("offhand", offhand),
        ):
            if item:
                full = item if ":" in item else "minecraft:" + item
                equip[slot] = nbt_compound({"id": nbt_string(full), "count": nbt_int(1)})
        ent: Dict[str, Tuple[int, Any]] = {
            "id": nbt_string("minecraft:armor_stand"),
            "UUID": nbt_int_array(u_ints),
            "Pos": nbt_list(TAG_DOUBLE, [x, y, z]),
            "Motion": nbt_list(TAG_DOUBLE, [0.0, 0.0, 0.0]),
            "Rotation": nbt_list(TAG_FLOAT, [yaw, 0.0]),
            "OnGround": nbt_byte(1),
            "Invulnerable": nbt_byte(1),
            "ShowArms": nbt_byte(1 if show_arms else 0),
            "NoBasePlate": nbt_byte(0),
            "Small": nbt_byte(0),
            "Health": nbt_float(20.0),
            "equipment": nbt_compound(equip),
        }
        self.entities.append(ent)

    def add_boat(self, x: float, y: float, z: float, yaw: float = 0.0, boat_id: str = "oak_boat") -> None:
        u = uuid.uuid5(uuid.NAMESPACE_DNS, f"prism-boat-{x:.2f}-{y:.2f}-{z:.2f}")
        u_ints = [int.from_bytes(u.bytes[i : i + 4], "big", signed=True) for i in range(0, 16, 4)]
        full = boat_id if ":" in boat_id else "minecraft:" + boat_id
        self.entities.append(
            {
                "id": nbt_string(full),
                "UUID": nbt_int_array(u_ints),
                "Pos": nbt_list(TAG_DOUBLE, [x, y, z]),
                "Motion": nbt_list(TAG_DOUBLE, [0.0, 0.0, 0.0]),
                "Rotation": nbt_list(TAG_FLOAT, [yaw, 0.0]),
                "OnGround": nbt_byte(0),
                "Invulnerable": nbt_byte(1),
            }
        )

    def register_waypoint(self, key: str, x: float, y: float, z: float, yaw: float, pitch: float, label: str) -> None:
        self.waypoints[key] = (x, y, z, yaw, pitch, label)

    def auto_connect_blocks(self) -> int:
        """Compute north/south/east/west connections for bars, glass panes, fences, and walls."""
        changed = 0
        # Collect indices of connectable blocks first for speed
        bars_ids = set()
        fence_ids = set()
        wall_ids = set()
        for sid, (name, _) in enumerate(self.states):
            short = name.removeprefix("minecraft:")
            if _is_bars_or_pane(short):
                bars_ids.add(sid)
            elif _is_fence(short):
                fence_ids.add(sid)
            elif _is_wall(short):
                wall_ids.add(sid)
        target_ids = bars_ids | fence_ids | wall_ids
        if not target_ids:
            return 0

        grid = self.grid
        states = self.states
        neighbors = (("north", 0, -1), ("south", 0, 1), ("west", -1, 0), ("east", 1, 0))
        updates: List[Tuple[int, int]] = []

        for y in range(CANVAS_MIN_Y, CANVAS_MAX_Y + 1):
            y_off = (y - CANVAS_MIN_Y) << 16
            for z in range(MIN_Z, MAX_Z + 1):
                row_off = y_off | ((z - MIN_Z) << 8)
                for x in range(MIN_X, MAX_X + 1):
                    idx = row_off | (x - MIN_X)
                    sid = grid[idx]
                    if sid not in target_ids:
                        continue
                    name, props = states[sid]
                    short = name.removeprefix("minecraft:")
                    pdict = dict(props)
                    if sid in bars_ids:
                        for dname, dx, dz in neighbors:
                            nx, nz = x + dx, z + dz
                            if MIN_X <= nx <= MAX_X and MIN_Z <= nz <= MAX_Z:
                                nshort = states[grid[y_off | ((nz - MIN_Z) << 8) | (nx - MIN_X)]][0].removeprefix("minecraft:")
                                conn = _is_bars_or_pane(nshort) or _is_connectable_solid(nshort)
                            else:
                                conn = False
                            pdict[dname] = "true" if conn else "false"
                        pdict.setdefault("waterlogged", "false")
                    elif sid in fence_ids:
                        is_nether = short == "nether_brick_fence"
                        for dname, dx, dz in neighbors:
                            nx, nz = x + dx, z + dz
                            if MIN_X <= nx <= MAX_X and MIN_Z <= nz <= MAX_Z:
                                nshort = states[grid[y_off | ((nz - MIN_Z) << 8) | (nx - MIN_X)]][0].removeprefix("minecraft:")
                                if _is_fence(nshort):
                                    conn = (nshort == "nether_brick_fence") == is_nether
                                elif nshort.endswith("_fence_gate"):
                                    conn = True
                                else:
                                    conn = _is_connectable_solid(nshort)
                            else:
                                conn = False
                            pdict[dname] = "true" if conn else "false"
                        pdict.setdefault("waterlogged", "false")
                    elif sid in wall_ids:
                        conn_dirs = []
                        above_short = "air"
                        if y < CANVAS_MAX_Y:
                            above_short = states[grid[((y + 1 - CANVAS_MIN_Y) << 16) | ((z - MIN_Z) << 8) | (x - MIN_X)]][0].removeprefix("minecraft:")
                        above_solid = _is_connectable_solid(above_short) or _is_wall(above_short)
                        for dname, dx, dz in neighbors:
                            nx, nz = x + dx, z + dz
                            if MIN_X <= nx <= MAX_X and MIN_Z <= nz <= MAX_Z:
                                nshort = states[grid[y_off | ((nz - MIN_Z) << 8) | (nx - MIN_X)]][0].removeprefix("minecraft:")
                                conn = _is_wall(nshort) or _is_bars_or_pane(nshort) or nshort.endswith("_fence_gate") or _is_connectable_solid(nshort)
                            else:
                                conn = False
                            if conn:
                                conn_dirs.append(dname)
                                pdict[dname] = "tall" if above_solid else "low"
                            else:
                                pdict[dname] = "none"
                        straight = set(conn_dirs) in ({"north", "south"}, {"east", "west"})
                        pdict["up"] = "false" if (straight and not above_solid) else "true"
                        pdict.setdefault("waterlogged", "false")

                    new_sid = self._id(name, pdict)
                    if new_sid != sid:
                        updates.append((idx, new_sid))
                        changed += 1

        for idx, new_sid in updates:
            grid[idx] = new_sid
        return changed

    def validate_against_jar(self, jar_path: str) -> Tuple[List[str], List[str]]:
        """Validate every block ID and property key against MC 26.2 client JAR blockstates."""
        missing_blocks: List[str] = []
        invalid_props: List[str] = []
        if not os.path.exists(jar_path):
            return ([f"JAR not found: {jar_path}"], [])
        with zipfile.ZipFile(jar_path) as z:
            names_set = set(z.namelist())
            checked_props: Dict[str, Optional[set[str]]] = {}
            for full_name, props in self.states:
                short = full_name.removeprefix("minecraft:")
                if short in ("air", "cave_air", "void_air", "water", "lava", "barrier", "light"):
                    continue
                bs_path = f"assets/minecraft/blockstates/{short}.json"
                if bs_path not in names_set:
                    if short not in missing_blocks:
                        missing_blocks.append(short)
                    continue
                if props:
                    allowed = checked_props.get(short)
                    if allowed is None:
                        data = json.loads(z.read(bs_path))
                        allowed = set()
                        for vkey in data.get("variants", {}).keys():
                            if vkey:
                                for pair in vkey.split(","):
                                    if "=" in pair:
                                        allowed.add(pair.split("=", 1)[0])
                        for part in data.get("multipart", []):
                            when = part.get("when")
                            if isinstance(when, dict):
                                for wk, wv in when.items():
                                    if wk in ("OR", "AND") and isinstance(wv, list):
                                        for sub in wv:
                                            if isinstance(sub, dict):
                                                allowed.update(sub.keys())
                                    else:
                                        allowed.add(wk)
                        # Vanilla non-visual blockstate properties that aren't in blockstates JSON:
                        allowed.update({"waterlogged", "powered", "distance", "persistent", "drag", "level", "signal_fire"})
                        # Entity-rendered blocks whose blockstates JSON only has an empty "" variant in MC 26.2:
                        if short in ("chest", "trapped_chest") or short.endswith("copper_chest"):
                            allowed.update({"facing", "type", "waterlogged"})
                        elif short == "ender_chest":
                            allowed.update({"facing", "waterlogged"})
                        elif short == "kelp":
                            allowed.update({"age"})
                        elif short.endswith("_wall_sign"):
                            allowed.update({"facing", "waterlogged"})
                        elif short.endswith("_sign"):
                            allowed.update({"rotation", "waterlogged"})
                        elif short.endswith("copper_golem_statue"):
                            allowed.update({"facing", "copper_golem_pose", "waterlogged"})
                        checked_props[short] = allowed
                    for pk, pv in props:
                        if pk not in allowed:
                            msg = f"{short}: unknown prop '{pk}={pv}' (known={sorted(allowed)})"
                            if msg not in invalid_props:
                                invalid_props.append(msg)
        return missing_blocks, invalid_props

    def bake_lighting(self) -> Tuple[bytearray, bytearray]:
        """Compute accurate 3D SkyLight & BlockLight over the 256x128x256 canvas."""
        total = SIZE_X * SIZE_Y * SIZE_Z
        sky = bytearray(total)
        blk = bytearray(total)

        # Precompute lookup tables per state id
        opacities = [block_light_opacity(name) for name, _ in self.states]
        emissions = [block_emission_level(name, props) for name, props in self.states]
        grid = self.grid

        # 1. SkyLight vertical pass from top (CANVAS_MAX_Y down to CANVAS_MIN_Y)
        sky_q: deque[int] = deque()
        for z in range(SIZE_Z):
            z_off = z << 8
            for x in range(SIZE_X):
                light = 15
                for y in range(SIZE_Y - 1, -1, -1):
                    idx = (y << 16) | z_off | x
                    op = opacities[grid[idx]]
                    if op >= 15:
                        light = 0
                        break
                    if op > 0:
                        light = max(0, light - op)
                    if light > 0:
                        sky[idx] = light
                        if light < 15:
                            sky_q.append(idx)
                        else:
                            # Check if any horizontal neighbor will need propagation (enqueue boundary only)
                            sky_q.append(idx)

        # Actually optimize sky_q: only enqueue cells adjacent to a dimmer cell
        sky_q2: deque[int] = deque()
        for idx in sky_q:
            lv = sky[idx]
            if lv <= 1:
                continue
            x = idx & 255
            z = (idx >> 8) & 255
            y = idx >> 16
            need = False
            if x > 0 and sky[idx - 1] < lv - 1 and opacities[grid[idx - 1]] < 15:
                need = True
            elif x < 255 and sky[idx + 1] < lv - 1 and opacities[grid[idx + 1]] < 15:
                need = True
            elif z > 0 and sky[idx - 256] < lv - 1 and opacities[grid[idx - 256]] < 15:
                need = True
            elif z < 255 and sky[idx + 256] < lv - 1 and opacities[grid[idx + 256]] < 15:
                need = True
            elif y > 0 and sky[idx - 65536] < lv - 1 and opacities[grid[idx - 65536]] < 15:
                need = True
            if need:
                sky_q2.append(idx)

        # SkyLight BFS
        while sky_q2:
            idx = sky_q2.popleft()
            lv = sky[idx]
            if lv <= 1:
                continue
            x = idx & 255
            z = (idx >> 8) & 255
            y = idx >> 16
            if x > 0:
                n = idx - 1
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > sky[n]:
                    sky[n] = lv - att
                    sky_q2.append(n)
            if x < 255:
                n = idx + 1
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > sky[n]:
                    sky[n] = lv - att
                    sky_q2.append(n)
            if z > 0:
                n = idx - 256
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > sky[n]:
                    sky[n] = lv - att
                    sky_q2.append(n)
            if z < 255:
                n = idx + 256
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > sky[n]:
                    sky[n] = lv - att
                    sky_q2.append(n)
            if y > 0:
                n = idx - 65536
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > sky[n]:
                    sky[n] = lv - att
                    sky_q2.append(n)
            if y < SIZE_Y - 1:
                n = idx + 65536
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > sky[n]:
                    sky[n] = lv - att
                    sky_q2.append(n)

        # 2. BlockLight BFS
        blk_q: deque[int] = deque()
        for idx, sid in enumerate(grid):
            em = emissions[sid]
            if em > 0:
                blk[idx] = em
                blk_q.append(idx)

        while blk_q:
            idx = blk_q.popleft()
            lv = blk[idx]
            if lv <= 1:
                continue
            x = idx & 255
            z = (idx >> 8) & 255
            y = idx >> 16
            if x > 0:
                n = idx - 1
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > blk[n]:
                    blk[n] = lv - att
                    blk_q.append(n)
            if x < 255:
                n = idx + 1
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > blk[n]:
                    blk[n] = lv - att
                    blk_q.append(n)
            if z > 0:
                n = idx - 256
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > blk[n]:
                    blk[n] = lv - att
                    blk_q.append(n)
            if z < 255:
                n = idx + 256
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > blk[n]:
                    blk[n] = lv - att
                    blk_q.append(n)
            if y > 0:
                n = idx - 65536
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > blk[n]:
                    blk[n] = lv - att
                    blk_q.append(n)
            if y < SIZE_Y - 1:
                n = idx + 65536
                att = max(1, opacities[grid[n]])
                if att < 15 and lv - att > blk[n]:
                    blk[n] = lv - att
                    blk_q.append(n)

        self.last_sky = sky
        self.last_blk = blk
        return sky, blk

    def get_baked_sky_light(self, x: int, y: int, z: int) -> int:
        if self.last_sky is None:
            self.bake_lighting()
        assert self.last_sky is not None
        if not self.in_bounds(x, y, z):
            return 15
        idx = ((y - CANVAS_MIN_Y) << 16) | ((z - MIN_Z) << 8) | (x - MIN_X)
        return int(self.last_sky[idx])

    def audit_waypoints(self) -> Dict[str, Any]:
        """Verify every waypoint has valid standing space, solid support, and unobstructed 3-block sightline."""
        issues: List[str] = []
        details: Dict[str, Any] = {}
        non_blocking = {
            "air", "cave_air", "void_air", "water", "bubble_column",
            "short_grass", "tall_grass", "fern", "seagrass", "tall_seagrass",
            "kelp", "kelp_plant", "torch", "soul_torch", "copper_torch", "redstone_torch",
        }
        for key, (wx, wy, wz, yaw, pitch, label) in self.waypoints.items():
            ix, iy, iz = int(math.floor(wx)), int(math.floor(wy)), int(math.floor(wz))
            b_feet = self.get_short(ix, iy, iz)
            b_head = self.get_short(ix, iy + 1, iz)
            b_below = self.get_short(ix, iy - 1, iz)
            is_underwater = ("underwater" in key) or ("snell" in key) or ("vault" in key)
            allowed_body = ("water", "bubble_column") if is_underwater else ("air",)

            wp_issues: List[str] = []
            if b_feet not in allowed_body:
                wp_issues.append(f"feet in '{b_feet}' (expected {allowed_body})")
            if b_head not in allowed_body:
                wp_issues.append(f"head in '{b_head}' (expected {allowed_body})")
            if not is_underwater and b_below in ("air", "cave_air", "void_air", "water", "lava", "bubble_column"):
                wp_issues.append(f"unsupported floor below='{b_below}'")

            yr = math.radians(yaw)
            pr = math.radians(pitch)
            dx = -math.sin(yr) * math.cos(pr)
            dy = -math.sin(pr)
            dz = math.cos(yr) * math.cos(pr)
            blocked: List[str] = []
            for t in (1.0, 2.0, 3.0):
                tx = int(math.floor(wx + dx * t))
                ty = int(math.floor(wy + 1.62 + dy * t))
                tz = int(math.floor(wz + dz * t))
                tb = self.get_short(tx, ty, tz)
                if tb not in non_blocking and "sign" not in tb and "button" not in tb:
                    blocked.append(f"t={t:.0f}({tx},{ty},{tz}):{tb}")
            if blocked:
                wp_issues.append("sightline blocked: " + ", ".join(blocked))

            if wp_issues:
                issues.append(f"{key}: " + "; ".join(wp_issues))
            details[key] = {
                "feet": b_feet,
                "head": b_head,
                "below": b_below,
                "blocked_3b": blocked,
                "ok": len(wp_issues) == 0,
            }
        return {"ok": len(issues) == 0, "issues": issues, "waypoints": details}

    def audit_dark_zones(self, probes: Sequence[Tuple[str, int, int, int]]) -> Dict[str, Any]:
        """Verify baked SkyLight == 0 at interior probe coordinates inside enclosed dark halls."""
        if self.last_sky is None:
            self.bake_lighting()
        results: Dict[str, Any] = {}
        leaks: List[str] = []
        for name, x, y, z in probes:
            sl = self.get_baked_sky_light(x, y, z)
            blk = self.get_short(x, y, z)
            results[name] = {"pos": [x, y, z], "block": blk, "sky_light": sl, "ok": sl == 0}
            if sl != 0:
                leaks.append(f"{name} at ({x},{y},{z}) has SkyLight={sl} (block={blk})")
        return {"ok": len(leaks) == 0, "leaks": leaks, "probes": results}

    def audit_attachments(self) -> Dict[str, Any]:
        """Verify wall signs, wall torches, buttons, levers, and hanging lanterns have valid solid supports."""
        back_dir = {"north": (0, 0, 1), "south": (0, 0, -1), "east": (-1, 0, 0), "west": (1, 0, 0)}
        invalid_sup = {"air", "cave_air", "void_air", "water", "lava", "bubble_column"}
        issues: List[str] = []
        for idx, sid in enumerate(self.grid):
            name, props = self.states[sid]
            sh = name.removeprefix("minecraft:")
            p = dict(props)
            if "wall_sign" in sh or "wall_torch" in sh:
                x = (idx & 255) + MIN_X
                z = ((idx >> 8) & 255) + MIN_Z
                y = (idx >> 16) + CANVAS_MIN_Y
                dx, dy, dz = back_dir[p["facing"]]
                sup = self.get_short(x + dx, y + dy, z + dz)
                if sup in invalid_sup:
                    issues.append(f"{sh} at ({x},{y},{z}) facing={p['facing']} unsupported ({sup})")
            elif sh in ("stone_button", "lever"):
                x = (idx & 255) + MIN_X
                z = ((idx >> 8) & 255) + MIN_Z
                y = (idx >> 16) + CANVAS_MIN_Y
                face = p.get("face", "wall")
                if face == "wall":
                    dx, dy, dz = back_dir[p["facing"]]
                    sup = self.get_short(x + dx, y + dy, z + dz)
                elif face == "floor":
                    sup = self.get_short(x, y - 1, z)
                else:
                    sup = self.get_short(x, y + 1, z)
                if sup in invalid_sup:
                    issues.append(f"{sh} at ({x},{y},{z}) face={face} unsupported ({sup})")
            elif sh.endswith("_lantern") or sh == "lantern":
                if sh == "sea_lantern" or sh == "jack_o_lantern":
                    continue
                x = (idx & 255) + MIN_X
                z = ((idx >> 8) & 255) + MIN_Z
                y = (idx >> 16) + CANVAS_MIN_Y
                sup = self.get_short(x, y + 1, z) if p.get("hanging") == "true" else self.get_short(x, y - 1, z)
                if sup in invalid_sup:
                    issues.append(f"{sh} at ({x},{y},{z}) hanging={p.get('hanging')} unsupported ({sup})")
        return {"ok": len(issues) == 0, "count": len(issues), "issues": issues[:30]}

    def audit_lights(self) -> Dict[str, Any]:
        """Count total emitters and 80-block-radius active emitters per waypoint (matching MegaLights)."""
        emissions = [block_emission_level(name, props) for name, props in self.states]
        emitters: List[Tuple[int, int, int, str, int]] = []
        grid = self.grid
        for idx, sid in enumerate(grid):
            em = emissions[sid]
            if em > 0:
                x = (idx & 255) + MIN_X
                z = ((idx >> 8) & 255) + MIN_Z
                y = (idx >> 16) + CANVAS_MIN_Y
                emitters.append((x, y, z, self.states[sid][0].removeprefix("minecraft:"), em))

        per_wp: Dict[str, Dict[str, Any]] = {}
        r2 = 80.0 * 80.0
        for key, (wx, wy, wz, _, _, label) in self.waypoints.items():
            within_80 = 0
            within_32 = 0
            for ex, ey, ez, _, _ in emitters:
                d2 = (ex + 0.5 - wx) ** 2 + (ey + 0.5 - wy) ** 2 + (ez + 0.5 - wz) ** 2
                if d2 <= r2:
                    within_80 += 1
                    if d2 <= 32.0 * 32.0:
                        within_32 += 1
            per_wp[key] = {
                "label": label,
                "pos": (wx, wy, wz),
                "lights_within_80": within_80,
                "lights_within_32": within_32,
            }
        return {"total_emitters": len(emitters), "waypoints": per_wp}

    def export_save(
        self,
        save_dir: str,
        template_save_dir: str,
        level_name: str = "Prism Foundry RTX Showcase",
        spawn_xyz: Tuple[int, int, int] = (0, MAIN_FLOOR_Y + 1, 14),
        spawn_yaw: float = 180.0,
        spawn_pitch: float = -4.0,
        initial_time_ticks: int = 2400,
        pre_connected: Optional[int] = None,
    ) -> Dict[str, Any]:
        """Auto-connect blocks, bake lighting, and write complete MC 26.2 Anvil save."""
        connected = self.auto_connect_blocks() if pre_connected is None else pre_connected
        sky, blk = self.bake_lighting()

        create_save_metadata(
            save_dir=save_dir,
            template_save_dir=template_save_dir,
            level_name=level_name,
            spawn_xyz=spawn_xyz,
            spawn_yaw=spawn_yaw,
            spawn_pitch=spawn_pitch,
            initial_time_ticks=initial_time_ticks,
            enabled_datapacks=["vanilla", "file/prism-foundry"],
        )

        # Group block entities and entities by chunk
        be_by_chunk: Dict[Tuple[int, int], List[Dict[str, Tuple[int, Any]]]] = {}
        for (bx, by, bz), be in self.block_entities.items():
            be_by_chunk.setdefault((bx >> 4, bz >> 4), []).append(be)

        ent_by_chunk: Dict[Tuple[int, int], List[Dict[str, Tuple[int, Any]]]] = {}
        for ent in self.entities:
            pos = ent["Pos"][1][1]
            cx = int(math.floor(pos[0])) >> 4
            cz = int(math.floor(pos[2])) >> 4
            ent_by_chunk.setdefault((cx, cz), []).append(ent)

        # Static subterranean base sections (Y=-64..23 -> sy=-4..1)
        bedrock = norm_block("bedrock")
        deepslate = norm_block("deepslate", {"axis": "y"})
        stone = norm_block("stone")
        air = norm_block("air")

        sec_m4 = [bedrock if (i >> 8) == 0 else deepslate for i in range(4096)]
        sec_deepslate = [deepslate] * 4096
        sec_stone = [stone] * 4096
        zero_light = [0] * 4096
        full_sky = [15] * 4096

        # Precompute heightmap predicates per state id
        is_air_id = [name in ("minecraft:air", "minecraft:cave_air", "minecraft:void_air") for name, _ in self.states]
        is_mb_id = [is_motion_blocking(name) for name, _ in self.states]
        is_leaves_id = ["leaves" in name for name, _ in self.states]
        is_water_id = [name in ("minecraft:water", "minecraft:bubble_column") for name, _ in self.states]

        region_chunks: Dict[Tuple[int, int], Dict[Tuple[int, int], bytes]] = {}
        entity_regions: Dict[Tuple[int, int], Dict[Tuple[int, int], bytes]] = {}
        grid = self.grid
        states = self.states

        for cx in range(MIN_X >> 4, (MAX_X >> 4) + 1):
            for cz in range(MIN_Z >> 4, (MAX_Z >> 4) + 1):
                wx0 = cx * 16
                wz0 = cz * 16

                section_blocks: Dict[int, List[BlockState]] = {
                    -4: sec_m4,
                    -3: sec_deepslate,
                    -2: sec_deepslate,
                    -1: sec_deepslate,
                    0: sec_deepslate,
                    1: [deepslate if (i >> 8) == 0 else stone for i in range(4096)],
                }
                # Above CANVAS_MAX_Y (151 -> sy=9 ends at 159), sy=10..19 are air with sky=15
                section_sky: Dict[int, List[int]] = {}
                section_blk: Dict[int, List[int]] = {}
                for sy in range(10, MAX_SECTION_Y + 1):
                    section_sky[sy] = full_sky

                # Sections sy=1..9 intersect canvas [24..151]
                # Note sy=1 is Y=16..31 (bottom half Y=16..23 stone, top half Y=24..31 canvas)
                for sy in range(1, 10):
                    y_base = sy * 16
                    s_blocks = [air] * 4096
                    s_sky = [15 if sy >= 9 else 0] * 4096
                    s_blk = [0] * 4096
                    has_sky = False
                    has_blk = False
                    for ly in range(16):
                        wy = y_base + ly
                        ly_off = ly << 8
                        if wy < CANVAS_MIN_Y:
                            fill_st = deepslate if wy == 0 else stone
                            for i in range(256):
                                s_blocks[ly_off + i] = fill_st
                        elif wy <= CANVAS_MAX_Y:
                            cy_off = (wy - CANVAS_MIN_Y) << 16
                            for lz in range(16):
                                wz = wz0 + lz
                                c_row = cy_off | ((wz - MIN_Z) << 8)
                                l_row = ly_off | (lz << 4)
                                for lx in range(16):
                                    wx = wx0 + lx
                                    c_idx = c_row | (wx - MIN_X)
                                    l_idx = l_row | lx
                                    s_blocks[l_idx] = states[grid[c_idx]]
                                    sv = sky[c_idx]
                                    bv = blk[c_idx]
                                    s_sky[l_idx] = sv
                                    s_blk[l_idx] = bv
                                    if sv:
                                        has_sky = True
                                    if bv:
                                        has_blk = True
                        else:
                            # wy > CANVAS_MAX_Y (152..159): air with skylight 15
                            for i in range(256):
                                s_sky[ly_off + i] = 15
                            has_sky = True

                    section_blocks[sy] = s_blocks
                    if has_sky:
                        section_sky[sy] = s_sky
                    if has_blk:
                        section_blk[sy] = s_blk

                # Heightmaps (256 columns, index = lz*16 + lx, value = top_y + 1 - WORLD_MIN_Y)
                hm_ws = [0] * 256
                hm_mb = [0] * 256
                hm_mb_nl = [0] * 256
                hm_of = [0] * 256
                for lz in range(16):
                    wz = wz0 + lz
                    z_off = (wz - MIN_Z) << 8
                    for lx in range(16):
                        wx = wx0 + lx
                        col_idx = (lz << 4) | lx
                        x_off = wx - MIN_X
                        ws = mb = mb_nl = of = CANVAS_MIN_Y
                        for wy in range(CANVAS_MAX_Y, CANVAS_MIN_Y - 1, -1):
                            sid = grid[((wy - CANVAS_MIN_Y) << 16) | z_off | x_off]
                            if not is_air_id[sid]:
                                if ws == CANVAS_MIN_Y:
                                    ws = wy + 1
                                if is_mb_id[sid]:
                                    if mb == CANVAS_MIN_Y:
                                        mb = wy + 1
                                    if not is_leaves_id[sid] and mb_nl == CANVAS_MIN_Y:
                                        mb_nl = wy + 1
                                    if not is_water_id[sid] and not is_leaves_id[sid] and of == CANVAS_MIN_Y:
                                        of = wy + 1
                                if ws > CANVAS_MIN_Y and mb > CANVAS_MIN_Y and mb_nl > CANVAS_MIN_Y and of > CANVAS_MIN_Y:
                                    break
                        hm_ws[col_idx] = ws - WORLD_MIN_Y
                        hm_mb[col_idx] = mb - WORLD_MIN_Y
                        hm_mb_nl[col_idx] = mb_nl - WORLD_MIN_Y
                        hm_of[col_idx] = of - WORLD_MIN_Y

                biome_id = self.chunk_biomes.get((cx, cz), "minecraft:warm_ocean")
                chunk_bytes = encode_chunk_nbt(
                    cx=cx,
                    cz=cz,
                    section_blocks=section_blocks,
                    section_sky=section_sky,
                    section_blk=section_blk,
                    biome_id=biome_id,
                    block_entities=be_by_chunk.get((cx, cz), []),
                    heightmaps={
                        "WORLD_SURFACE": hm_ws,
                        "MOTION_BLOCKING": hm_mb,
                        "MOTION_BLOCKING_NO_LEAVES": hm_mb_nl,
                        "OCEAN_FLOOR": hm_of,
                    },
                )
                rx, rz = cx >> 5, cz >> 5
                region_chunks.setdefault((rx, rz), {})[(cx & 31, cz & 31)] = chunk_bytes

                ents = ent_by_chunk.get((cx, cz))
                if ents:
                    eb = encode_entity_chunk_nbt(cx, cz, ents)
                    entity_regions.setdefault((rx, rz), {})[(cx & 31, cz & 31)] = eb

        for (rx, rz), cdict in region_chunks.items():
            out_path = os.path.join(save_dir, "dimensions", "minecraft", "overworld", "region", f"r.{rx}.{rz}.mca")
            write_mca_file(out_path, cdict)
        for (rx, rz), edict in entity_regions.items():
            out_path = os.path.join(save_dir, "dimensions", "minecraft", "overworld", "entities", f"r.{rx}.{rz}.mca")
            write_mca_file(out_path, edict)

        return {
            "connected_blocks": connected,
            "unique_states": len(self.states),
            "block_entities": len(self.block_entities),
            "entities": len(self.entities),
            "chunks_written": sum(len(v) for v in region_chunks.values()),
        }

    def render_previews(self, out_dir: str) -> List[str]:
        """Generate crisp diagnostic top-down & cross-section PNGs for visual inspection."""
        os.makedirs(out_dir, exist_ok=True)
        color_cache: List[Tuple[int, int, int]] = [_block_color(name) for name, _ in self.states]
        is_air = [name in ("minecraft:air", "minecraft:cave_air", "minecraft:void_air") for name, _ in self.states]
        grid = self.grid
        written: List[str] = []

        # 1. Surface top-down map with subtle height shading
        img = Image.new("RGB", (SIZE_X, SIZE_Z), (20, 40, 70))
        px = img.load()
        for z in range(SIZE_Z):
            z_off = z << 8
            for x in range(SIZE_X):
                for y in range(SIZE_Y - 1, -1, -1):
                    sid = grid[(y << 16) | z_off | x]
                    if not is_air[sid]:
                        r, g, b = color_cache[sid]
                        shade = max(0.65, min(1.25, 0.78 + (y - 38) * 0.008))
                        px[x, z] = (min(255, int(r * shade)), min(255, int(g * shade)), min(255, int(b * shade)))
                        break
        img_big = img.resize((SIZE_X * 3, SIZE_Z * 3), Image.NEAREST)
        draw = ImageDraw.Draw(img_big)
        for key, (wx, wy, wz, _, _, _) in self.waypoints.items():
            sx = int((wx - MIN_X) * 3)
            sz = int((wz - MIN_Z) * 3)
            draw.ellipse((sx - 5, sz - 5, sx + 5, sz + 5), fill=(255, 50, 50), outline=(255, 255, 255))
            draw.text((sx + 7, sz - 6), key, fill=(255, 255, 180))
        p_top = os.path.join(out_dir, "top_surface.png")
        img_big.save(p_top)
        written.append(p_top)

        # 2. Horizontal slices at Y=66 (main floor) and Y=46 (underground/underwater)
        for wy, fname in ((66, "slice_y66.png"), (46, "slice_y46.png")):
            simg = Image.new("RGB", (SIZE_X, SIZE_Z), (12, 12, 16))
            spx = simg.load()
            y_off = (wy - CANVAS_MIN_Y) << 16
            for z in range(SIZE_Z):
                row_off = y_off | (z << 8)
                for x in range(SIZE_X):
                    sid = grid[row_off | x]
                    spx[x, z] = (18, 20, 26) if is_air[sid] else color_cache[sid]
            sbig = simg.resize((SIZE_X * 3, SIZE_Z * 3), Image.NEAREST)
            p_sl = os.path.join(out_dir, fname)
            sbig.save(p_sl)
            written.append(p_sl)

        # 3. Cross sections along Z=0 (East-West) and X=0 (North-South)
        cs = Image.new("RGB", (SIZE_X, SIZE_Y * 2 + 12), (10, 12, 18))
        cpx = cs.load()
        z0 = 0 - MIN_Z
        for x in range(SIZE_X):
            for y in range(SIZE_Y):
                sid = grid[(y << 16) | (z0 << 8) | x]
                cpx[x, SIZE_Y - 1 - y] = (18, 24, 38) if is_air[sid] else color_cache[sid]
        x0 = 0 - MIN_X
        for z in range(SIZE_Z):
            for y in range(SIZE_Y):
                sid = grid[(y << 16) | (z << 8) | x0]
                cpx[z, SIZE_Y + 12 + (SIZE_Y - 1 - y)] = (18, 24, 38) if is_air[sid] else color_cache[sid]
        cs_big = cs.resize((SIZE_X * 3, (SIZE_Y * 2 + 12) * 3), Image.NEAREST)
        p_cs = os.path.join(out_dir, "cross_sections.png")
        cs_big.save(p_cs)
        written.append(p_cs)

        return written


def _block_color(full_name: str) -> Tuple[int, int, int]:
    s = full_name.removeprefix("minecraft:")
    if s in ("water", "bubble_column"):
        return (36, 118, 210)
    if s in ("lava", "magma_block"):
        return (245, 92, 24)
    if "gold" in s:
        return (245, 208, 66)
    if "iron" in s:
        return (210, 214, 220)
    if "oxidized" in s:
        return (82, 168, 138)
    if "weathered" in s:
        return (108, 158, 122)
    if "exposed" in s:
        return (168, 126, 102)
    if "copper" in s:
        return (206, 118, 82)
    if "netherite" in s or "obsidian" in s or "deepslate" in s or "blackstone" in s:
        return (56, 54, 68)
    if "quartz" in s or "white" in s or "snow" in s or "calcite" in s:
        return (238, 236, 230)
    if "sand" in s:
        return (222, 206, 152)
    if "grass" in s:
        return (96, 162, 74)
    if "leaves" in s:
        return (62, 132, 58)
    if "ice" in s:
        return (142, 204, 250)
    if "diamond" in s:
        return (96, 226, 220)
    if "emerald" in s:
        return (56, 210, 104)
    if "amethyst" in s or "purple" in s or "magenta" in s or "portal" in s:
        return (156, 86, 218)
    if "red" in s or "crimson" in s:
        return (214, 58, 54)
    if "orange" in s:
        return (235, 132, 46)
    if "yellow" in s or "glowstone" in s or "ochre" in s:
        return (246, 220, 108)
    if "green" in s or "lime" in s or "verdant" in s:
        return (86, 194, 78)
    if "cyan" in s or "soul" in s or "sea_lantern" in s or "prismarine" in s or "warped" in s:
        return (72, 204, 224)
    if "blue" in s or "lapis" in s:
        return (58, 96, 212)
    if "glass" in s:
        return (195, 228, 240)
    if "oak" in s or "spruce" in s or "birch" in s or "wood" in s or "planks" in s or "log" in s:
        return (156, 118, 76)
    if "lantern" in s or "torch" in s or "lamp" in s or "bulb" in s or "end_rod" in s or "beacon" in s:
        return (255, 244, 178)
    return (138, 142, 148)
