package dev.minecraftue.bridge;

import com.mojang.blaze3d.buffers.GpuBuffer;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.logging.LogUtils;
import com.sun.jna.Pointer;
import com.sun.jna.platform.win32.Kernel32;
import com.sun.jna.platform.win32.WinBase;
import com.sun.jna.platform.win32.WinNT;
import org.slf4j.Logger;

import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.util.Locale;
import java.util.UUID;

/** Publishes full-resolution pixels through acknowledged Windows shared-memory slots. */
public final class UiCapture {
    public static final UiCapture INSTANCE = new UiCapture();
    public static final int TARGET_FPS = 120;
    private static final long INTERVAL_NS = 1_000_000_000L / TARGET_FPS;
    private static final Logger LOGGER = LogUtils.getLogger();

    private final Readback[] readbacks = {new Readback(), new Readback(), new Readback()};
    private final GpuUiCapture gpu = new GpuUiCapture();
    private Source source;
    private long nextCapture;

    private UiCapture() {}

    static boolean supportsSize(int width, int height) {
        return width > 0 && height > 0 && width <= BridgeWire.MAX_UI_EDGE && height <= BridgeWire.MAX_UI_EDGE
                && (long) width * height <= BridgeWire.MAX_UI_PIXELS;
    }

    void setConnection(LocalTransport.Connection connection) {
        if ((source == null ? null : source.connection) == connection) {
            return;
        }
        gpu.reset();
        if (source != null) {
            source.close();
        }
        source = null;
        nextCapture = 0;
        if (connection != null) {
            try {
                source = new Source(connection);
            } catch (RuntimeException failure) {
                LOGGER.error("Could not allocate Minecraft UE interface buffers", failure);
                connection.invalidate();
            }
        }
    }

    public void capture(RenderTarget target) {
        Source current = source;
        long now = System.nanoTime();
        int width = target.width;
        int height = target.height;
        if (current == null || !supportsSize(width, height) || (!BridgePresentation.isHidden() && now < nextCapture)) {
            return;
        }
        boolean opaque = !WorldBridge.INSTANCE.isSceneBridged();
        if (BridgePresentation.isHidden() && gpu.capture(current.connection, target, opaque, now)) {
            current.lastSlot = -1;
            return;
        }
        for (Readback readback : readbacks) {
            if (!readback.pending) {
                var device = RenderSystem.getDevice();
                int size = width * height * 4;
                if (readback.buffer == null || readback.buffer.size() != size) {
                    if (readback.buffer != null) {
                        readback.buffer.close();
                    }
                    readback.buffer = device.createBuffer(() -> "Minecraft UE bridge GUI readback",
                            GpuBuffer.USAGE_MAP_READ | GpuBuffer.USAGE_COPY_DST, size);
                }
                nextCapture = now + INTERVAL_NS;
                readback.pending = true;
                device.createCommandEncoder().copyTextureToBuffer(target.getColorTexture(), readback.buffer, 0,
                        () -> publish(current, readback, width, height, opaque, now), 0);
                return;
            }
        }
    }

    public void afterFrameSubmit() {
        gpu.afterFrameSubmit();
    }

    public int frameLimit(int userLimit) {
        var stats = source != null ? source.connection.debugStats : null;
        int viewerLimit = stats == null || stats.fpsLimit() == 0 ? Integer.MAX_VALUE : stats.fpsLimit();
        return Math.min(Math.min(gpu.frameLimit(), viewerLimit), userLimit);
    }

    private void publish(Source current, Readback readback, int width, int height, boolean opaque, long captureTime) {
        try {
            if (source != current || gpu.active() || readback.buffer.isClosed()) {
                return;
            }
            try (var mapped = readback.buffer.map(true, false)) {
                ByteBuffer pixels = mapped.data();
                int dimensions = (width << 16) | height;
                if (current.lastSlot >= 0 && current.dimensions == dimensions && current.opaque == opaque
                        && pixels.mismatch(current.pixels(current.lastSlot, pixels.remaining())) < 0) {
                    return;
                }
                int slot = current.connection.reserveUiSlot();
                if (slot < 0) {
                    return;
                }
                current.pixels(slot, pixels.remaining()).put(pixels);
                VarHandle.fullFence();
                current.dimensions = dimensions;
                current.lastSlot = slot;
                current.opaque = opaque;
                int readbackMicros = (int) Math.min(Integer.MAX_VALUE, (System.nanoTime() - captureTime) / 1_000);
                current.connection.sendUi(BridgeWire.frame(BridgeWire.UI, 26)
                        .putLong(current.id.getMostSignificantBits()).putLong(current.id.getLeastSignificantBits())
                        .putInt(dimensions).put((byte) slot).put((byte) (opaque ? 1 : 0)).putInt(readbackMicros).flip());
            }
        } finally {
            readback.pending = false;
        }
    }

    public void close() {
        setConnection(null);
        for (Readback readback : readbacks) {
            if (readback.buffer != null) {
                readback.buffer.close();
            }
        }
    }

    private static final class Readback {
        GpuBuffer buffer;
        boolean pending;
    }

    private static final class Source implements AutoCloseable {
        private static final int MAPPING_BYTES = BridgeWire.UI_SLOTS * BridgeWire.UI_SLOT_BYTES;
        final LocalTransport.Connection connection;
        final UUID id = UUID.randomUUID();
        final WinNT.HANDLE mapping;
        final Pointer address;
        int dimensions;
        int lastSlot = -1;
        boolean opaque;

        Source(LocalTransport.Connection connection) {
            this.connection = connection;
            Kernel32 kernel = Kernel32.INSTANCE;
            String name = "{" + id.toString().toUpperCase(Locale.ROOT) + "}";
            mapping = kernel.CreateFileMapping(WinBase.INVALID_HANDLE_VALUE, null, WinNT.PAGE_READWRITE, 0, MAPPING_BYTES, name);
            if (mapping == null) {
                throw new IllegalStateException("CreateFileMapping failed: " + kernel.GetLastError());
            }
            address = kernel.MapViewOfFile(mapping, WinNT.FILE_MAP_WRITE, 0, 0, MAPPING_BYTES);
            if (address == null) {
                int error = kernel.GetLastError();
                kernel.CloseHandle(mapping);
                throw new IllegalStateException("MapViewOfFile failed: " + error);
            }
        }

        ByteBuffer pixels(int slot, int bytes) {
            return address.getByteBuffer((long) slot * BridgeWire.UI_SLOT_BYTES, bytes);
        }

        @Override
        public void close() {
            Kernel32.INSTANCE.UnmapViewOfFile(address);
            Kernel32.INSTANCE.CloseHandle(mapping);
        }
    }
}
