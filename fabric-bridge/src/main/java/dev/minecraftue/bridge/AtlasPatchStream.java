package dev.minecraftue.bridge;

import com.mojang.blaze3d.GpuFormat;
import com.mojang.blaze3d.buffers.GpuBuffer;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.logging.LogUtils;
import dev.minecraftue.bridge.mixin.TextureAtlasAccessor;
import dev.minecraftue.bridge.mixin.TextureAtlasSpriteAccessor;
import net.minecraft.client.renderer.texture.SpriteContents;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import org.slf4j.Logger;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;

import static dev.minecraftue.bridge.BridgeWire.*;

final class AtlasPatchStream {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final Runnable COPY_COMPLETED = () -> {};
    private final Map<Integer, Atlas> atlases = new HashMap<>();
    private final LinkedHashSet<Atlas> dirty = new LinkedHashSet<>();
    private Readback readback;

    void reset() {
        readback = null;
        dirty.clear();
        atlases.clear();
    }

    void forget(int id) {
        Atlas atlas = atlases.remove(id);
        if (atlas == null) {
            return;
        }
        dirty.remove(atlas);
        if (readback != null && readback.atlas == atlas) {
            readback = null;
        }
    }

    void track(int id, TextureAtlas source, int mipCount) {
        forget(id);
        var accessor = (TextureAtlasAccessor) source;
        var animations = accessor.minecraftUe$getAnimationStates();
        if (animations.isEmpty()) {
            return;
        }
        var gpu = source.getTexture();
        if (gpu.isClosed() || gpu.getFormat() != GpuFormat.RGBA8_UNORM || (gpu.usage() & GpuTexture.USAGE_COPY_SRC) == 0) {
            return;
        }
        // TextureAtlas creates its animation states in the same order as its animated sprites.
        var sprites = accessor.minecraftUe$getSprites().stream().filter(TextureAtlasSprite::isAnimated).toList();
        if (sprites.size() != animations.size()) {
            LOGGER.warn("Cannot match Minecraft UE atlas animation states for {}", source.location());
            return;
        }
        var atlas = new Atlas(id, source, gpu);
        for (int i = 0; i < sprites.size(); i++) {
            var sprite = sprites.get(i);
            int padding = ((TextureAtlasSpriteAccessor) sprite).minecraftUe$getPadding();
            List<Region> regions = new ArrayList<>();
            for (int mip = 0; mip < mipCount; mip++) {
                int width = (sprite.contents().width() + 2 * padding) >> mip;
                int height = (sprite.contents().height() + 2 * padding) >> mip;
                splitRegion(regions, mip, sprite.getX() >> mip, sprite.getY() >> mip, width, height);
            }
            atlas.slots.add(new Slot(animations.get(i), regions));
            // The base atlas upload uses the first frame. Bring it to the current frame once.
            atlas.dirty.addAll(regions);
        }
        atlases.put(id, atlas);
        dirty.add(atlas);
    }

    void atlasAnimated(TextureAtlas source) {
        for (Atlas atlas : atlases.values()) {
            if (atlas.source != source || !atlas.isCurrent()) {
                continue;
            }
            for (Slot slot : atlas.slots) {
                if (slot.animation().needsToDraw()) {
                    atlas.dirty.addAll(slot.regions());
                }
            }
            if (!atlas.dirty.isEmpty()) {
                dirty.add(atlas);
            }
        }
    }

    void capture(int scene) {
        if (readback != null && !readback.atlas.isCurrent()) {
            forget(readback.atlas.id);
        }
        while (readback == null && !dirty.isEmpty()) {
            Atlas atlas = dirty.removeFirst();
            if (!atlas.isCurrent()) {
                forget(atlas.id);
                continue;
            }
            List<Region> regions = new ArrayList<>();
            int pixelBytes = 0;
            var pending = atlas.dirty.iterator();
            while (pending.hasNext() && regions.size() < MAX_ATLAS_PATCH_REGIONS) {
                Region region = pending.next();
                int headers = ATLAS_PATCH_HEADER_BYTES + (regions.size() + 1) * ATLAS_PATCH_REGION_BYTES;
                if (headers + pixelBytes + region.pixelBytes() > ATLAS_PACKET_BYTES) {
                    break;
                }
                regions.add(region);
                pixelBytes += region.pixelBytes();
                pending.remove();
            }
            if (!atlas.dirty.isEmpty()) {
                dirty.add(atlas);
            }
            readback = new Readback(atlas);
            try {
                readback.copy(scene, regions, pixelBytes);
            } catch (RuntimeException failure) {
                failed(atlas, failure);
            }
        }
    }

    boolean publish(LocalTransport.Connection connection) {
        if (readback == null || readback.frame == null
                || !connection.sendDynamic(DYNAMIC_TEXTURE_TRANSFER, List.of(readback.frame))) {
            return false;
        }
        readback = null;
        return true;
    }

    private void failed(Atlas atlas, RuntimeException failure) {
        LOGGER.warn("Minecraft UE atlas patches stopped for {}", atlas.source.location(), failure);
        forget(atlas.id);
    }

    static void splitRegion(List<Region> regions, int mip, int x, int y, int width, int height) {
        int maxRows = (ATLAS_PACKET_BYTES - ATLAS_PATCH_HEADER_BYTES - ATLAS_PATCH_REGION_BYTES) / (width * Integer.BYTES);
        for (int row = 0; row < height; row += maxRows) {
            regions.add(new Region(mip, x, y + row, width, Math.min(height - row, maxRows)));
        }
    }

    static ByteBuffer encode(int scene, int id, List<Region> regions, int pixelBytes, ByteBuffer rgba) {
        int size = ATLAS_PATCH_HEADER_BYTES + regions.size() * ATLAS_PATCH_REGION_BYTES + pixelBytes;
        ByteBuffer frame = BridgeWire.frame(ATLAS_PATCH, size).putInt(scene).putInt(id).putShort((short) regions.size());
        for (Region region : regions) {
            frame.putShort((short) region.mip()).putShort((short) region.x()).putShort((short) region.y())
                    .putShort((short) region.width()).putShort((short) region.height());
            for (int i = 0; i < region.pixelBytes(); i += Integer.BYTES) {
                byte red = rgba.get();
                byte green = rgba.get();
                byte blue = rgba.get();
                byte alpha = rgba.get();
                frame.put(blue).put(green).put(red).put(alpha);
            }
        }
        return frame.flip();
    }

    record Region(int mip, int x, int y, int width, int height) {
        int pixelBytes() { return width * height * Integer.BYTES; }
    }

    private record Slot(SpriteContents.AnimationState animation, List<Region> regions) {}

    private static final class Atlas {
        final int id;
        final TextureAtlas source;
        final GpuTexture gpu;
        final List<Slot> slots = new ArrayList<>();
        final LinkedHashSet<Region> dirty = new LinkedHashSet<>();

        Atlas(int id, TextureAtlas source, GpuTexture gpu) {
            this.id = id;
            this.source = source;
            this.gpu = gpu;
        }

        boolean isCurrent() { return !gpu.isClosed() && gpu == source.getTexture(); }
    }

    private final class Readback {
        final Atlas atlas;
        ByteBuffer frame;

        Readback(Atlas atlas) {
            this.atlas = atlas;
        }

        void copy(int scene, List<Region> regions, int pixelBytes) {
            var device = RenderSystem.getDevice();
            GpuBuffer staging = device.createBuffer(() -> "Minecraft UE atlas patch readback",
                    GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST, pixelBytes);
            try {
                var encoder = device.createCommandEncoder();
                int offset = 0;
                for (int i = 0; i < regions.size(); i++) {
                    Region region = regions.get(i);
                    // Copies on the same encoder complete in order. Map once after the last copy.
                    Runnable completed = i + 1 < regions.size() ? COPY_COMPLETED
                            : () -> finish(staging, scene, regions, pixelBytes);
                    encoder.copyTextureToBuffer(atlas.gpu, staging, offset, completed,
                            region.mip(), region.x(), region.y(), region.width(), region.height());
                    offset += region.pixelBytes();
                }
            } catch (RuntimeException failure) {
                staging.close();
                throw failure;
            }
        }

        private void finish(GpuBuffer staging, int scene, List<Region> regions, int pixelBytes) {
            if (staging.isClosed()) {
                return;
            }
            try (staging) {
                if (readback == this && atlas.isCurrent()) {
                    try (var mapped = staging.map(true, false)) {
                        frame = encode(scene, atlas.id, regions, pixelBytes, mapped.data());
                    }
                }
            } catch (RuntimeException failure) {
                if (readback == this) {
                    failed(atlas, failure);
                }
            }
        }
    }
}
