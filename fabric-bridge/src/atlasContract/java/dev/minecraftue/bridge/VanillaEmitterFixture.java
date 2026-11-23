package dev.minecraftue.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.mojang.blaze3d.platform.NativeImage;
import com.mojang.blaze3d.vertex.QuadInstance;
import net.minecraft.client.model.geom.builders.UVPair;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.texture.SpriteContents;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.client.resources.metadata.animation.FrameSize;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.block.Blocks;
import org.joml.Vector3f;

import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

/** Stock texture samples and body-model UVs, sent through the production capture and shared-memory encoder. */
final class VanillaEmitterFixture {
    private static final String[] NAMES = {"magma", "lantern", "torch"};
    // Corners as xyz bit masks. Side faces run bottom-left, bottom-right, top-right, top-left.
    private static final int[][] CORNERS = {{0,4,5,1}, {2,3,7,6}, {4,0,2,6}, {1,5,7,3}, {0,1,3,2}, {5,4,6,7}};

    static void write(Path output, BlockGetter world) throws Exception {
        var textures = new ArrayList<TerrainAtlas.Sprite>();
        var renderSprites = new ArrayList<TextureAtlasSprite>();
        try (var resources = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of())) {
            int x = 16; // Slot zero is the neutral floor in validate_material_lights.py.
            for (String name : NAMES) {
                NativeImage image;
                try (var input = VanillaEmitterFixture.class.getResourceAsStream("/assets/minecraft/textures/block/" + name + ".png")) {
                    image = NativeImage.read(Objects.requireNonNull(input));
                }
                var id = Identifier.withDefaultNamespace("block/" + name);
                int[] frames = java.util.stream.IntStream.range(0, image.getHeight() / 16).toArray();
                textures.add(new TerrainAtlas.Sprite(id, new NativeImage[]{image}, x, 0, 16, 16, 0, 0, 0, frames));
                var contents = new SpriteContents(id, new FrameSize(16, 16), image);
                renderSprites.add(new TextureAtlasSprite(TextureAtlas.LOCATION_BLOCKS, contents, 128, 16, x, 0, 0) {});
                x += 16;
            }
            try (var atlas = TerrainAtlasCache.prepare(null, new TerrainAtlasCache.Layout(128, 16, 1),
                    textures, resources, TerrainReliefSettings.DISABLED)) {
                var capture = SectionCapture.begin(atlas.emissionColors, false, true);
                try {
                    var states = List.of(Blocks.MAGMA_BLOCK.defaultBlockState(), Blocks.LANTERN.defaultBlockState(),
                            Blocks.TORCH.defaultBlockState());
                    for (int i = 0; i < NAMES.length; i++) {
                        capture.beginBlock(world, new BlockPos(7+i, 1, 8), states.get(i));
                        JsonObject body = null;
                        if (i > 0) {
                            String file = "/assets/minecraft/models/block/template_" + NAMES[i] + ".json";
                            try (var reader = new InputStreamReader(Objects.requireNonNull(
                                    VanillaEmitterFixture.class.getResourceAsStream(file)), StandardCharsets.UTF_8)) {
                                body = JsonParser.parseReader(reader).getAsJsonObject().getAsJsonArray("elements").get(0).getAsJsonObject();
                            }
                        }
                        for (Direction face : Direction.values()) {
                            BakedQuad quad = face(renderSprites.get(i), body, face);
                            var patches = atlas.emissionColors.get(textures.get(i).name()).tile().patches(quad);
                            if (i == 1 && face.getAxis() == Direction.Axis.Y && !patches.isEmpty())
                                throw new AssertionError("lantern metal base and lid emitted light");
                            if (i == 2 && patches.stream().anyMatch(p -> p.center().y < 0.5))
                                throw new AssertionError("torch proxy on its shaft");
                            capture.putModelQuad(List.of(), (x0,y,z,q,instance) -> {}, 7+i, 1, 8, quad, new QuadInstance());
                        }
                    }
                    var wire = capture.encodeLights(1, SectionPos.of(0, 0, 0), true);
                    var bytes = new byte[wire.remaining()];
                    wire.get(bytes);
                    Files.write(output.resolve("vanilla-lights.bin"), bytes);
                } finally { SectionCapture.end(); }
            }
        } finally { renderSprites.forEach(TextureAtlasSprite::close); }
    }

    static BakedQuad face(TextureAtlasSprite sprite, JsonObject body, Direction direction) {
        JsonArray uv = body == null ? null : body.getAsJsonObject("faces").getAsJsonObject(direction.getSerializedName()).getAsJsonArray("uv");
        Vector3f[] p = new Vector3f[4];
        long[] tex = new long[4];
        for (int i = 0; i < 4; i++) {
            int corner = CORNERS[direction.get3DDataValue()][i];
            p[i] = new Vector3f();
            for (int axis = 0; axis < 3; axis++) {
                boolean high = (corner & (4 >> axis)) != 0;
                float value = body == null ? (high ? 1 : 0) : body.getAsJsonArray(high ? "to" : "from").get(axis).getAsFloat() / 16;
                p[i].setComponent(axis, value);
            }
            boolean right = i == 1 || i == 2, bottom = i < 2;
            float u = uv == null ? (right ? 1 : 0) : uv.get(right ? 2 : 0).getAsFloat()/16;
            float v = uv == null ? (bottom ? 1 : 0) : uv.get(bottom ? 3 : 1).getAsFloat()/16;
            tex[i] = UVPair.pack(sprite.getU(u), sprite.getV(v));
        }
        return new BakedQuad(p[0],p[1],p[2],p[3],tex[0],tex[1],tex[2],tex[3],direction,
                new BakedQuad.MaterialInfo(sprite, ChunkSectionLayer.SOLID, null, -1, true, 0));
    }
}
