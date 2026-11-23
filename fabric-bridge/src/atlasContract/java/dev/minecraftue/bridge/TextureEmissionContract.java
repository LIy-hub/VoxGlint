package dev.minecraftue.bridge;

import com.google.gson.GsonBuilder;
import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.blaze3d.vertex.QuadInstance;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.renderer.texture.SpriteContents;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.client.resources.metadata.animation.FrameSize;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.client.resources.model.sprite.Material;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.BlockStateProperties;
import net.minecraft.world.phys.Vec3;

import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.BitSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.stream.IntStream;

/** Uses the same atlas bytes, section classification and live light packets in Java and the UE replay. */
final class TextureEmissionContract {
    private static final List<String> SLOTS = List.of("glowstone", "sea_lantern", "redstone_ore",
            "jack_o_lantern", "carved_pumpkin", "stone", "firefly_bush_emissive", "animated_lamp");
    private static final int EDGE = 16, WIDTH = EDGE * SLOTS.size();

    static void verify(Path dir, BlockGetter world) throws Exception {
        Files.createDirectories(dir);
        var sprites = new ArrayList<TerrainAtlas.Sprite>();
        var models = new ArrayList<TextureAtlasSprite>();
        var report = new LinkedHashMap<String,Object>();
        try (var resources = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of())) {
            for (String name : SLOTS) {
                var id = name.equals("animated_lamp") ? Identifier.fromNamespaceAndPath("fixture", "block/" + name)
                        : Identifier.withDefaultNamespace("block/" + name);
                NativeImage image;
                if (name.equals("animated_lamp")) {
                    image = new NativeImage(EDGE, 2 * EDGE, true);
                    for (int y = 0; y < 2 * EDGE; y++) for (int x = 0; x < EDGE; x++) {
                        image.setPixel(x, y, y < EDGE ? 0xffee2909 : 0xff0934ed);
                    }
                } else {
                    try (var in = TextureEmissionContract.class.getResourceAsStream("/assets/minecraft/textures/" + id.getPath() + ".png")) {
                        image = NativeImage.read(Objects.requireNonNull(in));
                    }
                }
                int x = EDGE * sprites.size();
                var frames = IntStream.range(0, image.getHeight() / EDGE).toArray();
                sprites.add(new TerrainAtlas.Sprite(id, new NativeImage[]{image}, x, 0, EDGE, EDGE, 0, 0, 0, frames));
                var contents = new SpriteContents(id, new FrameSize(EDGE, EDGE), image);
                models.add(new TextureAtlasSprite(TextureAtlas.LOCATION_BLOCKS, contents, WIDTH, EDGE, x, 0, 0) {});
            }
            try (var atlas = TerrainAtlasCache.prepare(null, new TerrainAtlasCache.Layout(WIDTH, EDGE, 1), sprites,
                    resources, TerrainReliefSettings.DISABLED)) {
                int bytes = WIDTH * EDGE * 4;
                byte[] pbr = null;
                for (int i = 0; i < TerrainAtlas.TEXTURES.length; i++) {
                    var buffer = ByteBuffer.allocate(bytes);
                    TerrainAtlasCache.read(atlas.channel, buffer, TerrainAtlasCache.HEADER_BYTES + (long) i * bytes);
                    Files.write(dir.resolve("atlas-" + TerrainAtlas.TEXTURES[i] + ".bgra"), buffer.array());
                    if (i == 2) pbr = buffer.array();
                }
                Objects.requireNonNull(pbr);
                for (int slot = 0; slot < SLOTS.size(); slot++) {
                    var sprite = sprites.get(slot);
                    var mode = atlas.emissionColors.get(sprite.name()).mode();
                    var levels = new BitSet(256);
                    int moving = 0, borderMax = 0;
                    for (int y = 0; y < EDGE; y++) for (int x = 0; x < EDGE; x++) {
                        int packed = Byte.toUnsignedInt(pbr[4 * (y * WIDTH + sprite.x() + x)]);
                        int min = 255, max = 0;
                        for (int frame : sprite.frames()) {
                            int pixel = sprite.images()[0].getPixel(sprite.frameX(frame) + x, sprite.frameY(frame) + y);
                            double weight = TextureEmission.weight(mode, packed / 255.0, TerrainEmission.linear(pixel >> 16 & 255),
                                    TerrainEmission.linear(pixel >> 8 & 255), TerrainEmission.linear(pixel & 255));
                            int strength = (int) Math.round(weight * (pixel >>> 24));
                            min = Math.min(min, strength);
                            max = Math.max(max, strength);
                            levels.set(strength);
                        }
                        if (max != min) moving++;
                        if (Math.min(Math.min(x, y), Math.min(EDGE-1-x, EDGE-1-y)) < 2) borderMax = Math.max(max, borderMax);
                    }
                    String name = SLOTS.get(slot);
                    report.put(name, Map.of("frames", sprite.frames().length, "mode", mode, "levels", levels.cardinality(),
                            "min", levels.nextSetBit(0), "max", levels.length()-1, "movingTexels", moving, "borderMax", borderMax));
                    if (name.equals("glowstone")) require(levels.nextSetBit(0) > 0 && levels.get(255) && levels.cardinality() >= 5, "glowstone substrate and contrast");
                    if (name.equals("sea_lantern")) require(sprite.frames().length == 5 && borderMax == 5 && levels.nextSetBit(0) == 5
                            && moving > 30 && levels.cardinality() >= 6, "sea lantern frame and casing");
                    if (name.equals("firefly_bush_emissive")) require(moving != 0, "dark animation frames must remain eligible");
                    if (sprite.frames().length > 1) {
                        for (int frame : sprite.frames()) {
                            ByteBuffer bgra = ByteBuffer.allocate(EDGE * EDGE * 4);
                            for (int y = 0; y < EDGE; y++) for (int x = 0; x < EDGE; x++) {
                                int pixel = sprite.images()[0].getPixel(sprite.frameX(frame)+x,sprite.frameY(frame)+y);
                                bgra.put((byte)pixel).put((byte)(pixel >> 8)).put((byte)(pixel >> 16)).put((byte)(pixel >>> 24));
                            }
                            Files.write(dir.resolve(name + "-" + frame + ".bgra"), bgra.array());
                        }
                    }
                }
                var states = List.of(Blocks.GLOWSTONE.defaultBlockState(), Blocks.SEA_LANTERN.defaultBlockState(),
                        Blocks.REDSTONE_ORE.defaultBlockState().setValue(BlockStateProperties.LIT,true),
                        Blocks.REDSTONE_ORE.defaultBlockState(), Blocks.STONE.defaultBlockState());
                int[] slot = {0,1,2,2,5};
                var capture = SectionCapture.begin(atlas.emissionColors, false, true);
                try {
                    for (int i = 0; i < states.size(); i++) {
                        captureFace(capture, world, new BlockPos(6+i,1,8), states.get(i), models.get(slot[i]), i*4);
                    }
                    byte[] levels = new byte[states.size()];
                    byte[] buckets = capture.bucketsFor(ChunkSectionLayer.SOLID, states.size()*4, new int[8], levels);
                    int[] modes = {TextureEmission.PACKED,TextureEmission.SEA,TextureEmission.PACKED,TextureEmission.ORE_DIM,TextureEmission.PACKED};
                    for (int i=0; i<states.size(); i++) {
                        require(((buckets[i] & 0x38) >> 3) == modes[i] && levels[i] == states.get(i).getLightEmission(),
                                "state " + states.get(i) + " mode/level " + (buckets[i] & 0x38) + "/" + levels[i]);
                    }
                    Files.write(dir.resolve("face-flags.bin"), buckets);
                    Files.write(dir.resolve("face-levels.bin"), levels);
                } finally { SectionCapture.end(); }
                capture = SectionCapture.begin(atlas.emissionColors, false, true);
                try {
                    captureFace(capture, world, new BlockPos(8,1,8), Blocks.SEA_LANTERN.defaultBlockState(), models.get(7), 0);
                    var frame = capture.encodeLights(1, SectionPos.of(0,0,0), true);
                    byte[] wire = new byte[frame.remaining()];
                    frame.get(wire);
                    require((wire[28] & 0xc0) == 0xc0, "animated emitter has no UV footprint");
                    Files.write(dir.resolve("animated-lights.bin"), wire);
                } finally { SectionCapture.end(); }
                verifyModelElement(atlas, world, models.get(7), models.get(5));
            }
        } finally { models.forEach(TextureAtlasSprite::close); }
        for (double shade : new double[]{.12,.46,.70,.83,1.0}) {
            double previous = 0;
            for (int gate=0; gate<=255; gate++) {
                double weight = TextureEmission.weight(TextureEmission.SEA,gate/255.0,shade,shade,shade);
                require(weight >= previous,"filtering a sea lantern edge creates a spurious bright outline");
                previous = weight;
            }
        }
        verifyLava(world);
        OreEmissionContract.verify(dir, world);
        Files.writeString(dir.resolve("texture-measurements.json"), new GsonBuilder().setPrettyPrinting().create().toJson(report));
        new VanillaTextureCoverage().write(dir);
        System.out.println("Texture emission: " + report);
    }

    static void captureFace(SectionCapture capture, BlockGetter world, BlockPos p, BlockState state, TextureAtlasSprite sprite, int vertex) {
        capture.beginBlock(world,p,state);
        var quad = VanillaEmitterFixture.face(sprite,null,Direction.NORTH);
        capture.putModelQuad(List.of(), (x,y,z,q,c) -> capture.selectLayer(ChunkSectionLayer.SOLID,vertex),
                p.getX(),p.getY(),p.getZ(),quad,new QuadInstance());
    }

    private static void verifyModelElement(TerrainAtlasCache.Entry atlas, BlockGetter world,
                                           TextureAtlasSprite lens, TextureAtlasSprite chassis) {
        var source = VanillaEmitterFixture.face(lens, null, Direction.NORTH);
        var emitter = new BakedQuad(source.position0(),source.position1(),source.position2(),source.position3(),
                source.packedUV0(),source.packedUV1(),source.packedUV2(),source.packedUV3(),source.direction(),
                new BakedQuad.MaterialInfo(lens,ChunkSectionLayer.SOLID,null,-1,true,11));
        var base = VanillaEmitterFixture.face(chassis,null,Direction.SOUTH);
        var part = new BlockStateModelPart() {
            public List<BakedQuad> getQuads(Direction face) { return face == null ? List.of(base,emitter) : List.of(); }
            public boolean useAmbientOcclusion() { return true; }
            public Material.Baked particleMaterial() { return null; }
            public int materialFlags() { return 0; }
        };
        var capture = SectionCapture.begin(atlas.emissionColors,false,true);
        try {
            var states = List.of(Blocks.STONE.defaultBlockState(),Blocks.SEA_LANTERN.defaultBlockState());
            for (int i=0; i<states.size(); i++) {
                capture.beginBlock(world,new BlockPos(4+i,1,8),states.get(i));
                int first = i*8;
                capture.putModelQuad(List.of(part),(x,y,z,q,c)->capture.selectLayer(ChunkSectionLayer.SOLID,first),
                        4+i,1,8,base,new QuadInstance());
                capture.putModelQuad(List.of(part),(x,y,z,q,c)->capture.selectLayer(ChunkSectionLayer.SOLID,first+4),
                        4+i,1,8,emitter,new QuadInstance());
            }
            byte[] levels = new byte[4];
            byte[] faces = capture.bucketsFor(ChunkSectionLayer.SOLID,16,new int[8],levels);
            require(levels[0] == 0 && levels[1] == 11 && levels[2] == 0 && levels[3] == 15, "model emitter intrinsic level");
            require((faces[2] & 0x38) == TextureEmission.DARK << 3 && (faces[3] & 0x38) == TextureEmission.AUTOMATIC << 3,
                    "declared lens must exclude its machine chassis");
            var wire = capture.encodeLights(1,SectionPos.of(0,0,0),true);
            require(wire.getShort(21) == 2, "including a model-only emitter on an otherwise unlit block");
        } finally { SectionCapture.end(); }
    }

    private static void verifyLava(BlockGetter world) throws Exception {
        var sprites = new ArrayList<TerrainAtlas.Sprite>();
        try (var resources = new MultiPackResourceManager(PackType.CLIENT_RESOURCES,List.of())) {
            for (String name : List.of("lava_still","lava_flow")) {
                var id = Identifier.withDefaultNamespace("block/" + name);
                try (var in = TextureEmissionContract.class.getResourceAsStream("/assets/minecraft/textures/" + id.getPath() + ".png")) {
                    var image = NativeImage.read(Objects.requireNonNull(in));
                    int edge = image.getWidth();
                    sprites.add(new TerrainAtlas.Sprite(id,new NativeImage[]{image},sprites.isEmpty() ? 0 : 32,0,
                            edge,edge,0,0,0,IntStream.range(0,image.getHeight()/edge).toArray()));
                }
            }
            try (var atlas = TerrainAtlasCache.prepare(null,new TerrainAtlasCache.Layout(64,32,1),sprites,
                    resources,TerrainReliefSettings.DISABLED)) {
                var lava = Blocks.LAVA.defaultBlockState();
                var capture = SectionCapture.begin(atlas.emissionColors,false,true);
                try {
                    capture.beginBlock(world,new BlockPos(5,1,4),lava);
                    capture.setFluid(lava.getFluidState());
                    capture.selectLayer(ChunkSectionLayer.SOLID,0);
                    int owner = capture.lavaOwner(ChunkSectionLayer.SOLID,0);
                    capture.setFluid(null);
                    capture.selectLayer(ChunkSectionLayer.SOLID,4);
                    require(owner >= 0 && capture.lavaOwner(ChunkSectionLayer.SOLID,1) == -1, "lava source span");
                    var face = new Vec3[]{new Vec3(5,1.75,4), new Vec3(5,1.875,5),
                            new Vec3(6,1.875,5),new Vec3(6,1.75,4)};
                    int[] uv = {0x90001000,0x9000c000,0xe000c000,0xe0001000};
                    int mode = capture.addLavaFace(owner,face,uv,Direction.UP.get3DDataValue());
                    require(mode == TextureEmission.LIVE, "flowing texture uses the current frame");
                    var wire = capture.encodeLights(1,SectionPos.of(0,0,0),true);
                    int patchStart = 35;
                    require((wire.get(28) & 0xc0)==0xc0 && wire.get(30)==1 && wire.getInt(31)==1, "lava live surface record");
                    require(wire.getShort(patchStart+2)==832, "fluid surface height on a slope");
                    require(wire.getInt(patchStart+23)==uv[0], "actual fluid renderer UV");
                } finally { SectionCapture.end(); }
            }
        } finally { sprites.forEach(sprite -> sprite.images()[0].close()); }
    }

    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
}
