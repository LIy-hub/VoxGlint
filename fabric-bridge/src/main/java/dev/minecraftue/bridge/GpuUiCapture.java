package dev.minecraftue.bridge;

import com.mojang.blaze3d.opengl.GlTexture;
import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.blaze3d.vulkan.VulkanGpuTexture;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.logging.LogUtils;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.EXTMemoryObject;
import org.lwjgl.opengl.EXTMemoryObjectWin32;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.system.Platform;
import org.slf4j.Logger;

import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.VarHandle;
import java.nio.ByteOrder;
import java.util.Locale;
import java.util.UUID;

import static org.lwjgl.opengl.EXTMemoryObject.*;
import static org.lwjgl.opengl.EXTMemoryObjectWin32.*;
import static org.lwjgl.opengl.EXTSemaphore.*;
import static org.lwjgl.opengl.EXTSemaphoreWin32.*;
import static org.lwjgl.opengl.GL11C.*;
import static org.lwjgl.opengl.GL43C.glCopyImageSubData;
import static org.lwjgl.system.MemoryUtil.memAddress;

/** One GPU copy per published HUD frame. The consumer owns the textures and fence handles. */
final class GpuUiCapture {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final long TIMEOUT_NS = 2_000_000_000L;
    private static final long CAPTURE_GAP_NS = 500_000_000L;
    private static final int MAILBOX_BYTES = 208;
    private static final int RENDERER_PID = 40;
    private static final int TEXTURE_HANDLE_BASE = 160;
    private static final int FENCE_HANDLE_BASE = 184;
    private static final int STATUS = 0;
    private static final int ABANDONED = 4;
    private static final int TARGET_FPS = 8;
    private static final int DRAWN = 16;
    private static final int PUBLISHED = 24;
    private static final int DROPPED = 32;
    private static final int SLOT_BASE = 64;
    private static final int SLOT_BYTES = 32;
    private static final int RELEASED = 8;
    private static final int OPAQUE = 16;
    private static final VarHandle INT = ValueLayout.JAVA_INT.withOrder(ByteOrder.LITTLE_ENDIAN).varHandle();
    private static final VarHandle LONG = ValueLayout.JAVA_LONG.withOrder(ByteOrder.LITTLE_ENDIAN).varHandle();

    private Pool pool;
    private boolean disabled;
    private final RetryPolicy retryPolicy = new RetryPolicy();

    boolean capture(LocalTransport.Connection connection, RenderTarget target, boolean opaque, long now) {
        if (disabled || !retryPolicy.ready(now)) {
            return false;
        }
        GpuTexture source = target.getColorTexture();
        if (Platform.get() != Platform.WINDOWS || Boolean.getBoolean("minecraftue.cpuHud")) {
            disabled = true;
            return false;
        }
        try {
            if (pool != null && (pool.width != target.width || pool.height != target.height)) {
                closePool();
            }
            if (pool == null) {
                pool = new Pool(connection, target.width, target.height, now, source);
            }
            Pool current = pool;
            INT.setRelease(current.mailbox, (long) DRAWN, ++current.drawn);
            int status = (int) INT.getAcquire(current.mailbox, (long) STATUS);
            if (status < 0) {
                throw new IllegalStateException("D3D12 did not offer compatible shared textures");
            }
            if (status == 0) {
                if (now - current.offeredAt > TIMEOUT_NS) {
                    throw new TransientInteropException("D3D12 shared HUD offer timed out");
                }
                return false;
            }
            if (!current.imported) {
                current.importObjects();
            }
            boolean submitted = current.submit(source, opaque, now);
            if (current.reclaimed && retryPolicy.hasPendingRetries()) {
                retryPolicy.succeeded();
                LOGGER.info("Minecraft UE HUD: recovered {}/D3D12 shared textures {}x{}",
                        current.vulkan == null ? "OpenGL" : "Vulkan", current.width, current.height);
            }
            return submitted;
        } catch (TransientInteropException stall) {
            closePool();
            long delayNs = retryPolicy.scheduleRetry(now);
            if (delayNs > 0L) {
                LOGGER.warn("Minecraft UE HUD: temporarily using system memory after {} (retrying GPU interop in {}s)",
                        stall.getMessage(), delayNs / 1_000_000_000L);
            } else {
                LOGGER.warn("Minecraft UE HUD: using system memory after repeated interop stalls", stall);
                disabled = true;
            }
            return false;
        } catch (RuntimeException failure) {
            LOGGER.warn("Minecraft UE HUD: using system memory after interop failed", failure);
            closePool();
            disabled = true;
            return false;
        }
    }

    void afterFrameSubmit() {
        if (pool != null && pool.pendingSlot >= 0) {
            pool.publish(pool.pendingSlot, pool.pendingOpaque);
            pool.pendingSlot = -1;
        }
    }

    record Adapter(int luidLow, int luidHigh, int nodes) {}

    private static boolean supportsOpenGl() {
        var caps = GL.getCapabilities();
        return caps.GL_EXT_memory_object && caps.GL_EXT_memory_object_win32
                && caps.GL_EXT_semaphore && caps.GL_EXT_semaphore_win32
                && (caps.OpenGL43 || caps.GL_ARB_copy_image);
    }

    boolean active() {
        return pool != null && pool.sequence != 0;
    }

    int frameLimit() {
        if (!active()) {
            return UiCapture.TARGET_FPS;
        }
        return Math.clamp((int) INT.getAcquire(pool.mailbox, (long) TARGET_FPS), 10, 250);
    }

    void reset() {
        closePool();
        disabled = false;
        retryPolicy.succeeded();
    }

    private void closePool() {
        if (pool != null) {
            pool.close();
            pool = null;
        }
    }

    private static final class TransientInteropException extends RuntimeException {
        TransientInteropException(String message) {
            super(message);
        }
    }

    static final class RetryPolicy {
        static final long INITIAL_BACKOFF_NS = 4_000_000_000L;
        static final int MAX_RETRIES = 4;

        private int attempts;
        private long nextAttemptAt;
        private boolean exhausted;

        boolean ready(long now) {
            return !exhausted && (attempts == 0 || now >= nextAttemptAt);
        }

        boolean hasPendingRetries() {
            return attempts > 0;
        }

        long scheduleRetry(long now) {
            if (attempts >= MAX_RETRIES) {
                exhausted = true;
                return 0L;
            }
            long delay = INITIAL_BACKOFF_NS << attempts;
            attempts++;
            nextAttemptAt = now + delay;
            return delay;
        }

        void succeeded() {
            attempts = 0;
            nextAttemptAt = 0L;
            exhausted = false;
        }

        boolean exhausted() {
            return exhausted;
        }
    }

    static final class ProgressWatchdog {
        private boolean blocked;
        private long blockedSince;
        private long lastAttempt;

        boolean waiting(long now) {
            // Resource reload and world startup can pause capture with all slots occupied.
            if (!blocked || now - lastAttempt >= CAPTURE_GAP_NS) {
                blocked = true;
                blockedSince = now;
            }
            lastAttempt = now;
            return now - blockedSince >= TIMEOUT_NS;
        }

        void progressed() {
            blocked = false;
        }
    }

    private static final class Pool implements AutoCloseable {
        final int width;
        final int height;
        final String name;
        final Win32SharedMemory memory;
        final MemorySegment mailbox;
        final Slot[] slots = {new Slot(), new Slot(), new Slot()};
        int drawn;
        long sequence;
        long dropped;
        int usedSlots;
        final long offeredAt;
        final ProgressWatchdog watchdog = new ProgressWatchdog();
        boolean imported;
        boolean reclaimed;
        final VulkanHudInterop vulkan;
        int pendingSlot = -1;
        boolean pendingOpaque;

        Pool(LocalTransport.Connection connection, int width, int height, long now, GpuTexture source) {
            Adapter adapter;
            if (source instanceof VulkanGpuTexture vkSource) {
                vulkan = new VulkanHudInterop(vkSource);
                adapter = vulkan.adapter;
            } else if (source instanceof GlTexture && supportsOpenGl()) {
                vulkan = null;
                try (MemoryStack stack = MemoryStack.stackPush()) {
                    var luid = stack.malloc(8).order(ByteOrder.LITTLE_ENDIAN);
                    EXTMemoryObject.glGetUnsignedBytevEXT(EXTMemoryObjectWin32.GL_DEVICE_LUID_EXT, luid);
                    int nodes = glGetInteger(EXTMemoryObjectWin32.GL_DEVICE_NODE_MASK_EXT);
                    adapter = new Adapter(luid.getInt(0), luid.getInt(4), nodes);
                }
            } else {
                throw new IllegalStateException("HUD interop is unavailable for this graphics backend");
            }
            this.width = width;
            this.height = height;
            offeredAt = now;
            UUID id = UUID.randomUUID();
            String guid = id.toString().toUpperCase(Locale.ROOT);
            name = "MCUE-HUD-" + guid;
            memory = Win32SharedMemory.create("{" + guid + "}", MAILBOX_BYTES);
            mailbox = memory.segment();
            mailbox.fill((byte) 0);
            connection.sendUi(BridgeWire.frame(BridgeWire.GPU_UI, 33)
                    .putLong(id.getMostSignificantBits()).putLong(id.getLeastSignificantBits())
                    .putShort((short) width).putShort((short) height)
                    .putInt(adapter.luidLow()).putInt(adapter.luidHigh()).putInt(adapter.nodes())
                    .put((byte) (vulkan == null ? 0 : 1)).flip());
        }

        void importObjects() {
            if (vulkan != null) {
                long[] textures = new long[slots.length];
                long[] fences = new long[slots.length];
                for (int i = 0; i < slots.length; ++i) {
                    textures[i] = (long) LONG.get(mailbox, TEXTURE_HANDLE_BASE + i * 8L);
                    fences[i] = (long) LONG.get(mailbox, FENCE_HANDLE_BASE + i * 8L);
                }
                vulkan.importObjects(width, height, (int) INT.get(mailbox, (long) RENDERER_PID), textures, fences);
                imported = true;
                return;
            }
            int previous = glGetInteger(GL_TEXTURE_BINDING_2D);
            try (MemoryStack stack = MemoryStack.stackPush()) {
                for (int index = 0; index < slots.length; index++) {
                    Slot slot = slots[index];
                    slot.memory = glCreateMemoryObjectsEXT();
                    glMemoryObjectParameteriEXT(slot.memory, GL_DEDICATED_MEMORY_OBJECT_EXT, GL_TRUE);
                    glImportMemoryWin32NameEXT(slot.memory, 0, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT,
                            memAddress(stack.UTF16(name + "-T" + index)));
                    slot.texture = glGenTextures();
                    glBindTexture(GL_TEXTURE_2D, slot.texture);
                    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, width, height, slot.memory, 0);
                    slot.fence = glGenSemaphoresEXT();
                    glImportSemaphoreWin32NameEXT(slot.fence, GL_HANDLE_TYPE_D3D12_FENCE_EXT,
                            memAddress(stack.UTF16(name + "-F" + index)));
                    checkGl("importing the shared HUD texture and fence");
                }
                imported = true;
            } finally {
                glBindTexture(GL_TEXTURE_2D, previous);
            }
        }

        boolean submit(GpuTexture source, boolean opaque, long now) {
            if (pendingSlot >= 0) {
                return true;
            }
            for (int index = 0; index < slots.length; index++) {
                Slot slot = slots[index];
                long offset = SLOT_BASE + (long) index * SLOT_BYTES;
                if ((long) LONG.getAcquire(mailbox, offset + RELEASED) != slot.sequence) {
                    continue;
                }
                if (slot.sequence != 0) {
                    reclaimed = true;
                }
                long next = sequence + 1;
                if (vulkan != null) {
                    vulkan.copy((VulkanGpuTexture) source, index, slot.sequence, next, width, height);
                    pendingSlot = index;
                    pendingOpaque = opaque;
                    return true;
                }
                try (MemoryStack stack = MemoryStack.stackPush()) {
                    var buffers = stack.mallocInt(0);
                    var texture = stack.ints(slot.texture);
                    var layout = stack.ints(GL_LAYOUT_GENERAL_EXT);
                    if (slot.sequence != 0) {
                        glSemaphoreParameterui64EXT(slot.fence, GL_D3D12_FENCE_VALUE_EXT, 2 * slot.sequence);
                        glWaitSemaphoreEXT(slot.fence, buffers, texture, layout);
                    }
                    glCopyImageSubData(((GlTexture) source).glId(), GL_TEXTURE_2D, 0, 0, 0, 0, slot.texture, GL_TEXTURE_2D,
                            0, 0, 0, 0, width, height, 1);
                    glSemaphoreParameterui64EXT(slot.fence, GL_D3D12_FENCE_VALUE_EXT, 2 * next - 1);
                    glSignalSemaphoreEXT(slot.fence, buffers, texture, layout);
                    glFlush();
                    if (sequence == 0) {
                        checkGl("copying the first shared HUD frame");
                    }
                }
                publish(index, opaque);
                return true;
            }
            LONG.setRelease(mailbox, (long) DROPPED, ++dropped);
            if (watchdog.waiting(now)) {
                throw new TransientInteropException("shared HUD fences stopped making progress");
            }
            return true;
        }

        void publish(int index, boolean opaque) {
            if (sequence == 0) {
                LOGGER.info("Minecraft UE HUD: {}/D3D12 shared textures {}x{}", vulkan == null ? "OpenGL" : "Vulkan", width, height);
            }
            if ((usedSlots & (1 << index)) == 0) {
                usedSlots |= 1 << index;
                LOGGER.debug("Minecraft UE shared HUD slot {} published (used mask=0x{})", index,
                        Integer.toHexString(usedSlots));
                if (usedSlots == 0b111 && vulkan != null) {
                    LOGGER.info("Minecraft UE HUD: all three Vulkan/D3D12 slots published {}x{}", width, height);
                }
            }
            long next = ++sequence;
            slots[index].sequence = next;
            watchdog.progressed();
            long offset = SLOT_BASE + (long) index * SLOT_BYTES;
            INT.set(mailbox, offset + OPAQUE, opaque ? 1 : 0);
            LONG.setRelease(mailbox, offset, next);
            LONG.setRelease(mailbox, (long) PUBLISHED, next);
        }

        @Override
        public void close() {
            INT.setRelease(mailbox, (long) ABANDONED, 1);
            for (Slot slot : slots) {
                if (slot.texture != 0) glDeleteTextures(slot.texture);
                if (slot.fence != 0) glDeleteSemaphoresEXT(slot.fence);
                if (slot.memory != 0) glDeleteMemoryObjectsEXT(slot.memory);
            }
            if (vulkan != null) {
                vulkan.close();
            }
            memory.close();
        }
    }

    private static void checkGl(String operation) {
        int error = glGetError();
        if (error != GL_NO_ERROR) {
            throw new IllegalStateException(operation + " failed (GL 0x" + Integer.toHexString(error) + ")");
        }
    }

    private static final class Slot {
        int texture;
        int memory;
        int fence;
        long sequence;
    }
}
