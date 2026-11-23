package dev.minecraftue.bridge;

import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.ResourceManager;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.BitSet;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

final class TerrainEmission {
    private static final double[] SRGB_TO_LINEAR = new double[256];

    static {
        for (int i = 0; i < SRGB_TO_LINEAR.length; i++) {
            double value = i / 255.0;
            SRGB_TO_LINEAR[i] = value <= 0.04045 ? value / 12.92 : Math.pow((value + 0.055) / 1.055, 2.4);
        }
    }

    private TerrainEmission() {}

    static Pattern builtinPattern(Identifier name) {
        if (!name.getNamespace().equals("minecraft")) {
            return null;
        }
        String path = name.getPath();
        if (path.startsWith("block/") && path.endsWith("_copper_lantern")) return Pattern.LANTERN;
        return switch (path) {
            case "block/magma" -> Pattern.MAGMA;
            case "block/crying_obsidian" -> Pattern.CRYING_OBSIDIAN;
            case "block/lantern", "block/soul_lantern", "block/copper_lantern" -> Pattern.LANTERN;
            case "block/torch", "block/soul_torch", "block/copper_torch" -> Pattern.TORCH;
            case "block/redstone_torch" -> Pattern.REDSTONE_TORCH;
            default -> null;
        };
    }

    enum Pattern {
        MAGMA(0.14),
        CRYING_OBSIDIAN(0.06),
        LANTERN(0), TORCH(0), REDSTONE_TORCH(0);

        private final double threshold;

        Pattern(double threshold) {
            this.threshold = threshold;
        }

        BitSet stableMask(TerrainAtlas.Sprite sprite) {
            int pixels = sprite.width() * sprite.height();
            var mask = new BitSet(pixels);
            if (this == LANTERN || this == TORCH || this == REDSTONE_TORCH) {
                // The vanilla models reuse one sprite for the light and its hardware.
                for (int y = 0; y < sprite.height(); y++) {
                    int v = (2 * y + 1) * 8 / sprite.height();
                    for (int x = 0; x < sprite.width(); x++) {
                        int u = (2 * x + 1) * 8 / sprite.width();
                        boolean lit = switch (this) {
                            case LANTERN -> u >= 1 && u < 5 && v >= 3 && v < 8;
                            case REDSTONE_TORCH -> u >= 6 && u < 10 && v >= 5 && v < 9;
                            default -> u >= 7 && u < 9 && v >= 6 && v < 8;
                        };
                        if (lit) mask.set(y * sprite.width() + x);
                    }
                }
                return mask;
            }
            int[] frames = sprite.frames();
            boolean purple = this == CRYING_OBSIDIAN;
            var frameX = new int[frames.length];
            var frameY = new int[frames.length];
            for (int i = 0; i < frames.length; i++) {
                frameX[i] = sprite.frameX(frames[i]);
                frameY[i] = sprite.frameY(frames[i]);
            }
            var albedo = sprite.images()[0];
            int index = 0;
            for (int y = 0; y < sprite.height(); y++) {
                for (int x = 0; x < sprite.width(); x++, index++) {
                    int minA = 255, minR = 255, minG = 255, minB = 255;
                    int maxA = 0, maxR = 0, maxG = 0, maxB = 0;
                    for (int i = 0; i < frames.length; i++) {
                        int pixel = albedo.getPixel(frameX[i] + x, frameY[i] + y);
                        int a = pixel >>> 24;
                        int r = (pixel >>> 16) & 255;
                        int g = (pixel >>> 8) & 255;
                        int b = pixel & 255;
                        minA = Math.min(minA, a);
                        minR = Math.min(minR, r);
                        minG = Math.min(minG, g);
                        minB = Math.min(minB, b);
                        maxA = Math.max(maxA, a);
                        maxR = Math.max(maxR, r);
                        maxG = Math.max(maxG, g);
                        maxB = Math.max(maxB, b);
                    }
                    // Channel bounds also cover frames interpolated in either sRGB or linear space.
                    double minimum = score(purple ? maxR : minR, purple ? maxG : minG, minB);
                    double maximum = score(purple ? minR : maxR, purple ? minG : maxG, maxB);
                    if (minA >= 128 && minimum >= threshold) {
                        mask.set(index);
                    } else if (maxA >= 128 && maximum >= threshold) {
                        return null;
                    }
                }
            }
            // A replacement texture without separable bright and dark regions keeps its default material.
            return mask.isEmpty() || mask.cardinality() == pixels ? null : mask;
        }

        private double score(int r, int g, int b) {
            double red = SRGB_TO_LINEAR[r];
            double green = SRGB_TO_LINEAR[g];
            double blue = SRGB_TO_LINEAR[b];
            return this == MAGMA ? 0.2126 * red + 0.7152 * green + 0.0722 * blue
                    : blue - 2.0 * Math.max(red, green);
        }
    }

    static Map<Identifier, Color> readColors(List<TerrainAtlas.Sprite> sprites, MemorySegment packedAtlas, int atlasWidth,
                                             ResourceManager resources) {
        Map<Identifier, Color> colors = new HashMap<>();
        for (var sprite : sprites) {
            boolean authored = resources.getResource(TerrainMaterialAtlas.sidecarPath(sprite.name(), "_s")).isPresent();
            var pattern = builtinPattern(sprite.name());
            var rule = TextureEmission.rule(sprite.name());
            boolean generic = !authored && rule == null && pattern == null;
            int mode = generic ? TextureEmission.AUTOMATIC : !authored && rule != null
                    ? TextureEmission.mode(sprite, rule) : TextureEmission.PACKED;
            var albedo = sprite.images()[0];
            int left = sprite.x() + sprite.padding();
            int top = sprite.y() + sprite.padding();
            double r = 0.0, g = 0.0, b = 0.0;
            int width = generic ? Math.min(sprite.width(), 32) : sprite.width();
            int height = generic ? Math.min(sprite.height(), 32) : sprite.height();
            var tile = new SurfaceEmission.Tile(width, height,
                    !authored && (pattern == Pattern.TORCH || pattern == Pattern.REDSTONE_TORCH));
            tile.animated = sprite.frames().length > 1;
            tile.sampleMode = mode;
            if (sprite.name().getPath().equals("block/lava_flow")) {
                double atlasHeight = packedAtlas.byteSize() / (4L * atlasWidth);
                tile.bounds = new SurfaceEmission.AtlasRect((double)left/atlasWidth, top/atlasHeight,
                        (double)(left+sprite.width())/atlasWidth,(top+sprite.height())/atlasHeight);
            }
            for (int y = 0; y < height; y++) {
                int sy = (int) ((y + .5) * sprite.height() / height);
                long row = ((long) (top + sy) * atlasWidth + left) * Integer.BYTES;
                for (int x = 0; x < width; x++) {
                    int sx = (int) ((x + .5) * sprite.width() / width);
                    // The cached PBR atlas is BGRA, with emissive in the first byte.
                    int emissive = generic ? 255 : Byte.toUnsignedInt(packedAtlas.get(ValueLayout.JAVA_BYTE, row + (long) sx * Integer.BYTES));
                    if (emissive == 0 && mode != TextureEmission.SEA) {
                        continue;
                    }
                    if (tile.animated) tile.allow(x,y);
                    int argb = albedo.getPixel(sprite.frameX() + sx, sprite.frameY() + sy);
                    double strength = 255.0 * (argb >>> 24) * TextureEmission.weight(mode, emissive / 255.0,
                            linear(argb >> 16 & 255), linear(argb >> 8 & 255), linear(argb & 255));
                    double red = strength * SRGB_TO_LINEAR[(argb >>> 16) & 255];
                    double green = strength * SRGB_TO_LINEAR[(argb >>> 8) & 255];
                    double blue = strength * SRGB_TO_LINEAR[argb & 255];
                    r += red;
                    g += green;
                    b += blue;
                    tile.add(x, y, red, green, blue);
                }
            }
            double scale = 1.0 / (255.0 * 255.0 * width * height);
            tile.finish();
            // A declared but black mask must suppress the legacy lamp in material-aware viewers.
            colors.put(sprite.name(), new Color(r * scale, g * scale, b * scale, tile, authored, generic, mode,
                    !authored && rule == TextureEmission.Rule.ORE));
        }
        return Map.copyOf(colors);
    }

    static int toRgb(double red, double green, double blue, int fallback) {
        double peak = Math.max(red, Math.max(green, blue));
        if (peak <= 0.0) {
            return fallback;
        }
        // MC's light level continues to set intensity. Normalize only the linear chromaticity.
        return (toSrgb(red / peak) << 16) | (toSrgb(green / peak) << 8) | toSrgb(blue / peak);
    }

    static double linear(int srgb) {
        return SRGB_TO_LINEAR[srgb];
    }

    private static int toSrgb(double linear) {
        double value = linear <= 0.0031308 ? 12.92 * linear : 1.055 * Math.pow(linear, 1.0 / 2.4) - 0.055;
        return (int) Math.round(255.0 * value);
    }

    record Color(double red, double green, double blue, SurfaceEmission.Tile tile, boolean authored, boolean generic, int mode, boolean ore) {
        Color(double red, double green, double blue, SurfaceEmission.Tile tile) {
            this(red, green, blue, tile, true, false, TextureEmission.PACKED, false);
        }
        Color(double red, double green, double blue) { this(red, green, blue, null); }
    }
}
