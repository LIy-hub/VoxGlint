package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.font.GlyphBitmap;
import com.mojang.blaze3d.font.GlyphInfo;
import dev.minecraftue.bridge.DynamicTextureRevision;
import net.minecraft.client.gui.font.FontTexture;
import net.minecraft.client.gui.font.glyphs.BakedSheetGlyph;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

@Mixin(FontTexture.class)
abstract class FontTextureMixin implements DynamicTextureRevision {
    @Unique private int minecraftUe$revision;

    @Inject(method = "add", at = @At("RETURN"))
    private void minecraftUe$glyphAdded(GlyphInfo info, GlyphBitmap bitmap, CallbackInfoReturnable<BakedSheetGlyph> cir) {
        if (cir.getReturnValue() != null) {
            minecraftUe$revision++;
        }
    }

    @Override
    public int minecraftUe$textureRevision() {
        return minecraftUe$revision;
    }
}
