package dev.minecraftue.bridge;

import com.mojang.blaze3d.GpuFormat;
import com.mojang.blaze3d.buffers.GpuBuffer;
import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.logging.LogUtils;
import dev.minecraftue.bridge.mixin.TextureAtlasAccessor;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.texture.AbstractTexture;
import net.minecraft.client.renderer.texture.DynamicTexture;
import net.minecraft.client.renderer.texture.ReloadableTexture;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.resources.Identifier;
import net.minecraft.util.ARGB;
import org.slf4j.Logger;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

import static dev.minecraftue.bridge.BridgeWire.*;

final class DynamicTextureStream {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final int PACKETS_PER_FRAME = 4;
    private final Map<Identifier, TextureEntry> textures = new HashMap<>();
    private final Set<TextureEntry> requested = new LinkedHashSet<>();
    private final AtlasPatchStream patches = new AtlasPatchStream();
    private int scenePixels;
    private Upload uploading;
    private boolean preferPatch = true;

    void reset() {
        if (uploading != null) {
            uploading.discard();
            uploading = null;
        }
        textures.clear();
        requested.clear();
        patches.reset();
        scenePixels = 0;
        preferPatch = true;
    }

    void trackBlockAtlas(TextureAtlas blocks) {
        patches.track(DYNAMIC_BLOCK_ATLAS, blocks, blocks.getTexture().getMipLevels());
    }

    void atlasAnimated(TextureAtlas atlas) {
        patches.atlasAnimated(atlas);
    }

    int use(Minecraft minecraft, Identifier location) {
        if (location == null) {
            return DYNAMIC_WHITE;
        }
        if (location.equals(TextureAtlas.LOCATION_BLOCKS)) {
            return DYNAMIC_BLOCK_ATLAS;
        }
        TextureEntry entry = textures.get(location);
        if (entry == null) {
            if (textures.size() >= MAX_DYNAMIC_TEXTURES) {
                return -1;
            }
            entry = new TextureEntry(location, textures.size() + 2);
            textures.put(location, entry);
        }
        AbstractTexture texture = minecraft.getTextureManager().getTexture(location);
        GpuTexture gpu = texture.getTexture();
        if (gpu != null && !gpu.isClosed()) {
            int revision = texture instanceof DynamicTextureRevision versioned ? versioned.minecraftUe$textureRevision() : 0;
            if (gpu != entry.gpu || revision != entry.revision) {
                patches.forget(entry.id);
                entry.gpu = gpu;
                entry.revision = revision;
                entry.source = texture;
                requested.add(entry);
            }
        }
        return entry.pixels > 0 ? entry.id : -1;
    }

    void flush(Minecraft minecraft, LocalTransport.Connection connection, int scene) {
        patches.capture(scene);
        if (!connection.canQueueDynamic(DYNAMIC_TEXTURE_TRANSFER)) {
            return;
        }
        if (uploading != null) {
            if (uploading.row == uploading.height) {
                // The last row has left the socket queue. Later meshes can now reference this texture.
                TextureEntry entry = uploading.entry;
                int pixels = uploading.width * uploading.height;
                scenePixels += pixels - entry.pixels;
                entry.pixels = pixels;
                if (entry.source instanceof TextureAtlas atlas && uploading.gpu == entry.gpu && !entry.gpu.isClosed()) {
                    patches.track(entry.id, atlas, 1);
                }
                uploading.discard();
                uploading = null;
            } else if (uploading.failed || uploading.gpu.isClosed() || uploading.gpu != uploading.entry.gpu) {
                uploading.discard();
                uploading = null;
            }
        }
        // Texture rows and patches share a FIFO so a patch cannot overtake its base texture.
        if (preferPatch && patches.publish(connection)) {
            preferPatch = false;
        } else if (flushRows(minecraft, connection, scene)) {
            preferPatch = true;
        } else if (patches.publish(connection)) {
            preferPatch = false;
        }
    }

    private boolean flushRows(Minecraft minecraft, LocalTransport.Connection connection, int scene) {
        if (uploading == null && !requested.isEmpty()) {
            var iterator = requested.iterator();
            TextureEntry entry = iterator.next();
            iterator.remove();
            try {
                uploading = beginUpload(minecraft, entry);
            } catch (IOException | RuntimeException failure) {
                LOGGER.warn("Could not export dynamic texture {}", entry.location, failure);
            }
        }
        if (uploading == null || uploading.rows == null) {
            return false;
        }
        Upload upload = uploading;
        try {
            List<ByteBuffer> packets = new ArrayList<>(PACKETS_PER_FRAME);
            int row = upload.row;
            for (int packet = 0; row < upload.height && packet < PACKETS_PER_FRAME; packet++) {
                int count = Math.min(upload.height - row, ATLAS_PACKET_BYTES / (upload.width * Integer.BYTES));
                int size = DYNAMIC_TEXTURE_HEADER_BYTES + upload.width * count * Integer.BYTES;
                ByteBuffer frame = frame(DYNAMIC_TEXTURE_ROWS, size).putInt(scene).putInt(upload.entry.id)
                        .putShort((short) upload.width).putShort((short) upload.height)
                        .putShort((short) row).putShort((short) count);
                upload.rows.write(frame.order(ByteOrder.LITTLE_ENDIAN), row, count);
                packets.add(frame.order(ByteOrder.BIG_ENDIAN).position(frame.capacity()).flip());
                row += count;
            }
            if (connection.sendDynamic(DYNAMIC_TEXTURE_TRANSFER, packets)) {
                upload.row = row;
                return true;
            }
        } catch (RuntimeException failure) {
            LOGGER.warn("Could not send dynamic texture {}", upload.entry.location, failure);
            upload.discard();
            uploading = null;
        }
        return false;
    }

    private Upload beginUpload(Minecraft minecraft, TextureEntry entry) throws IOException {
        GpuTexture gpu = entry.gpu;
        if (gpu == null || gpu.isClosed()) {
            return null;
        }
        int width = gpu.getWidth(0);
        int height = gpu.getHeight(0);
        if (width <= 0 || height <= 0 || width > MAX_ATLAS_EDGE || height > MAX_ATLAS_EDGE
                || width * height > MAX_DYNAMIC_TEXTURE_PIXELS
                || scenePixels - entry.pixels + width * height > MAX_DYNAMIC_SCENE_PIXELS) {
            LOGGER.warn("Skipping dynamic texture {} at {}x{} (scene texture budget)", entry.location, width, height);
            return null;
        }

        Upload upload = new Upload(entry, gpu, width, height);
        if (entry.source instanceof TextureAtlas atlas) {
            List<TerrainAtlas.Sprite> sprites = ((TextureAtlasAccessor) atlas).minecraftUe$getSprites()
                    .stream().map(TerrainAtlas.Sprite::from).toList();
            upload.rows = (packet, first, count) -> {
                int start = packet.position();
                for (TerrainAtlas.Sprite sprite : sprites) {
                    sprite.writeRows(packet, start, width, 0, first, first + count, null);
                }
            };
        } else if (entry.source instanceof DynamicTexture dynamic && dynamic.getPixels() != null) {
            // DynamicTexture can be replaced or rewritten while packets are being sent.
            NativeImage image = dynamic.getPixels();
            if (image.getWidth() != width || image.getHeight() != height) {
                return null;
            }
            int[] abgr = image.getPixelsABGR();
            upload.rows = (packet, first, count) -> {
                for (int index = first * width; index < (first + count) * width; index++) {
                    packet.putInt(ARGB.fromABGR(abgr[index]));
                }
            };
        } else if (entry.source instanceof ReloadableTexture reloadable && gpu.getDepthOrLayers() == 1) {
            var contents = reloadable.loadContents(minecraft.getResourceManager());
            NativeImage image = contents.image();
            if (image.getWidth() != width || image.getHeight() != height) {
                contents.close();
                return null;
            }
            upload.closePixels = contents::close;
            upload.rows = (packet, first, count) -> {
                for (int y = first; y < first + count; y++) {
                    for (int x = 0; x < width; x++) {
                        packet.putInt(image.getPixel(x, y));
                    }
                }
            };
        } else if ((gpu.usage() & GpuTexture.USAGE_COPY_SRC) != 0 && gpu.getDepthOrLayers() == 1
                && (gpu.getFormat() == GpuFormat.RGBA8_UNORM || gpu.getFormat() == GpuFormat.R8_UNORM)) {
            upload.readback();
        } else {
            LOGGER.debug("Unsupported dynamic texture source {}", entry.location);
            return null;
        }
        return upload;
    }

    @FunctionalInterface
    private interface PixelRows {
        void write(ByteBuffer packet, int first, int count);
    }

    private static final class TextureEntry {
        final Identifier location;
        final int id;
        AbstractTexture source;
        GpuTexture gpu;
        int revision;
        int pixels;

        private TextureEntry(Identifier location, int id) {
            this.location = location;
            this.id = id;
        }
    }

    private static final class Upload {
        final TextureEntry entry;
        final GpuTexture gpu;
        final int width;
        final int height;
        PixelRows rows;
        Runnable closePixels;
        boolean cancelled;
        boolean failed;
        int row;

        private Upload(TextureEntry entry, GpuTexture gpu, int width, int height) {
            this.entry = entry;
            this.gpu = gpu;
            this.width = width;
            this.height = height;
        }

        void readback() {
            boolean grayscale = gpu.getFormat() == GpuFormat.R8_UNORM;
            int bytes = width * height * (grayscale ? 1 : Integer.BYTES);
            var device = RenderSystem.getDevice();
            GpuBuffer staging = device.createBuffer(() -> "Minecraft UE dynamic texture readback",
                    GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST, bytes);
            try {
                device.createCommandEncoder().copyTextureToBuffer(gpu, staging, 0, () -> {
                    try (staging) {
                        if (cancelled) {
                            return;
                        }
                        byte[] pixels = new byte[bytes];
                        try (var mapped = staging.map(true, false)) {
                            mapped.data().get(pixels);
                        }
                        rows = (packet, first, count) -> {
                            for (int index = first * width; index < (first + count) * width; index++) {
                                if (grayscale) {
                                    packet.putInt((Byte.toUnsignedInt(pixels[index]) << 24) | 0x00ffffff);
                                } else {
                                    int offset = index * Integer.BYTES;
                                    packet.put(pixels[offset + 2]).put(pixels[offset + 1])
                                            .put(pixels[offset]).put(pixels[offset + 3]);
                                }
                            }
                        };
                    } catch (RuntimeException failure) {
                        failed = true;
                        LOGGER.warn("Could not read back dynamic texture {}", entry.location, failure);
                    }
                }, 0);
            } catch (RuntimeException failure) {
                staging.close();
                throw failure;
            }
        }

        void discard() {
            cancelled = true;
            if (closePixels != null) {
                closePixels.run();
            }
        }
    }
}
