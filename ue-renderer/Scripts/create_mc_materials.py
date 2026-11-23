"""Build the cinematic sky, water, terrain wetness, precipitation and lightning materials."""

import math
import os
from pathlib import Path
import random
import struct
import sys
import tempfile

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True
from create_terrain_materials import TERRAIN_SAMPLE, assets, custom_inputs, library, link, tools
from create_stock_cloud_layers import build_cloud
from lunar_moon_shading import LUNAR_LIGHTING, LUNAR_SKY_RENDER

WATER_INCLUDE = "/MinecraftUEViewer/Private/McWaterP1.ush"
WATER_INTERFACE_INCLUDE = "/MinecraftUEViewer/Private/McWaterInterface.ush"
WATER_P3_INCLUDE = "/MinecraftUEViewer/Private/McWaterP3.ush"
WATER_FALLBACK = "/Game/Bridge/T_WaterArrayFallback"
WET_TERRAIN_INCLUDE = "/MinecraftUEViewer/Private/McWetTerrain.ush"

SAMPLE = (
    "#if RAYHITGROUPSHADER\nreturn Texture2DSampleLevel(Atlas, AtlasSampler, UV, 0);\n#else\n"
    + TERRAIN_SAMPLE
    + "\n#endif"
)

# Shared HLSL helper snippet for sampling the player-centered 256x256 precipitation height/biome grid.
# PrecipOrigin = (OriginMcX, OriginMcZ, GridSizeBlocks=256, MinY)
# UE coords: mcX = -WorldPos.y / 100.0, mcZ = WorldPos.x / 100.0, mcY = WorldPos.z / 100.0
PRECIP_EXPOSURE_FN = r"""
float3 sampleExposure(Texture2D gridTex, float4 gridOrigin, float gridValid, float3 worldPos) {
    // Returns float3(exposedRain, exposedSnow, isExposed)
    if (gridValid < 0.5) {
        return float3(1.0, 0.0, 1.0);
    }
    float mcX = -worldPos.y * 0.01;
    float mcZ =  worldPos.x * 0.01;
    float2 cell = float2(mcX - gridOrigin.x, mcZ - gridOrigin.y);
    float2 uv = (floor(cell) + 0.5) / gridOrigin.z;
    if (uv.x <= 0.002 || uv.x >= 0.998 || uv.y <= 0.002 || uv.y >= 0.998) {
        return float3(1.0, 0.0, 1.0);
    }
    float4 s = Texture2DSampleLevel(gridTex, GlobalPointClampedSampler, uv, 0);
    if (s.b < 0.5) {
        return float3(1.0, 0.0, 1.0);
    }
    // Slight 4-neighbor softness (0.18 block) only at eaves boundary, never leaking under full roofs.
    float topZ = s.r;
    float exposed = smoothstep(topZ - 18.0, topZ - 2.0, worldPos.z);
    float isRain = smoothstep(0.25, 0.45, s.g) * (1.0 - smoothstep(0.60, 0.80, s.g));
    float isSnow = smoothstep(0.75, 0.95, s.g);
    return float3(exposed * isRain, exposed * isSnow, exposed);
}
"""

# ---------------------------------------------------------------------------
# Terrain Wetness, Puddles, Roof Drying & Snow Frost (all driven by PrecipGrid + NormalAtlas.A porosity)
# ---------------------------------------------------------------------------
TERRAIN_WETNESS = r"""
// Inputs: Albedo(3), NormalTS(3), Roughness(1), Metallic(1), Porosity(1),
//         WorldPos(3), VertexNormalWS(3), Clock(1), Wetness(1), PuddleLevel(1),
//         LocalRain(1), LocalSnow(1), PrecipGrid(tex), GridOrigin(4), GridValid(1)
// Output: float4(AlbedoR, AlbedoG, AlbedoB, NewRoughness) -- normals updated in companion node.
float wetGlobal = saturate(Wetness);
float snowGlobal = saturate(LocalSnow);
float absorbFactor = saturate(Porosity) * (1.0 - Metallic);
float3 shoreAlbedo = Albedo * lerp(1.0, lerp(0.97, 0.60, absorbFactor) * (1.0 - Metallic) + Metallic, Shore.x);
float shoreRough = lerp(Roughness, min(Roughness, lerp(0.15, 0.085, absorbFactor)), Shore.y * (1.0 - Metallic));
if (wetGlobal < 0.002 && snowGlobal < 0.002) {
    return float4(shoreAlbedo, shoreRough);
}

float exposedRain = 1.0;
float exposedSnow = 0.0;
if (GridValid > 0.5) {
    float mcX = -WorldPos.y * 0.01;
    float mcZ =  WorldPos.x * 0.01;
    float2 uv = (floor(float2(mcX - GridOrigin.x, mcZ - GridOrigin.y)) + 0.5) / GridOrigin.z;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
        float4 g = Texture2DSampleLevel(PrecipGrid, GlobalPointClampedSampler, uv, 0);
        if (g.b > 0.5) {
            float expMask = smoothstep(g.r - 18.0, g.r - 2.0, WorldPos.z);
            float isRain = smoothstep(0.25, 0.45, g.g) * (1.0 - smoothstep(0.60, 0.80, g.g));
            float isSnow = smoothstep(0.75, 0.95, g.g);
            exposedRain = expMask * isRain;
            exposedSnow = expMask * isSnow;
        }
    }
}

float upFacing = saturate(VertexNormalWS.z);
float sideWet = lerp(0.32, 1.0, smoothstep(0.15, 0.75, upFacing));
float localWet = wetGlobal * exposedRain * sideWet;

// Porous surfaces darken when wet; conductors/glass keep albedo and only form a thin water film.
float3 wetAlbedo = shoreAlbedo * lerp(1.0, lerp(0.94, 0.48, absorbFactor), localWet);

// Low-frequency world-space puddle mask on exposed upward faces.
float2 wp = WorldPos.xy * 0.0026;
float n1 = sin(wp.x * 2.1 + sin(wp.y * 1.7)) * 0.5 + 0.5;
float n2 = cos(wp.y * 3.3 - cos(wp.x * 2.9 + 1.1)) * 0.5 + 0.5;
float puddleNoise = n1 * 0.6 + n2 * 0.4;
float puddle = smoothstep(0.56, 0.78, puddleNoise) * saturate(PuddleLevel) * exposedRain * smoothstep(0.85, 0.96, upFacing);

float wetRough = lerp(shoreRough, min(shoreRough, lerp(0.22, 0.11, absorbFactor)), localWet);
float finalRough = lerp(wetRough, 0.018, puddle);

// Subtle frost/snow dusting on exposed snow-biome top faces.
float frost = snowGlobal * exposedSnow * smoothstep(0.70, 0.95, upFacing) * 0.58;
wetAlbedo = lerp(wetAlbedo, float3(0.84, 0.89, 0.96), frost);
finalRough = lerp(finalRough, 0.72, frost);

return float4(wetAlbedo, finalRough);
"""

TERRAIN_WET_NORMAL = r"""
// Flattens tangent normal inside puddles and adds subtle raindrop ripples on puddles.
float puddleGlobal = saturate(PuddleLevel);
if (puddleGlobal < 0.005 || VertexNormalWS.z < 0.85) {
    return NormalTS;
}
float exposedRain = 1.0;
if (GridValid > 0.5) {
    float mcX = -WorldPos.y * 0.01;
    float mcZ =  WorldPos.x * 0.01;
    float2 uv = (floor(float2(mcX - GridOrigin.x, mcZ - GridOrigin.y)) + 0.5) / GridOrigin.z;
    if (uv.x > 0.002 && uv.x < 0.998 && uv.y > 0.002 && uv.y < 0.998) {
        float4 g = Texture2DSampleLevel(PrecipGrid, GlobalPointClampedSampler, uv, 0);
        if (g.b > 0.5) {
            float expMask = smoothstep(g.r - 18.0, g.r - 2.0, WorldPos.z);
            float isRain = smoothstep(0.25, 0.45, g.g) * (1.0 - smoothstep(0.60, 0.80, g.g));
            exposedRain = expMask * isRain;
        }
    }
}
float2 wp = WorldPos.xy * 0.0026;
float n1 = sin(wp.x * 2.1 + sin(wp.y * 1.7)) * 0.5 + 0.5;
float n2 = cos(wp.y * 3.3 - cos(wp.x * 2.9 + 1.1)) * 0.5 + 0.5;
float puddle = smoothstep(0.56, 0.78, n1 * 0.6 + n2 * 0.4) * puddleGlobal * exposedRain * smoothstep(0.85, 0.96, VertexNormalWS.z);
float3 flatN = normalize(lerp(NormalTS, float3(0.0, 0.0, 1.0), puddle * 0.94));
if (LocalRain > 0.02 && puddle > 0.15) {
    float2 rp = WorldPos.xy * 0.014;
    float2 cid = floor(rp);
    float2 cf = frac(rp);
    float h1 = frac(sin(dot(cid, float2(127.1, 311.7))) * 43758.5453);
    float h2 = frac(sin(dot(cid, float2(269.5, 183.3))) * 43758.5453);
    float cyc = frac(Clock * (1.3 + 0.4 * h2) + h1);
    float2 diff = cf - float2(0.25 + 0.5 * h1, 0.25 + 0.5 * h2);
    float r = length(diff);
    float rad = cyc * 0.45;
    float ring = exp(-pow((r - rad) * 20.0, 2.0)) * sin((r - rad) * 44.0) * (1.0 - cyc) * smoothstep(0.48, 0.32, r);
    flatN = normalize(flatN + float3((diff / max(r, 0.001)) * ring * 0.16 * puddle * saturate(LocalRain), 0.0));
}
return flatN;
"""

# ---------------------------------------------------------------------------
SKYDOME_NIGHT_AND_FALLBACK = LUNAR_LIGHTING + r"""
float3 viewDir = normalize(WorldPos - CamPos);
float overcast = saturate(Overcast);
// Scale the enlarged visual disk by its apparent area. Directional light and shadow softness are independent.
float sunDiskScale = min(1.0, 0.58 / SunDiameter);
float3 sky = AtmoLuminance + SunDisk * (sunDiskScale * sunDiskScale) * (1.0 - 0.82 * overcast);

#if PIXELSHADER
float3 lunarViewDx = ddx(viewDir);
float3 lunarViewDy = ddy(viewDir);
float lunarPixelAngle = max(length(lunarViewDx), length(lunarViewDy));
#else
float lunarPixelAngle = 0.0003;
#endif

float up = viewDir.z;
float sunAlt = SunDir.z;

// Night visibility ramps in smoothly across astronomical twilight
float nightFade = smoothstep(0.08, -0.14, sunAlt);
float horizonMask = smoothstep(-0.02, 0.14, up);
float clearSky = (1.0 - overcast * 0.92) * horizonMask;

if (nightFade > 0.002 && up > -0.02) {
    float nightGain = max(0.1, NightSkyScale);
    // Subtle dark-navy zenith airglow so clear nights and SkyLight captures keep subtle shadow legibility
    float3 nightAirglow = lerp(float3(0.0052, 0.0074, 0.0138), float3(0.0022, 0.0036, 0.0078), saturate(up * 1.6)) * nightGain;
    sky += nightAirglow * nightFade * (1.0 - overcast * 0.55);

    // 1) Crisp multi-magnitude procedural starfield (two angular resolutions)
    float3 starColor = float3(0.0, 0.0, 0.0);
    [unroll] for (int layer = 0; layer < 2; ++layer) {
        float gridScale = (layer == 0) ? 145.0 : 285.0;
        float3 p = viewDir * gridScale;
        float3 cell = floor(p);
        float3 f = frac(p) - 0.5;
        float h1 = frac(sin(dot(cell, float3(127.1, 311.7, 74.7))) * 43758.5453);
        float h2 = frac(sin(dot(cell, float3(269.5, 183.3, 246.1))) * 43758.5453);
        float h3 = frac(sin(dot(cell, float3(113.5, 271.9, 124.6))) * 43758.5453);
        float3 jitter = (float3(h1, h2, h3) - 0.5) * 0.62;
        float d = length(f - jitter);
        float prob = (layer == 0) ? 0.955 : 0.975;
        if (h1 > prob) {
            float mag = pow((h1 - prob) / (1.0 - prob), 2.2);
            float radius = lerp(0.075, 0.17, mag);
            float core = exp(-pow(d / radius, 2.0));
            float twinkle = 0.80 + 0.20 * sin(Clock * (2.2 + 3.5 * h2) + h3 * 62.8);
            // Blackbody tint: blue-white, pure white, warm amber
            float3 tint = lerp(float3(0.72, 0.84, 1.0), float3(1.0, 0.86, 0.68), h3);
            starColor += tint * core * (0.075 + 0.68 * mag) * twinkle;
        }
    }

    // 2) Subtle tilted Milky Way band (only visible on clear dark nights)
    float3 galAxis = normalize(float3(0.42, -0.58, 0.70));
    float galLat = dot(viewDir, galAxis);
    float band = exp(-galLat * galLat * 16.0);
    float3 gp = viewDir * 5.2;
    float dust = sin(gp.x * 2.3 + cos(gp.y * 3.1)) * cos(gp.z * 2.7 - sin(gp.x * 1.9));
    float rift = smoothstep(-0.2, 0.55, dust);
    float3 milkyWay = float3(0.018, 0.024, 0.038) * band * (0.45 + 0.55 * rift) * smoothstep(0.04, 0.32, up);

    float3 starsBehindMoon = (starColor + milkyWay) * nightGain * nightFade * clearSky;
    sky += starsBehindMoon;
""" + LUNAR_SKY_RENDER + r"""
}

// Performance-tier lightweight sky-plane cloud fallback when VolumetricCloud is disabled
if (QualityTier < 0.5 && up > 0.02) {
    float2 skyProj = viewDir.xy / (up + 0.14) * 0.22 + WindVec.xy * Clock * 0.025;
    float c0 = Texture2DSampleLevel(WeatherTex, WeatherTexSampler, skyProj, 0).r;
    float c1 = Texture2DSampleLevel(WeatherTex, WeatherTexSampler, skyProj * 2.3 + float2(0.4, 0.2), 0).g;
    float cov = saturate((c0 * 0.65 + c1 * 0.35 - lerp(0.48, 0.18, overcast)) * 2.8);
    float fade = smoothstep(0.02, 0.22, up);
    float sunsetWarm = exp(-abs(sunAlt) * 6.0) * smoothstep(-0.1, 0.85, dot(viewDir, normalize(SunDir.xyz)));
    float3 dayCol = lerp(float3(1.05, 1.08, 1.15), float3(0.55, 0.60, 0.66), overcast);
    float3 warmCol = float3(1.65, 0.72, 0.36) * sunsetWarm * (1.0 - overcast * 0.7);
    sky = lerp(sky, (dayCol + warmCol) * max(0.2, CloudFallbackLum), cov * fade * 0.72);
}

// Thunder sky flash boost
sky += float3(0.55, 0.72, 1.0) * saturate(SkyFlash) * max(0.0, SkyFlashLum) * horizonMask;

return max(float3(0.0, 0.0, 0.0), sky);
"""


class MaterialGraph:
    def __init__(self, name, shading, blend, *, recreate=True):
        path = f"/Game/Bridge/{name}"
        if recreate and assets.does_asset_exist(path):
            assets.delete_asset(path)
        self.material = assets.load_asset(path) if assets.does_asset_exist(path) else tools.create_asset(
            name, "/Game/Bridge", unreal.Material, unreal.MaterialFactoryNew()
        )
        if not self.material:
            raise RuntimeError(f"Could not create {path}")
        if name == "M_McSolid" and not recreate:
            # Repeat until the editor has removed every old expression before rebuilding the retained package.
            count = library.get_num_material_expressions(self.material)
            while count:
                library.delete_all_material_expressions(self.material)
                remaining = library.get_num_material_expressions(self.material)
                if remaining >= count:
                    raise RuntimeError(f"Could not remove stale expressions from {path}")
                count = remaining
        else:
            library.delete_all_material_expressions(self.material)
        self.material.set_editor_property("blend_mode", blend)
        self.material.set_editor_property("shading_model", shading)

    def node(self, kind, x, y, **properties):
        expression = library.create_material_expression(self.material, getattr(unreal, f"MaterialExpression{kind}"), x, y)
        for key, value in properties.items():
            expression.set_editor_property(key, value)
        return expression

    def output(self, expression, pin):
        if not library.connect_material_property(expression, "", getattr(unreal.MaterialProperty, f"MP_{pin}")):
            raise RuntimeError(f"Could not connect {self.material.get_name()}.{pin}")

    def custom(self, name, code, channels, x, y, *, include_paths=(), **inputs):
        output_type = getattr(unreal.CustomMaterialOutputType, f"CMOT_FLOAT{channels}")
        expression = self.node(
            "Custom", x, y, description=name, code=code, output_type=output_type, inputs=custom_inputs(*inputs)
        )
        if include_paths:
            expression.set_editor_property("include_file_paths", list(include_paths))
        for key, source in inputs.items():
            if isinstance(source, tuple):
                link(source[0], expression, key, source[1])
            else:
                link(source, expression, key)
        return expression

    def mask(self, source, channels, x, y):
        mask = self.node("ComponentMask", x, y, **{channel: channel in channels for channel in "rgba"})
        link(source, mask)
        return mask

    def texture(self, object_path, parameter, x, y, *, linear=False):
        texture = unreal.load_asset(object_path)
        if not texture:
            raise RuntimeError(f"Missing texture asset {object_path}")
        use_srgb = (not linear) and texture.get_editor_property("srgb")
        sampler = (
            unreal.MaterialSamplerType.SAMPLERTYPE_COLOR if use_srgb
            else unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR
        )
        return self.node(
            "TextureObjectParameter", x, y, parameter_name=parameter, texture=texture, sampler_type=sampler
        )

    def scalar(self, value, x, y):
        return self.node("Constant", x, y, r=value)

    def scalar_param(self, name, default, x, y):
        return self.node("ScalarParameter", x, y, parameter_name=name, default_value=default)

    def vector_param(self, name, default, x, y):
        return self.node("VectorParameter", x, y, parameter_name=name, default_value=default)

    def save(self):
        library.recompile_material(self.material)
        if not assets.save_loaded_asset(self.material, False):
            raise RuntimeError(f"Could not save {self.material.get_name()}")
        count = library.get_num_material_expressions(self.material)
        unreal.log(f"MCUE_V26_MATERIAL_SAVED {self.material.get_name()} exprs={count}")


def water_optical_scale(graph, x, y):
    collection_name = "MPC_McWaterOptics"
    parameter_name = "WaterOpticalScale"
    path = f"/Game/Bridge/{collection_name}"
    if assets.does_asset_exist(path):
        collection = assets.load_asset(path)
    else:
        collection = tools.create_asset(collection_name, "/Game/Bridge", unreal.MaterialParameterCollection,
                                        unreal.MaterialParameterCollectionFactoryNew())
        if not collection:
            raise RuntimeError(f"Could not create {path}")
        parameter = unreal.CollectionVectorParameter()
        parameter.set_editor_property("parameter_name", parameter_name)
        parameter.set_editor_property("default_value", unreal.LinearColor(1.0, 1.0, 0.0, 0.0))
        collection.set_editor_property("vector_parameters", [parameter])
        if not assets.save_loaded_asset(collection, False):
            raise RuntimeError(f"Could not save {path}")
    return graph.node("CollectionParameter", x, y, collection=collection, parameter_name=parameter_name)


def build_wet_coat(graph, uv, atlas_size, normal_atlas, normal, porosity, metallic, base, vertex, vertex_normal,
                   precip_grid, grid_origin):
    gray = "/Engine/EngineResources/Gray"
    options = dict(include_paths=(WET_TERRAIN_INCLUDE,))
    position = graph.node("WorldPosition", -1080, -2040,
                          world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_CAMERA_RELATIVE_NO_OFFSETS)
    history = graph.texture(gray, "TerrainWetHistory", -1080, -2150, linear=True)
    history_valid = graph.scalar_param("TerrainWetHistoryValid", 0.0, -1080, -2280)
    puddles = graph.scalar_param("TerrainWetPuddlesEnabled", 1.0, -1080, -2400)
    rain_rate = graph.scalar_param("SurfaceRainRate", 0.0, -1080, -2520)
    clock = graph.scalar_param("TerrainPuddleClock", 0.0, -1080, -2640)
    footsteps = graph.texture(gray, "TerrainPuddleFootsteps", -1080, -2760, linear=True)
    foot_count = graph.scalar_param("TerrainPuddleFootstepCount", 0.0, -1080, -2890)
    wind = graph.vector_param("TerrainPuddleWind", unreal.LinearColor(1, 0, 0, 0), -1080, -3010)
    depression = graph.scalar_param("TerrainPuddleDepression", 0.0, -1080, -3130)
    curvature = graph.custom(
        "Small-scale curvature from the existing stone normal",
        "return McWetMicroCurvature(UV, Dimensions.xy, BaseNormal);",
        1, -821, -1930, **options, UV=uv, Dimensions=atlas_size, BaseNormal=normal
    )
    wet = graph.custom(
        "Rain history, sky exposure and shallow depressions",
        "return McWetTerrainState(Position, VertexNormal, Curvature, History, Precip, Origin, Valid, Puddles, Rain);",
        4, -675, -2150, **options, Position=position, VertexNormal=vertex_normal, Curvature=curvature,
        History=history, Precip=precip_grid, Origin=grid_origin, Valid=history_valid, Puddles=puddles, Rain=rain_rate
    )
    color = graph.custom(
        "Absorption in the stone beneath the water",
        "float absorb = saturate(Porosity) * (1.0 - Metallic);"
        "float cover = 0.024 * smoothstep(0.70, 5.6, Wet.w);"
        "return Base * (1.0 - Wet.x * lerp(0.025, 0.45, absorb)) * (1.0 - cover);",
        3, 100, -340, Base=base, Wet=wet, Porosity=porosity, Metallic=metallic
    )
    weight = graph.custom(
        "Thin dielectric film and pooled coverage",
        "float film = smoothstep(0.015, 0.90, Wet.x) * lerp(0.24, 0.15, saturate(Porosity));"
        "return lerp(film, 0.68, smoothstep(0.48, 5.3, Wet.w));",
        1, -355, -2500, Wet=wet, Porosity=porosity
    )
    roughness = graph.custom(
        "Independent water-surface roughness",
        "float pool = smoothstep(0.55, 5.0, Wet.w);"
        "return lerp(0.33, 0.098, pool) - 0.038 * Wet.y * (1.0 - pool) + 0.016 * pool * Wet.z;",
        1, -150, -2310, Wet=wet
    )
    top_normal = graph.custom(
        "Water-surface normal with rain rings and grounded foot impulses",
        "return McWetCoatNormal(Position, Parameters.TangentToWorld, Bottom, Wet, Coat, Clock, Footsteps, Feet, Origin.xy, Wind);",
        3, -150, -2150, **options, Position=position, Bottom=normal, Wet=wet, Coat=weight, Clock=clock,
        Footsteps=footsteps, Feet=foot_count, Origin=grid_origin, Wind=wind
    )
    refraction = graph.custom(
        "Shallow bed refraction with the water surface kept in place",
        "return McPuddleBedRefraction(UV, Position, Parameters.CameraVector, Wet.w, Enabled, FullFace);",
        4, -525, -3060, **options, UV=uv, Position=position, Wet=wet, Enabled=depression, FullFace=(vertex, "A")
    )
    bed_uv = graph.custom("Texture coordinate on the apparent stone bed", "return UV + Refraction.xy;",
                          2, -275, -3060, UV=uv, Refraction=refraction)
    bed_normal = graph.custom(
        "Stone bed normal and local basin slope",
        "return McPuddleBedNormal(Atlas, UV, Refraction, Original, Parameters.TangentToWorld);",
        3, -275, -2920, **options, Atlas=normal_atlas, UV=uv, Refraction=refraction, Original=normal
    )
    bottom_normal = graph.node("ClearCoatNormalCustomOutput", -150, -2000)
    link(bed_normal, bottom_normal)
    return color, top_normal, weight, roughness, bed_uv


def build_terrain(name, blend, shading, *, two_sided=None, subsurface_scale=1.0, subsurface_opacity=0.62):
    water = shading == unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER
    wet_coat = shading == unreal.MaterialShadingModel.MSM_CLEAR_COAT
    graph = MaterialGraph(name, shading, blend, recreate=not wet_coat)
    foliage = shading == unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE
    translucent = blend == unreal.BlendMode.BLEND_TRANSLUCENT
    graph.material.set_editor_property("two_sided", foliage if two_sided is None else two_sided)
    graph.material.set_editor_property("opacity_mask_clip_value", 0.5)

    uv = graph.node("TextureCoordinate", -1520, -140)
    light_uv = graph.node("TextureCoordinate", -875, 635, coordinate_index=1)
    size = graph.vector_param("AtlasDimensions", unreal.LinearColor(16, 16, 0, 0), -1520, 30)
    mode = graph.scalar_param("TerrainFilter", 0.0, -1520, 205)
    white = "/Engine/EngineResources/WhiteSquareTexture"
    gray = "/Engine/EngineResources/Gray"

    def sample(parameter, preview, y, atlas=None):
        if atlas is None:
            atlas = graph.texture(preview, parameter, -1520, y)
        return graph.custom(f"Sample {parameter}", SAMPLE, 4, -1100, y,
                            Atlas=atlas, UV=uv, Dimensions=size, Mode=mode)

    pbr = sample("PbrAtlas", gray, 730)
    vertex = graph.node("VertexColor", -870, -170)

    # Shared weather / wetness inputs
    precip_grid = graph.texture(gray, "PrecipGrid", -1100, -830, linear=True)
    grid_origin = graph.vector_param("PrecipGridOrigin", unreal.LinearColor(0, 0, 256, -64), -1100, -680)
    grid_valid = graph.scalar_param("PrecipGridValid", 0.0, -1100, -560)
    local_rain = graph.scalar_param("LocalRain", 0.0, -860, -1080)

    p1 = graph.scalar_param("WaterP1", 0, -1320, -1700)
    debug = graph.scalar_param("WaterDebug", 0, -1320, -1810)
    face_uvs = {f"F{i}": graph.node("TextureCoordinate", -1500, -2050 - i * 96, coordinate_index=i)
                for i in range(2, 8)}

    if water:
        pack_normals = graph.texture(gray, "NormalAtlas", -1520, 330, linear=True)
        foam, debug_color, exit_transmission, sky_reflection = build_water(
            graph, vertex, pack_normals, uv, precip_grid, grid_origin, grid_valid, local_rain, p1, debug, face_uvs)
        albedo = sample("VoxelAtlas", white, -455)
        emissive_base = graph.scalar_param("EmissiveBase", 48.0, -610, 560)
        water_glow = graph.custom("Resource pack water emission",
                                  "return Debug > 0.5 ? DebugColor * DebugLuminance : ExitTransmission + SkyReflection + Texel.rgb * Tint.rgb * (Packed.b * EmissiveBase) * (1.0 - Foam);",
                                  3, -130, 555, Packed=pbr, Texel=albedo, Tint=vertex, Foam=foam,
                                  EmissiveBase=emissive_base, Debug=debug, DebugColor=debug_color,
                                  DebugLuminance=graph.scalar_param("WaterDebugLuminance", 520.0, -840, -1890),
                                  ExitTransmission=exit_transmission, SkyReflection=sky_reflection)
        graph.output(water_glow, "EMISSIVE_COLOR")
        graph.output(graph.mask(pbr, "a", -95, 825), "AMBIENT_OCCLUSION")
    else:
        albedo = sample("VoxelAtlas", white, -455)
        normal_atlas = graph.texture(gray, "NormalAtlas", -1520, 330)
        encoded_normal = sample("NormalAtlas", gray, 330, normal_atlas)
        tint = graph.custom("Biome tint and vertex AO", "return Texel.rgb * Tint;", 3, -608, -380,
                            Texel=albedo, Tint=vertex)
        normal = graph.custom(
            "LabPBR tangent-space normal",
            "return normalize(float3((Encoded.rg * 255.0 - 128.0) / 127.0, max(0.004, Encoded.b * 2.0 - 1.0)));",
            3, -610, 330, Encoded=encoded_normal
        )
        porosity = graph.mask(encoded_normal, "a", -610, 470)
        roughness = graph.custom("Convert linear roughness to UE perceptual roughness",
                                 "return sqrt(max(0.0016, Packed.r));", 1, -610, 730, Packed=pbr)
        metallic = graph.mask(pbr, "g", -610, 830)
        fallback = graph.scalar_param("FallbackBlockFill", 0.0, -875, 497)
        sky_fill = graph.vector_param("FallbackSkyFill", unreal.LinearColor(0.0, 0.0, 0.0, 0.0), -860, 925)
        emissive_base = graph.scalar_param("EmissiveBase", 48.0, -875, 390)
        emissive_day = graph.scalar_param("EmissiveDayBoost", 0.0, -875, 290)
        ambient_floor = graph.vector_param("AmbientFloor", unreal.LinearColor(0.0, 0.0, 0.0, 0.0), -860, 1040)
        world_pos = graph.node("WorldPosition", -1100, -1080)
        clock = graph.scalar_param("EnvClock", 0.0, -1100, -950)
        local_snow = graph.scalar_param("LocalSnow", 0.0, -860, -980)
        wetness = (graph.scalar(0.0, -860, -880) if wet_coat else
                   graph.scalar_param("SurfaceWetness", 0.0, -860, -880))
        puddle_level = (graph.scalar(0.0, -860, -780) if foliage or wet_coat else
                        graph.scalar_param("PuddleLevel", 0.0, -860, -780))
        v_normal = graph.node("VertexNormalWS", -860, -680)

        shore = graph.custom("Dry shore", "return float2(0.0, 0.0);", 2, -600, -1890) if translucent or foliage else graph.custom(
            "Water film and porous shore wetness from mesh contact segments",
            r"""
            if (Enabled < 0.5) return 0.0;
            float3 A[2] = {float3(F2, F3.x), float3(F5, F6.x)};
            float2 B[2] = {float2(F3.y, F4.x), float2(F6.y, F7.x)};
            float2 Fade = float2(F4.y, F7.y);
            float2 Wet = 0.0;
            [unroll] for (uint I = 0; I < 2; ++I)
            {
                float Span = B[I].x;
                float3 Segment = float3(max(0, Span), max(0, -Span), B[I].y);
                float LengthSq = dot(Segment.xy, Segment.xy);
                if (LengthSq < 0.0001) continue;
                float T = saturate(dot(-A[I].xy, Segment.xy) / LengthSq);
                float3 Near = A[I] + Segment * T;
                float Above = -Near.z;
                float Lateral = length(Near.xy);
                float Height = max(0, Above);
                float Dist = length(float2(Lateral, Height));
                float Reach = lerp(13.0, 34.0, saturate(Porosity));
                float Film = 1.0 - smoothstep(2.0, 10.0, Dist);
                float Bulk = 1.0 - smoothstep(4.0, Reach, Dist);
                float WettedSide = smoothstep(-4.0, -0.9, Above);
                Wet = max(Wet, float2(Bulk * Fade[I], Film * pow(Fade[I], 4)) * WettedSide);
            }
            return Wet;
            """, 2, -600, -1890, Enabled=p1, Porosity=porosity, **face_uvs
        )
        wet_surface = graph.custom(
            "Weather surface wetness, puddles and snow frost",
            TERRAIN_WETNESS, 4, -220, -340,
            Albedo=tint, NormalTS=normal, Roughness=roughness, Metallic=metallic, Porosity=porosity,
            WorldPos=world_pos, VertexNormalWS=v_normal, Clock=clock,
            Wetness=wetness, PuddleLevel=puddle_level, LocalRain=local_rain, LocalSnow=local_snow, Shore=shore,
            PrecipGrid=precip_grid, GridOrigin=grid_origin, GridValid=grid_valid
        )
        final_albedo = graph.mask(wet_surface, "rgb", 80, -380)
        final_rough = graph.mask(wet_surface, "a", 80, -250)
        if wet_coat:
            final_albedo, final_normal, coat_weight, coat_roughness, bed_uv = build_wet_coat(
                graph, uv, size, normal_atlas, normal, porosity, metallic, final_albedo, vertex, v_normal,
                precip_grid, grid_origin)
            link(bed_uv, albedo, "UV")
            link(bed_uv, pbr, "UV")
        else:
            final_normal = graph.custom(
                "Puddle normal flattening and rain ripples",
                TERRAIN_WET_NORMAL, 3, -180, 220,
                NormalTS=normal, WorldPos=world_pos, VertexNormalWS=v_normal, Clock=clock,
                PuddleLevel=puddle_level, LocalRain=local_rain,
                PrecipGrid=precip_grid, GridOrigin=grid_origin, GridValid=grid_valid
            )
        emissive = graph.custom("Pixel emission and light fallback", r"""
            if (Debug > 5.5 && Debug < 6.5) return float3(Shore.r, Shore.g * 0.68, 0.02) * 24.0;
            float encoded = LightUV.x * (65535.0 / 4096.0);
            float flags = floor(encoded + 0.005);
            float mode = fmod(flags,8.0);
            float level = saturate((encoded - flags) * (32.0 / 15.0));
            float block = flags < 8.0 ? level : 0;
            float mask = Packed.b;
            if (mode > 0.5 && mode < 1.5) mask = 0;
            if (mode > 1.5) {
                float y = dot(Texel.rgb, float3(.2126, .7152, .0722));
                float peak = max(Texel.r, max(Texel.g, Texel.b));
                float value = .5 * (peak + y);
                if (mode > 4.5) {
                    // Preserve the hue of dark mineral grains while keeping bright deposits subtle.
                    mask *= .006 / (.012 + .988*peak);
                } else if (mode > 3.5) {
                    float reference = max(1.0/255.0, (1.0-mask)/.75);
                    float ring = smoothstep(.89, 1.005, y / reference);
                    mask = .018 + saturate(4.0*mask) * .30 * ring * ring;
                } else if (mode > 2.5) {
                    mask = smoothstep(.032, .74, value);
                } else {
                    mask *= .035 + .965 * smoothstep(.015, .80, value);
                }
            }
            if (flags >= 8.0) mask *= pow(level,1.25);
            float3 blockFill = float3(1.0, 0.66, 0.37) * (Fill * pow(block, 3.0));
            float3 skyFill = Sky.rgb * (pow(LightUV.y, 4.0) * Packed.a);
            float emissiveGain = EmissiveBase + EmissiveDayBoost * (LightUV.y * LightUV.y);
            return Pigment * (mask * emissiveGain) + Tint * (blockFill + skyFill + AmbientFloor.rgb);
            """, 3, -130, 555, Packed=pbr, Texel=albedo, Pigment=tint, Tint=final_albedo, Fill=fallback, Sky=sky_fill, LightUV=light_uv,
            EmissiveBase=emissive_base, EmissiveDayBoost=emissive_day, AmbientFloor=ambient_floor, Shore=shore, Debug=debug)

        occlusion = graph.mask(pbr, "a", -95, 825)
        if wet_coat:
            # The raw CustomData outputs are hidden from the editor's Python enum.
            attributes = graph.node("MakeMaterialAttributes", 430, -255)
            for pin, expression in (("BaseColor", final_albedo), ("Roughness", final_rough),
                                    ("Metallic", metallic), ("AmbientOcclusion", occlusion),
                                    ("EmissiveColor", emissive), ("Normal", final_normal),
                                    ("ClearCoat", coat_weight), ("ClearCoatRoughness", coat_roughness)):
                link(expression, attributes, pin)
            graph.material.set_editor_property("use_material_attributes", True)
            graph.output(attributes, "MATERIAL_ATTRIBUTES")
        else:
            graph.output(final_albedo, "BASE_COLOR")
            graph.output(final_rough, "ROUGHNESS")
            graph.output(metallic, "METALLIC")
            graph.output(occlusion, "AMBIENT_OCCLUSION")
            graph.output(emissive, "EMISSIVE_COLOR")
            graph.output(final_normal, "NORMAL")

        if foliage:
            if subsurface_scale != 1.0:
                sss_scale = graph.scalar_param("VegetationSubsurfaceScale", subsurface_scale, -355, 40)
                sss_color = graph.custom(
                    "Scaled vegetation subsurface transmission", "return Albedo * Scale;", 3, -130, 10,
                    Albedo=final_albedo, Scale=sss_scale
                )
                graph.output(sss_color, "SUBSURFACE_COLOR")
            else:
                graph.output(final_albedo, "SUBSURFACE_COLOR")
            graph.output(graph.scalar(subsurface_opacity, -130, 105), "OPACITY")

    if blend == unreal.BlendMode.BLEND_MASKED or translucent:
        coverage = graph.custom("Minecraft alpha coverage",
                                "float a = Texel.a * VertexAlpha; return a < 0.1 ? 0.0 : a;",
                                1, -355, -165, Texel=albedo, VertexAlpha=(vertex, "A"))
        graph.output(coverage, "OPACITY" if translucent else "OPACITY_MASK")
    if translucent:
        graph.material.set_editor_property("translucency_lighting_mode",
                                           unreal.TranslucencyLightingMode.TLM_SURFACE_PER_PIXEL_LIGHTING)
        graph.material.set_editor_property("refraction_method", unreal.RefractionMode.RM_INDEX_OF_REFRACTION)
        graph.output(graph.scalar(1.08, -120, 35), "REFRACTION")

    graph.save()


def build_water(graph, tint, pack_normals, uv, precip_grid, grid_origin, grid_valid, local_rain,
                p1, debug, faces):
    def pixel(name, code, channels, x, y, *, fallback="0.0", **kwargs):
        return graph.custom(name, f"#if PIXELSHADER\n{code}\n#else\nreturn {fallback};\n#endif",
                            channels, x, y, include_paths=(WATER_P3_INCLUDE, WATER_INTERFACE_INCLUDE), **kwargs)

    def simulation_texture(parameter, y):
        return graph.texture(WATER_FALLBACK, parameter, -1520, y, linear=True)

    displacement = simulation_texture("DisplacementFoamTex", -1480)
    derivatives = simulation_texture("DerivativesTex", -1260)
    moments = simulation_texture("SlopeMomentsTex", -1150)
    caustics = simulation_texture("CausticsTex", -1030)
    ready = graph.scalar_param("WaterSimReady", 0.0, -860, -1460)
    wind = graph.scalar_param("WindStrength", 0.18, -860, -1360)
    pack_weight = graph.scalar_param("WaterLabPbrNormalWeight", 0.0, -860, -1260)
    relative_pos = graph.node("WorldPosition", -1100, -1080,
                              world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_CAMERA_RELATIVE_NO_OFFSETS)
    water_grid = graph.texture("/Engine/EngineResources/Gray", "WaterGrid", -1520, -1560, linear=True)
    water_field = graph.texture("/Engine/EngineResources/Gray", "WaterField", -1520, -1660, linear=True)
    water_info = graph.vector_param("WaterGridInfo", unreal.LinearColor(0, 0, -64, 0), -1520, -1750)
    ripple = graph.texture("/Engine/EngineResources/Gray", "WaterRipple", -1520, -2440, linear=True)
    ripple_info = graph.vector_param("WaterRippleInfo", unreal.LinearColor(0, 0, 0, 0), -1520, -2550)
    ripple_rain = graph.scalar_param("WaterRippleRain", 0.0, -1520, -2660)
    p3 = graph.scalar_param("WaterP3Enabled", 0.0, -1520, -2760)
    wpo_enabled = graph.scalar_param("WaterWpoEnabled", 0.0, -1520, -2860)
    global_rain = graph.scalar_param("GlobalWaterRain", 0, -1280, -1500)
    clock = graph.scalar_param("EnvClock", 0, -1280, -1570)
    flow, strength, drop, face_mode, west_east, north_south = (faces[f"F{i}"] for i in range(2, 8))
    p2 = graph.scalar_param("WaterP2", 0.0, -1150, -2080)
    optical_scale = water_optical_scale(graph, -1150, -1970)
    flat = graph.scalar_param("WaterP2FlatInterface", 0.0, -1150, -2190)
    eye_in_water = graph.scalar_param("WaterEyeInWater", 0.0, -860, -2060)
    exit_face = pixel("Camera-side check for the underside of a free water surface",
                      "return McWaterExitFace(Parameters, EyeInWater, Mode.x, Debug);", 1, -600, -2020,
                      EyeInWater=eye_in_water, Mode=face_mode, Debug=debug)
    exit_optics = pixel("Water-to-air direction and Fresnel including total internal reflection",
                        "return McWaterExitOptics(Parameters, ExitFace, Flat);", 4, -340, -2000, ExitFace=exit_face, Flat=flat)
    site = pixel("Height-matched water depth, shore, fetch and exposure",
                 "return McWaterSite(Parameters, Grid, Field, Origin, Valid, max(P1, P3), Mode.x);", 4, -960, -1750,
                 fallback="float4(-1, 0, 0, 0)", Grid=water_grid, Field=water_field, Origin=water_info, Valid=(water_info, "A"),
                 P1=p1, P3=p3, Mode=face_mode)
    local_ripple = pixel("Local free-surface gradient, foam history and height",
                         "return McWaterLocalRipple(Parameters, Ripple, float4(Domain, CellSize), GridInfo, Site);", 4, -695, -2450,
                         Ripple=ripple, Domain=ripple_info, CellSize=(ripple_info, "A"), GridInfo=water_info, Site=site)
    ice = pixel("Damping and optical preset near surface ice",
                "return McWaterIce(Parameters, Field, Origin.xyz, Site, Enabled);", 1, -950, -2630,
                Field=water_field, Origin=water_info, Site=site, Enabled=p3)
    waves = pixel("Local weights for the three water spectra",
                  "return McWaterP1Waves(Site, P1, Mode) * lerp(1.0, 0.35, Ice);", 3, -700, -1630, fallback="1.0",
                  Site=site, P1=p1, Mode=face_mode, Ice=ice)
    exposure = pixel("Rain exposure at the water surface", """
                     float Exposed = P1 < 0.5 ? McWaterRainExposure(Parameters, Grid, Origin, Valid, LocalRain)
                         : McWaterP1Rain(Parameters, Grid, Origin, Valid, GlobalRain, Site, Mode.x);
                     if (Domain.z > 0.0 && RippleRain > 0.0 && Site.x >= 0.0)
                     {
                         float2 Local = McWaterRelativeMC(Parameters, WaterOrigin.xyz).xy - Domain.xy;
                         Exposed *= 1.0 - RippleRain * McRippleFade(Local, Domain.z);
                     }
                     return Exposed;
                     """, 1, -600, -1060, Grid=precip_grid, Origin=grid_origin, Valid=grid_valid,
                     LocalRain=local_rain, GlobalRain=global_rain, P1=p1, Mode=face_mode, Site=site,
                     Domain=ripple_info, RippleRain=ripple_rain, WaterOrigin=water_info)
    motion = pixel("Dual-phase horizontal stream flow and gravity-aligned falling water",
                   "return McWaterP1Motion(Parameters, Flow, Strength, Drop, Mode, Clock, P1);",
                   3, -400, -1550, Flow=flow, Strength=strength, Drop=drop, Mode=face_mode, Clock=clock, P1=p1)
    debug_color = pixel("Per-face water diagnostic views",
                        "return Debug > 7.5 ? McWaterP3Debug(Parameters, Debug, Ripple) : McWaterP1Debug(Parameters, Debug, Flow, Strength, Drop, Mode, Site, Rain, Clock);",
                        3, -400, -1870, Debug=debug, Flow=flow, Strength=strength, Drop=drop, Mode=face_mode,
                        Site=site, Rain=exposure, Clock=clock, Ripple=local_ripple)
    surface = pixel(
        "FFT and local ripple normal with slope variance", """
        return McWaterSurface(Parameters, Position, Derivatives, Moments, Ready, Wind, Exposed,
                              PackNormals, PackNormalsSampler, UV, PackWeight, Waves, Motion, ExitFace, Ripple.xy);
        """, 4, -150, -1260, fallback="float4(0, 0, 1, 0.018)",
        Position=relative_pos, Derivatives=derivatives, Moments=moments, Ready=ready, Wind=wind, Exposed=exposure,
        PackNormals=pack_normals, UV=uv, PackWeight=pack_weight, Waves=waves, Motion=motion, ExitFace=exit_face,
        Ripple=local_ripple
    )
    graph.output(graph.mask(surface, "rgb", 120, -1240), "NORMAL")
    rough = graph.mask(surface, "a", 120, -1130)
    crest_data = pixel("FFT crests, persistent whitecaps and local wake foam", """
                       float2 Crest = McWaterCrestData(Parameters, Position, Displacement, Ready, Waves);
                       return float2(max(Crest.x, Ripple.z), Crest.y + Ripple.w);
                       """, 2, -450, -920, Position=relative_pos, Displacement=displacement, Ready=ready,
                       Waves=waves, Ripple=local_ripple)

    scene_depth = graph.node("SceneDepthWithoutWater", -620, -620)
    pixel_depth = graph.node("PixelDepth", -620, -510)
    vertical_depth = pixel("Vertical bed depth in centimetres",
                           "return McWaterVerticalDepth(Parameters, SceneD);", 1, -270, -620, SceneD=scene_depth)
    foam = pixel("Shore foam and persistent whitecaps",
                 """
                 if (Debug > 0.5) return 0.0;
                 if (P1 < 0.5) return McWaterFoam(Parameters, Lace, Micro, Depth, FoamAndHeight) * Ready;
                 float2 Impact = Drop;
                 if (Mode.x < 0.5 && Drop.y > 0.0 && Domain.z > 0.0 && Site.x >= 0.0)
                 {
                     float2 Local = McWaterRelativeMC(Parameters, WaterOrigin).xy - Domain.xy;
                     Impact.y *= 1.0 - McRippleFade(Local, Domain.z);
                 }
                 return McWaterP1Foam(Parameters, Lace, Micro, Flow, Strength, Impact, Mode,
                                       float4(WE, NS), Waves, FoamAndHeight, Clock);
                 """, 1, -90, -810,
                 Lace=graph.texture("/Game/Bridge/T_WaterFoamLace", "FoamLace", -1100, 90, linear=True),
                 Micro=graph.texture("/Game/Bridge/T_WaterFoamMicro", "FoamMicro", -1100, 220, linear=True),
                 Depth=vertical_depth, FoamAndHeight=crest_data, Ready=ready, P1=p1, Debug=debug,
                 Flow=flow, Strength=strength, Drop=drop, Mode=face_mode, WE=west_east, NS=north_south,
                 Waves=waves, Clock=clock, Domain=ripple_info, Site=site, WaterOrigin=water_info)
    roughness = graph.custom("Filtered GGX and foam roughness",
                             "return lerp(Rough, lerp(0.55, 0.75, Foam), Foam);", 1, 155, -980,
                             Rough=rough, Foam=foam)
    graph.output(roughness, "ROUGHNESS")
    graph.material.set_editor_property("max_world_position_offset_displacement", 8.0)
    wpo = graph.custom("Vertical waves with strict geometric and shore clearance", """
        #if VERTEXSHADER
        return McWaterGeometryOffset(Parameters, Grid, Field, float4(GridInfo, GridValid), Displacement, Ripple, float4(Domain, 0),
            Mask, Wind, Enabled, Ready);
        #else
        return 0.0;
        #endif
        """, 3, 125, -2380, include_paths=(WATER_P3_INCLUDE,), Grid=water_grid, Field=water_field,
        GridInfo=water_info, GridValid=(water_info, "A"), Displacement=displacement, Ripple=ripple, Domain=ripple_info,
        Mask=(tint, "A"), Wind=wind, Enabled=wpo_enabled, Ready=ready)
    graph.output(wpo, "WORLD_POSITION_OFFSET")
    base_color = pixel("Foam reflectance or equivalent Fresnel F0 below the free surface", """
                       [branch] if (ExitFace > 0.5)
                           return McWaterExitReflection(Parameters, Roughness, Optics.w, Tint.rgb, P2, Flat, OpticalScale.rg);
                       return Foam * float3(0.36, 0.41, 0.43);
                       """, 3, 155, -880, Foam=foam, ExitFace=exit_face, Optics=exit_optics,
                       Roughness=roughness, Tint=tint, P2=p2, Flat=flat, OpticalScale=optical_scale)
    graph.output(base_color, "BASE_COLOR")
    graph.output(exit_face, "METALLIC")
    opacity = graph.custom("Foam coverage or replacement of the air-to-water volume path",
                           "return ExitFace > 0.5 ? 1.0 : lerp(0.012, 0.84, Foam);",
                           1, 155, -770, Foam=foam, ExitFace=exit_face)
    graph.output(opacity, "OPACITY")
    specular = graph.custom("Foam specular attenuation", "return lerp(0.255, 0.055, Foam);", 1, 155, -660, Foam=foam)
    graph.output(specular, "SPECULAR")

    graph.material.set_editor_property("refraction_method", unreal.RefractionMode.RM_PIXEL_NORMAL_OFFSET)
    refraction = graph.custom("Pixel-normal refraction at a shore or across an open stream", """
                              if (P1 > 0.5 && Mode.x > 1.5) return 1.0;
                              if (P1 > 0.5 && (Mode.x > 0.5 || Drop.x > 0.5))
                                  return 1.0 + 0.017 * saturate((SceneD - PixelD) / 25.0);
                              return 1.0 + 0.024 * saturate(Depth / 25.0);
                              """, 1, 155, -420, Depth=vertical_depth, SceneD=scene_depth,
                              PixelD=pixel_depth, P1=p1, Mode=face_mode, Drop=drop)
    graph.output(refraction, "REFRACTION")

    sun_power = graph.scalar_param("SunCaustics", 0.0, -620, -350)
    sun_dir = graph.vector_param("WaterSunDirection", unreal.LinearColor(0, 0, 1, 0), -620, -250)
    crest = pixel("Forward scattering at wave crests",
                  "return McWaterCrest(Parameters, FoamAndHeight.y, SunDir.xyz, SunPower);", 1, -90, -240,
                  FoamAndHeight=crest_data, SunDir=sun_dir, SunPower=sun_power)
    volume = graph.node("SingleLayerWaterMaterialOutput", 460, -560)
    scattering, absorption, phase, color_scale = library.get_material_expression_input_names(volume)
    density = pixel("Optical path through connected water before the ray exits into air",
                    "return McWaterP1Density(Parameters, Grid, Field, Origin, Valid, SceneD, PixelD, Refraction, Mode, Drop.x, Site, P1, EyeInWater);",
                    1, -260, -40, fallback="1.0", Grid=water_grid, Field=water_field, Origin=water_info, Valid=(water_info, "A"),
                    SceneD=scene_depth, PixelD=pixel_depth, Refraction=refraction, Mode=face_mode, Drop=drop,
                    Site=site, P1=p1, EyeInWater=eye_in_water)
    coefficients = """
        float3 Abs, Scat;
        McWaterCoefficients(Tint.rgb, P2, Abs, Scat);
        Abs = lerp(Abs, float3(0.0074, 0.0016, 0.00135), Ice);
        Scat = lerp(Scat, float3(0.000012, 0.000094, 0.000105), Ice);
        McWaterApplyOpticalScale(OpticalScale.rg, Abs, Scat);
    """
    scatter = graph.custom("Biome and wave-crest scattering", coefficients +
                           "return (Scat + Crest * float3(0.0000017, 0.000011, 0.000008) + Air * float3(0.00025, 0.00028, 0.00030)) * Density;",
                           3, 155, -240, include_paths=(WATER_INCLUDE,), Tint=tint, Crest=crest, Density=density,
                           P2=p2, Ice=ice, OpticalScale=optical_scale,
                           Air=pixel("Air entrainment", "return P1 * McWaterP1Air(Parameters, Strength, Drop, Mode);",
                                     1, -220, -120, P1=p1, Strength=strength, Drop=drop, Mode=face_mode))
    absorb = graph.custom("Biome absorption per centimetre", coefficients + "return Abs * Density;",
                           3, 155, -110, include_paths=(WATER_INCLUDE,), Tint=tint, Density=density,
                           P2=p2, Ice=ice, OpticalScale=optical_scale)
    link(scatter, volume, scattering)
    link(absorb, volume, absorption)
    link(graph.custom("Scattering phase at crests", "float3 A, S; float G; McWaterOpticalPreset(Tint.rgb, A, S, G); return lerp(lerp(0.40, 0.72, Crest), G, P2);",
                      1, 155, 35, Crest=crest, Tint=tint, P2=p2, include_paths=(WATER_INCLUDE,)), volume, phase)
    scene_caustics = graph.scalar_param("UseSceneCaustics", 0.0, -610, 188)
    focus = pixel("Refracted lake-bed caustics with dispersion and VSM transmission",
                  "return UseSceneCaustics > 0.5 ? 1.0 : McWaterCausticColor(Parameters, Caustics, SceneD, PixelD, Refraction, SunDir.xyz, SunPower, Ready, Waves.y);",
                  3, 140, 190, fallback="1.0", Caustics=caustics, SceneD=scene_depth, PixelD=pixel_depth,
                  Refraction=refraction, SunDir=sun_dir, SunPower=sun_power, Ready=ready, Waves=waves, UseSceneCaustics=scene_caustics)
    link(focus, volume, color_scale)
    exit_transmission = pixel("Screen-depth intersection for the refracted ray in air",
                              "return McWaterExitTransmission(Parameters, ExitFace, Optics, Site);",
                              3, 125, -1980, ExitFace=exit_face, Optics=exit_optics, Site=site)
    strike = graph.vector_param("WaterStrike", unreal.LinearColor(0, 0, 0, 0), -620, 270)
    sky_reflection = pixel("Atmosphere and specular lightning reflection", """
                      return McWaterP3Reflection(Parameters, float4(Strike, Power), SkyFlash.xy, Site, Mode.x,
                          Roughness, Foam, Enabled, SkyFallback);
                      """, 3, 140, 370, Site=site, Mode=face_mode, Roughness=roughness, Foam=foam, Enabled=p3,
                      Strike=strike, Power=(strike, "A"),
                      SkyFlash=graph.vector_param("WaterSkyFlash", unreal.LinearColor(0, 0, 0, 0), -620, 380),
                      SkyFallback=graph.scalar_param("WaterSkyFallback", 0.0, -620, 465))
    return foam, debug_color, exit_transmission, sky_reflection


def build_sun_caustics(*, glass=False):
    name = "M_McGlassLightFunction" if glass else "M_McSunCausticsLF"
    graph = MaterialGraph(name, unreal.MaterialShadingModel.MSM_UNLIT, unreal.BlendMode.BLEND_OPAQUE,
                          recreate=not glass)
    graph.material.set_editor_property("material_domain", unreal.MaterialDomain.MD_LIGHT_FUNCTION)
    # This world-space projection uses renderer-supplied shadow textures and can't be baked to an atlas tile.
    fallback = "/Engine/EngineResources/DefaultTexture"
    grid = graph.texture(fallback, "P2WaterGrid", -760, -280, linear=True)
    air = graph.texture(fallback, "P2AirShadow", -760, -140, linear=True)
    wet = graph.texture(fallback, "P2WaterShadow", -760, 0, linear=True)
    pattern = graph.texture(WATER_FALLBACK, "CausticsTex", -760, 140, linear=True)
    zero = unreal.LinearColor(0, 0, 0, 0)
    origin = graph.vector_param("P2GridOriginCm", zero, -760, 260)
    center = graph.vector_param("P2ShadowCenterM", zero, -760, 360)
    sun = graph.vector_param("P2SunAir", unreal.LinearColor(0, 0, 1, 0), -760, 460)
    refracted = graph.vector_param("P2SunWater", unreal.LinearColor(0, 0, 1, 0), -760, 560)
    phase = graph.vector_param("P2PatternPhaseM", zero, -760, 660)
    radius = graph.scalar_param("P2ShadowRadiusM", 56.0, -760, 760)
    strength = graph.scalar_param("P2CausticStrength", 0.0, -760, 860)
    position = graph.node("WorldPosition", -760, 970,
                          world_position_shader_offset=unreal.WorldPositionIncludedOffsets.WPT_CAMERA_RELATIVE_NO_OFFSETS)
    output = graph.custom("World-space solar redistribution after water entry", """
        if (Strength <= 0.0) return 1.0;
        float3 ToCamera = CameraRelative.xyz - ResolvedView.TranslatedWorldCameraOrigin;
        if (dot(ToCamera, ToCamera) > 8200.0 * 8200.0) return 1.0;
        FDFVector3 World = WSToDF(GetWorldPosition_NoMaterialOffsets(Parameters));
        float3 P = ((World.High - GridOrigin.xyz) + World.Low) * 0.01;
        return McWaterCausticFactor(Grid, AirShadow, WetShadow, Pattern, PatternSampler,
            P, Center.xyz, SunAir.xyz, SunWater.xyz, Radius, Strength, Phase.xy);
    """, 1, -80, 60, include_paths=("/MinecraftUEViewer/Private/McWaterSun.ush",),
        Grid=grid, AirShadow=air, WetShadow=wet, Pattern=pattern, GridOrigin=origin, Center=center, SunAir=sun,
        SunWater=refracted, Radius=radius, Strength=strength, Phase=phase, CameraRelative=position)
    if glass:
        # The legacy light function writes one attenuation channel, so the viewer supplies one emitter per RGB channel.
        ray = graph.vector_param("GlassRay", zero, -600, 1240)
        output = graph.custom("Transmission through world-space glass between the receiver and this emitter", """
            const FDFVector3 World = WSToDF(GetWorldPosition_NoMaterialOffsets(Parameters));
            const FDFVector3 Anchor = MakeDFVector3(GlassHigh, GlassLow);
            const float3 Origin = DFSubtractDemote(World, Anchor) * 0.01;
            return WaterFactor * McGlassTransmission(Volumes, uint(Count), Origin, float4(Ray, Directional), Channel);
        """, 1, 390, 600, include_paths=("/MinecraftUEViewer/Private/McGlassLightFunction.ush",),
            Volumes=graph.texture(fallback, "GlassVolumes", -600, 1040, linear=True),
            Count=graph.scalar_param("GlassNodeCount", 0.0, -600, 1140), Ray=ray, Directional=(ray, "A"),
            GlassHigh=graph.vector_param("GlassAnchorHigh", zero, -600, 1340),
            GlassLow=graph.vector_param("GlassAnchorLow", zero, -600, 1440),
            Channel=graph.vector_param("GlassChannel", unreal.LinearColor(1.0, 0.0, 0.0, 0.0), -600, 1540),
            WaterFactor=output)
    graph.output(output, "EMISSIVE_COLOR")
    graph.save()


def build_water_particle(name, bubble):
    graph = MaterialGraph(name, unreal.MaterialShadingModel.MSM_UNLIT, unreal.BlendMode.BLEND_ADDITIVE, recreate=False)
    graph.material.set_editor_property("used_with_niagara_sprites", True)
    graph.material.set_editor_property("translucency_pass", unreal.MaterialTranslucencyPass.MTP_AFTER_DOF)
    uv = graph.node("TextureCoordinate", -580, 0)
    particle = graph.node("ParticleColor", -580, 160)
    bounds = graph.node("DepthFade", -580, 310, fade_distance_default=6.0)
    rgb = graph.custom("Attenuate the particle by the water distance to its world position", """
        float3 A, S;
        float G;
        McWaterOpticalPreset(float3(0.05, 0.183, 0.955), A, S, G);
        McWaterApplyOpticalScale(OpticalScale.rg, A, S);
        float Len = length(GetTranslatedWorldPosition(Parameters) - ResolvedView.TranslatedWorldCameraOrigin) * 0.01;
        return Color.rgb * McWaterTransmittance(A + S, Len) * Gain;
    """, 3, 50, 0, include_paths=("/MinecraftUEViewer/Private/McWaterOptics.ush",),
        Color=particle, Gain=graph.scalar(18.0 if bubble else 11.0, -560, -120),
        OpticalScale=water_optical_scale(graph, -560, -230))
    shape = ("""
        float d = length(UV * 2.0 - 1.0);
        float Ring = exp(-pow((d - 0.69) * 10.0, 2.0));
        return Alpha * Fade * (Ring * (0.3 + 0.7 * UV.y) + 0.065 * exp(-d * d * 6.0));
    """ if bubble else """
        float r2 = dot(UV * 2.0 - 1.0, UV * 2.0 - 1.0);
        return Alpha * Fade * exp(-r2 * 5.5) * (1.0 - smoothstep(0.75, 1.0, r2));
    """)
    opacity = graph.custom("Depth-faded bubble ring or plankton particle", shape, 1, 50, 260,
                          UV=uv, Alpha=(particle, "A"), Fade=bounds)
    graph.output(rgb, "EMISSIVE_COLOR")
    graph.output(opacity, "OPACITY")
    graph.save()


def build_surface_water_particles():
    mist = MaterialGraph("M_McWaterfallVolume", unreal.MaterialShadingModel.MSM_DEFAULT_LIT, unreal.BlendMode.BLEND_ADDITIVE)
    mist.material.set_editor_property("material_domain", unreal.MaterialDomain.MD_VOLUME)
    mist.material.set_editor_property("used_with_niagara_sprites", True)
    density = mist.custom("Soft ascending water mist density", """
        #if PIXELSHADER && USE_PARTICLE_POSITION
        float3 Q = (GetTranslatedWorldPosition(Parameters) - Parameters.Particle.TranslatedWorldPositionAndSize.xyz) / max(1.0, Radius);
        Q.z *= 1.20;
        float R2 = dot(Q, Q);
        float Fine = 0.75 + 0.25 * sin(dot(Q, float3(6.0, -3.5, 5.0)) + 2.3 * sin(4.0 * Q.z));
        float Envelope = smoothstep(0.0, 0.12, Age) * (1.0 - smoothstep(0.56, 1.0, Age));
        return exp(-3.2 * R2) * (1.0 - smoothstep(0.52, 0.95, R2)) * Fine * Envelope * 2.6;
        #else
        return 0.0;
        #endif
    """, 1, -90, 20, Radius=mist.node("ParticleRadius", -430, 50),
        Age=mist.node("ParticleRelativeTime", -430, 150))
    albedo = mist.custom("Water-droplet primary rainbow angular scattering", """
        #if PIXELSHADER
        float3 ViewToMist = normalize(GetTranslatedWorldPosition(Parameters) - ResolvedView.TranslatedWorldCameraOrigin);
        float CosTheta = dot(ViewToMist, -Sun);
        float3 Lines = (CosTheta - float3(0.739, 0.752, 0.764)) / 0.009;
        float3 Rainbow = exp(-Lines * Lines) * smoothstep(0.01, 0.14, Sun.z);
        return float3(0.69, 0.75, 0.78) + Rainbow * float3(0.20, 0.16, 0.17);
        #else
        return float3(0.69, 0.75, 0.78);
        #endif
    """, 3, -90, -175, Sun=mist.node("SkyAtmosphereLightDirection", -430, -175))
    mist.output(density, "SUBSURFACE_COLOR")
    mist.output(albedo, "BASE_COLOR")
    mist.save()

    spray = MaterialGraph("M_McWaterSpray", unreal.MaterialShadingModel.MSM_DEFAULT_LIT, unreal.BlendMode.BLEND_TRANSLUCENT)
    spray.material.set_editor_property("used_with_niagara_sprites", True)
    spray.material.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_DIRECTIONAL)
    spray.material.set_editor_property("translucency_pass", unreal.MaterialTranslucencyPass.MTP_AFTER_DOF)
    uv = spray.node("TextureCoordinate", -550, 0)
    age = spray.node("ParticleRelativeTime", -550, 140)
    color = spray.node("ParticleColor", -550, 260)
    depth = spray.node("DepthFade", -550, 425, fade_distance_default=4.0)
    shape = spray.custom("Soft, short-lived droplet and thin specular rim", """
        float2 Q = UV * 2.0 - 1.0;
        float R = length(Q * float2(1.0, 0.86));
        float Body = 1.0 - smoothstep(0.42, 0.94, R);
        float Rim = exp(-pow((R - 0.67) * 9.0, 2.0)) * saturate(0.67 + Q.y * 0.55);
        return (0.55 * Body + 0.45 * Rim) * (1.0 - smoothstep(0.46, 1.0, Age)) * Alpha * Depth;
    """, 1, -100, 90, UV=uv, Age=age, Depth=depth, Alpha=(color, "A"))
    spray.output(spray.mask(color, "rgb", -165, -190), "BASE_COLOR")
    spray.output(shape, "OPACITY")
    spray.output(spray.scalar(0.13, -170, 240), "ROUGHNESS")
    spray.output(spray.custom("Forward-scattered light inside droplets", "return Color.rgb * 1.8;",
                              3, -100, 400, Color=color), "EMISSIVE_COLOR")
    spray.save()

    veil = MaterialGraph("M_McWaterfallVeil", unreal.MaterialShadingModel.MSM_DEFAULT_LIT, unreal.BlendMode.BLEND_TRANSLUCENT)
    veil.material.set_editor_property("used_with_niagara_sprites", True)
    veil.material.set_editor_property("translucency_lighting_mode", unreal.TranslucencyLightingMode.TLM_VOLUMETRIC_DIRECTIONAL)
    uv = veil.node("TextureCoordinate", -570, 0)
    age = veil.node("ParticleRelativeTime", -570, 145)
    depth = veil.node("DepthFade", -570, 290, fade_distance_default=55.0)
    mist_opacity = veil.custom("Soft droplets in the rising mist", """
        float2 P = (UV * 2.0 - 1.0) * float2(0.91, 1.20);
        float R2 = dot(P, P);
        float Erosion = 0.75 + 0.25 * sin(dot(P, float2(8.1, 5.2)) + 1.4 * sin(6.4 * P.y + Age * 3.0));
        float Life = smoothstep(0.0, 0.20, Age) * (1.0 - smoothstep(0.50, 1.0, Age));
        return 0.42 * exp(-3.0 * R2) * (1.0 - smoothstep(0.54, 1.0, R2)) * Depth * Life * Erosion;
    """, 1, -100, 90, UV=uv, Age=age, Depth=depth)
    mist_light = veil.custom("Diffuse skylight and rainbow scattering in the fine spray", """
        #if PIXELSHADER
        float3 Ray = normalize(GetTranslatedWorldPosition(Parameters) - ResolvedView.TranslatedWorldCameraOrigin);
        float CosTheta = dot(Ray, -Sun);
        float3 Lines = (CosTheta - float3(0.739, 0.752, 0.764)) / 0.010;
        float Day = smoothstep(0.0, 0.23, Sun.z);
        return Day * (float3(11.0, 15.0, 18.0) + exp(-Lines * Lines) * float3(3.4, 2.7, 3.0));
        #else
        return 0.0;
        #endif
    """, 3, -100, 280, Sun=veil.node("SkyAtmosphereLightDirection", -570, 420))
    veil.output(veil.vector_param("MistAlbedo", unreal.LinearColor(0.62, 0.74, 0.78, 1), -400, -150), "BASE_COLOR")
    veil.output(mist_opacity, "OPACITY")
    veil.output(mist_light, "EMISSIVE_COLOR")
    veil.save()


def write_foam_texture(path, cells, seed, pore_width):
    """Generate a periodic field of bubble films with jittered cell centres."""
    size = 128
    rng = random.Random(seed)
    points = [(rng.uniform(0.17, 0.83), rng.uniform(0.17, 0.83)) for _ in range(cells * cells)]
    pixels = bytearray()
    for y in range(size):
        for x in range(size):
            u, v = (x + 0.5) * cells / size, (y + 0.5) * cells / size
            i, j = math.floor(u), math.floor(v)
            distances = []
            for cy in range(j - 1, j + 2):
                for cx in range(i - 1, i + 2):
                    ox, oy = points[(cy % cells) * cells + cx % cells]
                    distances.append((cx + ox - u) ** 2 + (cy + oy - v) ** 2)
            a, b = sorted(distances)[:2]
            edge = (math.sqrt(b) - math.sqrt(a)) / pore_width
            film = math.exp(-edge * edge)
            amplitude = 0.8 + 0.2 * math.sin(2.0 * math.pi * (x + 2 * y) / size)
            value = round(255 * film * amplitude)
            pixels.extend((value, value, value, 255))
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, size, size, 32, 0x28)
    path.write_bytes(header + pixels)


def build_water_textures():
    if not assets.does_asset_exist(WATER_FALLBACK):
        factory = unreal.Texture2DArrayFactory()
        fallback = tools.create_asset("T_WaterArrayFallback", "/Game/Bridge", unreal.Texture2DArray, factory)
        if not fallback:
            raise RuntimeError("Could not create the FFT texture array fallback")
        fallback.set_editor_property("source_textures", [unreal.load_asset("/Engine/EngineResources/Black")])
        fallback.set_editor_property("srgb", False)
        if not assets.save_loaded_asset(fallback, False):
            raise RuntimeError("Could not save the FFT texture array fallback")

    with tempfile.TemporaryDirectory(prefix="mcue-water-") as scratch:
        for name, cells, seed, pore_width in (("T_WaterFoamLace", 12, 42, 0.18), ("T_WaterFoamMicro", 26, 918, 0.27)):
            source = Path(scratch) / (name + ".tga")
            write_foam_texture(source, cells, seed, pore_width)
            task = unreal.AssetImportTask()
            task.set_editor_properties(dict(filename=str(source), destination_path="/Game/Bridge",
                                            destination_name=name, automated=True, replace_existing=True))
            tools.import_asset_tasks([task])
            texture = assets.load_asset("/Game/Bridge/" + name)
            if not texture:
                raise RuntimeError(f"Could not import {name}")
            texture.set_editor_property("srgb", False)
            texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_GRAYSCALE)
            texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_SIMPLE_AVERAGE)
            if not assets.save_loaded_asset(texture, False):
                raise RuntimeError(f"Could not save {name}")




def build_skydome():
    graph = MaterialGraph("M_McSkyDome", unreal.MaterialShadingModel.MSM_UNLIT, unreal.BlendMode.BLEND_OPAQUE,
                          recreate=False)
    graph.material.set_editor_property("is_sky", True)
    graph.material.set_editor_property("two_sided", True)

    atmo = graph.node("SkyAtmosphereViewLuminance", -860, -320)
    sun_disk = graph.node("SkyAtmosphereLightDiskLuminance", -860, -180)
    sun_diameter = graph.scalar_param("SunAngularDiameterDegrees", 3.3, -1360, -180)
    sun_diameter_clamp = graph.node("Clamp", -1110, -180, min_default=0.1, max_default=15.0)
    link(sun_diameter, sun_diameter_clamp)
    link(sun_diameter_clamp, sun_disk)
    world_pos = graph.node("WorldPosition", -860, -40)
    cam_pos = graph.node("CameraPositionWS", -860, 80)
    sun_dir = graph.vector_param("SunDirection", unreal.LinearColor(0, 0, 1, 1), -860, 200)
    moon_dir = graph.vector_param("MoonDirection", unreal.LinearColor(0, 0, -1, 0), -860, 340)
    moon_phase = graph.scalar_param("MoonPhase", 0.0, -860, 480)
    overcast = graph.scalar_param("CloudWeather", 0.0, -860, 580)
    sky_flash = graph.scalar_param("SkyFlash", 0.0, -860, 680)
    clock = graph.scalar_param("EnvClock", 0.0, -860, 780)
    tier = graph.scalar_param("CloudQualityTier", 2.0, -860, 880)
    wind_vec = graph.vector_param("WindVector", unreal.LinearColor(0.022, 0.009, 0.0, 0.0), -860, 980)
    night_sky_scale = graph.scalar_param("NightSkyScale", 24.0, -860, 1260)
    cloud_fallback_lum = graph.scalar_param("CloudFallbackLum", 120.0, -860, 1360)
    sky_flash_lum = graph.scalar_param("SkyFlashLum", 120.0, -860, 1460)
    weather_tex = graph.texture("/Engine/EngineSky/VolumetricClouds/CloudWeatherTexture", "WeatherTex", -860, 1120)
    lunar_albedo_path = "/Game/Bridge/LunarSky/T_McMoonAlbedo_4K"
    lunar_normal_path = "/Game/Bridge/LunarSky/T_McMoonNormal_4K"
    lunar_maps_available = all(assets.does_asset_exist(path) for path in (lunar_albedo_path, lunar_normal_path))
    gray = "/Engine/EngineResources/Gray"
    lunar_albedo = graph.texture(lunar_albedo_path if lunar_maps_available else gray, "LunarAlbedo", -860, 1580)
    if lunar_maps_available:
        lunar_normal = graph.node("TextureObjectParameter", -860, 1720, parameter_name="LunarRelief",
                                  texture=assets.load_asset(lunar_normal_path),
                                  sampler_type=unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL)
    else:
        lunar_normal = graph.texture(gray, "LunarRelief", -860, 1720)
    lunar_relief_strength = graph.scalar(1.0 if lunar_maps_available else 0.0, -860, 2120)
    moon_diameter = graph.scalar_param("MoonAngularDiameterDegrees", 2.9, -860, 1860)
    moon_luminance = graph.scalar_param("MoonLuminance", 1.0, -860, 1980)

    sky_out = graph.custom(
        "Physical sky + stars + Milky Way + 8-phase moon + performance fallback clouds",
        SKYDOME_NIGHT_AND_FALLBACK, 3, -180, 180,
        AtmoLuminance=atmo, SunDisk=sun_disk, SunDiameter=sun_diameter_clamp, WorldPos=world_pos, CamPos=cam_pos,
        SunDir=sun_dir, MoonDir=moon_dir, MoonPhase=moon_phase,
        Overcast=overcast, SkyFlash=sky_flash, Clock=clock, QualityTier=tier,
        WindVec=wind_vec, WeatherTex=weather_tex,
        NightSkyScale=night_sky_scale, CloudFallbackLum=cloud_fallback_lum, SkyFlashLum=sky_flash_lum,
        LunarAlbedo=lunar_albedo, LunarRelief=lunar_normal, LunarReliefStrength=lunar_relief_strength,
        MoonDiameter=moon_diameter,
        MoonLuminance=moon_luminance,
    )
    graph.output(sky_out, "EMISSIVE_COLOR")
    graph.save()


def build_precipitation_material():
    # Lightweight translucent unlit/soft-lit material for roof-occluded rain streaks and snowflakes.
    graph = MaterialGraph("M_McPrecipitation", unreal.MaterialShadingModel.MSM_UNLIT, unreal.BlendMode.BLEND_TRANSLUCENT)
    graph.material.set_editor_property("two_sided", True)
    uv = graph.node("TextureCoordinate", -640, -120)
    vcol = graph.node("VertexColor", -640, 40)
    tint = graph.vector_param("PrecipTint", unreal.LinearColor(0.76, 0.84, 0.94, 0.55), -640, 200)
    mode = graph.scalar_param("PrecipMode", 0.0, -640, 340)  # 0=Rain streak, 1=Snowflake
    precip_lum = graph.scalar_param("PrecipLuminance", 16.0, -640, 440)

    shape = graph.custom(
        "Rain streak / snowflake alpha mask",
        r"""
        float2 p = UV * 2.0 - 1.0;
        // Rain: crisp elongated streak with tapered tips
        float rainA = smoothstep(1.0, 0.05, abs(p.x)) * smoothstep(1.0, 0.25, abs(p.y));
        // Snow: soft radial crystal flake
        float r = length(p);
        float snowA = smoothstep(1.0, 0.18, r);
        return lerp(rainA, snowA, saturate(Mode)) * TintAlpha * VAlpha;
        """,
        1, -180, 120, UV=uv, Mode=mode, TintAlpha=(tint, "A"), VAlpha=(vcol, "A")
    )
    color = graph.custom(
        "Precipitation lit tint",
        "return TintRGB * VColor * max(0.05, PrecipLum);",
        3, -180, -80, TintRGB=tint, VColor=vcol, PrecipLum=precip_lum
    )
    graph.output(color, "EMISSIVE_COLOR")
    graph.output(shape, "OPACITY")
    graph.save()


def build_lightning_material():
    graph = MaterialGraph("M_McLightning", unreal.MaterialShadingModel.MSM_UNLIT, unreal.BlendMode.BLEND_ADDITIVE)
    graph.material.set_editor_property("two_sided", True)
    uv = graph.node("TextureCoordinate", -520, -80)
    vcol = graph.node("VertexColor", -520, 80)
    intensity = graph.scalar_param("LightningIntensity", 1.0, -520, 220)
    em = graph.custom(
        "High-energy lightning core + plasma glow",
        r"""
        float d = abs(UV.x * 2.0 - 1.0);
        float core = exp(-d * d * 18.0);
        float glow = exp(-d * 3.2);
        float3 col = lerp(float3(0.48, 0.74, 1.0), float3(1.0, 0.99, 1.0), core);
        return col * (core * 420.0 + glow * 55.0) * Intensity * VColor.rgb * VAlpha;
        """,
        3, -120, 40, UV=uv, VColor=vcol, VAlpha=(vcol, "A"), Intensity=intensity
    )
    graph.output(em, "EMISSIVE_COLOR")
    graph.save()


def build_solid():
    build_terrain("M_McSolid", unreal.BlendMode.BLEND_OPAQUE, unreal.MaterialShadingModel.MSM_CLEAR_COAT)


def main():
    p3_assets = os.environ.get("MCUE_WATER_P3_ONLY")
    if os.environ.get("MCUE_WET_STONE_ONLY") == "1":
        build_solid()
    elif os.environ.get("MCUE_SKY_ONLY") == "1":
        build_skydome()
    elif os.environ.get("MCUE_GLASS_LIGHT_ONLY") == "1":
        build_sun_caustics(glass=True)
    elif os.environ.get("MCUE_WATER_OPTICS_ONLY") == "1":
        build_terrain("M_McWater", unreal.BlendMode.BLEND_OPAQUE, unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
        build_water_particle("M_McPlankton", False)
        build_water_particle("M_McWaterBubble", True)
    elif p3_assets in ("1", "surface"):
        build_terrain("M_McWater", unreal.BlendMode.BLEND_OPAQUE, unreal.MaterialShadingModel.MSM_SINGLE_LAYER_WATER)
        if p3_assets == "1":
            build_surface_water_particles()
    elif os.environ.get("MCUE_P2_CAUSTICS_ONLY") == "1":
        build_sun_caustics()
    elif os.environ.get("MCUE_VEGETATION_ONLY") == "1":
        build_terrain("M_McFoliage", unreal.BlendMode.BLEND_MASKED, unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE)
        build_terrain("M_McVegetation", unreal.BlendMode.BLEND_MASKED, unreal.MaterialShadingModel.MSM_TWO_SIDED_FOLIAGE,
                      two_sided=False, subsurface_scale=0.52, subsurface_opacity=0.42)
    elif "--cloud-only" in sys.argv:
        build_cloud()
    else:
        blend = unreal.BlendMode
        shading = unreal.MaterialShadingModel
        if os.environ.get("MCUE_NON_WATER_TERRAIN_ONLY") != "1":
            if os.environ.get("MCUE_TERRAIN_MATERIALS_ONLY") != "1":
                build_water_textures()
            build_terrain("M_McWater", blend.BLEND_OPAQUE, shading.MSM_SINGLE_LAYER_WATER)
            build_sun_caustics()
            build_sun_caustics(glass=True)
            build_water_particle("M_McPlankton", False)
            build_water_particle("M_McWaterBubble", True)
            build_surface_water_particles()
        if os.environ.get("MCUE_WATER_MATERIALS_ONLY") != "1":
            build_solid()
            build_terrain("M_McCutout", blend.BLEND_MASKED, shading.MSM_DEFAULT_LIT)
            build_terrain("M_McFoliage", blend.BLEND_MASKED, shading.MSM_TWO_SIDED_FOLIAGE)
            build_terrain("M_McVegetation", blend.BLEND_MASKED, shading.MSM_TWO_SIDED_FOLIAGE,
                          two_sided=False, subsurface_scale=0.52, subsurface_opacity=0.42)
            build_terrain("M_McTranslucent", blend.BLEND_TRANSLUCENT, shading.MSM_DEFAULT_LIT)
            if os.environ.get("MCUE_TERRAIN_MATERIALS_ONLY") != "1" and os.environ.get("MCUE_NON_WATER_TERRAIN_ONLY") != "1":
                build_cloud()
                build_skydome()
                build_precipitation_material()
                build_lightning_material()


if __name__ == "__main__":
    main()
