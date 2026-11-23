package dev.minecraftue.bridge;

import com.mojang.blaze3d.platform.NativeImage;
import dev.minecraftue.bridge.mixin.SpriteContentsAccessor;
import dev.minecraftue.bridge.mixin.TextureAtlasSpriteAccessor;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.resources.Identifier;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;

final class TerrainAtlas {
    static final int[] TEXTURES = {BridgeWire.ATLAS_ROWS, BridgeWire.NORMAL_ATLAS_ROWS, BridgeWire.PBR_ATLAS_ROWS};

    private TerrainAtlas() {}

    static ByteBuffer rowHeader(int type, int mip, int mipCount, int width, int height, int firstRow, int rows) {
        return BridgeWire.frame(type, BridgeWire.ATLAS_HEADER_BYTES + width * rows * Integer.BYTES)
                .putShort((short) mip).putShort((short) mipCount).putShort((short) width).putShort((short) height)
                .putShort((short) firstRow).putShort((short) rows);
    }

    static ByteBuffer encodeRows(int type, int mip, int mipCount, int width, int height, int firstRow, int rows,
                                 List<Sprite> sprites, TerrainMaterialAtlas materials) {
        ByteBuffer frame = rowHeader(type, mip, mipCount, width, height, firstRow, rows);
        int pixelStart = frame.position();
        frame.order(ByteOrder.LITTLE_ENDIAN);
        boolean albedo = type == BridgeWire.ATLAS_ROWS;
        if (!albedo) {
            int fill = materials.fill(type);
            for (int offset = pixelStart; offset < frame.capacity(); offset += Integer.BYTES) {
                frame.putInt(offset, fill);
            }
        }
        // Fill the packet directly from Minecraft's generated sprite images, including cutout alpha coverage.
        for (int i = 0; i < sprites.size(); i++) {
            sprites.get(i).writeRows(frame, pixelStart, width, mip, firstRow, firstRow + rows,
                    albedo ? null : materials.tile(type, i));
        }
        return frame.order(ByteOrder.BIG_ENDIAN).position(frame.capacity()).flip();
    }

    record Sprite(Identifier name, NativeImage[] images, int x, int y, int width, int height, int padding,
                  int frameX, int frameY, int[] frames) {
        static Sprite from(TextureAtlasSprite sprite) {
            var contents = sprite.contents();
            NativeImage[] images = ((SpriteContentsAccessor) contents).minecraftUe$getMipImages();
            // The same first valid frame is used in every level, even when metadata starts at a nonzero index.
            int[] frames = contents.isAnimated() ? contents.getUniqueFrames().toIntArray() : new int[] {0};
            int frame = frames[0];
            int framesPerRow = images[0].getWidth() / contents.width();
            return new Sprite(contents.name(), images, sprite.getX(), sprite.getY(), contents.width(), contents.height(),
                    ((TextureAtlasSpriteAccessor) sprite).minecraftUe$getPadding(),
                    frame % framesPerRow * contents.width(), frame / framesPerRow * contents.height(), frames);
        }

        Sprite(Identifier name, NativeImage[] images, int x, int y, int width, int height, int padding,
               int frameX, int frameY) {
            this(name, images, x, y, width, height, padding, frameX, frameY,
                    new int[] {(frameY / height) * (images[0].getWidth() / width) + frameX / width});
        }

        int frameX(int frame) {
            return frame % (images[0].getWidth() / width) * width;
        }

        int frameY(int frame) {
            return frame / (images[0].getWidth() / width) * height;
        }

        void writeRows(ByteBuffer frame, int pixelStart, int atlasWidth, int mip, int firstRow, int endRow,
                       TerrainMaterialAtlas.Tile material) {
            int x0 = x >> mip;
            int y0 = y >> mip;
            int x1 = x0 + ((width + 2 * padding) >> mip);
            int y1 = y0 + ((height + 2 * padding) >> mip);
            if (y1 <= firstRow || y0 >= endRow) {
                return;
            }

            NativeImage image = images[mip];
            int gutter = padding >> mip;
            int sourceX = frameX >> mip;
            int sourceY = frameY >> mip;
            int lastX = (width >> mip) - 1;
            int lastY = (height >> mip) - 1;
            for (int row = Math.max(y0, firstRow); row < Math.min(y1, endRow); row++) {
                int offset = pixelStart + ((row - firstRow) * atlasWidth + x0) * Integer.BYTES;
                int sampleY = Math.clamp(row - y0 - gutter, 0, lastY);
                for (int column = x0; column < x1; column++) {
                    int sampleX = Math.clamp(column - x0 - gutter, 0, lastX);
                    // NativeImage exposes ARGB ints. Little-endian storage writes BGRA bytes.
                    int pixel = material == null ? image.getPixel(sourceX + sampleX, sourceY + sampleY)
                            : material.sample(mip, sampleY * (lastX + 1) + sampleX);
                    frame.putInt(offset, pixel);
                    offset += Integer.BYTES;
                }
            }
        }
    }
}
