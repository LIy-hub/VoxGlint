package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientChunkCache;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.SectionPos;
import net.minecraft.network.FriendlyByteBuf;
import net.minecraft.network.protocol.game.ClientboundLevelChunkPacketData;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.chunk.LevelChunk;
import net.minecraft.world.level.levelgen.Heightmap;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import java.util.Map;
import java.util.function.Consumer;

@Mixin(ClientChunkCache.class)
abstract class ClientChunkCacheMixin {
    @Shadow @Final private ClientLevel level;

    @Inject(method = "onLightUpdate", at = @At("RETURN"))
    private void minecraftUe$recordLightUpdate(LightLayer layer, SectionPos section, CallbackInfo ci) {
        Minecraft minecraft = Minecraft.getInstance();
        if (minecraft.isSameThread()) {
            WorldBridge.INSTANCE.lightChanged(level, section);
        } else {
            minecraft.execute(() -> WorldBridge.INSTANCE.lightChanged(level, section));
        }
    }

    @Inject(method = "drop", at = @At("RETURN"))
    private void minecraftUe$recordDrop(ChunkPos pos, CallbackInfo ci) {
        WorldBridge.INSTANCE.chunkDropped(pos);
    }

    @Inject(method = "replaceWithPacketData", at = @At("RETURN"))
    private void minecraftUe$recordReplacement(
            int x, int z, FriendlyByteBuf data, Map<Heightmap.Types, long[]> heights,
            Consumer<ClientboundLevelChunkPacketData.BlockEntityTagOutput> blockEntities,
            CallbackInfoReturnable<LevelChunk> cir) {
        if (cir.getReturnValue() != null) {
            WorldBridge.INSTANCE.chunkReplaced(cir.getReturnValue());
        }
    }
}
