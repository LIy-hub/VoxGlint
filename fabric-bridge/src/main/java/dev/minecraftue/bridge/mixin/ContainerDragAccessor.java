package dev.minecraftue.bridge.mixin;

import net.minecraft.client.gui.screens.inventory.AbstractContainerScreen;
import net.minecraft.world.inventory.Slot;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

import java.util.Set;

@Mixin(AbstractContainerScreen.class)
public interface ContainerDragAccessor {
    @Accessor("isQuickCrafting")
    void minecraftUe$setQuickCrafting(boolean crafting);

    @Accessor("quickCraftSlots")
    Set<Slot> minecraftUe$quickCraftSlots();

    @Accessor("skipNextRelease")
    void minecraftUe$setSkipNextRelease(boolean skip);

    @Accessor("doubleclick")
    void minecraftUe$setDoubleclick(boolean doubleClick);
}
