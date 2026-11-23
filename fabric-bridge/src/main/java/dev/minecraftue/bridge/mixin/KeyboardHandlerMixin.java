package dev.minecraftue.bridge.mixin;

import dev.minecraftue.bridge.BridgePresentation;
import dev.minecraftue.bridge.RemoteControls;
import dev.minecraftue.bridge.RemoteKeyboard;
import net.minecraft.client.KeyboardHandler;
import net.minecraft.client.Minecraft;
import net.minecraft.client.input.CharacterEvent;
import net.minecraft.client.input.KeyEvent;
import org.spongepowered.asm.mixin.Final;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.Shadow;
import org.spongepowered.asm.mixin.Unique;
import org.spongepowered.asm.mixin.gen.Invoker;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

@Mixin(KeyboardHandler.class)
abstract class KeyboardHandlerMixin implements RemoteKeyboard {
    @Shadow @Final private Minecraft minecraft;
    @Shadow private long debugCrashKeyTime;
    @Shadow private boolean usedDebugKeyAsModifier;
    @Unique private boolean minecraftUe$remoteEvent;

    @Invoker("keyPress")
    protected abstract void minecraftUe$vanillaKey(long window, int action, KeyEvent event);

    @Invoker("charTyped")
    protected abstract void minecraftUe$vanillaCharacter(long window, CharacterEvent event);

    @Override
    public void minecraftUe$key(long window, int action, KeyEvent event) {
        minecraftUe$remoteEvent = true;
        try {
            minecraftUe$vanillaKey(window, action, event);
        } finally {
            minecraftUe$remoteEvent = false;
        }
    }

    @Override
    public void minecraftUe$character(long window, CharacterEvent event) {
        minecraftUe$remoteEvent = true;
        try {
            minecraftUe$vanillaCharacter(window, event);
        } finally {
            minecraftUe$remoteEvent = false;
        }
    }

    @Inject(method = "keyPress", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$beforeKey(long window, int action, KeyEvent event, CallbackInfo ci) {
        if (!minecraftUe$remoteEvent && BridgePresentation.isHidden()) {
            ci.cancel();
        } else if (window == minecraft.getWindow().handle()) {
            RemoteControls.INSTANCE.returnToMinecraft(minecraft);
        }
    }

    @Inject(method = "charTyped", at = @At("HEAD"), cancellable = true)
    private void minecraftUe$beforeCharacter(long window, CharacterEvent event, CallbackInfo ci) {
        if (!minecraftUe$remoteEvent && BridgePresentation.isHidden()) {
            ci.cancel();
        }
    }

    @Override
    public void minecraftUe$resetDebugKeys() {
        debugCrashKeyTime = -1;
        usedDebugKeyAsModifier = false;
    }
}
