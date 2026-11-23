package dev.minecraftue.bridge;

import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Locale;
import java.util.UUID;

/**
 * 64-byte cache-line aligned Windows shared memory for ultra-low-latency camera,
 * look-epoch arbitration, block selection outline, and first-person hand state.
 * Layout matches ue-renderer/Source/MinecraftUEViewer/Private/BridgeShmProtocol.h byte-for-byte.
 */
final class ControlSharedMemory implements AutoCloseable {
    static final int MAGIC = BridgeWire.MAGIC;
    static final int VERSION = BridgeWire.VERSION;
    static final int CAMERA_SAMPLE_BYTES = 96;
    static final int CAMERA_MOVEMENT_KNOWN = 1 << 0;
    static final int CAMERA_GROUNDED_ON_FOOT = 1 << 1;

    // McToUeState.flags bits
    static final int FLAG_IN_WORLD = 1 << 0;
    static final int FLAG_GAMEPLAY_LOOK = 1 << 1;
    static final int FLAG_GUI_OPEN = 1 << 2;
    static final int FLAG_IS_PAUSED = 1 << 3;
    static final int FLAG_SNEAKING = 1 << 4;
    static final int FLAG_SPRINTING = 1 << 5;
    static final int FLAG_SWIMMING = 1 << 6;
    static final int FLAG_USING_ITEM = 1 << 7;
    static final int FLAG_HAS_BLOCK_HIT = 1 << 8;
    static final int FLAG_INVERT_X = 1 << 9;
    static final int FLAG_INVERT_Y = 1 << 10;
    static final int FLAG_HAS_EYE_POSE = 1 << 11;

    // UeToMcLook.flags bits
    static final int LOOK_FLAG_ACTIVE = 1 << 0;

    private static final long UE_CAPS = 16;
    private static final long MAPPING_SIZE = 24;
    private static final long CAP_WATER_BUBBLES = 1;
    static final long CAP_COVERED_FACE_LIGHTS = 1L << 1;
    static final long CAP_SELECTION_OUTLINE = 1L << 2;
    static final long CAP_VEGETATION_SHADING = 1L << 3;
    static final long CAP_MATERIAL_LIGHTS = 1L << 4;
    static final long CAP_TEXTURE_EMISSION = 1L << 5;
    static final long CAP_STAINED_GLASS = 1L << 6;
    static final long CAP_MINERAL_GLOW = 1L << 7;
    private static final long MC_SEQ = 64;
    private static final long MC_DATA = 128;
    private static final long LOOK_SEQ = 320;
    private static final long LOOK_DATA = 384;
    private static final long CAMERA_SEQ = 448;
    private static final long CAMERA_DATA = 512;
    private static final long SELECTION_SEQ = 640;
    private static final long SELECTION_DATA = 704;
    private static final int MAX_SELECTION_EDGES = 256;
    private static final int SELECTION_HEADER_BYTES = 8;
    private static final int SELECTION_EDGE_BYTES = 6 * Float.BYTES;
    private static final long TOTAL_BYTES =
            (SELECTION_DATA + SELECTION_HEADER_BYTES + MAX_SELECTION_EDGES * SELECTION_EDGE_BYTES + 63L) & ~63L;

    private static final VarHandle VH_INT = ValueLayout.JAVA_INT.withOrder(ByteOrder.LITTLE_ENDIAN).varHandle();
    private static final VarHandle VH_LONG = ValueLayout.JAVA_LONG.withOrder(ByteOrder.LITTLE_ENDIAN).varHandle();
    private final UUID id = UUID.randomUUID();
    private final Win32SharedMemory memory;
    private final MemorySegment segment;
    private final ByteBuffer stateBytes = ByteBuffer.allocate(192).order(ByteOrder.LITTLE_ENDIAN);
    private final ByteBuffer lookBytes = ByteBuffer.allocate(40).order(ByteOrder.LITTLE_ENDIAN);
    private final ByteBuffer selectionBytes = ByteBuffer.allocate(
            SELECTION_HEADER_BYTES + (MAX_SELECTION_EDGES + 1) * SELECTION_EDGE_BYTES).order(ByteOrder.LITTLE_ENDIAN);
    private boolean closed;
    private long mcSeq;
    private long cameraSeq;
    private long selectionSeq;
    private int selectionRevision;
    private VoxelShape lastSelectionOutline;

    ControlSharedMemory(long nonce) {
        String name = "{" + id.toString().toUpperCase(Locale.ROOT) + "}";
        memory = Win32SharedMemory.create(name, TOTAL_BYTES);
        segment = memory.segment();
        VH_INT.set(segment, 0L, MAGIC);
        VH_INT.set(segment, 4L, VERSION);
        VH_LONG.set(segment, 8L, nonce);
        VH_LONG.set(segment, MAPPING_SIZE, TOTAL_BYTES);
    }

    boolean supportsWaterBubbles() {
        return supports(CAP_WATER_BUBBLES);
    }

    synchronized boolean supports(long capability) {
        return !closed && ((long) VH_LONG.getAcquire(segment, UE_CAPS) & capability) != 0;
    }

    UUID id() {
        return id;
    }

    synchronized void publishState(StateSnapshot s, VoxelShape outline) {
        if (closed) {
            return;
        }
        int revision = publishSelectionOutline(outline);
        stateBytes.clear();
        stateBytes.putInt(s.worldSerial).putInt(s.flags).putLong(s.tickId).putLong(s.tickTimeNs)
                .putLong(s.overrideEpoch).putInt(s.teleportEpoch).putInt(s.hitBlockX).putInt(s.hitBlockY).putInt(s.hitBlockZ)
                .putDouble(s.prevX).putDouble(s.prevY).putDouble(s.prevZ)
                .putDouble(s.currX).putDouble(s.currY).putDouble(s.currZ).putDouble(s.mouseFactor)
                .putFloat(s.prevEyeHeight).putFloat(s.currEyeHeight).putFloat(s.forcedYaw).putFloat(s.forcedPitch)
                .putFloat(s.verticalFov).putFloat(s.aspectRatio).putFloat(s.mainHandSwing).putFloat(s.mainHandEquip)
                .putFloat(s.offHandSwing).putFloat(s.offHandEquip).putFloat(s.useProgress)
                .put(s.hitFace).put(s.useAction).putShort((short) revision)
                .putLong(s.mainHandModelHash).putLong(s.offHandModelHash).putDouble(s.eyeX).putDouble(s.eyeY).putDouble(s.eyeZ);
        mcSeq = publishWords(MC_SEQ, MC_DATA, mcSeq, stateBytes, stateBytes.capacity());
    }

    private int publishSelectionOutline(VoxelShape outline) {
        if (outline == null) {
            return 0;
        }
        if (outline != lastSelectionOutline) {
            int edgeCount = writeSelectionEdges(outline);
            if (edgeCount > MAX_SELECTION_EDGES) {
                edgeCount = writeSelectionEdges(Shapes.create(outline.bounds()));
            }
            selectionRevision = selectionRevision % 0xffff + 1;
            selectionBytes.putShort(0, (short) selectionRevision).putShort(2, (short) edgeCount).putInt(4, 0);
            selectionSeq = publishWords(SELECTION_SEQ, SELECTION_DATA, selectionSeq,
                    selectionBytes, SELECTION_HEADER_BYTES + edgeCount * SELECTION_EDGE_BYTES);
            lastSelectionOutline = outline;
        }
        return selectionRevision;
    }

    private int writeSelectionEdges(VoxelShape outline) {
        selectionBytes.clear().position(SELECTION_HEADER_BYTES);
        outline.forAllEdges((ax, ay, az, bx, by, bz) -> {
            if (selectionBytes.remaining() >= SELECTION_EDGE_BYTES) {
                selectionBytes.putFloat((float) ax).putFloat((float) ay).putFloat((float) az)
                        .putFloat((float) bx).putFloat((float) by).putFloat((float) bz);
            }
        });
        return (selectionBytes.position() - SELECTION_HEADER_BYTES) / SELECTION_EDGE_BYTES;
    }

    synchronized void publishCamera(ByteBuffer pose) {
        if (!closed) {
            cameraSeq = publishWords(CAMERA_SEQ, CAMERA_DATA, cameraSeq, pose, pose.capacity());
        }
    }

    // Write independently atomic aligned words between odd and even sequence values.
    private long publishWords(long seqOffset, long dataOffset, long seq, ByteBuffer data, int bytes) {
        VH_LONG.setOpaque(segment, seqOffset, seq + 1);
        VarHandle.storeStoreFence();
        for (int offset = 0; offset < bytes; offset += Long.BYTES) {
            long word = data.getLong(offset);
            VH_LONG.setOpaque(segment, dataOffset + offset,
                    data.order() == ByteOrder.LITTLE_ENDIAN ? word : Long.reverseBytes(word));
        }
        VH_LONG.setRelease(segment, seqOffset, seq + 2);
        return seq + 2;
    }

    synchronized boolean readUeLook(LookSample out) {
        if (closed) {
            return false;
        }
        for (int retry = 0; retry < 4; retry++) {
            long before = (long) VH_LONG.getAcquire(segment, LOOK_SEQ);
            if (before == 0L || (before & 1L) != 0L) {
                continue;
            }
            for (int offset = 0; offset < lookBytes.capacity(); offset += Long.BYTES) {
                lookBytes.putLong(offset, (long) VH_LONG.getOpaque(segment, LOOK_DATA + offset));
            }
            VarHandle.loadLoadFence();
            if (before != (long) VH_LONG.getOpaque(segment, LOOK_SEQ)) {
                continue;
            }
            lookBytes.clear();
            out.ackOverrideEpoch = lookBytes.getLong();
            out.timestampNs = lookBytes.getLong();
            out.worldSerial = lookBytes.getInt();
            out.captureId = lookBytes.getInt();
            out.lookSerial = lookBytes.getInt();
            out.yaw = lookBytes.getFloat();
            out.pitch = lookBytes.getFloat();
            out.flags = lookBytes.getInt();
            return true;
        }
        return false;
    }

    @Override
    public synchronized void close() {
        if (!closed) {
            closed = true;
            memory.close();
        }
    }

    static final class StateSnapshot {
        int worldSerial;
        int flags;
        long tickId;
        long tickTimeNs;
        long overrideEpoch;
        int teleportEpoch;
        int hitBlockX;
        int hitBlockY;
        int hitBlockZ;
        double prevX;
        double prevY;
        double prevZ;
        double currX;
        double currY;
        double currZ;
        double mouseFactor;
        float prevEyeHeight;
        float currEyeHeight;
        float forcedYaw;
        float forcedPitch;
        float verticalFov;
        float aspectRatio;
        float mainHandSwing;
        float mainHandEquip = 1.0f;
        float offHandSwing;
        float offHandEquip = 1.0f;
        float useProgress;
        byte hitFace;
        byte useAction;
        long mainHandModelHash;
        long offHandModelHash;
        double eyeX;
        double eyeY;
        double eyeZ;
    }

    static final class LookSample {
        long ackOverrideEpoch;
        long timestampNs;
        int worldSerial;
        int captureId;
        int lookSerial;
        float yaw;
        float pitch;
        int flags;

        boolean active() {
            return (flags & LOOK_FLAG_ACTIVE) != 0;
        }
    }
}
