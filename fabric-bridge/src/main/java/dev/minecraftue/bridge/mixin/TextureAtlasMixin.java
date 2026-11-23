package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.TerrainAtlasCache;
import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.renderer.texture.SpriteLoader;
import net.minecraft.client.renderer.texture.TextureAtlas;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(TextureAtlas.class)
abstract class TextureAtlasMixin implements TerrainAtlasCache.Prepared {
    @Unique private TerrainAtlasCache.Entry minecraftUe$prepared;

    @Override
    public TerrainAtlasCache.Entry minecraftUe$getPreparedAtlas() {
        return minecraftUe$prepared;
    }

    @Override
    public void minecraftUe$setPreparedAtlas(TerrainAtlasCache.Entry entry) {
        if (minecraftUe$prepared != null) {
            minecraftUe$prepared.close();
        }
        minecraftUe$prepared = entry;
    }

    @Inject(method = "upload", at = @At("RETURN"))
    private void minecraftUe$activatePreparedAtlas(SpriteLoader.Preparations preparation, CallbackInfo ci) {
        var result = (TerrainAtlasCache.Prepared) (Object) preparation;
        minecraftUe$setPreparedAtlas(result.minecraftUe$getPreparedAtlas());
        result.minecraftUe$setPreparedAtlas(null);
    }

    @Inject(method = "clearTextureData", at = @At("HEAD"))
    private void minecraftUe$clearPreparedAtlas(CallbackInfo ci) {
        minecraftUe$setPreparedAtlas(null);
    }

    @Inject(method = "cycleAnimationFrames", at = @At("RETURN"))
    private void minecraftUe$atlasAnimated(CallbackInfo ci) {
        WorldBridge.INSTANCE.atlasAnimated((TextureAtlas) (Object) this);
    }
}
