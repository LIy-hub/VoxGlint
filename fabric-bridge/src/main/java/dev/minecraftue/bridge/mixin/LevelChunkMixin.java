package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.multiplayer.ClientLevel;
import net.minecraft.core.BlockPos;
import net.minecraft.network.FriendlyByteBuf;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.LevelChunk;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(LevelChunk.class)
abstract class LevelChunkMixin {
    @Inject(method = "replaceBiomes", at = @At("RETURN"))
    private void minecraftUe$recordBiomeChange(FriendlyByteBuf data, CallbackInfo ci) {
        WorldBridge.INSTANCE.chunkBiomesChanged((LevelChunk) (Object) this);
    }

    @Inject(method = "setBlockState", at = @At("RETURN"))
    private void minecraftUe$recordChange(BlockPos pos, BlockState state, int flags,
                                         CallbackInfoReturnable<BlockState> cir) {
        LevelChunk chunk = (LevelChunk) (Object) this;
        if (cir.getReturnValue() != null && chunk.getLevel() instanceof ClientLevel level) {
            WorldBridge.INSTANCE.blockChanged(level, pos);
        }
    }
}
