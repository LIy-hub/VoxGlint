package dev.minecraftue.bridge;

import com.mojang.blaze3d.platform.NativeImage;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.texture.SpriteContents;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import net.minecraft.client.resources.metadata.animation.FrameSize;
import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.PackType;
import net.minecraft.server.packs.resources.MultiPackResourceManager;
import net.minecraft.world.level.BlockGetter;

import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;

final class OreEmissionContract {
    private static final int EDGE=16, WIDTH=512;

    static void verify(Path dir, BlockGetter world) throws Exception {
        var names = new ArrayList<String>();
        for (String mineral : List.of("diamond","emerald","lapis","iron","copper","gold","coal","redstone")) {
            names.add(mineral+"_ore");
            names.add("deepslate_"+mineral+"_ore");
        }
        names.addAll(List.of("nether_gold_ore","nether_quartz_ore","gilded_blackstone","ancient_debris_side","ancient_debris_top","cobalt_ore"));
        int oreCount=names.size();
        names.addAll(List.of("stone","deepslate","netherrack","blackstone"));
        var sprites=new ArrayList<TerrainAtlas.Sprite>();
        var models=new ArrayList<TextureAtlasSprite>();
        var lines=new ArrayList<String>();
        lines.add("texture,block,atlas_slot,emitting_texels,nonemitting_texels,mode");
        try (var packs=new MultiPackResourceManager(PackType.CLIENT_RESOURCES,List.of())) {
            for (String name : names) {
                boolean mod=name.equals("cobalt_ore");
                String file=mod ? "diamond_ore" : name;
                var id=Identifier.fromNamespaceAndPath(mod ? "fixture" : "minecraft","block/"+name);
                try (var in=OreEmissionContract.class.getResourceAsStream("/assets/minecraft/textures/block/"+file+".png")) {
                    var image=NativeImage.read(Objects.requireNonNull(in));
                    int x=sprites.size()*EDGE;
                    sprites.add(new TerrainAtlas.Sprite(id,new NativeImage[]{image},x,0,EDGE,EDGE,0,0,0,new int[]{0}));
                    var contents=new SpriteContents(id,new FrameSize(EDGE,EDGE),image);
                    models.add(new TextureAtlasSprite(TextureAtlas.LOCATION_BLOCKS,contents,WIDTH,EDGE,x,0,0){});
                }
            }
            try (var atlas=TerrainAtlasCache.prepare(null,new TerrainAtlasCache.Layout(WIDTH,EDGE,1),sprites,packs,TerrainReliefSettings.DISABLED)) {
                byte[] mask=null;
                for (int i=0; i<TerrainAtlas.TEXTURES.length; i++) {
                    var buffer=ByteBuffer.allocate(WIDTH*EDGE*4);
                    TerrainAtlasCache.read(atlas.channel,buffer,TerrainAtlasCache.HEADER_BYTES+(long)i*buffer.capacity());
                    Files.write(dir.resolve("ore-atlas-"+TerrainAtlas.TEXTURES[i]+".bgra"),buffer.array());
                    if (i==2) mask=buffer.array();
                }
                var capture=SectionCapture.begin(atlas.emissionColors,false,true);
                try {
                    for (int slot=0; slot<oreCount; slot++) {
                        String name=names.get(slot);
                        String block=name.startsWith("ancient_debris") ? "ancient_debris" : name.equals("cobalt_ore") ? "stone" : name;
                        var state=BuiltInRegistries.BLOCK.getValue(Identifier.withDefaultNamespace(block)).defaultBlockState();
                        if (state.getLightEmission()!=0) throw new AssertionError("expected MC0 ore "+block);
                        var p=new BlockPos(slot % 12,1,slot / 12);
                        TextureEmissionContract.captureFace(capture,world,p,state,models.get(slot),4*slot);
                        int active=0, sameRock=0;
                        String host=OreEmission.hostName("block/"+name);
                        var rock=host==null ? null : sprites.get(names.indexOf(host.substring(6)));
                        var sprite=sprites.get(slot);
                        for (int y=0; y<EDGE; y++) for (int x=0; x<EDGE; x++) {
                            int w=Byte.toUnsignedInt(mask[4*(y*WIDTH+slot*EDGE+x)]);
                            if (w!=0) active++;
                            int pixel=sprite.images()[0].getPixel(x,y);
                            if (w>0 && (name.contains("diamond") || name.contains("lapis") || name.contains("cobalt"))) {
                                int red=pixel>>16&255, green=pixel>>8&255, blue=pixel&255;
                                if (Math.max(red,Math.max(green,blue))-Math.min(red,Math.min(green,blue))<=6)
                                    throw new AssertionError("grey rock within coloured "+name+" is emissive");
                            }
                            if (rock!=null && sprite.images()[0].getPixel(x,y)==rock.images()[0].getPixel(x,y)) {
                                sameRock++;
                                if (w!=0) throw new AssertionError(name+" makes unchanged host-rock texels emissive");
                            }
                        }
                        if (active<4 || active>236 || rock!=null && sameRock<16)
                            throw new AssertionError(name+" active="+active+" host="+sameRock);
                        lines.add(sprite.name()+","+block+","+slot+","+active+","+(256-active)+","+TextureEmission.ORE_DIM);
                    }
                    byte[] intrinsic=new byte[oreCount];
                    byte[] modes=capture.bucketsFor(ChunkSectionLayer.SOLID,4*oreCount,new int[8],intrinsic);
                    for (int i=0;i<oreCount;i++) if (intrinsic[i]!=0 || ((modes[i]&0x38)>>3)!=TextureEmission.ORE_DIM)
                        throw new AssertionError("unexpected ore surface mode for "+names.get(i));
                    if (capture.encodeLights(1,SectionPos.of(0,0,0),true).getShort(21)!=0)
                        throw new AssertionError("decorative ore emission allocated a gameplay-source proxy");
                } finally { SectionCapture.end(); }
                var older=SectionCapture.begin(atlas.emissionColors,false,true,true,false);
                try {
                    var state=BuiltInRegistries.BLOCK.getValue(Identifier.withDefaultNamespace("diamond_ore")).defaultBlockState();
                    TextureEmissionContract.captureFace(older,world,BlockPos.ZERO,state,models.getFirst(),0);
                    byte mode=older.bucketsFor(ChunkSectionLayer.SOLID,4,new int[8])[0];
                    if (((mode&0x38)>>3)!=TextureEmission.DARK) throw new AssertionError("ore extension must be negotiated");
                } finally { SectionCapture.end(); }
            }
        } finally { models.forEach(TextureAtlasSprite::close); }
        Files.write(dir.resolve("ore-mask-coverage.csv"),lines);
    }
}
