package dev.minecraftue.bridge;

import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.logging.LogUtils;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.ResourceManager;
import org.slf4j.Logger;

import javax.imageio.ImageIO;
import javax.imageio.stream.FileCacheImageInputStream;
import java.awt.Rectangle;
import java.awt.image.BufferedImage;
import java.awt.image.ColorModel;
import java.awt.image.Raster;
import java.io.IOException;
import java.io.UncheckedIOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.channels.FileChannel;
import java.nio.file.Files;
import java.nio.file.StandardOpenOption;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

final class TerrainMaterialAtlas implements AutoCloseable {
    private static final Logger LOGGER = LogUtils.getLogger();
    static final int MATERIAL_REVISION = 11;
    private static final int MIN_WATER_NORMAL_EDGE = 32;
    private static final long MAX_SOURCE_PIXELS = 2L * BridgeWire.MAX_ATLAS_EDGE * BridgeWire.MAX_ATLAS_EDGE;
    private final PixelStore store = new PixelStore();
    private final int fallback;
    private final int fallbackNormal;
    private final List<SpriteMaps> sprites;

    TerrainMaterialAtlas(List<TerrainAtlas.Sprite> albedoSprites, ResourceManager resources, int mipCount,
                         TerrainReliefSettings relief) {
        long started = System.nanoTime();
        var defaults = TerrainMaterials.load();
        fallback = defaults.fallback();
        fallbackNormal = defaults.fallbackNormal();
        var prepared = new ArrayList<SpriteMaps>(albedoSprites.size());
        var byName = new HashMap<Identifier, TerrainAtlas.Sprite>();
        for (var sprite : albedoSprites) byName.put(sprite.name(), sprite);
        int normalCount = 0;
        int specularCount = 0;
        int gradientCount = 0;
        try {
            for (var sprite : albedoSprites) {
                var maps = new SpriteBuilder(sprite, mipCount, defaults.forSprite(sprite.name()),
                        defaults.porosityForSprite(sprite.name()));
                if (maps.readNormals(resources)) {
                    normalCount++;
                } else if (relief.strengthPercent() > 0 && !maps.water && maps.generateNormals(relief)) {
                    gradientCount++;
                }
                if (maps.readSpecular(resources)) {
                    specularCount++;
                } else {
                    maps.applyBuiltinEmission(byName);
                }
                prepared.add(maps.finish());
            }
        } catch (RuntimeException | Error failure) {
            close();
            throw failure;
        }
        sprites = List.copyOf(prepared);
        LOGGER.info("Minecraft UE terrain materials: {} sprites, {} LabPBR normals, {} LabPBR specular maps, "
                        + "{} gradient normals, {} ms", sprites.size(), normalCount, specularCount, gradientCount,
                (System.nanoTime() - started) / 1_000_000L);
    }

    int fill(int type) {
        return type == BridgeWire.NORMAL_ATLAS_ROWS ? fallbackNormal : fallback;
    }

    Tile tile(int type, int index) {
        SpriteMaps sprite = sprites.get(index);
        return type == BridgeWire.NORMAL_ATLAS_ROWS ? sprite.normal() : sprite.pbr();
    }

    @Override
    public void close() {
        store.close();
    }

    private final class SpriteBuilder {
        private final TerrainAtlas.Sprite sprite;
        private final int mipCount;
        private final int defaultPorosity;
        private final boolean water;
        private Tile normal;
        private Tile pbr;

        private SpriteBuilder(TerrainAtlas.Sprite sprite, int mipCount, int fallback, int defaultPorosity) {
            this.sprite = sprite;
            this.mipCount = mipCount;
            this.defaultPorosity = defaultPorosity;
            this.water = sprite.name().getNamespace().equals("minecraft") &&
                    switch (sprite.name().getPath()) {
                        case "block/water_still", "block/water_flow", "block/water_overlay" -> true;
                        default -> false;
                    };
            this.normal = new Tile(LabPbr.withPorosity(LabPbr.FLAT_NORMAL, defaultPorosity), null);
            this.pbr = new Tile(fallback, null);
        }

        boolean readNormals(ResourceManager resources) {
            if (water && Math.min(sprite.width(), sprite.height()) < MIN_WATER_NORMAL_EDGE) {
                return false;
            }
            Sidecar input = readSidecar(resources, sprite, "_n");
            if (input == null || (water && Math.min(input.raster.getWidth(), input.raster.getHeight()) < MIN_WATER_NORMAL_EDGE)) {
                return false;
            }
            normal = allocate(normal.fill());
            int next = 0;
            for (int y = 0; y < sprite.height(); y++) {
                for (int x = 0; x < sprite.width(); x++, next++) {
                    int raw = input.sample(sprite, x, y);
                    normal.set(0, next, LabPbr.decodeNormal(raw, defaultPorosity));
                    int occlusion = raw & 255;
                    if (occlusion != pbr.fill() >>> 24) {
                        ensurePbrPixels();
                        pbr.set(0, next, LabPbr.withOcclusion(pbr.fill(), occlusion));
                    }
                }
            }
            return true;
        }

        boolean readSpecular(ResourceManager resources) {
            Sidecar input = readSidecar(resources, sprite, "_s");
            if (input == null) {
                return false;
            }
            ensurePbrPixels();
            int next = 0;
            for (int y = 0; y < sprite.height(); y++) {
                for (int x = 0; x < sprite.width(); x++, next++) {
                    int raw = input.sample(sprite, x, y);
                    pbr.set(0, next, LabPbr.decodeSpecular(raw, pbr.sample(0, next) >>> 24));
                    int porosity = LabPbr.decodePorosity(raw);
                    if (porosity != (normal.sample(0, next) >>> 24)) {
                        ensureNormalPixels();
                        normal.set(0, next, LabPbr.withPorosity(normal.sample(0, next), porosity));
                    }
                }
            }
            return true;
        }

        void applyBuiltinEmission(Map<Identifier, TerrainAtlas.Sprite> byName) {
            var rule = TextureEmission.rule(sprite.name());
            if (rule != null) {
                String other = TextureEmission.companion(sprite.name().getPath(), rule);
                var off = rule == TextureEmission.Rule.ORE ? OreEmission.host(sprite,byName) :
                        other == null ? null : byName.get(sprite.name().withPath(other));
                byte[] weights = TextureEmission.gate(sprite, rule, off);
                ensurePbrPixels();
                for (int i = 0; i < weights.length; i++) {
                    pbr.set(0, i, (pbr.sample(0, i) & 0xffffff00) | Byte.toUnsignedInt(weights[i]));
                }
                return;
            }
            var pattern = TerrainEmission.builtinPattern(sprite.name());
            if (pattern == null) {
                return;
            }
            var mask = pattern.stableMask(sprite);
            if (mask == null) {
                return;
            }
            ensurePbrPixels();
            for (int i = 0; i < sprite.width() * sprite.height(); i++) {
                if (!mask.get(i)) {
                    pbr.set(0, i, pbr.sample(0, i) & 0xffffff00);
                }
            }
        }

        boolean generateNormals(TerrainReliefSettings relief) {
            double strength = reliefStrength(sprite.name().getPath()) * (relief.strengthPercent() / 100.0);
            if (strength == 0.0 || sprite.frames().length > 1) {
                return false;
            }
            NativeImage source = sprite.images()[0];
            int left = sprite.frameX();
            int top = sprite.frameY();
            int right = left + sprite.width() - 1;
            int bottom = top + sprite.height() - 1;
            for (int y = top; y <= bottom; y++) {
                for (int x = left; x <= right; x++) {
                    if ((source.getPixel(x, y) >>> 24) != 255) {
                        return false;
                    }
                }
            }

            double deadZone = relief.thresholdTenthsPercent() / 1000.0;
            double maxSlope = Math.tan(Math.toRadians(relief.maxTiltDegrees()));
            int next = 0;
            for (int y = top; y <= bottom; y++) {
                int above = Math.max(top, y - 1);
                int below = Math.min(bottom, y + 1);
                for (int x = left; x <= right; x++, next++) {
                    int before = Math.max(left, x - 1);
                    int after = Math.min(right, x + 1);
                    double upperLeft = luma(source.getPixel(before, above));
                    double upperRight = luma(source.getPixel(after, above));
                    double lowerLeft = luma(source.getPixel(before, below));
                    double lowerRight = luma(source.getPixel(after, below));
                    // Sobel smooths across the gradient, so isolated color flecks produce less relief.
                    double dx = (upperLeft + 2.0 * luma(source.getPixel(before, y)) + lowerLeft
                            - upperRight - 2.0 * luma(source.getPixel(after, y)) - lowerRight) * 0.25;
                    double dy = (upperLeft + 2.0 * luma(source.getPixel(x, above)) + upperRight
                            - lowerLeft - 2.0 * luma(source.getPixel(x, below)) - lowerRight) * 0.25;
                    dx = reliefSlope(dx, deadZone) * strength;
                    dy = reliefSlope(dy, deadZone) * strength;
                    if (dx == 0.0 && dy == 0.0) {
                        continue;
                    }
                    double slope = Math.sqrt(dx * dx + dy * dy);
                    double scale = Math.min(1.0, maxSlope / slope);
                    ensureNormalPixels();
                    normal.set(0, next, LabPbr.normal(dx * scale, dy * scale, 1.0, defaultPorosity));
                }
            }
            return normal.mips() != null;
        }

        private void ensureNormalPixels() {
            if (normal.mips() == null) {
                normal = allocate(normal.fill());
                for (int i = 0; i < sprite.width() * sprite.height(); i++) {
                    normal.set(0, i, normal.fill());
                }
            }
        }

        private void ensurePbrPixels() {
            if (pbr.mips() == null) {
                pbr = allocate(pbr.fill());
                for (int i = 0; i < sprite.width() * sprite.height(); i++) {
                    pbr.set(0, i, pbr.fill());
                }
            }
        }

        private Tile allocate(int fill) {
            var levels = new MemorySegment[mipCount];
            for (int mip = 0; mip < mipCount; mip++) {
                levels[mip] = store.allocate((sprite.width() >> mip) * (sprite.height() >> mip));
            }
            return new Tile(fill, levels);
        }

        private SpriteMaps finish() {
            if (normal.mips() != null) {
                generateMips(normal, true);
            }
            if (pbr.mips() != null) {
                generateMips(pbr, false);
            }
            return new SpriteMaps(normal, pbr);
        }

        private void generateMips(Tile tile, boolean isNormal) {
            for (int mip = 1; mip < mipCount; mip++) {
                int width = sprite.width() >> mip;
                int height = sprite.height() >> mip;
                int sourceWidth = sprite.width() >> (mip - 1);
                for (int y = 0; y < height; y++) {
                    for (int x = 0; x < width; x++) {
                        int source = (y * sourceWidth + x) * 2;
                        int a = tile.sample(mip - 1, source);
                        int b = tile.sample(mip - 1, source + 1);
                        int c = tile.sample(mip - 1, source + sourceWidth);
                        int d = tile.sample(mip - 1, source + sourceWidth + 1);
                        tile.set(mip, y * width + x,
                                isNormal ? LabPbr.averageNormal(a, b, c, d) : LabPbr.averagePacked(a, b, c, d));
                    }
                }
            }
        }
    }

    private static double reliefStrength(String path) {
        if (path.equals("block/grass_block_top") || path.endsWith("_leaves")) {
            return 0.0;
        }
        if (path.endsWith("cobblestone") || path.equals("block/cobbled_deepslate") || path.endsWith("bricks")) {
            return 1.8;
        }
        if (path.endsWith("_planks")) {
            return 1.5;
        }
        return 0.65;
    }

    private static double reliefSlope(double gradient, double deadZone) {
        return Math.copySign(Math.max(0.0, Math.abs(gradient) - deadZone), gradient);
    }

    private static double luma(int pixel) {
        return (0.2126 * ((pixel >>> 16) & 255) + 0.7152 * ((pixel >>> 8) & 255) + 0.0722 * (pixel & 255)) / 255.0;
    }

    static Identifier sidecarPath(Identifier sprite, String suffix) {
        return sprite.withPrefix("textures/").withSuffix(suffix + ".png");
    }

    private static Sidecar readSidecar(ResourceManager resources, TerrainAtlas.Sprite sprite, String suffix) {
        var path = sidecarPath(sprite.name(), suffix);
        var resource = resources.getResource(path).orElse(null);
        if (resource == null) {
            return null;
        }
        var png = ImageIO.getImageReadersByFormatName("PNG").next();
        try (var input = resource.open(); var cached = new FileCacheImageInputStream(input, null)) {
            // Read the PNG data channels without applying image metadata or allocating every animation frame.
            png.setInput(cached, true, true);
            int width = png.getWidth(0);
            int height = png.getHeight(0);
            if (width > 32768 || height > 32768 || (long) width * height > MAX_SOURCE_PIXELS) {
                throw new IOException("PNG exceeds supported material image size: " + width + "x" + height);
            }
            Rectangle frame = frameRegion(sprite, width, height);
            var params = png.getDefaultReadParam();
            params.setSourceRegion(frame);
            int strideX = Math.max(1, frame.width / sprite.width());
            int strideY = Math.max(1, frame.height / sprite.height());
            if (frame.width % strideX != 0) {
                strideX = 1;
            }
            if (frame.height % strideY != 0) {
                strideY = 1;
            }
            params.setSourceSubsampling(strideX, strideY, strideX / 2, strideY / 2);
            return new Sidecar(png.read(0, params));
        } catch (IOException | RuntimeException failure) {
            LOGGER.warn("Ignoring terrain sidecar {} from {}: {}", path, resource.sourcePackId(), failure.toString());
            return null;
        } finally {
            png.dispose();
        }
    }

    static Rectangle frameRegion(TerrainAtlas.Sprite sprite, int width, int height) throws IOException {
        NativeImage source = sprite.images()[0];
        int columns = source.getWidth() / sprite.width();
        int rows = source.getHeight() / sprite.height();
        if (width % columns == 0 && height % rows == 0 && sameAspect(width, height, source.getWidth(), source.getHeight())) {
            int frameWidth = width / columns;
            int frameHeight = height / rows;
            return new Rectangle(sprite.frameX() / sprite.width() * frameWidth,
                    sprite.frameY() / sprite.height() * frameHeight, frameWidth, frameHeight);
        }
        if (sameAspect(width, height, sprite.width(), sprite.height())) {
            return new Rectangle(width, height);
        }
        throw new IOException("PNG " + width + "x" + height + " does not fit the albedo frame or sheet");
    }

    private static boolean sameAspect(int width, int height, int otherWidth, int otherHeight) {
        return (long) width * otherHeight == (long) height * otherWidth;
    }

    private static final class Sidecar {
        private final Raster raster;
        private final ColorModel colors;
        private final int[] components;
        private final int[] maxima;
        private Object pixel;

        private Sidecar(BufferedImage image) {
            raster = image.getRaster();
            colors = image.getColorModel();
            components = new int[colors.getNumComponents()];
            maxima = colors.getComponentSize();
            for (int i = 0; i < maxima.length; i++) {
                maxima[i] = (1 << maxima[i]) - 1;
            }
        }

        private int sample(TerrainAtlas.Sprite sprite, int x, int y) {
            int sourceX = (int) (((2L * x + 1) * raster.getWidth()) / (2 * sprite.width()));
            int sourceY = (int) (((2L * y + 1) * raster.getHeight()) / (2 * sprite.height()));
            pixel = raster.getDataElements(sourceX, sourceY, pixel);
            // Read numeric data channels, including grayscale textures, without color-space conversion.
            colors.getComponents(pixel, components, 0);
            int red = channel(0);
            boolean grayscale = colors.getNumColorComponents() == 1;
            return LabPbr.packed(red, grayscale ? red : channel(1), grayscale ? red : channel(2),
                    colors.hasAlpha() ? channel(colors.getNumColorComponents()) : 255);
        }

        private int channel(int component) {
            int max = maxima[component];
            return (components[component] * 255 + max / 2) / max;
        }
    }

    private record SpriteMaps(Tile normal, Tile pbr) {}

    record Tile(int fill, MemorySegment[] mips) {
        int sample(int mip, int index) {
            return mips == null ? fill : mips[mip].getAtIndex(ValueLayout.JAVA_INT, index);
        }

        void set(int mip, int index, int pixel) {
            mips[mip].setAtIndex(ValueLayout.JAVA_INT, index, pixel);
        }
    }

    /** Default tiles consume no pixel arrays. Large packs spill decoded tiles to a temporary file. */
    private static final class PixelStore implements AutoCloseable {
        private static final long HEAP_BYTES = 64L * 1024 * 1024;
        private static final long MAPPING_BYTES = 32L * 1024 * 1024;
        private long heapBytes;
        private long fileOffset;
        private long mappingOffset;
        private Arena mappings;
        private MemorySegment mapping;
        private FileChannel file;

        MemorySegment allocate(int pixels) {
            long bytes = (long) pixels * Integer.BYTES;
            if (heapBytes + bytes <= HEAP_BYTES) {
                heapBytes += bytes;
                return MemorySegment.ofArray(new int[pixels]);
            }
            try {
                if (file == null) {
                    var scratch = Files.createTempFile("minecraft-ue-terrain-", ".pixels");
                    try {
                        file = FileChannel.open(scratch, StandardOpenOption.READ, StandardOpenOption.WRITE,
                                StandardOpenOption.DELETE_ON_CLOSE);
                    } catch (IOException failure) {
                        Files.deleteIfExists(scratch);
                        throw failure;
                    }
                    mappings = Arena.ofConfined();
                }
                if (mapping == null || mappingOffset + bytes > mapping.byteSize()) {
                    long capacity = Math.max(bytes, MAPPING_BYTES);
                    mapping = file.map(FileChannel.MapMode.READ_WRITE, fileOffset, capacity, mappings);
                    fileOffset += capacity;
                    mappingOffset = 0;
                }
                var slice = mapping.asSlice(mappingOffset, bytes);
                mappingOffset += bytes;
                return slice;
            } catch (IOException failure) {
                throw new UncheckedIOException("Could not allocate terrain material scratch data", failure);
            }
        }

        @Override
        public void close() {
            if (mappings != null) {
                mappings.close();
                mappings = null;
                mapping = null;
            }
            if (file != null) {
                try {
                    file.close();
                } catch (IOException failure) {
                    LOGGER.warn("Could not close terrain material scratch data", failure);
                }
                file = null;
            }
        }
    }
}
