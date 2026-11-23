package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.components.debug.DebugEntryFps;
import net.minecraft.client.gui.components.debug.DebugScreenDisplayer;
import net.minecraft.client.gui.components.debug.DebugScreenEntries;
import net.minecraft.world.level.Level;
import net.minecraft.world.level.chunk.LevelChunk;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import java.util.Locale;

@Mixin(DebugEntryFps.class)
abstract class DebugEntryFpsMixin {
    @Inject(method = "display", at = @At("HEAD"), cancellable = true)
    private void minecraftue$showRendererFps(DebugScreenDisplayer displayer, Level level,
                                              LevelChunk clientChunk, LevelChunk serverChunk, CallbackInfo ci) {
        WorldBridge bridge = WorldBridge.INSTANCE;
        if (!bridge.isSceneBridged()) {
            return;
        }
        Minecraft minecraft = Minecraft.getInstance();
        WorldBridge.DebugStats stats = bridge.debugStats();
        String unrealFps = stats != null && stats.frameMs() > 0
                ? String.format(Locale.ROOT, "UE %.0f fps (%.1f ms)", 1000.0 / stats.frameMs(), stats.frameMs())
                : "UE -- fps (-- ms)";
        displayer.addPriorityLine(unrealFps + " | MC " + minecraft.getFps() + " fps");
        if (minecraft.debugEntries.isOverlayVisible()
                && !minecraft.debugEntries.isCurrentlyEnabled(DebugScreenEntries.CHUNK_RENDER_STATS)) {
            DebugScreenEntries.getEntry(DebugScreenEntries.CHUNK_RENDER_STATS)
                    .display(displayer, level, clientChunk, serverChunk);
        }
        ci.cancel();
    }
}
