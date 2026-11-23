package dev.minecraftue.bridge;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.BitSet;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.UUID;

/** One writer and one reader per connection. TCP publication and return transfer ownership of each range. */
final class TerrainSharedMemory implements AutoCloseable {
    static final int HEADER_BYTES = 64;
    static final int PAGE_BYTES = 16 * 1024;
    static final int BULK_PAGES = 1024;
    static final int PRIORITY_PAGES = 256;
    static final int MAX_BODY_BYTES = BridgeWire.MAX_SECTION_FRAME_BYTES - 1 + BridgeWire.MAX_SECTION_LIGHTS_BYTES;
    static final int LARGE_PAGES = (MAX_BODY_BYTES + PAGE_BYTES - 1) / PAGE_BYTES;
    static final int TOTAL_BYTES = HEADER_BYTES + (BULK_PAGES + PRIORITY_PAGES + LARGE_PAGES) * PAGE_BYTES;
    static final int MAX_LEASES = 96 + 32 + 1;
    private static final int FRAME_HEADER_BYTES = Integer.BYTES + 1;
    private static final ValueLayout.OfInt U32 = ValueLayout.JAVA_INT.withOrder(ByteOrder.LITTLE_ENDIAN);
    private static final ValueLayout.OfLong U64 = ValueLayout.JAVA_LONG.withOrder(ByteOrder.LITTLE_ENDIAN);

    private final UUID id = UUID.randomUUID();
    private final Win32SharedMemory memory;
    private final MemorySegment segment;
    private final Lane bulk = new Lane(0, BULK_PAGES, 96);
    private final Lane priority = new Lane(BULK_PAGES, PRIORITY_PAGES, 32);
    private final Lane large = new Lane(BULK_PAGES + PRIORITY_PAGES, LARGE_PAGES, 1);
    private final Map<Long, Lease> leases = new HashMap<>();
    private long nextToken;
    private boolean closed;

    TerrainSharedMemory(long nonce) {
        memory = Win32SharedMemory.create("{" + id.toString().toUpperCase(Locale.ROOT) + "}", TOTAL_BYTES);
        segment = memory.segment();
        segment.set(U32, 0, BridgeWire.MAGIC);
        segment.set(U32, 4, BridgeWire.VERSION);
        segment.set(U32, 8, TOTAL_BYTES);
        segment.set(U32, 12, HEADER_BYTES);
        segment.set(U64, 16, nonce);
    }

    UUID id() {
        return id;
    }

    synchronized ByteBuffer publish(int scene, boolean urgent, ByteBuffer mesh, ByteBuffer lights) {
        if (closed) {
            return null;
        }
        int meshBytes = mesh.remaining() - FRAME_HEADER_BYTES;
        int lightBytes = lights.remaining() - FRAME_HEADER_BYTES;
        if (meshBytes < BridgeWire.SECTION_HEADER_BYTES + BridgeWire.SECTION_WATER_HEADER_BYTES
                || meshBytes >= BridgeWire.MAX_SECTION_FRAME_BYTES
                || lightBytes < BridgeWire.SECTION_LIGHTS_HEADER_BYTES || lightBytes > BridgeWire.MAX_MATERIAL_LIGHTS_BYTES
                || meshBytes + lightBytes > MAX_BODY_BYTES) {
            throw new IllegalArgumentException("Invalid terrain shared memory payload size");
        }
        int pages = (meshBytes + lightBytes + PAGE_BYTES - 1) / PAGE_BYTES;
        Lease lease = urgent ? priority.claim(pages) : null;
        if (lease == null) {
            lease = (pages <= BULK_PAGES ? bulk : large).claim(pages);
        }
        if (lease == null) {
            return null;
        }
        int offset = HEADER_BYTES + (lease.lane.firstPage + lease.page) * PAGE_BYTES;
        try {
            MemorySegment.copy(MemorySegment.ofBuffer(mesh), FRAME_HEADER_BYTES, segment, offset, meshBytes);
            MemorySegment.copy(MemorySegment.ofBuffer(lights), FRAME_HEADER_BYTES, segment, offset + meshBytes, lightBytes);
        } catch (RuntimeException failure) {
            lease.lane.release(lease);
            throw failure;
        }
        long token = ++nextToken;
        leases.put(token, lease);
        return BridgeWire.frame(BridgeWire.TERRAIN_SHM, BridgeWire.TERRAIN_SHM_BYTES)
                .putInt(scene).putLong(token).putInt(offset).putInt(meshBytes).putInt(lightBytes)
                .put((byte) (urgent ? 1 : 0)).flip();
    }

    synchronized boolean release(long token) {
        Lease lease = leases.remove(token);
        if (lease == null) {
            return false;
        }
        lease.lane.release(lease);
        return true;
    }

    @Override
    public synchronized void close() {
        if (!closed) {
            closed = true;
            leases.clear();
            memory.close();
        }
    }

    private static final class Lane {
        private final int firstPage;
        private final int pages;
        private final int maxLeases;
        private final BitSet occupied;
        private int inFlight;

        private Lane(int firstPage, int pages, int maxLeases) {
            this.firstPage = firstPage;
            this.pages = pages;
            this.maxLeases = maxLeases;
            occupied = new BitSet(pages);
        }

        private Lease claim(int count) {
            if (inFlight == maxLeases || count > pages) {
                return null;
            }
            for (int page = occupied.nextClearBit(0); page + count <= pages;) {
                int next = occupied.nextSetBit(page);
                if (next < 0 || next >= page + count) {
                    occupied.set(page, page + count);
                    inFlight++;
                    return new Lease(this, page, count);
                }
                page = occupied.nextClearBit(next);
            }
            return null;
        }

        private void release(Lease lease) {
            occupied.clear(lease.page, lease.page + lease.pages);
            inFlight--;
        }
    }

    private record Lease(Lane lane, int page, int pages) {}
}
