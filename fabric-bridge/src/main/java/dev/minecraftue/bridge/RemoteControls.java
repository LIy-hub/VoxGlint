package dev.minecraftue.bridge;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import com.mojang.logging.LogUtils;
import net.minecraft.client.KeyMapping;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.EditBox;
import net.minecraft.client.gui.components.MultiLineEditBox;
import net.minecraft.client.gui.components.events.ContainerEventHandler;
import net.minecraft.client.gui.components.events.GuiEventListener;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.inventory.AbstractSignEditScreen;
import net.minecraft.client.input.CharacterEvent;
import net.minecraft.client.input.KeyEvent;
import org.slf4j.Logger;

import java.util.Arrays;
import java.util.BitSet;

import static org.lwjgl.glfw.GLFW.*;

/** Delivers UE input through Minecraft's keyboard, bindings and mouse look on the client thread. */
public final class RemoteControls {
    public static final RemoteControls INSTANCE = new RemoteControls();
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final int GAMEPLAY = 1;
    private static final int SCREEN = 2;
    private static final int TEXT = 3;
    private static final int KEYBOARD = 4;

    private final BitSet heldKeys = new BitSet(GLFW_KEY_LAST + 1);
    private LocalTransport.Connection peer;
    private Window captureWindow;
    private int[] scanToKey;
    private int scene;
    private int capture;
    private int rejectedCapture;
    private int gameplayButtons;
    private Screen pointerScreen;
    private int pointerButtons;
    private boolean gameplayActive;
    private double dx;
    private double dy;
    private int lookSerial;

    private RemoteControls() {}

    static int inputMode(Minecraft minecraft) {
        if (minecraft.gui.overlay() != null) {
            // Minecraft handles global shortcuts during an overlay and filters the other keys itself.
            return KEYBOARD;
        }
        Screen screen = minecraft.gui.screen();
        if (screen != null) {
            GuiEventListener focused = screen.getFocused();
            while (focused instanceof ContainerEventHandler container) {
                focused = container.getFocused();
            }
            return focused instanceof EditBox || focused instanceof MultiLineEditBox
                    || screen instanceof AbstractSignEditScreen
                    || screen.children().stream().anyMatch(widget -> widget instanceof EditBox field && field.isFocused())
                    ? TEXT : SCREEN;
        }
        return gameplayScreen(minecraft) ? GAMEPLAY : minecraft.player != null ? KEYBOARD : 0;
    }

    static boolean gameplayScreen(Minecraft minecraft) {
        return minecraft.player != null && minecraft.player.isAlive() && !minecraft.isPaused()
                && minecraft.gui.screen() == null && minecraft.gui.overlay() == null;
    }

    public void beforeTick(Minecraft minecraft) {
        refresh(minecraft);
        if (gameplayActive) {
            boolean appliedShm = WorldBridge.INSTANCE.applyZeroLatencyLook(minecraft, capture, peer);
            if (!appliedShm && lookSerial != 0) {
                turn(minecraft);
            } else if (appliedShm) {
                dx = dy = 0;
                lookSerial = 0;
            }
        }
    }

    public void beforeRenderMouse(Minecraft minecraft) {
        refresh(minecraft);
        if (gameplayActive) {
            boolean appliedShm = WorldBridge.INSTANCE.applyZeroLatencyLook(minecraft, capture, peer);
            if (appliedShm) {
                dx = dy = 0;
                lookSerial = 0;
            } else {
                turn(minecraft);
            }
        }
    }

    private void refresh(Minecraft minecraft) {
        LocalTransport.Connection connection = WorldBridge.INSTANCE.inputConnection();
        if (connection != peer) {
            stop(minecraft);
            rejectedCapture = 0;
            peer = connection;
        }
        if (peer == null) {
            return;
        }
        WorldBridge.INSTANCE.deliverFileDrops(minecraft, peer);

        InputMailbox.Sample sample = peer.input.drain();
        if (scene != sample.scene()) {
            stop(minecraft);
            rejectedCapture = 0;
            scene = sample.scene();
        }
        if (sample.capture() == 0) {
            rejectedCapture = 0;
            stop(minecraft);
            return;
        }
        if (!sample.live()) {
            stop(minecraft);
            return;
        }
        if (hasLocalFocus(minecraft)) {
            revokeCapture(minecraft);
            return;
        }
        if (sample.capture() == rejectedCapture) {
            stop(minecraft);
            return;
        }
        if (capture != sample.capture()) {
            stop(minecraft);
            KeyMapping.releaseAll();
            captureWindow = minecraft.getWindow();
            capture = sample.capture();
            WorldBridge.INSTANCE.syncControlState(minecraft);
            LOGGER.info("Minecraft UE Bridge input captured ({})", Integer.toUnsignedString(capture));
        }

        for (InputMailbox.InputEvent event : sample.events()) {
            syncPointerScreen(minecraft);
            if (event.action() <= BridgeWire.CHARACTER) {
                dispatchKey(minecraft, event);
            } else {
                dispatchPointer(minecraft, event);
            }
            if (capture == 0) {
                return;
            }
        }
        syncPointerScreen(minecraft);
        boolean gameplay = gameplayScreen(minecraft);
        if (!gameplay && gameplayActive) {
            ((RemoteMouse) minecraft.mouseHandler).minecraftUe$resetSmoothing();
        } else if (gameplay && !gameplayActive) {
            WorldBridge.INSTANCE.syncControlState(minecraft);
        }
        gameplayActive = gameplay;
        if (!gameplay) {
            clearGameplayButtons(minecraft);
        }
        if (gameplay) {
            dx += sample.mouseX();
            dy += sample.mouseY();
            if (sample.lookSerial() != 0) {
                lookSerial = sample.lookSerial();
            }
            if (gameplayButtons != 0 || dx != 0 || dy != 0 || !heldKeys.isEmpty()) {
                minecraft.getFramerateLimitTracker().onInputReceived();
            }
        } else {
            dx = dy = 0;
            lookSerial = 0;
        }
    }

    private void dispatchPointer(Minecraft minecraft, InputMailbox.InputEvent event) {
        RemoteMouse mouse = (RemoteMouse) minecraft.mouseHandler;
        int value = event.value();
        long window = captureWindow.handle();
        switch (event.action()) {
            case BridgeWire.POINTER_MOVE -> {
                if (pointerScreen != null) {
                    mouse.minecraftUe$moveInScreen((short) (value >>> 16) / (double) BridgeWire.POINTER_SCALE,
                            (short) value / (double) BridgeWire.POINTER_SCALE);
                }
            }
            case BridgeWire.POINTER_DOWN, BridgeWire.POINTER_UP -> {
                if (pointerScreen == null) {
                    return;
                }
                int bit = 1 << value;
                boolean down = event.action() == BridgeWire.POINTER_DOWN;
                if (((pointerButtons & bit) != 0) != down) {
                    pointerButtons ^= bit;
                    mouse.minecraftUe$button(window, value, event.flags(), down);
                }
            }
            case BridgeWire.GAMEPLAY_DOWN, BridgeWire.GAMEPLAY_UP -> {
                if (!gameplayScreen(minecraft)) {
                    return;
                }
                int bit = 1 << value;
                boolean down = event.action() == BridgeWire.GAMEPLAY_DOWN;
                if (((gameplayButtons & bit) != 0) != down) {
                    gameplayButtons ^= bit;
                    mouse.minecraftUe$button(window, value, event.flags(), down);
                }
            }
            case BridgeWire.SCROLL -> {
                boolean screen = (event.flags() & BridgeWire.SCROLL_SCREEN) != 0;
                if (screen ? pointerScreen != null : gameplayScreen(minecraft)) {
                    float amount = Float.intBitsToFloat(value);
                    boolean horizontal = (event.flags() & BridgeWire.SCROLL_HORIZONTAL) != 0;
                    mouse.minecraftUe$scroll(window, horizontal ? amount : 0, horizontal ? 0 : amount);
                }
            }
            default -> { }
        }
    }

    private void syncPointerScreen(Minecraft minecraft) {
        Screen screen = minecraft.gui.overlay() == null ? minecraft.gui.screen() : null;
        if (screen != pointerScreen) {
            clearPointer(minecraft);
            pointerScreen = screen;
            if (screen != null) {
                clearGameplayButtons(minecraft);
            }
        }
    }

    private void clearPointer(Minecraft minecraft) {
        if (pointerButtons != 0) {
            ((RemoteMouse) minecraft.mouseHandler).minecraftUe$cancelScreenDrag(pointerScreen);
        }
        pointerButtons = 0;
        pointerScreen = null;
    }

    public boolean ownsPointer(Minecraft minecraft) {
        return capture != 0 && !hasLocalFocus(minecraft);
    }

    private void dispatchKey(Minecraft minecraft, InputMailbox.InputEvent event) {
        RemoteKeyboard keyboard = (RemoteKeyboard) minecraft.keyboardHandler;
        long window = captureWindow.handle();
        if (event.action() == BridgeWire.CHARACTER) {
            keyboard.minecraftUe$character(window, new CharacterEvent(event.value()));
            return;
        }
        if (scanToKey == null) {
            scanToKey = new int[BridgeWire.SCAN_CODES];
            Arrays.fill(scanToKey, GLFW_KEY_UNKNOWN);
            for (int key = GLFW_KEY_SPACE; key <= GLFW_KEY_LAST; key++) {
                int scan = glfwGetKeyScancode(key);
                if (scan >= 0 && scan < scanToKey.length) {
                    scanToKey[scan] = key;
                }
            }
        }
        int scan = event.value();
        int key = scanToKey[scan];
        if (key != GLFW_KEY_UNKNOWN) {
            heldKeys.set(key, event.action() != GLFW_RELEASE);
        }
        keyboard.minecraftUe$key(window, event.action(), new KeyEvent(key, scan, event.flags()));
    }

    private void clearGameplayButtons(Minecraft minecraft) {
        if (gameplayButtons == 0 && !gameplayActive) {
            return;
        }
        for (int button = 0; button < BridgeWire.MOUSE_BUTTONS; button++) {
            if ((gameplayButtons & (1 << button)) != 0) {
                KeyMapping.set(InputConstants.Type.MOUSE.getOrCreate(button), false);
            }
        }
        gameplayButtons = 0;
        ((RemoteMouse) minecraft.mouseHandler).minecraftUe$clearGameplayButtons();
    }

    private void turn(Minecraft minecraft) {
        ((RemoteMouse) minecraft.mouseHandler).minecraftUe$turn(dx, dy);
        if (lookSerial != 0) {
            peer.recordAppliedLook(capture, lookSerial);
            lookSerial = 0;
        }
        dx = dy = 0;
    }

    public void returnToMinecraft(Minecraft minecraft) {
        if (capture != 0 && hasLocalFocus(minecraft)) {
            revokeCapture(minecraft);
        }
    }

    private static boolean hasLocalFocus(Minecraft minecraft) {
        // Minecraft's cached flag starts true even if its window was opened in the background.
        return !BridgePresentation.isHidden()
                && glfwGetWindowAttrib(minecraft.getWindow().handle(), GLFW_FOCUSED) == GLFW_TRUE;
    }

    private void revokeCapture(Minecraft minecraft) {
        if (capture != 0) {
            rejectedCapture = capture;
            stop(minecraft);
        }
    }

    private void stop(Minecraft minecraft) {
        if (capture == 0) {
            return;
        }
        LOGGER.info("Minecraft UE Bridge input released ({})", Integer.toUnsignedString(capture));
        clearPointer(minecraft);
        clearGameplayButtons(minecraft);
        capture = 0;
        captureWindow = null;
        heldKeys.clear();
        gameplayActive = false;
        dx = dy = 0;
        lookSerial = 0;
        KeyMapping.releaseAll();
        ((RemoteKeyboard) minecraft.keyboardHandler).minecraftUe$resetDebugKeys();
        ((RemoteMouse) minecraft.mouseHandler).minecraftUe$resetSmoothing();
    }

    public boolean isGameplayActive() {
        return gameplayActive;
    }

    public Boolean keyDown(Window window, int key) {
        if (window == captureWindow) {
            return key >= 0 && key <= GLFW_KEY_LAST && heldKeys.get(key);
        }
        return BridgePresentation.isHidden() ? Boolean.FALSE : null;
    }
}
