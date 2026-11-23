package dev.minecraftue.bridge;

import net.minecraft.resources.Identifier;

/** Emitting parts of vanilla terrain materials, when a pack has not supplied LabPBR emission. */
final class TextureEmission {
    // These modes are shared with the terrain material and the direct-light sampler in UE.
    static final int PACKED = 0, DARK = 1, LIVE = 2, AUTOMATIC = 3, SEA = 4, ORE_DIM = 5;

    enum Rule { LUMINANCE, LUMINOUS_BODY, SEA_LANTERN, ORE, HOT, COOL, PURPLE, CANDLE, PICKLE, PUMPKIN, BULB, BERRIES, ANCHOR, SPAWNER }

    private TextureEmission() {}

    static Rule rule(Identifier id) {
        if (OreEmission.applies(id)) return Rule.ORE;
        if (!id.getNamespace().equals("minecraft") || !id.getPath().startsWith("block/")) return null;
        String n = id.getPath().substring(6);
        if (n.equals("candle_lit") || n.endsWith("_candle_lit")) return Rule.CANDLE;
        if (n.contains("copper_bulb_lit")) return Rule.BULB;
        if (n.contains("froglight_") || n.endsWith("_amethyst_bud")) return Rule.LUMINOUS_BODY;
        if (n.startsWith("respawn_anchor_side") && !n.endsWith("0")) return Rule.ANCHOR;
        if (n.startsWith("trial_spawner_") && !n.contains("inactive") && !n.contains("bottom")) return Rule.SPAWNER;
        if (n.startsWith("vault_front") || n.startsWith("vault_side") || n.startsWith("vault_top_ejecting")) return Rule.SPAWNER;
        if (n.startsWith("sculk_sensor_tendril") || n.startsWith("sculk_catalyst_top") || n.startsWith("sculk_catalyst_side")) return Rule.COOL;
        return switch (n) {
            case "glowstone", "shroomlight", "amethyst_cluster" -> Rule.LUMINOUS_BODY;
            case "brown_mushroom", "beacon", "glow_lichen",
                    "redstone_lamp_on", "firefly_bush_emissive", "end_portal_frame_eye", "end_portal_frame_top",
                    "calibrated_sculk_sensor_amethyst", "end_rod" -> Rule.LUMINANCE;
            case "sea_lantern" -> Rule.SEA_LANTERN;
            case "jack_o_lantern" -> Rule.PUMPKIN;
            case "cave_vines_lit", "cave_vines_plant_lit" -> Rule.BERRIES;
            case "lava_flow", "lava_still", "fire_0", "fire_1", "soul_fire_0", "soul_fire_1",
                    "campfire_fire", "soul_campfire_fire" -> Rule.LUMINANCE;
            case "campfire_log_lit", "furnace_front_on", "blast_furnace_front_on", "smoker_front_on", "brewing_stand" -> Rule.HOT;
            case "soul_campfire_log_lit", "enchanting_table_top", "enchanting_table_side" -> Rule.COOL;
            case "dragon_egg", "respawn_anchor_top", "nether_portal" -> Rule.PURPLE;
            case "sea_pickle" -> Rule.PICKLE;
            default -> null;
        };
    }

    static String companion(String path, Rule rule) {
        return switch (rule) {
            case PUMPKIN -> "block/carved_pumpkin";
            case BULB, BERRIES -> path.replace("_lit", "");
            case ANCHOR -> "block/respawn_anchor_side0";
            case ORE -> OreEmission.hostName(path);
            default -> null;
        };
    }

    static int mode(TerrainAtlas.Sprite sprite, Rule rule) {
        if (rule == Rule.SEA_LANTERN) return SEA;
        return sprite.frames().length > 1 ? LIVE : PACKED;
    }

    static double weight(int mode, double packed, double r, double g, double b) {
        if (mode == DARK) return 0;
        if (mode == PACKED) return packed;
        double y = .2126 * r + .7152 * g + .0722 * b;
        if (mode == SEA) {
            double reference = Math.max(1.0 / 255, (1 - packed) / .75);
            double ring = smooth(.89, 1.005, y / reference);
            return .018 + Math.min(1, 4 * packed) * .30 * ring * ring;
        }
        if (mode == ORE_DIM) return .006 * packed / (.012 + .988 * Math.max(r,Math.max(g,b)));
        double value = .5 * Math.max(r, Math.max(g, b)) + .5 * y;
        if (mode == AUTOMATIC) return smooth(.032, .74, value);
        return packed * (.035 + .965 * smooth(.015, .80, value));
    }

    static double smooth(double low, double high, double value) {
        double t = Math.clamp((value - low) / (high - low), 0, 1);
        return t * t * (3 - 2 * t);
    }

    /** Static pigment selection is shared across animation frames. The current texel supplies the live strength. */
    static byte[] gate(TerrainAtlas.Sprite s, Rule rule, TerrainAtlas.Sprite companion) {
        if (rule == Rule.ORE) return OreEmission.mask(s,companion);
        int w = s.width(), h = s.height(), count = Math.multiplyExact(w, h);
        var weights = new byte[count];
        var scores = new float[count];
        double high = 0, low = Double.POSITIVE_INFINITY, seaPeak = 0;
        int mode = mode(s, rule);
        var image = s.images()[0];
        for (int frame : s.frames()) {
            int left = s.frameX(frame), top = s.frameY(frame);
            for (int y = 0, i = 0; y < h; y++) for (int x = 0; x < w; x++, i++) {
                int pixel = image.getPixel(left + x, top + y);
                if ((pixel >>> 24) == 0) continue;
                int u = (int) ((x + .5) * 16 / w), v = (int) ((y + .5) * 16 / h);
                double r = TerrainEmission.linear(pixel >> 16 & 255), g = TerrainEmission.linear(pixel >> 8 & 255);
                double b = TerrainEmission.linear(pixel & 255);
                double yLinear = .2126 * r + .7152 * g + .0722 * b;
                double score = switch (rule) {
                    case SEA_LANTERN -> u > 1 && u < 14 && v > 1 && v < 14 ? 1 : 0;
                    case CANDLE -> u == 0 && v == 5 ? 1 : u < 2 && v >= 6 && v <= 8 ? (9 - v) * .17 : 0;
                    case PICKLE -> v < 5 && (u < 3 || u >= 13) ? yLinear : 0;
                    case HOT -> Math.max(0, r - 1.25 * b) * smooth(.025, .19, r - b);
                    case COOL -> Math.max(0, Math.max(g, b) - .96 * r);
                    case PURPLE -> Math.max(0, Math.min(r, b) - .95 * g);
                    case SPAWNER -> Math.max(Math.max(0, r - 1.2 * b), Math.max(0, b - 1.4 * r)) * smooth(.025, .40, Math.max(r, b));
                    case PUMPKIN, BULB, BERRIES, ANCHOR -> companion == null ? yLinear : difference(r, g, b, companion, x, y, w, h);
                    default -> yLinear;
                };
                if (rule == Rule.SEA_LANTERN && score != 0) seaPeak = Math.max(seaPeak, yLinear);
                high = Math.max(high, score);
                low = Math.min(low, score);
                scores[i] = Math.max(scores[i], (float) score);
            }
        }
        if (high <= 0) return weights;
        // A uniformly coloured replacement can still describe a fully luminous surface.
        boolean uniform = high - low < 1.e-5;
        for (int i = 0; i < count; i++) {
            double v = scores[i];
            if (v <= 0) continue;
            double weight;
            // An inverse encoding makes interpolation towards the dark casing reduce emission monotonically.
            if (rule == Rule.SEA_LANTERN) weight = 1 - .75 * seaPeak;
            else if (rule == Rule.CANDLE) weight = v;
            else if (mode == LIVE && (rule == Rule.LUMINANCE || rule == Rule.LUMINOUS_BODY)) weight = 1;
            else if (uniform) weight = 1;
            else {
                boolean graded = rule == Rule.LUMINANCE || rule == Rule.LUMINOUS_BODY;
                double floor = graded ? low + .055 * (high - low) : .035 * high;
                weight = smooth(floor, high, v);
                if (rule == Rule.LUMINOUS_BODY) weight = .10 + .90 * weight;
            }
            weights[i] = (byte) Math.round(255 * weight);
        }
        return weights;
    }

    private static double difference(double r, double g, double b, TerrainAtlas.Sprite off, int x, int y, int w, int h) {
        int old = off.images()[0].getPixel(off.frameX() + x * off.width() / w, off.frameY() + y * off.height() / h);
        if ((old >>> 24) == 0) return Math.max(r, Math.max(g, b));
        return Math.max(0, Math.max(r - TerrainEmission.linear(old >> 16 & 255),
                Math.max(g - TerrainEmission.linear(old >> 8 & 255), b - TerrainEmission.linear(old & 255))));
    }
}
