package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.DebugScreenOverlay;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyArg;
import org.spongepowered.asm.mixin.injection.Redirect;

import java.util.List;

@Mixin(DebugScreenOverlay.class)
abstract class DebugScreenOverlayMixin {
    @Shadow
    private void extractLines(GuiGraphicsExtractor graphics, List<String> lines, boolean left) {
        throw new AssertionError();
    }

    @Redirect(method = "extractRenderState", at = @At(value = "INVOKE", ordinal = 1, target =
            "Lnet/minecraft/client/gui/components/DebugScreenOverlay;extractLines(Lnet/minecraft/client/gui/GuiGraphicsExtractor;Ljava/util/List;Z)V"))
    private void minecraftue$leaveRoomForPanel(DebugScreenOverlay overlay, GuiGraphicsExtractor graphics,
                                               List<String> lines, boolean left) {
        WorldBridge.DebugStats stats = WorldBridge.INSTANCE.debugStats();
        int inset = stats == null ? 0 : stats.panelInset(graphics.guiHeight());
        if (inset == 0) {
            extractLines(graphics, lines, left);
            return;
        }
        var pose = graphics.pose();
        pose.pushMatrix().translate(0, inset);
        try {
            extractLines(graphics, lines, left);
        } finally {
            pose.popMatrix();
        }
    }

    @ModifyArg(method = "extractRenderState", index = 2, at = @At(value = "INVOKE", ordinal = 1, target =
            "Lnet/minecraft/client/gui/components/DebugScreenOverlay;formatChart(Lnet/minecraft/client/KeyMapping;Lnet/minecraft/client/KeyMapping;Ljava/lang/String;Z)Ljava/lang/String;"))
    private String minecraftue$labelMinecraftFpsChart(String chart) {
        return WorldBridge.INSTANCE.isSceneBridged() ? "MC " + chart : chart;
    }
}
