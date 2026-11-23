"""Minimal zero-dependency Minecraft 26.2 (DataVersion 4903) NBT & Anvil (.mca) serializer."""

from __future__ import annotations

import gzip
import math
import os
import shutil
import struct
import time
import zlib
from collections import deque
from typing import Any, Dict, Iterable, List, Optional, Tuple

DATA_VERSION = 4903
MIN_SECTION_Y = -4
MAX_SECTION_Y = 19  # inclusive -> 24 sections, Y in [-64, 319]
WORLD_MIN_Y = MIN_SECTION_Y * 16  # -64
WORLD_HEIGHT = (MAX_SECTION_Y - MIN_SECTION_Y + 1) * 16  # 384

# NBT tag IDs
TAG_END = 0
TAG_BYTE = 1
TAG_SHORT = 2
TAG_INT = 3
TAG_LONG = 4
TAG_FLOAT = 5
TAG_DOUBLE = 6
TAG_BYTE_ARRAY = 7
TAG_STRING = 8
TAG_LIST = 9
TAG_COMPOUND = 10
TAG_INT_ARRAY = 11
TAG_LONG_ARRAY = 12


# Convenience constructors returning tagged tuples: (tag_id, value)
def nbt_byte(v: int) -> Tuple[int, int]:
    return (TAG_BYTE, int(v))


def nbt_short(v: int) -> Tuple[int, int]:
    return (TAG_SHORT, int(v))


def nbt_int(v: int) -> Tuple[int, int]:
    return (TAG_INT, int(v))


def nbt_long(v: int) -> Tuple[int, int]:
    return (TAG_LONG, int(v))


def nbt_float(v: float) -> Tuple[int, float]:
    return (TAG_FLOAT, float(v))


def nbt_double(v: float) -> Tuple[int, float]:
    return (TAG_DOUBLE, float(v))


def nbt_byte_array(v: bytes | bytearray) -> Tuple[int, bytes]:
    return (TAG_BYTE_ARRAY, bytes(v))


def nbt_string(v: str) -> Tuple[int, str]:
    return (TAG_STRING, str(v))


def nbt_list(elem_tag: int, items: List[Any]) -> Tuple[int, Tuple[int, List[Any]]]:
    """items are raw payloads matching elem_tag (or tagged tuples if unwrapped)."""
    raw_items = []
    for item in items:
        if isinstance(item, tuple) and len(item) == 2 and isinstance(item[0], int) and item[0] == elem_tag:
            raw_items.append(item[1])
        else:
            raw_items.append(item)
    return (TAG_LIST, (elem_tag, raw_items))


def nbt_compound(d: Dict[str, Tuple[int, Any]]) -> Tuple[int, Dict[str, Tuple[int, Any]]]:
    return (TAG_COMPOUND, d)


def nbt_int_array(vals: Iterable[int]) -> Tuple[int, List[int]]:
    return (TAG_INT_ARRAY, [int(x) for x in vals])


def nbt_long_array(vals: Iterable[int]) -> Tuple[int, List[int]]:
    return (TAG_LONG_ARRAY, [int(x) for x in vals])


def _write_str(out: bytearray, s: str) -> None:
    b = s.encode("utf-8")
    out += struct.pack(">H", len(b))
    out += b


def _write_payload(out: bytearray, tag: int, val: Any) -> None:
    if tag == TAG_BYTE:
        out += struct.pack(">b", val)
    elif tag == TAG_SHORT:
        out += struct.pack(">h", val)
    elif tag == TAG_INT:
        out += struct.pack(">i", val)
    elif tag == TAG_LONG:
        out += struct.pack(">q", val)
    elif tag == TAG_FLOAT:
        out += struct.pack(">f", val)
    elif tag == TAG_DOUBLE:
        out += struct.pack(">d", val)
    elif tag == TAG_BYTE_ARRAY:
        out += struct.pack(">i", len(val))
        out += val
    elif tag == TAG_STRING:
        _write_str(out, val)
    elif tag == TAG_LIST:
        elem_tag, items = val
        if not items:
            out += struct.pack(">bi", TAG_END if elem_tag == TAG_END else elem_tag, 0)
        else:
            out += struct.pack(">bi", elem_tag, len(items))
            for item in items:
                _write_payload(out, elem_tag, item)
    elif tag == TAG_COMPOUND:
        for k, (ct, cv) in val.items():
            out.append(ct)
            _write_str(out, k)
            _write_payload(out, ct, cv)
        out.append(TAG_END)
    elif tag == TAG_INT_ARRAY:
        out += struct.pack(">i", len(val))
        if val:
            out += struct.pack(f">{len(val)}i", *val)
    elif tag == TAG_LONG_ARRAY:
        out += struct.pack(">i", len(val))
        if val:
            out += struct.pack(f">{len(val)}q", *val)
    else:
        raise ValueError(f"Unsupported NBT tag: {tag}")


def dumps_nbt(root_compound: Tuple[int, Dict[str, Any]], root_name: str = "") -> bytes:
    tag, payload = root_compound
    assert tag == TAG_COMPOUND
    out = bytearray()
    out.append(TAG_COMPOUND)
    _write_str(out, root_name)
    _write_payload(out, TAG_COMPOUND, payload)
    return bytes(out)


def loads_nbt(buf: bytes) -> Tuple[str, Tuple[int, Any]]:
    off = 0

    def r_u8() -> int:
        nonlocal off
        v = buf[off]
        off += 1
        return v

    def r_str() -> str:
        nonlocal off
        n = struct.unpack_from(">H", buf, off)[0]
        off += 2
        s = buf[off : off + n].decode("utf-8")
        off += n
        return s

    def r_pay(t: int) -> Any:
        nonlocal off
        if t == TAG_BYTE:
            v = struct.unpack_from(">b", buf, off)[0]
            off += 1
            return v
        if t == TAG_SHORT:
            v = struct.unpack_from(">h", buf, off)[0]
            off += 2
            return v
        if t == TAG_INT:
            v = struct.unpack_from(">i", buf, off)[0]
            off += 4
            return v
        if t == TAG_LONG:
            v = struct.unpack_from(">q", buf, off)[0]
            off += 8
            return v
        if t == TAG_FLOAT:
            v = struct.unpack_from(">f", buf, off)[0]
            off += 4
            return v
        if t == TAG_DOUBLE:
            v = struct.unpack_from(">d", buf, off)[0]
            off += 8
            return v
        if t == TAG_BYTE_ARRAY:
            n = struct.unpack_from(">i", buf, off)[0]
            off += 4
            v = bytes(buf[off : off + n])
            off += n
            return v
        if t == TAG_STRING:
            return r_str()
        if t == TAG_LIST:
            et = r_u8()
            n = struct.unpack_from(">i", buf, off)[0]
            off += 4
            return (et, [r_pay(et) for _ in range(n)])
        if t == TAG_COMPOUND:
            d: Dict[str, Tuple[int, Any]] = {}
            while True:
                ct = r_u8()
                if ct == TAG_END:
                    break
                cn = r_str()
                d[cn] = (ct, r_pay(ct))
            return d
        if t == TAG_INT_ARRAY:
            n = struct.unpack_from(">i", buf, off)[0]
            off += 4
            v = list(struct.unpack_from(f">{n}i", buf, off))
            off += 4 * n
            return v
        if t == TAG_LONG_ARRAY:
            n = struct.unpack_from(">i", buf, off)[0]
            off += 4
            v = list(struct.unpack_from(f">{n}q", buf, off))
            off += 8 * n
            return v
        raise ValueError(f"Unknown NBT tag {t}")

    root_tag = r_u8()
    root_name = r_str()
    return root_name, (root_tag, r_pay(root_tag))


def pack_padded_longs(values: List[int], bits: int) -> List[int]:
    """Pack values into 64-bit signed longs without spanning long boundaries (MC 1.16+ format)."""
    per_long = 64 // bits
    mask = (1 << bits) - 1
    out: List[int] = []
    n = len(values)
    for base in range(0, n, per_long):
        acc = 0
        end = min(base + per_long, n)
        for i in range(base, end):
            acc |= (values[i] & mask) << ((i - base) * bits)
        # Convert unsigned 64-bit to signed int64
        if acc >= (1 << 63):
            acc -= (1 << 64)
        out.append(acc)
    return out


def write_mca_file(path: str, chunks: Dict[Tuple[int, int], bytes]) -> None:
    """Write a region .mca file from {(local_cx, local_cz): uncompressed_nbt_bytes}."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    locations = bytearray(4096)
    timestamps = bytearray(4096)
    body = bytearray()
    now = int(time.time())
    sector_cursor = 2  # sectors 0 and 1 are header

    for (lx, lz), raw_nbt in sorted(chunks.items()):
        assert 0 <= lx < 32 and 0 <= lz < 32
        compressed = zlib.compress(raw_nbt, level=6)
        payload_len = len(compressed) + 1  # +1 for compression type byte (2 = zlib)
        entry = struct.pack(">IB", payload_len, 2) + compressed
        sectors_needed = math.ceil(len(entry) / 4096)
        assert sectors_needed < 256, f"Chunk too large ({sectors_needed} sectors)"
        pad = sectors_needed * 4096 - len(entry)
        body += entry
        if pad:
            body += bytes(pad)

        idx = (lx + lz * 32) * 4
        loc_val = (sector_cursor << 8) | sectors_needed
        struct.pack_into(">I", locations, idx, loc_val)
        struct.pack_into(">I", timestamps, idx, now)
        sector_cursor += sectors_needed

    with open(path, "wb") as f:
        f.write(locations)
        f.write(timestamps)
        f.write(body)


def _nibble_array_from_levels(levels_4096: List[int]) -> bytes:
    """Pack 4096 4-bit light values (index = y*256 + z*16 + x) into 2048 bytes."""
    out = bytearray(2048)
    for i in range(0, 4096, 2):
        out[i >> 1] = (levels_4096[i] & 0xF) | ((levels_4096[i + 1] & 0xF) << 4)
    return bytes(out)


def block_emission_level(name: str, props: Tuple[Tuple[str, str], ...]) -> int:
    """Return Minecraft block light emission level (0..15) for accurate pre-baked lighting."""
    p = dict(props)
    short = name.removeprefix("minecraft:")
    if short in (
        "glowstone",
        "sea_lantern",
        "beacon",
        "lava",
        "fire",
        "jack_o_lantern",
        "ochre_froglight",
        "pearlescent_froglight",
        "verdant_froglight",
        "lantern",
        "shroomlight",
        "campfire",
        "conduit",
    ):
        if short == "campfire" and p.get("lit") == "false":
            return 0
        return 15
    if short in ("copper_lantern", "waxed_copper_lantern", "exposed_copper_lantern", "waxed_exposed_copper_lantern",
                 "weathered_copper_lantern", "waxed_weathered_copper_lantern", "oxidized_copper_lantern",
                 "waxed_oxidized_copper_lantern"):
        return 15
    if short in ("torch", "wall_torch", "end_rod", "copper_torch", "copper_wall_torch"):
        return 14
    if short == "redstone_lamp":
        return 15 if p.get("lit") == "true" else 0
    if short in ("copper_bulb", "waxed_copper_bulb"):
        return 15 if p.get("lit") == "true" else 0
    if short in ("exposed_copper_bulb", "waxed_exposed_copper_bulb"):
        return 12 if p.get("lit") == "true" else 0
    if short in ("weathered_copper_bulb", "waxed_weathered_copper_bulb"):
        return 8 if p.get("lit") == "true" else 0
    if short in ("oxidized_copper_bulb", "waxed_oxidized_copper_bulb"):
        return 4 if p.get("lit") == "true" else 0
    if short in ("furnace", "blast_furnace", "smoker"):
        return 13 if p.get("lit") == "true" else 0
    if short == "nether_portal":
        return 11
    if short in ("soul_torch", "soul_wall_torch", "soul_lantern", "soul_campfire", "soul_fire", "crying_obsidian"):
        if short == "soul_campfire" and p.get("lit") == "false":
            return 0
        return 10
    if short == "respawn_anchor":
        ch = int(p.get("charges", "0"))
        return [0, 3, 7, 11, 15][min(max(ch, 0), 4)]
    if short in ("redstone_torch", "redstone_wall_torch"):
        return 7 if p.get("lit", "true") == "true" else 0
    if short == "glow_lichen":
        return 7
    if short == "amethyst_cluster":
        return 5
    if short == "large_amethyst_bud":
        return 4
    if short == "magma_block":
        return 3
    if short == "enchanting_table":
        return 7
    if short == "sea_pickle":
        cnt = int(p.get("pickles", "1"))
        return 3 + 3 * cnt if p.get("waterlogged", "true") == "true" else 0
    return 0


def block_light_opacity(name: str) -> int:
    """How much light (0..15) is attenuated passing through this block."""
    short = name.removeprefix("minecraft:")
    if short in ("air", "cave_air", "void_air", "barrier", "light"):
        return 0
    if short == "tinted_glass":
        return 15  # MC tinted glass blocks all light while staying visually transparent!
    if (
        "glass" in short
        or "bars" in short
        or "grate" in short
        or "chain" in short
        or "fence" in short
        or "trapdoor" in short
        or "door" in short
        or "torch" in short
        or "lantern" in short
        or "sign" in short
        or "button" in short
        or "lever" in short
        or "ladder" in short
        or "rail" in short
        or "carpet" in short
        or "amethyst" in short and short != "amethyst_block" and short != "budding_amethyst"
        or short in (
            "end_rod",
            "campfire",
            "soul_campfire",
            "fire",
            "soul_fire",
            "nether_portal",
            "beacon",
            "enchanting_table",
            "anvil",
            "short_grass",
            "tall_grass",
            "fern",
            "dandelion",
            "poppy",
            "blue_orchid",
            "cornflower",
            "lily_of_the_valley",
            "redstone_wire",
            "seagrass",
            "tall_seagrass",
            "kelp",
            "kelp_plant",
            "sea_pickle",
            "chest",
            "copper_chest",
            "waxed_copper_chest",
            "copper_golem_statue",
            "waxed_copper_golem_statue",
            "waxed_exposed_copper_golem_statue",
            "waxed_weathered_copper_golem_statue",
            "waxed_oxidized_copper_golem_statue",
            "lightning_rod",
            "waxed_lightning_rod",
            "lily_pad",
            "glow_lichen",
            "sculk_vein",
            "scaffolding",
        )
        or "coral" in short and "block" not in short
        or "slab" in short
        or "stairs" in short
        or "wall" in short and "sign" not in short and "torch" not in short and "coral" not in short
    ):
        return 0
    if short in ("water", "bubble_column", "ice", "frosted_ice") or "leaves" in short:
        return 1
    return 15


def is_motion_blocking(name: str) -> bool:
    short = name.removeprefix("minecraft:")
    if short in (
        "air",
        "cave_air",
        "void_air",
        "short_grass",
        "tall_grass",
        "fern",
        "dandelion",
        "poppy",
        "blue_orchid",
        "cornflower",
        "lily_of_the_valley",
        "torch",
        "wall_torch",
        "soul_torch",
        "soul_wall_torch",
        "copper_torch",
        "copper_wall_torch",
        "redstone_torch",
        "redstone_wall_torch",
        "redstone_wire",
        "stone_button",
        "lever",
        "fire",
        "soul_fire",
        "nether_portal",
        "seagrass",
        "tall_seagrass",
        "kelp",
        "kelp_plant",
        "glow_lichen",
    ) or "rail" in short or "sign" in short:
        return False
    return True


def create_save_metadata(
    save_dir: str,
    template_save_dir: str,
    level_name: str,
    spawn_xyz: Tuple[int, int, int],
    spawn_yaw: float = 0.0,
    spawn_pitch: float = 0.0,
    initial_time_ticks: int = 2600,
    enabled_datapacks: Optional[List[str]] = None,
) -> None:
    """Create MC 26.2 save metadata by adapting a verified 26.2 template save."""
    os.makedirs(save_dir, exist_ok=True)
    for sub in (
        "data/minecraft",
        "datapacks",
        "dimensions/minecraft/overworld/region",
        "dimensions/minecraft/overworld/entities",
        "dimensions/minecraft/overworld/poi",
        "dimensions/minecraft/overworld/data",
        "players/data",
    ):
        os.makedirs(os.path.join(save_dir, sub), exist_ok=True)

    # 1. level.dat
    tpl_level = os.path.join(template_save_dir, "level.dat")
    with gzip.open(tpl_level, "rb") as f:
        _, tree = loads_nbt(f.read())
    data = tree[1]["Data"][1]
    data["LevelName"] = nbt_string(level_name)
    data["GameType"] = nbt_int(1)  # Creative
    data["allowCommands"] = nbt_byte(1)
    data["initialized"] = nbt_byte(1)
    data["Time"] = nbt_long(initial_time_ticks)
    data["LastPlayed"] = nbt_long(int(time.time() * 1000))
    data["difficulty_settings"] = nbt_compound(
        {
            "difficulty": nbt_string("peaceful"),
            "hardcore": nbt_byte(0),
            "locked": nbt_byte(0),
        }
    )
    data["spawn"] = nbt_compound(
        {
            "dimension": nbt_string("minecraft:overworld"),
            "pos": nbt_int_array([spawn_xyz[0], spawn_xyz[1], spawn_xyz[2]]),
            "yaw": nbt_float(spawn_yaw),
            "pitch": nbt_float(spawn_pitch),
        }
    )
    if "singleplayer_uuid" in data:
        del data["singleplayer_uuid"]
    packs = enabled_datapacks or ["vanilla", "file/prism-foundry"]
    data["DataPacks"] = nbt_compound(
        {
            "Enabled": nbt_list(TAG_STRING, packs),
            "Disabled": nbt_list(
                TAG_STRING,
                ["minecart_improvements", "redstone_experiments", "trade_rebalance"],
            ),
        }
    )
    with gzip.open(os.path.join(save_dir, "level.dat"), "wb") as f:
        f.write(dumps_nbt(tree))

    # 2. data/minecraft/world_gen_settings.dat -> infinite warm ocean at Y=62 outside generated chunks
    flat_layers = [
        {"block": nbt_string("minecraft:bedrock"), "height": nbt_int(1)},      # Y=-64
        {"block": nbt_string("minecraft:deepslate"), "height": nbt_int(64)},   # Y=-63..0
        {"block": nbt_string("minecraft:stone"), "height": nbt_int(32)},       # Y=1..32
        {"block": nbt_string("minecraft:sand"), "height": nbt_int(3)},         # Y=33..35
        {"block": nbt_string("minecraft:water"), "height": nbt_int(27)},       # Y=36..62
    ]
    wgs = nbt_compound(
        {
            "DataVersion": nbt_int(DATA_VERSION),
            "data": nbt_compound(
                {
                    "bonus_chest": nbt_byte(0),
                    "generate_structures": nbt_byte(0),
                    "seed": nbt_long(20261122),
                    "dimensions": nbt_compound(
                        {
                            "minecraft:overworld": nbt_compound(
                                {
                                    "type": nbt_string("minecraft:overworld"),
                                    "generator": nbt_compound(
                                        {
                                            "type": nbt_string("minecraft:flat"),
                                            "settings": nbt_compound(
                                                {
                                                    "biome": nbt_string("minecraft:warm_ocean"),
                                                    "features": nbt_byte(0),
                                                    "lakes": nbt_byte(0),
                                                    "layers": nbt_list(TAG_COMPOUND, flat_layers),
                                                    "structure_overrides": nbt_list(TAG_STRING, []),
                                                }
                                            ),
                                        }
                                    ),
                                }
                            ),
                            "minecraft:the_nether": nbt_compound(
                                {
                                    "type": nbt_string("minecraft:the_nether"),
                                    "generator": nbt_compound(
                                        {
                                            "type": nbt_string("minecraft:noise"),
                                            "settings": nbt_string("minecraft:nether"),
                                            "biome_source": nbt_compound(
                                                {
                                                    "type": nbt_string("minecraft:multi_noise"),
                                                    "preset": nbt_string("minecraft:nether"),
                                                }
                                            ),
                                        }
                                    ),
                                }
                            ),
                            "minecraft:the_end": nbt_compound(
                                {
                                    "type": nbt_string("minecraft:the_end"),
                                    "generator": nbt_compound(
                                        {
                                            "type": nbt_string("minecraft:noise"),
                                            "settings": nbt_string("minecraft:end"),
                                            "biome_source": nbt_compound(
                                                {"type": nbt_string("minecraft:the_end")}
                                            ),
                                        }
                                    ),
                                }
                            ),
                        }
                    ),
                }
            ),
        }
    )
    with gzip.open(os.path.join(save_dir, "data", "minecraft", "world_gen_settings.dat"), "wb") as f:
        f.write(dumps_nbt(wgs))

    # 3. data/minecraft/game_rules.dat (clone template and enforce clean showcase rules)
    tpl_rules = os.path.join(template_save_dir, "data", "minecraft", "game_rules.dat")
    with gzip.open(tpl_rules, "rb") as f:
        _, rules_tree = loads_nbt(f.read())
    rd = rules_tree[1]["data"][1]
    rd["minecraft:random_tick_speed"] = nbt_int(0)
    rd["minecraft:advance_time"] = nbt_byte(0)
    rd["minecraft:advance_weather"] = nbt_byte(0)
    rd["minecraft:spawn_mobs"] = nbt_byte(0)
    rd["minecraft:spawn_monsters"] = nbt_byte(0)
    rd["minecraft:spawn_wandering_traders"] = nbt_byte(0)
    rd["minecraft:spawn_phantoms"] = nbt_byte(0)
    rd["minecraft:spawn_patrols"] = nbt_byte(0)
    rd["minecraft:fire_spread_radius_around_player"] = nbt_int(0)
    rd["minecraft:mob_griefing"] = nbt_byte(0)
    rd["minecraft:command_block_output"] = nbt_byte(0)
    rd["minecraft:command_blocks_work"] = nbt_byte(1)
    with gzip.open(os.path.join(save_dir, "data", "minecraft", "game_rules.dat"), "wb") as f:
        f.write(dumps_nbt(rules_tree))

    # 4. data/minecraft/world_clocks.dat
    clocks = nbt_compound(
        {
            "DataVersion": nbt_int(DATA_VERSION),
            "data": nbt_compound(
                {
                    "minecraft:overworld": nbt_compound({"total_ticks": nbt_long(initial_time_ticks)}),
                    "minecraft:the_end": nbt_compound({"total_ticks": nbt_long(0)}),
                }
            ),
        }
    )
    with gzip.open(os.path.join(save_dir, "data", "minecraft", "world_clocks.dat"), "wb") as f:
        f.write(dumps_nbt(clocks))

    # 5. data/minecraft/weather.dat
    weather = nbt_compound(
        {
            "DataVersion": nbt_int(DATA_VERSION),
            "data": nbt_compound(
                {
                    "clear_weather_time": nbt_int(100000),
                    "rain_time": nbt_int(0),
                    "raining": nbt_byte(0),
                    "thunder_time": nbt_int(0),
                    "thundering": nbt_byte(0),
                }
            ),
        }
    )
    with gzip.open(os.path.join(save_dir, "data", "minecraft", "weather.dat"), "wb") as f:
        f.write(dumps_nbt(weather))


def encode_chunk_nbt(
    cx: int,
    cz: int,
    section_blocks: Dict[int, List[Tuple[str, Tuple[Tuple[str, str], ...]]]],
    section_sky: Dict[int, List[int]],
    section_blk: Dict[int, List[int]],
    biome_id: str,
    block_entities: List[Dict[str, Tuple[int, Any]]],
    heightmaps: Dict[str, List[int]],
) -> bytes:
    """Serialize a full MC 26.2 chunk into uncompressed NBT bytes."""
    sections_list = []
    for sy in range(MIN_SECTION_Y, MAX_SECTION_Y + 1):
        blocks = section_blocks.get(sy)
        if blocks is None:
            palette_nbt = [{"Name": nbt_string("minecraft:air")}]
            bs_compound: Dict[str, Tuple[int, Any]] = {
                "palette": nbt_list(TAG_COMPOUND, palette_nbt)
            }
        else:
            pal_map: Dict[Tuple[str, Tuple[Tuple[str, str], ...]], int] = {}
            palette_nbt = []
            indices = [0] * 4096
            for idx, bstate in enumerate(blocks):
                pi = pal_map.get(bstate)
                if pi is None:
                    pi = len(palette_nbt)
                    pal_map[bstate] = pi
                    name, props = bstate
                    entry: Dict[str, Tuple[int, Any]] = {"Name": nbt_string(name)}
                    if props:
                        entry["Properties"] = nbt_compound({k: nbt_string(v) for k, v in props})
                    palette_nbt.append(entry)
                indices[idx] = pi
            bs_compound = {"palette": nbt_list(TAG_COMPOUND, palette_nbt)}
            if len(palette_nbt) > 1:
                bits = max(4, (len(palette_nbt) - 1).bit_length())
                bs_compound["data"] = nbt_long_array(pack_padded_longs(indices, bits))

        sec_dict: Dict[str, Tuple[int, Any]] = {
            "Y": nbt_byte(sy),
            "block_states": nbt_compound(bs_compound),
            "biomes": nbt_compound({"palette": nbt_list(TAG_STRING, [biome_id])}),
        }
        sky_arr = section_sky.get(sy)
        if sky_arr is not None:
            sec_dict["SkyLight"] = nbt_byte_array(_nibble_array_from_levels(sky_arr))
        blk_arr = section_blk.get(sy)
        if blk_arr is not None:
            sec_dict["BlockLight"] = nbt_byte_array(_nibble_array_from_levels(blk_arr))
        sections_list.append(sec_dict)

    hm_nbt = {k: nbt_long_array(pack_padded_longs(v, 9)) for k, v in heightmaps.items()}
    post_proc = [(TAG_SHORT, []) for _ in range(MAX_SECTION_Y - MIN_SECTION_Y + 1)]

    chunk = nbt_compound(
        {
            "DataVersion": nbt_int(DATA_VERSION),
            "xPos": nbt_int(cx),
            "yPos": nbt_int(MIN_SECTION_Y),
            "zPos": nbt_int(cz),
            "Status": nbt_string("minecraft:full"),
            "LastUpdate": nbt_long(1),
            "InhabitedTime": nbt_long(0),
            "isLightOn": nbt_byte(1),
            "sections": nbt_list(TAG_COMPOUND, sections_list),
            "Heightmaps": nbt_compound(hm_nbt),
            "block_entities": nbt_list(TAG_COMPOUND, block_entities),
            "block_ticks": nbt_list(TAG_COMPOUND, []),
            "fluid_ticks": nbt_list(TAG_COMPOUND, []),
            "PostProcessing": nbt_list(TAG_LIST, post_proc),
            "structures": nbt_compound(
                {
                    "References": nbt_compound({}),
                    "starts": nbt_compound({}),
                }
            ),
        }
    )
    return dumps_nbt(chunk)


def encode_entity_chunk_nbt(cx: int, cz: int, entities: List[Dict[str, Tuple[int, Any]]]) -> bytes:
    chunk = nbt_compound(
        {
            "DataVersion": nbt_int(DATA_VERSION),
            "Position": nbt_int_array([cx, cz]),
            "Entities": nbt_list(TAG_COMPOUND, entities),
        }
    )
    return dumps_nbt(chunk)
