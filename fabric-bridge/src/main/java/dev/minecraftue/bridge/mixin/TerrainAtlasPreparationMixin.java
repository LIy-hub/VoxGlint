package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.TerrainAtlasCache;
import net.minecraft.client.renderer.texture.SpriteLoader;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.server.packs.resources.ResourceManager;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;

@Mixin(targets = "net.minecraft.client.resources.model.sprite.AtlasManager$AtlasEntry")
abstract class TerrainAtlasPreparationMixin {
    @Shadow @Final private TextureAtlas atlas;

    @Inject(method = "scheduleLoad", at = @At("RETURN"), cancellable = true)
    private void minecraftUe$prepareTerrainAtlas(ResourceManager resources, Executor executor, int mips,
            CallbackInfoReturnable<CompletableFuture<SpriteLoader.Preparations>> callback) {
        if (!atlas.location().equals(TextureAtlas.LOCATION_BLOCKS)) {
            return;
        }
        // The reload owns the prepared sprites and the resource manager until this stage completes.
        // The atlas cannot upload or close its new sprite images while the worker is reading them.
        callback.setReturnValue(callback.getReturnValue().thenCompose(preparation ->
                preparation.readyForUpload().thenApplyAsync(ignored -> {
                    TerrainAtlasCache.prepare(preparation, resources);
                    return preparation;
                }, TerrainAtlasCache.PREPARE)));
    }
}
