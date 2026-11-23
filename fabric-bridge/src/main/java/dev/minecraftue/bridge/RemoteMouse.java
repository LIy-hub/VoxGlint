package dev.minecraftue.bridge;

import net.minecraft.client.gui.screens.Screen;

public interface RemoteMouse {
    void minecraftUe$turn(double dx, double dy);

    void minecraftUe$resetSmoothing();

    void minecraftUe$moveInScreen(double normalizedX, double normalizedY);

    void minecraftUe$button(long window, int button, int modifiers, boolean down);

    void minecraftUe$scroll(long window, double horizontal, double vertical);

    void minecraftUe$clearGameplayButtons();

    void minecraftUe$cancelScreenDrag(Screen screen);
}
