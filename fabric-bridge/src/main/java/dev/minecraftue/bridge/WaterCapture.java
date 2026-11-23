package dev.minecraftue.bridge;

import it.unimi.dsi.fastutil.longs.Long2ObjectOpenHashMap;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.tags.BlockTags;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.StainedGlassPaneBlock;
import net.minecraft.world.level.block.TransparentBlock;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FlowingFluid;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

/** Associates vanilla fluid vertices with their source cells before translucent sorting. */
final class WaterCapture {
    static final int TOP = 0, OPEN_SIDE = 1, TRANSPARENT = 2, CLOSED = 3;
    static final int STILL = 0, FLOW = 1, FALL = 2;
    static final int EXPOSED_SKY_LIGHT = 1, INCOMPLETE_DROP = 2;
    private static final Direction[] HORIZONTAL = {Direction.WEST, Direction.EAST, Direction.NORTH, Direction.SOUTH};
    private static final float EPSILON = 0.003f;
    private BlockGetter region;
    private final Long2ObjectOpenHashMap<float[]> heights = new Long2ObjectOpenHashMap<>();
    private final Long2ObjectOpenHashMap<Cell> cells = new Long2ObjectOpenHashMap<>();

    record Run(int firstQuad, Cell cell) {}

    record Segment(Vec3 a, Vec3 b) {
        void write(ByteBuffer target, int bucket, int quad) {
            target.put((byte) bucket).putInt(quad);
            for (Vec3 v : List.of(a, b)) {
                target.putShort(fixed(v.x)).putShort(fixed(v.y)).putShort(fixed(v.z));
            }
        }

        private static short fixed(double value) {
            return (short) Math.round(value * 64);
        }
    }

    record Cell(BlockPos pos, FluidState fluid, boolean falling, Vec3 flow, float strength,
                float turbulence, int impact, int drop, int fromLip, int toToe, boolean incomplete, float[] corners) {}

    void useRegion(BlockGetter level) {
        region = level;
    }

    Cell cell(BlockPos pos) {
        long key = pos.asLong();
        Cell cached = cells.get(key);
        if (cached != null) return cached;
        FluidState fluid = region.getFluidState(pos);
        Vec3 flow = fluid.getFlow(region, pos);
        double length = flow.horizontalDistance();
        flow = length > 1e-6 ? new Vec3(flow.x / length, 0, flow.z / length) : Vec3.ZERO;
        boolean falling = falling(fluid);
        float strength = 0;
        float turbulence = 0;
        float ownHeight = fluid.getOwnHeight();
        if (length > 1e-6) {
            for (Direction side : HORIZONTAL) {
                float along = (float) (flow.x * side.getStepX() + flow.z * side.getStepZ());
                if (along <= 0.05f) continue;
                BlockPos neighborPos = pos.relative(side);
                BlockState neighbor = region.getBlockState(neighborPos);
                FluidState next = neighbor.getFluidState();
                if (next.is(FluidTags.WATER)) {
                    strength += along * Math.max(0, ownHeight - next.getOwnHeight()) * 2.3f;
                    Vec3 nextFlow = next.getFlow(region, neighborPos);
                    if (nextFlow.horizontalDistanceSqr() > 0.01) {
                        double alignment = flow.dot(nextFlow) / nextFlow.horizontalDistance();
                        turbulence = Math.max(turbulence, along * (float) Math.max(0, 0.75 - alignment));
                    }
                } else if (neighbor.getCollisionShape(region, neighborPos).isEmpty()) {
                    strength += along * ownHeight;
                } else {
                    turbulence = Math.max(turbulence, along * 0.85f);
                }
            }
        }
        int drop = 0;
        int fromLip = 0;
        int toToe = 0;
        boolean incomplete = false;
        // A source or spreading cell can feed the very same jet without having FALLING set.
        // Give its lip the drop length too, including before the column grid reaches the viewer.
        if (falling || falling(region.getFluidState(pos.below()))) {
            // The compiler owns a 3x3x3 snapshot. Never read beyond its vertical halo.
            int firstY = (pos.getY() & ~15) - 16;
            int lastY = (pos.getY() & ~15) + 31;
            int bottom = falling ? pos.getY() : pos.getY() - 1;
            int top = bottom;
            while (bottom > firstY && falling(region.getFluidState(pos.atY(bottom - 1)))) --bottom;
            while (top < lastY && falling(region.getFluidState(pos.atY(top + 1)))) ++top;
            drop = top - bottom + 1;
            if (falling) {
                fromLip = top - pos.getY();
                toToe = pos.getY() - bottom;
            }
            incomplete = bottom == firstY || top == lastY;
        }
        int impact = falling ? 0 : directImpact(pos);
        if (!falling && length > 1e-6) {
            Direction upstream = Math.abs(flow.x) >= Math.abs(flow.z)
                    ? (flow.x > 0 ? Direction.WEST : Direction.EAST)
                    : (flow.z > 0 ? Direction.NORTH : Direction.SOUTH);
            BlockPos next = pos;
            for (int step = 1; step <= 3; ++step) {
                next = next.relative(upstream);
                FluidState previous = region.getFluidState(next);
                if (!previous.is(FluidTags.WATER) || falling(previous)
                        || Math.abs(previous.getOwnHeight() - ownHeight) > 0.46f
                        || region.getFluidState(next.above()).is(FluidTags.WATER)) break;
                Vec3 previousFlow = previous.getFlow(region, next);
                if (previousFlow.horizontalDistanceSqr() > 0.01 && flow.dot(previousFlow) < 0.1) break;
                impact = Math.max(impact, directImpact(next) * (4 - step) / 6);
            }
        }
        Cell result = new Cell(pos.immutable(), fluid, falling, flow, Math.clamp(strength, 0, 1),
                Math.clamp(turbulence * strength, 0, 1), impact, drop, fromLip, toToe, incomplete, corners(region, pos, fluid));
        cells.put(key, result);
        return result;
    }

    private int directImpact(BlockPos pos) {
        if (falling(region.getFluidState(pos.above())) || falling(region.getFluidState(pos.above(2)))) {
            return 255;
        }
        for (Direction side : HORIZONTAL) {
            BlockPos incoming = pos.relative(side);
            if (falling(region.getFluidState(incoming)) && falling(region.getFluidState(incoming.above()))) {
                return 180;
            }
        }
        return 0;
    }

    private static boolean falling(FluidState state) {
        return state.is(FluidTags.WATER) && state.hasProperty(FlowingFluid.FALLING)
                && state.getValue(FlowingFluid.FALLING);
    }

    private float[] contactCorners(BlockPos pos, FluidState fluid) {
        Cell known = cells.get(pos.asLong());
        return known != null ? known.corners() :
                heights.computeIfAbsent(pos.asLong(), key -> corners(region, pos, fluid));
    }

    static float[] corners(BlockGetter level, BlockPos pos, FluidState fluid) {
        float center = fluid.getHeight(level, pos);
        if (center >= 1) return new float[]{1, 1, 1, 1};
        float west = height(level, pos.west());
        float east = height(level, pos.east());
        float north = height(level, pos.north());
        float south = height(level, pos.south());
        return new float[] {
                corner(level, pos.north().west(), center, west, north),
                corner(level, pos.north().east(), center, east, north),
                corner(level, pos.south().west(), center, west, south),
                corner(level, pos.south().east(), center, east, south)
        };
    }

    private static float height(BlockGetter level, BlockPos pos) {
        BlockState block = level.getBlockState(pos);
        FluidState fluid = block.getFluidState();
        return fluid.is(FluidTags.WATER) ? fluid.getHeight(level, pos) : block.isSolid() ? -1 : 0;
    }

    private static float corner(BlockGetter level, BlockPos diagonal, float center, float a, float b) {
        if (a >= 1 || b >= 1) return 1;
        float c = a > 0 || b > 0 ? height(level, diagonal) : -1;
        if (c >= 1) return 1;
        return (weighted(center) + weighted(a) + weighted(b) + weighted(c))
                / (weight(center) + weight(a) + weight(b) + weight(c));
    }

    private static int weight(float height) {
        return height >= 0.8f ? 10 : height >= 0 ? 1 : 0;
    }

    private static float weighted(float height) {
        return height * weight(height);
    }

    private static boolean glass(BlockState state) {
        return state.getBlock() instanceof TransparentBlock
                || state.getBlock() instanceof StainedGlassPaneBlock
                || state.is(Blocks.GLASS_PANE) || state.is(Blocks.TINTED_GLASS);
    }

    private boolean coversEdge(BlockPos pos, Direction side, double along, double h, boolean requireGlass) {
        BlockPos neighbor = pos.relative(side);
        BlockState state = region.getBlockState(neighbor);
        if (requireGlass && !glass(state)) return false;
        double x = side.getStepX() == 0 ? along : side.getStepX() > 0 ? EPSILON : 1 - EPSILON;
        double z = side.getStepZ() == 0 ? along : side.getStepZ() > 0 ? EPSILON : 1 - EPSILON;
        var shape = state.getCollisionShape(region, neighbor);
        if (shape.isEmpty()) return false;
        for (AABB box : shape.toAabbs()) {
            if (x >= box.minX && x <= box.maxX && z >= box.minZ && z <= box.maxZ
                    && h >= box.minY - EPSILON && h <= box.maxY + EPSILON) return true;
        }
        return false;
    }

    void writeFace(ByteBuffer target, Cell cell, Vec3[] vertices, int skyLight) {
        BlockPos pos = cell.pos();
        int x = pos.getX() & 15, y = pos.getY() & 15, z = pos.getZ() & 15;
        int role = TOP;
        Direction side = null;
        for (Direction direction : HORIZONTAL) {
            double plane = direction.getStepX() != 0
                    ? x + Math.max(0, direction.getStepX()) : z + Math.max(0, direction.getStepZ());
            boolean onPlane = true;
            for (Vec3 v : vertices) {
                double axis = direction.getStepX() != 0 ? v.x : v.z;
                onPlane &= Math.abs(axis - plane) < EPSILON;
            }
            if (onPlane) {
                side = direction;
                break;
            }
        }
        if (side != null) {
            double midpoint = (vertices[0].y + vertices[1].y + vertices[2].y + vertices[3].y) * 0.25 - y;
            role = coversEdge(pos, side, 0.125, midpoint, true) && coversEdge(pos, side, 0.875, midpoint, true)
                    ? TRANSPARENT : OPEN_SIDE;
        } else if (vertices[0].y - y < EPSILON && vertices[1].y - y < EPSILON
                && vertices[2].y - y < EPSILON && vertices[3].y - y < EPSILON) {
            role = CLOSED;
        }
        float[] corners = cell.corners().clone();
        if (role == TOP) {
            for (Vec3 v : vertices) {
                int index = (v.x - x > 0.5 ? 1 : 0) | (v.z - z > 0.5 ? 2 : 0);
                corners[index] = (float) (v.y - y);
            }
        }
        int edges = 0;
        int[] starts = {0, 1, 0, 2};
        int[] ends = {2, 3, 1, 3};
        for (int i = 0; i < HORIZONTAL.length; i++) {
            float h = (corners[starts[i]] + corners[ends[i]]) * 0.5f;
            if (coversEdge(pos, HORIZONTAL[i], 0.5, h, false)) edges |= 1 << i;
        }
        int motion = cell.falling() ? FALL : cell.flow() == Vec3.ZERO ? STILL : FLOW;
        int flags = role | (motion << 2) | (cell.fluid().isSource() ? 16 : 0)
                | (cell.falling() ? 32 : 0);
        target.put((byte) flags).put((byte) Math.clamp(cell.fluid().getAmount(), 0, 255))
                .put((byte) Math.round(cell.flow().x * 127)).put((byte) Math.round(cell.flow().z * 127))
                .put(unit(cell.strength())).put(unit(cell.turbulence()))
                .put((byte) ((skyLight == 15 ? EXPOSED_SKY_LIGHT : 0) | (cell.incomplete() ? INCOMPLETE_DROP : 0)))
                .put((byte) edges);
        for (float height : corners) target.put((byte) Math.round(Math.clamp(height, 0, 1) * 64));
        target.put((byte) cell.drop()).put((byte) (role == TOP ? cell.impact() : 0))
                .put((byte) cell.fromLip()).put((byte) cell.toToe());
    }

    private static byte unit(float value) {
        return (byte) Math.round(Math.clamp(value, 0, 1) * 255);
    }

    /**
     * Intersect a visible model face with the appropriate free water surface along its wet side.
     * Above a waterline, the continuation can extend into the next block.
     */
    List<Segment> contacts(BlockPos base, Vec3[] face, int faceIndex) {
        if (faceIndex == BridgeWire.GEOMETRIC_NORMAL || faceIndex == Direction.DOWN.get3DDataValue()) return List.of();
        Direction direction = Direction.from3DDataValue(faceIndex);
        if (direction == Direction.UP) return horizontalContacts(base, face);
        boolean constantX = direction.getAxis() == Direction.Axis.X;
        int sign = constantX ? direction.getStepX() : direction.getStepZ();
        double plane = constantX ? face[0].x : face[0].z;
        double minT = Double.POSITIVE_INFINITY, maxT = Double.NEGATIVE_INFINITY;
        double minY = Double.POSITIVE_INFINITY, maxY = Double.NEGATIVE_INFINITY;
        for (Vec3 vertex : face) {
            double along = constantX ? vertex.z : vertex.x;
            minT = Math.min(minT, along);
            maxT = Math.max(maxT, along);
            minY = Math.min(minY, vertex.y);
            maxY = Math.max(maxY, vertex.y);
        }
        if (plane < -1 || plane > 17 || minT < -1 || maxT > 17 || minY < -1 || maxY > 17
                || maxT - minT < 1.0 / 64 || maxY - minY < EPSILON) return List.of();
        int normalCell = (int) Math.floor(plane + sign * EPSILON);
        var result = new ArrayList<Segment>(2);
        for (int height = (int) Math.floor(maxY - EPSILON); height >= Math.floor(minY - 0.37); height--) {
            for (int t = (int) Math.floor(minT + EPSILON); t <= Math.floor(maxT - EPSILON); t++) {
                int lx = constantX ? normalCell : t, lz = constantX ? t : normalCell;
                BlockPos p = base.offset(lx, height, lz);
                FluidState fluid = region.getFluidState(p);
                if (!fluid.is(FluidTags.WATER) || region.getFluidState(p.above()).is(FluidTags.WATER)) continue;
                float[] h = contactCorners(p, fluid);
                double t0 = Math.max(minT, t), t1 = Math.min(maxT, t + 1);
                double across = Math.clamp(plane - normalCell, 0, 1);
                double y0 = surfaceHeight(h, constantX ? across : t0 - t, constantX ? t0 - t : across) + height;
                double y1 = surfaceHeight(h, constantX ? across : t1 - t, constantX ? t1 - t : across) + height;
                if (Math.max(y0, y1) < minY - 0.37 || Math.min(y0, y1) > maxY + 0.02) continue;
                if (Math.round(t0 * 64) == Math.round(t1 * 64)) continue;
                result.add(new Segment(constantX ? new Vec3(plane, y0, t0) : new Vec3(t0, y0, plane),
                        constantX ? new Vec3(plane, y1, t1) : new Vec3(t1, y1, plane)));
                if (result.size() == BridgeWire.MAX_SHORE_SEGMENTS_PER_QUAD) return result;
            }
        }
        return result;
    }

    private List<Segment> horizontalContacts(BlockPos base, Vec3[] face) {
        double y = face[0].y;
        for (Vec3 v : face) {
            if (v.x < -1 || v.x > 17 || v.z < -1 || v.z > 17 || y < -1 || y > 17) return List.of();
        }
        var contacts = new ArrayList<Segment>(2);
        for (int edge = 0; edge < 4; edge++) {
            Vec3 a = face[edge], b = face[(edge + 1) & 3];
            double dx = b.x - a.x, dz = b.z - a.z;
            if ((Math.abs(dx) < 1.0 / 64) == (Math.abs(dz) < 1.0 / 64)) continue;
            Vec3 mid = a.lerp(b, 0.5).add(-Math.signum(dz) * EPSILON, 0, Math.signum(dx) * EPSILON);
            int x = (int) Math.floor(mid.x), z = (int) Math.floor(mid.z), h = (int) Math.floor(y + EPSILON);
            for (int layer = h; layer >= h - 1; layer--) {
                BlockPos p = base.offset(x, layer, z);
                FluidState fluid = region.getFluidState(p);
                if (!fluid.is(FluidTags.WATER) || region.getFluidState(p.above()).is(FluidTags.WATER)) continue;
                float[] heights = contactCorners(p, fluid);
                double ya = layer + surfaceHeight(heights, a.x - x, a.z - z);
                double yb = layer + surfaceHeight(heights, b.x - x, b.z - z);
                if (Math.max(ya, yb) >= y - 0.35 && Math.min(ya, yb) <= y + 0.025) {
                    contacts.add(new Segment(new Vec3(a.x, ya, a.z), new Vec3(b.x, yb, b.z)));
                    break;
                }
            }
            if (contacts.size() == BridgeWire.MAX_SHORE_SEGMENTS_PER_QUAD) break;
        }
        return contacts;
    }

    static double surfaceHeight(float[] h, double x, double z) {
        x = Math.clamp(x, 0, 1);
        z = Math.clamp(z, 0, 1);
        return x >= z ? h[0] + x * (h[1] - h[0]) + z * (h[3] - h[1])
                : h[0] + z * (h[2] - h[0]) + x * (h[3] - h[2]);
    }

    static boolean isIce(BlockState block) {
        return block.is(BlockTags.ICE);
    }
}

