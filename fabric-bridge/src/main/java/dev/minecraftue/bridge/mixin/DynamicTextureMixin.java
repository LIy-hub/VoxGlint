package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.DynamicTextureRevision;
import net.minecraft.client.renderer.texture.DynamicTexture;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(DynamicTexture.class)
abstract class DynamicTextureMixin implements DynamicTextureRevision {
    @Unique private int minecraftUe$revision;

    @Inject(method = "upload", at = @At("RETURN"))
    private void minecraftUe$uploaded(CallbackInfo ci) {
        minecraftUe$revision++;
    }

    @Override
    public int minecraftUe$textureRevision() {
        return minecraftUe$revision;
    }
}
