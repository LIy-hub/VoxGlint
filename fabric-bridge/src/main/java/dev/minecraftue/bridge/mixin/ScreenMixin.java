package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.SodiumCompat;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.screens.Screen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(Screen.class)
abstract class ScreenMixin {
    @Inject(method = "extractRenderStateWithTooltipAndSubtitles", at = @At(
            value = "INVOKE",
            target = "Lnet/minecraft/client/gui/screens/Screen;extractRenderState(Lnet/minecraft/client/gui/GuiGraphicsExtractor;IIF)V",
            shift = At.Shift.AFTER))
    private void minecraftUe$renderSodiumEntry(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick,
                                               CallbackInfo ci) {
        SodiumCompat.renderOverlayButton((Screen) (Object) this, graphics, mouseX, mouseY);
    }
}
