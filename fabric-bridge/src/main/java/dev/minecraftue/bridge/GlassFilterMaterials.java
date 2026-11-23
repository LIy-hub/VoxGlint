package dev.minecraftue.bridge;

import com.google.gson.JsonElement;
import com.google.gson.JsonParseException;
import com.google.gson.JsonParser;
import com.mojang.logging.LogUtils;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.server.packs.resources.ResourceManager;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import org.slf4j.Logger;

import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

import static java.util.Map.entry;

final class GlassFilterMaterials {
    static final String RESOURCE_PATH = "voxglint/light_filters.json";
    static final GlassFilterMaterials EMPTY = new GlassFilterMaterials(Map.of());
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final Map<String, Material> VANILLA = Map.ofEntries(
            entry("white",      new Material(0.78f,  0.78f,  0.78f,  1)),
            entry("orange",     new Material(0.78f,  0.24f,  0.025f, 1)),
            entry("magenta",    new Material(0.72f,  0.06f,  0.58f,  1)),
            entry("light_blue", new Material(0.19f,  0.53f,  0.83f,  1)),
            entry("yellow",     new Material(0.84f,  0.72f,  0.04f,  1)),
            entry("lime",       new Material(0.35f,  0.83f,  0.055f, 1)),
            entry("pink",       new Material(0.89f,  0.32f,  0.48f,  1)),
            entry("gray",       new Material(0.24f,  0.24f,  0.24f,  1)),
            entry("light_gray", new Material(0.56f,  0.56f,  0.56f,  1)),
            entry("cyan",       new Material(0.035f, 0.48f,  0.59f,  1)),
            entry("purple",     new Material(0.30f,  0.045f, 0.54f,  1)),
            entry("blue",       new Material(0.025f, 0.075f, 0.72f,  1)),
            entry("brown",      new Material(0.34f,  0.14f,  0.055f, 1)),
            entry("green",      new Material(0.055f, 0.65f,  0.11f,  1)),
            entry("red",        new Material(0.75f,  0.045f, 0.035f, 1)),
            entry("black",      new Material(0.075f, 0.075f, 0.075f, 1)));

    private final Map<Block, Material> byBlock;

    private GlassFilterMaterials(Map<Block, Material> byBlock) {
        this.byBlock = Map.copyOf(byBlock);
    }

    static GlassFilterMaterials load(ResourceManager resources) {
        Map<Block, Material> byBlock = new HashMap<>();
        loadEntries(resources).forEach((id, material) -> {
            var registry = BuiltInRegistries.BLOCK;
            if (registry.containsKey(id)) {
                Block block = registry.getValue(id);
                if (!block.defaultBlockState().isAir()) {
                    byBlock.put(block, material);
                }
            }
        });
        return new GlassFilterMaterials(byBlock);
    }

    static Map<Identifier, Material> loadEntries(ResourceManager resources) {
        Map<Identifier, Material> entries = new HashMap<>();
        VANILLA.forEach((color, material) -> {
            entries.put(Identifier.withDefaultNamespace(color + "_stained_glass"), material);
            entries.put(Identifier.withDefaultNamespace(color + "_stained_glass_pane"), material);
        });
        var stacks = resources.listResourceStacks("voxglint", id -> id.getPath().equals(RESOURCE_PATH));
        stacks.forEach((file, stack) -> {
            // The resource manager returns each stack from low to high pack priority.
            for (var resource : stack) {
                try (var reader = resource.openAsReader()) {
                    var json = JsonParser.parseReader(reader);
                    if (!json.isJsonObject()) {
                        throw new IllegalArgumentException("expected an object of block paths");
                    }
                    for (var entry : json.getAsJsonObject().entrySet()) {
                        try {
                            Identifier block = Identifier.tryBuild(file.getNamespace(), entry.getKey());
                            if (block == null || block.getPath().isEmpty()) {
                                throw new IllegalArgumentException("expected a block path within the asset namespace");
                            }
                            if (entry.getValue().isJsonNull()) {
                                entries.remove(block);
                            } else {
                                entries.put(block, Material.parse(entry.getValue()));
                            }
                        } catch (IllegalArgumentException invalid) {
                            LOGGER.warn("Invalid VoxGlint filter {} in {} from {}: {}",
                                    entry.getKey(), file, resource.sourcePackId(), invalid.getMessage());
                        }
                    }
                } catch (IOException | JsonParseException | IllegalArgumentException invalid) {
                    LOGGER.warn("Could not read VoxGlint filters {} from {}: {}",
                            file, resource.sourcePackId(), invalid.getMessage());
                }
            }
        });
        return Map.copyOf(entries);
    }

    Material forState(BlockState state) {
        return byBlock.get(state.getBlock());
    }

    boolean isEmpty() {
        return byBlock.isEmpty();
    }

    boolean contains(BlockState state) {
        return byBlock.containsKey(state.getBlock());
    }

    record Material(float red, float green, float blue, float thickness) {
        Material {
            if (!isTransmittance(red) || !isTransmittance(green) || !isTransmittance(blue)
                    || !Float.isFinite(thickness) || thickness <= 0) {
                throw new IllegalArgumentException("transmittance must be in [0,1] and reference_thickness must be finite and positive");
            }
        }

        private static boolean isTransmittance(float value) {
            return value >= 0 && value <= 1;
        }

        private static Material parse(JsonElement element) {
            if (!element.isJsonObject()) {
                throw new IllegalArgumentException("expected a filter object or null");
            }
            var data = element.getAsJsonObject();
            var transmittance = data.get("transmittance");
            if (transmittance == null || !transmittance.isJsonArray() || transmittance.getAsJsonArray().size() != 3) {
                throw new IllegalArgumentException("expected three linear RGB transmittance channels");
            }
            var rgb = transmittance.getAsJsonArray();
            return new Material(channel(rgb.get(0)), channel(rgb.get(1)), channel(rgb.get(2)),
                    numeric(data.get("reference_thickness")));
        }

        private static float channel(JsonElement element) {
            if (element == null || !element.isJsonPrimitive() || !element.getAsJsonPrimitive().isNumber()) {
                throw new IllegalArgumentException("expected a numeric transmittance channel");
            }
            double value = element.getAsDouble();
            if (!(value >= 0 && value <= 1)) {
                throw new IllegalArgumentException("transmittance must be in [0,1]");
            }
            return (float) value;
        }

        private static float numeric(JsonElement element) {
            if (element == null || !element.isJsonPrimitive() || !element.getAsJsonPrimitive().isNumber()) {
                throw new IllegalArgumentException("expected numeric reference_thickness");
            }
            return element.getAsFloat();
        }
    }
}
