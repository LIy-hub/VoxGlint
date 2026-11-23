package dev.minecraftue.bridge;

import com.mojang.blaze3d.platform.NativeImage;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.PackLocationInfo;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.PathPackResources;
import net.minecraft.server.packs.repository.PackSource;
import net.minecraft.server.packs.resources.MultiPackResourceManager;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.nio.file.StandardOpenOption;
import java.util.List;
import java.util.Optional;

final class TerrainAtlasContract {
    private static final int ALBEDO = 0x91234065;
    private static final TerrainReliefSettings RELIEF = Boolean.getBoolean("minecraftue.gradientNormals")
            ? TerrainReliefSettings.DEFAULT : TerrainReliefSettings.DISABLED;

    public static void main(String[] args) throws Exception {
        check(BridgeWire.NORMAL_ATLAS_ROWS == 27 && BridgeWire.PBR_ATLAS_ROWS == 28, "terrain atlas packet types");
        check(LabPbr.decodeSpecular(0xff80e500, 71) == 0x473f0000, "roughness decode, dielectric and alpha 255");
        check(LabPbr.decodeSpecular(0xfe80e600, 71) == 0x473fffff, "first hardcoded metal and full emission");
        check(LabPbr.decodeSpecular(0x7fff_ff00, 255) == 0xff00ff80, "custom metal and half emission");
        check(LabPbr.decodeNormal(0x0080_ee00) == LabPbr.decodeNormal(0xff80_eeff), "normal ignores height/AO");
        check(((LabPbr.decodeNormal(0x0080_ee00) >>> 8) & 255) == 238, "LabPBR positive Y stays down");
        check((LabPbr.decodeNormal(0x00ff_ff00) & 255) == 128, "out-of-disk XY normalizes with Z=0");
        check(LabPbr.decodePorosity(0x0000_0040) == 255 && LabPbr.decodePorosity(0x0000_0020) == 128
                && LabPbr.decodePorosity(0x0000_0041) == 0, "LabPBR porosity decode and SSS exclusion");
        testResourcePack(Path.of(args[0]));
        testWaterNormals(Path.of(args[0]).resolve("water-normals"));
        testPatternedEmission(Path.of(args[0]).resolve("patterned-emission"));
        testAuthoredMineralEmission(Path.of(args[0]).resolve("authored-mineral-emission"));
        testLampHardware(Path.of(args[0]).resolve("material-lights"));
        testReliefSettings(Path.of(args[0]).resolve("relief-settings.properties"));
        testGeneratedNormals(Path.of(args[0]).resolve("generated-normals"));
        testAtlasPatch();
        System.out.println("Terrain atlas contract passed");
    }

    private static void testPatternedEmission(Path folder) throws Exception {
        try (var magma = vanillaAlbedo("magma");
             var obsidian = vanillaAlbedo("crying_obsidian");
             var custom = new NativeImage(4, 4, false);
             var empty = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of())) {
            var magmaSprite = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/magma"),
                    new NativeImage[] {magma}, 0, 0, 16, 16, 0, 0, 0, new int[] {0, 1, 2});
            var obsidianSprite = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/crying_obsidian"),
                    new NativeImage[] {obsidian}, 16, 0, 16, 16, 0, 0, 0);
            var sprites = List.of(magmaSprite, obsidianSprite);
            var layout = new TerrainAtlasCache.Layout(64, 32, 1);
            try (var materials = new TerrainMaterialAtlas(sprites, empty, 1, RELIEF)) {
                check(countEmissive(materials.tile(BridgeWire.PBR_ATLAS_ROWS, 0), 256) == 96,
                        "magma cracks occupy the same 96 pixels across the actual animation");
                check(countEmissive(materials.tile(BridgeWire.PBR_ATLAS_ROWS, 1), 256) == 65,
                        "crying obsidian emits from the saturated purple veins");
            }

            byte[] key = TerrainAtlasCache.fingerprint(layout, sprites, empty, RELIEF);
            magma.setPixel(0, 16, magma.getPixel(0, 16) ^ 0x00000001);
            check(!Arrays.equals(key, TerrainAtlasCache.fingerprint(layout, sprites, empty, RELIEF)),
                    "magma changes outside the initial frame invalidate the static mask cache");

            Path cache = Files.createDirectories(folder.resolve("cache"));
            try (var cold = TerrainAtlasCache.prepare(cache, layout, sprites, empty, RELIEF);
                 var warm = TerrainAtlasCache.prepare(cache, layout, sprites, empty, RELIEF)) {
                var purple = cold.emissionColors.get(obsidianSprite.name());
                var orange = cold.emissionColors.get(magmaSprite.name());
                check(cold.shared && cold.emissionColors.equals(warm.emissionColors), "cold/warm emissive colors match");
                check(purple.blue() > purple.red() * 3.0 && purple.red() > purple.green() * 5.0,
                        "purple veins set the light hue");
                check(orange.red() > orange.green() * 3.0 && orange.green() > orange.blue() * 5.0,
                        "orange cracks set the light hue");
                System.out.printf("Vanilla emitter RGB: crying obsidian #%06X, magma #%06X%n",
                        TerrainEmission.toRgb(purple.red(), purple.green(), purple.blue(), 0),
                        TerrainEmission.toRgb(orange.red(), orange.green(), orange.blue(), 0));
            }

            Path overrides = folder.resolve("overrides");
            write(overrides, "crying_obsidian_s", 16, 0xff00_0000, 0xff00_0000);
            write(overrides, "example", "lamp_s", 4, 0xff00_0000, 0xfe00_0000);
            custom.fillRect(0, 0, 4, 4, 0xffff_1100);
            custom.fillRect(2, 2, 2, 2, 0xff00_66ff);
            var customSprite = new TerrainAtlas.Sprite(Identifier.fromNamespaceAndPath("example", "block/lamp"),
                    new NativeImage[] {custom}, 32, 0, 4, 4, 0, 0, 0);
            try (var packs = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of(pack(overrides)));
                 var prepared = TerrainAtlasCache.prepare(cache, layout, List.of(obsidianSprite, customSprite), packs, RELIEF)) {
                check(prepared.emissionColors.get(obsidianSprite.name()).blue() == 0,
                        "authored zero emission wins over the crying obsidian mask");
                var blue = prepared.emissionColors.get(customSprite.name());
                check(TerrainEmission.toRgb(blue.red(), blue.green(), blue.blue(), 0) == 0x0066ff,
                        "modded light samples only the declared emissive pixels");
            }
        }
    }

    private static void testAuthoredMineralEmission(Path folder) throws Exception {
        write(folder,"sea_lantern_s",16,0xff00_0000,0xff00_0000);
        write(folder,"example","diamond_ore_s",16,0xff00_0000,0xff00_0000);
        try (var sea=vanillaAlbedo("sea_lantern"); var ore=vanillaAlbedo("diamond_ore");
             var packs=new MultiPackResourceManager(PackType.CLIENT_RESOURCES,List.of(pack(folder)))) {
            var seaId=Identifier.withDefaultNamespace("block/sea_lantern");
            var oreId=Identifier.fromNamespaceAndPath("example","block/diamond_ore");
            var sprites=List.of(new TerrainAtlas.Sprite(seaId,new NativeImage[]{sea},0,0,16,16,0,0,0,new int[]{0,1,2,3,4}),
                    new TerrainAtlas.Sprite(oreId,new NativeImage[]{ore},16,0,16,16,0,0,0));
            try (var atlas=TerrainAtlasCache.prepare(null,new TerrainAtlasCache.Layout(32,16,1),sprites,packs,RELIEF)) {
                for (var sprite : sprites) {
                    var color=atlas.emissionColors.get(sprite.name());
                    check(color.authored() && !color.ore() && color.mode()==TextureEmission.PACKED && !color.tile().hasEmission(),
                            "an authored dark mask overrides the luminous body and the inferred mineral grains");
                }
            }
        }
    }

    private static NativeImage vanillaAlbedo(String texture) throws Exception {
        try (var input = TerrainAtlasContract.class.getResourceAsStream("/assets/minecraft/textures/block/" + texture + ".png")) {
            check(input != null, "vanilla texture " + texture);
            return NativeImage.read(input);
        }
    }

    private static void testLampHardware(Path folder) throws Exception {
        Files.createDirectories(folder);
        for (String name : List.of("magma", "lantern", "soul_lantern", "copper_lantern", "oxidized_copper_lantern",
                "torch", "soul_torch", "copper_torch", "redstone_torch")) {
            try (var image = vanillaAlbedo(name);
                 var packs = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of())) {
                int[] frames = java.util.stream.IntStream.range(0, image.getHeight() / 16).toArray();
                var sprite = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/" + name),
                        new NativeImage[]{image}, 0, 0, 16, 16, 0, 0, 0, frames);
                try (var maps = new TerrainMaterialAtlas(List.of(sprite), packs, 1, RELIEF)) {
                    var tile = maps.tile(BridgeWire.PBR_ATLAS_ROWS, 0);
                    var mask = new BufferedImage(16, 16, BufferedImage.TYPE_INT_ARGB);
                    int active = 0;
                    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
                        int e = tile.sample(0, y * 16 + x) & 255;
                        mask.setRGB(x, y, 0xff000000 | e * 0x010101);
                        if (e > 0) active++;
                        if (name.contains("lantern")) {
                            check((e != 0) == (x >= 1 && x <= 4 && y >= 3 && y <= 7), name + " window / metal");
                        } else if (name.contains("torch")) {
                            check(y < 9 || e == 0, name + " shaft stays dark");
                            check((tile.sample(0, 7 * 16 + 7) & 255) != 0, name + " head is emissive");
                        }
                    }
                    int expected = name.equals("magma") ? 96 : name.contains("lantern") ? 20 : name.equals("redstone_torch") ? 16 : 4;
                    check(active == expected, name + " texel coverage");
                    ImageIO.write(mask, "PNG", folder.resolve(name + "-mask.png").toFile());
                }
            }
        }
    }

    private static int countEmissive(TerrainMaterialAtlas.Tile tile, int pixels) {
        int count = 0;
        for (int i = 0; i < pixels; i++) {
            if ((tile.sample(0, i) & 255) != 0) {
                count++;
            }
        }
        return count;
    }

    private static void testReliefSettings(Path path) throws Exception {
        Files.writeString(path, "hudMaxFps=90\ndebugPanel=on\ngradientNormals=on\n");
        var legacy = BridgeSettings.read(path);
        check(legacy.hudMaxFps() == 90 && legacy.debugPanel() == BridgeSettings.Toggle.ON
                        && legacy.effectiveRelief().equals(TerrainReliefSettings.DEFAULT),
                "legacy settings keep their controls and use the current relief defaults");
        var customized = new BridgeSettings(legacy.hudMaxFps(), legacy.debugPanel(), BridgeSettings.Toggle.OFF,
                new TerrainReliefSettings(160, 8, 18));
        customized.save(path);
        var saved = BridgeSettings.read(path);
        check(saved.equals(customized) && saved.effectiveRelief().equals(TerrainReliefSettings.DISABLED),
                "disabled relief retains the instance's slider settings");
        Files.writeString(path, "gradientNormals=on\nreliefStrengthPercent=55\nreliefThresholdTenthsPercent=invalid\n"
                + "reliefMaxTiltDegrees=90\n");
        check(BridgeSettings.read(path).effectiveRelief().equals(TerrainReliefSettings.DEFAULT.withStrength(55)),
                "invalid relief fields revert individually to the defaults");
    }

    private static void testGeneratedNormals(Path folder) throws Exception {
        int authored = 0xff80_ae40;
        write(folder, "stone_bricks_n", 8, authored, authored);
        try (var resources = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of(pack(folder)));
             var sheet = new NativeImage(32, 8, false);
             var mip = new NativeImage(16, 4, true)) {
            sheet.fillRect(0, 0, 32, 8, 0xff30_3030);
            sheet.fillRect(4, 0, 4, 8, 0xff60_6060);
            sheet.fillRect(12, 0, 4, 8, 0xff34_3434);
            sheet.fillRect(28, 0, 4, 8, 0xff60_6060);
            sheet.setPixel(24, 0, 0);
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    sheet.setPixel(16 + x, y, x + y >= 7 ? 0xffff_ffff : 0xff00_0000);
                }
            }
            var images = new NativeImage[] {sheet, mip};
            var cobble = reliefSprite("cobblestone", images, 0);
            var animated = new TerrainAtlas.Sprite(cobble.name(), images, 0, 0, 8, 8, 0, 0, 0, new int[] {0, 1});
            var sprites = List.of(cobble, reliefSprite("oak_planks", images, 0), reliefSprite("stone", images, 0),
                    reliefSprite("grass_block_top", images, 0), reliefSprite("oak_leaves", images, 0),
                    reliefSprite("bricks", images, 1), reliefSprite("bricks", images, 2),
                    reliefSprite("bricks", images, 3), animated, reliefSprite("stone_bricks", images, 0));
            var layout = new TerrainAtlasCache.Layout(8, 8, 2);
            check(!Arrays.equals(TerrainAtlasCache.fingerprint(layout, List.of(cobble), resources, RELIEF),
                    TerrainAtlasCache.fingerprint(layout, List.of(animated), resources, RELIEF)),
                    "changing a static texture to an animation invalidates cached relief");
            try (var materials = new TerrainMaterialAtlas(sprites, resources, 2, RELIEF)) {
                int normals = BridgeWire.NORMAL_ATLAS_ROWS;
                int rgb = 0x00ff_ffff;
                int flat = LabPbr.FLAT_NORMAL & rgb;
                int edge = 3 * 8 + 3;
                var cobbleNormals = materials.tile(normals, 0);
                int generated = cobbleNormals.sample(0, edge);
                if (RELIEF.strengthPercent() > 0) {
                    int wood = materials.tile(normals, 1).sample(0, edge);
                    int stone = materials.tile(normals, 2).sample(0, edge);
                    int cobbleX = (generated >>> 16) & 255;
                    int woodX = (wood >>> 16) & 255;
                    int stoneX = (stone >>> 16) & 255;
                    check(cobbleX < woodX && woodX < stoneX && stoneX < 128 && ((generated >>> 8) & 255) == 128,
                            "vertical seams tilt in the texture X direction with material-specific strength");
                    int mipX = (cobbleNormals.sample(1, 1 * 4 + 1) >>> 16) & 255;
                    check(cobbleX < mipX && mipX < 128, "normal mip averages relief at the seam");
                    int capped = materials.tile(normals, 6).sample(0, edge);
                    double diagonalSlope = -Math.tan(Math.toRadians(25.0)) / Math.sqrt(2.0);
                    check((capped & rgb) == (LabPbr.normal(diagonalSlope, diagonalSlope, 1.0) & rgb),
                            "strong diagonal contrast encodes the 25-degree tilt cap");
                    check((generated >>> 24) == TerrainMaterials.load().porosityForSprite(cobble.name()),
                            "generated normal preserves material porosity");
                } else {
                    check((generated & rgb) == flat, "disabled generation keeps opaque block normals flat");
                }
                check((cobbleNormals.sample(0, 3 * 8 + 7) & rgb) == flat,
                        "Sobel taps stay in the selected sprite frame");
                for (int index : new int[] {3, 4, 5, 7, 8}) {
                    check(materials.tile(normals, index).mips() == null,
                            sprites.get(index).name() + " in slot " + index + " stays flat without pixel storage");
                }
                check((materials.tile(normals, 9).sample(0, edge) & rgb) == (LabPbr.decodeNormal(authored) & rgb)
                                && (materials.tile(BridgeWire.PBR_ATLAS_ROWS, 9).sample(0, edge) >>> 24) == (authored & 255),
                        "authored normal and AO override generated relief");
            }
            testReliefControls(folder, sprites, resources, layout, authored);
        }
    }

    private static void testReliefControls(Path folder, List<TerrainAtlas.Sprite> sprites,
                                           MultiPackResourceManager resources, TerrainAtlasCache.Layout layout,
                                           int authored) throws Exception {
        var defaults = TerrainReliefSettings.DEFAULT;
        var lowRelief = new TerrainReliefSettings(50, 0, 12);
        int type = BridgeWire.NORMAL_ATLAS_ROWS;
        int rgb = 0x00ff_ffff;
        int edge = 3 * 8 + 3;
        try (var materials = new TerrainMaterialAtlas(sprites, resources, 2, lowRelief)) {
            double edgeSlope = -1.8 * 0.5 * 48.0 / 255.0;
            check((materials.tile(type, 0).sample(0, edge) & rgb) == (LabPbr.normal(edgeSlope, 0.0, 1.0) & rgb),
                    "strength slider scales the generated surface slope");
            check((materials.tile(type, 5).sample(0, edge) & rgb) != (LabPbr.FLAT_NORMAL & rgb),
                    "zero threshold keeps a color edge removed by the default threshold");
            double capped = -Math.tan(Math.toRadians(12.0)) / Math.sqrt(2.0);
            check((materials.tile(type, 6).sample(0, edge) & rgb) == (LabPbr.normal(capped, capped, 1.0) & rgb),
                    "tilt slider limits strong diagonal gradients");
            check((materials.tile(type, 9).sample(0, edge) & rgb) == (LabPbr.decodeNormal(authored) & rgb),
                    "relief sliders leave a resource pack's normal map intact");
        }

        var cobble = List.of(sprites.getFirst());
        byte[] initial = TerrainAtlasCache.fingerprint(layout, cobble, resources, defaults);
        for (var changed : List.of(defaults.withStrength(50), defaults.withThreshold(0), defaults.withMaxTilt(12))) {
            check(!Arrays.equals(initial, TerrainAtlasCache.fingerprint(layout, cobble, resources, changed)),
                    "each relief setting contributes to cache identity");
        }
        Path cache = Files.createTempDirectory(folder, "relief-cache-");
        try (var before = TerrainAtlasCache.prepare(cache, layout, cobble, resources, defaults);
             var after = TerrainAtlasCache.prepare(cache, layout, cobble, resources, lowRelief);
             var warm = TerrainAtlasCache.prepare(cache, layout, cobble, resources, lowRelief)) {
            check(!Arrays.equals(before.checksum, after.checksum) && after.path.equals(warm.path)
                            && Arrays.equals(after.checksum, warm.checksum),
                    "rebaking with a changed profile publishes new atlas pixels and reuses its own cached result");
        }
    }

    private static TerrainAtlas.Sprite reliefSprite(String texture, NativeImage[] images, int frame) {
        return new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/" + texture),
                images, 0, 0, 8, 8, 0, frame * 8, 0);
    }

    private static void testAtlasPatch() {
        var regions = List.of(new AtlasPatchStream.Region(0, 8, 6, 2, 1),
                new AtlasPatchStream.Region(1, 4, 3, 1, 1));
        int bytes = 3 * Integer.BYTES;
        var rgba = ByteBuffer.allocate(bytes).putInt(0x11223344).putInt(0x66778899).putInt(0xaabbccdd).flip();
        var frame = AtlasPatchStream.encode(67, BridgeWire.DYNAMIC_BLOCK_ATLAS, regions, bytes, rgba);
        check(frame.getInt() == frame.remaining() && Byte.toUnsignedInt(frame.get()) == 32, "atlas patch framing");
        check(frame.getInt() == 67 && frame.getInt() == 1 && frame.getShort() == 2, "atlas patch header");
        check(frame.getShort() == 0 && frame.getShort() == 8 && frame.getShort() == 6
                && frame.getShort() == 2 && frame.getShort() == 1, "atlas patch mip 0 region");
        check(frame.getInt() == 0x33221144 && frame.getInt() == 0x88776699, "atlas patch BGRA and alpha");
        check(frame.getShort() == 1 && frame.getShort() == 4 && frame.getShort() == 3
                && frame.getShort() == 1 && frame.getShort() == 1, "atlas patch mip 1 region");
        check(frame.getInt() == 0xccbbaadd && !frame.hasRemaining(), "atlas patch mip 1 pixels and size");

        List<AtlasPatchStream.Region> bands = new ArrayList<>();
        AtlasPatchStream.splitRegion(bands, 0, 246, 130, 1024, 513);
        int endRow = 130;
        for (var band : bands) {
            check(band.x() == 246 && band.y() == endRow && band.width() == 1024, "atlas patch split row layout");
            check(band.pixelBytes() + BridgeWire.ATLAS_PATCH_HEADER_BYTES + BridgeWire.ATLAS_PATCH_REGION_BYTES
                    <= BridgeWire.ATLAS_PACKET_BYTES, "atlas patch split budget");
            endRow += band.height();
        }
        check(bands.size() > 1 && endRow == 643, "atlas patch split coverage");
    }

    private static void testWaterNormals(Path folder) throws Exception {
        int sidecar = 0x0080_eeff;
        write(folder, "water_still_n", 16, sidecar, sidecar);
        write(folder, "water_flow_n", 32, sidecar, sidecar);
        try (var resources = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of(pack(folder)));
             var sixteen = new NativeImage(16, 16, false);
             var thirtyTwo = new NativeImage(32, 32, false)) {
            var vanillaSize = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/water_still"),
                    new NativeImage[] {sixteen}, 0, 0, 16, 16, 0, 0, 0);
            var lowResNormal = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/water_still"),
                    new NativeImage[] {thirtyTwo}, 0, 0, 32, 32, 0, 0, 0);
            var highResNormal = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/water_flow"),
                    new NativeImage[] {thirtyTwo}, 0, 0, 32, 32, 0, 0, 0);
            try (var materials = new TerrainMaterialAtlas(List.of(vanillaSize, lowResNormal, highResNormal), resources, 1, RELIEF)) {
                int normal = BridgeWire.NORMAL_ATLAS_ROWS;
                int flat = LabPbr.FLAT_NORMAL & 0xffffff;
                check((materials.tile(normal, 0).sample(0, 0) & 0xffffff) == flat, "vanilla-size water stays flat");
                check((materials.tile(normal, 1).sample(0, 0) & 0xffffff) == flat, "low-res sidecar stays flat");
                check((materials.tile(normal, 2).sample(0, 0) & 0xffffff) == (LabPbr.decodeNormal(sidecar) & 0xffffff),
                        "high-res LabPBR water normal is preserved");
            }
        }
    }

    private static void testResourcePack(Path working) throws Exception {
        Path low = working.resolve("lower-priority");
        Path high = working.resolve("higher-priority");
        write(low, "stone_n", 4, 0xff80_80ff, 0x0080_ee40);
        write(low, "stone_s", 4, 0xff00_0000, 0xff00_0000);
        write(high, "stone_s", 8, 0xff00_0000, 0xfec0_e600);
        try (var resources = new MultiPackResourceManager(PackType.CLIENT_RESOURCES, List.of(pack(low), pack(high)));
             var image = new NativeImage(4, 4, false);
             var mip = new NativeImage(2, 2, false)) {
            image.fillRect(0, 0, 4, 4, 0x11224444);
            image.fillRect(2, 2, 2, 2, ALBEDO);
            mip.fillRect(0, 0, 2, 2, 0x66336688);
            mip.setPixel(1, 1, ALBEDO);
            var stone = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/stone"),
                    new NativeImage[] {image, mip}, 0, 0, 2, 2, 2, 2, 2);
            var iron = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace("block/iron_block"),
                    new NativeImage[] {image, mip}, 8, 0, 2, 2, 2, 2, 2);
            var sprites = List.of(stone, iron);
            try (var materials = new TerrainMaterialAtlas(sprites, resources, 2, RELIEF)) {
                int normal = LabPbr.decodeNormal(0x0080_ee40, 0); // high-priority stone_s has B=0 -> porosity=0
                int ironNormal = LabPbr.withPorosity(LabPbr.FLAT_NORMAL, 0); // iron_block default porosity=0
                int pbr = 0x4010_ffff;
                int defaults = 0xff09_ff00;
                for (int level = 0; level < 2; level++) {
                    int width = 16 >> level;
                    int height = 8 >> level;
                    int slot = 6 >> level;
                    int ironStart = 8 >> level;
                    checkRows(BridgeWire.ATLAS_ROWS, level, width, height, sprites, materials,
                            (x, y) -> y < slot && (x < slot || x >= ironStart && x < ironStart + slot) ? ALBEDO : 0);
                    checkRows(BridgeWire.NORMAL_ATLAS_ROWS, level, width, height, sprites, materials,
                            (x, y) -> x < slot && y < slot ? normal : y < slot && x >= ironStart && x < ironStart + slot
                                    ? ironNormal : materials.fill(BridgeWire.NORMAL_ATLAS_ROWS));
                    checkRows(BridgeWire.PBR_ATLAS_ROWS, level, width, height, sprites, materials,
                            (x, y) -> x < slot && y < slot ? pbr : y < slot && x >= ironStart && x < ironStart + slot
                                    ? defaults : materials.fill(BridgeWire.PBR_ATLAS_ROWS));
                }
            }
            testDiskCache(working, high, resources, sprites);
        }
    }

    private static void testDiskCache(Path working, Path high, MultiPackResourceManager resources,
                                      List<TerrainAtlas.Sprite> sprites) throws Exception {
        var layout = new TerrainAtlasCache.Layout(16, 8, 2);
        Path cache = Files.createTempDirectory(working, "atlas-cache-");
        byte[] initialKey;
        byte[] initialChecksum;
        Path initialFile;
        try (var cold = TerrainAtlasCache.prepare(cache, layout, sprites, resources, RELIEF);
             var materials = new TerrainMaterialAtlas(sprites, resources, 2, RELIEF)) {
            check(cold.shared && Files.isRegularFile(cold.path), "cold atlas cache committed");
            initialKey = cold.key;
            initialChecksum = cold.checksum;
            initialFile = cold.path;
            long offset = TerrainAtlasCache.HEADER_BYTES;
            for (int type : TerrainAtlas.TEXTURES) {
                for (int mip = 0; mip < layout.mips(); ++mip) {
                    int width = layout.width() >> mip;
                    int height = layout.height() >> mip;
                    var expected = TerrainAtlas.encodeRows(type, mip, layout.mips(), width, height, 0, height,
                            sprites, materials).position(5 + BridgeWire.ATLAS_HEADER_BYTES);
                    var file = ByteBuffer.allocate(expected.remaining());
                    TerrainAtlasCache.read(cold.channel, file, offset);
                    offset += file.position();
                    check(file.flip().equals(expected), "disk and row atlas pixels match for " + type + " mip " + mip);
                }
            }
        }
        var oldWriteTime = Files.getLastModifiedTime(initialFile);
        try (var warm = TerrainAtlasCache.prepare(cache, layout, sprites, resources, RELIEF)) {
            check(warm.path.equals(initialFile) && Arrays.equals(warm.checksum, initialChecksum)
                    && Files.getLastModifiedTime(warm.path).equals(oldWriteTime), "warm cache reused in place");
        }
        try (var damaged = Files.newByteChannel(initialFile, StandardOpenOption.WRITE)) {
            damaged.position(TerrainAtlasCache.HEADER_BYTES);
            damaged.write(ByteBuffer.wrap(new byte[] {31}));
        }
        try (var repaired = TerrainAtlasCache.prepare(cache, layout, sprites, resources, RELIEF)) {
            check(repaired.path.equals(initialFile) && Arrays.equals(repaired.checksum, initialChecksum), "corrupt cache repaired");
        }
        Files.delete(initialFile);
        try (var recovered = TerrainAtlasCache.prepare(cache, layout, sprites, resources, RELIEF)) {
            check(recovered.path.equals(initialFile) && Arrays.equals(recovered.checksum, initialChecksum), "missing cache rebuilt");
        }
        write(high, "stone_s", 8, 0xff00_0000, 0x0c40_f100);
        try (var changed = TerrainAtlasCache.prepare(cache, layout, sprites, resources, RELIEF)) {
            check(!Arrays.equals(changed.key, initialKey) && !Arrays.equals(changed.checksum, initialChecksum),
                    "editing a sidecar in a pack with the same name changed atlas identity and pixels");
            try (var scratch = TerrainAtlasCache.prepare(null, layout, sprites, resources, RELIEF)) {
                check(!scratch.shared && Arrays.equals(scratch.key, changed.key)
                        && Arrays.equals(scratch.checksum, changed.checksum), "private fallback contains identical pixels");
                initialFile = scratch.path;
            }
            check(!Files.exists(initialFile), "private fallback cleaned up");
        }
        System.out.println("Terrain cache cold, warm, corruption, missing, same-name edit and private fallback passed");
    }

    private static PathPackResources pack(Path path) {
        String name = path.getFileName().toString();
        return new PathPackResources(new PackLocationInfo(name, Component.literal(name), PackSource.DEFAULT, Optional.empty()), path);
    }

    private static void write(Path pack, String id, int size, int fallback, int firstFrame) throws Exception {
        write(pack, "minecraft", id, size, fallback, firstFrame);
    }

    private static void write(Path pack, String namespace, String id, int size, int fallback, int firstFrame) throws Exception {
        var image = new BufferedImage(size, size, BufferedImage.TYPE_4BYTE_ABGR);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                image.setRGB(x, y, x >= size / 2 && y >= size / 2 ? firstFrame : fallback);
            }
        }
        Path path = pack.resolve("assets/" + namespace + "/textures/block/" + id + ".png");
        Files.createDirectories(path.getParent());
        check(ImageIO.write(image, "PNG", path.toFile()), "PNG writer available");
    }

    private static void checkRows(int type, int mip, int width, int height, List<TerrainAtlas.Sprite> sprites,
                                  TerrainMaterialAtlas materials, Pixel expected) {
        int firstRow = 1;
        int rows = height - firstRow;
        var frame = TerrainAtlas.encodeRows(type, mip, 2, width, height, firstRow, rows, sprites, materials);
        check(frame.getInt() == 1 + 12 + width * rows * 4 && Byte.toUnsignedInt(frame.get()) == type, "frame header");
        check(frame.getShort() == mip && frame.getShort() == 2 && frame.getShort() == width && frame.getShort() == height
                && frame.getShort() == firstRow && frame.getShort() == rows, "shared atlas layout");
        frame.order(ByteOrder.LITTLE_ENDIAN);
        for (int y = firstRow; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int pixel = frame.getInt();
                check(pixel == expected.get(x, y), "BGRA packet " + type + " mip " + mip + " pixel " + x + "," + y
                        + " was " + Integer.toHexString(pixel) + " expected " + Integer.toHexString(expected.get(x, y)));
            }
        }
        check(!frame.hasRemaining(), "frame ended at advertised size");
    }

    private static void check(boolean condition, String detail) {
        if (!condition) {
            throw new AssertionError(detail);
        }
    }

    @FunctionalInterface
    private interface Pixel {
        int get(int x, int y);
    }
}
