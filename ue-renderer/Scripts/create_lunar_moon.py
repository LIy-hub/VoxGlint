"""Build the native Moon assets in an editor project with the Unreal Python plugin."""

import os
from pathlib import Path
import struct
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.dont_write_bytecode = True
from create_terrain_materials import assets, custom_inputs, library, link, tools
from lunar_moon_shading import LUNAR_LIGHTING

MODEL_DIR = "/Game/LunarMoon"
SKY_DIR = "/Game/Bridge/LunarSky"


def save(asset):
    if not assets.save_loaded_asset(asset, False):
        raise RuntimeError(f"Cannot save {asset.get_path_name()}")


def import_texture(source, folder, name, compression, srgb=False, resident=False):
    task = unreal.AssetImportTask()
    task.set_editor_properties({
        "filename": str(source / f"{name}.png"),
        "destination_path": folder,
        "automated": True,
        "replace_existing": True,
    })
    tools.import_asset_tasks([task])
    texture = next((item for item in task.get_objects() if isinstance(item, unreal.Texture2D)), None)
    if not texture:
        raise RuntimeError(f"Texture import failed: {name}")
    texture.set_editor_properties({
        "compression_settings": compression,
        "compression_no_alpha": True,
        "srgb": srgb,
        "address_x": unreal.TextureAddress.TA_WRAP,
        "address_y": unreal.TextureAddress.TA_CLAMP,
        "lod_group": unreal.TextureGroup.TEXTUREGROUP_SKYBOX,
        "mip_gen_settings": unreal.TextureMipGenSettings.TMGS_SIMPLE_AVERAGE,
        "never_stream": resident,
        "virtual_texture_streaming": False,
        "flip_green_channel": False,
    })
    save(texture)
    unreal.log(f"LUNAR_TEXTURE {texture.get_name()} {texture.blueprint_get_size_x()}x{texture.blueprint_get_size_y()}")
    return texture


def material(name, shading):
    path = f"{MODEL_DIR}/{name}"
    mat = assets.load_asset(path) if assets.does_asset_exist(path) else tools.create_asset(
        name, MODEL_DIR, unreal.Material, unreal.MaterialFactoryNew())
    if not mat:
        raise RuntimeError(f"Cannot create {path}")
    library.delete_all_material_expressions(mat)
    mat.set_editor_property("shading_model", shading)
    return mat


def node(mat, kind, x, y, **properties):
    expr = library.create_material_expression(mat, getattr(unreal, f"MaterialExpression{kind}"), x, y)
    expr.set_editor_properties(properties)
    return expr


def connect_output(expr, prop):
    if not library.connect_material_property(expr, "", getattr(unreal.MaterialProperty, f"MP_{prop}")):
        raise RuntimeError(f"Cannot connect material output {prop}")


def color_and_relief(mat, albedo, normal):
    sampler = unreal.MaterialSamplerType
    color = node(mat, "TextureSampleParameter2D", -700, -200, parameter_name="Albedo", texture=albedo,
                 sampler_type=sampler.SAMPLERTYPE_COLOR)
    relief = node(mat, "TextureSampleParameter2D", -700, 100, parameter_name="TerrainNormal", texture=normal,
                  sampler_type=sampler.SAMPLERTYPE_NORMAL)
    return color, relief


def scalar(mat, name, value, x, y):
    return node(mat, "ScalarParameter", x, y, parameter_name=name, default_value=value)


def build_surface(albedo, normal):
    mat = material("M_LunarSurface", unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    color, relief = color_and_relief(mat, albedo, normal)
    connect_output(color, "BASE_COLOR")
    connect_output(relief, "NORMAL")
    connect_output(scalar(mat, "Roughness", 0.94, -350, 370), "ROUGHNESS")
    connect_output(scalar(mat, "Specular", 0.12, -350, 490), "SPECULAR")
    library.recompile_material(mat)
    save(mat)
    return mat


def build_sky_material(albedo, normal):
    mat = material("M_LunarSky", unreal.MaterialShadingModel.MSM_UNLIT)
    color, relief = color_and_relief(mat, albedo, normal)
    camera = node(mat, "CameraVectorWS", -980, 430)
    local_view = node(mat, "Transform", -700, 430,
                      transform_source_type=unreal.MaterialVectorCoordTransformSource.TRANSFORMSOURCE_WORLD,
                      transform_type=unreal.MaterialVectorCoordTransform.TRANSFORM_LOCAL)
    link(camera, local_view, "")
    inputs = {
        "UV": node(mat, "TextureCoordinate", -700, 560),
        "Color": color,
        "NormalTS": relief,
        "ViewLocal": local_view,
        "Phase": scalar(mat, "Phase", 0.0, -410, 710),
        "LightLatitude": scalar(mat, "LightLatitude", 6.0, -410, 830),
        "Relief": scalar(mat, "ReliefStrength", 1.35, -410, 950),
        "Luminance": scalar(mat, "Luminance", 0.95, -410, 1070),
        "Earthshine": scalar(mat, "Earthshine", 0.0, -410, 1190),
    }
    code = LUNAR_LIGHTING + r"""
float3 view = normalize(float3(-ViewLocal.y, ViewLocal.z, ViewLocal.x));
float lit = lunar.Intensity(lunar.Radial(UV), NormalTS, view, Phase, LightLatitude, Relief, Earthshine);
return lunar.GradeAlbedo(Color) * max(Luminance, 0.0) * lit;
"""
    shade = node(mat, "Custom", -110, 195, description="Lunar phase and topographic shading", code=code,
                 output_type=unreal.CustomMaterialOutputType.CMOT_FLOAT3, inputs=custom_inputs(*inputs))
    for pin, expression in inputs.items():
        link(expression, shade, pin)
    connect_output(shade, "EMISSIVE_COLOR")
    library.recompile_material(mat)
    save(mat)
    return mat


def sky_instance(mat, name, **parameters):
    path = f"{MODEL_DIR}/{name}"
    inst = assets.load_asset(path) if assets.does_asset_exist(path) else tools.create_asset(
        name, MODEL_DIR, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    library.set_material_instance_parent(inst, mat)
    for key, value in parameters.items():
        library.set_material_instance_scalar_parameter_value(inst, key, value)
    library.update_material_instance(inst)
    save(inst)
    return inst


def build_mesh(source, surface):
    path = f"{MODEL_DIR}/SM_LunarMoon"
    data = (source / "lunar_mesh.bin").read_bytes()
    n_vertices, n_triangles = struct.unpack_from("<II", data)
    triangle_offset = 8 + n_vertices * 32
    rows = list(struct.iter_unpack("<8f", data[8:triangle_offset]))
    triangles = list(struct.iter_unpack("<3i", data[triangle_offset:]))
    if len(triangles) != n_triangles:
        raise RuntimeError("Lunar mesh triangle count mismatch")
    buffers = unreal.GeometryScriptSimpleMeshBuffers()
    buffers.set_editor_properties({
        "vertices": [unreal.Vector(*row[:3]) for row in rows],
        "normals": [unreal.Vector(*row[3:6]) for row in rows],
        "uv0": [unreal.Vector2D(*row[6:]) for row in rows],
        "triangles": [unreal.IntVector(*triangle) for triangle in triangles],
    })
    dynamic_mesh = unreal.new_object(unreal.DynamicMesh)
    dynamic_mesh.append_buffers_to_mesh(buffers)
    if assets.does_asset_exist(path):
        mesh = assets.load_asset(path)
        options = unreal.GeometryScriptCopyMeshToAssetOptions(enable_recompute_tangents=True)
        _, outcome = unreal.GeometryScript_AssetUtils.copy_mesh_to_static_mesh(
            dynamic_mesh, mesh, options, unreal.GeometryScriptMeshWriteLOD())
        if outcome != unreal.GeometryScriptOutcomePins.SUCCESS:
            raise RuntimeError(f"Static mesh update failed: {outcome}")
    else:
        options = unreal.GeometryScriptCreateNewStaticMeshAssetOptions(
            enable_recompute_tangents=True, enable_collision=False)
        mesh, outcome = unreal.GeometryScript_NewAssetUtils.create_new_static_mesh_asset_from_mesh(
            dynamic_mesh, path, options)
        if not mesh:
            raise RuntimeError(f"Static mesh build failed: {outcome}")

    mesh.set_material(0, surface)
    editor = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    build = editor.get_lod_build_settings(mesh, 0)
    build.set_editor_properties({"use_full_precision_u_vs": True, "use_high_precision_tangent_basis": True,
                                 "generate_lightmap_u_vs": False, "distance_field_resolution_scale": 0.0,
                                 "max_lumen_mesh_cards": 0})
    editor.set_lod_build_settings(mesh, 0, build)
    reductions = [unreal.StaticMeshReductionSettings(p, s) for p, s in
                  ((1.0, 1.0), (0.25, 0.32), (0.065, 0.12), (0.018, 0.045))]
    reduction = unreal.StaticMeshReductionOptions(auto_compute_lod_screen_size=False, reduction_settings=reductions)
    if editor.set_lods(mesh, reduction) != len(reductions):
        raise RuntimeError("Cannot generate the lunar mesh LODs")
    save(mesh)
    unreal.log(f"LUNAR_MODEL {mesh.get_name()} {[mesh.get_num_triangles(i) for i in range(len(reductions))]}")
    return mesh


def build_level(mesh, preview_material):
    levels = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    path = f"{MODEL_DIR}/L_MoonPreview"
    if assets.does_asset_exist(path):
        if not levels.load_level(path):
            raise RuntimeError("Cannot reopen the lunar preview level")
        for actor in actors.get_all_level_actors():
            if actor.get_actor_label() in {"Moon", "Moon Camera", "Moon Exposure"}:
                actors.destroy_actor(actor)
    elif not levels.new_level(path):
        raise RuntimeError("Cannot create the lunar preview level")

    actor = actors.spawn_actor_from_class(unreal.StaticMeshActor, unreal.Vector())
    actor.set_actor_label("Moon")
    component = actor.static_mesh_component
    component.set_static_mesh(mesh)
    component.set_material(0, preview_material)
    component.set_collision_profile_name("NoCollision")
    component.set_cast_shadow(False)

    camera_location = unreal.Vector(7000.0, 0.0, 0.0)
    camera_rotation = unreal.Rotator(yaw=180.0)
    camera = actors.spawn_actor_from_class(unreal.CameraActor, camera_location, camera_rotation)
    camera.set_actor_label("Moon Camera")
    camera.set_editor_property("auto_activate_for_player", unreal.AutoReceiveInput.PLAYER0)
    camera.camera_component.set_editor_properties({"field_of_view": 36.0, "aspect_ratio": 16.0 / 9.0,
                                                   "constrain_aspect_ratio": False})

    volume = actors.spawn_actor_from_class(unreal.PostProcessVolume, unreal.Vector())
    volume.set_actor_label("Moon Exposure")
    volume.set_editor_property("unbound", True)
    settings = volume.get_editor_property("settings")
    settings.set_editor_properties({
        "override_auto_exposure_method": True,
        "auto_exposure_method": unreal.AutoExposureMethod.AEM_MANUAL,
        "override_auto_exposure_apply_physical_camera_exposure": True,
        "auto_exposure_apply_physical_camera_exposure": False,
        "override_auto_exposure_bias": True,
        "auto_exposure_bias": 0.0,
        "override_bloom_intensity": True,
        "bloom_intensity": 0.0,
        "override_vignette_intensity": True,
        "vignette_intensity": 0.0,
    })
    volume.set_editor_property("settings", settings)
    levels.set_level_viewport_camera_info(camera_location, camera_rotation, "None")
    levels.set_level_viewport_fov(36.0, "None")
    if not levels.save_current_level():
        raise RuntimeError("Cannot save the lunar preview level")


def import_sky_textures(source):
    compression = unreal.TextureCompressionSettings
    import_texture(source, SKY_DIR, "T_McMoonAlbedo_4K", compression.TC_BC7, srgb=True, resident=True)
    import_texture(source, SKY_DIR, "T_McMoonNormal_4K", compression.TC_NORMALMAP, resident=True)


def main():
    source = Path(os.environ["MCUE_LUNAR_INPUT"])
    compression = unreal.TextureCompressionSettings
    if os.environ.get("MCUE_LUNAR_BUILD") == "sky":
        import_sky_textures(source)
    else:
        albedo = import_texture(source, MODEL_DIR, "T_LunarAlbedo_8K", compression.TC_BC7, srgb=True)
        normal = import_texture(source, MODEL_DIR, "T_LunarNormal_8K", compression.TC_NORMALMAP)
        import_texture(source, MODEL_DIR, "T_LunarElevation_16bit", compression.TC_GRAYSCALE)
        surface = build_surface(albedo, normal)
        sky = build_sky_material(albedo, normal)
        sky_instance(sky, "MI_LunarSky")
        gibbous = sky_instance(sky, "MI_LunarSky_Gibbous", Phase=7.0, LightLatitude=14.0,
                               ReliefStrength=1.45, Luminance=0.95)
        build_level(build_mesh(source, surface), gibbous)
    unreal.log("LUNAR_ASSET_BUILD_COMPLETE")


if __name__ == "__main__":
    main()
