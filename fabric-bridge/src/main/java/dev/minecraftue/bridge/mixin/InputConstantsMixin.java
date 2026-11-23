package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.platform.InputConstants;
import com.mojang.blaze3d.platform.Window;
import dev.minecraftue.bridge.RemoteControls;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(InputConstants.class)
abstract class InputConstantsMixin {
    @Inject(method = "isKeyDown", at = @At("HEAD"), cancellable = true)
    private static void minecraftUe$bridgeKeyDown(Window window, int key, CallbackInfoReturnable<Boolean> cir) {
        Boolean pressed = RemoteControls.INSTANCE.keyDown(window, key);
        if (pressed != null) {
            cir.setReturnValue(pressed);
        }
    }
}
