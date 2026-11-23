"""Generate the MC 26.2 companion datapack (time/weather presets + clickable teleport tour menu)."""

from __future__ import annotations

import json
import os
from typing import Dict, Tuple


def _write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def write_prism_datapack(
    save_dir: str,
    waypoints: Dict[str, Tuple[float, float, float, float, float, str]],
) -> None:
    dp_root = os.path.join(save_dir, "datapacks", "prism-foundry")
    fn_dir = os.path.join(dp_root, "data", "prism", "function")
    tag_dir = os.path.join(dp_root, "data", "minecraft", "tags", "function")

    _write(
        os.path.join(dp_root, "pack.mcmeta"),
        json.dumps(
            {
                "pack": {
                    "description": "Prism Foundry RTX Showcase — lighting, weather & teleport controls",
                    "min_format": [107, 1],
                    "max_format": [107, 1],
                }
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
    )

    _write(
        os.path.join(tag_dir, "load.json"),
        json.dumps({"values": ["prism:load"]}, indent=2) + "\n",
    )

    # Load hook: wait until a player joins, then run one-time welcome setup
    _write(
        os.path.join(fn_dir, "load.mcfunction"),
        "\n".join(
            [
                "gamerule minecraft:random_tick_speed 0",
                "gamerule minecraft:spawn_mobs false",
                "gamerule minecraft:spawn_monsters false",
                "gamerule minecraft:fire_spread_radius_around_player 0",
                "gamerule minecraft:command_block_output false",
                "difficulty peaceful",
                "schedule function prism:welcome_check 20t",
            ]
        )
        + "\n",
    )

    _write(
        os.path.join(fn_dir, "welcome_check.mcfunction"),
        "\n".join(
            [
                "execute unless entity @a run schedule function prism:welcome_check 20t",
                "execute as @a[tag=!prism_welcomed] run function prism:welcome_player",
            ]
        )
        + "\n",
    )

    _write(
        os.path.join(fn_dir, "welcome_player.mcfunction"),
        "\n".join(
            [
                "tag @s add prism_welcomed",
                "gamemode creative @s",
                "tp @s 0.5 65.0 12.5 180.0 -8.0",
                "function prism:guide",
            ]
        )
        + "\n",
    )

    # Time & Weather presets
    presets = {
        "time_dawn": [
            "gamerule minecraft:advance_time false",
            "time set 23100",
            'tellraw @a {"text":"[Prism Foundry] 时间已切换: 黎明金光 (23100)","color":"gold"}',
        ],
        "time_noon": [
            "gamerule minecraft:advance_time false",
            "time set 6000",
            'tellraw @a {"text":"[Prism Foundry] 时间已切换: 正午强光 (6000) — 水下焦散最佳","color":"yellow"}',
        ],
        "time_sunset": [
            "gamerule minecraft:advance_time false",
            "time set 12650",
            'tellraw @a {"text":"[Prism Foundry] 时间已切换: 燃橙黄昏 (12650) — 大气与湖面反射最佳","color":"gold"}',
        ],
        "time_midnight": [
            "gamerule minecraft:advance_time false",
            "time set 18000",
            'tellraw @a {"text":"[Prism Foundry] 时间已切换: 午夜星空 (18000) — 全图体素光最佳","color":"aqua"}',
        ],
        "time_flow_on": [
            "gamerule minecraft:advance_time true",
            'tellraw @a {"text":"[Prism Foundry] 日夜流动: 已开启","color":"green"}',
        ],
        "time_flow_off": [
            "gamerule minecraft:advance_time false",
            'tellraw @a {"text":"[Prism Foundry] 日夜流动: 已定格","color":"gray"}',
        ],
        "weather_clear": [
            "gamerule minecraft:advance_weather false",
            "weather clear 100000",
            'tellraw @a {"text":"[Prism Foundry] 天气已切换: 晴朗无雨","color":"aqua"}',
        ],
        "weather_rain": [
            "gamerule minecraft:advance_weather false",
            "weather rain 100000",
            'tellraw @a {"text":"[Prism Foundry] 天气已切换: 降雨 (观察区0屋檐干湿线与水洼反射)","color":"blue"}',
        ],
        "weather_thunder": [
            "gamerule minecraft:advance_weather false",
            "weather thunder 100000",
            'tellraw @a {"text":"[Prism Foundry] 天气已切换: 雷暴 (观察分叉闪电与水面瞬态高光)","color":"light_purple"}',
        ],
    }
    for name, lines in presets.items():
        _write(os.path.join(fn_dir, f"{name}.mcfunction"), "\n".join(lines) + "\n")

    # Individual waypoint teleport functions
    for key, (x, y, z, yaw, pitch, label) in waypoints.items():
        # Give night_vision=clear, water_breathing if underwater waypoint
        extra = []
        if "underwater" in key or "snell" in key or "vault" in key:
            extra.append("effect give @p minecraft:water_breathing 300 0 true")
        cmd_lines = [
            f"tp @p {x:.1f} {y:.1f} {z:.1f} {yaw:.1f} {pitch:.1f}",
            *extra,
            f'tellraw @p {{"text":"[传送] {label}","color":"green"}}',
        ]
        _write(os.path.join(fn_dir, f"tp_{key}.mcfunction"), "\n".join(cmd_lines) + "\n")

    # Interactive clickable tour guide using MC 26.2 snake_case click_event
    def btn(text: str, command: str, color: str = "aqua") -> dict:
        return {
            "text": f"[{text}]",
            "color": color,
            "bold": True,
            "click_event": {"action": "run_command", "command": command},
        }

    guide_lines = [
        'tellraw @p {"text":"========== 光棱圣殿 Prism Foundry RTX 演示地图 ==========","color":"gold","bold":true}',
        "tellraw @p "
        + json.dumps(
            [
                {"text": "时间: ", "color": "gray"},
                btn("黎明", "/function prism:time_dawn", "gold"),
                {"text": " "},
                btn("正午", "/function prism:time_noon", "yellow"),
                {"text": " "},
                btn("黄昏", "/function prism:time_sunset", "gold"),
                {"text": " "},
                btn("午夜", "/function prism:time_midnight", "aqua"),
                {"text": "  天气: ", "color": "gray"},
                btn("晴", "/function prism:weather_clear", "white"),
                {"text": " "},
                btn("雨", "/function prism:weather_rain", "blue"),
                {"text": " "},
                btn("雷暴", "/function prism:weather_thunder", "light_purple"),
            ],
            ensure_ascii=False,
        ),
    ]
    # Emit clickable waypoint buttons in pairs
    wp_items = list(waypoints.items())
    for i in range(0, len(wp_items), 2):
        row = [{"text": "传送: ", "color": "gray"}]
        for j in range(i, min(i + 2, len(wp_items))):
            key, (_, _, _, _, _, label) = wp_items[j]
            row.append(btn(label, f"/function prism:tp_{key}", "green" if j % 2 == 0 else "aqua"))
            row.append({"text": "  "})
        guide_lines.append("tellraw @p " + json.dumps(row, ensure_ascii=False))
    guide_lines.append(
        'tellraw @p {"text":"提示: 输入 /function prism:guide 可随时重开目录 | Ctrl+Home 可拍 256 SPP 路径追踪图","color":"gray"}'
    )
    _write(os.path.join(fn_dir, "guide.mcfunction"), "\n".join(guide_lines) + "\n")
