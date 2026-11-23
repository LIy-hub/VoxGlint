package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import net.fabricmc.loader.api.FabricLoader;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.Font;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.screens.Screen;
import org.lwjgl.glfw.GLFW;
import org.slf4j.Logger;

import java.lang.reflect.Method;

/**
 * Optional compatibility layer for Sodium's video settings screen.
 * Works offline without a hard Sodium compile dependency and adapts cleanly across Sodium 0.6/0.8/26.x.
 */
public final class SodiumCompat {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final int BTN_HEIGHT = 20;
    private static final int BTN_MARGIN_RIGHT = 8;
    private static final int BTN_MARGIN_TOP = 6;
    private static Boolean sodiumLoaded;

    private SodiumCompat() {}

    public static boolean isSodiumLoaded() {
        if (sodiumLoaded == null) {
            try {
                sodiumLoaded = FabricLoader.getInstance().isModLoaded("sodium");
            } catch (Throwable ignored) {
                sodiumLoaded = false;
            }
        }
        return sodiumLoaded;
    }

    public static boolean isSodiumVideoScreen(Screen screen) {
        if (screen == null || !isSodiumLoaded()) {
            return false;
        }
        return isSodiumVideoScreenClassName(screen.getClass().getName());
    }

    static boolean isSodiumVideoScreenClassName(String className) {
        if (className == null || className.isEmpty()) {
            return false;
        }
        boolean inSodiumPkg = className.startsWith("net.caffeinemc.mods.sodium.")
                || className.startsWith("me.jellysquid.mods.sodium.");
        if (!inSodiumPkg) {
            return false;
        }
        int dot = className.lastIndexOf('.');
        String simple = dot >= 0 ? className.substring(dot + 1) : className;
        if (simple.contains("Prompt") || simple.contains("Confirm") || simple.contains("Dialog")
                || simple.contains("Warning") || simple.contains("Donate")) {
            return false;
        }
        return simple.contains("OptionsGUI")
                || simple.contains("VideoSettings")
                || simple.contains("SodiumOptions")
                || simple.contains("ConfigScreen")
                || simple.contains("SettingsScreen");
    }

    public static void renderOverlayButton(Screen screen, GuiGraphicsExtractor graphics, int mouseX, int mouseY) {
        if (!isSodiumVideoScreen(screen)) {
            return;
        }
        Minecraft mc = Minecraft.getInstance();
        Font font = mc.font;
        boolean zh = mc.getLanguageManager().getSelected().equals("zh_cn");
        String label = VoxGlintOptionCatalog.summaryButtonLabel(zh).getString();
        int btnW = buttonWidth(font, label, screen.width);
        int x = screen.width - btnW - BTN_MARGIN_RIGHT;
        int y = BTN_MARGIN_TOP;
        boolean hovered = mouseX >= x && mouseX < x + btnW && mouseY >= y && mouseY < y + BTN_HEIGHT;

        int bg = hovered ? 0xEE2B6DC4 : 0xDE162338;
        int border = hovered ? 0xFF70B8FF : 0xFF3E78C4;
        graphics.fill(x, y, x + btnW, y + BTN_HEIGHT, bg);
        graphics.outline(x, y, btnW, BTN_HEIGHT, border);
        graphics.fill(x + 1, y + 1, x + 3, y + BTN_HEIGHT - 1, 0xFF4BA3FF);

        String trimmed = font.plainSubstrByWidth(label, btnW - 10);
        graphics.centeredText(font, trimmed, x + btnW / 2 + 1, y + (BTN_HEIGHT - font.lineHeight) / 2, 0xFFF0F5FC);

        if (hovered) {
            String tip = zh ? "打开 VoxGlint (UE5) 完整图形与光追设置" : "Open VoxGlint (UE5) Graphics & Ray-Tracing Settings";
            int tipW = font.width(tip) + 12;
            int tipX = Math.max(4, screen.width - tipW - BTN_MARGIN_RIGHT);
            int tipY = y + BTN_HEIGHT + 4;
            graphics.fill(tipX, tipY, tipX + tipW, tipY + 16, 0xEC0E141F);
            graphics.outline(tipX, tipY, tipW, 16, 0xFF3E78C4);
            graphics.text(font, tip, tipX + 6, tipY + 4, 0xFFD9E6F7);
        }
    }

    public static boolean handleMouseClick(Screen screen, double mouseX, double mouseY, int button) {
        if (button != GLFW.GLFW_MOUSE_BUTTON_LEFT || !isSodiumVideoScreen(screen)) {
            return false;
        }
        Minecraft mc = Minecraft.getInstance();
        Font font = mc.font;
        boolean zh = mc.getLanguageManager().getSelected().equals("zh_cn");
        String label = VoxGlintOptionCatalog.summaryButtonLabel(zh).getString();
        int btnW = buttonWidth(font, label, screen.width);
        int x = screen.width - btnW - BTN_MARGIN_RIGHT;
        int y = BTN_MARGIN_TOP;
        if (mouseX < x || mouseX >= x + btnW || mouseY < y || mouseY >= y + BTN_HEIGHT) {
            return false;
        }
        tryApplySodiumChanges(screen);
        mc.gui.setScreen(UnrealSettingsScreen.create(screen));
        return true;
    }

    private static int buttonWidth(Font font, String label, int screenWidth) {
        int desired = font.width(label) + 16;
        return Math.clamp(desired, 118, Math.max(120, Math.min(220, screenWidth / 3)));
    }

    private static void tryApplySodiumChanges(Screen screen) {
        for (String methodName : new String[]{"applyChanges", "saveChanges"}) {
            try {
                Method m = screen.getClass().getDeclaredMethod(methodName);
                m.setAccessible(true);
                m.invoke(screen);
                return;
            } catch (NoSuchMethodException ignored) {
                // Try next candidate name
            } catch (Throwable t) {
                LOGGER.debug("Could not invoke Sodium {} before opening VoxGlint settings", methodName, t);
                return;
            }
        }
    }
}
