import sys

import unreal


TERRAIN_SAMPLE = r"""
// Restrict the bilinear transition to one screen pixel when the texels are magnified.
// For minification, use the unmodified UV gradients to select the Minecraft mips.
if (Mode > 2.5)
    return Texture2DSampleLevel(Atlas, GlobalPointClampedSampler, UV, 0);

float2 size = Dimensions.xy;
float2 dx = ddx(UV);
float2 dy = ddy(UV);
float2 footprint = sqrt(dx * dx + dy * dy);
float2 centerSpace = UV * size - 0.5;
float2 edge = (frac(centerSpace) - 0.5) / max(footprint * size, 1.e-5);
float2 sharpenedUV = (floor(centerSpace) + 0.5 + saturate(edge + 0.5)) / size;
if (Mode > 1.5)
    return Texture2DSampleGrad(Atlas, AtlasSampler, sharpenedUV, dx, dy);

float4 filtered = Texture2DSampleGrad(Atlas, GlobalTrilinearClampedSampler, sharpenedUV, dx, dy);
if (Mode < 0.5)
    return filtered;

float atlasEdge = max(size.x, size.y);
float weight = smoothstep(1.0, 2.0, max(footprint.x, footprint.y) * atlasEdge);
if (weight <= 0.0)
    return filtered;

// Rotated grid with bilinear texels and interpolated mip levels.
float lod = log2(max(1.0, sqrt(length(dx) * length(dy)) * atlasEdge));
const float2 taps[4] = {
    float2(0.125, 0.375), float2(-0.125, -0.375),
    float2(0.375, -0.125), float2(-0.375, 0.125)
};
float4 sum = 0.0;
[unroll] for (int tap = 0; tap < 4; ++tap)
    sum += Texture2DSampleLevel(Atlas, GlobalTrilinearClampedSampler, UV + taps[tap] / size, lod);
return lerp(filtered, sum * 0.25, weight);
"""

PARTICLE_SAMPLE = "return Texture2DSampleLevel(Atlas, GlobalPointClampedSampler, UV, 0);"

library = unreal.MaterialEditingLibrary
assets = unreal.EditorAssetLibrary
tools = unreal.AssetToolsHelpers.get_asset_tools()


def link(source, target, input_name="", output_name=""):
    if not library.connect_material_expressions(source, output_name, target, input_name):
        raise RuntimeError(f"Could not connect {source.get_name()} to {target.get_name()}.{input_name}")


def output(expression, property_):
    if not library.connect_material_property(expression, "", property_):
        raise RuntimeError(f"Could not connect material property {property_}")


def custom_inputs(*names):
    inputs = []
    for name in names:
        argument = unreal.CustomInput()
        argument.set_editor_property("input_name", name)
        inputs.append(argument)
    return inputs


def build(name, blend, particle=False, shading=unreal.MaterialShadingModel.MSM_UNLIT):
    package = "/Game/Bridge"
    path = f"{package}/{name}"
    material = assets.load_asset(path) if assets.does_asset_exist(path) else tools.create_asset(
        name, package, unreal.Material, unreal.MaterialFactoryNew()
    )
    if not material:
        raise RuntimeError(f"Could not create {name}")
    library.delete_all_material_expressions(material)
    material.set_editor_property("shading_model", shading)
    material.set_editor_property("blend_mode", blend)
    material.set_editor_property("opacity_mask_clip_value", 0.5)
    translucent = blend == unreal.BlendMode.BLEND_TRANSLUCENT
    foliage = shading == unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE
    water = shading == unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER
    # Minecraft terrain culls backfaces. Fluids emit reverse-wound faces where needed.
    material.set_editor_property("two_sided", foliage)

    def node(expression, x, y):
        return library.create_material_expression(material, expression, x, y)

    def scalar(value, x, y):
        constant = node(unreal.MaterialExpressionConstant, x, y)
        constant.set_editor_property("r", value)
        return constant

    atlas = node(unreal.MaterialExpressionTextureObjectParameter, -850, -80)
    atlas.set_editor_property("parameter_name", "VoxelAtlas")
    atlas.set_editor_property("texture", unreal.load_asset("/Engine/EngineResources/WhiteSquareTexture"))
    atlas.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
    uv = node(unreal.MaterialExpressionTextureCoordinate, -850, 30)
    sample = node(unreal.MaterialExpressionCustom, -530, 30)
    sample.set_editor_property("description", "Block particle mip 0" if particle else "Minecraft terrain sampling")
    sample.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    sample.set_editor_property("inputs", custom_inputs("Atlas", "UV") if particle else
                               custom_inputs("Atlas", "UV", "Dimensions", "Mode"))
    code = PARTICLE_SAMPLE if particle else TERRAIN_SAMPLE
    if foliage or water:
        code = ("#if RAYHITGROUPSHADER\nreturn Texture2DSampleLevel(Atlas, AtlasSampler, UV, 0);\n#else\n"
                + code + "\n#endif")
    sample.set_editor_property("code", code)
    link(atlas, sample, "Atlas")
    link(uv, sample, "UV")
    if not particle:
        size = node(unreal.MaterialExpressionVectorParameter, -850, 150)
        size.set_editor_property("parameter_name", "AtlasDimensions")
        size.set_editor_property("default_value", unreal.LinearColor(16, 16, 0, 0))
        mode = node(unreal.MaterialExpressionScalarParameter, -850, 303)
        mode.set_editor_property("parameter_name", "TerrainFilter")
        link(size, sample, "Dimensions")
        link(mode, sample, "Mode")

    rgb = node(unreal.MaterialExpressionComponentMask, -260, 0)
    for prop in ("r", "g", "b"):
        rgb.set_editor_property(prop, True)
    link(sample, rgb)
    vertex = node(unreal.MaterialExpressionVertexColor, -275, 120)
    tint = node(unreal.MaterialExpressionMultiply, -40, 25)
    link(rgb, tint, "A")
    link(vertex, tint, "B")
    if foliage:
        output(tint, unreal.MaterialProperty.MP_BASE_COLOR)
        output(tint, unreal.MaterialProperty.MP_SUBSURFACE_COLOR)
        output(scalar(0.8, -40, 125), unreal.MaterialProperty.MP_ROUGHNESS)
    elif water:
        output(scalar(0.0, 130, -90), unreal.MaterialProperty.MP_BASE_COLOR)
        output(scalar(0.12, 130, 5), unreal.MaterialProperty.MP_ROUGHNESS)
        output(scalar(0.035, 130, 100), unreal.MaterialProperty.MP_OPACITY)
        volume = node(unreal.MaterialExpressionSingleLayerWaterMaterialOutput, 400, 280)
        scattering_name, absorption_name, phase_name, _ = library.get_material_expression_input_names(volume)
        scattering = node(unreal.MaterialExpressionMultiply, 140, 270)
        link(tint, scattering, "A")
        link(scalar(0.003, -45, 275), scattering, "B")
        absorption = node(unreal.MaterialExpressionConstant3Vector, 140, 390)
        absorption.set_editor_property("constant", unreal.LinearColor(0.004, 0.0016, 0.0007, 1.0))
        link(scattering, volume, scattering_name)
        link(absorption, volume, absorption_name)
        link(scalar(0.25, 140, 580), volume, phase_name)
    else:
        output(tint, unreal.MaterialProperty.MP_EMISSIVE_COLOR)

    if blend != unreal.BlendMode.BLEND_OPAQUE:
        alpha = node(unreal.MaterialExpressionComponentMask, -275, 235)
        for channel in ("r", "g", "b", "a"):
            alpha.set_editor_property(channel, channel == "a")
        link(sample, alpha)
        if not particle:
            coverage = node(unreal.MaterialExpressionMultiply, -60, 230)
            link(alpha, coverage, "A")
            link(vertex, coverage, "B", "A")
            alpha = coverage
        if translucent:
            cutoff = node(unreal.MaterialExpressionIf, 140, 235)
            cutoff.set_editor_property("const_b", 0.1)
            transparent = node(unreal.MaterialExpressionConstant, -65, 360)
            link(alpha, cutoff, "A")
            link(alpha, cutoff, "A > B")
            link(alpha, cutoff, "A == B")
            link(transparent, cutoff, "A < B")
            output(cutoff, unreal.MaterialProperty.MP_OPACITY)
        else:
            output(alpha, unreal.MaterialProperty.MP_OPACITY_MASK)

    library.recompile_material(material)
    if not assets.save_loaded_asset(material, False):
        raise RuntimeError(f"Could not save {name}")
    unreal.log(f"MCUE_TERRAIN_MATERIAL_SAVED {name}")


if __name__ == "__main__":
    if "--semantic-only" not in sys.argv:
        build("M_TerrainSolid", unreal.BlendMode.BLEND_OPAQUE)
        build("M_TerrainCutout", unreal.BlendMode.BLEND_MASKED)
        build("M_TerrainTranslucent", unreal.BlendMode.BLEND_TRANSLUCENT)
        build("M_BlockParticle", unreal.BlendMode.BLEND_MASKED, particle=True)

    build("M_TerrainFoliage", unreal.BlendMode.BLEND_MASKED, shading=unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
    build("M_TerrainWater", unreal.BlendMode.BLEND_OPAQUE, shading=unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
