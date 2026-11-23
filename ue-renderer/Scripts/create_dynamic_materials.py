"""Generate the surface materials for the dynamic mesh stream."""

from pathlib import Path
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True
from create_terrain_materials import assets, custom_inputs, library, link, output, tools


DYNAMIC_SAMPLE = """
    // The terrain texture can change its filter at runtime. Dynamic draws use the
    // same animated texels with the original point-clamped atlas sampling.
    [branch] if (PointSampleBlockAtlas > 0.5)
        return Texture2DSampleLevel(Source, GlobalPointClampedSampler, UV, 0);
    return Texture2DSampleLevel(Source, SourceSampler, UV, 0);
"""


def build(name, translucent, first_person=False):
    path = f"/Game/Bridge/{name}"
    material = assets.load_asset(path) if assets.does_asset_exist(path) else tools.create_asset(
        name, "/Game/Bridge", unreal.Material, unreal.MaterialFactoryNew()
    )
    if not material:
        raise RuntimeError(f"Could not create {path}")
    library.delete_all_material_expressions(material)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT if translucent
                                 else unreal.BlendMode.BLEND_MASKED)
    material.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    if translucent:
        material.set_editor_property("translucency_lighting_mode",
                                     unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
    material.set_editor_property("two_sided", True)
    material.set_editor_property("opacity_mask_clip_value", 0.1)

    def node(kind, x, y, **properties):
        expression = library.create_material_expression(material, getattr(unreal, f"MaterialExpression{kind}"), x, y)
        for key, value in properties.items():
            expression.set_editor_property(key, value)
        return expression

    texture = node("TextureObjectParameter", -1060, -550, parameter_name="DynamicTexture",
                   texture=unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture"),
                   sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    uv = node("TextureCoordinate", -1060, -400)
    point_atlas = node("ScalarParameter", -1060, -270, parameter_name="PointSampleBlockAtlas")
    sample = node("Custom", -850, -500, description="Minecraft dynamic texture sampling", code=DYNAMIC_SAMPLE,
                  output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT4,
                  inputs=custom_inputs("Source", "UV", "PointSampleBlockAtlas"))
    link(texture, sample, "Source")
    link(uv, sample, "UV")
    link(point_atlas, sample, "PointSampleBlockAtlas")
    sample_rgb = node("ComponentMask", -630, -340, r=True, g=True, b=True, a=False)
    sample_alpha = node("ComponentMask", -630, -155, r=False, g=False, b=False, a=True)
    link(sample, sample_rgb)
    link(sample, sample_alpha)
    vertex = node("VertexColor", -720, 0)
    light = node("TextureCoordinate", -720, 220, coordinate_index=1)
    color = node("Custom", -400, -250, description="Minecraft vertex tint", code="return RGB * pow(saturate(Tint), 2.2);",
                 output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, inputs=custom_inputs("RGB", "Tint"))
    link(sample_rgb, color, "RGB")
    link(vertex, color, "Tint")
    alpha = node("Multiply", -160, 190)
    link(sample_alpha, alpha, "A")
    link(vertex, alpha, "B", "A")
    opacity = unreal.MaterialProperty.MP_OPACITY if translucent else unreal.MaterialProperty.MP_OPACITY_MASK
    output(alpha, opacity)

    light_code = """
        float3 Block = float3(1.0, 0.66, 0.37) * (BlockFill * pow(Light.x, 3.0));
        float y = dot(Pigment, float3(.2126, .7152, .0722));
        float mask = 0;
        if (Mode > .5 && Mode < 1.5) {
            float jewel = max(min(Pigment.g, Pigment.b) - .78 * Pigment.r, min(Pigment.r, Pigment.b) - Pigment.g);
            mask = smoothstep(.018, .16, jewel);
        } else if (Mode > 1.5 && Mode < 2.5) {
            mask = smoothstep(.018, .36, min(Pigment.g, Pigment.b) - .85 * Pigment.r);
        } else if (Mode > 2.5) mask = smoothstep(.19, .51, y);
        return RGB * (mask * 48.0 + (Mode > .5 ? 0 : Block) + SkyFill * pow(Light.y, 4.0) + AmbientFloor.rgb);
    """
    emission = node("Custom", -100, -150, description="Minecraft packed light", code=light_code,
                    output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3,
                    inputs=custom_inputs("RGB", "Pigment", "Mode", "Light", "BlockFill", "SkyFill", "AmbientFloor"))
    link(color, emission, "RGB")
    link(sample_rgb, emission, "Pigment")
    link(node("ScalarParameter", -600, 800, parameter_name="BlockEmissionMode"), emission, "Mode")
    link(light, emission, "Light")
    block_fill = node("ScalarParameter", -700, 390, parameter_name="FallbackBlockFill")
    sky_fill = node("VectorParameter", -700, 550, parameter_name="FallbackSkyFill",
                    default_value=unreal.LinearColor(0, 0, 0, 0))
    ambient_floor = node("VectorParameter", -700, 710, parameter_name="AmbientFloor",
                         default_value=unreal.LinearColor(0, 0, 0, 0))
    link(block_fill, emission, "BlockFill")
    link(sky_fill, emission, "SkyFill")
    link(ambient_floor, emission, "AmbientFloor")
    if first_person:
        # Preserve model readability in a dark cave while the lit surface responds to the world.
        minimum = node("Multiply", -420, 670)
        link(color, minimum, "A")
        link(node("Constant", -690, 670, r=0.045), minimum, "B")
        exposed = node("EyeAdaptationInverse", -180, 670)
        light_value_pin, alpha_pin = library.get_material_expression_input_names(exposed)
        link(minimum, exposed, light_value_pin)
        link(node("Constant", -420, 770, r=1.0), exposed, alpha_pin)
        combined = node("Add", 100, -150)
        link(emission, combined, "A")
        link(exposed, combined, "B")
        emission = combined
    output(emission, unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    output(color, unreal.MaterialProperty.MP_BASE_COLOR)
    output(node("Constant", -80, -380, r=0.84), unreal.MaterialProperty.MP_ROUGHNESS)
    output(node("Constant", -80, -485, r=0.24), unreal.MaterialProperty.MP_SPECULAR)
    library.recompile_material(material)
    if not assets.save_loaded_asset(material, False):
        raise RuntimeError(f"Could not save {name}")
    unreal.log(f"MCUE_DYNAMIC_MATERIAL_SAVED {name}")


build("M_McDynamicMasked", translucent=False)
build("M_McDynamicTranslucent", translucent=True)
build("M_McDynamicHandMasked", translucent=False, first_person=True)
