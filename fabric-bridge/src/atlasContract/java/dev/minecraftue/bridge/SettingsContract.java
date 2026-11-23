package dev.minecraftue.bridge;

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import static dev.minecraftue.bridge.BridgeGraphicsSettings.FOLLOW;
import static dev.minecraftue.bridge.BridgeGraphicsSettings.Setting;

/** Verifies VoxGlint settings serialization, domain separation, Sodium detection, and C++ CVar parity. */
public final class SettingsContract {
    private static final Pattern CPP_SPEC = Pattern.compile(
            "\\{TEXT\\(\"([^\"]+)\"\\),\\s*[A-Za-z0-9_]+,\\s*(-?\\d+),\\s*(-?\\d+)\\}");

    public static void main(String[] args) throws Exception {
        Path tempDir = args.length > 0 ? Path.of(args[0]) : Files.createTempDirectory("voxglint-settings-test-");
        Files.createDirectories(tempDir);

        verifyDomainSeparationAndPersistence(tempDir);
        verifyValidationGuards(tempDir);
        verifyClarityAndColorMath();
        verifyBridgeSettingsBackwardCompat(tempDir);
        verifySodiumClassMatching();
        verifyCppParity();

        System.out.println("SettingsContract OK (" + Setting.values().length + " graphics settings verified)");
    }

    private static void verifyDomainSeparationAndPersistence(Path tempDir) throws Exception {
        Path file = tempDir.resolve("graphics-test.txt");
        Files.deleteIfExists(file);

        BridgeGraphicsSettings settings = BridgeGraphicsSettings.read(file);
        assertEquals(0, settings.getOrDefault(Setting.GLASS_LIGHTING, 0), "glassLighting defaults off");
        assertEquals(0, settings.getOrDefault(Setting.PUDDLE_DEPRESSIONS, 0), "puddleDepressions defaults off for old settings");
        settings.changeAll(Map.of(
                Setting.RENDER_SCALE, 70,
                Setting.SHADOW_QUALITY, 2,
                Setting.GLASS_LIGHTING, 1,
                Setting.REGION_EDGE, 2,
                Setting.EXPOSURE_BIAS, 24,
                Setting.CAVE_BRIGHTNESS, 150,
                Setting.WATER_ABSORPTION, 50,
                Setting.BLOOM_INTENSITY, 60,
                Setting.SATURATION, 118,
                Setting.PHOTO_LONG_EDGE, 3840
        ));
        settings.change(Setting.PUDDLE_DEPRESSIONS, 1);
        settings.triggerPhotoCapture();

        BridgeGraphicsSettings reloaded = BridgeGraphicsSettings.read(file);
        assertEquals(70, reloaded.get(Setting.RENDER_SCALE), "renderScale roundtrip");
        assertEquals(1, reloaded.get(Setting.GLASS_LIGHTING), "glassLighting roundtrip");
        assertEquals(2, reloaded.get(Setting.REGION_EDGE), "regionEdge roundtrip");
        assertEquals(24, reloaded.get(Setting.EXPOSURE_BIAS), "exposureBias roundtrip");
        assertEquals(150, reloaded.get(Setting.CAVE_BRIGHTNESS), "caveBrightness roundtrip");
        assertEquals(1, reloaded.get(Setting.PUDDLE_DEPRESSIONS), "puddleDepressions on roundtrip");
        assertEquals(1, reloaded.get(Setting.PHOTO_TRIGGER), "photoTrigger roundtrip");
        assertTrue(reloaded.hasQualityOrExpertOverrides(), "hasQualityOrExpertOverrides before clear");

        // Switching total quality preset must clear QUALITY & EXPERT overrides while preserving PREFERENCE & SYSTEM!
        reloaded.clearQualityOverrides();
        BridgeGraphicsSettings afterPresetSwitch = BridgeGraphicsSettings.read(file);
        assertEquals(FOLLOW, afterPresetSwitch.get(Setting.RENDER_SCALE), "renderScale cleared");
        assertEquals(FOLLOW, afterPresetSwitch.get(Setting.SHADOW_QUALITY), "shadowQuality cleared");
        assertEquals(FOLLOW, afterPresetSwitch.get(Setting.GLASS_LIGHTING), "glassLighting cleared");
        assertEquals(FOLLOW, afterPresetSwitch.get(Setting.REGION_EDGE), "regionEdge cleared");
        assertEquals(24, afterPresetSwitch.get(Setting.EXPOSURE_BIAS), "exposureBias kept");
        assertEquals(150, afterPresetSwitch.get(Setting.CAVE_BRIGHTNESS), "caveBrightness kept");
        assertEquals(50, afterPresetSwitch.get(Setting.WATER_ABSORPTION), "waterAbsorption kept");
        assertEquals(1, afterPresetSwitch.get(Setting.PUDDLE_DEPRESSIONS), "puddleDepressions kept across presets");
        assertEquals(60, afterPresetSwitch.get(Setting.BLOOM_INTENSITY), "bloomIntensity kept");
        assertEquals(118, afterPresetSwitch.get(Setting.SATURATION), "saturation kept");
        assertEquals(3840, afterPresetSwitch.get(Setting.PHOTO_LONG_EDGE), "photoLongEdge kept");
        assertEquals(1, afterPresetSwitch.get(Setting.PHOTO_TRIGGER), "photoTrigger kept");
        assertTrue(!afterPresetSwitch.hasQualityOrExpertOverrides(), "no quality overrides after clear");

        afterPresetSwitch.change(Setting.PUDDLE_DEPRESSIONS, 0);
        assertEquals(0, BridgeGraphicsSettings.read(file).get(Setting.PUDDLE_DEPRESSIONS), "puddleDepressions off roundtrip");

        // Reset all user settings keeps SYSTEM photo trigger so UE won't refire old triggers.
        afterPresetSwitch.resetAllUserSettings();
        BridgeGraphicsSettings afterResetAll = BridgeGraphicsSettings.read(file);
        assertEquals(FOLLOW, afterResetAll.get(Setting.EXPOSURE_BIAS), "exposureBias reset");
        assertEquals(FOLLOW, afterResetAll.get(Setting.CAVE_BRIGHTNESS), "caveBrightness reset");
        assertEquals(FOLLOW, afterResetAll.get(Setting.PUDDLE_DEPRESSIONS), "puddleDepressions reset to default");
        assertEquals(1, afterResetAll.get(Setting.PHOTO_TRIGGER), "photoTrigger preserved on resetAll");
    }

    private static void verifyValidationGuards(Path tempDir) throws Exception {
        Path file = tempDir.resolve("invalid-test.txt");
        Files.writeString(file, """
                aaMethod=3
                regionEdge=3
                megaLightSamples=3
                fogGrid=4
                renderScale=999
                waterP3=1
                puddleDepressions=2
                caveBrightness=140
                """);
        BridgeGraphicsSettings loaded = BridgeGraphicsSettings.read(file);
        assertEquals(FOLLOW, loaded.get(Setting.AA_METHOD), "reject aaMethod=3");
        assertEquals(FOLLOW, loaded.get(Setting.REGION_EDGE), "reject regionEdge=3");
        assertEquals(FOLLOW, loaded.get(Setting.MEGA_LIGHT_SAMPLES), "reject megaLightSamples=3");
        assertEquals(FOLLOW, loaded.get(Setting.FOG_GRID), "reject fogGrid=4");
        assertEquals(FOLLOW, loaded.get(Setting.RENDER_SCALE), "reject out-of-range renderScale");
        assertEquals(1, loaded.get(Setting.WATER_P3), "accept waterP3=1");
        assertEquals(FOLLOW, loaded.get(Setting.PUDDLE_DEPRESSIONS), "reject puddleDepressions outside 0..1");
        assertEquals(140, loaded.get(Setting.CAVE_BRIGHTNESS), "accept caveBrightness=140");
    }

    private static void verifyClarityAndColorMath() {
        assertEquals(100, BridgeGraphicsSettings.absorptionFromClarity(100), "clarity 100 -> abs 100");
        assertEquals(50, BridgeGraphicsSettings.absorptionFromClarity(200), "clarity 200 -> abs 50");
        assertEquals(25, BridgeGraphicsSettings.absorptionFromClarity(400), "clarity 400 -> abs 25");
        assertEquals(200, BridgeGraphicsSettings.absorptionFromClarity(50), "clarity 50 -> abs 200");
        assertEquals(100, BridgeGraphicsSettings.clarityFromAbsorption(100), "abs 100 -> clarity 100");
        assertEquals(200, BridgeGraphicsSettings.clarityFromAbsorption(50), "abs 50 -> clarity 200");
        assertEquals(400, BridgeGraphicsSettings.clarityFromAbsorption(25), "abs 25 -> clarity 400");
        assertEquals(50, BridgeGraphicsSettings.clarityFromAbsorption(200), "abs 200 -> clarity 50");

        assertEquals(0, VoxGlintOptionCatalog.inferColorStyle(100, 100, 6500), "natural preset");
        assertEquals(1, VoxGlintOptionCatalog.inferColorStyle(106, 106, 7100), "cinematic warm preset");
        assertEquals(2, VoxGlintOptionCatalog.inferColorStyle(118, 104, 6400), "vibrant preset");
        assertEquals(3, VoxGlintOptionCatalog.inferColorStyle(94, 112, 5900), "cool contrast preset");
        assertEquals(-1, VoxGlintOptionCatalog.inferColorStyle(110, 100, 6500), "custom color");
    }

    private static void verifyBridgeSettingsBackwardCompat(Path tempDir) throws Exception {
        Path legacy = tempDir.resolve("legacy-settings.properties");
        Files.writeString(legacy, """
                hudMaxFps=144
                debugPanel=off
                gradientNormals=on
                reliefStrengthPercent=120
                """);
        BridgeSettings readLegacy = BridgeSettings.read(legacy);
        assertEquals(144, readLegacy.hudMaxFps(), "legacy hudMaxFps");
        assertEquals(BridgeSettings.Toggle.OFF, readLegacy.debugPanel(), "legacy debugPanel");
        assertEquals(BridgeSettings.Toggle.ON, readLegacy.gradientNormals(), "legacy gradientNormals");
        assertEquals(120, readLegacy.relief().strengthPercent(), "legacy relief strength");
        assertTrue(readLegacy.connectedGlass(), "legacy defaults connectedGlass to true");

        BridgeSettings updated = new BridgeSettings(readLegacy.hudMaxFps(), readLegacy.debugPanel(),
                readLegacy.gradientNormals(), readLegacy.relief(), false);
        updated.save(legacy);
        assertTrue(!BridgeSettings.read(legacy).connectedGlass(), "connectedGlass false persists");
    }

    private static void verifySodiumClassMatching() {
        assertTrue(SodiumCompat.isSodiumVideoScreenClassName("net.caffeinemc.mods.sodium.client.gui.SodiumOptionsGUI"),
                "match modern SodiumOptionsGUI");
        assertTrue(SodiumCompat.isSodiumVideoScreenClassName("net.caffeinemc.mods.sodium.client.gui.SodiumConfigScreen"),
                "match SodiumConfigScreen");
        assertTrue(SodiumCompat.isSodiumVideoScreenClassName("me.jellysquid.mods.sodium.client.gui.SodiumOptionsGUI"),
                "match legacy SodiumOptionsGUI");
        assertTrue(!SodiumCompat.isSodiumVideoScreenClassName("net.caffeinemc.mods.sodium.client.gui.PromptScreen"),
                "exclude PromptScreen");
        assertTrue(!SodiumCompat.isSodiumVideoScreenClassName("net.minecraft.client.gui.screens.options.VideoSettingsScreen"),
                "exclude vanilla VideoSettingsScreen");
    }

    private static void verifyCppParity() throws Exception {
        Path cpp = Path.of("..", "ue-renderer", "Source", "MinecraftUEViewer", "Private", "BridgeQuality.cpp")
                .toAbsolutePath().normalize();
        if (!Files.isRegularFile(cpp)) {
            return;
        }
        String src = Files.readString(cpp);
        int start = src.indexOf("DetailSpecs[]");
        int end = src.indexOf("};", start);
        assertTrue(start > 0 && end > start, "found DetailSpecs in BridgeQuality.cpp");
        String block = src.substring(start, end);

        record Range(int min, int max) {}
        Map<String, Range> cppSpecs = new HashMap<>();
        Matcher m = CPP_SPEC.matcher(block);
        while (m.find()) {
            cppSpecs.put(m.group(1), new Range(Integer.parseInt(m.group(2)), Integer.parseInt(m.group(3))));
        }
        assertEquals(Setting.values().length, cppSpecs.size(), "Java/C++ setting count parity");
        for (Setting setting : Setting.values()) {
            Range r = cppSpecs.get(setting.key);
            if (r == null) {
                throw new AssertionError("Missing C++ DetailSpec for Java key: " + setting.key);
            }
            assertEquals(r.min(), setting.min, "min mismatch for " + setting.key);
            assertEquals(r.max(), setting.max, "max mismatch for " + setting.key);
        }
    }

    private static void assertTrue(boolean cond, String msg) {
        if (!cond) throw new AssertionError("Assertion failed: " + msg);
    }

    private static void assertEquals(Object expected, Object actual, String msg) {
        if (!java.util.Objects.equals(expected, actual)) {
            throw new AssertionError(msg + ": expected=" + expected + ", actual=" + actual);
        }
    }
}
