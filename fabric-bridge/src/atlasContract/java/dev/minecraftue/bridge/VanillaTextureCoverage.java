package dev.minecraftue.bridge;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import com.mojang.blaze3d.platform.NativeImage;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;

import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.BitSet;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.stream.IntStream;

/** Resolves only visible faces in the stock models used by luminous states. Shares production pigment rules. */
final class VanillaTextureCoverage {
    private record Model(Map<String,String> textures, JsonArray elements) {}
    private final Map<String,Model> models = new HashMap<>();
    private final Map<String,TerrainAtlas.Sprite> sprites = new HashMap<>();
    private final Map<String,BitSet> emitting = new HashMap<>();

    void write(Path dir) throws IOException {
        try {
            measure(dir);
        } finally {
            sprites.values().forEach(s -> s.images()[0].close());
        }
    }

    private void measure(Path dir) throws IOException {
        var rows = new ArrayList<String>();
        var totals = new TreeMap<String,Integer>();
        var missingMasks = new ArrayList<String>();
        int luminousStates = 0;
        for (var block : BuiltInRegistries.BLOCK) {
            String id = BuiltInRegistries.BLOCK.getKey(block).getPath();
            var states = block.getStateDefinition().getPossibleStates().stream().filter(s -> s.getLightEmission() > 0).toList();
            if (states.isEmpty()) continue;
            luminousStates += states.size();
            var refs = new TreeSet<String>();
            var stateModels = new HashMap<Set<String>,Integer>();
            var variants = asset("blockstates/" + id);
            for (var state : states) {
                var stateRefs = new HashSet<String>();
                if (variants.has("variants")) {
                    for (var entry : variants.getAsJsonObject("variants").entrySet()) {
                        var terms = entry.getKey();
                        if (terms.isEmpty() || Arrays.stream(terms.split(",")).allMatch(term -> {
                            var eq = term.indexOf('=');
                            return eq > 0 && matches(state, term.substring(0,eq), term.substring(eq+1));
                        })) collectModels(entry.getValue(), stateRefs);
                    }
                }
                if (variants.has("multipart")) for (var part : variants.getAsJsonArray("multipart")) {
                    var object = part.getAsJsonObject();
                    if (!object.has("when") || condition(object.getAsJsonObject("when"),state)) collectModels(object.get("apply"),stateRefs);
                }
                refs.addAll(stateRefs);
                stateModels.merge(Set.copyOf(stateRefs), 1, Integer::sum);
            }
            var surfaces = new TreeSet<String>();
            var seenPixels = new HashMap<String,BitSet>();
            var visibleModels = new HashSet<String>();
            var luminousModels = new HashSet<String>();
            for (String ref : refs) {
                var model = model(ref);
                var modelPixels = new HashMap<String,BitSet>();
                for (var part : model.elements()) {
                    var faces = part.getAsJsonObject().getAsJsonObject("faces");
                    if (faces == null) continue;
                    for (var face : faces.entrySet()) {
                        String tex = face.getValue().getAsJsonObject().get("texture").getAsString();
                        var seen = new HashSet<String>();
                        while (tex.startsWith("#") && seen.add(tex)) tex = model.textures.getOrDefault(tex.substring(1),"");
                        if (!tex.isEmpty() && !tex.startsWith("#")) {
                            String name = tex.replace("minecraft:","");
                            surfaces.add(name);
                            visibleModels.add(ref);
                            if (hasRule(name)) {
                                var area = modelPixels.computeIfAbsent(name,unused -> new BitSet());
                                var element = part.getAsJsonObject();
                                var uv = face.getValue().getAsJsonObject().getAsJsonArray("uv");
                                includeUv(area,sprite(name),uv == null ? defaultUv(element,face.getKey()) : uv.asList().stream()
                                        .mapToDouble(JsonElement::getAsDouble).toArray());
                            }
                        }
                    }
                }
                for (var entry : modelPixels.entrySet()) {
                    entry.getValue().and(litPixels(entry.getKey()));
                    if (!entry.getValue().isEmpty()) luminousModels.add(ref);
                    seenPixels.computeIfAbsent(entry.getKey(), unused -> new BitSet()).or(entry.getValue());
                }
            }
            var selected = new TreeSet<String>();
            int luminousTexels = 0;
            for (var entry : seenPixels.entrySet()) {
                if (!entry.getValue().isEmpty()) selected.add(entry.getKey());
                luminousTexels += entry.getValue().cardinality();
            }
            String kind = switch (id) {
                case "lava" -> "fluid";
                case "ender_chest", "conduit" -> "block_entity";
                case "light" -> "invisible";
                case "end_portal", "end_gateway" -> "procedural";
                default -> !selected.isEmpty() ? "baked_rule" : surfaces.isEmpty() ? "no_uv" : "baked_auto";
            };
            int maskedStates = 0, noFaceStates = 0;
            if (kind.startsWith("baked_") || kind.equals("no_uv")) {
                for (var group : stateModels.entrySet()) {
                    if (group.getKey().stream().anyMatch(luminousModels::contains)) maskedStates += group.getValue();
                    else if (group.getKey().stream().noneMatch(visibleModels::contains)) noFaceStates += group.getValue();
                    else missingMasks.add(id + " (" + group.getValue() + " states): " + group.getKey());
                }
            }
            totals.merge(kind,1,Integer::sum);
            rows.add("minecraft:" + id + "," + states.size() + "," + kind + "," + luminousTexels + "," + String.join(" ",selected)
                    + "," + String.join(" ",surfaces) + "," + maskedStates + "," + noFaceStates);
        }
        rows.sort(String::compareTo);
        rows.addFirst("block,luminous_states,path,emitting_source_texels_in_visible_uvs,matched_visible_textures,all_visible_textures,states_with_visible_mask,states_without_model_faces");
        Files.write(dir.resolve("vanilla-texture-coverage.csv"),rows);
        System.out.println("Vanilla luminous block types=" + (rows.size()-1) + ", states=" + luminousStates + ", paths=" + totals);
        if (!missingMasks.isEmpty()) throw new AssertionError("Luminous vanilla states with visible models missing a mask: " + missingMasks);
    }

    private static boolean hasRule(String name) {
        var id = Identifier.withDefaultNamespace(name);
        return TextureEmission.rule(id) != null || TerrainEmission.builtinPattern(id) != null;
    }

    private TerrainAtlas.Sprite sprite(String name) throws IOException {
        if (sprites.containsKey(name)) return sprites.get(name);
        try (var in = getClass().getResourceAsStream("/assets/minecraft/textures/" + name + ".png")) {
            if (in == null) throw new IOException("Stock texture missing: " + name);
            var image = NativeImage.read(in);
            int edge = Math.min(image.getWidth(),image.getHeight());
            var frames = IntStream.range(0,image.getWidth()*image.getHeight()/(edge*edge)).toArray();
            var sprite = new TerrainAtlas.Sprite(Identifier.withDefaultNamespace(name),new NativeImage[]{image},
                    0,0,edge,edge,0,0,0,frames);
            sprites.put(name,sprite);
            return sprite;
        }
    }

    private BitSet litPixels(String name) throws IOException {
        var cached = emitting.get(name);
        if (cached != null) return cached;
        var s = sprite(name);
        var rule = TextureEmission.rule(s.name());
        var weights = new byte[s.width()*s.height()];
        int mode = TextureEmission.PACKED;
        if (rule != null) {
            String other = TextureEmission.companion(name,rule);
            weights = TextureEmission.gate(s,rule,other == null ? null : sprite(other));
            mode = TextureEmission.mode(s,rule);
        } else {
            var mask = TerrainEmission.builtinPattern(s.name()).stableMask(s);
            for (int i=0; i<weights.length; i++) if (mask == null || mask.get(i)) weights[i]=(byte)255;
        }
        var lit = new BitSet(weights.length);
        for (int i=0; i<weights.length; i++) {
            if (weights[i] == 0 && mode != TextureEmission.SEA) continue;
            for (int frame : s.frames()) {
                int p = s.images()[0].getPixel(s.frameX(frame)+i%s.width(),s.frameY(frame)+i/s.width());
                if ((p >>> 24) == 0 || (p & 0xffffff) == 0) continue;
                double strength = TextureEmission.weight(mode,Byte.toUnsignedInt(weights[i])/255.0,
                        TerrainEmission.linear(p>>16&255),TerrainEmission.linear(p>>8&255),TerrainEmission.linear(p&255));
                if (strength > 0) { lit.set(i); break; }
            }
        }
        emitting.put(name,lit);
        return lit;
    }

    private static void includeUv(BitSet area, TerrainAtlas.Sprite s, double[] uv) {
        int l = Math.max(0,(int)Math.floor(Math.min(uv[0],uv[2])*s.width()/16));
        int r = Math.min(s.width(),(int)Math.ceil(Math.max(uv[0],uv[2])*s.width()/16));
        int t = Math.max(0,(int)Math.floor(Math.min(uv[1],uv[3])*s.height()/16));
        int b = Math.min(s.height(),(int)Math.ceil(Math.max(uv[1],uv[3])*s.height()/16));
        for (int y=t; y<b && r>l; y++) area.set(y*s.width()+l,y*s.width()+r);
    }

    private static double[] defaultUv(JsonObject e, String side) {
        var a=e.getAsJsonArray("from"); var b=e.getAsJsonArray("to");
        double x=a.get(0).getAsDouble(), y=a.get(1).getAsDouble(), z=a.get(2).getAsDouble();
        double X=b.get(0).getAsDouble(), Y=b.get(1).getAsDouble(), Z=b.get(2).getAsDouble();
        return switch(side) {
            case "up" -> new double[]{x,z,X,Z};
            case "down" -> new double[]{x,16-Z,X,16-z};
            case "north" -> new double[]{16-X,16-Y,16-x,16-y};
            case "south" -> new double[]{x,16-Y,X,16-y};
            case "west" -> new double[]{z,16-Y,Z,16-y};
            case "east" -> new double[]{16-Z,16-Y,16-z,16-y};
            default -> throw new IllegalArgumentException(side);
        };
    }

    private static boolean condition(JsonObject when, BlockState state) {
        for (var term : when.entrySet()) {
            if (term.getValue().isJsonArray()) {
                var checks = term.getValue().getAsJsonArray().asList().stream().map(c -> condition(c.getAsJsonObject(), state));
                boolean value = term.getKey().equals("OR") ? checks.anyMatch(Boolean::booleanValue) : checks.allMatch(Boolean::booleanValue);
                if (!value) return false;
            } else if (!matches(state,term.getKey(),term.getValue().getAsString())) return false;
        }
        return true;
    }

    private static boolean matches(BlockState state, String key, String value) {
        var property = state.getBlock().getStateDefinition().getProperty(key);
        if (property == null) return false;
        boolean negated = value.startsWith("!");
        String actual = valueName(state,property);
        return negated != Arrays.asList((negated ? value.substring(1) : value).split("\\|")).contains(actual);
    }

    private static <T extends Comparable<T>> String valueName(BlockState s, Property<T> p) {
        return p.getName(s.getValue(p));
    }

    private static void collectModels(JsonElement element, Set<String> result) {
        if (element.isJsonArray()) element.getAsJsonArray().forEach(child -> collectModels(child,result));
        else if (element.isJsonObject() && element.getAsJsonObject().has("model"))
            result.add(element.getAsJsonObject().get("model").getAsString().replace("minecraft:",""));
    }

    private Model model(String path) throws IOException {
        var cached = models.get(path);
        if (cached != null) return cached;
        var json = asset("models/" + path);
        var parent = json.has("parent") ? model(json.get("parent").getAsString().replace("minecraft:",""))
                : new Model(Map.of(),new JsonArray());
        var textures = new HashMap<>(parent.textures);
        if (json.has("textures")) json.getAsJsonObject("textures").entrySet().forEach(e -> textures.put(e.getKey(),e.getValue().getAsString()));
        var value = new Model(textures, json.has("elements") ? json.getAsJsonArray("elements") : parent.elements);
        models.put(path,value);
        return value;
    }

    private static JsonObject asset(String file) throws IOException {
        try (var in = VanillaTextureCoverage.class.getResourceAsStream("/assets/minecraft/" + file + ".json")) {
            if (in == null) return new JsonObject();
            return JsonParser.parseReader(new InputStreamReader(in, StandardCharsets.UTF_8)).getAsJsonObject();
        }
    }
}
