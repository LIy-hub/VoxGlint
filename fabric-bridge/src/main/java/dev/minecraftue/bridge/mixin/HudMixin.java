package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.pipeline.RenderPipeline;
import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.gui.Hud;
import net.minecraft.client.renderer.RenderPipelines;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

@Mixin(Hud.class)
abstract class HudMixin {
    @Redirect(method = "extractCrosshair", at = @At(value = "FIELD",
            target = "Lnet/minecraft/client/renderer/RenderPipelines;CROSSHAIR:Lcom/mojang/blaze3d/pipeline/RenderPipeline;"))
    private RenderPipeline minecraftUe$crosshairOverTransparentBackground() {
        // The original inverse-color blend requires the scene as its destination.
        return WorldBridge.INSTANCE.isSceneBridged() ? RenderPipelines.GUI_TEXTURED : RenderPipelines.CROSSHAIR;
    }
}
