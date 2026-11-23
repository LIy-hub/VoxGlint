package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import org.slf4j.Logger;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.StandardCharsets;
import java.nio.file.InvalidPathException;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicReferenceArray;
import java.util.concurrent.locks.LockSupport;

final class LocalTransport {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final InetSocketAddress ENDPOINT = new InetSocketAddress("127.0.0.1", BridgeWire.PORT);

    private final Thread worker;
    private volatile boolean running = true;
    private volatile Connection current;
    private final AtomicBoolean closeRequested = new AtomicBoolean();

    LocalTransport() {
        worker = Thread.ofPlatform().daemon().name("Minecraft UE Bridge").unstarted(this::run);
        worker.start();
    }

    boolean takeCloseRequest() { return closeRequested.getAndSet(false); }

    Connection current() {
        Connection connection = current;
        return connection != null && connection.valid ? connection : null;
    }

    void close() {
        running = false;
        RendererProcess.INSTANCE.close();
        Connection active = current;
        if (active != null) {
            active.selector.wakeup();
        }
        LockSupport.unpark(worker);
    }

    private void run() {
        RendererProcess renderer = RendererProcess.INSTANCE;
        boolean managed = renderer.enabled();
        InetSocketAddress endpoint = ENDPOINT;
        if (managed) {
            int port = renderer.start();
            if (port == 0) {
                return;
            }
            endpoint = new InetSocketAddress("127.0.0.1", port);
        }
        boolean reportedHandshakeFailure = false;
        while (running && (!managed || renderer.isRunning())) {
            try (SocketChannel socket = SocketChannel.open(); Selector selector = Selector.open()) {
                startConnect(socket, endpoint);
                long deadline = System.nanoTime() + 500_000_000L;
                while (running && !socket.finishConnect() && System.nanoTime() < deadline) {
                    LockSupport.parkNanos(2_000_000L);
                }
                if (running && socket.isConnected()) {
                    long nonce = ThreadLocalRandom.current().nextLong();
                    try (ControlSharedMemory control = new ControlSharedMemory(nonce);
                         TerrainSharedMemory terrain = new TerrainSharedMemory(nonce)) {
                        Connection connection = new Connection(selector, nonce, control, terrain);
                        try {
                            pump(connection, socket.register(selector, SelectionKey.OP_READ), endpoint);
                        } finally {
                            if (!connection.ready && running && !reportedHandshakeFailure) {
                                LOGGER.warn("Minecraft UE Bridge handshake at {} ended before protocol {} and shared mappings were confirmed",
                                        endpoint, BridgeWire.VERSION);
                                reportedHandshakeFailure = true;
                            }
                            connection.valid = false;
                            current = null;
                        }
                    }
                }
            } catch (ProtocolMismatch e) {
                LOGGER.error("Minecraft UE Bridge {}", e.getMessage());
                return;
            } catch (IOException e) {
                if (running) {
                    LOGGER.debug("Minecraft UE Bridge connection closed", e);
                }
            } catch (RuntimeException e) {
                if (running) {
                    LOGGER.error("Minecraft UE Bridge could not initialize the session shared memory", e);
                    if (managed) {
                        renderer.close();
                    }
                }
                return;
            }
            if (running) {
                LockSupport.parkNanos(500_000_000L);
            }
        }
    }

    private static final class ProtocolMismatch extends IOException {
        private ProtocolMismatch(int rendererVersion) {
            super("renderer protocol " + rendererVersion + " differs from Mod protocol " + BridgeWire.VERSION);
        }
    }

    private static void startConnect(SocketChannel socket, InetSocketAddress endpoint) throws IOException {
        socket.configureBlocking(false);
        socket.setOption(StandardSocketOptions.TCP_NODELAY, true);
        socket.connect(endpoint);
    }

    private void pump(Connection connection, SelectionKey streamKey, InetSocketAddress endpoint) throws IOException {
        SocketChannel socket = (SocketChannel) streamKey.channel();
        ByteBuffer outgoing = ByteBuffer.allocate(5 + 16 + 5 + BridgeWire.CONTROL_SHM_BYTES);
        var controlId = connection.controlShm.id();
        var terrainId = connection.terrainShm.id();
        outgoing.put(BridgeWire.reset(connection.nonce))
                .putInt(1 + BridgeWire.CONTROL_SHM_BYTES).put((byte) BridgeWire.CONTROL_SHM)
                .putLong(controlId.getMostSignificantBits()).putLong(controlId.getLeastSignificantBits())
                .putLong(terrainId.getMostSignificantBits()).putLong(terrainId.getLeastSignificantBits()).flip();
        ByteBuffer incoming = ByteBuffer.allocate(Integer.BYTES + BridgeWire.MAX_FILE_DROP_SIZE);
        long streamProgress = System.nanoTime();
        long handshakeDeadline = streamProgress + 5_000_000_000L;
        int streamTurn = 0;
        int dynamicSlot = -1;
        int sendingTerrainBytes = 0;
        FrameTransfer sendingTerrain = null;
        while (running && connection.valid && readInput(socket, incoming, connection)) {
            if (current == null && connection.ready) {
                current = connection;
                LOGGER.info("Minecraft UE Bridge connected to {} (protocol {}, one TCP channel, both shared mappings ready)",
                        endpoint, BridgeWire.VERSION);
            }
            long now = System.nanoTime();
            if (outgoing == null && connection.ready) {
                outgoing = connection.control.poll();
                if (outgoing == null) {
                    outgoing = connection.environment.getAndSet(null);
                }
                // Round robin over the bounded streams at message boundaries. Control retains priority.
                int streamCount = 2 + BridgeWire.DYNAMIC_TRANSFER_COUNT;
                for (int attempt = 0; outgoing == null && attempt < streamCount; attempt++) {
                    int turn = streamTurn;
                    streamTurn = (streamTurn + 1) % streamCount;
                    if (turn == 0) {
                        outgoing = connection.ui.poll();
                    } else if (turn == 1) {
                        if (sendingTerrain == null) {
                            sendingTerrain = connection.terrain.poll();
                        }
                        if (sendingTerrain != null) {
                            outgoing = sendingTerrain.nextFrame();
                            sendingTerrainBytes = outgoing.remaining();
                        }
                    } else {
                        FrameTransfer transfer = connection.dynamic.get(turn - 2);
                        if (transfer != null) {
                            outgoing = transfer.nextFrame();
                            dynamicSlot = turn - 2;
                        }
                    }
                }
                streamProgress = now;
            }
            boolean streamWrote = outgoing != null && socket.write(outgoing) > 0;
            if (streamWrote) {
                streamProgress = now;
                if (!outgoing.hasRemaining()) {
                    outgoing = null;
                    if (sendingTerrainBytes > 0) {
                        connection.terrainBytes.addAndGet(-sendingTerrainBytes);
                        if (!sendingTerrain.hasMore()) {
                            connection.terrainSent++;
                            sendingTerrain = null;
                        }
                        sendingTerrainBytes = 0;
                    }
                    if (dynamicSlot >= 0) {
                        FrameTransfer transfer = connection.dynamic.get(dynamicSlot);
                        if (!transfer.hasMore()) {
                            connection.dynamic.set(dynamicSlot, null);
                        }
                        dynamicSlot = -1;
                    }
                }
            }
            if (outgoing != null && now - streamProgress > (connection.ready ? 15_000_000_000L : 3_000_000_000L)) {
                return;
            }
            if (!connection.ready && now >= handshakeDeadline) {
                throw new IOException("Renderer did not acknowledge protocol " + BridgeWire.VERSION + " shared mappings");
            }
            if (!streamWrote) {
                streamKey.interestOps(outgoing != null ? SelectionKey.OP_READ | SelectionKey.OP_WRITE : SelectionKey.OP_READ);
                // Enqueue operations wake the selector. INPUT wakes it directly through socket readability.
                connection.selector.select(250);
                connection.selector.selectedKeys().clear();
            }
        }
        if (!running && (outgoing == null || outgoing.position() == 0)) {
            socket.write(BridgeWire.frame(BridgeWire.CLOSE, 0).flip());
        }
    }

    private boolean readInput(SocketChannel socket, ByteBuffer incoming, Connection connection) throws IOException {
        int read = socket.read(incoming);
        if (read < 0) {
            return false;
        }
        if (read == 0) {
            return true;
        }
        incoming.flip();
        while (incoming.remaining() >= Integer.BYTES) {
            incoming.mark();
            int size = incoming.getInt();
            if (size < 1 || size > BridgeWire.MAX_FILE_DROP_SIZE) {
                return false;
            }
            if (incoming.remaining() < size) {
                incoming.reset();
                break;
            }
            int type = Byte.toUnsignedInt(incoming.get());
            if (!connection.ready) {
                if (type != BridgeWire.RESET || size != 17 || incoming.getInt() != BridgeWire.MAGIC) {
                    return false;
                }
                int version = incoming.getInt();
                if (version != BridgeWire.VERSION) {
                    throw new ProtocolMismatch(version);
                }
                if (incoming.getLong() != connection.nonce) {
                    return false;
                }
                connection.ready = true;
                continue;
            }
            if (type == BridgeWire.CLOSE && size == 1) {
                closeRequested.set(true);
                return false;
            }
            if (type == BridgeWire.FILE_DROP) {
                int payload = size - 1;
                if (!readFileDrop(incoming.slice(incoming.position(), payload), connection)) {
                    return false;
                }
                incoming.position(incoming.position() + payload);
                continue;
            }
            if (type != BridgeWire.INPUT || size < BridgeWire.INPUT_BASE_SIZE || size > BridgeWire.MAX_INPUT_SIZE) {
                return false;
            }
            int capture = incoming.getInt();
            float mouseX = incoming.getFloat();
            float mouseY = incoming.getFloat();
            int viewport = incoming.getInt();
            int readyScene = incoming.getInt();
            int releasedSlots = Byte.toUnsignedInt(incoming.get());
            int eventCount = Byte.toUnsignedInt(incoming.get());
            int debugScene = incoming.getInt();
            float frameMs = incoming.getFloat();
            int loadedSections = incoming.getInt();
            int pendingSections = incoming.getInt();
            int receivedColumns = Short.toUnsignedInt(incoming.getShort());
            int viewDistance = Byte.toUnsignedInt(incoming.get());
            int fpsLimit = Short.toUnsignedInt(incoming.getShort());
            int vsync = Byte.toUnsignedInt(incoming.get());
            int debugPanelBottom = Short.toUnsignedInt(incoming.getShort());
            int lookSerial = incoming.getInt();
            long presentingWindow = incoming.getLong();
            int atlasScene = incoming.getInt();
            int atlasStatus = Byte.toUnsignedInt(incoming.get());
            int releaseCount = Byte.toUnsignedInt(incoming.get());
            if (atlasStatus > BridgeWire.ATLAS_CACHE_PREPARING || eventCount > BridgeWire.MAX_INPUT_EVENTS
                    || releaseCount > BridgeWire.MAX_TERRAIN_RELEASES
                    || size != BridgeWire.INPUT_BASE_SIZE + eventCount * BridgeWire.INPUT_EVENT_SIZE + releaseCount * Long.BYTES
                    || (releasedSlots >>> BridgeWire.UI_SLOTS) != 0
                    || (viewport != 0 && !UiCapture.supportsSize(viewport >>> 16, viewport & 0xffff))
                    || !(Math.abs(mouseX) <= BridgeWire.MAX_MOUSE_DELTA && Math.abs(mouseY) <= BridgeWire.MAX_MOUSE_DELTA)
                    || ((lookSerial == 0) != (mouseX == 0 && mouseY == 0))
                    || (capture == 0 && (mouseX != 0 || mouseY != 0 || eventCount != 0))) {
                return false;
            }
            List<InputMailbox.InputEvent> events = eventCount == 0 ? List.of() : new ArrayList<>(eventCount);
            for (int i = 0; i < eventCount; i++) {
                int action = Byte.toUnsignedInt(incoming.get());
                int value = incoming.getInt();
                int flags = Byte.toUnsignedInt(incoming.get());
                if (!validInputEvent(action, value, flags)) {
                    return false;
                }
                events.add(new InputMailbox.InputEvent(action, value, flags));
            }
            for (int i = 0; i < releaseCount; i++) {
                if (!connection.terrainShm.release(incoming.getLong())) {
                    return false;
                }
            }
            connection.viewport = viewport;
            connection.presentingWindow = presentingWindow;
            connection.readyScene = readyScene;
            connection.atlasReply = (Integer.toUnsignedLong(atlasScene) << 8) | atlasStatus;
            connection.availableUiSlots.getAndAccumulate(releasedSlots, (current, released) -> current | released);
            if (!connection.input.update(debugScene, capture, mouseX, mouseY, lookSerial, events)) {
                return false;
            }
            if (Float.isFinite(frameMs) && frameMs >= 0 && loadedSections >= 0 && pendingSections >= 0
                    && viewDistance <= BridgeWire.MAX_VIEW_DISTANCE && vsync <= 1) {
                connection.debugStats = new WorldBridge.DebugStats(debugScene, frameMs, loadedSections,
                        pendingSections, receivedColumns, viewDistance, fpsLimit, vsync == 1,
                        debugPanelBottom, System.nanoTime());
            }
        }
        incoming.compact();
        return true;
    }

    private static boolean readFileDrop(ByteBuffer frame, Connection connection) {
        if (frame.remaining() < Integer.BYTES + 1) {
            return false;
        }
        int scene = frame.getInt();
        int count = Byte.toUnsignedInt(frame.get());
        if (count == 0 || count > BridgeWire.MAX_DROPPED_FILES) {
            return false;
        }
        List<Path> paths = new ArrayList<>(count);
        var decoder = StandardCharsets.UTF_8.newDecoder();
        for (int i = 0; i < count; i++) {
            if (frame.remaining() < Short.BYTES) {
                return false;
            }
            int length = Short.toUnsignedInt(frame.getShort());
            if (length == 0 || length > BridgeWire.MAX_FILE_PATH_BYTES || frame.remaining() < length) {
                return false;
            }
            try {
                Path path = Path.of(decoder.decode(frame.slice(frame.position(), length)).toString());
                if (!path.isAbsolute()) {
                    return false;
                }
                paths.add(path);
                frame.position(frame.position() + length);
            } catch (CharacterCodingException | InvalidPathException invalid) {
                return false;
            }
        }
        return !frame.hasRemaining() && connection.fileDrops.offer(new DroppedFiles(scene, List.copyOf(paths)));
    }

    record DroppedFiles(int scene, List<Path> paths) {}

    private static final class FrameTransfer {
        private final List<ByteBuffer> frames;
        private int next; // Owned by the socket thread once published.

        private FrameTransfer(List<ByteBuffer> frames) {
            this.frames = List.copyOf(frames);
        }

        private ByteBuffer nextFrame() {
            return frames.get(next++);
        }

        private boolean hasMore() {
            return next < frames.size();
        }
    }

    private static boolean validInputEvent(int action, int value, int flags) {
        return switch (action) {
            case 0, 1, 2 -> flags <= 0x3F && value >= 0 && value < BridgeWire.SCAN_CODES;
            case BridgeWire.CHARACTER -> flags == 0 && Character.isValidCodePoint(value)
                    && !Character.isISOControl(value) && !(value >= 0xD800 && value <= 0xDFFF);
            case BridgeWire.POINTER_MOVE -> flags == 0;
            case BridgeWire.POINTER_DOWN, BridgeWire.POINTER_UP, BridgeWire.GAMEPLAY_DOWN, BridgeWire.GAMEPLAY_UP ->
                    flags <= 0x3F && value >= 0 && value < BridgeWire.MOUSE_BUTTONS;
            case BridgeWire.SCROLL -> flags <= (BridgeWire.SCROLL_HORIZONTAL | BridgeWire.SCROLL_SCREEN)
                    && Math.abs(Float.intBitsToFloat(value)) <= BridgeWire.MAX_WHEEL_DELTA;
            default -> false;
        };
    }

    static final class Connection {
        private final Selector selector;
        private final long nonce;
        final ControlSharedMemory controlShm;
        private final TerrainSharedMemory terrainShm;
        private boolean ready; // Published to the client only after renderer acknowledgement.
        private final ArrayBlockingQueue<ByteBuffer> control = new ArrayBlockingQueue<>(64);
        private static final int TERRAIN_QUEUE_BUDGET_BYTES = 8 * 1024 * 1024;
        private final AtomicLong terrainBytes = new AtomicLong();
        private final ArrayBlockingQueue<FrameTransfer> terrain = new ArrayBlockingQueue<>(256);
        private final AtomicReferenceArray<FrameTransfer> dynamic =
                new AtomicReferenceArray<>(BridgeWire.DYNAMIC_TRANSFER_COUNT);
        private final AtomicReference<ByteBuffer> environment = new AtomicReference<>();
        private final ArrayBlockingQueue<ByteBuffer> ui = new ArrayBlockingQueue<>(BridgeWire.UI_SLOTS);
        private final AtomicInteger availableUiSlots = new AtomicInteger((1 << BridgeWire.UI_SLOTS) - 1);
        final InputMailbox input = new InputMailbox();
        final ArrayBlockingQueue<DroppedFiles> fileDrops = new ArrayBlockingQueue<>(4);
        private int appliedLookSerial;
        private int appliedLookCapture;
        private long terrainQueued; // Published by the client thread.
        private volatile long terrainSent; // Advanced after a complete socket write.
        private volatile boolean valid = true;
        volatile int viewport;
        volatile long presentingWindow;
        volatile int readyScene;
        volatile long atlasReply;
        volatile WorldBridge.DebugStats debugStats;

        private Connection(Selector selector, long nonce, ControlSharedMemory controlShm, TerrainSharedMemory terrainShm) {
            this.selector = selector;
            this.nonce = nonce;
            this.controlShm = controlShm;
            this.terrainShm = terrainShm;
        }

        boolean sendControl(ByteBuffer frame) {
            if (!valid || !control.offer(frame)) {
                valid = false;
            }
            selector.wakeup();
            return valid;
        }

        boolean sendTerrain(ByteBuffer frame) {
            return sendTerrain(List.of(frame));
        }

        private boolean sendTerrain(List<ByteBuffer> frames) {
            int bytes = frames.stream().mapToInt(ByteBuffer::remaining).sum();
            terrainBytes.addAndGet(bytes);
            if (!valid || !terrain.offer(new FrameTransfer(frames))) {
                terrainBytes.addAndGet(-bytes);
                // Reconnect and send fresh snapshots rather than losing a terrain delta.
                valid = false;
            } else {
                terrainQueued++;
            }
            selector.wakeup();
            return valid;
        }

        long sendSection(int scene, boolean urgent, ByteBuffer mesh, ByteBuffer lights, List<ByteBuffer> glass) {
            if (!canQueueSection(urgent)) {
                return 0;
            }
            ByteBuffer descriptor = terrainShm.publish(scene, urgent, mesh, lights);
            if (descriptor == null) {
                return 0;
            }
            long token = descriptor.getLong(9);
            if (glass.isEmpty()) {
                return sendTerrain(descriptor) ? token : 0;
            }
            // One FIFO item keeps all glass parts and the matching terrain descriptor together.
            var frames = new ArrayList<ByteBuffer>(glass.size() + 1);
            for (var part : glass) {
                frames.add(part.duplicate());
            }
            frames.add(descriptor);
            return sendTerrain(frames) ? token : 0;
        }

        boolean canQueueSection(boolean urgent) {
            return urgent ? valid && terrain.size() < 32 : canQueueTerrain();
        }

        boolean canQueueTerrain() {
            return valid && terrain.size() < 24 && terrainBytes.get() < TERRAIN_QUEUE_BUDGET_BYTES;
        }

        boolean terrainDrained() {
            return valid && terrainSent == terrainQueued;
        }

        boolean canQueueDynamic(int slot) {
            return valid && dynamic.get(slot) == null;
        }

        boolean sendDynamic(int slot, List<ByteBuffer> frames) {
            if (!valid || !dynamic.compareAndSet(slot, null, new FrameTransfer(frames))) {
                return false;
            }
            selector.wakeup();
            return true;
        }

        void invalidate() {
            valid = false;
            selector.wakeup();
        }

        void sendEnvironment(ByteBuffer frame) {
            environment.set(frame);
            selector.wakeup();
        }

        void changeInputScene(int scene) {
            input.changeScene(scene);
            appliedLookSerial = 0;
            appliedLookCapture = 0;
            environment.set(null);
        }

        int appliedLookCapture() { return appliedLookCapture; }

        int appliedLookSerial() { return appliedLookSerial; }

        void recordAppliedLook(int capture, int serial) {
            appliedLookSerial = serial;
            appliedLookCapture = capture;
        }

        int reserveUiSlot() {
            int old = availableUiSlots.getAndUpdate(slots -> slots & (slots - 1));
            return old == 0 ? -1 : Integer.numberOfTrailingZeros(old);
        }

        void sendUi(ByteBuffer frame) {
            if (!ui.offer(frame)) {
                invalidate();
            }
            selector.wakeup();
        }
    }
}
