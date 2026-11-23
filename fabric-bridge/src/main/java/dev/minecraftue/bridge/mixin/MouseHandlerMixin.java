package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.Blaze3D;
import dev.minecraftue.bridge.BridgePresentation;
import dev.minecraftue.bridge.RemoteControls;
import dev.minecraftue.bridge.RemoteMouse;
import dev.minecraftue.bridge.SodiumCompat;
import net.minecraft.client.Minecraft;
import net.minecraft.client.MouseHandler;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.gui.screens.inventory.AbstractContainerScreen;
import net.minecraft.client.input.MouseButtonEvent;
import net.minecraft.client.input.MouseButtonInfo;
import net.minecraft.util.SmoothDouble;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.gen.Invoker;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import static org.lwjgl.glfw.GLFW.GLFW_MOUSE_BUTTON_LEFT;
import static org.lwjgl.glfw.GLFW.GLFW_PRESS;
import static org.lwjgl.glfw.GLFW.GLFW_RELEASE;

@Mixin(MouseHandler.class)
abstract class MouseHandlerMixin implements RemoteMouse {
    @Shadow @Final private Minecraft minecraft;
    @Shadow @Final private SmoothDouble smoothTurnX;
    @Shadow @Final private SmoothDouble smoothTurnY;
    @Shadow private double accumulatedDX;
    @Shadow private double accumulatedDY;
    @Shadow private double lastHandleMovementTime;
    @Shadow private double xpos;
    @Shadow private double ypos;
    @Shadow private MouseButtonInfo activeButton;
    @Shadow private boolean isLeftPressed;
    @Shadow private boolean isMiddlePressed;
    @Shadow private boolean isRightPressed;
    @Unique private boolean minecraftUe$remoteEvent;

    @Invoker("turnPlayer")
    protected abstract void minecraftUe$vanillaTurn(double elapsed);

    @Invoker("onButton")
    protected abstract void minecraftUe$vanillaButton(long window, MouseButtonInfo info, int action);

    @Invoker("onScroll")
    protected abstract void minecraftUe$vanillaScroll(long window, double horizontal, double vertical);

    @Inject(method = "onMove", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$keepRemotePointer(long window, double x, double y, CallbackInfo ci) {
        if (BridgePresentation.isHidden() || RemoteControls.INSTANCE.ownsPointer(minecraft)) {
            ci.cancel();
        }
    }

    @Inject(method = "onButton", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$beforeButton(long window, MouseButtonInfo info, int action, CallbackInfo ci) {
        if (!minecraftUe$remoteEvent) {
            if (BridgePresentation.isHidden()) {
                ci.cancel();
                return;
            } else {
                RemoteControls.INSTANCE.returnToMinecraft(minecraft);
            }
        }
        if (action == GLFW_PRESS && info.button() == GLFW_MOUSE_BUTTON_LEFT) {
            Screen screen = minecraft.gui.screen();
            if (screen != null && SodiumCompat.isSodiumVideoScreen(screen)) {
                double sx = MouseHandler.getScaledXPos(minecraft.getWindow(), xpos);
                double sy = MouseHandler.getScaledYPos(minecraft.getWindow(), ypos);
                if (SodiumCompat.handleMouseClick(screen, sx, sy, info.button())) {
                    ci.cancel();
                }
            }
        }
    }

    @Redirect(method = "onButton", at = @At(value = "INVOKE", target = "Lnet/minecraft/client/MouseHandler;grabMouse()V"))
    private void minecraftUe$grabNativeMouse(MouseHandler handler) {
        if (!minecraftUe$remoteEvent) {
            handler.grabMouse();
        }
    }

    @Inject(method = "onScroll", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$beforeScroll(long window, double horizontal, double vertical, CallbackInfo ci) {
        if (BridgePresentation.isHidden() && !minecraftUe$remoteEvent) {
            ci.cancel();
        }
    }

    @Inject(method = "handleAccumulatedMovement", at = @At("HEAD"))
    private void minecraftUe$pollMouse(CallbackInfo ci) {
        RemoteControls.INSTANCE.beforeRenderMouse(minecraft);
    }

    @Override
    public void minecraftUe$turn(double dx, double dy) {
        double physicalX = accumulatedDX;
        double physicalY = accumulatedDY;
        double now = Blaze3D.getTime();
        double elapsed = Math.clamp(now - lastHandleMovementTime, 0, 0.1);
        lastHandleMovementTime = now;
        accumulatedDX = dx;
        accumulatedDY = dy;
        try {
            minecraftUe$vanillaTurn(elapsed);
        } finally {
            accumulatedDX = physicalX;
            accumulatedDY = physicalY;
        }
    }

    @Override
    public void minecraftUe$resetSmoothing() {
        smoothTurnX.reset();
        smoothTurnY.reset();
    }

    @Override
    public void minecraftUe$moveInScreen(double normalizedX, double normalizedY) {
        var window = minecraft.getWindow();
        double nextX = normalizedX * window.getScreenWidth();
        double nextY = normalizedY * window.getScreenHeight();
        if (nextX == xpos && nextY == ypos) {
            return;
        }
        double dx = MouseHandler.getScaledXPos(window, nextX - xpos);
        double dy = MouseHandler.getScaledYPos(window, nextY - ypos);
        xpos = nextX;
        ypos = nextY;
        double x = MouseHandler.getScaledXPos(window, xpos);
        double y = MouseHandler.getScaledYPos(window, ypos);
        // Vanilla dispatches movement only while its window is active. UE currently owns the pointer.
        Screen screen = minecraft.gui.screen();
        screen.mouseMoved(x, y);
        if (activeButton != null && minecraft.gui.screen() == screen) {
            screen.mouseDragged(new MouseButtonEvent(x, y, activeButton), dx, dy);
        }
        screen.afterMouseMove();
        minecraft.getFramerateLimitTracker().onInputReceived();
    }

    @Override
    public void minecraftUe$button(long window, int button, int modifiers, boolean down) {
        minecraftUe$remoteEvent = true;
        try {
            minecraftUe$vanillaButton(window, new MouseButtonInfo(button, modifiers), down ? GLFW_PRESS : GLFW_RELEASE);
        } finally {
            minecraftUe$remoteEvent = false;
        }
    }

    @Override
    public void minecraftUe$scroll(long window, double horizontal, double vertical) {
        minecraftUe$remoteEvent = true;
        try {
            minecraftUe$vanillaScroll(window, horizontal, vertical);
        } finally {
            minecraftUe$remoteEvent = false;
        }
    }

    @Override
    public void minecraftUe$clearGameplayButtons() {
        activeButton = null;
        isLeftPressed = isMiddlePressed = isRightPressed = false;
    }

    @Override
    public void minecraftUe$cancelScreenDrag(Screen screen) {
        activeButton = null;
        screen.setDragging(false);
        if (screen instanceof AbstractContainerScreen<?> container) {
            var drag = (ContainerDragAccessor) container;
            drag.minecraftUe$setQuickCrafting(false);
            drag.minecraftUe$quickCraftSlots().clear();
            drag.minecraftUe$setSkipNextRelease(false);
            drag.minecraftUe$setDoubleclick(false);
        }
    }
}
