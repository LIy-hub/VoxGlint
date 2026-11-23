package dev.minecraftue.bridge;

import java.util.ArrayList;
import java.util.List;

/** Transfers ordered input events alongside coalesced relative mouse movement. */
final class InputMailbox {
    private static final long TIMEOUT_NS = 400_000_000L;
    private static final int MAX_PENDING_EVENTS = 512;

    private List<InputEvent> pendingEvents = new ArrayList<>();
    private int scene;
    private int capture;
    private double dx;
    private double dy;
    private int lookSerial;
    private long receivedAt;

    synchronized void changeScene(int nextScene) {
        scene = nextScene;
        capture = 0;
        receivedAt = 0;
        clearEvents();
    }

    synchronized boolean update(int inputScene, int nextCapture, float mouseX, float mouseY,
                                int nextLookSerial, List<InputEvent> events) {
        if (inputScene != scene) {
            return true;
        }
        if (capture != nextCapture) {
            clearEvents();
        }
        if (pendingEvents.size() + events.size() > MAX_PENDING_EVENTS) {
            return false;
        }
        capture = nextCapture;
        receivedAt = System.nanoTime();
        pendingEvents.addAll(events);
        dx = Math.clamp(dx + mouseX, -BridgeWire.MAX_MOUSE_DELTA, BridgeWire.MAX_MOUSE_DELTA);
        dy = Math.clamp(dy + mouseY, -BridgeWire.MAX_MOUSE_DELTA, BridgeWire.MAX_MOUSE_DELTA);
        if (nextLookSerial != 0) {
            lookSerial = nextLookSerial;
        }
        return true;
    }

    synchronized Sample drain() {
        if (capture == 0) {
            return new Sample(scene, 0, false, 0, 0, 0, List.of());
        }
        if (System.nanoTime() - receivedAt > TIMEOUT_NS) {
            clearEvents();
            return new Sample(scene, capture, false, 0, 0, 0, List.of());
        }
        List<InputEvent> pending = pendingEvents.isEmpty() ? List.of() : pendingEvents;
        if (!pending.isEmpty()) {
            pendingEvents = new ArrayList<>();
        }
        Sample sample = new Sample(scene, capture, true, dx, dy, lookSerial, pending);
        clearEvents();
        return sample;
    }

    private void clearEvents() {
        pendingEvents.clear();
        lookSerial = 0;
        dx = dy = 0;
    }

    record InputEvent(int action, int value, int flags) {}

    record Sample(int scene, int capture, boolean live,
                  double mouseX, double mouseY, int lookSerial, List<InputEvent> events) {}
}
