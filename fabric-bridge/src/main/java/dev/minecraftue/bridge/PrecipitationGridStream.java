package dev.minecraftue.bridge;

import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Holder;
import net.minecraft.core.QuartPos;
import net.minecraft.tags.BiomeTags;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.levelgen.Heightmap;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Iterator;
import java.util.Map;
import java.util.Set;

/** Streams MOTION_BLOCKING heightmaps, Minecraft precipitation types and visual climate profiles to UE. */
final class PrecipitationGridStream {
    private static final int MAX_CHUNKS_PER_TICK = 12;
    private static final int STREAM_RADIUS_CHUNKS = 8; // Covers 128 blocks in each cardinal direction.
    private static final int VALID_BIT = 1 << 14;
    private static final int TYPE_NONE = 0;
    private static final int TYPE_RAIN = 1;
    private static final int TYPE_SNOW = 2;
    private static final int PROFILE_TEMPERATE = 0;
    private static final int PROFILE_DRY = 1;
    private static final int PROFILE_COLD = 2;
    private static final int PROFILE_HUMID = 3;

    private final Set<ChunkPos> dirty = new HashSet<>();
    private final Map<ChunkPos, short[]> sent = new HashMap<>();
    private ChunkPos focus;
    private int focusRadius;

    void reset() {
        dirty.clear();
        sent.clear();
        focus = null;
        focusRadius = 0;
    }

    void blockChanged(BlockPos pos) {
        dirty.add(new ChunkPos(pos.getX() >> 4, pos.getZ() >> 4));
    }

    void chunkReplaced(ChunkPos pos) {
        dirty.add(pos);
        invalidateBiomeNeighbors(pos);
    }

    void chunkDropped(ChunkPos pos) {
        dirty.remove(pos);
        sent.remove(pos);
        invalidateBiomeNeighbors(pos);
    }

    private void invalidateBiomeNeighbors(ChunkPos changed) {
        // MC's biome interpolation can select a quart cell across either chunk boundary.
        for (int dz = -1; dz <= 1; dz++) {
            for (int dx = -1; dx <= 1; dx++) {
                ChunkPos neighbor = new ChunkPos(changed.x() + dx, changed.z() + dz);
                if (!neighbor.equals(changed) && sent.containsKey(neighbor)) {
                    dirty.add(neighbor);
                }
            }
        }
    }

    void tick(LocalTransport.Connection connection, int scene, ClientLevel level, ChunkPos center, int viewDistance) {
        if (!connection.canQueueTerrain()) {
            return;
        }
        int radius = Math.min(STREAM_RADIUS_CHUNKS, Math.max(2, viewDistance));
        if (!center.equals(focus) || radius != focusRadius) {
            focus = center;
            focusRadius = radius;
            sent.keySet().removeIf(pos -> pos.getChessboardDistance(center) > radius);
            dirty.removeIf(pos -> pos.getChessboardDistance(center) > radius);
            for (int dz = -radius; dz <= radius; dz++) {
                for (int dx = -radius; dx <= radius; dx++) {
                    ChunkPos pos = new ChunkPos(center.x() + dx, center.z() + dz);
                    if (!sent.containsKey(pos)) {
                        dirty.add(pos);
                    }
                }
            }
        }

        if (dirty.isEmpty()) {
            return;
        }

        int seaLevel = level.getSeaLevel();
        var chunks = level.getChunkSource();
        var biomes = level.getBiomeManager().withDifferentSource((quartX, quartY, quartZ) -> {
            LevelChunk source = chunks.getChunk(QuartPos.toSection(quartX), QuartPos.toSection(quartZ),
                    ChunkStatus.FULL, false);
            // Keep MC's spatial biome selection, avoiding its plains fallback while a neighboring chunk is absent.
            return source == null ? null : source.getNoiseBiome(quartX, quartY, quartZ);
        });
        BlockPos.MutableBlockPos cursor = new BlockPos.MutableBlockPos();
        int budget = MAX_CHUNKS_PER_TICK;
        Iterator<ChunkPos> it = dirty.iterator();
        while (it.hasNext() && budget > 0 && connection.canQueueTerrain()) {
            ChunkPos pos = it.next();
            if (pos.getChessboardDistance(center) > radius) {
                it.remove();
                continue;
            }
            LevelChunk chunk = chunks.getChunk(pos.x(), pos.z(), ChunkStatus.FULL, false);
            if (chunk == null) {
                continue;
            }
            it.remove();
            budget--;
            short[] cells = sampleChunk(chunk, seaLevel, biomes, cursor);
            short[] previous = sent.get(pos);
            if (previous != null && Arrays.equals(previous, cells)) {
                continue;
            }
            if (!connection.sendTerrain(encodeChunk(scene, pos, cells))) {
                dirty.add(pos);
                return;
            }
            sent.put(pos, cells);
        }
    }

    static ByteBuffer encodeChunk(int scene, ChunkPos pos, short[] cells) {
        ByteBuffer frame = BridgeWire.frame(BridgeWire.CHUNK_PRECIPITATION, BridgeWire.CHUNK_PRECIPITATION_BYTES)
                .putInt(scene).putInt(pos.x()).putInt(pos.z());
        for (short cell : cells) {
            frame.putShort(cell);
        }
        return frame.flip();
    }

    static short[] sampleChunk(ChunkAccess chunk, int seaLevel, BiomeManager biomes, BlockPos.MutableBlockPos cursor) {
        short[] cells = new short[256];
        int minY = chunk.getMinY();
        int baseX = chunk.getPos().getMinBlockX();
        int baseZ = chunk.getPos().getMinBlockZ();
        for (int lz = 0; lz < 16; lz++) {
            int wz = baseZ + lz;
            for (int lx = 0; lx < 16; lx++) {
                int wx = baseX + lx;
                // ChunkAccess.getHeight returns the highest blocking block Y; top surface is Y + 1.
                int topY = chunk.getHeight(Heightmap.Types.MOTION_BLOCKING, lx, lz) + 1;
                int relY = Math.clamp(topY - minY, 0, 2047);
                cursor.set(wx, Math.max(minY, topY), wz);
                Holder<Biome> biome = biomes.getBiome(cursor);
                if (biome != null) {
                    cells[lz * 16 + lx] = encodeColumn(relY, biome, cursor, seaLevel);
                }
            }
        }
        return cells;
    }

    static short encodeColumn(int relY, Holder<Biome> biome, BlockPos position, int seaLevel) {
        Biome.Precipitation precipitation = biome.value().getPrecipitationAt(position, seaLevel);
        int type = switch (precipitation) {
            case NONE -> TYPE_NONE;
            case RAIN -> TYPE_RAIN;
            case SNOW -> TYPE_SNOW;
        };
        int profile = switch (precipitation) {
            case NONE -> PROFILE_DRY;
            case SNOW -> PROFILE_COLD;
            case RAIN -> isHumid(biome) ? PROFILE_HUMID : PROFILE_TEMPERATE;
        };
        int profileBits = ((profile & 1) << 11) | ((profile & 2) << 14);
        return (short) (VALID_BIT | (type << 12) | relY | profileBits);
    }

    private static boolean isHumid(Holder<Biome> biome) {
        return biome.is(BiomeTags.IS_JUNGLE) || biome.is(BiomeTags.HAS_SWAMP_HUT)
                || biome.is(Biomes.SWAMP) || biome.is(Biomes.MANGROVE_SWAMP) || biome.is(Biomes.JUNGLE)
                || biome.is(Biomes.SPARSE_JUNGLE) || biome.is(Biomes.BAMBOO_JUNGLE);
    }
}
