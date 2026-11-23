package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.Minecraft;
import net.minecraft.client.multiplayer.ClientPacketListener;
import net.minecraft.network.protocol.game.ClientboundPlayerPositionPacket;
import net.minecraft.network.protocol.game.ClientboundRespawnPacket;
import net.minecraft.world.entity.PositionMoveRotation;
import net.minecraft.world.entity.Relative;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

import java.util.Set;

@Mixin(ClientPacketListener.class)
abstract class ClientPacketListenerMixin {
    @Inject(method = "handleMovePlayer", at = @At("TAIL"))
    private void minecraftUe$onServerMovePlayer(ClientboundPlayerPositionPacket packet, CallbackInfo ci) {
        Set<Relative> relatives = packet.relatives();
        PositionMoveRotation change = packet.change();
        boolean forcedLook = !relatives.contains(Relative.Y_ROT) || !relatives.contains(Relative.X_ROT)
                || change.yRot() != 0.0f || change.xRot() != 0.0f;
        if (forcedLook) {
            WorldBridge.INSTANCE.onServerForcedLook(Minecraft.getInstance());
        } else {
            WorldBridge.INSTANCE.onServerPositionSync(Minecraft.getInstance());
        }
    }

    @Inject(method = "handleRespawn", at = @At("TAIL"))
    private void minecraftUe$onServerRespawn(ClientboundRespawnPacket packet, CallbackInfo ci) {
        WorldBridge.INSTANCE.onTeleport(Minecraft.getInstance());
    }
}
