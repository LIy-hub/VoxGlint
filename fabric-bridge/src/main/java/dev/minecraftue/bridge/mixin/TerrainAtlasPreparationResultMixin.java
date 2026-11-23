package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.TerrainAtlasCache;
import net.minecraft.client.renderer.texture.SpriteLoader;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;

@Mixin(SpriteLoader.Preparations.class)
abstract class TerrainAtlasPreparationResultMixin implements TerrainAtlasCache.Prepared {
    @Unique private TerrainAtlasCache.Entry minecraftUe$prepared;

    @Override
    public TerrainAtlasCache.Entry minecraftUe$getPreparedAtlas() {
        return minecraftUe$prepared;
    }

    @Override
    public void minecraftUe$setPreparedAtlas(TerrainAtlasCache.Entry entry) {
        minecraftUe$prepared = entry;
    }
}
