package dev.minecraftue.bridge;

import net.minecraft.client.model.geom.builders.UVPair;
import net.minecraft.client.renderer.block.BlockStateModelSet;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.StainedGlassBlock;
import net.minecraft.world.level.block.StainedGlassPaneBlock;
import net.minecraft.world.level.block.state.BlockState;
import org.joml.Vector3f;
import org.joml.Vector3fc;

import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;

/** Joins matching glass faces in the UE section mesh using the baked geometry of both blocks. */
final class ConnectedGlass {
    private static final Direction[] DIRECTIONS = Direction.values();
    private static final Direction.Axis[] AXES = Direction.Axis.values();
    private static final float GEOMETRY_EPSILON = 0.00001f;
    // Sample beyond the frame and its short painted highlights in the vanilla 16x16 sprites.
    private static final float FRAME_INSET = 5.0f / 16;

    private final BlockStateModelSet models;
    private final BlockPos.MutableBlockPos adjacent = new BlockPos.MutableBlockPos();
    private final RandomSource random = RandomSource.create(0);
    private final List<BlockStateModelPart> parts = new ArrayList<>();
    private final IdentityHashMap<BakedQuad, FaceBounds> bounds = new IdentityHashMap<>();

    ConnectedGlass(BlockStateModelSet models) {
        this.models = models;
    }

    static boolean appliesTo(BlockState state) {
        var block = state.getBlock();
        return state.is(Blocks.GLASS) || state.is(Blocks.TINTED_GLASS) || state.is(Blocks.GLASS_PANE)
                || block instanceof StainedGlassBlock || block instanceof StainedGlassPaneBlock;
    }

    BakedQuad connect(BlockGetter region, BlockPos pos, BlockState state, BakedQuad quad) {
        var material = quad.materialInfo();
        TextureAtlasSprite sprite = material.sprite();
        if (material.layer() != ChunkSectionLayer.TRANSLUCENT || sprite.isAnimated() || state.hasOffsetFunction()) {
            return quad;
        }
        var name = sprite.contents().name();
        if (!name.getNamespace().equals("minecraft")) {
            return quad;
        }
        String path = name.getPath();
        boolean paneEdge = path.equals("block/glass_pane_top") || path.endsWith("_stained_glass_pane_top");
        if (!paneEdge && !path.equals("block/glass") && !path.equals("block/tinted_glass")
                && !path.endsWith("_stained_glass")) {
            return quad;
        }
        FaceBounds faceBounds = bounds.computeIfAbsent(quad, FaceBounds::of);
        if (faceBounds == null) {
            return quad;
        }

        Direction face = quad.direction();
        // Matching caps are interior, including the individual arms of panes stacked vertically.
        if (faceBounds.touches(face) && hasMatchingFace(region, pos, state, faceBounds, face, face.getOpposite(), sprite)) {
            return null;
        }
        if (paneEdge) {
            return quad;
        }

        long[] uv = null;
        for (Direction edge : DIRECTIONS) {
            if (edge.getAxis() != face.getAxis() && faceBounds.touches(edge)
                    && hasMatchingFace(region, pos, state, faceBounds, edge, face, sprite)) {
                if (uv == null) {
                    uv = new long[] {quad.packedUV0(), quad.packedUV1(), quad.packedUV2(), quad.packedUV3()};
                }
                trimEdge(quad, sprite, edge, uv);
            }
        }
        if (uv == null || (quad.packedUV0() == uv[0] && quad.packedUV1() == uv[1]
                && quad.packedUV2() == uv[2] && quad.packedUV3() == uv[3])) {
            return quad;
        }
        return new BakedQuad(quad.position0(), quad.position1(), quad.position2(), quad.position3(),
                uv[0], uv[1], uv[2], uv[3], face, material);
    }

    private boolean hasMatchingFace(BlockGetter region, BlockPos pos, BlockState state, FaceBounds faceBounds,
                                    Direction offset, Direction expected, TextureAtlasSprite sprite) {
        adjacent.setWithOffset(pos, offset);
        BlockState neighbor = region.getBlockState(adjacent);
        if (!neighbor.is(state.getBlock())) {
            return false;
        }
        parts.clear();
        random.setSeed(neighbor.getSeed(adjacent));
        models.get(neighbor).collectParts(random, parts);
        for (BlockStateModelPart part : parts) {
            if (matches(part.getQuads(expected), faceBounds, offset, expected, sprite)
                    || matches(part.getQuads(null), faceBounds, offset, expected, sprite)) {
                return true;
            }
        }
        return false;
    }

    private boolean matches(List<BakedQuad> faces, FaceBounds faceBounds, Direction offset, Direction expected,
                            TextureAtlasSprite sprite) {
        for (BakedQuad candidate : faces) {
            if (candidate.direction() == expected && candidate.materialInfo().sprite() == sprite
                    && candidate.materialInfo().layer() == ChunkSectionLayer.TRANSLUCENT) {
                FaceBounds candidateBounds = bounds.computeIfAbsent(candidate, FaceBounds::of);
                if (candidateBounds != null && candidateBounds.touches(offset.getOpposite())
                        && faceBounds.sameAlongSeam(candidateBounds, offset.getAxis())) {
                    return true;
                }
            }
        }
        return false;
    }

    private static void trimEdge(BakedQuad quad, TextureAtlasSprite sprite, Direction edge, long[] uv) {
        int first = -1;
        int second = -1;
        int opposite = -1;
        for (int i = 0; i < BakedQuad.VERTEX_COUNT; i++) {
            if (near(quad.position(i).get(edge.getAxis().ordinal()), boundary(edge))) {
                if (first == -1) {
                    first = i;
                } else {
                    second = i;
                }
            } else {
                opposite = i;
            }
        }
        if (second == -1 || opposite == -1) {
            return;
        }

        // Baked UVs can be rotated or mirrored. Only move the coordinate constant along this edge.
        long a = quad.packedUV(first);
        long b = quad.packedUV(second);
        long far = quad.packedUV(opposite);
        float u = UVPair.unpackU(a);
        float v = UVPair.unpackV(a);
        if (u == UVPair.unpackU(b) && v != UVPair.unpackV(b)) {
            float inset = trimCoordinate(u, UVPair.unpackU(far), sprite.getU0(), sprite.getU1());
            uv[first] = UVPair.pack(inset, UVPair.unpackV(uv[first]));
            uv[second] = UVPair.pack(inset, UVPair.unpackV(uv[second]));
        } else if (v == UVPair.unpackV(b) && u != UVPair.unpackU(b)) {
            float inset = trimCoordinate(v, UVPair.unpackV(far), sprite.getV0(), sprite.getV1());
            uv[first] = UVPair.pack(UVPair.unpackU(uv[first]), inset);
            uv[second] = UVPair.pack(UVPair.unpackU(uv[second]), inset);
        }
    }

    private static float trimCoordinate(float value, float opposite, float start, float end) {
        float inset = (end - start) * FRAME_INSET;
        float target = Math.clamp(value, start + inset, end - inset);
        // Keep short, cropped UV rectangles from collapsing or changing orientation.
        float limit = value + (opposite - value) * 0.75f;
        return Math.clamp(target, Math.min(value, limit), Math.max(value, limit));
    }

    private static float boundary(Direction direction) {
        return direction.getAxisDirection() == Direction.AxisDirection.POSITIVE ? 1 : 0;
    }

    private static boolean near(float a, float b) {
        return Math.abs(a - b) < GEOMETRY_EPSILON;
    }

    private record FaceBounds(Vector3fc min, Vector3fc max) {
        static FaceBounds of(BakedQuad quad) {
            var min = new Vector3f(quad.position0());
            var max = new Vector3f(min);
            for (int i = 1; i < BakedQuad.VERTEX_COUNT; i++) {
                min.min(quad.position(i));
                max.max(quad.position(i));
            }
            int normal = quad.direction().getAxis().ordinal();
            if (!near(min.get(normal), max.get(normal))) {
                return null;
            }
            int corners = 0;
            for (int i = 0; i < BakedQuad.VERTEX_COUNT; i++) {
                int corner = 0;
                for (Direction.Axis axis : AXES) {
                    int index = axis.ordinal();
                    float coordinate = quad.position(i).get(index);
                    if (!near(coordinate, min.get(index))) {
                        if (!near(coordinate, max.get(index))) {
                            return null;
                        }
                        corner |= 1 << index;
                    }
                }
                corners |= 1 << corner;
            }
            return Integer.bitCount(corners) == BakedQuad.VERTEX_COUNT ? new FaceBounds(min, max) : null;
        }

        boolean touches(Direction direction) {
            float plane = boundary(direction);
            int axis = direction.getAxis().ordinal();
            return near((plane == 0 ? min : max).get(axis), plane);
        }

        boolean sameAlongSeam(FaceBounds other, Direction.Axis seamNormal) {
            for (Direction.Axis axis : AXES) {
                int index = axis.ordinal();
                if (axis != seamNormal && (!near(min.get(index), other.min.get(index))
                        || !near(max.get(index), other.max.get(index)))) {
                    return false;
                }
            }
            return true;
        }
    }
}
