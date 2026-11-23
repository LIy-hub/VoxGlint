"""Entry point: Generate the complete 'PrismFoundry' Minecraft 26.2 RTX showcase save + full QA verification."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any, Dict, List

from build_datapack import write_prism_datapack
from build_part_dark import (
    build_zone2_voxel_light_hall,
    build_zone3_lumen_gi_lab,
    build_zone6_stress_dungeon,
)
from build_part_surface import (
    build_island_foundation,
    build_sky_overview_ring,
    build_zone0_plaza,
    build_zone1_pbr_gallery,
    build_zone4_water_optics,
    build_zone5_cathedral,
)
from refine_details import apply_refinements
from voxel_canvas import MAIN_FLOOR_Y, VoxelCanvas

MEGALIGHTS_80B_BUDGET = 850

DARK_ZONE_PROBES = [
    ("2A_center", 5, 66, -48),
    ("2A_west", -22, 66, -48),
    ("2A_east", 22, 66, -48),
    ("2B_flush_ies", 23, 66, -80),
    ("2C_colors", -23, 66, -72),
    ("2D_lantern_hall", 0, 66, -72),
    ("2D_tower_mid", 0, 72, -75),
    ("3B_gold_mirror", -52, 66, -82),
    ("3B_iron_mirror", -58, 66, -82),
    ("3B_obsidian_mirror", -64, 66, -82),
    ("6A_forge_floor", 53, 40, 70),
    ("6A_forge_mezzanine", 53, 48, 48),
    ("6B_bridge", 85, 44, 70),
    ("6B_fire_side", 85, 42, 55),
    ("6B_ice_side", 85, 42, 80),
]


def audit_bubble_elevators(c: VoxelCanvas) -> Dict[str, Any]:
    """Verify twin bubble-column elevators have intact columns, sealed perimeters, and zero plaza water spills."""
    issues: List[str] = []
    water_types = {"water", "bubble_column"}
    seal_ok_blocks = {"glass", "polished_deepslate", "waxed_cut_copper", "spruce_fence_gate", "oak_fence_gate", "dark_oak_fence_gate"}

    for label, sx, base_blk, top_y in (("west_up", -4, "soul_sand", 97), ("east_down", 4, "magma_block", 96)):
        sz = -7
        if c.get_short(sx, MAIN_FLOOR_Y, sz) != base_blk:
            issues.append(f"{label}: expected {base_blk} at ({sx},{MAIN_FLOOR_Y},{sz}), got {c.get_short(sx, MAIN_FLOOR_Y, sz)}")
        for y in range(MAIN_FLOOR_Y + 1, top_y + 1):
            if c.get_short(sx, y, sz) != "bubble_column":
                issues.append(f"{label}: broken bubble_column at ({sx},{y},{sz}): {c.get_short(sx, y, sz)}")
            for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nb = c.get_short(sx + dx, y, sz + dz)
                if nb not in seal_ok_blocks:
                    issues.append(f"{label}: unsealed side ({sx+dx},{y},{sz+dz})='{nb}'")
    # Check surrounding plaza deck (Y=65) around elevators has zero stray water
    spills = 0
    for x in range(-9, 10):
        for z in range(-13, -1):
            if (x, z) in ((-4, -7), (4, -7)):
                continue
            if c.get_short(x, MAIN_FLOOR_Y + 1, z) in water_types:
                spills += 1
    if spills:
        issues.append(f"plaza water spill detected: {spills} water blocks at Y={MAIN_FLOOR_Y + 1}")
    return {"ok": len(issues) == 0, "issues": issues[:20]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template-save", required=True, type=Path, help="A fresh Minecraft 26.2 vanilla save")
    parser.add_argument("--minecraft-jar", required=True, type=Path, help="The Minecraft 26.2 client JAR")
    parser.add_argument("--output", required=True, type=Path, help="New destination directory for the showcase")
    args = parser.parse_args()
    template = args.template_save.resolve()
    minecraft_jar = args.minecraft_jar.resolve()
    output = args.output.resolve()
    for required in (template / "level.dat", template / "data/minecraft/game_rules.dat", minecraft_jar):
        if not required.is_file():
            parser.error(f"File not found: {required}")
    if output.exists() or output.is_relative_to(template):
        parser.error("Choose a new output directory outside the template save")

    t0 = time.perf_counter()
    print("[1/7] Initializing 256x128x256 voxel canvas...")
    c = VoxelCanvas()

    print("[2/7] Building Prism Foundry main architecture (Zones 0-6 + Sky Ring)...")
    build_island_foundation(c)
    build_zone0_plaza(c)
    build_zone1_pbr_gallery(c)
    build_zone2_voxel_light_hall(c)
    build_zone3_lumen_gi_lab(c)
    build_zone4_water_optics(c)
    build_zone5_cathedral(c)
    build_zone6_stress_dungeon(c)
    build_sky_overview_ring(c)

    print("[3/7] Applying micro-refinement & architectural polish pass...")
    apply_refinements(c)

    print("[4/7] Auto-connecting fences/bars/walls & validating 100% blocks against MC 26.2 JAR...")
    connected = c.auto_connect_blocks()
    missing_blocks, invalid_props = c.validate_against_jar(minecraft_jar)
    if missing_blocks:
        print("  ERROR missing blocks:", missing_blocks)
    if invalid_props:
        print("  ERROR invalid properties:", invalid_props)
    if not missing_blocks and not invalid_props:
        print(f"  OK: 100% block IDs & properties verified against MC 26.2 JAR (auto-connected: {connected}).")

    # Claim the output directory at the point of writing. A concurrent run cannot overwrite it.
    output.mkdir(parents=True)
    preview_dir = output / "qa_previews"
    print("[5/7] Auditing MegaLights 80-block budgets, waypoints, attachments & generating previews...")
    light_audit = c.audit_lights()
    wp_audit = c.audit_waypoints()
    attach_audit = c.audit_attachments()
    elev_audit = audit_bubble_elevators(c)
    previews = c.render_previews(preview_dir)

    print(f"[6/7] Baking 3D lighting & exporting MC 26.2 Anvil save -> {output} ...")
    stats = c.export_save(
        save_dir=output,
        template_save_dir=template,
        level_name="Prism Foundry RTX Showcase",
        spawn_xyz=(0, MAIN_FLOOR_Y + 1, 12),
        spawn_yaw=180.0,
        spawn_pitch=-8.0,
        initial_time_ticks=2400,
        pre_connected=connected,
    )
    write_prism_datapack(output, c.waypoints)

    # Copy a crisp 64x64 save icon from top_surface.png so the world looks great in the MC world list!
    try:
        from PIL import Image

        with Image.open(preview_dir / "top_surface.png") as top_img:
            top_img.resize((64, 64), Image.LANCZOS).save(output / "icon.png")
    except Exception as exc:
        print("  Warning: could not write icon.png:", exc)

    print("[7/7] Running post-bake dark-zone zero-skylight verification & final QA gate...")
    dark_audit = c.audit_dark_zones(DARK_ZONE_PROBES)

    max_w80 = max(info["lights_within_80"] for info in light_audit["waypoints"].values())
    over_budget = [
        f"{k}={info['lights_within_80']}"
        for k, info in light_audit["waypoints"].items()
        if info["lights_within_80"] > MEGALIGHTS_80B_BUDGET
    ]

    qa_gates = {
        "jar_blocks_and_props_ok": (len(missing_blocks) == 0 and len(invalid_props) == 0),
        "megalights_80b_budget_ok": (len(over_budget) == 0),
        "waypoints_safety_and_sightline_ok": wp_audit["ok"],
        "dark_zones_zero_skylight_ok": dark_audit["ok"],
        "attachments_zero_floating_ok": attach_audit["ok"],
        "bubble_elevators_sealed_ok": elev_audit["ok"],
    }
    all_pass = all(qa_gates.values())

    report = {
        "elapsed_seconds": round(time.perf_counter() - t0, 2),
        "all_qa_gates_passed": all_pass,
        "qa_gates": qa_gates,
        "missing_blocks": missing_blocks,
        "invalid_props": invalid_props,
        "export_stats": stats,
        "light_audit": {
            "budget_limit_80b": MEGALIGHTS_80B_BUDGET,
            "max_lights_within_80": max_w80,
            "over_budget": over_budget,
            **light_audit,
        },
        "waypoint_audit": wp_audit,
        "dark_zone_audit": dark_audit,
        "attachment_audit": attach_audit,
        "bubble_elevator_audit": elev_audit,
        "previews": previews,
    }
    report_path = preview_dir / "qa_report.json"
    with report_path.open("w", encoding="utf-8") as f:
        json.dump(report, f, ensure_ascii=False, indent=2)

    print(f"\nDone in {report['elapsed_seconds']}s!")
    print(f"  Chunks written : {stats['chunks_written']}")
    print(f"  Unique states  : {stats['unique_states']} (auto-connected: {stats['connected_blocks']})")
    print(f"  Block entities : {stats['block_entities']} | Entities: {stats['entities']}")
    print(f"  Total emitters : {light_audit['total_emitters']} | Max 80b: {max_w80} (limit <= {MEGALIGHTS_80B_BUDGET})")
    print("  Per-waypoint 80-block MegaLights budget check:")
    for key, info in light_audit["waypoints"].items():
        w80 = info["lights_within_80"]
        flag = "OK" if w80 <= 700 else ("WARN" if w80 <= MEGALIGHTS_80B_BUDGET else "OVER!")
        print(f"    - {key:18s} {w80:4d} lights/80b ({info['lights_within_32']:3d}/32b) [{flag}] | {info['label']}")
    print("\n  Automated Strict QA Gates:")
    for gk, gv in qa_gates.items():
        print(f"    - {gk:36s}: {'PASS' if gv else 'FAIL'}")
    if not wp_audit["ok"]:
        print("  Waypoint issues:", wp_audit["issues"])
    if not dark_audit["ok"]:
        print("  Dark zone skylight leaks:", dark_audit["leaks"])
    if not attach_audit["ok"]:
        print("  Floating attachment issues:", attach_audit["issues"])
    if not elev_audit["ok"]:
        print("  Bubble elevator issues:", elev_audit["issues"])
    print(f"  QA report      : {report_path}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())
