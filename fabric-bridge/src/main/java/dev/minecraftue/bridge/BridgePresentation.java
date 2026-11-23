package dev.minecraftue.bridge;

import com.mojang.blaze3d.platform.Window;
import com.mojang.logging.LogUtils;
import com.sun.jna.Native;
import com.sun.jna.Pointer;
import com.sun.jna.platform.win32.User32;
import com.sun.jna.platform.win32.WinDef.HWND;
import com.sun.jna.win32.StdCallLibrary;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.screens.TitleScreen;
import org.slf4j.Logger;

import static org.lwjgl.glfw.GLFW.*;
import static org.lwjgl.glfw.GLFWNativeWin32.glfwGetWin32Window;

public final class BridgePresentation {
    public static final boolean REQUESTED = Boolean.parseBoolean(System.getProperty("minecraftue.singleWindow",
            Boolean.toString(RendererProcess.INSTANCE.enabled())));
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final long RECONNECT_TIMEOUT = 20_000_000_000L;
    private static final long RESIZE_SETTLE_TIME = 100_000_000L;
    private static long disconnectedAt;
    private static long resizeAt;
    private static int appliedSize;
    private static int pendingSize;
    private static boolean hidden;
    private static boolean startupReady;

    private BridgePresentation() {}

    private interface WindowActivation extends StdCallLibrary {
        WindowActivation INSTANCE = Native.load("user32", WindowActivation.class);

        boolean EnableWindow(HWND window, boolean enable);
    }

    public static void showInitially(long window) {
        glfwShowWindow(window);
        WorldBridge.INSTANCE.start();
    }

    static boolean canCapture(Minecraft minecraft) {
        if (REQUESTED && !startupReady) {
            startupReady = (minecraft.gui.screen() instanceof TitleScreen && minecraft.gui.overlay() == null)
                    || RemoteControls.gameplayScreen(minecraft);
        }
        return !REQUESTED || startupReady;
    }

    static void update(Minecraft minecraft, LocalTransport.Connection connection) {
        if (!REQUESTED) {
            return;
        }
        long now = System.nanoTime();
        Window window = minecraft.getWindow();
        if (!hidden) {
            long viewerHandle = connection == null ? 0 : connection.presentingWindow;
            if (startupReady && viewerHandle != 0) {
                User32 windows = User32.INSTANCE;
                HWND viewer = new HWND(Pointer.createConstant(viewerHandle));
                if (!windows.IsWindowVisible(viewer)) {
                    return;
                }
                HWND nativeWindow = new HWND(Pointer.createConstant(glfwGetWin32Window(window.handle())));
                if (nativeWindow.equals(windows.GetForegroundWindow())) {
                    windows.SetForegroundWindow(viewer);
                }
                ((NativeWindowMode) (Object) window).minecraftUe$syncNativeFullscreen(false);
                glfwHideWindow(window.handle());
                WindowActivation.INSTANCE.EnableWindow(nativeWindow, false);
                hidden = true;
                LOGGER.info("Minecraft UE Bridge handed off presentation; native window hidden");
            }
        }
        if (!hidden) {
            return;
        }
        if (connection != null && connection.presentingWindow != 0) {
            disconnectedAt = 0;
            int size = connection.viewport;
            if (pendingSize != size) {
                pendingSize = size;
                resizeAt = now + RESIZE_SETTLE_TIME;
            }
            if (size != 0 && appliedSize != size && (appliedSize == 0 || now - resizeAt >= 0)) {
                int width = size >>> 16;
                int height = size & 0xffff;
                if (width != window.getWidth() || height != window.getHeight()) {
                    int logicalWidth = (int) Math.round(width * (double) window.getScreenWidth() / Math.max(1, window.getWidth()));
                    int logicalHeight = (int) Math.round(height * (double) window.getScreenHeight() / Math.max(1, window.getHeight()));
                    glfwSetWindowSize(window.handle(), Math.max(1, logicalWidth), Math.max(1, logicalHeight));
                }
                appliedSize = size;
            }
        } else if (disconnectedAt == 0) {
            disconnectedAt = now;
        } else if (now - disconnectedAt >= RECONNECT_TIMEOUT && minecraft.isRunning()) {
            LOGGER.warn("Minecraft UE Bridge viewer did not reconnect; saving and stopping the client");
            minecraft.stop();
        }
    }

    public static boolean isHidden() { return hidden; }
}
