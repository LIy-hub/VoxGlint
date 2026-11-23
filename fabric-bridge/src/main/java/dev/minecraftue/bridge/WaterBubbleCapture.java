package dev.minecraftue.bridge;

import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.BubbleColumnBlock;
import net.minecraft.world.level.chunk.LevelChunk;

import java.nio.ByteBuffer;
import java.util.Arrays;

final class WaterBubbleCapture {
    static final int MAX_RUNS = 4096;
    static final int HEADER_BYTES = 16;
    static final int RUN_BYTES = 6;

    private WaterBubbleCapture() {}

    static byte[] sample(ClientLevel level, LevelChunk chunk) {
        var sections = chunk.getSections();
        boolean[] containsBubble = new boolean[sections.length];
        boolean any = false;
        for (int i = 0; i < sections.length; i++) {
            containsBubble[i] = sections[i].maybeHas(state -> state.is(Blocks.BUBBLE_COLUMN));
            any |= containsBubble[i];
        }
        if (!any) return new byte[0];

        ByteBuffer runs = ByteBuffer.allocate(MAX_RUNS * RUN_BYTES);
        BlockPos.MutableBlockPos block = new BlockPos.MutableBlockPos();
        int minY = level.getMinY();
        int height = level.getHeight();
        int baseX = chunk.getPos().getMinBlockX();
        int baseZ = chunk.getPos().getMinBlockZ();
        for (int z = 0; z < 16; ++z) {
            for (int x = 0; x < 16; ++x) {
                int start = 0;
                int flow = -1;
                for (int y = 0; y <= height; ++y) {
                    int next = -1;
                    if (y < height && containsBubble[y >> 4]) {
                        var state = chunk.getBlockState(block.set(baseX + x, minY + y, baseZ + z));
                        if (state.is(Blocks.BUBBLE_COLUMN)) {
                            next = state.getValue(BubbleColumnBlock.DRAG_DOWN) ? 1 : 0;
                        }
                    }
                    if (next != flow) {
                        if (flow >= 0) {
                            runs.put((byte) (x | (z << 4))).put((byte) flow)
                                    .putShort((short) start).putShort((short) (y - start));
                            if (!runs.hasRemaining()) return runs.array();
                        }
                        flow = next;
                        start = y;
                    }
                    if (y < height && flow < 0 && !containsBubble[y >> 4]) y |= 15;
                }
            }
        }
        return Arrays.copyOf(runs.array(), runs.position());
    }

    static boolean send(LocalTransport.Connection connection, int scene, int x, int z, byte[] runs) {
        return connection.sendTerrain(BridgeWire.frame(BridgeWire.CHUNK_WATER_BUBBLES, HEADER_BYTES + runs.length)
                .putInt(scene).putInt(x).putInt(z).putShort((short) (runs.length / RUN_BYTES)).putShort((short) 0)
                .put(runs).flip());
    }
}
