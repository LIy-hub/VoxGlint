package dev.minecraftue.bridge;

import com.mojang.blaze3d.platform.NativeImage;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.client.renderer.block.BlockStateModelSet;
import net.minecraft.client.resources.model.ModelBakery;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.phys.Vec3;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Map;

final class SceneEffectsStream {
    private static final int MAX_CRACKS = 32;
    private static final double MAX_CRACK_DISTANCE_SQUARED = 32 * 32;
    private final Map<BlockPos, SentCrack> sentCracks = new HashMap<>();
    private boolean atlasSent;

    void reset() {
        sentCracks.clear();
        atlasSent = false;
    }

    void tick(LocalTransport.Connection connection, int scene, Minecraft minecraft, ClientLevel level,
              BlockStateModelSet models, Vec3 eye) {
        if (!connection.canQueueTerrain()) {
            return;
        }
        if (!atlasSent) {
            atlasSent = connection.sendTerrain(crackAtlas(scene, minecraft));
            if (!atlasSent) {
                return;
            }
        }
        flushCracks(connection, scene, level, models, eye);
    }

    private void flushCracks(LocalTransport.Connection connection, int scene, ClientLevel level,
                             BlockStateModelSet models, Vec3 eye) {
        if (sentCracks.isEmpty() && level.destructionProgress().isEmpty()) {
            return;
        }
        Map<BlockPos, Crack> current = new LinkedHashMap<>();
        // Use the same nearest-stage selection and range as LevelExtractor.
        level.destructionProgress().values().stream().filter(progress -> !progress.isEmpty())
                .map(progress -> progress.last())
                .filter(progress -> progress.getPos().distToCenterSqr(eye.x, eye.y, eye.z)
                        <= MAX_CRACK_DISTANCE_SQUARED)
                .sorted(Comparator.comparingDouble(progress -> progress.getPos().distToCenterSqr(eye.x, eye.y, eye.z)))
                .limit(MAX_CRACKS).forEach(progress -> {
                    BlockPos pos = progress.getPos();
                    BlockState state = level.getBlockState(pos);
                    if (state.getRenderShape() == RenderShape.MODEL) {
                        current.put(pos, new Crack(progress.getId(), progress.getProgress(), state));
                    }
                });

        var sent = sentCracks.entrySet().iterator();
        while (sent.hasNext()) {
            var old = sent.next();
            if (current.containsKey(old.getKey())) {
                continue;
            }
            if (old.getValue().visible() && (!connection.canQueueTerrain()
                    || !connection.sendTerrain(CrackMesh.clear(scene, old.getKey(), old.getValue().crack().sourceId())))) {
                return;
            }
            sent.remove();
        }
        for (var update : current.entrySet()) {
            BlockPos pos = update.getKey();
            Crack crack = update.getValue();
            SentCrack previous = sentCracks.get(pos);
            if (previous != null && previous.crack().equals(crack)) {
                continue;
            }
            if (!connection.canQueueTerrain()) {
                return;
            }
            ByteBuffer frame = CrackMesh.build(scene, pos, crack.sourceId(), crack.stage(), crack.state(), models);
            boolean visible = frame != null;
            if (!visible && previous != null && previous.visible()) {
                frame = CrackMesh.clear(scene, pos, previous.crack().sourceId());
            }
            if (frame != null && !connection.sendTerrain(frame)) {
                return;
            }
            sentCracks.put(pos, new SentCrack(crack, visible));
        }
    }

    private ByteBuffer crackAtlas(int scene, Minecraft minecraft) {
        int edge = BridgeWire.CRACK_TILE_EDGE;
        ByteBuffer frame = BridgeWire.frame(BridgeWire.CRACK_ATLAS,
                Integer.BYTES + BridgeWire.CRACK_STAGES * edge * edge * Integer.BYTES).putInt(scene);
        for (var location : ModelBakery.BREAKING_LOCATIONS) {
            try (var resource = minecraft.getResourceManager().open(location);
                 var image = NativeImage.read(resource)) {
                for (int y = 0; y < edge; y++) {
                    int sourceY = (2 * y + 1) * image.getHeight() / (2 * edge);
                    for (int x = 0; x < edge; x++) {
                        int sourceX = (2 * x + 1) * image.getWidth() / (2 * edge);
                        int argb = image.getPixel(sourceX, sourceY);
                        frame.put((byte) argb).put((byte) (argb >>> 8))
                                .put((byte) (argb >>> 16)).put((byte) (argb >>> 24));
                    }
                }
            } catch (IOException e) {
                throw new IllegalStateException("Could not read Minecraft crack texture " + location, e);
            }
        }
        return frame.flip();
    }

    private record Crack(int sourceId, int stage, BlockState state) {}
    private record SentCrack(Crack crack, boolean visible) {}
}
