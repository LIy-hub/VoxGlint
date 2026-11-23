package dev.minecraftue.bridge;

import net.minecraft.core.BlockPos;
import net.minecraft.core.SectionPos;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.phys.shapes.VoxelShape;

import java.nio.ByteBuffer;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;

import static dev.minecraftue.bridge.BridgeWire.MAX_SECTION_GLASS_BOXES_PER_PART;
import static dev.minecraftue.bridge.BridgeWire.MAX_SECTION_GLASS_PARTS;

final class SectionGlass {
    static final Snapshot EMPTY = new Snapshot(List.of(), null, 0);
    private static final int MAX_BOXES = MAX_SECTION_GLASS_PARTS * MAX_SECTION_GLASS_BOXES_PER_PART;

    private SectionGlass() {}

    static Snapshot capture(int scene, SectionPos section, BlockGetter region, GlassFilterMaterials filters) {
        var byShape = new IdentityHashMap<VoxelShape, List<Bounds>>();
        var boxes = new ArrayList<Box>();
        for (BlockPos pos : BlockPos.betweenClosed(section.minBlockX(), section.minBlockY(), section.minBlockZ(),
                section.maxBlockX(), section.maxBlockY(), section.maxBlockZ())) {
            var state = region.getBlockState(pos);
            var material = filters.forState(state);
            if (material == null) {
                continue;
            }
            var shape = state.getShape(region, pos);
            var bounds = byShape.computeIfAbsent(shape, key -> boundsFor(key, section));
            if (boxes.size() > MAX_BOXES - bounds.size()) {
                throw capacityExceeded(section);
            }
            int localPos = (pos.getX() & 15) | ((pos.getY() & 15) << 4) | ((pos.getZ() & 15) << 8);
            for (var bound : bounds) {
                boxes.add(new Box(localPos, material, bound));
            }
        }
        return boxes.isEmpty() ? EMPTY : snapshot(scene, section, boxes);
    }

    private static List<Bounds> boundsFor(VoxelShape shape, SectionPos section) {
        var bounds = new ArrayList<Bounds>();
        // Minecraft decomposes the voxel union into disjoint boxes, including connected panes.
        shape.forAllBoxes((minX, minY, minZ, maxX, maxY, maxZ) -> {
            if (bounds.size() == MAX_BOXES) {
                throw capacityExceeded(section);
            }
            bounds.add(new Bounds((float) minX, (float) minY, (float) minZ,
                    (float) maxX, (float) maxY, (float) maxZ));
        });
        return bounds;
    }

    private static IllegalStateException capacityExceeded(SectionPos section) {
        return new IllegalStateException("VoxGlint glass section " + section + " exceeds " + MAX_BOXES + " filter boxes");
    }

    static Snapshot snapshot(int scene, SectionPos section, List<Box> boxes) {
        if (boxes.size() > MAX_BOXES) {
            throw capacityExceeded(section);
        }
        int partCount = Math.max(1, Math.ceilDiv(boxes.size(), MAX_SECTION_GLASS_BOXES_PER_PART));
        var parts = new ArrayList<ByteBuffer>(partCount);
        MessageDigest digest = newDigest();
        for (int partIndex = 0; partIndex < partCount; partIndex++) {
            int first = partIndex * MAX_SECTION_GLASS_BOXES_PER_PART;
            var part = encodePart(scene, section, partIndex, partCount,
                    boxes.subList(first, Math.min(boxes.size(), first + MAX_SECTION_GLASS_BOXES_PER_PART)));
            digest.update(part.asReadOnlyBuffer());
            parts.add(part.asReadOnlyBuffer());
        }
        return new Snapshot(List.copyOf(parts), digest.digest(), boxes.size());
    }

    static ByteBuffer clear(int scene, SectionPos section) {
        return encodePart(scene, section, 0, 1, List.of());
    }

    private static MessageDigest newDigest() {
        try {
            return MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException failure) {
            throw new AssertionError(failure);
        }
    }

    private static ByteBuffer encodePart(int scene, SectionPos section, int partIndex, int partCount, List<Box> boxes) {
        var materials = new LinkedHashMap<GlassFilterMaterials.Material, Integer>();
        for (var box : boxes) {
            materials.computeIfAbsent(box.material(), ignored -> materials.size());
        }
        int payload = BridgeWire.SECTION_GLASS_HEADER_BYTES + materials.size() * BridgeWire.SECTION_GLASS_MATERIAL_BYTES
                + boxes.size() * BridgeWire.SECTION_GLASS_BOX_BYTES;
        var frame = BridgeWire.frame(BridgeWire.SECTION_GLASS, payload).putInt(scene)
                .putInt(section.x()).putInt(section.y()).putInt(section.z())
                .putShort((short) partIndex).putShort((short) partCount)
                .putShort((short) materials.size()).putShort((short) boxes.size());
        for (var material : materials.keySet()) {
            frame.putFloat(material.red()).putFloat(material.green()).putFloat(material.blue()).putFloat(material.thickness());
        }
        for (var box : boxes) {
            var bounds = box.bounds();
            frame.putShort((short) box.localPos()).putShort(materials.get(box.material()).shortValue())
                    .putFloat(bounds.minX()).putFloat(bounds.minY()).putFloat(bounds.minZ())
                    .putFloat(bounds.maxX()).putFloat(bounds.maxY()).putFloat(bounds.maxZ());
        }
        return frame.flip();
    }

    record Snapshot(List<ByteBuffer> parts, byte[] digest, int boxCount) {}

    record Box(int localPos, GlassFilterMaterials.Material material, Bounds bounds) {}

    record Bounds(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
        Bounds {
            if (!ordered(minX, maxX) || !ordered(minY, maxY) || !ordered(minZ, maxZ)) {
                throw new IllegalArgumentException("VoxGlint filter bounds must be finite with min < max on each axis");
            }
        }

        private static boolean ordered(float min, float max) {
            return Float.isFinite(min) && Float.isFinite(max) && min < max;
        }
    }
}
