package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.platform.FramerateLimitTracker;
import com.mojang.blaze3d.systems.GpuSurface;
import com.mojang.blaze3d.systems.SurfaceException;
import dev.minecraftue.bridge.BridgePresentation;
import dev.minecraftue.bridge.UiCapture;
import dev.minecraftue.bridge.RemoteControls;
import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.Minecraft;
import net.minecraft.client.MouseHandler;
import net.minecraft.util.Util;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Minecraft.class)
abstract class MinecraftMixin {
    @Shadow private long lastActiveTime;

    @Redirect(method = "<init>", at = @At(value = "INVOKE", target = "Lorg/lwjgl/glfw/GLFW;glfwShowWindow(J)V"))
    private void minecraftUe$showWindow(long window) {
        BridgePresentation.showInitially(window);
    }

    @Redirect(method = "renderFrame", at = @At(value = "INVOKE",
            target = "Lcom/mojang/blaze3d/systems/GpuSurface;acquireNextTexture()V"))
    private void minecraftUe$acquireWindowImage(GpuSurface surface) throws SurfaceException {
        if (!BridgePresentation.isHidden()) {
            surface.acquireNextTexture();
        }
    }

    @Redirect(method = "renderFrame", at = @At(value = "INVOKE",
            target = "Lcom/mojang/blaze3d/platform/FramerateLimitTracker;getFramerateLimit()I"))
    private int minecraftUe$frameLimit(FramerateLimitTracker tracker) {
        if (BridgePresentation.isHidden()) {
            return UiCapture.INSTANCE.frameLimit(((Minecraft) (Object) this).options.framerateLimit().get());
        }
        return tracker.getFramerateLimit();
    }

    @Inject(method = "close", at = @At("HEAD"))
    private void minecraftUe$closeBridge(CallbackInfo ci) {
        WorldBridge.INSTANCE.close();
    }

    @Inject(method = "tick", at = @At("HEAD"))
    private void minecraftUe$pollControls(CallbackInfo ci) {
        RemoteControls.INSTANCE.beforeTick((Minecraft) (Object) this);
    }

    @Inject(method = "tick", at = @At("TAIL"))
    private void minecraftUe$syncTerrain(CallbackInfo ci) {
        WorldBridge.INSTANCE.tick((Minecraft) (Object) this);
    }

    @Inject(method = "pauseIfInactive", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$keepWorldRunning(CallbackInfo ci) {
        if (WorldBridge.INSTANCE.keepWorldRunning((Minecraft) (Object) this)) {
            lastActiveTime = Util.getMillis();
            ci.cancel();
        }
    }

    @Redirect(method = "handleKeybinds", at = @At(value = "INVOKE",
            target = "Lnet/minecraft/client/MouseHandler;isMouseGrabbed()Z"))
    private boolean minecraftUe$canContinueAttack(MouseHandler mouse) {
        return mouse.isMouseGrabbed() || RemoteControls.INSTANCE.isGameplayActive();
    }

    @Inject(method = "renderFrame", at = @At(value = "INVOKE",
            target = "Lcom/mojang/blaze3d/systems/CommandEncoder;submit()V", shift = At.Shift.AFTER))
    private void minecraftUe$hudFrameSubmitted(boolean render, CallbackInfo callback) {
        UiCapture.INSTANCE.afterFrameSubmit();
    }
}
