package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.renderer.ItemInHandRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(ItemInHandRenderer.class)
abstract class ItemInHandRendererMixin {
    @Shadow private float mainHandHeight;
    @Shadow private float offHandHeight;

    @Inject(method = "tick", at = @At("TAIL"))
    private void minecraftUe$syncHandEquip(CallbackInfo ci) {
        WorldBridge.INSTANCE.updateHandEquip(mainHandHeight, offHandHeight);
    }
}
