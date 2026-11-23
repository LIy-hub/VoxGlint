package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.BridgePresentation;
import dev.minecraftue.bridge.UnrealSettingsScreen;
import net.minecraft.client.OptionInstance;
import net.minecraft.client.Options;
import net.minecraft.client.gui.components.OptionsList;
import net.minecraft.client.gui.screens.options.VideoSettingsScreen;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

import java.util.Arrays;

@Mixin(VideoSettingsScreen.class)
abstract class VideoSettingsScreenMixin {
    @Redirect(method = "addOptions", at = @At(value = "INVOKE",
            target = "Lnet/minecraft/client/gui/components/OptionsList;addBig(Lnet/minecraft/client/OptionInstance;)V", ordinal = 0))
    private void minecraftUe$bridgeFullscreenResolution(OptionsList list, OptionInstance<?> resolution) {
        if (!BridgePresentation.isHidden()) {
            list.addBig(resolution);
        }
        list.addBig(UnrealSettingsScreen.entry((VideoSettingsScreen) (Object) this, list::applyUnsavedChanges));
    }

    @Inject(method = "displayOptions", at = @At("RETURN"), cancellable = true)
    private static void minecraftUe$bridgeDisplayOptions(Options options, CallbackInfoReturnable<OptionInstance<?>[]> cir) {
        if (BridgePresentation.isHidden()) {
            cir.setReturnValue(Arrays.stream(cir.getReturnValue())
                    .filter(option -> option != options.exclusiveFullscreen())
                    .toArray(OptionInstance<?>[]::new));
        }
    }
}
