package dev.minecraftue.bridge;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.EnumMap;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Properties;

import static dev.minecraftue.bridge.BridgeGraphicsSettings.Setting;

/** Built-in hardware tuning presets and user profile export/import helpers. */
final class VoxGlintProfiles {
    static final String PROFILES_DIR = "voxglint-profiles";
    private static final DateTimeFormatter STAMP = DateTimeFormatter.ofPattern("yyyyMMdd-HHmmss");

    record BuiltinProfile(String id, String zhName, String enName, String zhDesc, String enDesc,
                          BridgeQuality quality, Map<Setting, Integer> overrides) {}

    static final List<BuiltinProfile> BUILTINS = List.of(
            new BuiltinProfile(
                    "rtx3060_high",
                    "RTX 3060 均衡高画质",
                    "RTX 3060 Tuned High",
                    "基于实测优化：高画质档 + 70% TSR 缩放 + 1 次反射反弹 + 2×2 光追区块（重启生效），兼顾光追画质与流畅帧率。",
                    "Benchmarked preset: High tier + 70% TSR + 1 reflection bounce + 2x2 RT regions (restart required) for smooth ray tracing.",
                    BridgeQuality.High,
                    Map.of(
                            Setting.RENDER_SCALE, 70,
                            Setting.REFLECTION_BOUNCES, 1,
                            Setting.REGION_EDGE, 2
                    )),
            new BuiltinProfile(
                    "smooth_rt",
                    "主流显卡流畅光追",
                    "Smooth Hardware RT",
                    "均衡光追档 + 开启动态分辨率（60 FPS 目标）+ 轻度锐化，适合中端光追显卡稳定 60 帧游玩。",
                    "Balanced hardware RT with 60 FPS dynamic resolution and subtle sharpening for mid-range ray-tracing GPUs.",
                    BridgeQuality.Balanced,
                    Map.of(
                            Setting.DYNAMIC_RES, 1,
                            Setting.DYNAMIC_RES_TARGET_FPS, 60,
                            Setting.SHARPEN, 35,
                            Setting.REFLECTION_BOUNCES, 1
                    )),
            new BuiltinProfile(
                    "compat_high_fps",
                    "无光追兼容高帧",
                    "Compat High-FPS (No RT)",
                    "关闭硬件光追，采用屏幕空间反射 + 精简水体光学与体积云，适合核显或无光追显卡追求高帧流畅体验。",
                    "Software lighting path with SSR and streamlined atmosphere/water effects for non-RT GPUs or maximum FPS.",
                    BridgeQuality.Performance,
                    Map.of(
                            Setting.HARDWARE_LIGHTING, 0,
                            Setting.RENDER_SCALE, 75,
                            Setting.CLOUD_SCALE, 50,
                            Setting.SHARPEN, 25
                    )),
            new BuiltinProfile(
                    "cinematic_photo",
                    "影视大片与摄影档",
                    "Cinematic Showcase",
                    "极高档位 + 90% 原生级清晰度 + 完整三层卷云与全套水体交互 + 连续日影 + 4K 512 采样路径追踪拍照。",
                    "Ultra tier + 90% render scale + 3-layer clouds + full P3 water + continuous sun shadows + 4K 512-spp photo capture.",
                    BridgeQuality.Ultra,
                    Map.of(
                            Setting.RENDER_SCALE, 90,
                            Setting.CLOUD_LAYERS, 3,
                            Setting.CLOUD_SHADOWS, 1,
                            Setting.SUN_SHADOW_SMOOTH, 1,
                            Setting.WATER_P3, 1,
                            Setting.WATER_WAVES, 1,
                            Setting.PHOTO_LONG_EDGE, 3840,
                            Setting.PHOTO_SAMPLES, 512
                    ))
    );

    private VoxGlintProfiles() {}

    static Path directory() {
        return BridgeSettings.instanceFile(PROFILES_DIR);
    }

    static BridgeQuality applyBuiltin(BuiltinProfile profile, BridgeGraphicsSettings graphics) throws IOException {
        profile.quality().save();
        // Reset quality & expert domains, preserve user's aesthetic preferences unless explicitly specified by profile.
        var next = new EnumMap<Setting, Integer>(Setting.class);
        for (Setting setting : Setting.values()) {
            if (setting.domain == BridgeGraphicsSettings.Domain.QUALITY
                    || setting.domain == BridgeGraphicsSettings.Domain.EXPERT) {
                next.put(setting, BridgeGraphicsSettings.FOLLOW);
            }
        }
        next.putAll(profile.overrides());
        graphics.changeAll(next);
        return profile.quality();
    }

    static Path exportProfile(BridgeQuality quality, BridgeGraphicsSettings graphics, BridgeSettings bridge)
            throws IOException {
        Path dir = directory();
        Files.createDirectories(dir);
        String name = "voxglint-" + LocalDateTime.now().format(STAMP) + ".properties";
        Path out = dir.resolve(name);
        var content = new StringBuilder();
        content.append("# VoxGlint Profile\n");
        content.append("quality=").append(quality.name()).append('\n');
        content.append("hudMaxFps=").append(bridge.hudMaxFps()).append('\n');
        content.append("debugPanel=").append(bridge.debugPanel().name().toLowerCase(java.util.Locale.ROOT)).append('\n');
        content.append("gradientNormals=").append(bridge.gradientNormals().name().toLowerCase(java.util.Locale.ROOT)).append('\n');
        content.append("reliefStrengthPercent=").append(bridge.relief().strengthPercent()).append('\n');
        content.append("reliefThresholdTenthsPercent=").append(bridge.relief().thresholdTenthsPercent()).append('\n');
        content.append("reliefMaxTiltDegrees=").append(bridge.relief().maxTiltDegrees()).append('\n');
        content.append("connectedGlass=").append(bridge.connectedGlass()).append('\n');
        for (var entry : graphics.snapshot().entrySet()) {
            if (entry.getKey().domain != BridgeGraphicsSettings.Domain.SYSTEM) {
                content.append("gfx.").append(entry.getKey().key).append('=').append(entry.getValue()).append('\n');
            }
        }
        BridgeSettings.write(out, content.toString());
        return out;
    }

    record ImportResult(BridgeQuality quality, BridgeSettings bridgeSettings) {}

    static ImportResult importLatestProfile(BridgeGraphicsSettings graphics) throws IOException {
        Path dir = directory();
        if (!Files.isDirectory(dir)) {
            return null;
        }
        Path latest;
        try (var stream = Files.list(dir)) {
            latest = stream
                    .filter(p -> Files.isRegularFile(p) && p.getFileName().toString().endsWith(".properties"))
                    .max((a, b) -> {
                        try {
                            return Files.getLastModifiedTime(a).compareTo(Files.getLastModifiedTime(b));
                        } catch (IOException e) {
                            return a.getFileName().compareTo(b.getFileName());
                        }
                    })
                    .orElse(null);
        }
        if (latest == null) {
            return null;
        }
        return importProfile(latest, graphics);
    }

    static ImportResult importProfile(Path file, BridgeGraphicsSettings graphics) throws IOException {
        Properties props = new Properties();
        try (var reader = Files.newBufferedReader(file)) {
            props.load(reader);
        }
        BridgeQuality quality = BridgeQuality.High;
        String q = props.getProperty("quality", "").strip();
        for (BridgeQuality candidate : BridgeQuality.values()) {
            if (candidate.name().equalsIgnoreCase(q)) {
                quality = candidate;
                break;
            }
        }
        quality.save();

        BridgeSettings current = BridgeSettings.read();
        int hudFps = current.hudMaxFps();
        try {
            hudFps = BridgeSettings.hudLimit(Integer.parseInt(props.getProperty("hudMaxFps", Integer.toString(hudFps)).strip()));
        } catch (NumberFormatException ignored) {}
        var relief = new TerrainReliefSettings(
                parseBounded(props, "reliefStrengthPercent", 0, TerrainReliefSettings.MAX_STRENGTH_PERCENT, current.relief().strengthPercent()),
                parseBounded(props, "reliefThresholdTenthsPercent", 0, TerrainReliefSettings.MAX_THRESHOLD_TENTHS_PERCENT, current.relief().thresholdTenthsPercent()),
                parseBounded(props, "reliefMaxTiltDegrees", TerrainReliefSettings.MIN_TILT_DEGREES, TerrainReliefSettings.MAX_TILT_DEGREES, current.relief().maxTiltDegrees()));
        boolean glass = !"false".equalsIgnoreCase(props.getProperty("connectedGlass", Boolean.toString(current.connectedGlass())).strip());
        BridgeSettings nextBridge = new BridgeSettings(
                hudFps,
                BridgeSettings.Toggle.parse(props.getProperty("debugPanel", current.debugPanel().name())),
                BridgeSettings.Toggle.parse(props.getProperty("gradientNormals", current.gradientNormals().name())),
                relief,
                glass);
        nextBridge.save();

        var updates = new EnumMap<Setting, Integer>(Setting.class);
        for (Setting setting : Setting.values()) {
            if (setting.domain != BridgeGraphicsSettings.Domain.SYSTEM) {
                updates.put(setting, BridgeGraphicsSettings.FOLLOW);
            }
        }
        for (Setting setting : Setting.values()) {
            if (setting.domain == BridgeGraphicsSettings.Domain.SYSTEM) {
                continue;
            }
            String raw = props.getProperty("gfx." + setting.key);
            if (raw == null) {
                continue;
            }
            try {
                int parsed = Integer.parseInt(raw.strip());
                if (setting.accepts(parsed)) {
                    updates.put(setting, parsed);
                }
            } catch (NumberFormatException ignored) {}
        }
        graphics.changeAll(updates);
        return new ImportResult(quality, nextBridge);
    }

    private static int parseBounded(Properties props, String key, int min, int max, int fallback) {
        try {
            int val = Integer.parseInt(props.getProperty(key, "").strip());
            if (val >= min && val <= max) {
                return val;
            }
        } catch (NumberFormatException ignored) {}
        return fallback;
    }

    static EnumSet<BridgeGraphicsSettings.Domain> qualityDomains() {
        return EnumSet.of(BridgeGraphicsSettings.Domain.QUALITY, BridgeGraphicsSettings.Domain.EXPERT);
    }
}
