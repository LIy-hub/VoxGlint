"""Generate the materials shared by Minecraft's submitted particle geometry."""

from pathlib import Path
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True
from create_terrain_materials import assets, custom_inputs, library, link, output, tools


def build(mode, blend, overlay):
    name = f"M_McParticle{mode}{'Overlay' if overlay else ''}"
    path = f"/Game/Bridge/{name}"
    material = assets.load_asset(path) if assets.does_asset_exist(path) else tools.create_asset(
        name, "/Game/Bridge", unreal.Material, unreal.MaterialFactoryNew()
    )
    if not material:
        raise RuntimeError(f"Could not create {path}")
    library.delete_all_material_expressions(material)
    masked = mode == "Masked" and not overlay
    material.set_editor_property("blend_mode", blend)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("disable_depth_test", overlay)
    material.set_editor_property("opacity_mask_clip_value", 0.1)

    def node(kind, x, y, **properties):
        expression = library.create_material_expression(material, getattr(unreal, f"MaterialExpression{kind}"), x, y)
        for key, value in properties.items():
            expression.set_editor_property(key, value)
        return expression

    def custom(x, y, description, code, output_type, **inputs):
        expression = node("Custom", x, y, description=description, code=code, output_type=output_type,
                          inputs=custom_inputs(*inputs))
        for key, value in inputs.items():
            link(value, expression, key)
        return expression

    texture = node("TextureObjectParameter", -1100, -360, parameter_name="DynamicTexture",
                   texture=unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture"),
                   sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    point_atlas = node("ScalarParameter", -1100, -80, parameter_name="PointSampleBlockAtlas")
    sample = custom(-800, -360, "Particle texture at mip 0", """
        [branch] if (PointAtlas > 0.5)
            return Texture2DSampleLevel(Source, GlobalPointClampedSampler, UV, 0);
        return Texture2DSampleLevel(Source, SourceSampler, UV, 0);
    """, unreal.CustomMaterialOutputType.CMOT_FLOAT4, Source=texture,
        UV=node("TextureCoordinate", -1100, -200), PointAtlas=point_atlas)

    vertex = node("VertexColor", -800, -50)
    alpha = node("ComponentMask", -570, -255, r=False, g=False, b=False, a=True)
    link(sample, alpha)
    opacity = node("Multiply", -365, -250)
    link(alpha, opacity, "A")
    link(vertex, opacity, "B", "A")
    if masked:
        output(opacity, unreal.MaterialProperty.MP_OPACITY_MASK)
    else:
        params = {"Alpha": opacity}
        formula = "return Alpha;"
        if mode == "Masked":
            formula = "return 1.0;"
        elif mode == "Additive":
            params["UsesAlpha"] = node("ScalarParameter", -365, -410, parameter_name="AdditiveUsesAlpha",
                                       default_value=1.0)
            formula = "return lerp(1.0, Alpha, UsesAlpha);"
        opacity = custom(-145, -250, "Particle coverage", f"clip(Alpha - 0.1); {formula}",
                         unreal.CustomMaterialOutputType.CMOT_FLOAT1, **params)
        output(opacity, unreal.MaterialProperty.MP_OPACITY)

    rgb = node("ComponentMask", -570, -440, r=True, g=True, b=True, a=False)
    link(sample, rgb)
    emission = custom(-125, 0, "Particle vertex tint and packed light", """
        float3 Block = float3(1.0, 0.66, 0.37) * BlockFill * pow(Light.x, 3.0);
        float3 Lighting = saturate(Block + SkyFill * pow(Light.y, 4.0) + AmbientFloor);
        Lighting = lerp(Lighting, float3(1.0, 1.0, 1.0), step(0.999, min(Light.x, Light.y)));
        return RGB * pow(saturate(Tint), 2.2) * Lighting;
    """, unreal.CustomMaterialOutputType.CMOT_FLOAT3, RGB=rgb, Tint=vertex,
        Light=node("TextureCoordinate", -800, 160, coordinate_index=1),
        BlockFill=node("ScalarParameter", -800, 310, parameter_name="FallbackBlockFill"),
        SkyFill=node("VectorParameter", -800, 445, parameter_name="FallbackSkyFill"),
        AmbientFloor=node("VectorParameter", -800, 600, parameter_name="AmbientFloor"))
    # Packed light already controls sprite brightness. Compensate for UE's HDR exposure so
    # full-bright sprites remain visible against terrain lit at daylight intensity.
    exposed = node("EyeAdaptationInverse", 105, 0)
    light_value_pin, alpha_pin = library.get_material_expression_input_names(exposed)
    link(emission, exposed, light_value_pin)
    link(node("Constant", -125, 320, r=1.0), exposed, alpha_pin)
    output(exposed, unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    library.recompile_material(material)
    if not assets.save_loaded_asset(material, False):
        raise RuntimeError(f"Could not save {path}")
    unreal.log(f"MCUE_PARTICLE_MATERIAL_SAVED {name}")


for overlay in (False, True):
    for mode, blend in (
        ("Masked", unreal.BlendMode.BLEND_TRANSLUCENT if overlay else unreal.BlendMode.BLEND_MASKED),
        ("Alpha", unreal.BlendMode.BLEND_TRANSLUCENT),
        ("Additive", unreal.BlendMode.BLEND_ADDITIVE),
        ("Premultiplied", unreal.BlendMode.BLEND_ALPHA_COMPOSITE),
    ):
        build(mode, blend, overlay)
