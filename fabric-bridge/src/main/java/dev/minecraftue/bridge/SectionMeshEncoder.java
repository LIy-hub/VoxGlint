package dev.minecraftue.bridge;

import com.mojang.blaze3d.IndexType;
import com.mojang.blaze3d.PrimitiveTopology;
import com.mojang.blaze3d.vertex.MeshData;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.SectionPos;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import net.minecraft.world.phys.Vec3;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;

import static com.mojang.blaze3d.vertex.DefaultVertexFormat.*;

final class SectionMeshEncoder {
    private SectionMeshEncoder() {}

    static ByteBuffer encode(SectionPos pos, Map<ChunkSectionLayer, MeshData> layers, SectionCapture capture) {
        long total = 0;
        for (var entry : layers.entrySet()) {
            var state = entry.getValue().drawState();
            if (state.primitiveTopology() != PrimitiveTopology.QUADS || state.vertexCount() % 4 != 0) {
                throw new IllegalArgumentException("Unsupported section topology " + state.primitiveTopology());
            }
            total += state.vertexCount();
        }
        if (total > BridgeWire.MAX_SECTION_VERTICES) {
            throw new IllegalArgumentException("Section " + pos + " has " + total + " vertices");
        }

        int[] counts = new int[BridgeWire.TERRAIN_BUCKETS];
        byte[][] sourceBuckets = new byte[ChunkSectionLayer.values().length][];
        byte[][] sourceLevels = new byte[sourceBuckets.length][];
        for (var entry : layers.entrySet()) {
            var layer = entry.getKey();
            int vertices = entry.getValue().drawState().vertexCount();
            sourceLevels[layer.ordinal()] = new byte[vertices / 4];
            sourceBuckets[layer.ordinal()] = capture.bucketsFor(layer,vertices,counts,sourceLevels[layer.ordinal()]);
        }

        int waterCount = counts[BridgeWire.WATER] / 4;
        int geometryBytes = BridgeWire.SECTION_HEADER_BYTES + (int) (total / 4) * BridgeWire.SECTION_QUAD_BYTES;
        int baseBytes = geometryBytes + BridgeWire.SECTION_WATER_HEADER_BYTES + waterCount * BridgeWire.WATER_FACE_BYTES;
        ByteBuffer frame = BridgeWire.frame(BridgeWire.SECTION_MESH, baseBytes);
        frame.putInt(pos.x()).putInt(pos.y()).putInt(pos.z());
        for (int count : counts) {
            frame.putInt(count);
        }

        ByteBuffer[] targets = new ByteBuffer[BridgeWire.TERRAIN_BUCKETS];
        int offset = frame.position();
        for (int bucket = 0; bucket < counts.length; bucket++) {
            int bytes = counts[bucket] / 4 * BridgeWire.SECTION_QUAD_BYTES;
            if (bytes != 0) {
                targets[bucket] = frame.slice(offset, bytes);
                offset += bytes;
            }
        }
        frame.position(offset);
        frame.putInt(waterCount).putInt(0);
        ByteBuffer waterFaces = frame.slice(frame.position(), waterCount * BridgeWire.WATER_FACE_BYTES);
        var shores = new ArrayList<Shore>();
        for (var entry : layers.entrySet()) {
            var layer = entry.getKey();
            encodeLayer(targets, entry.getValue(), sourceBuckets[layer.ordinal()],
                    capture.water(), capture.waterRuns(layer), pos.origin(), waterFaces, shores, capture, layer, sourceLevels[layer.ordinal()]);
        }
        frame.position(5 + baseBytes).flip();
        if (shores.isEmpty()) {
            return frame;
        }

        int payloadSize = Math.addExact(baseBytes, Math.multiplyExact(shores.size(), BridgeWire.SHORE_SEGMENT_BYTES));
        if (1 + payloadSize > BridgeWire.MAX_SECTION_FRAME_BYTES) {
            throw new IllegalArgumentException("Section water records exceed the frame limit");
        }
        ByteBuffer complete = BridgeWire.frame(BridgeWire.SECTION_MESH, payloadSize);
        frame.position(5);
        complete.put(frame);
        complete.putInt(5 + geometryBytes + Integer.BYTES, shores.size());
        for (Shore shore : shores) {
            shore.segment().write(complete, shore.bucket(), shore.quad());
        }
        return complete.flip();
    }

    private record Shore(int bucket, int quad, WaterCapture.Segment segment) {}

    private static WaterCapture.Cell findWaterCell(List<WaterCapture.Run> runs, int sourceQuad) {
        int low = 0;
        int high = runs.size();
        while (low < high) {
            int middle = (low + high) >>> 1;
            if (runs.get(middle).firstQuad() <= sourceQuad) {
                low = middle + 1;
            } else {
                high = middle;
            }
        }
        if (low == 0) throw new IllegalStateException("Missing fluid source for quad " + sourceQuad);
        return runs.get(low - 1).cell();
    }

    private static void encodeLayer(ByteBuffer[] targets, MeshData mesh, byte[] buckets,
                                    WaterCapture water, List<WaterCapture.Run> runs, BlockPos base,
                                    ByteBuffer waterFaces, List<Shore> shores, SectionCapture capture, ChunkSectionLayer layer, byte[] levels) {
        var state = mesh.drawState();
        var format = state.format();
        int stride = format.getVertexSize();
        int position = format.getElement(POSITION_SEMANTIC_NAME).offset();
        int uv = format.getElement(UV0_SEMANTIC_NAME).offset();
        int color = format.getElement(COLOR_SEMANTIC_NAME).offset();
        int light = format.getElement(UV2_SEMANTIC_NAME).offset();
        ByteBuffer vertices = mesh.vertexBuffer().order(ByteOrder.nativeOrder());
        ByteBuffer indices = mesh.indexBuffer();
        if (indices != null) {
            indices.order(ByteOrder.nativeOrder());
        }
        for (int quad = 0; quad < state.vertexCount() / 4; quad++) {
            int first = quad * 4;
            if (indices != null) {
                // Minecraft sorts the translucent layer by emitting six indices per quad.
                first = state.indexType() == IndexType.SHORT
                        ? Short.toUnsignedInt(indices.getShort(quad * 6 * Short.BYTES))
                        : indices.getInt(quad * 6 * Integer.BYTES);
            }
            // The sort changes quad order, not the source vertices to which the metadata belongs.
            if (first < 0 || first % 4 != 0 || first >= state.vertexCount()) {
                throw new IllegalArgumentException("Invalid sorted quad vertex " + first);
            }
            int entry = buckets[first / 4] & 0xff;
            int bucket = entry & SectionCapture.BUCKET_MASK;
            int faceFlags = entry & (BridgeWire.FACE_VEGETATION | BridgeWire.FACE_EMISSION_MASK);
            int sourceLevel = levels[first/4];
            if (sourceLevel > 0) faceFlags |= 0x40;
            ByteBuffer target = targets[bucket];
            int targetQuad = target.position() / BridgeWire.SECTION_QUAD_BYTES;
            int face = faceIndex(vertices, first * stride + position, stride);
            int faceOffset = target.position();
            target.put((byte) (face | faceFlags));
            int lava = capture.lavaOwner(layer,first/4);
            int[] lavaUV = lava >= 0 ? new int[4] : null;
            Vec3[] geometry = lava >= 0 || bucket == BridgeWire.WATER || (bucket != BridgeWire.FOLIAGE && (faceFlags & BridgeWire.FACE_VEGETATION) == 0)
                    && face != BridgeWire.GEOMETRIC_NORMAL && face != Direction.DOWN.get3DDataValue() ? new Vec3[4] : null;
            int skyLight = 0;
            for (int vertex = first; vertex < first + 4; vertex++) {
                int start = vertex * stride;
                float x = vertices.getFloat(start + position);
                float y = vertices.getFloat(start + position + 4);
                float z = vertices.getFloat(start + position + 8);
                if (geometry != null) geometry[vertex - first] = new Vec3(x, y, z);
                float u = vertices.getFloat(start + uv), v = vertices.getFloat(start + uv + 4);
                if (lavaUV != null) lavaUV[vertex-first] = Math.round(Math.clamp(u,0,1)*65535) << 16 | Math.round(Math.clamp(v,0,1)*65535);
                target.putFloat(x).putFloat(y).putFloat(z)
                        .putFloat(u).putFloat(v);
                for (int channel = 0; channel < 4; channel++) {
                    target.put(vertices.get(start + color + channel));
                }
                // The lamp's own vanilla propagated light would otherwise make even its handle self-lit.
                int block = sourceLevel > 0 ? sourceLevel : lava < 0 && (entry & SectionCapture.MATERIAL_LIGHT_BIT) == 0
                        ? vertices.getShort(start + light) >>> 4 & 15 : 0;
                int sky = vertices.getShort(start + light + Short.BYTES) >>> 4 & 15;
                skyLight = Math.max(skyLight, sky);
                target.put((byte) ((sky << 4) | block));
            }
            if (lavaUV != null) target.put(faceOffset,(byte)(face | (faceFlags & 0x40) | capture.addLavaFace(lava,geometry,lavaUV,face) << 3));
            if (bucket == BridgeWire.WATER) {
                water.writeFace(waterFaces, findWaterCell(runs, first / 4), geometry, skyLight);
            } else if (geometry != null) {
                for (var contact : water.contacts(base, geometry, face)) {
                    shores.add(new Shore(bucket, targetQuad, contact));
                }
            }
        }
    }

    private static int faceIndex(ByteBuffer vertices, int first, int stride) {
        // BLOCK has no normal attribute. On an axis-aligned face all four vertices share one coordinate.
        float x = vertices.getFloat(first);
        float y = vertices.getFloat(first + 4);
        float z = vertices.getFloat(first + 8);
        float ax = vertices.getFloat(first + stride) - x;
        float ay = vertices.getFloat(first + stride + 4) - y;
        float az = vertices.getFloat(first + stride + 8) - z;
        float bx = vertices.getFloat(first + stride * 2) - x;
        float by = vertices.getFloat(first + stride * 2 + 4) - y;
        float bz = vertices.getFloat(first + stride * 2 + 8) - z;
        float cx = vertices.getFloat(first + stride * 3) - x;
        float cy = vertices.getFloat(first + stride * 3 + 4) - y;
        float cz = vertices.getFloat(first + stride * 3 + 8) - z;
        float nx = ay * bz - az * by + by * cz - bz * cy;
        float ny = az * bx - ax * bz + bz * cx - bx * cz;
        float nz = ax * by - ay * bx + bx * cy - by * cx;

        if (ax == 0 && bx == 0 && cx == 0 && nx != 0) {
            return (nx > 0 ? Direction.EAST : Direction.WEST).get3DDataValue();
        }
        if (ay == 0 && by == 0 && cy == 0 && ny != 0) {
            return (ny > 0 ? Direction.UP : Direction.DOWN).get3DDataValue();
        }
        if (az == 0 && bz == 0 && cz == 0 && nz != 0) {
            return (nz > 0 ? Direction.SOUTH : Direction.NORTH).get3DDataValue();
        }
        return BridgeWire.GEOMETRIC_NORMAL;
    }
}
