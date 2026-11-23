package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.Camera;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Camera.class)
abstract class CameraMixin {
    @Inject(method = "extractRenderState", at = @At("RETURN"))
    private void minecraftUe$syncCamera(CameraRenderState state, float partialTick, CallbackInfo ci) {
        WorldBridge.INSTANCE.captureCamera((Camera) (Object) this, state, partialTick);
    }
}
