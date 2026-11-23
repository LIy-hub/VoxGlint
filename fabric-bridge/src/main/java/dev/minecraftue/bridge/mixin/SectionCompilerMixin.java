package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.vertex.BufferBuilder;
import dev.minecraftue.bridge.SectionCapture;
import net.minecraft.client.renderer.SectionBufferBuilderPack;
import net.minecraft.client.renderer.block.BlockAndTintGetter;
import net.minecraft.client.renderer.block.BlockStateModelSet;
import net.minecraft.client.renderer.block.FluidRenderer;
import net.minecraft.client.renderer.chunk.ChunkSectionLayer;
import net.minecraft.client.renderer.chunk.RenderSectionRegion;
import net.minecraft.client.renderer.chunk.SectionCompiler;
import net.minecraft.core.BlockPos;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.material.FluidState;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import java.util.Map;

@Mixin(SectionCompiler.class)
abstract class SectionCompilerMixin {
    @Shadow @Final private BlockStateModelSet blockModelSet;

    @Inject(method = "compile", at = @At("HEAD"))
    private void minecraftUe$useBakedModels(CallbackInfoReturnable<SectionCompiler.Results> cir) {
        SectionCapture capture = SectionCapture.active();
        if (capture != null) {
            capture.useBakedModels(blockModelSet);
        }
    }

    @Redirect(method = "compile", at = @At(value = "INVOKE", target =
            "Lnet/minecraft/client/renderer/chunk/RenderSectionRegion;getBlockState(Lnet/minecraft/core/BlockPos;)Lnet/minecraft/world/level/block/state/BlockState;"))
    private BlockState minecraftUe$beginBlock(RenderSectionRegion region, BlockPos pos) {
        BlockState state = region.getBlockState(pos);
        SectionCapture capture = SectionCapture.active();
        if (capture != null && !state.isAir()) {
            capture.beginBlock(region, pos, state);
        }
        return state;
    }

    @Redirect(method = "compile", at = @At(value = "INVOKE", target =
            "Lnet/minecraft/client/renderer/block/FluidRenderer;tesselate(Lnet/minecraft/client/renderer/block/BlockAndTintGetter;Lnet/minecraft/core/BlockPos;Lnet/minecraft/client/renderer/block/FluidRenderer$Output;Lnet/minecraft/world/level/block/state/BlockState;Lnet/minecraft/world/level/material/FluidState;)V"))
    private void minecraftUe$markFluid(FluidRenderer renderer, BlockAndTintGetter region, BlockPos pos,
                                       FluidRenderer.Output output, BlockState block, FluidState fluid) {
        SectionCapture capture = SectionCapture.active();
        if (capture == null) {
            renderer.tesselate(region, pos, output, block, fluid);
            return;
        }
        capture.setFluid(fluid);
        try {
            renderer.tesselate(region, pos, output, block, fluid);
        } finally {
            capture.setFluid(null);
        }
    }

    @Inject(method = "getOrBeginLayer", at = @At("RETURN"))
    private void minecraftUe$recordLayer(Map<ChunkSectionLayer, BufferBuilder> startedLayers,
                                        SectionBufferBuilderPack builders, ChunkSectionLayer layer,
                                        CallbackInfoReturnable<BufferBuilder> cir) {
        SectionCapture capture = SectionCapture.active();
        if (capture != null) {
            capture.selectLayer(layer, ((BufferBuilderAccessor) cir.getReturnValue()).minecraftUe$getVertexCount());
        }
    }
}
