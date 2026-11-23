package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.vertex.QuadInstance;
import dev.minecraftue.bridge.SectionCapture;
import net.minecraft.client.renderer.block.BlockQuadOutput;
import net.minecraft.client.renderer.block.ModelBlockRenderer;
import net.minecraft.client.renderer.block.dispatch.BlockStateModelPart;
import net.minecraft.client.resources.model.geometry.BakedQuad;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

import java.util.List;

@Mixin(ModelBlockRenderer.class)
abstract class TerrainModelRendererMixin {
    @Shadow @Final private List<BlockStateModelPart> parts;

    @Redirect(method = "putQuadWithTint", at = @At(value = "INVOKE", target =
            "Lnet/minecraft/client/renderer/block/BlockQuadOutput;put(FFFLnet/minecraft/client/resources/model/geometry/BakedQuad;Lcom/mojang/blaze3d/vertex/QuadInstance;)V"))
    private void minecraftUe$putTerrainQuad(BlockQuadOutput output, float x, float y, float z,
                                           BakedQuad quad, QuadInstance instance) {
        SectionCapture capture = SectionCapture.active();
        if (capture != null) {
            capture.putModelQuad(parts, output, x, y, z, quad, instance);
        } else {
            output.put(x, y, z, quad, instance);
        }
    }
}
