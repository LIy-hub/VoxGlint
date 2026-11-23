package dev.minecraftue.bridge;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import net.minecraft.resources.Identifier;

import java.io.IOException;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

final class TerrainMaterials {
    static final String DEFAULTS = "/assets/minecraft_ue_bridge/terrain_materials.json";
    private final Entry fallback;
    private final List<Rule> rules;

    private TerrainMaterials(Entry fallback, List<Rule> rules) {
        this.fallback = fallback;
        this.rules = rules;
    }

    static TerrainMaterials load() {
        var input = TerrainMaterials.class.getResourceAsStream(DEFAULTS);
        if (input == null) {
            throw new IllegalStateException("Missing terrain material table " + DEFAULTS);
        }
        try (var reader = new InputStreamReader(input, StandardCharsets.UTF_8)) {
            var data = JsonParser.parseReader(reader).getAsJsonObject();
            List<Rule> rules = new ArrayList<>();
            for (var item : data.getAsJsonArray("materials")) {
                var material = item.getAsJsonObject();
                Entry entry = parseEntry(material);
                for (var texture : material.getAsJsonArray("textures")) {
                    var glob = new StringBuilder();
                    for (String part : texture.getAsString().split("\\*", -1)) {
                        if (!glob.isEmpty()) {
                            glob.append(".*");
                        }
                        glob.append(Pattern.quote(part));
                    }
                    rules.add(new Rule(Pattern.compile(glob.toString()), entry));
                }
            }
            return new TerrainMaterials(parseEntry(data.getAsJsonObject("fallback")), List.copyOf(rules));
        } catch (IOException failure) {
            throw new IllegalStateException("Could not read terrain material table " + DEFAULTS, failure);
        }
    }

    int fallback() {
        return fallback.pbr();
    }

    int fallbackNormal() {
        return LabPbr.withPorosity(LabPbr.FLAT_NORMAL, fallback.porosity());
    }

    int forSprite(Identifier name) {
        return entryFor(name).pbr();
    }

    int porosityForSprite(Identifier name) {
        return entryFor(name).porosity();
    }

    private Entry entryFor(Identifier name) {
        String id = name.toString();
        for (Rule rule : rules) {
            if (rule.pattern().matcher(id).matches()) {
                return rule.entry();
            }
        }
        return fallback;
    }

    private static Entry parseEntry(JsonObject material) {
        int pbr = LabPbr.packed(channel(material, "roughness", 0.9), channel(material, "metallic", 0),
                channel(material, "emissive", 0), channel(material, "ao", 1));
        int porosity = channel(material, "porosity", 0.65);
        return new Entry(pbr, porosity);
    }

    private static int channel(JsonObject material, String name, double fallback) {
        double value = material.has(name) ? material.get(name).getAsDouble() : fallback;
        if (!(value >= 0.0 && value <= 1.0)) {
            throw new IllegalArgumentException("Terrain material " + name + " must be in [0, 1]");
        }
        return (int) Math.round(value * 255.0);
    }

    private record Entry(int pbr, int porosity) {}
    private record Rule(Pattern pattern, Entry entry) {}
}
