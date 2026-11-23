package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import org.slf4j.Logger;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.EnumMap;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

final class BridgeGraphicsSettings {
    private static final Logger LOGGER = LogUtils.getLogger();
    static final String FILE_NAME = "minecraftue-graphics.txt";
    private static final String PHOTO_DIR_FILE = "minecraftue-photo-dir.txt";
    private static final Map<String, String> NO_RENDERER = Map.of();
    static final int FOLLOW = -1;

    enum Domain {
        QUALITY,
        PREFERENCE,
        EXPERT,
        SYSTEM
    }

    enum Setting {
        // --- Display & Upscaling ---
        UPSCALER("upscaler", "Upscaler", 1, 3, Domain.QUALITY),
        UPSCALER_QUALITY("upscalerQuality", "UpscalerQuality", 1, 2, Domain.QUALITY),
        FRAME_GEN_BACKEND("frameGenBackend", "FrameGenBackend", 1, 3, Domain.PREFERENCE),
        FRAME_GEN_MULTIPLIER("frameGenMultiplier", "FrameGenMultiplier", 0, 8, Domain.PREFERENCE),
        RENDER_SCALE("renderScale", "r.ScreenPercentage", 50, 100, Domain.QUALITY),
        HISTORY_SCALE("historyScale", "r.TSR.History.ScreenPercentage", 100, 200, Domain.EXPERT),
        AA_METHOD("aaMethod", "r.AntiAliasingMethod", 0, 4, Domain.QUALITY),
        DYNAMIC_RES("dynamicRes", "r.DynamicRes.OperationMode", 0, 1, Domain.QUALITY),
        DYNAMIC_RES_TARGET_FPS("dynamicResTargetFps", "r.DynamicRes.FrameTimeBudget", 30, 240, Domain.QUALITY),
        SHARPEN("sharpen", "r.Tonemapper.Sharpen", 0, 200, Domain.PREFERENCE),

        // --- Lighting & Shadows ---
        SHADOW_QUALITY("shadowQuality", "sg.ShadowQuality", 1, 3, Domain.QUALITY),
        GI_QUALITY("giQuality", "sg.GlobalIlluminationQuality", 1, 3, Domain.QUALITY),
        REFLECTION_QUALITY("reflectionQuality", "sg.ReflectionQuality", 1, 3, Domain.QUALITY),
        SHADOW_RAYS("shadowRays", "r.Shadow.Virtual.SMRT.RayCountDirectional", 1, 8, Domain.EXPERT),
        CONTACT_SHADOWS("contactShadows", "mcue.Shadows.ContactLength", 0, 1, Domain.QUALITY),
        SUN_SHADOW_SMOOTH("sunShadowSmooth", "SunShadowSmooth", 0, 1, Domain.QUALITY),
        HARDWARE_LIGHTING("hardwareLighting", "HWRT", 0, 1, Domain.QUALITY),
        HARDWARE_GI("hardwareGi", "HardGi", 0, 1, Domain.QUALITY),
        HARDWARE_REFLECTIONS("hardwareReflections", "HardReflections", 0, 1, Domain.QUALITY),
        TRANSLUCENCY_REFLECTIONS("translucencyReflections", "TranslucencyReflections", 0, 1, Domain.QUALITY),
        REFLECTION_BOUNCES("reflectionBounces", "r.Lumen.Reflections.MaxBounces", 1, 8, Domain.EXPERT),
        REFLECTION_ROUGHNESS("reflectionRoughness", "r.Lumen.Reflections.MaxRoughnessToTrace", 0, 100, Domain.EXPERT),
        REFLECTION_SCREEN_TRACES("reflectionScreenTraces", "r.Lumen.Reflections.ScreenTraces", 0, 1, Domain.EXPERT),
        DIRECTIONAL_SHADOWS("directionalShadows", "DirectionalRayShadows", 0, 1, Domain.QUALITY),
        MEGA_LIGHTS("megaLights", "MegaLights", 0, 1, Domain.QUALITY),
        MEGA_LIGHT_SAMPLES("megaLightSamples", "r.MegaLights.NumSamplesPerPixel", 1, 4, Domain.EXPERT),
        MEGA_LIGHT_BUDGET("megaLightBudget", "mcue.PointLights.Max", 0, 2048, Domain.EXPERT),
        FALLBACK_LIGHT_BUDGET("fallbackLightBudget", "mcue.PointLights.FallbackMax", 0, 128, Domain.EXPERT),
        BLOCK_LIGHT_INTENSITY("blockLightIntensity", "BlockLightIntensity", 25, 250, Domain.PREFERENCE),
        GLASS_LIGHTING("glassLighting", "mcue.Glass.Lighting", 0, 1, Domain.QUALITY),

        // --- Sky, Atmosphere & Weather ---
        CLOUD_SCALE("cloudScale", "CloudSamples", 0, 100, Domain.QUALITY),
        CLOUD_COVERAGE("cloudCoverage", "mcue.Clouds.Coverage", 0, 100, Domain.PREFERENCE),
        CLOUD_LAYERS("cloudLayers", "CloudLayers", 1, 3, Domain.QUALITY),
        CLOUD_SHADOWS("cloudShadows", "CloudShadows", 0, 1, Domain.QUALITY),
        FOG_GRID("fogGrid", "r.VolumetricFog.GridPixelSize", 0, 32, Domain.QUALITY),
        FOG_DENSITY("fogDensity", "FogDensity", 0, 200, Domain.PREFERENCE),
        RAIN_WETNESS("rainWetness", "RainWetness", 0, 2, Domain.PREFERENCE),
        PUDDLE_DEPRESSIONS("puddleDepressions", "PuddleDepressions", 0, 1, Domain.PREFERENCE),
        PRECIP_DENSITY("precipDensity", "PrecipDensity", 0, 150, Domain.PREFERENCE),

        // --- Water (P1 / P2 / P3) ---
        WATER_P1("waterP1", "mc.Water.P1", 0, 1, Domain.QUALITY),
        WATER_P2("waterP2", "mc.Water.P2", 0, 1, Domain.QUALITY),
        WATER_P3("waterP3", "WaterP3", 0, 1, Domain.QUALITY),
        WATER_WAVES("waterWaves", "WaterWaves", 0, 1, Domain.QUALITY),
        WATER_VOLUME("waterVolume", "mc.Water.P2.Volume", 0, 1, Domain.QUALITY),
        WATER_CAUSTICS("waterCaustics", "mc.Water.P2.Caustics", 0, 1, Domain.QUALITY),
        WATER_PARTICLES("waterParticles", "mc.Water.P2.Plankton", 0, 1, Domain.QUALITY),
        WATER_LENS_FX("waterLensFx", "mc.Water.P2.Film", 0, 1, Domain.PREFERENCE),
        WATER_ABSORPTION("waterAbsorption", "WaterAbsorption", 25, 200, Domain.PREFERENCE),
        WATER_SCATTERING("waterScattering", "WaterScattering", 0, 200, Domain.PREFERENCE),
        WATER_LABPBR_NORMALS("waterLabPbrNormals", "mcue.Water.LabPbrNormalWeight", 0, 100, Domain.PREFERENCE),

        // --- Exposure, Color & Post-Processing ---
        EXPOSURE_MODE("exposureMode", "ExposureMode", 0, 2, Domain.PREFERENCE),
        EXPOSURE_BIAS("exposureBias", "ExposureBias", 0, 40, Domain.PREFERENCE),
        LOCAL_EXPOSURE("localExposure", "LocalExposure", 0, 100, Domain.PREFERENCE),
        CAVE_BRIGHTNESS("caveBrightness", "CaveBrightness", 0, 200, Domain.PREFERENCE),
        BLOOM_INTENSITY("bloomIntensity", "BloomIntensity", 0, 200, Domain.PREFERENCE),
        SATURATION("saturation", "Saturation", 50, 150, Domain.PREFERENCE),
        CONTRAST("contrast", "Contrast", 70, 130, Domain.PREFERENCE),
        COLOR_TEMP("colorTemp", "ColorTemp", 4500, 8500, Domain.PREFERENCE),
        VIGNETTE_INTENSITY("vignetteIntensity", "VignetteIntensity", 0, 100, Domain.PREFERENCE),
        MOTION_BLUR("motionBlur", "MotionBlur", 0, 100, Domain.PREFERENCE),

        // --- Path-Traced Photo Capture ---
        PHOTO_LONG_EDGE("photoLongEdge", "mcue.Photo.LongEdge", 720, 4096, Domain.PREFERENCE),
        PHOTO_SAMPLES("photoSamples", "mcue.Photo.Samples", 32, 1024, Domain.PREFERENCE),
        PHOTO_SAVE_EXR("photoSaveExr", "mcue.Photo.SaveExr", 0, 1, Domain.PREFERENCE),
        PHOTO_DENOISER("photoDenoiser", "mcue.Photo.Denoiser", 0, 1, Domain.PREFERENCE),

        // --- Expert Ray-Tracing Geometry ---
        REGION_EDGE("regionEdge", "RegionChunks", 1, 4, Domain.EXPERT),
        TERRAIN_RANGE("terrainRange", "mcue.RayTracing.TerrainRange", 1, 32, Domain.EXPERT),
        DYNAMIC_RANGE("dynamicRange", "DynamicRange", 0, 36, Domain.EXPERT),
        DYNAMIC_BUDGET("dynamicBudget", "DynamicBudget", 0, 256, Domain.EXPERT),

        // --- Live System Controls ---
        DEBUG_OVERLAY("debugOverlay", "DebugOverlay", 0, 1, Domain.SYSTEM),
        PHOTO_TRIGGER("photoTrigger", "PhotoTrigger", 0, 1_000_000, Domain.SYSTEM);

        final String key;
        final String effectiveKey;
        final int min;
        final int max;
        final Domain domain;

        Setting(String key, String effectiveKey, int min, int max, Domain domain) {
            this.key = key;
            this.effectiveKey = effectiveKey;
            this.min = min;
            this.max = max;
            this.domain = domain;
        }

        boolean accepts(int value) {
            return value >= min && value <= max
                    && (this != MEGA_LIGHT_SAMPLES || value != 3)
                    && (this != FRAME_GEN_MULTIPLIER || value != 1)
                    && (this != REGION_EDGE || value != 3)
                    && (this != AA_METHOD || value != 3)
                    && (this != FOG_GRID || value == 0 || value >= 8);
        }
    }

    private final EnumMap<Setting, Integer> values = new EnumMap<>(Setting.class);
    private final Path targetPath;

    BridgeGraphicsSettings() {
        this(null);
    }

    BridgeGraphicsSettings(Path targetPath) {
        this.targetPath = targetPath;
    }

    static BridgeGraphicsSettings read() {
        return read(BridgeSettings.instanceFile(FILE_NAME));
    }

    static BridgeGraphicsSettings read(Path path) {
        BridgeGraphicsSettings result = new BridgeGraphicsSettings(path);
        try {
            if (Files.isRegularFile(path) && Files.size(path) <= 8192) {
                var entries = new HashMap<String, String>();
                Files.readAllLines(path).forEach(line -> addEntry(entries, line));
                for (Setting setting : Setting.values()) {
                    String raw = entries.get(setting.key);
                    if (raw == null) {
                        continue;
                    }
                    try {
                        int value = Integer.parseInt(raw);
                        if (setting.accepts(value) && raw.equals(Integer.toString(value))) {
                            result.values.put(setting, value);
                        }
                    } catch (NumberFormatException ignored) {}
                }
            }
        } catch (IOException e) {
            LOGGER.warn("Could not read Minecraft UE detailed graphics settings", e);
        }
        return result;
    }

    int get(Setting setting) {
        return values.getOrDefault(setting, FOLLOW);
    }

    int getOrDefault(Setting setting, int fallback) {
        return values.getOrDefault(setting, fallback);
    }

    boolean isOverridden(Setting setting) {
        return values.containsKey(setting);
    }

    Map<Setting, Integer> snapshot() {
        return Map.copyOf(values);
    }

    boolean hasQualityOrExpertOverrides() {
        return values.keySet().stream().anyMatch(s -> s.domain == Domain.QUALITY || s.domain == Domain.EXPERT);
    }

    boolean hasDomainOverrides(Domain domain) {
        return values.keySet().stream().anyMatch(s -> s.domain == domain);
    }

    boolean hasAnyUserOverrides() {
        return values.keySet().stream().anyMatch(s -> s.domain != Domain.SYSTEM);
    }

    void change(Setting setting, int value) throws IOException {
        changeAll(Map.of(setting, value));
    }

    void changeAll(Map<Setting, Integer> updates) throws IOException {
        var changed = new EnumMap<>(values);
        for (var entry : updates.entrySet()) {
            Setting setting = entry.getKey();
            int value = entry.getValue();
            if (value == FOLLOW) {
                changed.remove(setting);
            } else if (setting.accepts(value)) {
                changed.put(setting, value);
            }
        }
        store(changed);
        values.clear();
        values.putAll(changed);
    }

    void clearDomains(Set<Domain> domains) throws IOException {
        var changed = new EnumMap<>(values);
        changed.keySet().removeIf(setting -> domains.contains(setting.domain));
        store(changed);
        values.clear();
        values.putAll(changed);
    }

    void clearQualityOverrides() throws IOException {
        clearDomains(EnumSet.of(Domain.QUALITY, Domain.EXPERT));
    }

    void clearSettings(Set<Setting> settings) throws IOException {
        var changed = new EnumMap<>(values);
        changed.keySet().removeAll(settings);
        store(changed);
        values.clear();
        values.putAll(changed);
    }

    void resetAllUserSettings() throws IOException {
        clearDomains(EnumSet.of(Domain.QUALITY, Domain.PREFERENCE, Domain.EXPERT));
    }

    void triggerPhotoCapture() throws IOException {
        int next = Math.min(Setting.PHOTO_TRIGGER.max, Math.max(0, getOrDefault(Setting.PHOTO_TRIGGER, 0)) + 1);
        change(Setting.PHOTO_TRIGGER, next);
    }

    static int absorptionFromClarity(int clarityPercent) {
        int clamped = Math.clamp(clarityPercent, 50, 400);
        return Math.clamp(Math.round(10000.0f / clamped), Setting.WATER_ABSORPTION.min, Setting.WATER_ABSORPTION.max);
    }

    static int clarityFromAbsorption(int absorptionPercent) {
        int clamped = Math.clamp(absorptionPercent, Setting.WATER_ABSORPTION.min, Setting.WATER_ABSORPTION.max);
        return Math.clamp(Math.round(10000.0f / clamped), 50, 400);
    }

    static Path photoDirectory() {
        Path hint = BridgeSettings.instanceFile(PHOTO_DIR_FILE);
        try {
            if (Files.isRegularFile(hint) && Files.size(hint) <= 1024) {
                String line = Files.readString(hint).strip();
                if (!line.isEmpty()) {
                    return Path.of(line);
                }
            }
        } catch (IOException | IllegalArgumentException ignored) {}
        String configured = System.getProperty("minecraftue.renderer.directory", "minecraftue-renderer");
        return BridgeSettings.instanceFile(configured)
                .resolve("MinecraftUEViewer/Saved/Screenshots/Windows/PathTracing")
                .toAbsolutePath().normalize();
    }

    private void store(Map<Setting, Integer> next) throws IOException {
        Path path = targetPath != null ? targetPath : BridgeSettings.instanceFile(FILE_NAME);
        if (next.isEmpty()) {
            Files.deleteIfExists(path);
        } else {
            var content = new StringBuilder();
            next.forEach((setting, value) -> content.append(setting.key).append('=').append(value).append('\n'));
            BridgeSettings.write(path, content.toString());
        }
    }

    static Map<String, String> effective() {
        Path path = BridgeSettings.instanceFile(BridgeSettings.QUALITY_FILE + ".effective");
        try {
            if (!Files.isRegularFile(path) || Files.size(path) > 16384) {
                return NO_RENDERER;
            }
            var values = new HashMap<String, String>();
            for (String entry : Files.readString(path).trim().split("\\s+")) {
                addEntry(values, entry);
            }
            if ("2".equals(values.get("DetailsVersion"))) {
                long pid = Long.parseLong(values.getOrDefault("RendererPid", "0"));
                if (pid > 0 && ProcessHandle.of(pid).filter(ProcessHandle::isAlive).isPresent()) {
                    return Map.copyOf(values);
                }
            }
        } catch (IOException | IllegalArgumentException ignored) {
            // The renderer updates this status file independently of the game.
        }
        return NO_RENDERER;
    }

    private static void addEntry(Map<String, String> entries, String input) {
        int separator = input.indexOf('=');
        if (separator > 0) {
            entries.put(input.substring(0, separator).strip(), input.substring(separator + 1).strip());
        }
    }
}
