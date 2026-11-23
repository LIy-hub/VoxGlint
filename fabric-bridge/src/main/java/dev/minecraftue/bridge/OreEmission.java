package dev.minecraftue.bridge;

import net.minecraft.resources.Identifier;

import java.util.BitSet;
import java.util.Map;

/** Isolates mineral inclusions from their host rock in the active resource pack. */
final class OreEmission {
    private OreEmission() {}

    static boolean applies(Identifier id) {
        String path = id.getPath();
        if (!path.startsWith("block/")) return false;
        String leaf = path.substring(path.lastIndexOf('/') + 1);
        return leaf.endsWith("_ore") || path.contains("/ores/") ||
                id.getNamespace().equals("minecraft") && (leaf.equals("gilded_blackstone") || leaf.startsWith("ancient_debris_"));
    }

    static String hostName(String path) {
        if (path.contains("ancient_debris")) return null;
        if (path.contains("deepslate")) return "block/deepslate";
        if (path.contains("nether")) return "block/netherrack";
        if (path.contains("blackstone")) return "block/blackstone";
        if (path.contains("end_stone")) return "block/end_stone";
        return "block/stone";
    }

    static TerrainAtlas.Sprite host(TerrainAtlas.Sprite ore, Map<Identifier,TerrainAtlas.Sprite> atlas) {
        String path = hostName(ore.name().getPath());
        if (path == null) return null;
        var own = atlas.get(ore.name().withPath(path));
        return own != null ? own : atlas.get(Identifier.withDefaultNamespace(path));
    }

    static byte[] mask(TerrainAtlas.Sprite ore, TerrainAtlas.Sprite host) {
        int w = ore.width(), h = ore.height();
        var result = new byte[Math.multiplyExact(w,h)];
        var rockPalette = new BitSet(32768);
        int[] rockBrightness = new int[256];
        int groundCount = 0, coloredGround = 0;
        if (host != null) {
            for (int y=0; y<host.height(); y++) for (int x=0; x<host.width(); x++) {
                int sample = pixel(host,host.frameX()+x,host.frameY()+y);
                if ((sample >>> 24) == 0) continue;
                rockPalette.set(bin(sample));
                int peak = peak(sample);
                rockBrightness[peak]++;
                groundCount++;
                if (chroma(sample) >= 22) coloredGround++;
            }
        }
        boolean debris = ore.name().getPath().contains("ancient_debris");
        boolean coloredMineral = groundCount > 0 && coloredGround < .12*groundCount && hasColoredInclusions(ore,rockPalette);
        boolean coal = ore.name().getPath().endsWith("coal_ore");
        int darkGround = 0, samples = 0;
        while (darkGround < 255 && (samples += rockBrightness[darkGround]) < Math.max(1,groundCount/8)) darkGround++;
        for (int frame : ore.frames()) {
            int left=ore.frameX(frame), top=ore.frameY(frame);
            for (int y=0,i=0; y<h; y++) for (int x=0; x<w; x++,i++) {
                int p=pixel(ore,left+x,top+y);
                if ((p >>> 24) == 0 || rockPalette.get(bin(p))) continue;
                int r=p>>16&255, g=p>>8&255, b=p&255;
                double weight;
                if (host != null) {
                    int ground=pixel(host,host.frameX()+x*host.width()/w,host.frameY()+y*host.height()/h);
                    int delta=Math.max(Math.abs(r-(ground>>16&255)),Math.max(Math.abs(g-(ground>>8&255)),Math.abs(b-(ground&255))));
                    weight=TextureEmission.smooth(5,78,delta);
                    // Some stone variants shift the grey shading around an inclusion. Those grey accents belong to the rock.
                    if (coloredMineral) weight *= TextureEmission.smooth(6,20,chroma(p));
                    else if (coal) weight *= TextureEmission.smooth(0,24,darkGround-peak(p));
                } else if (debris) {
                    // Debris has a banded metallic texture and no separate host-rock asset.
                    weight=TextureEmission.smooth(67,145,.2126*r+.7152*g+.0722*b);
                } else {
                    int peak=Math.max(r,Math.max(g,b)), min=Math.min(r,Math.min(g,b));
                    weight=TextureEmission.smooth(8,60,peak-min);
                }
                result[i]=(byte)Math.max(Byte.toUnsignedInt(result[i]),Math.round(weight*255));
            }
        }
        return result;
    }

    private static boolean hasColoredInclusions(TerrainAtlas.Sprite sprite, BitSet rock) {
        int colored = 0, novel = 0;
        int stepX = Math.max(1,sprite.width()/64), stepY = Math.max(1,sprite.height()/64);
        for (int y=0; y<sprite.height(); y+=stepY) for (int x=0; x<sprite.width(); x+=stepX) {
            int p=pixel(sprite,sprite.frameX()+x,sprite.frameY()+y);
            if ((p>>>24)==0 || rock.get(bin(p))) continue;
            novel++;
            if (chroma(p)>=22) colored++;
        }
        return colored>=3 && colored>.18*novel;
    }

    private static int peak(int p) { return Math.max(p>>16&255,Math.max(p>>8&255,p&255)); }
    private static int chroma(int p) { return peak(p)-Math.min(p>>16&255,Math.min(p>>8&255,p&255)); }
    private static int pixel(TerrainAtlas.Sprite s,int x,int y) { return s.images()[0].getPixel(x,y); }
    private static int bin(int argb) {
        return ((argb >> 19 & 31) << 10) | ((argb >> 11 & 31) << 5) | (argb >> 3 & 31);
    }
}
