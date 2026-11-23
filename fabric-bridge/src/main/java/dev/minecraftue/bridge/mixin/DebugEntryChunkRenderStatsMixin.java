package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.gui.components.debug.DebugEntryChunkRenderStats;
import net.minecraft.client.gui.components.debug.DebugScreenDisplayer;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.chunk.LevelChunk;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(DebugEntryChunkRenderStats.class)
abstract class DebugEntryChunkRenderStatsMixin {
    @Inject(method = "display", at = @At("HEAD"), cancellable = true)
    private void minecraftue$showRendererSections(DebugScreenDisplayer displayer, Level level,
                                                  LevelChunk clientChunk, LevelChunk serverChunk, CallbackInfo ci) {
        WorldBridge bridge = WorldBridge.INSTANCE;
        if (bridge.isSceneBridged()) {
            WorldBridge.DebugStats stats = bridge.debugStats();
            if (stats == null) {
                displayer.addLine("UE sections: --");
            } else {
                displayer.addLine("UE view: " + stats.viewDistance() + " chunks, " + stats.receivedColumns() + " columns");
                displayer.addLine("UE sections: " + stats.loadedSections() + " loaded, " + stats.pendingSections() + " queued");
                String limit = stats.fpsLimit() == 0 ? "unlimited" : stats.fpsLimit() + " fps";
                displayer.addLine("UE limit: " + limit + ", VSync: " + (stats.vsync() ? "on" : "off"));
            }
            ci.cancel();
        }
    }
}
