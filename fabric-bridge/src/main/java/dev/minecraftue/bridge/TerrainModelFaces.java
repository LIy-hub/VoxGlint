package dev.minecraftue.bridge;

import com.mojang.blaze3d.vertex.QuadInstance;
import net.minecraft.client.renderer.block.BlockQuadOutput;
import net.minecraft.client.renderer.block.BlockStateModelSet;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.RenderShape;
import net.minecraft.world.level.block.state.BlockState;

import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;

/** Adjust full cube faces for the bridge's masked and two-sided foliage materials. */
final class TerrainModelFaces {
    private static final Direction[] FACES = Direction.values();
    // A narrow separation between front and back surfaces, including in ray tracing.
    private static final float BACK_FACE_INSET = 1.0f / 1024;
    private final BlockStateModelSet models;
    private final IdentityHashMap<BlockStateModelPart, Boolean> shells = new IdentityHashMap<>();
    private final BlockPos.MutableBlockPos adjacent = new BlockPos.MutableBlockPos();
    private final List<BlockStateModelPart> neighborParts = new ArrayList<>();
    private final RandomSource neighborRandom = RandomSource.create(0);
    private final QuadInstance backInstance = new QuadInstance();

    TerrainModelFaces(BlockStateModelSet models) {
        this.models = models;
    }

    void put(List<BlockStateModelPart> parts, BlockQuadOutput output, float x, float y, float z,
             BakedQuad quad, QuadInstance instance, BlockGetter region, BlockPos pos, BlockState state, boolean foliage) {
        Direction face = quad.direction();
        boolean addBack = false;
        if ((!foliage || face.getAxisDirection() == Direction.AxisDirection.NEGATIVE)
                && !state.hasOffsetFunction() && parts.size() == 1
                && shells.computeIfAbsent(parts.getFirst(), TerrainModelFaces::isCubeShell)) {
            adjacent.setWithOffset(pos, face);
            BlockState neighbor = region.getBlockState(adjacent);
            if (foliage) {
                // The positive-facing leaf owns the shared plane, even across section boundaries.
                if (SectionCapture.isFoliage(neighbor) && hasNeighborFace(state, neighbor, face)) {
                    return;
                }
            } else {
                addBack = !hasNeighborFace(state, neighbor, face);
            }
        }

        output.put(x, y, z, quad, instance);
        if (addBack) {
            // Reuse authored UVs and the light/tint at each corner while reversing the winding.
            BakedQuad back = new BakedQuad(quad.position0(), quad.position3(), quad.position2(), quad.position1(),
                    quad.packedUV0(), quad.packedUV3(), quad.packedUV2(), quad.packedUV1(),
                    face.getOpposite(), quad.materialInfo());
            for (int vertex = 0; vertex < BakedQuad.VERTEX_COUNT; ++vertex) {
                int source = (BakedQuad.VERTEX_COUNT - vertex) % BakedQuad.VERTEX_COUNT;
                backInstance.setColor(vertex, instance.getColor(source));
                backInstance.setLightCoords(vertex, instance.getLightCoords(source));
            }
            backInstance.setOverlayCoords(instance.overlayCoords());
            output.put(x - face.getStepX() * BACK_FACE_INSET, y - face.getStepY() * BACK_FACE_INSET,
                    z - face.getStepZ() * BACK_FACE_INSET, back, backInstance);
        }
    }

    private boolean hasNeighborFace(BlockState state, BlockState neighbor, Direction face) {
        if (neighbor.getRenderShape() != RenderShape.MODEL || neighbor.hasOffsetFunction()) {
            return false;
        }
        Direction opposite = face.getOpposite();
        boolean renderCulledFaces = Block.shouldRenderFace(neighbor, state, opposite);
        neighborParts.clear();
        neighborRandom.setSeed(neighbor.getSeed(adjacent));
        models.get(neighbor).collectParts(neighborRandom, neighborParts);
        for (BlockStateModelPart part : neighborParts) {
            if ((renderCulledFaces && hasFullFace(part.getQuads(opposite), opposite))
                    || hasFullFace(part.getQuads(null), opposite)) {
                return true;
            }
        }
        return false;
    }

    private static boolean isCubeShell(BlockStateModelPart part) {
        // Models with authored inside faces or multiple layers keep their baked geometry.
        if (!part.getQuads(null).isEmpty()) {
            return false;
        }
        for (Direction face : FACES) {
            List<BakedQuad> quads = part.getQuads(face);
            if (quads.size() != 1 || !spansBlockFace(quads.getFirst(), face)) {
                return false;
            }
        }
        return true;
    }

    private static boolean hasFullFace(List<BakedQuad> quads, Direction face) {
        for (BakedQuad quad : quads) {
            if (spansBlockFace(quad, face)) {
                return true;
            }
        }
        return false;
    }

    private static boolean spansBlockFace(BakedQuad quad, Direction face) {
        if (quad.direction() != face) {
            return false;
        }
        int plane = face.getAxisDirection() == Direction.AxisDirection.POSITIVE ? 1 : 0;
        int corners = 0;
        for (int vertex = 0; vertex < BakedQuad.VERTEX_COUNT; ++vertex) {
            var p = quad.position(vertex);
            if ((p.x() != 0 && p.x() != 1) || (p.y() != 0 && p.y() != 1) || (p.z() != 0 && p.z() != 1)
                    || face.getAxis().choose(p.x(), p.y(), p.z()) != plane) {
                return false;
            }
            corners |= 1 << ((int) p.x() * 4 + (int) p.y() * 2 + (int) p.z());
        }
        return Integer.bitCount(corners) == BakedQuad.VERTEX_COUNT;
    }
}
