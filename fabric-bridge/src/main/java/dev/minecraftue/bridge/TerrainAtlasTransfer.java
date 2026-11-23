package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.resources.Identifier;
import org.slf4j.Logger;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.nio.ByteBuffer;
import java.util.Map;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

final class TerrainAtlasTransfer implements AutoCloseable {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final int MAX_PACKETS_PER_TICK = 8;
    private static final long CACHE_REPLY_TIMEOUT_NS = TimeUnit.SECONDS.toNanos(8);
    private static final long CACHE_PREPARE_TIMEOUT_NS = TimeUnit.SECONDS.toNanos(60);
    private static final Executor ROW_READER = Executors.newSingleThreadExecutor(
            task -> Thread.ofPlatform().daemon().name("Minecraft UE atlas row transfer").unstarted(task));

    private final TerrainAtlasCache.Entry entry;
    private final ArrayBlockingQueue<ByteBuffer> pendingRows = new ArrayBlockingQueue<>(MAX_PACKETS_PER_TICK);
    private long offeredAt;
    private boolean rowTransfer;
    private volatile boolean closed;
    private volatile boolean rowsRead;
    private volatile IOException readFailure;

    TerrainAtlasTransfer(TextureAtlas atlas) {
        var prepared = ((TerrainAtlasCache.Prepared) atlas).minecraftUe$getPreparedAtlas();
        if (prepared == null) {
            throw new IllegalStateException("No prepared block atlas for this resource generation");
        }
        var texture = atlas.getTexture();
        if (texture.getWidth(0) != prepared.size.width() || texture.getHeight(0) != prepared.size.height()
                || texture.getMipLevels() != prepared.size.mips()) {
            throw new IllegalStateException("Prepared block atlas dimensions differ from the current texture");
        }
        entry = prepared.retain();
    }

    Map<Identifier, TerrainEmission.Color> emissionColors() {
        return entry.emissionColors;
    }

    boolean flush(LocalTransport.Connection connection, int scene) {
        if (!rowTransfer) {
            if (!entry.shared) {
                startRows(scene);
            } else if (offeredAt == 0) {
                var request = BridgeWire.frame(BridgeWire.ATLAS_REFERENCE,
                                4 + TerrainAtlasCache.KEY_BYTES + TerrainAtlasCache.HASH_BYTES)
                        .putInt(scene).put(entry.key).put(entry.checksum).flip();
                if (connection.sendTerrain(request)) {
                    offeredAt = System.nanoTime();
                }
                return false;
            } else {
                long reply = connection.atlasReply;
                int status = (int) (reply & 255);
                boolean matchingScene = (int) (reply >>> 8) == scene;
                if (matchingScene && status == BridgeWire.ATLAS_CACHE_READY) {
                    return connection.sendTerrain(BridgeWire.frame(BridgeWire.ATLAS_USE, 4).putInt(scene).flip());
                }
                long timeout = matchingScene && status == BridgeWire.ATLAS_CACHE_PREPARING
                        ? CACHE_PREPARE_TIMEOUT_NS : CACHE_REPLY_TIMEOUT_NS;
                if ((matchingScene && status == BridgeWire.ATLAS_CACHE_REJECTED)
                        || System.nanoTime() - offeredAt >= timeout) {
                    startRows(scene);
                } else {
                    return false;
                }
            }
        }

        if (readFailure != null) {
            throw new UncheckedIOException("Could not stream terrain atlas fallback", readFailure);
        }
        for (int i = 0; i < MAX_PACKETS_PER_TICK && connection.canQueueTerrain(); i++) {
            var frame = pendingRows.peek();
            if (frame == null || !connection.sendTerrain(frame)) {
                break;
            }
            pendingRows.remove();
        }
        return rowsRead && pendingRows.isEmpty();
    }

    private void startRows(int scene) {
        rowTransfer = true;
        var data = entry.retain();
        LOGGER.info("Minecraft UE terrain atlas row fallback scene={}", scene);
        ROW_READER.execute(() -> {
            try (data) {
                streamRows(data);
            } catch (IOException failure) {
                readFailure = failure;
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            } finally {
                rowsRead = true;
            }
        });
    }

    private void streamRows(TerrainAtlasCache.Entry data) throws IOException, InterruptedException {
        var size = data.size;
        long offset = TerrainAtlasCache.HEADER_BYTES;
        for (int type : TerrainAtlas.TEXTURES) {
            for (int mip = 0; mip < size.mips(); mip++) {
                int width = size.width() >> mip;
                int height = size.height() >> mip;
                for (int row = 0; row < height && !closed;) {
                    int rows = Math.min(height - row, BridgeWire.ATLAS_PACKET_BYTES / (width * 4));
                    var frame = TerrainAtlas.rowHeader(type, mip, size.mips(), width, height, row, rows);
                    TerrainAtlasCache.read(data.channel, frame, offset);
                    offset += (long) width * rows * 4;
                    row += rows;
                    frame.flip();
                    while (!closed && !pendingRows.offer(frame, 100, TimeUnit.MILLISECONDS)) {
                        // Stop waiting if a new scene has replaced this stream.
                    }
                }
            }
        }
    }

    @Override
    public void close() {
        closed = true;
        entry.close();
    }
}
