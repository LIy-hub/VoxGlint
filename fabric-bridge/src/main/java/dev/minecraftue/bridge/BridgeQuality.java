package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import org.slf4j.Logger;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;

enum BridgeQuality {
    Startup, Baseline, Performance, Balanced, High, Ultra;

    private static final Logger LOGGER = LogUtils.getLogger();

    static BridgeQuality read() {
        Path path = BridgeSettings.instanceFile(BridgeSettings.QUALITY_FILE);
        try {
            if (Files.exists(path) && Files.size(path) <= 128) {
                String value = Files.readString(path).replace("\uFEFF", "").strip();
                if (value.length() == 1 && value.charAt(0) >= '0' && value.charAt(0) <= '3') {
                    return values()[value.charAt(0) - '0' + Performance.ordinal()];
                }
                for (BridgeQuality quality : values()) {
                    if (quality.name().equalsIgnoreCase(value)) {
                        return quality;
                    }
                }
            }
        } catch (IOException e) {
            LOGGER.warn("Could not read Minecraft UE quality setting", e);
        }
        return Startup;
    }

    void save() throws IOException {
        Path path = BridgeSettings.instanceFile(BridgeSettings.QUALITY_FILE);
        if (this == Startup) {
            Files.deleteIfExists(path);
        } else {
            BridgeSettings.write(path, name() + "\n");
        }
    }
}
