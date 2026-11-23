package dev.minecraftue.bridge;

import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.tags.BiomeTags;
import net.minecraft.tags.FluidTags;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.levelgen.Heightmap;

import java.nio.ByteBuffer;
import java.util.BitSet;
import java.util.Arrays;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.LinkedHashSet;
import java.util.Set;

/** Samples only dirty columns. Work on large newly loaded chunks spans client ticks. */
final class WaterGridStream {
    private static final int RADIUS = 9;
    private static final int COLS = 16 * 16;
    private static final long BUDGET_NS = 650_000L;
    private static final int KNOWN = 1 << 26, PRESENT = 1 << 27, SKY = 1 << 28, ICE = 1 << 29;
    private static final int OBSTACLE = 1 << 30, SATURATED = 1 << 31;
    private static final Direction[] SIDES = {Direction.WEST, Direction.EAST, Direction.NORTH, Direction.SOUTH};

    private static final class Tile {
        final BitSet dirty = new BitSet(COLS);
        final int[] cells = new int[COLS];
        boolean changed;
        boolean sent;
        boolean[] hasWater;
        boolean bubblesDirty = true;
        byte[] bubbles = new byte[0];

        Tile() {
            dirty.set(0, COLS);
        }

        void refresh() {
            dirty.set(0, COLS);
            hasWater = null;
            bubblesDirty = true;
        }
    }

    private final Map<ChunkPos, Tile> tiles = new LinkedHashMap<>();
    private final Set<ChunkPos> invalid = new LinkedHashSet<>();
    private final Set<ChunkPos> invalidBubbles = new LinkedHashSet<>();
    private ChunkPos focus;
    private int radius;

    void reset() {
        tiles.clear();
        invalid.clear();
        invalidBubbles.clear();
        focus = null;
    }

    private void markColumn(int x, int z) {
        ChunkPos chunk = new ChunkPos(x >> 4, z >> 4);
        Tile tile = tiles.get(chunk);
        if (tile != null) {
            tile.dirty.set((z & 15) * 16 + (x & 15));
            tile.hasWater = null;
            tile.bubblesDirty = true;
        }
    }

    void blockChanged(BlockPos pos) {
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dx = -1; dx <= 1; ++dx) {
                markColumn(pos.getX() + dx, pos.getZ() + dz);
            }
        }
    }

    void chunkChanged(ChunkPos chunk, boolean dropped) {
        if (dropped) {
            Tile tile = tiles.remove(chunk);
            if (tile != null && tile.sent) invalid.add(chunk);
            if (tile != null && tile.bubbles.length > 0) invalidBubbles.add(chunk);
        } else if (focus != null && chunk.getChessboardDistance(focus) <= radius) {
            tiles.computeIfAbsent(chunk, unused -> new Tile()).refresh();
        }
        int x = chunk.getMinBlockX(), z = chunk.getMinBlockZ();
        for (int i = -1; i <= 16; ++i) {
            markColumn(x - 1, z + i);
            markColumn(x + 16, z + i);
            markColumn(x + i, z - 1);
            markColumn(x + i, z + 16);
        }
    }

    void tick(LocalTransport.Connection connection, int scene, ClientLevel level, ChunkPos center, int viewDistance) {
        if (!connection.canQueueTerrain()) return;
        long stop = System.nanoTime() + BUDGET_NS;
        int nextRadius = Math.min(RADIUS, Math.max(2, viewDistance));
        if (!center.equals(focus) || nextRadius != radius) {
            focus = center;
            radius = nextRadius;
            tiles.entrySet().removeIf(entry -> {
                if (entry.getKey().getChessboardDistance(center) <= radius) return false;
                if (entry.getValue().sent) invalid.add(entry.getKey());
                if (entry.getValue().bubbles.length > 0) invalidBubbles.add(entry.getKey());
                return true;
            });
            for (int ring = 0; ring <= radius; ++ring) {
                for (int dz = -ring; dz <= ring; ++dz) {
                    for (int dx = -ring; dx <= ring; ++dx) {
                        if (Math.max(Math.abs(dx), Math.abs(dz)) == ring) {
                            tiles.computeIfAbsent(new ChunkPos(center.x() + dx, center.z() + dz), unused -> new Tile());
                        }
                    }
                }
            }
        }

        boolean sendBubbles = connection.controlShm.supportsWaterBubbles();
        var bubbleRemoved = invalidBubbles.iterator();
        while (sendBubbles && bubbleRemoved.hasNext() && connection.canQueueTerrain() && System.nanoTime() < stop) {
            ChunkPos pos = bubbleRemoved.next();
            if (!WaterBubbleCapture.send(connection, scene, pos.x(), pos.z(), new byte[0])) return;
            bubbleRemoved.remove();
        }
        var removed = invalid.iterator();
        while (removed.hasNext() && connection.canQueueTerrain() && System.nanoTime() < stop) {
            ChunkPos pos = removed.next();
            ByteBuffer frame = BridgeWire.frame(BridgeWire.CHUNK_WATER_GRID, BridgeWire.CHUNK_WATER_GRID_BYTES)
                    .putInt(scene).putInt(pos.x()).putInt(pos.z());
            frame.position(frame.capacity());
            if (!connection.sendTerrain(frame.flip())) return;
            removed.remove();
        }

        Iterator<Map.Entry<ChunkPos, Tile>> iterator = tiles.entrySet().iterator();
        while (iterator.hasNext() && connection.canQueueTerrain() && System.nanoTime() < stop) {
            var entry = iterator.next();
            ChunkPos pos = entry.getKey();
            Tile tile = entry.getValue();
            if (tile.dirty.isEmpty() && !tile.changed && (!sendBubbles || !tile.bubblesDirty)) continue;
            LevelChunk chunk = level.getChunkSource().getChunk(pos.x(), pos.z(), ChunkStatus.FULL, false);
            if (chunk == null) continue;
            if (sendBubbles && tile.dirty.isEmpty() && tile.bubblesDirty) {
                byte[] runs = WaterBubbleCapture.sample(level, chunk);
                if (!Arrays.equals(tile.bubbles, runs) && !WaterBubbleCapture.send(connection, scene, pos.x(), pos.z(), runs))
                    return;
                tile.bubbles = runs;
                tile.bubblesDirty = false;
            }
            if (tile.hasWater == null) {
                var sections = chunk.getSections();
                tile.hasWater = new boolean[sections.length];
                for (int i = 0; i < sections.length; ++i) {
                    tile.hasWater[i] = sections[i].maybeHas(state -> state.getFluidState().is(FluidTags.WATER));
                }
                boolean anyWater = false;
                for (boolean wetSection : tile.hasWater) anyWater |= wetSection;
                if (!anyWater) {
                    for (int column = tile.dirty.nextSetBit(0); column >= 0; column = tile.dirty.nextSetBit(column + 1)) {
                        tile.changed |= tile.cells[column] != KNOWN;
                        tile.cells[column] = KNOWN;
                    }
                    tile.dirty.clear();
                }
            }
            for (int column = tile.dirty.nextSetBit(0); column >= 0; column = tile.dirty.nextSetBit(column + 1)) {
                int data = sample(level, chunk, tile.hasWater, column & 15, column >> 4);
                tile.changed |= tile.cells[column] != data;
                tile.cells[column] = data;
                tile.dirty.clear(column);
                if (System.nanoTime() >= stop) break;
            }
            if (tile.dirty.isEmpty() && tile.changed) {
                ByteBuffer frame = BridgeWire.frame(BridgeWire.CHUNK_WATER_GRID, BridgeWire.CHUNK_WATER_GRID_BYTES)
                        .putInt(scene).putInt(pos.x()).putInt(pos.z());
                for (int data : tile.cells) frame.putInt(data);
                if (!connection.sendTerrain(frame.flip())) return;
                tile.changed = false;
                tile.sent = true;
            }
        }
    }

    private static int sample(ClientLevel level, LevelChunk chunk, boolean[] hasWater, int lx, int lz) {
        int minY = level.getMinY();
        int x = chunk.getPos().getMinBlockX() + lx, z = chunk.getPos().getMinBlockZ() + lz;
        int topY = chunk.getHeight(Heightmap.Types.MOTION_BLOCKING, lx, lz);
        for (int section = Math.min(hasWater.length - 1, (topY - minY) >> 4); section >= 0; --section) {
            if (!hasWater[section]) continue;
            int baseY = minY + 16 * section;
            for (int y = Math.min(topY, baseY + 15); y >= baseY; --y) {
                BlockPos pos = new BlockPos(x, y, z);
                var block = chunk.getBlockState(pos);
                var fluid = block.getFluidState();
                if (!fluid.is(FluidTags.WATER) || chunk.getFluidState(pos.above()).is(FluidTags.WATER)) continue;
                float[] corners = WaterCapture.corners(level, pos, fluid);
                float height = (float) WaterCapture.surfaceHeight(corners, 0.5, 0.5);
                var shape = block.getCollisionShape(level, pos);
                boolean blocked = !shape.isEmpty();
                if (blocked && shape.toAabbs().stream().anyMatch(box -> box.maxY >= height && box.minY <= height
                        && box.minX <= 0.5 && box.maxX >= 0.5 && box.minZ <= 0.5 && box.maxZ >= 0.5)) continue;

                int flags = KNOWN | PRESENT;
                if (y == topY) flags |= SKY;
                // The displacement guard needs every object in the two-block vertical
                // neighborhood, including waterlogged plants with an empty collision shape.
                // Keep the same column record and flag used by the renderer's wind fetch.
                if (blocked || !block.is(Blocks.WATER) || !fluid.isSource()
                        || !chunk.getBlockState(pos.below()).is(Blocks.WATER)
                        || !chunk.getBlockState(pos.below(2)).is(Blocks.WATER)
                        || !chunk.getBlockState(pos.above()).isAir()
                        || !chunk.getBlockState(pos.above(2)).isAir()
                        || Math.abs(corners[1] - corners[0]) > 0.00001f
                        || Math.abs(corners[2] - corners[0]) > 0.00001f
                        || Math.abs(corners[3] - corners[0]) > 0.00001f
                        || fluid.getFlow(level, pos).horizontalDistanceSqr() > 1e-12) flags |= OBSTACLE;
                for (Direction dir : SIDES) {
                    BlockPos near = pos.relative(dir);
                    var adjacent = level.getBlockState(near);
                    if (WaterCapture.isIce(adjacent) || WaterCapture.isIce(level.getBlockState(near.above()))) flags |= ICE;
                    if (!adjacent.getCollisionShape(level, near).isEmpty()) flags |= OBSTACLE;
                }
                if (WaterCapture.isIce(chunk.getBlockState(pos.above()))) flags |= ICE;
                int depth = 0;
                BlockPos.MutableBlockPos below = new BlockPos.MutableBlockPos();
                while (depth < 32 && y - depth >= minY && chunk.getFluidState(below.set(x, y - depth, z)).is(FluidTags.WATER)) {
                    ++depth;
                }
                if (depth > 31) flags |= SATURATED;
                int fixedHeight = (y - minY) * 64 + Math.round(height * 64);
                int optical = opticalPreset(level, pos);
                return flags | fixedHeight / 64 | (fixedHeight % 64) << 11 | Math.min(depth, 31) << 17 | optical << 22;
            }
        }
        return KNOWN;
    }

    private static int opticalPreset(ClientLevel level, BlockPos pos) {
        var biome = level.getBiome(pos);
        if (biome.is(Biomes.SWAMP) || biome.is(Biomes.MANGROVE_SWAMP) || biome.is(BiomeTags.HAS_SWAMP_HUT)) return 1;
        if (biome.is(Biomes.WARM_OCEAN) || biome.is(Biomes.LUKEWARM_OCEAN) || biome.is(Biomes.DEEP_LUKEWARM_OCEAN)) return 2;
        if (biome.is(BiomeTags.IS_OCEAN)) return 3;
        if (biome.is(BiomeTags.IS_RIVER)) return 4;
        return 0;
    }
}

