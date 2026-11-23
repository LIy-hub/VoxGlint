package dev.minecraftue.bridge;

import com.mojang.logging.LogUtils;
import net.minecraft.client.Minecraft;
import net.minecraft.client.gui.GuiGraphicsExtractor;
import net.minecraft.client.gui.components.Button;
import net.minecraft.client.gui.components.EditBox;
import net.minecraft.client.gui.components.Tooltip;
import net.minecraft.client.gui.components.toasts.SystemToast;
import net.minecraft.client.gui.screens.Screen;
import net.minecraft.client.input.CharacterEvent;
import net.minecraft.client.input.KeyEvent;
import net.minecraft.client.input.MouseButtonEvent;
import net.minecraft.network.chat.CommonComponents;
import net.minecraft.network.chat.Component;
import net.minecraft.util.FormattedCharSequence;
import org.lwjgl.glfw.GLFW;
import org.slf4j.Logger;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.Map;

import static dev.minecraftue.bridge.VoxGlintOptionCatalog.Category;
import static dev.minecraftue.bridge.VoxGlintOptionCatalog.ControlKind;
import static dev.minecraftue.bridge.VoxGlintOptionCatalog.OptionEntry;

/** Modern multi-pane settings screen for VoxGlint (Unreal Engine real-time renderer). */
public final class UnrealSettingsScreen extends Screen implements VoxGlintOptionCatalog.Host {
    private static final Logger LOGGER = LogUtils.getLogger();
    private static final SystemToast.SystemToastId TOAST_ID = new SystemToast.SystemToastId();

    // Palette
    private static final int COL_BACKDROP = 0xB8090C12;
    private static final int COL_BAR_BG = 0xEB10151F;
    private static final int COL_NAV_BG = 0xE20E131D;
    private static final int COL_DETAIL_BG = 0xE20E131D;
    private static final int COL_BORDER = 0xFF253145;
    private static final int COL_ACCENT = 0xFF4BA3FF;
    private static final int COL_ACCENT_SOFT = 0x3D4BA3FF;
    private static final int COL_PREF_DOT = 0xFFE8B44C;
    private static final int COL_ROW_BG = 0xB8161E2C;
    private static final int COL_ROW_HOVER = 0xDA1D283B;
    private static final int COL_ROW_DISABLED = 0x80121721;
    private static final int COL_CTRL_BG = 0xFF0E141F;
    private static final int COL_CTRL_HOVER = 0xFF182233;
    private static final int COL_SLIDER_FILL = 0xFF337EE0;
    private static final int COL_SLIDER_FILL_HOVER = 0xFF4B9AF5;
    private static final int COL_TEXT_PRIMARY = 0xFFF0F4FA;
    private static final int COL_TEXT_SECONDARY = 0xFFA2B0C6;
    private static final int COL_TEXT_MUTED = 0xFF667387;
    private static final int COL_GROUP_TEXT = 0xFF7FB8FF;

    private static final int HEADER_H = 34;
    private static final int FOOTER_H = 30;
    private static final int GROUP_H = 20;
    private static final int ROW_H = 26;
    private static final int ROW_GAP = 3;

    private final Screen parent;
    private final boolean chinese;
    private final TerrainReliefSettings initialRelief;
    private final int initialMipmap;

    private BridgeSettings bridgeSettings;
    private BridgeQuality quality;
    private final BridgeGraphicsSettings graphics;
    private Map<String, String> effective;

    private Category activeCategory = Category.GENERAL;
    private boolean expertExpanded;
    private boolean manualPreview;
    private String searchQuery = "";
    private EditBox searchBox;

    private List<OptionEntry> allOptions = List.of();
    private final List<RowItem> visibleRows = new ArrayList<>();
    private OptionEntry focusedEntry;
    private OptionEntry hoveredEntry;

    // Scroll & drag state
    private double scrollOffset;
    private double maxScroll;
    private OptionEntry draggingSlider;
    private int dragTrackX;
    private int dragTrackW;
    private int draggingPreviewValue;
    private long lastLiveWriteNs;
    private boolean draggingScrollbar;
    private int tickCounter;

    // Layout metrics (computed in layout())
    private int navW;
    private int detailW;
    private int listX;
    private int listY;
    private int listW;
    private int listH;

    // Footer & header hit rectangles
    private int previewBtnX, previewBtnY, previewBtnW, previewBtnH;
    private int clearSearchX, clearSearchY, clearSearchW, clearSearchH;
    private int resetPageX, resetPageY, resetPageW, resetPageH;
    private int resetAllX, resetAllY, resetAllW, resetAllH;
    private int doneX, doneY, doneW, doneH;

    private record RowItem(boolean isHeader, String headerTitle, OptionEntry entry, int y, int height) {}

    private UnrealSettingsScreen(Screen parent, Minecraft minecraft, boolean chinese) {
        super(Component.literal(chinese ? "VoxGlint 渲染设置" : "VoxGlint Settings"));
        this.parent = parent;
        this.chinese = chinese;
        this.bridgeSettings = BridgeSettings.read();
        this.initialRelief = bridgeSettings.effectiveRelief();
        this.initialMipmap = minecraft.options.mipmapLevels().get();
        this.quality = BridgeQuality.read();
        this.graphics = BridgeGraphicsSettings.read();
        this.effective = BridgeGraphicsSettings.effective();
    }

    public static UnrealSettingsScreen create(Screen parent) {
        Minecraft minecraft = Minecraft.getInstance();
        boolean zh = minecraft.getLanguageManager().getSelected().equals("zh_cn");
        return new UnrealSettingsScreen(parent, minecraft, zh);
    }

    public static Button entry(Screen parent, Runnable applyVideoChanges) {
        Minecraft minecraft = Minecraft.getInstance();
        boolean zh = minecraft.getLanguageManager().getSelected().equals("zh_cn");
        return Button.builder(VoxGlintOptionCatalog.summaryButtonLabel(zh), button -> {
            if (applyVideoChanges != null) {
                applyVideoChanges.run();
            }
            minecraft.gui.setScreen(new UnrealSettingsScreen(parent, minecraft, zh));
        }).tooltip(Tooltip.create(Component.literal(zh
                ? "打开 VoxGlint（Unreal Engine）图形、光追、水体、天空、后处理与摄影设置"
                : "Open VoxGlint (Unreal Engine) graphics, ray tracing, water, sky, post-processing and photo settings")))
                .build();
    }

    @Override
    protected void init() {
        allOptions = VoxGlintOptionCatalog.build(this);
        computeLayout();

        int searchW = Math.clamp(width / 4, 110, 190);
        int searchX = Math.max(navW + 10, (width - searchW) / 2 - 20);
        int searchY = (HEADER_H - 18) / 2;
        searchBox = new EditBox(font, searchX, searchY, searchW, 18,
                Component.literal(chinese ? "搜索设置" : "Search settings"));
        searchBox.setMaxLength(48);
        searchBox.setBordered(true);
        searchBox.setHint(Component.literal(chinese ? "搜索设置（如：洞穴、云、焦散、光追）" : "Search (cave, cloud, water, rt...)"));
        searchBox.setValue(searchQuery);
        searchBox.setResponder(text -> {
            String next = text == null ? "" : text.strip();
            if (!next.equals(searchQuery)) {
                searchQuery = next;
                scrollOffset = 0;
                rebuildRows();
            }
        });
        addWidget(searchBox);
        rebuildRows();
    }

    private void computeLayout() {
        navW = width >= 540 ? 118 : 94;
        detailW = width >= 590 ? Math.min(204, Math.max(168, width / 4)) : 0;
        listX = navW + 8;
        listY = HEADER_H + 6;
        listW = Math.max(180, width - navW - detailW - 18);
        listH = Math.max(80, height - HEADER_H - FOOTER_H - 12);

        previewBtnW = chinese ? 62 : 68;
        previewBtnH = 18;
        previewBtnX = width - previewBtnW - 8;
        previewBtnY = (HEADER_H - previewBtnH) / 2;

        resetPageW = chinese ? 76 : 86;
        resetPageH = 20;
        resetPageX = 10;
        resetPageY = height - FOOTER_H + (FOOTER_H - resetPageH) / 2;

        resetAllW = chinese ? 88 : 94;
        resetAllH = 20;
        resetAllX = resetPageX + resetPageW + 6;
        resetAllY = resetPageY;

        doneW = 84;
        doneH = 20;
        doneX = width - doneW - 10;
        doneY = resetPageY;
    }

    private void rebuildRows() {
        visibleRows.clear();
        String query = searchQuery.toLowerCase(Locale.ROOT);
        boolean searching = !query.isEmpty();

        String lastGroup = null;
        int y = 0;
        for (OptionEntry entry : allOptions) {
            if (searching) {
                if (entry.kind == ControlKind.FOLD_TOGGLE) continue;
                if (!entry.matchesSearch(query)) continue;
            } else {
                if (entry.category != activeCategory) continue;
                if (entry.expertOnly && !expertExpanded) continue;
            }
            String groupTitle = searching
                    ? entry.category.title(chinese) + " · " + entry.group(chinese)
                    : entry.group(chinese);
            if (!groupTitle.equals(lastGroup)) {
                if (y > 0) y += 6;
                visibleRows.add(new RowItem(true, groupTitle, null, y, GROUP_H));
                y += GROUP_H + 2;
                lastGroup = groupTitle;
            }
            visibleRows.add(new RowItem(false, null, entry, y, ROW_H));
            y += ROW_H + ROW_GAP;
        }
        maxScroll = Math.max(0, y + 6 - listH);
        scrollOffset = Math.clamp(scrollOffset, 0, maxScroll);
        if (focusedEntry != null && visibleRows.stream().noneMatch(r -> r.entry == focusedEntry)) {
            focusedEntry = null;
        }
        if (focusedEntry == null) {
            for (RowItem r : visibleRows) {
                if (!r.isHeader) {
                    focusedEntry = r.entry;
                    break;
                }
            }
        }
    }

    private boolean isPreviewActive() {
        return minecraft.level != null && (manualPreview || (draggingSlider != null && draggingSlider.livePreview));
    }

    @Override
    public void tick() {
        if (++tickCounter % 10 == 0) {
            effective = BridgeGraphicsSettings.effective();
        }
    }

    @Override
    public void extractBackground(GuiGraphicsExtractor graphics, int mouseX, int mouseY, float partialTick) {
        if (minecraft.level == null) {
            super.extractBackground(graphics, mouseX, mouseY, partialTick);
            return;
        }
        if (isPreviewActive()) {
            // Keep the live Unreal world crystal clear behind the compact preview control.
            return;
        }
        graphics.fill(0, 0, width, height, COL_BACKDROP);
    }

    @Override
    public void extractRenderState(GuiGraphicsExtractor g, int mouseX, int mouseY, float partialTick) {
        if (isPreviewActive()) {
            renderPreviewMode(g, mouseX, mouseY, partialTick);
            return;
        }

        // Update hover target
        hoveredEntry = hitOptionAt(mouseX, mouseY);
        if (hoveredEntry != null) {
            focusedEntry = hoveredEntry;
        }

        renderHeader(g, mouseX, mouseY, partialTick);
        renderNav(g, mouseX, mouseY);
        renderOptionList(g, mouseX, mouseY);
        if (detailW > 0) {
            renderDetailPane(g);
        }
        renderFooter(g, mouseX, mouseY);
    }

    private void renderPreviewMode(GuiGraphicsExtractor g, int mouseX, int mouseY, float partialTick) {
        // Minimal floating bar so >=85% of the live Unreal 3D view is completely unobstructed.
        OptionEntry active = draggingSlider != null ? draggingSlider : focusedEntry;
        int sx;
        int sw;
        int cardW;
        int cx;
        if (draggingSlider != null && dragTrackW > 0) {
            // Keep the slider track horizontally locked to where the user grabbed it so dragging never jumps.
            sx = dragTrackX;
            sw = dragTrackW;
            cardW = Math.max(sw + 20, 200);
            cx = Math.clamp(sx - 10, 8, Math.max(8, width - cardW - 8));
            if (sx < cx + 6 || sx + sw > cx + cardW - 6) {
                cardW = Math.min(width - 16, Math.max(cardW, sw + 20));
                cx = Math.clamp(sx - 10, 8, Math.max(8, width - cardW - 8));
            }
        } else {
            cardW = Math.min(340, width - 24);
            cx = (width - cardW) / 2;
            sx = cx + 10;
            sw = cardW - 20;
        }
        int cardH = active != null ? 44 : 28;
        int cy = height - cardH - 12;

        g.fill(cx, cy, cx + cardW, cy + cardH, 0xCC0D121B);
        g.outline(cx, cy, cardW, cardH, COL_ACCENT);

        if (active != null && active.kind == ControlKind.SLIDER) {
            int val = draggingSlider == active ? draggingPreviewValue : active.getter.getAsInt();
            String label = font.plainSubstrByWidth(active.name(chinese), Math.max(40, cardW - 80));
            String valStr = active.formatter.apply(val);
            g.text(font, label, cx + 10, cy + 7, COL_TEXT_PRIMARY);
            g.text(font, valStr, cx + cardW - font.width(valStr) - 10, cy + 7, COL_ACCENT);

            int sy = cy + 23;
            int sh = 12;
            drawSliderTrack(g, sx, sy, sw, sh, active.min, active.max, val, true, true);
        } else {
            String msg = chinese ? "实时画面预览中 — 点击右上角或按 Esc 退出预览"
                    : "Live Preview — click top-right button or press Esc to return";
            g.centeredText(font, msg, cx + cardW / 2, cy + 10, COL_TEXT_PRIMARY);
        }

        // Floating exit-preview button top-right
        drawButton(g, previewBtnX, previewBtnY, previewBtnW, previewBtnH,
                chinese ? "退出预览" : "Exit Preview",
                inRect(mouseX, mouseY, previewBtnX, previewBtnY, previewBtnW, previewBtnH), true, true);
    }

    private void renderHeader(GuiGraphicsExtractor g, int mouseX, int mouseY, float partialTick) {
        g.fill(0, 0, width, HEADER_H, COL_BAR_BG);
        g.fill(0, HEADER_H - 1, width, HEADER_H, COL_BORDER);

        // Title & live path badge
        g.text(font, title, 10, 7, COL_TEXT_PRIMARY);
        String pathBadge;
        int badgeCol;
        if (effective.isEmpty()) {
            pathBadge = chinese ? "● 等待 UE 连接" : "● Waiting UE";
            badgeCol = 0xFFD9B44A;
        } else if ("1".equals(effective.get("HWRT"))) {
            pathBadge = chinese ? "● 硬件光追" : "● Hardware RT";
            badgeCol = 0xFF5CD68D;
        } else {
            pathBadge = chinese ? "● 软件兼容" : "● Software";
            badgeCol = 0xFF5CC8E6;
        }
        g.text(font, pathBadge, 10, 19, badgeCol);

        // Search box
        if (searchBox != null) {
            searchBox.extractRenderState(g, mouseX, mouseY, partialTick);
            if (!searchQuery.isEmpty()) {
                clearSearchW = 14;
                clearSearchH = 14;
                clearSearchX = searchBox.getX() + searchBox.getWidth() - clearSearchW - 2;
                clearSearchY = searchBox.getY() + 2;
                boolean hover = inRect(mouseX, mouseY, clearSearchX, clearSearchY, clearSearchW, clearSearchH);
                g.fill(clearSearchX, clearSearchY, clearSearchX + clearSearchW, clearSearchY + clearSearchH,
                        hover ? 0xFF33415C : 0xFF1F2A3D);
                g.centeredText(font, "×", clearSearchX + clearSearchW / 2, clearSearchY + 3, COL_TEXT_PRIMARY);
            } else {
                clearSearchW = 0;
            }
        }

        // Live FPS & internal resolution badge (left of Preview button)
        String telemetry = buildTelemetry();
        if (!telemetry.isEmpty()) {
            int tw = font.width(telemetry);
            int tx = previewBtnX - tw - 10;
            if (searchBox == null || tx > searchBox.getX() + searchBox.getWidth() + 8) {
                g.text(font, telemetry, tx, (HEADER_H - font.lineHeight) / 2, COL_TEXT_SECONDARY);
            }
        }

        // Preview toggle button
        boolean canPreview = minecraft.level != null;
        drawButton(g, previewBtnX, previewBtnY, previewBtnW, previewBtnH,
                chinese ? "预览画面" : "Preview",
                canPreview && inRect(mouseX, mouseY, previewBtnX, previewBtnY, previewBtnW, previewBtnH),
                canPreview, manualPreview);
    }

    private String buildTelemetry() {
        var parts = new ArrayList<String>(2);
        WorldBridge.DebugStats stats = WorldBridge.INSTANCE.debugStats();
        if (stats != null && stats.frameMs() > 0.01f) {
            parts.add(Math.round(1000.0f / stats.frameMs()) + " FPS");
        }
        String scaleRaw = effective.get("r.ScreenPercentage");
        if (scaleRaw != null && !"0".equals(scaleRaw)) {
            try {
                int pct = (int) Math.round(Double.parseDouble(scaleRaw));
                int w = Math.max(1, Math.round(minecraft.getWindow().getWidth() * pct / 100.0f));
                int h = Math.max(1, Math.round(minecraft.getWindow().getHeight() * pct / 100.0f));
                parts.add(w + "×" + h);
            } catch (NumberFormatException ignored) {}
        }
        return String.join(" · ", parts);
    }

    private void renderNav(GuiGraphicsExtractor g, int mouseX, int mouseY) {
        g.fill(0, HEADER_H, navW, height - FOOTER_H, COL_NAV_BG);
        g.fill(navW - 1, HEADER_H, navW, height - FOOTER_H, COL_BORDER);

        Category[] cats = Category.values();
        int itemH = 26;
        int y = HEADER_H + 8;
        boolean searching = !searchQuery.isEmpty();
        for (Category cat : cats) {
            boolean selected = !searching && cat == activeCategory;
            boolean hovered = inRect(mouseX, mouseY, 4, y, navW - 9, itemH);
            if (selected) {
                g.fill(4, y, navW - 5, y + itemH, COL_ACCENT_SOFT);
                g.fill(4, y + 3, 6, y + itemH - 3, COL_ACCENT);
            } else if (hovered) {
                g.fill(4, y, navW - 5, y + itemH, 0x3326344A);
            }

            boolean modified = hasModifiedInCategory(cat);
            String label = font.plainSubstrByWidth(cat.title(chinese), navW - (modified ? 24 : 16));
            g.text(font, label, 11, y + (itemH - font.lineHeight) / 2,
                    selected ? COL_TEXT_PRIMARY : (hovered ? 0xFFDCE6F5 : COL_TEXT_SECONDARY));
            if (modified) {
                int dotX = navW - 12;
                int dotY = y + itemH / 2 - 2;
                int dotCol = cat == Category.COLOR || cat == Category.MATERIALS ? COL_PREF_DOT : COL_ACCENT;
                g.fill(dotX, dotY, dotX + 4, dotY + 4, dotCol);
            }
            y += itemH + 3;
        }
    }

    private boolean hasModifiedInCategory(Category cat) {
        for (OptionEntry e : allOptions) {
            if (e.category == cat && e.kind != ControlKind.ACTION && e.kind != ControlKind.FOLD_TOGGLE
                    && Boolean.TRUE.equals(e.modifiedCheck.get())) {
                return true;
            }
        }
        return false;
    }

    private void renderOptionList(GuiGraphicsExtractor g, int mouseX, int mouseY) {
        g.enableScissor(listX - 2, listY, listX + listW + 8, listY + listH);

        if (visibleRows.isEmpty()) {
            String empty = chinese ? "没有找到匹配的设置项，请尝试其他关键词" : "No matching settings found";
            g.centeredText(font, empty, listX + listW / 2, listY + 32, COL_TEXT_MUTED);
        }

        int ctrlW = Math.clamp((int) (listW * 0.44), 106, 158);
        for (RowItem row : visibleRows) {
            int ry = listY + row.y - (int) Math.round(scrollOffset);
            if (ry + row.height < listY - 4 || ry > listY + listH + 4) {
                continue;
            }
            if (row.isHeader) {
                g.fill(listX, ry + 5, listX + 2, ry + row.height - 4, COL_ACCENT);
                g.text(font, row.headerTitle, listX + 7, ry + 6, COL_GROUP_TEXT);
                g.fill(listX + 7 + font.width(row.headerTitle) + 6, ry + 10, listX + listW - 6, ry + 11, 0x55253145);
                continue;
            }

            OptionEntry entry = row.entry;
            String disabled = entry.disabledReason.get();
            boolean active = disabled == null;
            boolean hovered = inRect(mouseX, mouseY, listX, ry, listW - 6, row.height)
                    && mouseY >= listY && mouseY <= listY + listH;
            boolean modified = Boolean.TRUE.equals(entry.modifiedCheck.get());

            int bg = !active ? COL_ROW_DISABLED : (hovered || entry == focusedEntry ? COL_ROW_HOVER : COL_ROW_BG);
            int border = entry == focusedEntry ? 0xFF3E6396 : (hovered && active ? 0xFF344866 : COL_BORDER);
            g.fill(listX, ry, listX + listW - 6, ry + row.height, bg);
            g.outline(listX, ry, listW - 6, row.height, border);

            // Modified indicator dot
            int textStart = listX + 9;
            if (modified) {
                int dotCol = entry.persistence == VoxGlintOptionCatalog.Persistence.KEPT_ON_PRESET
                        ? COL_PREF_DOT : COL_ACCENT;
                g.fill(listX + 4, ry + row.height / 2 - 2, listX + 7, ry + row.height / 2 + 2, dotCol);
                textStart = listX + 11;
            }

            int cx = listX + listW - 10 - ctrlW;
            int cy = ry + 4;
            int ch = row.height - 8;
            int maxNameW = Math.max(40, cx - textStart - 8);
            String titleStr = font.plainSubstrByWidth(entry.name(chinese), maxNameW);
            g.text(font, titleStr, textStart, ry + (row.height - font.lineHeight) / 2,
                    active ? COL_TEXT_PRIMARY : COL_TEXT_MUTED);

            boolean ctrlHover = active && inRect(mouseX, mouseY, cx, cy, ctrlW, ch);
            switch (entry.kind) {
                case CYCLE -> {
                    int val = entry.getter.getAsInt();
                    String formatted = font.plainSubstrByWidth(entry.formatter.apply(val), ctrlW - 10);
                    g.fill(cx, cy, cx + ctrlW, cy + ch, ctrlHover ? COL_CTRL_HOVER : COL_CTRL_BG);
                    g.outline(cx, cy, ctrlW, ch, ctrlHover ? COL_ACCENT : 0xFF2D3C54);
                    g.centeredText(font, formatted, cx + ctrlW / 2, cy + (ch - font.lineHeight) / 2,
                            active ? COL_TEXT_PRIMARY : COL_TEXT_MUTED);
                }
                case SLIDER -> {
                    int val = draggingSlider == entry ? draggingPreviewValue : entry.getter.getAsInt();
                    drawSliderTrack(g, cx, cy, ctrlW, ch, entry.min, entry.max, val, active, ctrlHover || draggingSlider == entry);
                    String formatted = font.plainSubstrByWidth(entry.formatter.apply(val), ctrlW - 8);
                    g.centeredText(font, formatted, cx + ctrlW / 2, cy + (ch - font.lineHeight) / 2,
                            active ? COL_TEXT_PRIMARY : COL_TEXT_MUTED);
                }
                case ACTION, FOLD_TOGGLE -> {
                    String btnTxt = entry.kind == ControlKind.FOLD_TOGGLE
                            ? entry.formatter.apply(entry.getter.getAsInt())
                            : (chinese ? "执行" : "Run");
                    drawButton(g, cx, cy, ctrlW, ch, btnTxt, ctrlHover, active,
                            entry.kind == ControlKind.FOLD_TOGGLE && expertExpanded);
                }
            }
        }

        g.disableScissor();

        // Scrollbar
        if (maxScroll > 0) {
            int barX = listX + listW - 3;
            g.fill(barX, listY, barX + 4, listY + listH, 0x551B2433);
            int totalH = (int) Math.round(listH + maxScroll);
            int thumbH = Math.max(24, (int) Math.round(listH * (double) listH / totalH));
            int thumbY = listY + (int) Math.round((listH - thumbH) * (scrollOffset / maxScroll));
            g.fill(barX, thumbY, barX + 4, thumbY + thumbH, draggingScrollbar ? COL_ACCENT : 0xFF475B7A);
        }
    }

    private void drawSliderTrack(GuiGraphicsExtractor g, int x, int y, int w, int h,
                                 int min, int max, int val, boolean active, boolean highlight) {
        g.fill(x, y, x + w, y + h, COL_CTRL_BG);
        double frac = max > min ? Math.clamp((val - min) / (double) (max - min), 0.0, 1.0) : 0.0;
        int fillW = (int) Math.round((w - 2) * frac);
        if (fillW > 0) {
            int fillCol = !active ? 0xFF283244 : (highlight ? COL_SLIDER_FILL_HOVER : COL_SLIDER_FILL);
            g.fill(x + 1, y + 1, x + 1 + fillW, y + h - 1, fillCol);
        }
        g.outline(x, y, w, h, highlight && active ? COL_ACCENT : 0xFF2D3C54);
        int knobX = x + 1 + Math.clamp(fillW - 1, 0, Math.max(0, w - 4));
        if (active) {
            g.fill(knobX, y + 1, knobX + 2, y + h - 1, 0xFFEAF2FF);
        }
    }

    private void renderDetailPane(GuiGraphicsExtractor g) {
        int px = width - detailW;
        g.fill(px, HEADER_H, width, height - FOOTER_H, COL_DETAIL_BG);
        g.fill(px, HEADER_H, px + 1, height - FOOTER_H, COL_BORDER);

        int pad = 10;
        int tx = px + pad;
        int tw = detailW - pad * 2;
        int y = HEADER_H + 10;

        OptionEntry entry = focusedEntry;
        if (entry == null) {
            g.text(font, activeCategory.title(chinese), tx, y, COL_TEXT_PRIMARY);
            y += 14;
            y = drawWrapped(g, chinese ? activeCategory.zhSubtitle : activeCategory.en, tx, y, tw, COL_TEXT_SECONDARY);
            return;
        }

        // Title
        y = drawWrapped(g, entry.name(chinese), tx, y, tw, COL_TEXT_PRIMARY);
        y += 6;

        // Impact & persistence badges
        String impactLbl = (chinese ? "开销：" : "Impact: ") + entry.impact.label(chinese);
        g.fill(tx, y, tx + 3, y + 10, entry.impact.color);
        g.text(font, impactLbl, tx + 7, y + 1, entry.impact.color);
        y += 15;

        String timingLbl = (chinese ? "生效：" : "Apply: ") + entry.timing.label(chinese);
        g.text(font, font.plainSubstrByWidth(timingLbl, tw), tx, y, COL_TEXT_SECONDARY);
        y += 12;

        String persistLbl = (chinese ? "切档：" : "Preset: ") + entry.persistence.label(chinese);
        g.text(font, font.plainSubstrByWidth(persistLbl, tw), tx, y,
                entry.persistence == VoxGlintOptionCatalog.Persistence.KEPT_ON_PRESET ? COL_PREF_DOT : COL_TEXT_SECONDARY);
        y += 14;

        g.fill(tx, y, tx + tw, y + 1, COL_BORDER);
        y += 8;

        // Description
        y = drawWrapped(g, entry.desc(chinese), tx, y, tw, 0xFFC5D0E0);
        y += 10;

        // Disabled reason
        String disabled = entry.disabledReason.get();
        if (disabled != null) {
            g.fill(tx, y, tx + tw, y + 1, 0x66E67E50);
            y += 6;
            y = drawWrapped(g, (chinese ? "已锁定：" : "Locked: ") + disabled, tx, y, tw, 0xFFF29C6B);
            y += 8;
        }

        // Reported value from UE
        String reported = entry.reportedValue.get();
        if (reported != null && !reported.isEmpty()) {
            g.fill(tx, y, tx + tw, y + 1, COL_BORDER);
            y += 6;
            y = drawWrapped(g, (chinese ? "UE 当前生效：" : "Unreal active: ") + reported, tx, y, tw, 0xFF88C8FF);
            y += 6;
        }

        // Shortcut hint at bottom of detail pane
        int hintY = height - FOOTER_H - 28;
        if (hintY > y + 8) {
            String hint = entry.livePreview
                    ? (chinese ? "提示：拖动滑块自动透明预览；右键单项重置" : "Tip: Drag slider for live preview; right-click to reset")
                    : (chinese ? "提示：右键任意选项或按 Home 可单项还原默认" : "Tip: Right-click any option or press Home to reset");
            drawWrapped(g, hint, tx, hintY, tw, COL_TEXT_MUTED);
        }
    }

    private int drawWrapped(GuiGraphicsExtractor g, String text, int x, int y, int maxWidth, int color) {
        List<FormattedCharSequence> lines = font.split(Component.literal(text), Math.max(40, maxWidth));
        for (FormattedCharSequence line : lines) {
            g.text(font, line, x, y, color);
            y += font.lineHeight + 2;
        }
        return y;
    }

    private void renderFooter(GuiGraphicsExtractor g, int mouseX, int mouseY) {
        int fy = height - FOOTER_H;
        g.fill(0, fy, width, height, COL_BAR_BG);
        g.fill(0, fy, width, fy + 1, COL_BORDER);

        drawButton(g, resetPageX, resetPageY, resetPageW, resetPageH,
                chinese ? "重置本页" : "Reset Page",
                inRect(mouseX, mouseY, resetPageX, resetPageY, resetPageW, resetPageH), true, false);
        drawButton(g, resetAllX, resetAllY, resetAllW, resetAllH,
                chinese ? "全部恢复默认" : "Reset All",
                inRect(mouseX, mouseY, resetAllX, resetAllY, resetAllW, resetAllH), true, false);

        // Status hint in middle (especially useful on narrow screens or when resource reload is pending)
        String footerHint = null;
        if (needsResourceReload()) {
            footerHint = chinese ? "● 关闭时将自动重载资源包" : "● Will reload resource packs on close";
        } else if (detailW == 0 && focusedEntry != null) {
            footerHint = focusedEntry.name(chinese) + " · " + focusedEntry.impact.label(chinese);
        }
        if (footerHint != null) {
            int hx = resetAllX + resetAllW + 10;
            int maxW = doneX - hx - 10;
            if (maxW > 60) {
                g.text(font, font.plainSubstrByWidth(footerHint, maxW), hx, fy + (FOOTER_H - font.lineHeight) / 2,
                        needsResourceReload() ? COL_PREF_DOT : COL_TEXT_SECONDARY);
            }
        }

        drawButton(g, doneX, doneY, doneW, doneH, CommonComponents.GUI_DONE.getString(),
                inRect(mouseX, mouseY, doneX, doneY, doneW, doneH), true, true);
    }

    private void drawButton(GuiGraphicsExtractor g, int x, int y, int w, int h,
                            String text, boolean hover, boolean active, boolean primary) {
        int bg;
        int border;
        if (!active) {
            bg = 0xFF141923;
            border = 0xFF202A3A;
        } else if (primary) {
            bg = hover ? 0xFF3987E8 : 0xFF2869BD;
            border = hover ? 0xFF74B8FF : 0xFF478EE8;
        } else {
            bg = hover ? 0xFF253248 : 0xFF192233;
            border = hover ? COL_ACCENT : 0xFF30405A;
        }
        g.fill(x, y, x + w, y + h, bg);
        g.outline(x, y, w, h, border);
        g.centeredText(font, font.plainSubstrByWidth(text, w - 8), x + w / 2, y + (h - font.lineHeight) / 2,
                active ? COL_TEXT_PRIMARY : COL_TEXT_MUTED);
    }

    // -------------------------------------------------------------------------
    // Input Handling
    // -------------------------------------------------------------------------
    @Override
    public boolean mouseClicked(MouseButtonEvent event, boolean doubleClick) {
        double mx = event.x();
        double my = event.y();
        int button = event.button();

        // Preview button works even in preview mode
        if (button == GLFW.GLFW_MOUSE_BUTTON_LEFT && minecraft.level != null
                && inRect(mx, my, previewBtnX, previewBtnY, previewBtnW, previewBtnH)) {
            manualPreview = !manualPreview;
            return true;
        }
        if (isPreviewActive()) {
            // Allow dragging the preview slider in manual preview mode too
            if (button == GLFW.GLFW_MOUSE_BUTTON_LEFT && focusedEntry != null && focusedEntry.kind == ControlKind.SLIDER) {
                int cardW = Math.min(340, width - 24);
                int cx = (width - cardW) / 2;
                int cy = height - 44 - 12;
                if (inRect(mx, my, cx + 10, cy + 20, cardW - 20, 18)) {
                    draggingSlider = focusedEntry;
                    dragTrackX = cx + 10;
                    dragTrackW = cardW - 20;
                    updateSliderDrag(mx, dragTrackX, dragTrackW, true);
                    return true;
                }
            }
            manualPreview = false;
            return true;
        }

        // Clear search button
        if (button == GLFW.GLFW_MOUSE_BUTTON_LEFT && clearSearchW > 0
                && inRect(mx, my, clearSearchX, clearSearchY, clearSearchW, clearSearchH)) {
            searchBox.setValue("");
            setFocused(null);
            return true;
        }

        // Search box
        if (searchBox != null && searchBox.isMouseOver(mx, my)) {
            setFocused(searchBox);
            return searchBox.mouseClicked(event, doubleClick);
        } else if (searchBox != null) {
            searchBox.setFocused(false);
        }

        // Left category navigation
        if (button == GLFW.GLFW_MOUSE_BUTTON_LEFT && mx >= 0 && mx < navW && my >= HEADER_H && my < height - FOOTER_H) {
            int y = HEADER_H + 8;
            int itemH = 26;
            for (Category cat : Category.values()) {
                if (my >= y && my <= y + itemH) {
                    activeCategory = cat;
                    if (!searchQuery.isEmpty()) {
                        searchQuery = "";
                        if (searchBox != null) searchBox.setValue("");
                    }
                    scrollOffset = 0;
                    rebuildRows();
                    return true;
                }
                y += itemH + 3;
            }
        }

        // Footer buttons
        if (button == GLFW.GLFW_MOUSE_BUTTON_LEFT) {
            if (inRect(mx, my, resetPageX, resetPageY, resetPageW, resetPageH)) {
                resetCurrentCategory();
                return true;
            }
            if (inRect(mx, my, resetAllX, resetAllY, resetAllW, resetAllH)) {
                resetAllSettings();
                return true;
            }
            if (inRect(mx, my, doneX, doneY, doneW, doneH)) {
                onClose();
                return true;
            }
        }

        // Scrollbar drag
        if (button == GLFW.GLFW_MOUSE_BUTTON_LEFT && maxScroll > 0
                && mx >= listX + listW - 5 && mx <= listX + listW + 4 && my >= listY && my <= listY + listH) {
            draggingScrollbar = true;
            updateScrollbarDrag(my);
            return true;
        }

        // Option rows
        RowItem hit = hitRowAt(mx, my);
        if (hit != null && !hit.isHeader) {
            OptionEntry entry = hit.entry;
            focusedEntry = entry;
            if (entry.disabledReason.get() != null) {
                return true;
            }
            if (button == GLFW.GLFW_MOUSE_BUTTON_RIGHT) {
                if (entry.resetter != null) {
                    entry.resetter.run();
                    rebuildRows();
                }
                return true;
            }
            if (button == GLFW.GLFW_MOUSE_BUTTON_LEFT) {
                int ctrlW = Math.clamp((int) (listW * 0.44), 106, 158);
                int cx = listX + listW - 10 - ctrlW;
                switch (entry.kind) {
                    case CYCLE -> {
                        cycleNext(entry, false);
                        return true;
                    }
                    case SLIDER -> {
                        if (mx >= cx - 4 && mx <= cx + ctrlW + 4) {
                            draggingSlider = entry;
                            dragTrackX = cx;
                            dragTrackW = ctrlW;
                            updateSliderDrag(mx, dragTrackX, dragTrackW, true);
                        }
                        return true;
                    }
                    case ACTION, FOLD_TOGGLE -> {
                        if (entry.action != null) {
                            entry.action.run();
                        }
                        return true;
                    }
                }
            }
        }

        return super.mouseClicked(event, doubleClick);
    }

    @Override
    public boolean mouseDragged(MouseButtonEvent event, double dx, double dy) {
        if (draggingScrollbar && maxScroll > 0) {
            updateScrollbarDrag(event.y());
            return true;
        }
        if (draggingSlider != null) {
            updateSliderDrag(event.x(), dragTrackX, dragTrackW, false);
            return true;
        }
        return super.mouseDragged(event, dx, dy);
    }

    @Override
    public boolean mouseReleased(MouseButtonEvent event) {
        if (draggingScrollbar) {
            draggingScrollbar = false;
            return true;
        }
        if (draggingSlider != null) {
            OptionEntry finished = draggingSlider;
            int finalVal = draggingPreviewValue;
            draggingSlider = null;
            finished.setter.accept(finalVal);
            return true;
        }
        return super.mouseReleased(event);
    }

    @Override
    public boolean mouseScrolled(double mouseX, double mouseY, double horizontal, double vertical) {
        if (!isPreviewActive() && maxScroll > 0 && mouseX >= navW && mouseX <= width - detailW) {
            scrollOffset = Math.clamp(scrollOffset - vertical * 24.0, 0, maxScroll);
            return true;
        }
        return super.mouseScrolled(mouseX, mouseY, horizontal, vertical);
    }

    @Override
    public boolean keyPressed(KeyEvent event) {
        if (event.key() == GLFW.GLFW_KEY_ESCAPE) {
            if (manualPreview) {
                manualPreview = false;
                return true;
            }
            if (searchBox != null && !searchQuery.isEmpty()) {
                searchBox.setValue("");
                searchBox.setFocused(false);
                return true;
            }
            onClose();
            return true;
        }
        if (searchBox != null && searchBox.isFocused()) {
            return searchBox.keyPressed(event);
        }
        if (focusedEntry != null && focusedEntry.disabledReason.get() == null) {
            if (event.key() == GLFW.GLFW_KEY_HOME && focusedEntry.resetter != null) {
                focusedEntry.resetter.run();
                rebuildRows();
                return true;
            }
            if (focusedEntry.kind == ControlKind.SLIDER && (event.key() == GLFW.GLFW_KEY_LEFT || event.key() == GLFW.GLFW_KEY_RIGHT)) {
                int cur = focusedEntry.getter.getAsInt();
                int dir = event.key() == GLFW.GLFW_KEY_LEFT ? -1 : 1;
                int next = Math.clamp(cur + dir * focusedEntry.step, focusedEntry.min, focusedEntry.max);
                if (next != cur) {
                    focusedEntry.setter.accept(next);
                }
                return true;
            }
            if (focusedEntry.kind == ControlKind.CYCLE && (event.key() == GLFW.GLFW_KEY_LEFT || event.key() == GLFW.GLFW_KEY_RIGHT)) {
                cycleNext(focusedEntry, event.key() == GLFW.GLFW_KEY_LEFT);
                return true;
            }
        }
        return super.keyPressed(event);
    }

    @Override
    public boolean charTyped(CharacterEvent event) {
        if (searchBox != null && searchBox.isFocused()) {
            return searchBox.charTyped(event);
        }
        return super.charTyped(event);
    }

    private void updateSliderDrag(double mouseX, int trackX, int trackW, boolean immediate) {
        OptionEntry e = draggingSlider;
        if (e == null) return;
        double frac = Math.clamp((mouseX - trackX) / Math.max(1.0, trackW), 0.0, 1.0);
        int raw = e.min + (int) Math.round(frac * (e.max - e.min));
        int stepped = e.min + Math.round((raw - e.min) / (float) e.step) * e.step;
        int clamped = Math.clamp(stepped, e.min, e.max);
        boolean changed = clamped != draggingPreviewValue;
        draggingPreviewValue = clamped;
        long now = System.nanoTime();
        // Throttle live disk writes while dragging so UE updates smoothly every ~100ms in real time.
        if (immediate || (changed && now - lastLiveWriteNs >= 85_000_000L)) {
            lastLiveWriteNs = now;
            e.setter.accept(clamped);
        }
    }

    private void updateScrollbarDrag(double mouseY) {
        double frac = Math.clamp((mouseY - listY) / Math.max(1.0, listH), 0.0, 1.0);
        scrollOffset = frac * maxScroll;
    }

    private void cycleNext(OptionEntry entry, boolean backward) {
        List<Integer> vals = entry.selectableValues(this);
        if (vals.isEmpty()) return;
        int cur = entry.getter.getAsInt();
        int idx = vals.indexOf(cur);
        if (idx < 0) idx = 0;
        int nextIdx = backward ? (idx - 1 + vals.size()) % vals.size() : (idx + 1) % vals.size();
        entry.setter.accept(vals.get(nextIdx));
    }

    private RowItem hitRowAt(double mx, double my) {
        if (mx < listX || mx > listX + listW - 6 || my < listY || my > listY + listH) {
            return null;
        }
        for (RowItem row : visibleRows) {
            int ry = listY + row.y - (int) Math.round(scrollOffset);
            if (my >= ry && my <= ry + row.height) {
                return row;
            }
        }
        return null;
    }

    private OptionEntry hitOptionAt(double mx, double my) {
        RowItem row = hitRowAt(mx, my);
        return row != null && !row.isHeader ? row.entry : null;
    }

    private boolean needsResourceReload() {
        return !bridgeSettings.effectiveRelief().equals(initialRelief)
                || minecraft.options.mipmapLevels().get() != initialMipmap;
    }

    private void resetCurrentCategory() {
        try {
            var settings = VoxGlintOptionCatalog.settingsForCategory(allOptions, activeCategory);
            if (!settings.isEmpty()) {
                graphics.clearSettings(settings);
            }
            if (activeCategory == Category.MATERIALS) {
                saveBridge(new BridgeSettings(bridgeSettings.hudMaxFps(), bridgeSettings.debugPanel(),
                        BridgeSettings.Toggle.STARTUP, TerrainReliefSettings.DEFAULT, true));
            } else if (activeCategory == Category.PHOTO_ADVANCED) {
                saveBridge(new BridgeSettings(0, BridgeSettings.Toggle.STARTUP,
                        bridgeSettings.gradientNormals(), bridgeSettings.relief(), bridgeSettings.connectedGlass()));
            }
            notifyInfo("已重置本页设置", "Reset settings on this page");
            rebuildRows();
        } catch (IOException e) {
            notifyError("重置失败", "Could not reset page", e);
        }
    }

    private void resetAllSettings() {
        try {
            BridgeQuality.High.save();
            quality = BridgeQuality.High;
            graphics.resetAllUserSettings();
            saveBridge(new BridgeSettings(0, BridgeSettings.Toggle.STARTUP,
                    BridgeSettings.Toggle.STARTUP, TerrainReliefSettings.DEFAULT, true));
            notifyInfo("已恢复全部默认设置", "Restored all defaults");
            rebuildRows();
        } catch (IOException e) {
            notifyError("重置失败", "Could not restore defaults", e);
        }
    }

    @Override
    public void onClose() {
        minecraft.options.save();
        minecraft.gui.setScreen(parent);
        if (needsResourceReload()) {
            minecraft.reloadResourcePacks();
        }
    }

    // -------------------------------------------------------------------------
    // VoxGlintOptionCatalog.Host Implementation
    // -------------------------------------------------------------------------
    @Override public Minecraft minecraft() { return minecraft; }
    @Override public boolean chinese() { return chinese; }
    @Override public BridgeQuality quality() { return quality; }
    @Override public void setQuality(BridgeQuality quality) { this.quality = quality; }
    @Override public BridgeGraphicsSettings graphics() { return graphics; }
    @Override public BridgeSettings bridge() { return bridgeSettings; }
    @Override public void setBridge(BridgeSettings bridge) { this.bridgeSettings = bridge; }
    @Override public Map<String, String> effective() { return effective; }
    @Override public boolean expertExpanded() { return expertExpanded; }
    @Override public void setExpertExpanded(boolean expanded) { this.expertExpanded = expanded; }

    @Override
    public void saveQuality(BridgeQuality next, boolean clearOverrides) {
        try {
            next.save();
            quality = next;
            if (clearOverrides) {
                graphics.clearQualityOverrides();
            }
            rebuildRows();
        } catch (IOException e) {
            notifyError("无法保存画质预设", "Could not save quality preset", e);
        }
    }

    @Override
    public void saveGraphics(Map<BridgeGraphicsSettings.Setting, Integer> updates) {
        try {
            graphics.changeAll(updates);
        } catch (IOException e) {
            notifyError("无法保存图形设置", "Could not save graphics settings", e);
        }
    }

    @Override
    public void saveBridge(BridgeSettings next) {
        try {
            next.save();
            bridgeSettings = next;
        } catch (IOException e) {
            notifyError("无法保存设置", "Could not save settings", e);
        }
    }

    @Override
    public void saveVanillaOptions() {
        minecraft.options.save();
    }

    @Override
    public void notifyInfo(String zh, String en) {
        SystemToast.addOrUpdate(minecraft.gui.toastManager(), TOAST_ID, title,
                Component.literal(chinese ? zh : en));
    }

    @Override
    public void notifyError(String zh, String en, Throwable error) {
        LOGGER.warn(en, error);
        SystemToast.addOrUpdate(minecraft.gui.toastManager(), TOAST_ID, title,
                Component.literal(chinese ? zh : en));
    }

    @Override
    public void closeForPhoto() {
        minecraft.options.save();
        // Close all screens back to live world so the camera view is clean for path tracing.
        minecraft.gui.setScreen(null);
    }

    @Override
    public void rebuildList() {
        rebuildRows();
    }

    private static boolean inRect(double mx, double my, int x, int y, int w, int h) {
        return mx >= x && mx < x + w && my >= y && my < y + h;
    }
}
