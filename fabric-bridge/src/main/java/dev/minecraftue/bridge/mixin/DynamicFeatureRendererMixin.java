package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.vertex.VertexConsumer;
import dev.minecraftue.bridge.DynamicMeshCapture;
import net.minecraft.client.renderer.feature.FeatureFrameContext;
import net.minecraft.client.renderer.feature.RenderTypeFeatureRenderer;
import net.minecraft.client.renderer.feature.submit.SubmitNode;
import net.minecraft.client.renderer.rendertype.RenderType;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import java.util.List;

@Mixin(RenderTypeFeatureRenderer.class)
abstract class DynamicFeatureRendererMixin implements DynamicMeshCapture.FeatureBuilder {
    @Shadow protected abstract void buildGroup(FeatureFrameContext context, List<? extends SubmitNode> submits);

    @Override
    public void minecraftUe$buildDynamicGeometry(FeatureFrameContext context, List<? extends SubmitNode> submits) {
        buildGroup(context, submits);
    }

    @Inject(method = "getVertexBuilder", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$captureVertices(RenderType type, CallbackInfoReturnable<VertexConsumer> cir) {
        VertexConsumer vertices = DynamicMeshCapture.activeVertices(type);
        if (vertices != null) {
            cir.setReturnValue(vertices);
        }
    }
}
