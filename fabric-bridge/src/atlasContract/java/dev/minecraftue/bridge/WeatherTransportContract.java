package dev.minecraftue.bridge;

import com.mojang.serialization.Lifecycle;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.MappedRegistry;
import net.minecraft.core.QuartPos;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.data.registries.VanillaRegistries;
import net.minecraft.resources.ResourceKey;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.Biomes;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.levelgen.Heightmap;

import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Set;

final class WeatherTransportContract {
    private static final int SCENE = 29;
    private static final int SEA_LEVEL = 63;
    private static final int MIN_Y = -64;
    private static final int HEIGHT = 384;
    private static final int VALID = 0x4000;
    private static final int RAIN = 0x1000;
    private static final int SNOW = 0x2000;
    private static final int DRY = 0x0800;
    private static final int COLD = 0x8000;
    private static final int HUMID = DRY | COLD;

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        var defaults = VanillaRegistries.createLookup().lookupOrThrow(Registries.BIOME);
        var biomes = new MappedRegistry<Biome>(Registries.BIOME, Lifecycle.stable());
        defaults.listElements().forEach(biome -> Registry.register(biomes, biome.key(), biome.value()));
        biomes.freeze();

        expectBiome(biomes, Biomes.PLAINS, 64, RAIN);
        expectBiome(biomes, Biomes.DESERT, 64, DRY);
        expectBiome(biomes, Biomes.BADLANDS, 64, DRY);
        expectBiome(biomes, Biomes.SNOWY_PLAINS, 64, SNOW | COLD);
        expectBiome(biomes, Biomes.ICE_SPIKES, 64, SNOW | COLD);
        expectBiome(biomes, Biomes.FROZEN_OCEAN, 230, SNOW | COLD);
        expectBiome(biomes, Biomes.SWAMP, 64, RAIN | HUMID);
        expectBiome(biomes, Biomes.MANGROVE_SWAMP, 64, RAIN | HUMID);
        expectBiome(biomes, Biomes.JUNGLE, 64, RAIN | HUMID);
        expectBiome(biomes, Biomes.TAIGA, 64, RAIN);
        expectBiome(biomes, Biomes.TAIGA, 300, SNOW | COLD);

        Path output = Files.createDirectories(Path.of(args[0]));
        testChunk(biomes, output);
        System.out.println("Weather transport contract passed.");
    }

    private static void expectBiome(Registry<Biome> biomes, ResourceKey<Biome> key, int y, int bits) {
        BlockPos pos = new BlockPos(-23, y, 71);
        var holder = biomes.getOrThrow(key);
        int encoded = Short.toUnsignedInt(PrecipitationGridStream.encodeColumn(y - MIN_Y, holder, pos, SEA_LEVEL));
        check(encoded == (VALID | bits | (y - MIN_Y)), "Incorrect column bits for " + key + " at y=" + y);
        System.out.printf("%s y=%d native=%s cell=0x%04x%n", key.identifier(), y,
                holder.value().getPrecipitationAt(pos, SEA_LEVEL), encoded);
    }

    private static void testChunk(Registry<Biome> biomes, Path output) throws Exception {
        ChunkPos chunkPos = new ChunkPos(-2, 3);
        var registries = new RegistryAccess.ImmutableRegistryAccess(List.of(biomes));
        var chunk = new ProtoChunk(chunkPos, UpgradeData.EMPTY, LevelHeightAccessor.create(MIN_Y, HEIGHT),
                PalettedContainerFactory.create(registries), null);
        place(chunk, 4, 63, 4, Blocks.STONE);
        place(chunk, 12, 63, 4, Blocks.SAND);
        place(chunk, 4, 63, 12, Blocks.ICE);
        place(chunk, 12, 63, 12, Blocks.STONE);
        place(chunk, 12, 70, 12, Blocks.GLASS);
        place(chunk, 5, 319, 4, Blocks.STONE);
        Heightmap.primeHeightmaps(chunk, Set.of(Heightmap.Types.MOTION_BLOCKING));

        var plains = biomes.getOrThrow(Biomes.PLAINS);
        var desert = biomes.getOrThrow(Biomes.DESERT);
        var snow = biomes.getOrThrow(Biomes.SNOWY_PLAINS);
        var swamp = biomes.getOrThrow(Biomes.SWAMP);
        BiomeManager interpolator = new BiomeManager((x, y, z) -> {
            if (QuartPos.toSection(x) != chunkPos.x() || QuartPos.toSection(z) != chunkPos.z()) {
                return null;
            }
            if ((z & 3) < 2) {
                return (x & 3) < 2 ? plains : desert;
            }
            return (x & 3) < 2 ? snow : swamp;
        }, 74631L);
        var cursor = new BlockPos.MutableBlockPos();
        short[] cells = PrecipitationGridStream.sampleChunk(chunk, SEA_LEVEL, interpolator, cursor);
        expectCell(cells, 4, 4, VALID | RAIN | 128);
        expectCell(cells, 12, 4, VALID | DRY | 128);
        expectCell(cells, 4, 12, VALID | SNOW | COLD | 128);
        expectCell(cells, 12, 12, VALID | RAIN | HUMID | 135);
        expectCell(cells, 5, 4, VALID | RAIN | HEIGHT);
        expectCell(cells, 4, 5, VALID | RAIN);
        int missing = 0;
        for (short cell : cells) {
            if (cell == 0) missing++;
        }
        check(missing > 0 && missing < 100, "Missing biome sources must leave only the affected border columns invalid");
        short[] loaded = PrecipitationGridStream.sampleChunk(chunk, SEA_LEVEL,
                interpolator.withDifferentSource((x, y, z) -> plains), cursor);
        for (short cell : loaded) {
            check((cell & VALID) != 0, "Complete source data must leave no invalid columns");
        }

        ByteBuffer frame = PrecipitationGridStream.encodeChunk(SCENE, chunkPos, cells);
        ByteBuffer reader = frame.duplicate();
        check(reader.getInt() == 525 && reader.get() == 35, "Frame keeps the v28 ChunkPrecipitation size and type");
        check(reader.getInt() == SCENE && reader.getInt() == -2 && reader.getInt() == 3,
                "Frame keeps scene and signed chunk coordinates");
        for (short cell : cells) {
            check(reader.getShort() == cell, "Columns must serialize in lz * 16 + lx order");
        }
        check(!reader.hasRemaining(), "Frame is fully consumed");
        Files.write(output.resolve("chunk-precipitation-v28.bin"), frame.array());

        place(chunk, 12, 70, 12, Blocks.AIR);
        chunk.getOrCreateHeightmapUnprimed(Heightmap.Types.MOTION_BLOCKING).update(12, 70, 12, Blocks.AIR.defaultBlockState());
        expectCell(PrecipitationGridStream.sampleChunk(chunk, SEA_LEVEL, interpolator, cursor), 12, 12,
                VALID | RAIN | HUMID | 128);
        System.out.printf("Chunk=(-2,3) scene=%d missing=%d glassTop=71 openTop=64 maxTop=320%n", SCENE, missing);
    }

    private static void place(ProtoChunk chunk, int x, int y, int z, net.minecraft.world.level.block.Block block) {
        chunk.setBlockState(chunk.getPos().getBlockAt(x, y, z), block.defaultBlockState(), 0);
    }

    private static void expectCell(short[] cells, int x, int z, int expected) {
        int actual = Short.toUnsignedInt(cells[z * 16 + x]);
        check(actual == expected, "Incorrect cell (" + x + "," + z + "): expected " + expected + ", got " + actual);
        System.out.printf("local=(%d,%d) cell=0x%04x%n", x, z, actual);
    }

    private static void check(boolean good, String reason) {
        if (!good) throw new AssertionError(reason);
    }
}
