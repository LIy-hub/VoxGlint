"""Generate optional UE-lit terrain materials from the default Minecraft materials."""

import unreal


assets = unreal.EditorAssetLibrary
materials = unreal.MaterialEditingLibrary

for name in ("M_TerrainSolid", "M_TerrainCutout"):
    source = f"/Game/Bridge/{name}"
    target = f"{source}Lit"
    material = assets.load_asset(target) if assets.does_asset_exist(target) else assets.duplicate_asset(source, target)
    if not material:
        raise RuntimeError(f"Could not open {target}")
    tint = materials.get_material_property_input_node(material, unreal.MaterialProperty.MP_BASE_COLOR)
    if not tint:
        tint = materials.get_material_property_input_node(material, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
        if not tint or not materials.connect_material_property(tint, "", unreal.MaterialProperty.MP_BASE_COLOR):
            raise RuntimeError(f"Could not connect Base Color for {target}")
        black = materials.create_material_expression(material, unreal.MaterialExpressionConstant, -40, 110)
        if not materials.connect_material_property(black, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR):
            raise RuntimeError(f"Could not clear emissive for {target}")

    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    # A ray hit has no raster-space derivatives. Sample the atlas explicitly in hit shaders.
    pending = [tint]
    visited = set()
    while pending:
        expression = pending.pop()
        if not expression or expression in visited:
            continue
        visited.add(expression)
        pending.extend(materials.get_inputs_for_material_expression(material, expression))
        if isinstance(expression, unreal.MaterialExpressionCustom):
            code = expression.get_editor_property("code")
            if "#if RAYHITGROUPSHADER" not in code:
                expression.set_editor_property(
                    "code",
                    "#if RAYHITGROUPSHADER\nreturn Texture2DSampleLevel(Atlas, AtlasSampler, UV, 0);\n#else\n"
                    + code + "\n#endif",
                )
    materials.recompile_material(material)
    if not assets.save_loaded_asset(material, False):
        raise RuntimeError(f"Could not save {target}")
    unreal.log(f"MCUE_LIT_MATERIAL_SAVED {target}")
