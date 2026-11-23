package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.renderer.texture.SpriteLoader;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.ResourceManager;
import org.slf4j.Logger;

import java.io.EOFException;
import java.io.IOException;
import java.lang.foreign.Arena;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Comparator;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.regex.Pattern;

/** Disk representation of one complete, immutable terrain atlas set. */
public final class TerrainAtlasCache {
    public static final String DIRECTORY = "minecraftue-atlas-cache";
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final int MAGIC = 0x4d434154; // MCAT
    private static final int FORMAT = 1;
    static final int KEY_BYTES = 32;
    static final int HASH_BYTES = 20;
    static final int HEADER_BYTES = 16 + KEY_BYTES + HASH_BYTES;
    private static final Pattern CACHE_NAME = Pattern.compile("[0-9a-f]{64}\\.mcat");
    private static final int KEEP_ENTRIES = 3;
    private static final long MAX_SIDECARS = 2L * 1024 * 1024 * 1024;
    public static final Executor PREPARE = Executors.newSingleThreadExecutor(
            task -> Thread.ofPlatform().daemon().name("Minecraft UE atlas preparation").unstarted(task));

    private TerrainAtlasCache() {}

    public interface Prepared {
        Entry minecraftUe$getPreparedAtlas();
        void minecraftUe$setPreparedAtlas(Entry entry);
    }

    public static void prepare(SpriteLoader.Preparations preparation, ResourceManager resources) {
        long start = System.nanoTime();
        try {
            var size = new Layout(preparation.width(), preparation.height(), preparation.mipLevel() + 1);
            var sprites = preparation.regions().values().stream().map(TerrainAtlas.Sprite::from)
                    .sorted(Comparator.comparing(sprite -> sprite.name().toString())).toList();
            Path root = cacheRoot();
            var relief = BridgeSettings.read().effectiveRelief();
            var entry = prepare(root, size, sprites, resources, relief);
            ((Prepared) (Object) preparation).minecraftUe$setPreparedAtlas(entry);
            LOGGER.info("Minecraft UE atlas preparation {}x{}, {} mips, {} emissive sprites, {} ms",
                    size.width(), size.height(), size.mips(), entry.emissionColors.size(), (System.nanoTime() - start) / 1_000_000L);
        } catch (IOException | RuntimeException failure) {
            LOGGER.warn("Minecraft UE terrain atlas was unavailable for this resource reload", failure);
        }
    }

    private static Path cacheRoot() {
        try {
            Path instance = FabricLoader.getInstance().getGameDir().toRealPath();
            Path root = Files.createDirectories(instance.resolve(DIRECTORY));
            if (!root.equals(root.toRealPath()) || !Files.isDirectory(root, LinkOption.NOFOLLOW_LINKS)) {
                throw new IOException("cache directory resolves outside its configured path");
            }
            return root;
        } catch (IOException failure) {
            LOGGER.warn("Minecraft UE terrain disk cache unavailable: {}", failure.toString());
            return null;
        }
    }

    static Entry prepare(Path root, Layout size, List<TerrainAtlas.Sprite> sprites, ResourceManager resources,
                         TerrainReliefSettings relief) throws IOException {
        byte[] key;
        try {
            key = fingerprint(size, sprites, resources, relief);
        } catch (IOException failure) {
            LOGGER.warn("Minecraft UE terrain resources could not be fingerprinted; using private atlas rows", failure);
            byte[] privateKey = new byte[KEY_BYTES];
            ThreadLocalRandom.current().nextBytes(privateKey);
            return writePrivate(size, privateKey, sprites, resources, relief);
        }
        String name = HexFormat.of().formatHex(key) + ".mcat";
        Path target = root == null ? null : root.resolve(name);
        if (target != null && Files.isRegularFile(target, LinkOption.NOFOLLOW_LINKS)) {
            try {
                var entry = open(target, size, key, true, sprites, resources);
                LOGGER.info("Minecraft UE terrain cache hit {}", name);
                return entry;
            } catch (IOException failure) {
                LOGGER.warn("Minecraft UE terrain cache will rebuild {}: {}", name, failure.getMessage());
            }
        }

        LOGGER.info("Minecraft UE terrain cache miss {}", name);
        Path scratch = null;
        if (target != null) {
            try {
                scratch = Files.createTempFile(root, ".atlas-", ".tmp");
            } catch (IOException failure) {
                LOGGER.warn("Minecraft UE terrain cache write unavailable: {}", failure.toString());
                target = null;
            }
        }
        if (scratch == null) {
            scratch = Files.createTempFile("minecraft-ue-atlas-", ".tmp");
        }
        boolean ownedByEntry = false;
        try {
            write(scratch, size, key, sprites, resources, relief);
            if (target != null) {
                // Directory-backed packs may be edited outside the game while the worker is decoding.
                boolean unchanged;
                try {
                    unchanged = Arrays.equals(key, fingerprint(size, sprites, resources, relief));
                } catch (IOException failure) {
                    unchanged = false;
                }
                if (unchanged) {
                    try {
                        Files.move(scratch, target, StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
                        var entry = open(target, size, key, true, sprites, resources);
                        trim(root, target);
                        return entry;
                    } catch (IOException failure) {
                        LOGGER.warn("Minecraft UE terrain cache commit unavailable: {}", failure.toString());
                        if (!Files.exists(scratch)) {
                            // A competing launcher may have replaced the entry immediately after our move.
                            scratch = Files.createTempFile("minecraft-ue-atlas-", ".tmp");
                            write(scratch, size, key, sprites, resources, relief);
                        }
                    }
                } else {
                    LOGGER.warn("Minecraft UE terrain resources changed during preparation; keeping this atlas private");
                }
            }
            var entry = open(scratch, size, key, false, sprites, resources);
            ownedByEntry = true;
            return entry;
        } finally {
            if (!ownedByEntry) {
                Files.deleteIfExists(scratch);
            }
        }
    }

    private static Entry writePrivate(Layout size, byte[] key, List<TerrainAtlas.Sprite> sprites,
                                      ResourceManager resources, TerrainReliefSettings relief) throws IOException {
        Path scratch = Files.createTempFile("minecraft-ue-atlas-", ".tmp");
        try {
            write(scratch, size, key, sprites, resources, relief);
            return open(scratch, size, key, false, sprites, resources);
        } catch (IOException | RuntimeException failure) {
            Files.deleteIfExists(scratch);
            throw failure;
        }
    }

    private static void trim(Path root, Path current) {
        try (var files = Files.list(root)) {
            var obsolete = files.filter(file -> !file.equals(current) && CACHE_NAME.matcher(file.getFileName().toString()).matches()
                            && Files.isRegularFile(file, LinkOption.NOFOLLOW_LINKS))
                    .sorted(Comparator.comparingLong(TerrainAtlasCache::modified).reversed()).skip(KEEP_ENTRIES - 1L).toList();
            for (Path path : obsolete) {
                try {
                    Files.deleteIfExists(path);
                } catch (IOException ignored) {
                    // Another launcher may still be reading this entry.
                }
            }
        } catch (IOException failure) {
            LOGGER.debug("Could not trim old Minecraft UE atlas cache entries", failure);
        }
    }

    private static long modified(Path path) {
        try {
            return Files.getLastModifiedTime(path, LinkOption.NOFOLLOW_LINKS).toMillis();
        } catch (IOException failure) {
            return 0;
        }
    }

    static byte[] fingerprint(Layout layout, List<TerrainAtlas.Sprite> sprites, ResourceManager resources,
                              TerrainReliefSettings relief) throws IOException {
        var key = digest("SHA-256");
        var scratch = ByteBuffer.allocate(32);
        ints(key, scratch, FORMAT, BridgeWire.VERSION, TerrainMaterialAtlas.MATERIAL_REVISION, layout.width(), layout.height(), layout.mips(),
                relief.strengthPercent(), sprites.size());
        if (relief.strengthPercent() > 0) {
            ints(key, scratch, relief.thresholdTenthsPercent(), relief.maxTiltDegrees());
        }
        try (var defaults = TerrainMaterials.class.getResourceAsStream(TerrainMaterials.DEFAULTS)) {
            key.update(defaults.readAllBytes());
        }
        byte[] streamBuffer = new byte[64 * 1024];
        long sidecarBytes = 0;
        for (var sprite : sprites) {
            byte[] name = sprite.name().toString().getBytes(StandardCharsets.UTF_8);
            ints(key, scratch, name.length, sprite.x(), sprite.y(), sprite.width(), sprite.height(),
                    sprite.padding(), sprite.frameX(), sprite.frameY());
            key.update(name);
            // A static sprite and an animation can have identical first-frame pixels and different relief.
            key.update((byte) (sprite.frames().length > 1 ? 1 : 0));
            for (int mip = 0; mip < layout.mips(); ++mip) {
                var image = sprite.images()[mip];
                ints(key, scratch, image.getWidth(), image.getHeight());
                hashFrame(key, sprite, mip, sprite.frameX(), sprite.frameY());
            }
            if (TerrainEmission.builtinPattern(sprite.name()) != null || TextureEmission.rule(sprite.name()) != null) {
                ints(key, scratch, sprite.frames().length);
                for (int frame : sprite.frames()) {
                    ints(key, scratch, frame);
                    hashFrame(key, sprite, 0, sprite.frameX(frame), sprite.frameY(frame));
                }
            }
            for (String suffix : List.of("_n", "_s")) {
                var resource = resources.getResource(TerrainMaterialAtlas.sidecarPath(sprite.name(), suffix));
                key.update((byte) (resource.isPresent() ? 1 : 0));
                if (resource.isEmpty()) {
                    continue;
                }
                var sidecar = digest("SHA-256");
                try (var input = resource.get().open()) {
                    int read;
                    while ((read = input.read(streamBuffer)) != -1) {
                        sidecarBytes += read;
                        if (sidecarBytes > MAX_SIDECARS) {
                            throw new IOException("Terrain LabPBR source exceeds maximum compressed size");
                        }
                        sidecar.update(streamBuffer, 0, read);
                    }
                }
                key.update(sidecar.digest());
            }
        }
        return key.digest();
    }

    private static void hashFrame(MessageDigest key, TerrainAtlas.Sprite sprite, int mip, int frameX, int frameY) {
        var image = sprite.images()[mip];
        ByteBuffer pixels = image.getPixelBytes();
        int stride = image.getWidth() * Integer.BYTES;
        int width = (sprite.width() >> mip) * Integer.BYTES;
        int x = (frameX >> mip) * Integer.BYTES;
        int y = frameY >> mip;
        for (int row = 0; row < (sprite.height() >> mip); ++row) {
            key.update(pixels.slice((y + row) * stride + x, width));
        }
    }

    private static void ints(MessageDigest digest, ByteBuffer buffer, int... values) {
        buffer.clear();
        for (int value : values) {
            buffer.putInt(value);
        }
        digest.update(buffer.flip());
    }

    private static void write(Path path, Layout size, byte[] key, List<TerrainAtlas.Sprite> sprites, ResourceManager resources,
                              TerrainReliefSettings relief) throws IOException {
        MessageDigest pixels = digest("SHA-1");
        try (var channel = FileChannel.open(path, StandardOpenOption.WRITE, StandardOpenOption.TRUNCATE_EXISTING);
             var materials = new TerrainMaterialAtlas(sprites, resources, size.mips(), relief)) {
            channel.position(HEADER_BYTES);
            for (int type : TerrainAtlas.TEXTURES) {
                for (int mip = 0; mip < size.mips(); ++mip) {
                    int width = size.width() >> mip;
                    int height = size.height() >> mip;
                    for (int row = 0; row < height;) {
                        int rows = Math.min(height - row, BridgeWire.ATLAS_PACKET_BYTES / (width * 4));
                        var frame = TerrainAtlas.encodeRows(type, mip, size.mips(), width, height, row, rows, sprites, materials);
                        frame.position(5 + BridgeWire.ATLAS_HEADER_BYTES);
                        pixels.update(frame.duplicate());
                        while (frame.hasRemaining()) {
                            channel.write(frame);
                        }
                        row += rows;
                    }
                }
            }
            var header = ByteBuffer.allocate(HEADER_BYTES).putInt(MAGIC).putInt(FORMAT)
                    .putShort((short) size.width()).putShort((short) size.height()).putShort((short) size.mips())
                    .putShort((short) 0).put(key).put(pixels.digest()).flip();
            channel.position(0);
            while (header.hasRemaining()) {
                channel.write(header);
            }
            channel.force(true);
        }
    }

    private static Entry open(Path path, Layout size, byte[] key, boolean shared, List<TerrainAtlas.Sprite> sprites,
                              ResourceManager resources) throws IOException {
        FileChannel channel = FileChannel.open(path, StandardOpenOption.READ, LinkOption.NOFOLLOW_LINKS);
        try {
            var header = ByteBuffer.allocate(HEADER_BYTES);
            read(channel, header, 0);
            header.flip();
            byte[] checksum = new byte[HASH_BYTES];
            if (header.getInt() != MAGIC || header.getInt() != FORMAT || header.getShort() != size.width()
                    || header.getShort() != size.height() || header.getShort() != size.mips() || header.getShort() != 0
                    || channel.size() != HEADER_BYTES + size.pixelBytes()) {
                throw new IOException("unexpected atlas cache header or length");
            }
            byte[] storedKey = new byte[KEY_BYTES];
            header.get(storedKey).get(checksum);
            if (!Arrays.equals(key, storedKey)) {
                throw new IOException("terrain atlas cache key mismatch");
            }
            var hash = digest("SHA-1");
            var chunk = ByteBuffer.allocateDirect(256 * 1024);
            for (long offset = HEADER_BYTES; offset < channel.size();) {
                chunk.clear().limit((int) Math.min(chunk.capacity(), channel.size() - offset));
                read(channel, chunk, offset);
                offset += chunk.position();
                hash.update(chunk.flip());
            }
            if (!Arrays.equals(checksum, hash.digest())) {
                throw new IOException("atlas cache checksum mismatch");
            }
            return new Entry(size, key, checksum, path, channel, shared, emissionColors(channel, size, sprites, resources));
        } catch (IOException | RuntimeException failure) {
            channel.close();
            throw failure;
        }
    }

    private static Map<Identifier, TerrainEmission.Color> emissionColors(FileChannel channel, Layout size,
            List<TerrainAtlas.Sprite> sprites, ResourceManager resources) {
        // Both cold and warm loads read the already-decoded PBR.B channel. The wire/disk layout stays unchanged.
        try (var arena = Arena.ofConfined()) {
            long offset = HEADER_BYTES + 2 * size.textureBytes();
            long bytes = (long) size.width() * size.height() * Integer.BYTES;
            var pbr = channel.map(FileChannel.MapMode.READ_ONLY, offset, bytes, arena);
            return TerrainEmission.readColors(sprites, pbr, size.width(), resources);
        } catch (IOException failure) {
            LOGGER.warn("Minecraft UE could not read terrain emission colors; using the block light palette", failure);
            return Map.of();
        }
    }

    static void read(FileChannel channel, ByteBuffer target, long offset) throws IOException {
        while (target.hasRemaining()) {
            int count = channel.read(target, offset);
            if (count < 0) {
                throw new EOFException("atlas file ended during read");
            }
            offset += count;
        }
    }

    private static MessageDigest digest(String algorithm) {
        try {
            return MessageDigest.getInstance(algorithm);
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError(impossible);
        }
    }

    record Layout(int width, int height, int mips) {
        Layout {
            if (width <= 0 || height <= 0 || width > BridgeWire.MAX_ATLAS_EDGE || height > BridgeWire.MAX_ATLAS_EDGE
                    || mips <= 0 || mips > BridgeWire.MAX_ATLAS_MIPS || (Math.min(width, height) >> (mips - 1)) == 0) {
                throw new IllegalArgumentException("Unsupported terrain atlas " + width + "x" + height + " mips=" + mips);
            }
        }

        long pixelBytes() {
            return textureBytes() * TerrainAtlas.TEXTURES.length;
        }

        long textureBytes() {
            long bytes = 0;
            for (int mip = 0; mip < mips; ++mip) {
                bytes += (long) (width >> mip) * (height >> mip) * 4;
            }
            return bytes;
        }
    }

    public static final class Entry implements AutoCloseable {
        final Layout size;
        final byte[] key;
        final byte[] checksum;
        final Path path;
        final FileChannel channel;
        final boolean shared;
        final Map<Identifier, TerrainEmission.Color> emissionColors;
        private int references = 1;

        Entry(Layout size, byte[] key, byte[] checksum, Path path, FileChannel channel, boolean shared,
              Map<Identifier, TerrainEmission.Color> emissionColors) {
            this.size = size;
            this.key = key;
            this.checksum = checksum;
            this.path = path;
            this.channel = channel;
            this.shared = shared;
            this.emissionColors = emissionColors;
        }

        synchronized Entry retain() {
            if (references == 0) {
                throw new IllegalStateException("Terrain atlas cache entry closed");
            }
            references++;
            return this;
        }

        @Override
        public synchronized void close() {
            if (--references == 0) {
                try {
                    channel.close();
                    if (!shared) {
                        Files.deleteIfExists(path);
                    }
                } catch (IOException failure) {
                    LOGGER.debug("Could not release Minecraft UE atlas cache entry", failure);
                }
            }
        }
    }
}
