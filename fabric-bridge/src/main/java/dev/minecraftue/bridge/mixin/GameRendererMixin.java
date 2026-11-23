package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.vertex.PoseStack;
import dev.minecraftue.bridge.UiCapture;
import dev.minecraftue.bridge.WorldBridge;
import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.Minecraft;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.LightmapRenderStateExtractor;
import net.minecraft.client.renderer.extract.LevelExtractor;
import net.minecraft.client.renderer.state.LightmapRenderState;
import net.minecraft.client.renderer.state.level.CameraRenderState;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.ModifyVariable;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(GameRenderer.class)
abstract class GameRendererMixin {
    @Shadow @Final private Minecraft minecraft;
    @Shadow @Final private RenderTarget mainRenderTarget;
    @Shadow protected abstract void bobHurt(CameraRenderState camera, PoseStack pose);
    @Shadow protected abstract void bobView(CameraRenderState camera, PoseStack pose);
    @Unique private boolean minecraftUe$guiOnly;

    @Inject(method = "extract", at = @At("HEAD"))
    private void minecraftUe$beginFrame(DeltaTracker delta, boolean renderLevel, CallbackInfo ci) {
        // Use one decision for extraction and rendering even if the socket drops mid-frame.
        minecraftUe$guiOnly = WorldBridge.INSTANCE.beginRenderFrame(minecraft, renderLevel);
    }

    @Inject(method = "extractCamera", at = @At("RETURN"))
    private void minecraftUe$snapshotEnvironment(DeltaTracker delta, float partialTick, float cameraPartialTick,
                                                CallbackInfo ci) {
        GameRenderer renderer = (GameRenderer) (Object) this;
        WorldBridge.INSTANCE.captureEnvironment(minecraft.level, renderer.mainCamera(),
                renderer.gameRenderState().levelRenderState.cameraRenderState, partialTick);
    }

    @Inject(method = "extract", at = @At("TAIL"))
    private void minecraftUe$snapshotDynamicEntities(DeltaTracker delta, boolean renderLevel, CallbackInfo ci) {
        if (!minecraftUe$guiOnly) {
            return;
        }
        GameRenderer renderer = (GameRenderer) (Object) this;
        var state = renderer.gameRenderState();
        WorldBridge.INSTANCE.captureDynamic(minecraft, renderer.mainCamera(), state, delta, pose -> {
            var camera = state.levelRenderState.cameraRenderState;
            bobHurt(camera, pose);
            if (state.optionsRenderState.bobView) {
                bobView(camera, pose);
            }
        });
    }

    @Redirect(method = "extract", at = @At(value = "INVOKE", target =
            "Lnet/minecraft/client/renderer/LightmapRenderStateExtractor;extract(Lnet/minecraft/client/renderer/state/LightmapRenderState;F)V"))
    private void minecraftUe$extractWorldLight(LightmapRenderStateExtractor extractor, LightmapRenderState state, float delta) {
        if (!minecraftUe$guiOnly) {
            extractor.extract(state, delta);
        }
    }

    @Redirect(method = "extract", at = @At(value = "INVOKE", target =
            "Lnet/minecraft/client/renderer/extract/LevelExtractor;extract(Lnet/minecraft/client/DeltaTracker;Lnet/minecraft/client/Camera;F)V"))
    private void minecraftUe$extractWorld(LevelExtractor extractor, DeltaTracker delta, Camera camera, float partialTick) {
        if (!minecraftUe$guiOnly) {
            extractor.extract(delta, camera, partialTick);
        }
    }

    @ModifyVariable(method = "render", at = @At("HEAD"), argsOnly = true)
    private boolean minecraftUe$renderWorld(boolean renderLevel) {
        return renderLevel && !minecraftUe$guiOnly;
    }

    @Inject(method = "processBlurEffect", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$skipSceneBlur(CallbackInfo ci) {
        if (minecraftUe$guiOnly) {
            // The scene lives in UE. MC still draws the menu's translucent dimming layer.
            ci.cancel();
        }
    }

    @Inject(method = "render", at = @At(value = "INVOKE",
            target = "Lnet/minecraft/client/gui/render/GuiRenderer;render()V", shift = At.Shift.AFTER))
    private void minecraftUe$readUi(DeltaTracker delta, boolean renderLevel, CallbackInfo ci) {
        UiCapture.INSTANCE.capture(mainRenderTarget);
    }

    @Inject(method = "close", at = @At("HEAD"))
    private void minecraftUe$closeCapture(CallbackInfo ci) {
        UiCapture.INSTANCE.close();
    }
}
