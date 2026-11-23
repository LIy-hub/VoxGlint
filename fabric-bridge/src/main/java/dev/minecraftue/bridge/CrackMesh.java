package dev.minecraftue.bridge;

import com.mojang.blaze3d.vertex.PoseStack;
import com.mojang.blaze3d.vertex.QuadInstance;
import com.mojang.blaze3d.vertex.SheetedDecalTextureGenerator;
import com.mojang.blaze3d.vertex.VertexConsumer;
import net.minecraft.client.renderer.block.BlockStateModelSet;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.block.state.BlockState;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

final class CrackMesh {
    private static final Direction[] DIRECTIONS = Direction.values();

    private CrackMesh() {}

    static ByteBuffer build(int scene, BlockPos pos, int sourceId, int stage,
                            BlockState state, BlockStateModelSet models) {
        var random = RandomSource.createThreadLocalInstance();
        random.setSeed(state.getSeed(pos));
        List<BlockStateModelPart> parts = new ArrayList<>();
        models.get(state).collectParts(random, parts);

        List<BakedQuad> quads = new ArrayList<>();
        for (BlockStateModelPart part : parts) {
            for (Direction direction : DIRECTIONS) {
                if (!addQuads(quads, part.getQuads(direction))) {
                    return null;
                }
            }
            if (!addQuads(quads, part.getQuads(null))) {
                return null;
            }
        }
        if (quads.isEmpty()) {
            return null;
        }

        int count = quads.size() * BakedQuad.VERTEX_COUNT;
        ByteBuffer frame = header(scene, pos, sourceId, stage,
                Integer.BYTES + count * BridgeWire.CRACK_VERTEX_BYTES).putInt(count);
        // Vanilla uses the seeded model parts and the same pose for the mesh and decal projection.
        var pose = new PoseStack();
        pose.translate(state.getOffset(pos));
        var decal = new SheetedDecalTextureGenerator(new Writer(frame), pose.last(), 1.0f);
        var quadInstance = new QuadInstance();
        for (BakedQuad quad : quads) {
            decal.putBakedQuad(pose.last(), quad, quadInstance);
        }
        return frame.flip();
    }

    private static boolean addQuads(List<BakedQuad> quads, List<BakedQuad> face) {
        if (face.size() > BridgeWire.MAX_CRACK_VERTICES / BakedQuad.VERTEX_COUNT - quads.size()) {
            return false;
        }
        quads.addAll(face);
        return true;
    }

    static ByteBuffer clear(int scene, BlockPos pos, int sourceId) {
        return header(scene, pos, sourceId, BridgeWire.CLEAR_CRACK, 0).flip();
    }

    private static ByteBuffer header(int scene, BlockPos pos, int sourceId, int stage, int meshBytes) {
        return BridgeWire.frame(BridgeWire.CRACK_UPDATE, BridgeWire.CRACK_HEADER_BYTES + meshBytes)
                .putInt(scene).putInt(sourceId).putInt(pos.getX()).putInt(pos.getY()).putInt(pos.getZ())
                .put((byte) stage);
    }

    private record Writer(ByteBuffer frame) implements VertexConsumer {
        @Override
        public VertexConsumer addVertex(float x, float y, float z) {
            frame.putFloat(x).putFloat(y).putFloat(z);
            return this;
        }

        @Override
        public VertexConsumer setUv(float u, float v) {
            frame.putFloat(u).putFloat(v);
            return this;
        }

        @Override
        public VertexConsumer setColor(int red, int green, int blue, int alpha) { return this; }

        @Override
        public VertexConsumer setColor(int color) { return this; }

        @Override
        public VertexConsumer setUv1(int u, int v) { return this; }

        @Override
        public VertexConsumer setUv2(int u, int v) { return this; }

        @Override
        public VertexConsumer setNormal(float x, float y, float z) { return this; }

        @Override
        public VertexConsumer setLineWidth(float width) { return this; }
    }
}
