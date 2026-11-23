package dev.minecraftue.bridge;

import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.packs.PackLocationInfo;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.PathPackResources;
import net.minecraft.server.packs.repository.PackSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.BlockStateProperties;
import net.minecraft.world.level.material.FluidState;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.TreeSet;

final class SectionGlassContract {
    static {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
    }

    private static final int SCENE = 1;
    private static final SectionPos ORIGIN = SectionPos.of(0, 0, 0);
    private static final GlassFilterMaterials.Material GREEN = new GlassFilterMaterials.Material(0.055f, 0.65f, 0.11f, 1);
    private static final BlockState GREEN_BLOCK = vanilla("green_stained_glass");
    private static final BlockState GREEN_PANE = vanilla("green_stained_glass_pane");
    private static final BlockState BLUE_BLOCK = vanilla("blue_stained_glass");

    public static void main(String[] args) throws Exception {
        Path output = Files.createDirectories(Path.of(args[0]));
        GlassFilterMaterials materials;
        try (var resources = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of())) {
            materials = GlassFilterMaterials.load(resources);
            var entries = GlassFilterMaterials.loadEntries(resources);
            check(entries.size() == 32, "each of the 16 stained glass colors has a block and a pane");
            for (var suffix : List.of("block", "pane")) {
                long distinct = entries.entrySet().stream()
                        .filter(item -> item.getKey().getPath().endsWith("_pane") == suffix.equals("pane"))
                        .map(Map.Entry::getValue).distinct().count();
                check(distinct == 16, "all dye colors have distinct reference transmittance");
            }
            for (var entry : entries.entrySet()) {
                var block = BuiltInRegistries.BLOCK.getOptional(entry.getKey()).orElseThrow();
                check(materials.forState(block.defaultBlockState()).equals(entry.getValue()), "every vanilla id resolves");
                if (entry.getKey().getPath().endsWith("_pane")) {
                    var solidId = Identifier.withDefaultNamespace(entry.getKey().getPath().replace("_pane", ""));
                    check(entry.getValue().equals(entries.get(solidId)), "pane and block use the same reference material");
                }
            }
            check(materials.forState(Blocks.GLASS.defaultBlockState()) == null, "clear glass has no filter volume");
            check(materials.forState(Blocks.TINTED_GLASS.defaultBlockState()) == null, "vanilla tinted glass keeps its own light rule");
        }

        testPacks(output);
        testGeometry(materials);
        writeReplays(output, materials);
        System.out.println("Section glass contract passed");
    }

    private static void testPacks(Path output) throws Exception {
        Path low = output.resolve("low-pack");
        Path high = output.resolve("high-pack");
        writePack(low, "minecraft", """
                {
                  "green_stained_glass": {"transmittance": [0.6, 0.2, 0.1], "reference_thickness": 0.5},
                  "green_stained_glass_pane": {"transmittance": [0.125, 0.5, 0.75], "reference_thickness": 1},
                  "glass": {"transmittance": [1, 0, 1], "reference_thickness": 0.25}
                }
                """);
        writePack(low, "glassworks", """
                {"decor/small_glass": {"transmittance": [0.2, 0.4, 0.6], "reference_thickness": 0.125}}
                """);
        writePack(high, "minecraft", """
                {
                  "green_stained_glass": {"transmittance": [0, 1, 0.75], "reference_thickness": 0.125},
                  "green_stained_glass_pane": {"transmittance": [0.1, 1.000000001, 0.1], "reference_thickness": 1},
                  "red_stained_glass_pane": null,
                  "red_stained_glass": {"transmittance": [1, 0, 0], "reference_thickness": 0}
                }
                """);
        writePack(high, "glassworks", """
                {
                  "minecraft:blue_stained_glass": null,
                  "decor/another_glass": {"transmittance": [0, 0.125, 1], "reference_thickness": 2}
                }
                """);
        try (var resourceManager = new MultiPackResourceManager(PackType.CLIENT_RESOURCES,
                List.of(pack("low", low), pack("high", high)))) {
            var materials = GlassFilterMaterials.load(resourceManager);
            check(materials.forState(GREEN_BLOCK).equals(new GlassFilterMaterials.Material(0, 1, 0.75f, 0.125f)),
                    "highest pack overrides a matching identifier");
            check(materials.forState(GREEN_PANE).equals(new GlassFilterMaterials.Material(0.125f, 0.5f, 0.75f, 1)),
                    "out-of-range entry leaves valid lower pack in place");
            check(materials.forState(Blocks.GLASS.defaultBlockState()) != null, "a pack may declare new opt-in blocks");
            check(materials.forState(vanilla("red_stained_glass_pane")) == null, "explicit null removes a filter");
            check(materials.forState(vanilla("red_stained_glass")).thickness() == 1,
                    "invalid reference thickness leaves built-in material");
            var entries = GlassFilterMaterials.loadEntries(resourceManager);
            check(entries.containsKey(Identifier.parse("glassworks:decor/small_glass"))
                            && entries.containsKey(Identifier.parse("glassworks:decor/another_glass")),
                    "namespaces are isolated and distinct declarations merge by block path");
            check(entries.containsKey(Identifier.parse("minecraft:blue_stained_glass")), "a namespace cannot edit another");
        }
        try (var resources = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of())) {
            var reloaded = GlassFilterMaterials.load(resources);
            check(reloaded.forState(GREEN_BLOCK).equals(GREEN) && reloaded.forState(Blocks.GLASS.defaultBlockState()) == null,
                    "resource reload restores defaults and drops disabled pack declarations");
        }
    }

    private static void testGeometry(GlassFilterMaterials materials) {
        BlockPos pos = new BlockPos(-1, 31, 15);
        SectionPos section = SectionPos.of(pos);
        var world = new Fixture();
        world.states.put(pos, GREEN_BLOCK);
        var cube = SectionGlass.capture(SCENE, section, world, materials);
        var part = parse(cube.parts().getFirst(), section, 0, 1);
        check(part.materials().equals(List.of(GREEN)) && part.boxes().equals(List.of(
                new SectionGlass.Box(0xfff, GREEN, new SectionGlass.Bounds(0, 0, 0, 1, 1, 1)))),
                "unit glass cube with signed section coordinates and end-of-section local position");
        check(java.util.Arrays.equals(cube.digest(), SectionGlass.capture(SCENE, section, world, materials).digest()),
                "unchanged filter snapshots are stable");

        world.states.put(pos, GREEN_PANE);
        var pane = SectionGlass.capture(SCENE, section, world, materials);
        var centre = parse(pane.parts().getFirst(), section, 0, 1).boxes().getFirst().bounds();
        check(centre.maxZ() - centre.minZ() == 2.0f / 16 && centre.maxX() - centre.minX() == 2.0f / 16,
                "disconnected pane uses its two-texel post, not the full cell");
        var attached = GREEN_PANE.setValue(BlockStateProperties.EAST, true)
                .setValue(BlockStateProperties.SOUTH, true).setValue(BlockStateProperties.WEST, true);
        world.states.put(pos, attached);
        world.states.put(pos.east(), GREEN_PANE.setValue(BlockStateProperties.WEST, true));
        var modified = SectionGlass.capture(SCENE, section, world, materials);
        var bounds = parse(modified.parts().getFirst(), section, 0, 1).boxes().stream().map(SectionGlass.Box::bounds).toList();
        check(bounds.stream().anyMatch(box -> box.maxX() == 1)
                        && bounds.stream().anyMatch(box -> box.maxZ() == 1),
                "pane arms reach the changed neighbours");
        for (int index = 0; index < bounds.size(); index++) {
            for (var b : bounds.subList(index + 1, bounds.size())) {
                var a = bounds.get(index);
                check(Math.min(a.maxX(), b.maxX()) <= Math.max(a.minX(), b.minX())
                                || Math.min(a.maxY(), b.maxY()) <= Math.max(a.minY(), b.minY())
                                || Math.min(a.maxZ(), b.maxZ()) <= Math.max(a.minZ(), b.minZ()),
                        "connected voxel union has no positive volume overlap");
            }
        }
        var adjacent = SectionGlass.capture(SCENE, SectionPos.of(pos.east()), world, materials);
        check(parse(adjacent.parts().getFirst(), SectionPos.of(pos.east()), 0, 1).boxes().getFirst().localPos() == 0xff0,
                "neighbouring section starts at local X=0");
        var invalidated = new TreeSet<Long>();
        SectionPos.aroundAndAtBlockPos(pos.east().asLong(), invalidated::add);
        check(invalidated.contains(section.asLong()) && invalidated.contains(SectionPos.of(pos.east()).asLong()),
                "the production block edit invalidation reaches both cross-boundary pane sections");
        world.states.remove(pos);
        check(SectionGlass.capture(SCENE, section, world, materials) == SectionGlass.EMPTY, "removed filter leaves a sparse empty snapshot");
        var clear = parse(SectionGlass.clear(SCENE, section), section, 0, 1);
        check(clear.materials().isEmpty() && clear.boxes().isEmpty(), "one empty part clears the section");
    }

    private static void writeReplays(Path output, GlassFilterMaterials materials) throws Exception {
        var world = new Fixture();
        BlockPos first = new BlockPos(8, 4, 10);
        world.states.put(first, BLUE_BLOCK);
        write(output, "blue", SectionGlass.capture(SCENE, ORIGIN, world, materials).parts());
        world.states.put(first, GREEN_BLOCK);
        write(output, "green", SectionGlass.capture(SCENE, ORIGIN, world, materials).parts());
        world.states.put(first.south(), GREEN_BLOCK);
        write(output, "green-stacked", SectionGlass.capture(SCENE, ORIGIN, world, materials).parts());
        world.states.clear();
        var eastWest = GREEN_PANE.setValue(BlockStateProperties.WEST, true).setValue(BlockStateProperties.EAST, true);
        world.states.put(first, eastWest);
        write(output, "green-pane", SectionGlass.capture(SCENE, ORIGIN, world, materials).parts());
        world.states.clear();
        world.states.put(first.west(), eastWest);
        write(output, "green-rect-half", SectionGlass.capture(SCENE, ORIGIN, world, materials).parts());
        world.states.put(first, eastWest);
        write(output, "green-rect-full", SectionGlass.capture(SCENE, ORIGIN, world, materials).parts());
        world.states.clear();
        world.states.put(new BlockPos(8, 5, 8), GREEN_BLOCK);
        write(output, "green-sun", SectionGlass.capture(SCENE, ORIGIN, world, materials).parts());
        world.states.clear();
        for (int x = 3; x <= 13; x++) {
            for (int y = 1; y <= 10; y++) {
                world.states.put(new BlockPos(x, y, 10), ((x + y) & 1) == 0
                        ? GREEN_BLOCK : BLUE_BLOCK);
            }
        }
        write(output, "multi-light", SectionGlass.capture(SCENE, ORIGIN, world, materials).parts());
        write(output, "clear", List.of(SectionGlass.clear(SCENE, ORIGIN)));

        var cube = new SectionGlass.Bounds(0, 0, 0, 1, 1, 1);
        int max = BridgeWire.MAX_SECTION_GLASS_BOXES_PER_PART;
        var boxes = new ArrayList<SectionGlass.Box>();
        for (int index = 0; index < max; index++) {
            boxes.add(new SectionGlass.Box(index, GREEN, cube));
        }
        var extended = new SectionGlass.Bounds(-0.25f, 0, 0, 0, 1, 1.25f);
        var zeroChannel = new GlassFilterMaterials.Material(0, 1, 0.5f, 0.125f);
        boxes.add(new SectionGlass.Box(0, zeroChannel, extended));
        var multipart = SectionGlass.snapshot(SCENE, ORIGIN, boxes);
        check(multipart.parts().size() == 2, "4097 boxes require two complete parts");
        var firstPart = parse(multipart.parts().get(0), ORIGIN, 0, 2);
        var lastPart = parse(multipart.parts().get(1), ORIGIN, 1, 2);
        check(firstPart.materials().size() == 1 && firstPart.boxes().size() == max, "local material table is deduplicated");
        check(lastPart.boxes().equals(List.of(new SectionGlass.Box(0, zeroChannel, extended))),
                "last part starts a new material table and preserves out-of-cell geometry");
        write(output, "multipart", multipart.parts());
        writeTransactionalReplay(output, cube);
        var tooMany = Collections.nCopies(max * BridgeWire.MAX_SECTION_GLASS_PARTS + 1, boxes.getFirst());
        try {
            SectionGlass.snapshot(SCENE, ORIGIN, tooMany);
            throw new AssertionError("more than 256 parts must fail explicitly");
        } catch (IllegalStateException limit) {
            check(limit.getMessage().contains("exceeds"), "capacity failure explains the bound");
        }
    }

    private static void writeTransactionalReplay(Path output, SectionGlass.Bounds cube) throws Exception {
        var boxes = new ArrayList<SectionGlass.Box>(4097);
        // Put the only box in the light path in part zero. The last part is needed to publish it.
        boxes.add(new SectionGlass.Box(8 | (4 << 4) | (10 << 8), GREEN, cube));
        for (int index = 0; index < 4096; index++) {
            float x = (index & 15) / 16.0f;
            float y = ((index >> 4) & 15) / 16.0f;
            float z = (index >> 8) / 16.0f;
            float edge = 1.0f / 32;
            var bounds = new SectionGlass.Bounds(x, y, z, x + edge, y + edge, z + edge);
            boxes.add(new SectionGlass.Box(0x111, GREEN, bounds));
        }
        var snapshot = SectionGlass.snapshot(SCENE, ORIGIN, boxes);
        write(output, "transaction", snapshot.parts());
    }

    private static Part parse(ByteBuffer encoded, SectionPos section, int part, int total) {
        var frame = encoded.duplicate();
        check(frame.getInt() == frame.remaining() && Byte.toUnsignedInt(frame.get()) == 40, "frame length and id");
        check(frame.getInt() == SCENE && frame.getInt() == section.x() && frame.getInt() == section.y()
                        && frame.getInt() == section.z(), "scene and signed section coordinates");
        check(Short.toUnsignedInt(frame.getShort()) == part && Short.toUnsignedInt(frame.getShort()) == total,
                "part sequence");
        int materialCount = Short.toUnsignedInt(frame.getShort());
        int boxCount = Short.toUnsignedInt(frame.getShort());
        check(frame.remaining() == materialCount * 16 + boxCount * 28, "material and box stride");
        var materials = new ArrayList<GlassFilterMaterials.Material>();
        for (int index = 0; index < materialCount; index++) {
            materials.add(new GlassFilterMaterials.Material(frame.getFloat(), frame.getFloat(), frame.getFloat(), frame.getFloat()));
        }
        var boxes = new ArrayList<SectionGlass.Box>();
        for (int index = 0; index < boxCount; index++) {
            int localPos = Short.toUnsignedInt(frame.getShort());
            var material = materials.get(Short.toUnsignedInt(frame.getShort()));
            boxes.add(new SectionGlass.Box(localPos, material, new SectionGlass.Bounds(frame.getFloat(), frame.getFloat(),
                    frame.getFloat(), frame.getFloat(), frame.getFloat(), frame.getFloat())));
        }
        return new Part(materials, boxes);
    }

    private static void write(Path output, String name, List<ByteBuffer> parts) throws Exception {
        var bytes = new ByteArrayOutputStream();
        for (var part : parts) {
            var source = part.duplicate();
            byte[] data = new byte[source.remaining()];
            source.get(data);
            bytes.writeBytes(data);
        }
        Files.write(output.resolve(name + ".bin"), bytes.toByteArray());
    }

    private static void writePack(Path root, String namespace, String json) throws Exception {
        var path = root.resolve("assets").resolve(namespace).resolve(GlassFilterMaterials.RESOURCE_PATH);
        Files.createDirectories(path.getParent());
        Files.writeString(path, json);
    }

    private static PathPackResources pack(String name, Path root) {
        return new PathPackResources(new PackLocationInfo(name, Component.literal(name), PackSource.DEFAULT, Optional.empty()), root);
    }

    private static BlockState vanilla(String id) {
        return BuiltInRegistries.BLOCK.getOptional(Identifier.withDefaultNamespace(id)).orElseThrow().defaultBlockState();
    }

    private static void check(boolean value, String message) {
        if (!value) {
            throw new AssertionError(message);
        }
    }

    private record Part(List<GlassFilterMaterials.Material> materials, List<SectionGlass.Box> boxes) {}

    private static final class Fixture implements BlockGetter {
        private final Map<BlockPos, BlockState> states = new HashMap<>();

        @Override
        public BlockEntity getBlockEntity(BlockPos pos) {
            return null;
        }

        @Override
        public BlockState getBlockState(BlockPos pos) {
            return states.getOrDefault(pos, Blocks.AIR.defaultBlockState());
        }

        @Override
        public FluidState getFluidState(BlockPos pos) {
            return getBlockState(pos).getFluidState();
        }

        @Override
        public int getHeight() {
            return 384;
        }

        @Override
        public int getMinY() {
            return -64;
        }
    }
}
