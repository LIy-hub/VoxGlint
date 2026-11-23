package dev.minecraftue.bridge;

import java.nio.ByteBuffer;

import static dev.minecraftue.bridge.BridgeWire.*;

record DynamicGeometry(int texture, int blend, int topology, ByteBuffer vertices, int hash) {
    DynamicGeometry(int texture, int blend, int topology, ByteBuffer vertices) {
        this(texture, blend, topology, vertices, vertices.hashCode());
    }

    int vertexCount() {
        return vertices.remaining() / DYNAMIC_VERTEX_BYTES;
    }

    ByteBuffer encode(int scene, int space, int meshId) {
        return frame(DYNAMIC_MESH, DYNAMIC_MESH_HEADER_BYTES + vertices.remaining())
                .putInt(scene).put((byte) space).putInt(meshId).putInt(texture)
                .put((byte) blend).put((byte) topology).putInt(vertexCount())
                .put(vertices.duplicate()).flip();
    }

    @Override
    public int hashCode() {
        return ((31 * texture + topology) * 31 + blend) * 31 + hash;
    }
}
