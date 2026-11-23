"""Regenerate local renderer assets before running or cooking the project."""

import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True

from create_mc_materials import build_skydome, build_water_textures
from create_stock_cloud_layers import build_cloud
from create_terrain_materials import assets


def main():
    lunar_input = os.environ.get("MCUE_LUNAR_INPUT")
    if lunar_input:
        from create_lunar_moon import import_sky_textures

        import_sky_textures(Path(lunar_input))

    if not all(assets.does_asset_exist(f"/Game/Bridge/{name}") for name in ("T_WaterFoamLace", "T_WaterFoamMicro")):
        build_water_textures()

    if not all(assets.does_asset_exist(f"/Game/Bridge/{name}") for name in ("M_EpicCloudLayers", "MI_EpicCloudLayers")):
        build_cloud()

    if lunar_input or not assets.does_asset_exist("/Game/Bridge/M_McSkyDome"):
        build_skydome()


if __name__ == "__main__":
    main()
