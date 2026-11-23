package dev.minecraftue.bridge;

import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.blaze3d.vertex.QuadInstance;
import net.minecraft.SharedConstants;
import net.minecraft.client.model.geom.builders.UVPair;
import net.minecraft.client.renderer.block.BlockStateModelSet;
import net.minecraft.client.renderer.block.dispatch.SingleVariant;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.texture.SpriteContents;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.client.resources.metadata.animation.FrameSize;
import net.minecraft.client.resources.model.SimpleModelWrapper;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.client.resources.model.geometry.QuadCollection;
import net.minecraft.client.resources.model.sprite.Material;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.AbstractFurnaceBlock;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.BlockStateProperties;
import net.minecraft.world.level.block.state.properties.Half;
import net.minecraft.world.level.material.FluidState;
import org.joml.Vector3f;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** Exercises the production mask and wire encoder with real 26.2 block states. */
final class SectionLightingContract {
    private static final int SURFACE = 1 << 6;
    private static final int COVERED = 1 << 7;

    public static void main(String[] args) throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        BlockPos origin = new BlockPos(15, 31, -1);
        BlockState stone = Blocks.STONE.defaultBlockState();
        BlockState lantern = Blocks.SEA_LANTERN.defaultBlockState();
        BlockState lava = Blocks.LAVA.defaultBlockState();
        Fixture world = new Fixture();
        expect(world, origin, lantern, SURFACE | 0x3f, "all six faces on a floating lantern");

        for (Direction direction : Direction.values()) {
            world.states.put(origin.relative(direction), stone);
        }
        expect(world, origin, lantern, SURFACE, "sealed lantern");
        expect(world, origin, lava, SURFACE, "sealed lava");
        world.states.remove(origin.above());
        expect(world, origin, lantern, SURFACE | face(Direction.UP), "open ceiling across section boundary");
        world.states.put(origin.east(), Blocks.GLASS.defaultBlockState());
        int twoFaces = SURFACE | face(Direction.UP) | face(Direction.EAST);
        expect(world, origin, lantern, twoFaces, "transparent neighbor and two open faces");
        world.states.put(origin.above(), lava);
        expect(world, origin, lava, SURFACE | face(Direction.EAST), "shared lava face omitted");
        world.states.put(origin.east(), stone);
        expect(world, origin, lava, SURFACE, "buried and contiguous lava contributes no light");
        check(encodedCount(world, origin, lava) == 0, "buried emitters must be omitted from wire records");
        world.states.remove(origin.above());
        expect(world, origin, lava, SURFACE | face(Direction.UP), "reopening sealed lava");

        BlockState furnace = Blocks.FURNACE.defaultBlockState().setValue(AbstractFurnaceBlock.LIT, true)
                .setValue(AbstractFurnaceBlock.FACING, Direction.EAST);
        expect(world, origin, furnace, SURFACE, "furnace top opening cannot substitute for blocked mouth");
        world.states.remove(origin.east());
        expect(world, origin, furnace, SURFACE | face(Direction.EAST), "furnace front facing east");
        expect(world, origin, Blocks.LIGHT.defaultBlockState(), 0, "invisible light retains a center source");
        expect(world, origin, Blocks.TORCH.defaultBlockState(), 0, "torch retains a center source");
        expect(world, origin, Blocks.BEACON.defaultBlockState(), 0, "beacon transparent model retains a center source");

        SectionCapture capture = SectionCapture.begin(Map.of());
        try {
            capture.beginBlock(world, origin, lantern);
            var frame = capture.encodeLights(7, SectionPos.of(origin), true);
            check(frame.getInt() == 1 + BridgeWire.SECTION_LIGHTS_HEADER_BYTES + BridgeWire.SECTION_LIGHT_BYTES
                            && Byte.toUnsignedInt(frame.get()) == BridgeWire.SECTION_LIGHTS && frame.getInt() == 7,
                    "record length and scene");
            frame.position(frame.position() + 12);
            check(frame.getShort() == 1 && Short.toUnsignedInt(frame.getShort()) == 0xfff, "one corner block");
            check(readRgb(frame) == 0xd9f4ff, "palette supplies light color when the model has no emission data");
            check(Byte.toUnsignedInt(frame.get()) == 15 && Byte.toUnsignedInt(frame.get()) == twoFaces
                            && !frame.hasRemaining(), "level and two faces in separate bytes");
        } finally {
            SectionCapture.end();
        }

        // The production SceneMeshStream uses the same dispatch for every block change.
        Set<Long> affected = new HashSet<>();
        SectionPos.aroundAndAtBlockPos(origin.east().asLong(), affected::add);
        check(affected.contains(SectionPos.of(origin).asLong()) && affected.contains(SectionPos.of(origin.east()).asLong()),
                "an edit across X=15/16 marks the emitter's section");
        testCoveredCopperBulb(origin);
        testModelColors(origin);
        testMaterialLights(args.length == 0 ? null : java.nio.file.Path.of(args[0]));
        if (args.length > 0) {
            var output = java.nio.file.Path.of(args[0]);
            VanillaEmitterFixture.write(output, new Fixture());
            TextureEmissionContract.verify(output.resolve("texture-emission"), new Fixture());
        }
        testVegetationBuckets(origin);
        testConnectedGlass(origin);
        System.out.println("Section lighting contract passed");
    }

    private static void testConnectedGlass(BlockPos origin) {
        var glass = Blocks.GLASS.defaultBlockState();
        var pane = Blocks.GLASS_PANE.defaultBlockState();
        var eastPane = pane.setValue(BlockStateProperties.EAST, true);
        var westPane = pane.setValue(BlockStateProperties.WEST, true);
        var elbow = westPane.setValue(BlockStateProperties.NORTH, true);
        float centerMin = 7f / 16;
        float centerMax = 9f / 16;

        try (var face = glassSprite("block/glass"); var edge = glassSprite("block/glass_pane_top")) {
            var front = glassFace(face, 0, 1, 0, 0, 1);
            var eastArm = glassFace(face, centerMax, 1, centerMin, centerMax, 1);
            var westArm = glassFace(face, 0, centerMin, centerMin, 0, centerMin);
            var northArm = new BakedQuad(new Vector3f(centerMin, 0, 0), new Vector3f(centerMin, 1, 0),
                    new Vector3f(centerMin, 1, centerMin), new Vector3f(centerMin, 0, centerMin),
                    westArm.packedUV0(), westArm.packedUV1(), westArm.packedUV2(), westArm.packedUV3(),
                    Direction.WEST, glassMaterial(face));
            var top = glassCap(edge, Direction.UP, centerMax, 1);
            var bottom = glassCap(edge, Direction.DOWN, centerMax, 1);
            var postTop = glassCap(edge, Direction.UP, centerMin, centerMax);
            var postBottom = glassCap(edge, Direction.DOWN, centerMin, centerMax);
            var models = new BlockStateModelSet(Map.of(
                    glass, glassModel(face, front),
                    pane, glassModel(face, postTop, postBottom),
                    eastPane, glassModel(face, eastArm, top, bottom, postTop, postBottom),
                    elbow, glassModel(face, westArm, northArm, postTop, postBottom)), glassModel(face));
            var world = new Fixture();
            var connected = new ConnectedGlass(models);

            check(connected.connect(world, origin, glass, front) == front, "isolated glass keeps its full outline");
            world.states.put(origin.east(), glass);
            world.states.put(origin.above(), glass);
            BakedQuad joined = connected.connect(world, origin, glass, front);
            check(UVPair.unpackU(joined.packedUV2()) == face.getU(11f / 16)
                    && UVPair.unpackV(joined.packedUV2()) == face.getV(5f / 16), "shared corner trims both texture edges");
            check(joined.packedUV0() == front.packedUV0(), "unconnected edges retain their authored UVs");
            BakedQuad otherSide = connected.connect(world, origin.east(), glass, front);
            check(otherSide == front, "a neighbor absent from the snapshot does not join");
            world.states.put(origin, glass);
            otherSide = connected.connect(world, origin.east(), glass, front);
            check(UVPair.unpackU(otherSide.packedUV0()) == face.getU(5f / 16), "both sides trim across an X section boundary");
            var red = BuiltInRegistries.BLOCK.getValue(Identifier.withDefaultNamespace("red_stained_glass")).defaultBlockState();
            check(ConnectedGlass.appliesTo(red), "registered stained glass is eligible for connections");
            world.states.put(origin.east(), red);
            world.states.remove(origin.above());
            check(connected.connect(world, origin, glass, front) == front, "different glass types keep their boundary");

            world.states.put(origin, eastPane);
            world.states.put(origin.east(), elbow);
            world.states.put(origin.above(), eastPane);
            joined = connected.connect(world, origin, eastPane, eastArm);
            check(UVPair.unpackU(joined.packedUV2()) == face.getU(11f / 16)
                    && UVPair.unpackV(joined.packedUV2()) == face.getV(5f / 16), "joined pane arm trims horizontal and vertical frames");
            check(connected.connect(world, origin.east(), elbow, northArm) == northArm,
                    "the perpendicular arm of an L-shaped join keeps its outline");
            check(connected.connect(world, origin, eastPane, top) == null, "matching stacked pane arm cap is hidden");
            check(connected.connect(world, origin.above(), eastPane, bottom) == null, "matching underside is hidden too");
            world.states.put(origin.above(), pane);
            check(connected.connect(world, origin, eastPane, top) == top, "exposed cap on an unmatched arm remains");
            check(connected.connect(world, origin, eastPane, postTop) == null, "matching post cap is hidden despite a different arm state");

            var emitted = new ArrayList<BakedQuad>();
            var capture = SectionCapture.begin(Map.of());
            try {
                capture.useBakedModels(models);
                capture.beginBlock(world, origin, eastPane);
                var instance = new QuadInstance();
                capture.putModelQuad(List.of(), (x, y, z, q, colors) -> emitted.add(q), 0, 0, 0, postTop, instance);
                capture.putModelQuad(List.of(), (x, y, z, q, colors) -> emitted.add(q), 0, 0, 0, eastArm, instance);
                check(emitted.size() == 1 && UVPair.unpackU(emitted.getFirst().packedUV2()) == face.getU(11f / 16)
                        && UVPair.unpackV(emitted.getFirst().packedUV2()) == face.getV(0), "section output culls and joins only matching geometry");
            } finally {
                SectionCapture.end();
            }
        }
    }

    private static TextureAtlasSprite glassSprite(String path) {
        var contents = new SpriteContents(Identifier.withDefaultNamespace(path), new FrameSize(16, 16),
                new NativeImage(16, 16, true));
        return new TextureAtlasSprite(TextureAtlas.LOCATION_BLOCKS, contents, 64, 64, 16, 16, 0) {};
    }

    private static SingleVariant glassModel(TextureAtlasSprite sprite, BakedQuad... faces) {
        var quads = new QuadCollection.Builder();
        for (BakedQuad face : faces) {
            quads.addUnculledFace(face);
        }
        return new SingleVariant(new SimpleModelWrapper(quads.build(), false, new Material.Baked(sprite, true)));
    }

    private static BakedQuad glassFace(TextureAtlasSprite sprite, float left, float right, float depth,
                                      float uStart, float uEnd) {
        return new BakedQuad(new Vector3f(left, 0, depth), new Vector3f(left, 1, depth),
                new Vector3f(right, 1, depth), new Vector3f(right, 0, depth),
                UVPair.pack(sprite.getU(uStart), sprite.getV(1)), UVPair.pack(sprite.getU(uStart), sprite.getV(0)),
                UVPair.pack(sprite.getU(uEnd), sprite.getV(0)), UVPair.pack(sprite.getU(uEnd), sprite.getV(1)),
                Direction.NORTH, glassMaterial(sprite));
    }

    private static BakedQuad glassCap(TextureAtlasSprite sprite, Direction face, float left, float right) {
        float height = face == Direction.UP ? 1 : 0;
        return new BakedQuad(new Vector3f(left, height, 7f / 16), new Vector3f(left, height, 9f / 16),
                new Vector3f(right, height, 9f / 16), new Vector3f(right, height, 7f / 16),
                0, 0, 0, 0, face, glassMaterial(sprite));
    }

    private static BakedQuad.MaterialInfo glassMaterial(TextureAtlasSprite sprite) {
        return new BakedQuad.MaterialInfo(sprite, ChunkSectionLayer.TRANSLUCENT, null, -1, true, 0);
    }

    private static void testVegetationBuckets(BlockPos origin) {
        var world = new Fixture();
        BlockState[] plants = {
                Blocks.SHORT_GRASS.defaultBlockState(),
                Blocks.TALL_GRASS.defaultBlockState(),
                Blocks.SUGAR_CANE.defaultBlockState(),
                Blocks.DANDELION.defaultBlockState(),
                Blocks.WHEAT.defaultBlockState(),
                Blocks.VINE.defaultBlockState(),
                Blocks.LILY_PAD.defaultBlockState()
        };
        BlockState[] nonPlantCutouts = {
                Blocks.RAIL.defaultBlockState(),
                Blocks.REDSTONE_WIRE.defaultBlockState(),
                Blocks.COBWEB.defaultBlockState()
        };
        for (boolean enabled : new boolean[]{true, false}) {
            var capture = SectionCapture.begin(Map.of(), enabled);
            try {
                int vertex = 0;
                for (BlockState plant : plants) {
                    capture.beginBlock(world, origin, plant);
                    capture.selectLayer(ChunkSectionLayer.CUTOUT, vertex);
                    vertex += 4;
                }
                for (BlockState other : nonPlantCutouts) {
                    capture.beginBlock(world, origin, other);
                    capture.selectLayer(ChunkSectionLayer.CUTOUT, vertex);
                    vertex += 4;
                }
                capture.beginBlock(world, origin, Blocks.OAK_LEAVES.defaultBlockState());
                capture.selectLayer(ChunkSectionLayer.CUTOUT, vertex);
                vertex += 4;

                int[] counts = new int[BridgeWire.TERRAIN_BUCKETS];
                byte[] packed = capture.bucketsFor(ChunkSectionLayer.CUTOUT, vertex, counts);
                check(counts[BridgeWire.CUTOUT_NO_SHADOW] == (plants.length + nonPlantCutouts.length) * 4,
                        "plants and thin non-plant cutouts stay in CUTOUT_NO_SHADOW");
                check(counts[BridgeWire.FOLIAGE] == 4, "leaves stay in FOLIAGE");
                int expectedPlant = BridgeWire.CUTOUT_NO_SHADOW | (enabled ? BridgeWire.FACE_VEGETATION : 0);
                for (int i = 0; i < plants.length; i++) {
                    check((packed[i] & 0xff) == expectedPlant,
                            "plant " + plants[i] + " enabled=" + enabled + " entry=" + (packed[i] & 0xff));
                }
                for (int i = 0; i < nonPlantCutouts.length; i++) {
                    check((packed[plants.length + i] & 0xff) == BridgeWire.CUTOUT_NO_SHADOW,
                            "non-plant cutout must not set FACE_VEGETATION: " + nonPlantCutouts[i]);
                }
                check((packed[plants.length + nonPlantCutouts.length] & 0xff) == BridgeWire.FOLIAGE,
                        "leaves must not set FACE_VEGETATION");
            } finally {
                SectionCapture.end();
            }
        }
    }

    private static void testCoveredCopperBulb(BlockPos origin) {
        var world = new Fixture();
        for (Direction face : Direction.values()) {
            world.states.put(origin.relative(face), Blocks.NETHERRACK.defaultBlockState());
        }
        world.states.put(origin.below(), Blocks.MAGMA_BLOCK.defaultBlockState());
        var bulb = BuiltInRegistries.BLOCK.getValue(Identifier.withDefaultNamespace("waxed_oxidized_copper_bulb"))
                .defaultBlockState().setValue(BlockStateProperties.LIT, true);
        var trapdoor = Blocks.OAK_TRAPDOOR.defaultBlockState().setValue(BlockStateProperties.HALF, Half.BOTTOM);
        int topFace = SURFACE | face(Direction.UP);
        world.states.put(origin.above(), trapdoor);
        expect(world, origin, bulb, topFace | COVERED, "closed bottom trapdoor intersects the light's hover space");
        var capture = SectionCapture.begin(Map.of());
        try {
            capture.beginBlock(world, origin, bulb);
            var extended = capture.encodeLights(7, SectionPos.of(origin), true);
            var legacy = capture.encodeLights(7, SectionPos.of(origin), false);
            check(Byte.toUnsignedInt(extended.get(extended.limit() - 1)) == (topFace | COVERED), "covered face on the wire");
            check(Byte.toUnsignedInt(legacy.get(legacy.limit() - 1)) == topFace, "legacy face flags remain compatible");
        } finally {
            SectionCapture.end();
        }
        world.states.put(origin.above(), trapdoor.setValue(BlockStateProperties.HALF, Half.TOP));
        expect(world, origin, bulb, topFace, "top trapdoor is above the hover space");
        world.states.put(origin.above(), Blocks.GLASS.defaultBlockState());
        expect(world, origin, bulb, topFace, "transparent blocks retain the exposed light");
        world.states.remove(origin.above());
        expect(world, origin, bulb, topFace, "removing the cover restores the exposed light");
    }

    private static void testModelColors(BlockPos origin) {
        var name = Identifier.fromNamespaceAndPath("example", "block/blue_emitter");
        var contents = new SpriteContents(name, new FrameSize(1, 1), new NativeImage(1, 1, true));
        try (var sprite = new TextureAtlasSprite(TextureAtlas.LOCATION_BLOCKS, contents, 16, 16, 0, 0, 0) {}) {
            var material = new BakedQuad.MaterialInfo(sprite, ChunkSectionLayer.SOLID, null, -1, true, 0);
            var quad = new BakedQuad(new Vector3f(0, 1, 0), new Vector3f(0, 1, 1), new Vector3f(1, 1, 1),
                    new Vector3f(1, 1, 0), 0, 0, 0, 0, Direction.UP, material);
            var capture = SectionCapture.begin(Map.of(name, new TerrainEmission.Color(0, 0, 1)));
            try {
                var world = new Fixture();
                var emitter = Blocks.CRYING_OBSIDIAN.defaultBlockState();
                capture.beginBlock(world, origin, emitter);
                capture.putModelQuad(List.of(), (x, y, z, q, instance) -> {}, 0, 0, 0, quad, new QuadInstance());
                capture.beginBlock(world, origin.below(), Blocks.STONE.defaultBlockState());
                capture.putModelQuad(List.of(), (x, y, z, q, instance) -> {}, 0, 0, 0, quad, new QuadInstance());
                capture.beginBlock(world, origin.west(), emitter);

                var frame = capture.encodeLights(7, SectionPos.of(origin), true);
                frame.position(5 + 16);
                check(frame.getShort() == 2, "authored textures only light states with a nonzero MC light level");
                frame.getShort();
                check(readRgb(frame) == 0x0000ff, "the rendered model face overrides the vanilla palette");
                frame.position(frame.position() + 2);
                frame.getShort();
                int fallback = readRgb(frame);
                check((fallback & 255) > (fallback >>> 16) && ((fallback >>> 8) & 255) < 64,
                        "missing model emission retains a purple crying obsidian fallback");
            } finally {
                SectionCapture.end();
            }
        }
    }

    private static int readRgb(ByteBuffer frame) {
        return Byte.toUnsignedInt(frame.get()) << 16 | Byte.toUnsignedInt(frame.get()) << 8 | Byte.toUnsignedInt(frame.get());
    }

    private static void testMaterialLights(java.nio.file.Path output) throws Exception {
        var pos = new BlockPos(8, 1, 8);
        var world = new Fixture();
        String path = "block/test_material_lamp";
        try (var sprite = glassSprite(path)) {
            var tile = new SurfaceEmission.Tile(16, 16);
            // Black bars on top/bottom; equally bright red and blue windows.
            for (int y = 4; y < 12; y++) for (int x = 0; x < 16; x++) {
                tile.add(x, y, x < 8 ? 65025 : 0, 0, x < 8 ? 0 : 65025);
            }
            tile.finish();
            for (boolean mirror : List.of(false, true)) {
                var quad = glassFace(sprite, 0, 1, 0, mirror ? 1 : 0, mirror ? 0 : 1);
                var patches = tile.patches(quad);
                check(patches.size() == 2, "two contiguous, differently coloured windows");
                for (var p : patches) {
                    boolean red = p.energy().x > 0;
                    check(Math.abs(p.center().x - (red != mirror ? 0.25 : 0.75)) < 1e-5 &&
                            Math.abs(p.center().y - 0.5) < 1e-5 && p.normal().z < -0.99, "UVs locate each colour");
                    check(Math.abs(p.power() - 0.25) < 1e-5 && Math.abs(p.width() - 0.5) < 1e-5 &&
                            Math.abs(p.height() - 0.5) < 1e-5, "black border supplies no area or energy");
                }
                var colors = Map.of(Identifier.withDefaultNamespace(path), new TerrainEmission.Color(.25, 0, .25, tile));
                var capture = SectionCapture.begin(colors, false, true);
                try {
                    capture.beginBlock(world, pos, Blocks.SEA_LANTERN.defaultBlockState());
                    capture.putModelQuad(List.of(), (x,y,z,q,instance) -> capture.selectLayer(ChunkSectionLayer.TRANSLUCENT, 0),
                            8, 1, 8, quad, new QuadInstance());
                    var buckets = capture.bucketsFor(ChunkSectionLayer.TRANSLUCENT, 4, new int[BridgeWire.TERRAIN_BUCKETS]);
                    check((buckets[0] & SectionCapture.MATERIAL_LIGHT_BIT) != 0, "material emitter suppresses block fill on the lamp itself");
                    var frame = capture.encodeLights(1, SectionPos.of(pos), true);
                    int levelOffset = 5 + BridgeWire.SECTION_LIGHTS_HEADER_BYTES + 5;
                    check(frame.get(levelOffset) == (byte) 0x8f && frame.get(levelOffset + 2) == 2 &&
                            frame.remaining() == levelOffset + 3 + 2 * SurfaceEmission.PATCH_BYTES, "bounded wire records");
                    var limited = capture.encodeLights(1, SectionPos.of(pos), true, BridgeWire.SECTION_LIGHTS_HEADER_BYTES + 7);
                    check(limited.get(levelOffset) == 15 && limited.remaining() == levelOffset + 2, "full lane fallback");
                    if (output != null) {
                        java.nio.file.Files.createDirectories(output);
                        byte[] bytes = new byte[frame.remaining()];
                        frame.get(bytes);
                        java.nio.file.Files.write(output.resolve(mirror ? "lamp-mirror.bin" : "lamp.bin"), bytes);
                    }
                } finally { SectionCapture.end(); }
            }
            for (boolean enabled : List.of(false, true)) {
                var authored = new TerrainEmission.Color(.25, 0, .25, tile);
                var capture = SectionCapture.begin(Map.of(Identifier.withDefaultNamespace(path), authored), false, enabled);
                try {
                    capture.beginBlock(world, pos, Blocks.STONE.defaultBlockState());
                    var quad = glassFace(sprite, 0, 1, 0, 0, 1);
                    capture.putModelQuad(List.of(), (x,y,z,q,instance) -> {}, 8.125f, 1.25f, 7.75f, quad, new QuadInstance());
                    var frame = capture.encodeLights(1, SectionPos.of(pos), true);
                    int headerEnd = 5 + BridgeWire.SECTION_LIGHTS_HEADER_BYTES;
                    check(frame.getShort(headerEnd - 2) == (enabled ? 1 : 0), "resource-pack lamp works on MC level zero when negotiated");
                    if (enabled) {
                        int first = headerEnd + BridgeWire.SECTION_LIGHT_BYTES + 1;
                        check(frame.get(headerEnd + 5) == (byte) 0x8f && frame.getShort(first + 2) == 768 &&
                                frame.getShort(first + 4) == -256, "light follows the model's section-local translation");
                    }
                } finally { SectionCapture.end(); }
            }
            var dark = new SurfaceEmission.Tile(16, 16);
            var capture = SectionCapture.begin(Map.of(Identifier.withDefaultNamespace(path),
                    new TerrainEmission.Color(0, 0, 0, dark)), false, true);
            try {
                capture.beginBlock(world, pos, Blocks.SEA_LANTERN.defaultBlockState());
                capture.putModelQuad(List.of(), (x,y,z,q,instance) -> {}, 8, 1, 8, glassFace(sprite, 0, 1, 0, 0, 1), new QuadInstance());
                var frame = capture.encodeLights(1, SectionPos.of(pos), true);
                check(frame.get(frame.limit() - 1) == 0 && frame.get(frame.limit() - 3) == (byte) 0x8f,
                        "explicit black mask suppresses the legacy block lamp");
            } finally { SectionCapture.end(); }
        }
    }

    private static int encodedCount(Fixture world, BlockPos pos, BlockState state) {
        SectionCapture capture = SectionCapture.begin(Map.of());
        try {
            capture.beginBlock(world, pos, state);
            return Short.toUnsignedInt(capture.encodeLights(7, SectionPos.of(pos), true).getShort(5 + 16));
        } finally {
            SectionCapture.end();
        }
    }

    private static int face(Direction face) {
        return 1 << face.get3DDataValue();
    }

    private static void expect(Fixture world, BlockPos pos, BlockState state, int flags, String label) {
        int actual = SectionCapture.lightFaces(world, pos, state);
        check(actual == flags, label + " expected=" + flags + " actual=" + actual);
    }

    private static void check(boolean value, String message) {
        if (!value) {
            throw new AssertionError(message);
        }
    }

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
