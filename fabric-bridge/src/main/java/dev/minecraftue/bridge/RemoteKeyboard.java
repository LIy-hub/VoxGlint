package dev.minecraftue.bridge;

import net.minecraft.client.input.CharacterEvent;
import net.minecraft.client.input.KeyEvent;

public interface RemoteKeyboard {
    void minecraftUe$key(long window, int action, KeyEvent event);

    void minecraftUe$character(long window, CharacterEvent event);

    void minecraftUe$resetDebugKeys();
}
