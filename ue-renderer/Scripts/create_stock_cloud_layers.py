"""Stack the built-in Epic cloud preset at three altitudes in one volume material."""
import unreal

from create_terrain_materials import assets, custom_inputs, library, link, tools


ENGINE = "/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud"
MASTER = "/Game/Bridge/M_EpicCloudLayers"
INSTANCE = "/Game/Bridge/MI_EpicCloudLayers"

# The preset receives its own 0..1 height in each band. The other channels identify
# the band, scale its optical density and leave clear air between neighbouring bands.
BANDS = """
float index = H < 0.418 ? 0.0 : (H < 0.710 ? 1.0 : 2.0);
float bottom = index < 0.5 ? 0.0 : (index < 1.5 ? 0.435 : 0.75);
float depth = index < 0.5 ? 0.40 : (index < 1.5 ? 0.24 : 0.235);
float local = (H - bottom) / depth;
float weight = index < 0.5 ? smoothstep(0.0, 0.08, max(Coverage, Rain))
             : (index < 1.5 ? (Tier >= 1.0 ? Middle * 0.48 : 0.0)
                             : (Tier >= 2.0 ? Upper * 0.14 : 0.0));
float edge = smoothstep(0.0, 0.05, local) * (1.0 - smoothstep(0.92, 1.0, local));
return float4(saturate(local), index, weight * edge, (weight > 0.0 && edge > 0.0) ? 1.0 : 0.0);
"""

COORDINATES = """
float2 xy = Position.xy * 0.00001;
float2 drift = Clock * Wind.xy;
if (Band.y > 0.5 && Band.y < 1.5)
{
    xy = float2(0.8 * xy.x - 0.6 * xy.y, 0.6 * xy.x + 0.8 * xy.y) * 1.35
       + MiddlePlacement.xy + drift * float2(0.46, -0.24);
}
else if (Band.y >= 1.5)
{
    xy = float2(0.60 * xy.x + 0.80 * xy.y, -0.80 * xy.x + 0.60 * xy.y)
       * float2(0.65, 1.75) + UpperPlacement.xy + drift * float2(-0.32, 0.76);
}
// Give the stock noise the same vertical coordinates as its single-layer reference.
return float3(xy * 100000.0, Position.z + (Band.x * 4.4 - H * 6.4) * 100000.0);
"""


def build_cloud():
    unreal.AssetRegistryHelpers.get_asset_registry().scan_paths_synchronous(["/Engine/EngineSky/VolumetricClouds", "/Game/Bridge"])

    def preset_copy(source, target):
        if assets.does_asset_exist(target):
            return assets.load_asset(target)
        folder, name = target.rsplit("/", 1)
        return tools.duplicate_asset(name, folder, unreal.load_asset(source))

    master = preset_copy(ENGINE, MASTER)
    if not master:
        raise RuntimeError(f"Could not duplicate Epic cloud material {ENGINE}")
    original = list(library.get_material_expressions(master))
    tagged = {str(n.get_editor_property("description")): n for n in original
              if isinstance(n, unreal.MaterialExpressionCustom)}
    editing = "Epic cloud altitude bands" in tagged

    def scalar(name, value, y):
        n = library.create_material_expression(master, unreal.MaterialExpressionScalarParameter, -3700, y)
        n.set_editor_property("parameter_name", name)
        n.set_editor_property("default_value", value)
        return n

    def vector(name, value, y):
        n = library.create_material_expression(master, unreal.MaterialExpressionVectorParameter, -3700, y)
        n.set_editor_property("parameter_name", name)
        n.set_editor_property("default_value", unreal.LinearColor(*value))
        return n

    def expression(title, code, channels, x, y, **inputs):
        n = library.create_material_expression(master, unreal.MaterialExpressionCustom, x, y)
        n.set_editor_property("description", title)
        n.set_editor_property("inputs", custom_inputs(*inputs))
        n.set_editor_property("output_type", getattr(unreal.CustomMaterialOutputType, f"CMOT_FLOAT{channels}"))
        n.set_editor_property("code", code)
        for pin, src in inputs.items():
            node, output = src if isinstance(src, tuple) else (src, "")
            link(node, n, pin, output)
        return n

    if editing:
        for title, code in [("Epic cloud altitude bands", BANDS), ("Epic layer sample position", COORDINATES)]:
            tagged[title].set_editor_property("code", code)
    else:
        # Record the stock connections before inserting the coordinate adapters.
        height_edges, position_edges, coverage_edges = [], [], []
        for dst in original:
            pins = library.get_material_expression_input_names(dst)
            for pin, src in zip(pins, library.get_inputs_for_material_expression(master, dst)):
                if not src:
                    continue
                if isinstance(src, unreal.MaterialExpressionCloudSampleAttribute):
                    output = library.get_input_node_output_name_for_material_expression(dst, src)
                    if output == "NormAltitudeInLayer":
                        height_edges.append((src, dst, str(pin)))
                elif isinstance(src, unreal.MaterialExpressionWorldPosition) and isinstance(dst, unreal.MaterialExpressionDivide):
                    position_edges.append((src, dst, str(pin)))
                elif isinstance(src, unreal.MaterialExpressionScalarParameter) and str(src.get_editor_property("parameter_name")) == "Cloud_GlobalCoverage":
                    coverage_edges.append((src, dst, str(pin)))
        if not height_edges or len(position_edges) != 1 or not coverage_edges:
            raise RuntimeError("Epic cloud graph has changed. Check its sample position, height and coverage inputs.")
        h = (height_edges[0][0], "NormAltitudeInLayer")
        coverage = scalar("CloudCoverage", 0.5, 150)
        rain = scalar("CloudWeather", 0.0, 320)
        band = expression("Epic cloud altitude bands", BANDS, 4, -3300, 100, H=h, Coverage=coverage, Rain=rain,
                          Tier=scalar("CloudQualityTier", 3.0, 480), Middle=scalar("MidCloudAmount", 1.0, 650),
                          Upper=scalar("CirrusAmount", 1.0, 820))
        height = expression("Epic layer local height", "return Band.x;", 1, -3000, -80, Band=band)
        coords = expression("Epic layer sample position", COORDINATES, 3, -3000, 120,
                            Position=(position_edges[0][0], "XYZ"), Band=band, H=h,
                            Clock=scalar("EnvClock", 0.0, 990), Wind=vector("WindVector", (.022,.009,0,0), 1150),
                            MiddlePlacement=vector("MiddlePlacementKm", (17,32,0,0), 1310),
                            UpperPlacement=vector("UpperPlacementKm", (-36,21,0,0), 1470))
        local_coverage = expression("Epic layer coverage",
                                    "return Base + (Band.y < 0.5 ? (Coverage - 0.5) * 0.70 : 0.0) + Rain * 0.36;",
                                    1, -3000, 350, Base=coverage_edges[0][0], Band=band, Coverage=coverage, Rain=rain)
        for _,dst,pin in height_edges:
            link(height, dst, pin)
        for _,dst,pin in position_edges:
            link(coords, dst, pin)
        for _,dst,pin in coverage_edges:
            link(local_coverage, dst, pin)

        prop = unreal.MaterialProperty.MP_SUBSURFACE_COLOR
        ext = library.get_material_property_input_node(master, prop)
        ext_pin = library.get_material_property_input_node_output_name(master, prop)
        density = expression("Epic layer optical density", "return Stock * Band.z;", 3, 1000, 100,
                             Stock=(ext, ext_pin), Band=band)
        if not library.connect_material_property(density, "", prop):
            raise RuntimeError("Could not connect the stock cloud extinction")
        out = next(n for n in original if isinstance(n, unreal.MaterialExpressionVolumetricAdvancedMaterialOutput))
        pins = library.get_material_expression_input_names(out)
        ins = library.get_inputs_for_material_expression(master, out)
        stock = next(n for pin,n in zip(pins,ins) if str(pin) == "ConservativeDensity")
        output = library.get_input_node_output_name_for_material_expression(out, stock)
        occupancy = expression("Epic layer empty space", "return float4(Stock.x * Band.w, Stock.yzw);",
                               4, 1000, 360, Stock=(stock,output), Band=band)
        link(occupancy, out, "ConservativeDensity")
    library.recompile_material(master)
    if not assets.save_loaded_asset(master, False):
        raise RuntimeError("Could not save the stacked Epic cloud material")

    # The stock instance supplies the weather layout, height-profile atlas, Perlin–Worley
    # volume, lighting and authored parameter overrides. Keep those overrides intact.
    instance = preset_copy(ENGINE + "_Inst", INSTANCE)
    if not instance:
        raise RuntimeError("Could not duplicate the Epic cloud preset")
    library.set_material_instance_parent(instance, master)
    library.update_material_instance(instance)
    if not assets.save_loaded_asset(instance, False):
        raise RuntimeError("Could not save the stacked Epic cloud preset")
    unreal.log("MCUE_EPIC_CLOUD_LAYERS_SAVED")
