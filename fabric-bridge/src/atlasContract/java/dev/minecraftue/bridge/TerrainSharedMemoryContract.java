package dev.minecraftue.bridge;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;

public final class TerrainSharedMemoryContract {
    public static void main(String[] args) {
        ByteBuffer mesh = BridgeWire.frame(BridgeWire.SECTION_MESH, 44).position(49).flip();
        ByteBuffer lights = BridgeWire.frame(BridgeWire.SECTION_LIGHTS, 18).position(23).flip();
        int bulkStart = TerrainSharedMemory.HEADER_BYTES;
        int nearStart = bulkStart + TerrainSharedMemory.PAGE_BYTES * TerrainSharedMemory.BULK_PAGES;
        int largeStart = nearStart + TerrainSharedMemory.PAGE_BYTES * TerrainSharedMemory.PRIORITY_PAGES;
        int token = 0;
        try (var arena = new TerrainSharedMemory(123)) {
            List<ByteBuffer> descriptors = new ArrayList<>();
            for (int count = 0; count < 96; count++) {
                var descriptor = arena.publish(3, false, mesh, lights);
                verify(descriptor, ++token, bulkStart + count * TerrainSharedMemory.PAGE_BYTES, 44, 18, false);
                descriptors.add(descriptor);
            }
            require(arena.publish(3, false, mesh, lights) == null, "bulk lease count");
            for (int count = 0; count < 32; count++) {
                var descriptor = arena.publish(3, true, mesh, lights);
                verify(descriptor, ++token, nearStart + count * TerrainSharedMemory.PAGE_BYTES, 44, 18, true);
            }
            require(arena.publish(3, true, mesh, lights) == null, "priority lease count");

            int maxMeshBytes = BridgeWire.MAX_SECTION_FRAME_BYTES - 1;
            int maxLightBytes = BridgeWire.MAX_SECTION_LIGHTS_BYTES;
            var largeMesh = BridgeWire.frame(BridgeWire.SECTION_MESH, maxMeshBytes).position(5 + maxMeshBytes).flip();
            var largeLights = BridgeWire.frame(BridgeWire.SECTION_LIGHTS, maxLightBytes).position(5 + maxLightBytes).flip();
            var large = arena.publish(3, false, largeMesh, largeLights);
            verify(large, ++token, largeStart, maxMeshBytes, maxLightBytes, false);
            require(token == TerrainSharedMemory.MAX_LEASES, "total lease count");
            require(arena.publish(3, true, largeMesh, largeLights) == null, "large lane while held");

            require(arena.release(7) && !arena.release(7), "out-of-order and duplicate release");
            var reused = arena.publish(3, false, mesh, lights);
            verify(reused, ++token, descriptors.get(6).getInt(17), 44, 18, false);
            require(arena.release(token - 1), "large lease release");
            verify(arena.publish(3, true, largeMesh, largeLights), ++token, largeStart, maxMeshBytes, maxLightBytes, true);
            require(mesh.position() == 0 && lights.position() == 0, "caller retains input views");
        }
        System.out.printf("v27 terrain arena contract passed (max mesh %d bytes, max combined %d bytes, mapping %d bytes)%n",
                BridgeWire.MAX_SECTION_FRAME_BYTES - 1, TerrainSharedMemory.MAX_BODY_BYTES, TerrainSharedMemory.TOTAL_BYTES);
    }

    private static void verify(ByteBuffer descriptor, long token, int offset, int mesh, int lights, boolean urgent) {
        require(descriptor != null && descriptor.remaining() == 30, "descriptor length");
        ByteBuffer read = descriptor.duplicate();
        require(read.getInt() == 26 && read.get() == BridgeWire.TERRAIN_SHM && read.getInt() == 3, "descriptor header");
        require(read.getLong() == token && read.getInt() == offset, "descriptor lease");
        require(read.getInt() == mesh && read.getInt() == lights && read.get() == (urgent ? 1 : 0), "descriptor body");
    }

    private static void require(boolean condition, String description) {
        if (!condition) {
            throw new AssertionError(description);
        }
    }
}
