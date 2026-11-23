package dev.minecraftue.bridge.mixin;

import net.minecraft.world.entity.LightningBolt;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(LightningBolt.class)
public interface LightningBoltAccessor {
    @Accessor("life")
    int minecraftUe$getLife();

    @Accessor("flashes")
    int minecraftUe$getFlashes();
}
