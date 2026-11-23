package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.platform.DisplayData;
import com.mojang.blaze3d.platform.Window;
import dev.minecraftue.bridge.BridgePresentation;
import dev.minecraftue.bridge.NativeWindowMode;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import static org.lwjgl.glfw.GLFW.glfwGetWindowMonitor;

@Mixin(Window.class)
abstract class WindowMixin implements NativeWindowMode {
    @Shadow private boolean fullscreen;
    @Shadow private boolean actuallyFullscreen;
    @Shadow public abstract long handle();
    @Shadow private void setMode() { throw new AssertionError(); }
    @Unique private boolean minecraftUe$startupFullscreen;

    @Redirect(method = "<init>", at = @At(value = "INVOKE",
            target = "Lcom/mojang/blaze3d/platform/DisplayData;isFullscreen()Z"))
    private boolean minecraftUe$createWindowed(DisplayData display) {
        minecraftUe$startupFullscreen = display.isFullscreen();
        return !BridgePresentation.REQUESTED && minecraftUe$startupFullscreen;
    }

    @Inject(method = "<init>", at = @At("RETURN"))
    private void minecraftUe$restoreStartupPreference(CallbackInfo ci) {
        if (BridgePresentation.REQUESTED) {
            fullscreen = minecraftUe$startupFullscreen;
        }
    }

    @Inject(method = "setMode", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$keepHiddenWindowWindowed(CallbackInfo ci) {
        if (BridgePresentation.isHidden()) {
            ci.cancel();
        }
    }

    @Override
    public void minecraftUe$syncNativeFullscreen(boolean nativeFullscreen) {
        if ((glfwGetWindowMonitor(handle()) != 0) != nativeFullscreen) {
            boolean requestedFullscreen = fullscreen;
            fullscreen = nativeFullscreen;
            try {
                setMode();
            } finally {
                fullscreen = requestedFullscreen;
            }
        }
        actuallyFullscreen = fullscreen;
    }
}
