package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import net.fabricmc.loader.api.FabricLoader;
import org.slf4j.Logger;

import java.io.IOException;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;
import java.util.Properties;

record BridgeSettings(int hudMaxFps, Toggle debugPanel, Toggle gradientNormals, TerrainReliefSettings relief,
                      boolean connectedGlass) {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final String SETTINGS_FILE = "minecraftue-settings.properties";
    static final String QUALITY_FILE = "minecraftue-quality.txt";
    private static volatile boolean cachedConnectedGlass = true;

    BridgeSettings(int hudMaxFps, Toggle debugPanel, Toggle gradientNormals, TerrainReliefSettings relief) {
        this(hudMaxFps, debugPanel, gradientNormals, relief, true);
    }

    enum Toggle {
        STARTUP, ON, OFF;

        static Toggle parse(String value) {
            return switch (value.strip().toLowerCase(Locale.ROOT)) {
                case "on" -> ON;
                case "off" -> OFF;
                default -> STARTUP;
            };
        }
    }

    static boolean connectedGlassEnabled() {
        return cachedConnectedGlass;
    }

    static BridgeSettings read() {
        BridgeSettings loaded = read(instanceFile(SETTINGS_FILE));
        cachedConnectedGlass = loaded.connectedGlass();
        return loaded;
    }

    static BridgeSettings read(Path path) {
        Properties values = new Properties();
        try {
            if (Files.exists(path)) {
                try (var reader = Files.newBufferedReader(path)) {
                    values.load(reader);
                }
            }
        } catch (IOException | IllegalArgumentException e) {
            LOGGER.warn("Could not read Minecraft UE settings", e);
            values.clear();
        }
        int fps = 0;
        try {
            fps = hudLimit(Integer.parseInt(values.getProperty("hudMaxFps", "0").strip()));
        } catch (NumberFormatException ignored) {}
        var defaults = TerrainReliefSettings.DEFAULT;
        var relief = new TerrainReliefSettings(
                readBounded(values, "reliefStrengthPercent", 0, TerrainReliefSettings.MAX_STRENGTH_PERCENT,
                        defaults.strengthPercent()),
                readBounded(values, "reliefThresholdTenthsPercent", 0, TerrainReliefSettings.MAX_THRESHOLD_TENTHS_PERCENT,
                        defaults.thresholdTenthsPercent()),
                readBounded(values, "reliefMaxTiltDegrees", TerrainReliefSettings.MIN_TILT_DEGREES,
                        TerrainReliefSettings.MAX_TILT_DEGREES, defaults.maxTiltDegrees()));
        boolean glass = !"false".equalsIgnoreCase(values.getProperty("connectedGlass", "true").strip());
        return new BridgeSettings(fps, Toggle.parse(values.getProperty("debugPanel", "")),
                Toggle.parse(values.getProperty("gradientNormals", "")), relief, glass);
    }

    private static int readBounded(Properties values, String key, int min, int max, int fallback) {
        try {
            int value = Integer.parseInt(values.getProperty(key, "").strip());
            if (value >= min && value <= max) {
                return value;
            }
        } catch (NumberFormatException ignored) {}
        return fallback;
    }

    TerrainReliefSettings effectiveRelief() {
        boolean enabled = switch (gradientNormals) {
            case STARTUP -> Boolean.getBoolean("minecraftue.gradientNormals");
            case ON -> true;
            case OFF -> false;
        };
        return enabled && relief.strengthPercent() > 0 ? relief : TerrainReliefSettings.DISABLED;
    }

    static int hudLimit(int value) {
        return value >= 10 && value <= 250 ? value : 0;
    }

    void save() throws IOException {
        save(instanceFile(SETTINGS_FILE));
        cachedConnectedGlass = connectedGlass;
    }

    void save(Path path) throws IOException {
        write(path, "hudMaxFps=" + hudMaxFps + "\n"
                + "debugPanel=" + debugPanel.name().toLowerCase(Locale.ROOT) + "\n"
                + "gradientNormals=" + gradientNormals.name().toLowerCase(Locale.ROOT) + "\n"
                + "reliefStrengthPercent=" + relief.strengthPercent() + "\n"
                + "reliefThresholdTenthsPercent=" + relief.thresholdTenthsPercent() + "\n"
                + "reliefMaxTiltDegrees=" + relief.maxTiltDegrees() + "\n"
                + "connectedGlass=" + connectedGlass + "\n");
    }

    static Path instanceFile(String name) {
        return FabricLoader.getInstance().getGameDir().resolve(name);
    }

    static void write(Path path, String content) throws IOException {
        Path pending = Files.createTempFile(path.getParent(), "minecraftue-settings-", ".tmp");
        try {
            Files.writeString(pending, content);
            try {
                Files.move(pending, path, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(pending, path, StandardCopyOption.REPLACE_EXISTING);
            }
        } finally {
            Files.deleteIfExists(pending);
        }
    }
}
