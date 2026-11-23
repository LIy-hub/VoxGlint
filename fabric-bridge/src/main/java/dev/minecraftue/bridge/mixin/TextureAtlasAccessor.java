package dev.minecraftue.bridge.mixin;

import net.minecraft.client.renderer.texture.SpriteContents;
import net.minecraft.client.renderer.texture.TextureAtlas;
import net.minecraft.client.renderer.texture.TextureAtlasSprite;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

import java.util.List;

@Mixin(TextureAtlas.class)
public interface TextureAtlasAccessor {
    @Accessor("sprites")
    List<TextureAtlasSprite> minecraftUe$getSprites();

    @Accessor("animatedTexturesStates")
    List<SpriteContents.AnimationState> minecraftUe$getAnimationStates();
}
