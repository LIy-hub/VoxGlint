package dev.minecraftue.bridge;

import net.minecraft.client.InactivityFpsLimit;
import net.minecraft.client.Minecraft;
import net.minecraft.client.TextureFilteringMethod;
import net.minecraft.network.chat.Component;
import net.minecraft.util.Util;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.function.IntConsumer;
import java.util.function.IntFunction;
import java.util.function.IntSupplier;
import java.util.function.Supplier;

import static dev.minecraftue.bridge.BridgeGraphicsSettings.FOLLOW;
import static dev.minecraftue.bridge.BridgeGraphicsSettings.Setting;

/** Declarative catalog of all VoxGlint settings organized into player-friendly categories. */
final class VoxGlintOptionCatalog {

    enum Category {
        GENERAL("概览与显示", "General & Display", "总画质、超分辨率、动态帧率与基础显示"),
        LIGHTING("光照与阴影", "Lighting & Shadows", "Lumen 全局光照、反射、阴影与方块点光源"),
        SKY("天空与天气", "Sky & Weather", "三层体积云、大气雾、雨雪与地面积水反光"),
        WATER("水体", "Water", "P1/P2/P3 水体物理光学、焦散、波浪与清澈度"),
        COLOR("色彩与后处理", "Color & Post", "曝光、洞穴保底亮度、调色、泛光与镜头效果"),
        MATERIALS("材质与表面", "Materials & Surface", "自动法线凹凸、无缝玻璃与纹理过滤"),
        PHOTO_ADVANCED("摄影与高级", "Photo & Advanced", "路径追踪摄影、监控浮窗、配置档案与专家微调");

        final String zh;
        final String en;
        final String zhSubtitle;

        Category(String zh, String en, String zhSubtitle) {
            this.zh = zh;
            this.en = en;
            this.zhSubtitle = zhSubtitle;
        }

        String title(boolean chinese) {
            return chinese ? zh : en;
        }
    }

    enum Impact {
        NONE("无损耗", "Negligible", 0xFF5CD68D),
        LOW("低开销", "Low", 0xFF5CC8E6),
        MEDIUM("中开销", "Medium", 0xFFF0C95A),
        HIGH("高开销", "High", 0xFFF08B49),
        EXTREME("极高开销", "Very High", 0xFFE65C5C);

        final String zh;
        final String en;
        final int color;

        Impact(String zh, String en, int color) {
            this.zh = zh;
            this.en = en;
            this.color = color;
        }

        String label(boolean chinese) {
            return chinese ? zh : en;
        }
    }

    enum ApplyTiming {
        INSTANT("实时生效", "Applies immediately"),
        RELOAD_WORLD("自动刷新区块", "Refreshes terrain automatically"),
        RELOAD_PACKS("退出时重载资源包", "Reloads resources on close"),
        RESTART_UE("重启渲染器生效", "Requires renderer restart");

        final String zh;
        final String en;

        ApplyTiming(String zh, String en) {
            this.zh = zh;
            this.en = en;
        }

        String label(boolean chinese) {
            return chinese ? zh : en;
        }
    }

    enum Persistence {
        PRESET_CONTROLLED("跟随总画质预设", "Reset when switching preset"),
        KEPT_ON_PRESET("切画质档保留", "Kept across preset switches"),
        VANILLA_SYNC("同步至 MC 设置", "Synced with Minecraft options"),
        ACTION("操作", "Action");

        final String zh;
        final String en;

        Persistence(String zh, String en) {
            this.zh = zh;
            this.en = en;
        }

        String label(boolean chinese) {
            return chinese ? zh : en;
        }
    }

    enum ControlKind {
        CYCLE,
        SLIDER,
        ACTION,
        FOLD_TOGGLE
    }

    interface Host {
        Minecraft minecraft();
        boolean chinese();
        BridgeQuality quality();
        void setQuality(BridgeQuality quality);
        BridgeGraphicsSettings graphics();
        BridgeSettings bridge();
        void setBridge(BridgeSettings bridge);
        Map<String, String> effective();
        boolean expertExpanded();
        void setExpertExpanded(boolean expanded);
        void saveQuality(BridgeQuality next, boolean clearOverrides);
        void saveGraphics(Map<Setting, Integer> updates);
        void saveBridge(BridgeSettings next);
        void saveVanillaOptions();
        void notifyInfo(String zh, String en);
        void notifyError(String zh, String en, Throwable error);
        void closeForPhoto();
        void rebuildList();
    }

    static final class OptionEntry {
        final String id;
        final Category category;
        final String groupZh;
        final String groupEn;
        final String nameZh;
        final String nameEn;
        final String descZh;
        final String descEn;
        final String keywords;
        final Impact impact;
        final ApplyTiming timing;
        final Persistence persistence;
        final ControlKind kind;
        final boolean expertOnly;
        final boolean livePreview;
        final Set<Setting> backingSettings;
        final String effectiveKey;

        // Slider / cycle bindings
        final int min;
        final int max;
        final int step;
        final List<Integer> cycleValues;
        final IntSupplier getter;
        final IntConsumer setter;
        final IntFunction<String> formatter;
        final Supplier<Boolean> modifiedCheck;
        final Runnable resetter;
        final Supplier<String> disabledReason;
        final Supplier<String> reportedValue;
        final Runnable action;

        OptionEntry(String id, Category category, String groupZh, String groupEn,
                    String nameZh, String nameEn, String descZh, String descEn, String keywords,
                    Impact impact, ApplyTiming timing, Persistence persistence, ControlKind kind,
                    boolean expertOnly, boolean livePreview, Set<Setting> backingSettings, String effectiveKey,
                    int min, int max, int step, List<Integer> cycleValues,
                    IntSupplier getter, IntConsumer setter, IntFunction<String> formatter,
                    Supplier<Boolean> modifiedCheck, Runnable resetter,
                    Supplier<String> disabledReason, Supplier<String> reportedValue, Runnable action) {
            this.id = id;
            this.category = category;
            this.groupZh = groupZh;
            this.groupEn = groupEn;
            this.nameZh = nameZh;
            this.nameEn = nameEn;
            this.descZh = descZh;
            this.descEn = descEn;
            this.keywords = keywords.toLowerCase(Locale.ROOT);
            this.impact = impact;
            this.timing = timing;
            this.persistence = persistence;
            this.kind = kind;
            this.expertOnly = expertOnly;
            this.livePreview = livePreview;
            this.backingSettings = backingSettings;
            this.effectiveKey = effectiveKey;
            this.min = min;
            this.max = max;
            this.step = step;
            this.cycleValues = cycleValues;
            this.getter = getter;
            this.setter = setter;
            this.formatter = formatter;
            this.modifiedCheck = modifiedCheck;
            this.resetter = resetter;
            this.disabledReason = disabledReason;
            this.reportedValue = reportedValue;
            this.action = action;
        }

        String name(boolean zh) { return zh ? nameZh : nameEn; }
        String desc(boolean zh) { return zh ? descZh : descEn; }
        String group(boolean zh) { return zh ? groupZh : groupEn; }

        List<Integer> selectableValues(Host host) {
            if (!backingSettings.contains(Setting.FRAME_GEN_MULTIPLIER)) return cycleValues;
            int mask = effectiveInt(host, "FrameGenModes", 0);
            List<Integer> supported = new ArrayList<>(cycleValues);
            for (int mode = 2; mode <= Setting.FRAME_GEN_MULTIPLIER.max; mode++) {
                if ((mask & (1 << mode)) != 0) supported.add(mode);
            }
            return supported;
        }

        boolean matchesSearch(String queryLower) {
            if (queryLower.isEmpty()) return true;
            return nameZh.toLowerCase(Locale.ROOT).contains(queryLower)
                    || nameEn.toLowerCase(Locale.ROOT).contains(queryLower)
                    || groupZh.toLowerCase(Locale.ROOT).contains(queryLower)
                    || groupEn.toLowerCase(Locale.ROOT).contains(queryLower)
                    || descZh.toLowerCase(Locale.ROOT).contains(queryLower)
                    || descEn.toLowerCase(Locale.ROOT).contains(queryLower)
                    || keywords.contains(queryLower);
        }
    }

    private VoxGlintOptionCatalog() {}

    static List<OptionEntry> build(Host host) {
        List<OptionEntry> list = new ArrayList<>(64);
        addGeneral(list, host);
        addLighting(list, host);
        addSky(list, host);
        addWater(list, host);
        addColor(list, host);
        addMaterials(list, host);
        addPhotoAndAdvanced(list, host);
        return list;
    }

    // -------------------------------------------------------------------------
    // 1. GENERAL & DISPLAY
    // -------------------------------------------------------------------------
    private static void addGeneral(List<OptionEntry> list, Host h) {
        String g1Zh = "画质与光追", g1En = "Quality & Ray Tracing";
        list.add(cycle("quality_preset", Category.GENERAL, g1Zh, g1En,
                "总画质预设", "Quality Preset",
                "一键切换整套性能与画质档位。切换时会重置性能画质类覆盖项，但会保留你的曝光、调色、水体清澈度等视觉偏好。",
                "Switches the overall rendering preset. Resets performance/quality overrides while keeping your visual preferences (exposure, color, water clarity, etc.).",
                "preset quality tier performance balanced high ultra 性能 均衡 高 极高 画质 预设",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false, Set.of(), null,
                List.of(0, 1, 2, 3),
                () -> presetIndex(h.quality()),
                v -> h.saveQuality(presetFromIndex(v), true),
                v -> {
                    String base = qualityName(presetFromIndex(v), h.chinese());
                    if (h.graphics().hasQualityOrExpertOverrides()) {
                        return h.chinese() ? "自定义（基于 " + base + "）" : "Custom (" + base + ")";
                    }
                    return base;
                },
                () -> h.graphics().hasQualityOrExpertOverrides(),
                () -> h.saveQuality(BridgeQuality.High, true),
                () -> null,
                () -> reportEffectiveQuality(h)));

        list.add(cycle("hardware_rt", Category.GENERAL, g1Zh, g1En,
                "硬件光线追踪", "Hardware Ray Tracing",
                "使用 DX12 SM6 硬件光追运行 Lumen 命中点全局光照、硬件反射与 MegaLights。关闭后切至兼容的屏幕空间反射与原版补光。",
                "Enables DX12 SM6 hardware ray tracing for Lumen Hit Lighting and MegaLights. Turning off switches to software SSR and Minecraft vertex lighting.",
                "hwrt raytracing lumen 光追 硬件 软件 回退 兼容",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.HARDWARE_LIGHTING), Setting.HARDWARE_LIGHTING.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.HARDWARE_LIGHTING),
                v -> h.saveGraphics(Map.of(Setting.HARDWARE_LIGHTING, v)),
                v -> v == FOLLOW ? follow(h) : v == 1 ? txt(h, "开启（硬件光追）", "On (Hardware RT)") : txt(h, "关闭（软件兼容）", "Off (Software)"),
                () -> h.graphics().isOverridden(Setting.HARDWARE_LIGHTING),
                () -> h.saveGraphics(Map.of(Setting.HARDWARE_LIGHTING, FOLLOW)),
                () -> isRayTracingUnavailable(h) ? txt(h, "当前渲染器或显卡不支持 DX12 硬件光追", "Hardware ray tracing unavailable on current renderer/GPU") : null,
                () -> reportHwrt(h)));

        // Built-in quick hardware profile selector (action cycle)
        final int[] selectedBuiltin = {0};
        list.add(cycle("builtin_profile_pick", Category.GENERAL, g1Zh, g1En,
                "硬件优化模板", "Hardware Tuning Template",
                "选择一套针对不同显卡实测打磨好的推荐方案，点击下方「应用所选模板」一键套用。",
                "Choose a curated hardware tuning template, then click Apply Template below.",
                "rtx 3060 profile preset template 显卡 模板 推荐 优化",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.ACTION, false, Set.of(), null,
                List.of(0, 1, 2, 3),
                () -> selectedBuiltin[0],
                v -> selectedBuiltin[0] = v,
                v -> {
                    var p = VoxGlintProfiles.BUILTINS.get(Math.clamp(v, 0, VoxGlintProfiles.BUILTINS.size() - 1));
                    return h.chinese() ? p.zhName() : p.enName();
                },
                () -> false, () -> {}, () -> null,
                () -> {
                    var p = VoxGlintProfiles.BUILTINS.get(Math.clamp(selectedBuiltin[0], 0, VoxGlintProfiles.BUILTINS.size() - 1));
                    return h.chinese() ? p.zhDesc() : p.enDesc();
                }));

        list.add(action("builtin_profile_apply", Category.GENERAL, g1Zh, g1En,
                "应用所选优化模板", "Apply Selected Template",
                "将上方选中的推荐硬件方案写入当前配置（保留你的亮度、调色、水体清澈度等个人偏好）。",
                "Applies the selected hardware tuning template while keeping your personal visual preferences.",
                "apply template profile 模板 应用 推荐",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.ACTION, false,
                () -> {
                    var p = VoxGlintProfiles.BUILTINS.get(Math.clamp(selectedBuiltin[0], 0, VoxGlintProfiles.BUILTINS.size() - 1));
                    try {
                        BridgeQuality q = VoxGlintProfiles.applyBuiltin(p, h.graphics());
                        h.setQuality(q);
                        h.notifyInfo("已应用模板：" + p.zhName(), "Applied template: " + p.enName());
                        h.rebuildList();
                    } catch (IOException e) {
                        h.notifyError("应用模板失败", "Failed to apply template", e);
                    }
                }, () -> null));

        String g2Zh = "清晰度与超分辨率", g2En = "Resolution & Anti-Aliasing";
        Supplier<String> engineResolutionOnly = () -> h.graphics().isOverridden(Setting.UPSCALER)
                ? txt(h, "当前由第三方超分控制", "Controlled by the selected upscaler") : null;
        Supplier<String> manualScaleDisabled = () -> {
            String reason = engineResolutionOnly.get();
            if (reason != null) return reason;
            return h.graphics().get(Setting.DYNAMIC_RES) == 1
                    ? txt(h, "动态分辨率已接管渲染比例", "Controlled by Dynamic Resolution") : null;
        };
        list.add(cycle("temporal_upscaler", Category.GENERAL, g2Zh, g2En,
                "超分算法", "Upscaler",
                "选择 UE 自带方案或 DLSS、FSR、XeSS。所选算法不可用时使用 TSR。",
                "Select the Unreal renderer, DLSS, FSR or XeSS. Unavailable choices fall back to TSR.",
                "upscaler dlss fsr xess tsr 超分 算法",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.UPSCALER), Setting.UPSCALER.effectiveKey, List.of(FOLLOW, 1, 2, 3),
                () -> h.graphics().get(Setting.UPSCALER), v -> h.saveGraphics(Map.of(Setting.UPSCALER, v)),
                v -> switch (v) {
                    case 1 -> "DLSS";
                    case 2 -> "FSR";
                    case 3 -> "XeSS";
                    default -> txt(h, "UE 自带", "Unreal");
                },
                () -> h.graphics().isOverridden(Setting.UPSCALER),
                () -> h.saveGraphics(Map.of(Setting.UPSCALER, FOLLOW)),
                () -> null, () -> h.effective().get("RenderedUpscaler")));

        list.add(cycle("temporal_upscaler_quality", Category.GENERAL, g2Zh, g2En,
                "第三方超分质量", "Third-Party Upscaling Quality",
                "按所选算法的质量、均衡或性能档设定渲染比例。",
                "Uses the selected upscaler's Quality, Balanced or Performance scale.",
                "upscaler quality dlss fsr xess 超分 质量",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.UPSCALER_QUALITY), Setting.UPSCALER_QUALITY.effectiveKey, List.of(FOLLOW, 1, 2),
                () -> h.graphics().get(Setting.UPSCALER_QUALITY),
                v -> h.saveGraphics(Map.of(Setting.UPSCALER_QUALITY, v)),
                v -> switch (v) {
                    case 1 -> txt(h, "均衡", "Balanced");
                    case 2 -> txt(h, "性能", "Performance");
                    default -> txt(h, "质量", "Quality");
                },
                () -> h.graphics().isOverridden(Setting.UPSCALER_QUALITY),
                () -> h.saveGraphics(Map.of(Setting.UPSCALER_QUALITY, FOLLOW)),
                () -> h.graphics().isOverridden(Setting.UPSCALER) ? null
                        : txt(h, "先选择第三方超分算法", "Select a third-party upscaler first"),
                () -> reportRenderScale(h)));

        list.add(cycle("aa_method", Category.GENERAL, g2Zh, g2En,
                "抗锯齿与超分方案", "Anti-Aliasing Method",
                "TSR 是 UE5 默认的时序超分辨率，低渲染比例下最清晰稳定；FXAA 与关闭适合追求极低延迟或原生像素锐利感。",
                "TSR is UE5's temporal super resolution (recommended). TAA/FXAA/Off reduce overhead or offer crisp raw pixels.",
                "tsr taa fxaa aa anti aliasing 抗锯齿 超分 模糊 锯齿",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.AA_METHOD), Setting.AA_METHOD.effectiveKey,
                List.of(FOLLOW, 4, 2, 1, 0),
                () -> h.graphics().get(Setting.AA_METHOD),
                v -> h.saveGraphics(Map.of(Setting.AA_METHOD, v)),
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 4 -> txt(h, "TSR（推荐）", "TSR (Recommended)");
                    case 2 -> "TAA";
                    case 1 -> "FXAA";
                    default -> off(h);
                },
                () -> h.graphics().isOverridden(Setting.AA_METHOD),
                () -> h.saveGraphics(Map.of(Setting.AA_METHOD, FOLLOW)),
                engineResolutionOnly,
                () -> reportAa(h)));

        list.add(cycle("upscale_tier", Category.GENERAL, g2Zh, g2En,
                "超分辨率质量档", "Upscaling Quality",
                "快速选择内部渲染比例档位。降低比例可显著提升帧率，配合 TSR 与锐化可保持清晰观感。",
                "Quick presets for internal rendering resolution. Lower scales substantially boost GPU performance.",
                "upscale resolution tsr scale dlss fsr 分辨率 超分 清晰度 帧率",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.RENDER_SCALE), Setting.RENDER_SCALE.effectiveKey,
                List.of(FOLLOW, 100, 85, 75, 67, 58),
                () -> {
                    int s = h.graphics().get(Setting.RENDER_SCALE);
                    if (s == FOLLOW || s == 100 || s == 85 || s == 75 || s == 67 || s == 58) return s;
                    return 75;
                },
                v -> h.saveGraphics(Map.of(Setting.RENDER_SCALE, v)),
                v -> {
                    int raw = h.graphics().get(Setting.RENDER_SCALE);
                    if (raw != FOLLOW && raw != 100 && raw != 85 && raw != 75 && raw != 67 && raw != 58) {
                        return txt(h, "自定义 (" + raw + "%)", "Custom (" + raw + "%)");
                    }
                    return switch (v) {
                        case FOLLOW -> follow(h);
                        case 100 -> txt(h, "原生 (100%)", "Native (100%)");
                        case 85 -> txt(h, "超质量 (85%)", "Ultra Quality (85%)");
                        case 75 -> txt(h, "质量 (75%)", "Quality (75%)");
                        case 67 -> txt(h, "均衡 (67%)", "Balanced (67%)");
                        default -> txt(h, "性能 (58%)", "Performance (58%)");
                    };
                },
                () -> h.graphics().isOverridden(Setting.RENDER_SCALE),
                () -> h.saveGraphics(Map.of(Setting.RENDER_SCALE, FOLLOW)),
                manualScaleDisabled,
                () -> reportRenderScale(h)));

        list.add(slider("render_scale_custom", Category.GENERAL, g2Zh, g2En,
                "自定义渲染比例", "Custom Render Scale",
                "精细控制内部渲染分辨率百分比（50%–100%）。100% 为原生分辨率，降低可明显降低 GPU 负载。",
                "Fine-grained internal rendering resolution percentage (50%-100%). 100% is native resolution.",
                "screen percentage scale resolution 渲染比例 分辨率",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false, false,
                Set.of(Setting.RENDER_SCALE), Setting.RENDER_SCALE.effectiveKey,
                50, 100, 1,
                () -> h.graphics().getOrDefault(Setting.RENDER_SCALE, effectiveInt(h, "r.ScreenPercentage", 75)),
                v -> h.saveGraphics(Map.of(Setting.RENDER_SCALE, v)),
                v -> v + "%",
                () -> h.graphics().isOverridden(Setting.RENDER_SCALE),
                () -> h.saveGraphics(Map.of(Setting.RENDER_SCALE, FOLLOW)),
                manualScaleDisabled,
                () -> reportRenderScale(h)));

        list.add(cycle("dynamic_res", Category.GENERAL, g2Zh, g2En,
                "动态分辨率", "Dynamic Resolution",
                "开启后，UE 会根据当前场景压力自动上下微调渲染比例以维持目标帧率，避免复杂建筑或暴雨中掉帧。",
                "Automatically adjusts internal resolution to maintain your target frame rate in heavy scenes.",
                "drs dynamic resolution fps 稳定 帧率 动态分辨率",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.DYNAMIC_RES), Setting.DYNAMIC_RES.effectiveKey,
                List.of(FOLLOW, 0, 1),
                () -> h.graphics().get(Setting.DYNAMIC_RES),
                v -> h.saveGraphics(Map.of(Setting.DYNAMIC_RES, v)),
                v -> v == FOLLOW ? txt(h, "默认（关）", "Default (Off)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.DYNAMIC_RES),
                () -> h.saveGraphics(Map.of(Setting.DYNAMIC_RES, FOLLOW)),
                engineResolutionOnly,
                () -> reportRawOnOff(h, "r.DynamicRes.OperationMode")));

        list.add(slider("dynamic_res_fps", Category.GENERAL, g2Zh, g2En,
                "动态分辨率目标帧率", "Dynamic Res Target FPS",
                "动态分辨率开启时力求维持的目标帧率（30–240 FPS）。",
                "Target frame rate budget used by Dynamic Resolution (30-240 FPS).",
                "target fps budget dynamic resolution 目标帧率 动态分辨率",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false, false,
                Set.of(Setting.DYNAMIC_RES_TARGET_FPS), Setting.DYNAMIC_RES_TARGET_FPS.effectiveKey,
                30, 240, 5,
                () -> h.graphics().getOrDefault(Setting.DYNAMIC_RES_TARGET_FPS, 60),
                v -> h.saveGraphics(Map.of(Setting.DYNAMIC_RES_TARGET_FPS, v)),
                v -> v + " FPS",
                () -> h.graphics().isOverridden(Setting.DYNAMIC_RES_TARGET_FPS),
                () -> h.saveGraphics(Map.of(Setting.DYNAMIC_RES_TARGET_FPS, FOLLOW)),
                () -> h.graphics().get(Setting.DYNAMIC_RES) != 1
                        ? txt(h, "需先开启「动态分辨率」", "Requires Dynamic Resolution enabled") : null,
                () -> null));

        list.add(slider("sharpen", Category.GENERAL, g2Zh, g2En,
                "画面锐化", "Image Sharpening",
                "提升方块边缘与材质纹理的清晰度。使用 TSR 低渲染比例时建议设为 25%–60%，过高会产生过锐白边。",
                "Enhances edge and texture crispness. 25%-60% pairs well with TSR upscaling.",
                "sharpen cas clarity crisp 锐化 清晰 模糊",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.SHARPEN), Setting.SHARPEN.effectiveKey,
                0, 200, 5,
                () -> h.graphics().getOrDefault(Setting.SHARPEN, 0),
                v -> h.saveGraphics(Map.of(Setting.SHARPEN, v)),
                v -> v == 0 ? off(h) : v + "%",
                () -> h.graphics().isOverridden(Setting.SHARPEN),
                () -> h.saveGraphics(Map.of(Setting.SHARPEN, FOLLOW)),
                () -> null,
                () -> reportPercentFloat(h, "r.Tonemapper.Sharpen")));

        // Synced vanilla display controls
        String fgZh = "帧生成", fgEn = "Frame Generation";
        list.add(cycle("frame_gen_backend", Category.GENERAL, fgZh, fgEn,
                "帧生成后端", "Frame Generation Backend",
                "重启渲染器后切换帧生成后端。兼容性由显卡和驱动决定。",
                "Switches the backend on renderer restart. Availability depends on the GPU and driver.",
                "frame generation fsr xess dlss 插帧",
                Impact.MEDIUM, ApplyTiming.RESTART_UE, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.FRAME_GEN_BACKEND), Setting.FRAME_GEN_BACKEND.effectiveKey, List.of(FOLLOW, 1, 3),
                () -> h.graphics().get(Setting.FRAME_GEN_BACKEND),
                v -> h.saveGraphics(Map.of(Setting.FRAME_GEN_BACKEND, v)),
                v -> switch (v) {
                    case 1 -> "XeSS";
                    case 3 -> "DLSS";
                    default -> "FSR";
                },
                () -> h.graphics().isOverridden(Setting.FRAME_GEN_BACKEND),
                () -> h.saveGraphics(Map.of(Setting.FRAME_GEN_BACKEND, FOLLOW)),
                () -> null, () -> h.effective().get("FrameGenBackend")));

        list.add(cycle("frame_gen_multiplier", Category.GENERAL, fgZh, fgEn,
                "帧生成倍数", "Frame Generation Multiplier",
                "倍数按当前后端和设备上报的能力显示。默认请求 2×。",
                "Offers multipliers reported by the current backend and GPU. Requests 2x by default.",
                "frame generation multiplier 插帧 倍数",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.FRAME_GEN_MULTIPLIER), Setting.FRAME_GEN_MULTIPLIER.effectiveKey, List.of(FOLLOW, 0),
                () -> h.graphics().get(Setting.FRAME_GEN_MULTIPLIER),
                v -> h.saveGraphics(Map.of(Setting.FRAME_GEN_MULTIPLIER, v)),
                v -> frameMultiplierLabel(h, v),
                () -> h.graphics().isOverridden(Setting.FRAME_GEN_MULTIPLIER),
                () -> h.saveGraphics(Map.of(Setting.FRAME_GEN_MULTIPLIER, FOLLOW)),
                () -> null, () -> {
                    int value = effectiveInt(h, "FrameGenMultiplier", FOLLOW);
                    return value == FOLLOW ? null : frameMultiplierLabel(h, value);
                }));

        String g3Zh = "基础显示（与 MC 实时同步）", g3En = "Display (Synced with Minecraft)";
        var opts = h.minecraft().options;
        list.add(slider("vanilla_view_dist", Category.GENERAL, g3Zh, g3En,
                "渲染距离（视距）", "Render Distance",
                "同步 Minecraft 原版视距（2–32 区块）。调高将加载更多地形区块并显著增加显存和光追构建开销。",
                "Synced with Minecraft Render Distance (2-32 chunks). Higher values load more terrain and use more VRAM.",
                "view render distance chunks 视距 区块 渲染距离",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.VANILLA_SYNC, false, false, Set.of(), null,
                2, BridgeWire.MAX_VIEW_DISTANCE, 1,
                () -> opts.renderDistance().get(),
                v -> { opts.renderDistance().set(v); h.saveVanillaOptions(); },
                v -> h.chinese() ? v + " 区块" : v + " chunks",
                () -> false, () -> { opts.renderDistance().set(12); h.saveVanillaOptions(); },
                () -> null, () -> null));

        list.add(slider("vanilla_max_fps", Category.GENERAL, g3Zh, g3En,
                "最大帧率上限", "Max Framerate",
                "同步限制 UE 渲染器与 Minecraft 的帧率上限。拖至最右为无限制。",
                "Caps Unreal renderer and Minecraft frame rate. Far right is Unlimited.",
                "fps limit framerate cap 帧率 上限 限帧",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.VANILLA_SYNC, false, false, Set.of(), null,
                10, BridgeWire.UNLIMITED_FPS, 10,
                () -> opts.framerateLimit().get(),
                v -> { opts.framerateLimit().set(v); h.saveVanillaOptions(); },
                v -> v >= BridgeWire.UNLIMITED_FPS ? txt(h, "无限制", "Unlimited") : v + " FPS",
                () -> false, () -> { opts.framerateLimit().set(120); h.saveVanillaOptions(); },
                () -> null, () -> null));

        list.add(cycle("vanilla_vsync", Category.GENERAL, g3Zh, g3En,
                "垂直同步 (VSync)", "VSync",
                "同步显示器刷新率以消除画面撕裂，可能略微增加输入延迟。",
                "Synchronizes presentation with display refresh to prevent screen tearing.",
                "vsync tear refresh 垂直同步 撕裂",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.VANILLA_SYNC, false, Set.of(), null,
                List.of(0, 1),
                () -> opts.enableVsync().get() ? 1 : 0,
                v -> { opts.enableVsync().set(v == 1); h.saveVanillaOptions(); },
                v -> onOff(h, v),
                () -> false, () -> { opts.enableVsync().set(false); h.saveVanillaOptions(); },
                () -> null, () -> null));

        list.add(cycle("vanilla_afk_fps", Category.GENERAL, g3Zh, g3En,
                "空闲时降低帧率", "Reduce FPS When Idle",
                "窗口最小化或长时间无操作时自动降低 UE 帧率以省电降温。",
                "Throttles Unreal frame rate when minimized or AFK to save power.",
                "afk idle power minimize 空闲 省电 降帧",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.VANILLA_SYNC, false, Set.of(), null,
                List.of(0, 1),
                () -> opts.inactivityFpsLimit().get() == InactivityFpsLimit.AFK ? 1 : 0,
                v -> {
                    opts.inactivityFpsLimit().set(v == 1 ? InactivityFpsLimit.AFK : InactivityFpsLimit.MINIMIZED);
                    h.saveVanillaOptions();
                },
                v -> v == 1 ? txt(h, "离开时降帧", "AFK & Minimized") : txt(h, "仅最小化时", "Minimized Only"),
                () -> false, () -> { opts.inactivityFpsLimit().set(InactivityFpsLimit.MINIMIZED); h.saveVanillaOptions(); },
                () -> null, () -> null));

        list.add(cycle("vanilla_fullscreen", Category.GENERAL, g3Zh, g3En,
                "无边框全屏", "Borderless Fullscreen",
                "控制 UE 渲染窗口的无边框全屏状态，游戏中也可随时按 F11 切换。",
                "Controls Unreal's borderless fullscreen window. Also toggled with F11.",
                "fullscreen window borderless f11 全屏 窗口",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.VANILLA_SYNC, false, Set.of(), null,
                List.of(0, 1),
                () -> h.minecraft().getWindow().isFullscreen() ? 1 : 0,
                v -> {
                    boolean want = v == 1;
                    opts.fullscreen().set(want);
                    if (h.minecraft().getWindow().isFullscreen() != want) {
                        h.minecraft().getWindow().toggleFullScreen();
                    }
                    h.saveVanillaOptions();
                },
                v -> onOff(h, v),
                () -> false, () -> {
                    opts.fullscreen().set(false);
                    if (h.minecraft().getWindow().isFullscreen()) {
                        h.minecraft().getWindow().toggleFullScreen();
                    }
                    h.saveVanillaOptions();
                },
                () -> null, () -> null));
    }

    // -------------------------------------------------------------------------
    // 2. LIGHTING & SHADOWS
    // -------------------------------------------------------------------------
    private static void addLighting(List<OptionEntry> list, Host h) {
        String g1Zh = "全局光照与反射", g1En = "Global Illumination & Reflections";
        list.add(cycle("gi_mode", Category.LIGHTING, g1Zh, g1En,
                "全局光照 (GI)", "Global Illumination",
                "控制光线在方块间的间接弹射与色彩溢出。关闭后回退至 Minecraft 原版天空光与方块光补光，可大幅减轻 GPU 负担。",
                "Controls indirect light bouncing and color bleed. Disabling falls back to Minecraft sky/block lighting for much higher FPS.",
                "lumen gi indirect global illumination bounce 全局光照 间接光 补光",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.HARDWARE_GI, Setting.GI_QUALITY), Setting.HARDWARE_GI.effectiveKey,
                List.of(FOLLOW, 0, 1, 2, 3),
                () -> {
                    int hw = h.graphics().get(Setting.HARDWARE_GI);
                    if (hw == 0) return 0;
                    int q = h.graphics().get(Setting.GI_QUALITY);
                    if (q != FOLLOW) return q;
                    return hw == 1 ? 2 : FOLLOW;
                },
                v -> {
                    if (v == FOLLOW) {
                        h.saveGraphics(Map.of(Setting.HARDWARE_GI, FOLLOW, Setting.GI_QUALITY, FOLLOW));
                    } else if (v == 0) {
                        h.saveGraphics(Map.of(Setting.HARDWARE_GI, 0, Setting.GI_QUALITY, FOLLOW));
                    } else {
                        h.saveGraphics(Map.of(Setting.HARDWARE_GI, 1, Setting.GI_QUALITY, v));
                    }
                },
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 0 -> txt(h, "关闭（MC 原版补光）", "Off (MC Vertex Fill)");
                    case 1 -> txt(h, "Lumen 低", "Lumen Low");
                    case 2 -> txt(h, "Lumen 高", "Lumen High");
                    default -> txt(h, "Lumen 极高", "Lumen Epic");
                },
                () -> h.graphics().isOverridden(Setting.HARDWARE_GI) || h.graphics().isOverridden(Setting.GI_QUALITY),
                () -> h.saveGraphics(Map.of(Setting.HARDWARE_GI, FOLLOW, Setting.GI_QUALITY, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "当前为软件兼容路径，需开启硬件光追", "Requires Hardware Ray Tracing") : null,
                () -> "1".equals(h.effective().get("HardGi"))
                        ? txt(h, "Lumen (档位 " + h.effective().getOrDefault("sg.GlobalIlluminationQuality", "-") + ")",
                        "Lumen (Tier " + h.effective().getOrDefault("sg.GlobalIlluminationQuality", "-") + ")")
                        : (h.effective().isEmpty() ? null : txt(h, "MC 补光", "MC Vertex Fill"))));

        list.add(cycle("refl_mode", Category.LIGHTING, g1Zh, g1En,
                "反射质量", "Reflection Quality",
                "控制水面、金属、湿润地面与冰块的反射清晰度。屏幕空间反射开销低但看不到屏幕外物体；Lumen 光追反射可完整映出四周景物。",
                "Controls reflections on water, metals, wet blocks, and ice. Lumen RT reflects off-screen geometry; SSR is cheaper.",
                "reflection ssr lumen mirror water 反射 倒影 水面 镜面",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.HARDWARE_REFLECTIONS, Setting.REFLECTION_QUALITY), Setting.HARDWARE_REFLECTIONS.effectiveKey,
                List.of(FOLLOW, 0, 1, 2, 3),
                () -> {
                    int hw = h.graphics().get(Setting.HARDWARE_REFLECTIONS);
                    if (hw == 0) return 0;
                    int q = h.graphics().get(Setting.REFLECTION_QUALITY);
                    if (q != FOLLOW) return q;
                    return hw == 1 ? 2 : FOLLOW;
                },
                v -> {
                    if (v == FOLLOW) {
                        h.saveGraphics(Map.of(Setting.HARDWARE_REFLECTIONS, FOLLOW, Setting.REFLECTION_QUALITY, FOLLOW));
                    } else if (v == 0) {
                        h.saveGraphics(Map.of(Setting.HARDWARE_REFLECTIONS, 0, Setting.REFLECTION_QUALITY, FOLLOW));
                    } else {
                        h.saveGraphics(Map.of(Setting.HARDWARE_REFLECTIONS, 1, Setting.REFLECTION_QUALITY, v));
                    }
                },
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 0 -> txt(h, "屏幕空间反射 (SSR)", "Screen-Space (SSR)");
                    case 1 -> txt(h, "光追反射 · 中（半分辨率）", "RT Medium (Half-Res)");
                    case 2 -> txt(h, "光追反射 · 高（半分辨率）", "RT High (Half-Res)");
                    default -> txt(h, "光追反射 · 极高（全分辨率）", "RT Epic (Full-Res)");
                },
                () -> h.graphics().isOverridden(Setting.HARDWARE_REFLECTIONS) || h.graphics().isOverridden(Setting.REFLECTION_QUALITY),
                () -> h.saveGraphics(Map.of(Setting.HARDWARE_REFLECTIONS, FOLLOW, Setting.REFLECTION_QUALITY, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "当前为软件兼容路径，已自动使用 SSR", "Software path active (using SSR)") : null,
                () -> "1".equals(h.effective().get("HardReflections"))
                        ? txt(h, "Lumen 光追 (档位 " + h.effective().getOrDefault("sg.ReflectionQuality", "-") + ")",
                        "Lumen RT (Tier " + h.effective().getOrDefault("sg.ReflectionQuality", "-") + ")")
                        : (h.effective().isEmpty() ? null : txt(h, "屏幕空间反射", "SSR"))));

        list.add(cycle("translucency_refl", Category.LIGHTING, g1Zh, g1En,
                "半透明前层反射", "Translucency Reflections",
                "为玻璃、染色玻璃等半透明材质启用高精度 Lumen 前层镜面反射。",
                "Enables high-quality Lumen front-layer reflections on glass and translucent blocks.",
                "glass translucent reflection 玻璃 半透明 反射",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.TRANSLUCENCY_REFLECTIONS), Setting.TRANSLUCENCY_REFLECTIONS.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.TRANSLUCENCY_REFLECTIONS),
                v -> h.saveGraphics(Map.of(Setting.TRANSLUCENCY_REFLECTIONS, v)),
                v -> v == FOLLOW ? follow(h) : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.TRANSLUCENCY_REFLECTIONS),
                () -> h.saveGraphics(Map.of(Setting.TRANSLUCENCY_REFLECTIONS, FOLLOW)),
                () -> !isHardwareReflectionsActive(h) ? txt(h, "需开启 Lumen 硬件反射", "Requires Lumen hardware reflections") : null,
                () -> reportRawOnOff(h, "TranslucencyReflections")));

        String g2Zh = "太阳与月亮阴影", g2En = "Sun & Moon Shadows";
        list.add(cycle("shadow_quality", Category.LIGHTING, g2Zh, g2En,
                "阴影质量", "Shadow Quality",
                "控制太阳和月亮阴影的清晰度、柔和过渡和可见距离。",
                "Controls sun and moon shadow sharpness, softness, and range.",
                "shadow vsm sun moon 阴影 影子 清晰度",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.SHADOW_QUALITY), Setting.SHADOW_QUALITY.effectiveKey,
                List.of(FOLLOW, 1, 2, 3),
                () -> h.graphics().get(Setting.SHADOW_QUALITY),
                v -> h.saveGraphics(Map.of(Setting.SHADOW_QUALITY, v)),
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 1 -> txt(h, "低", "Low");
                    case 2 -> txt(h, "中", "Medium");
                    default -> txt(h, "高", "High");
                },
                () -> h.graphics().isOverridden(Setting.SHADOW_QUALITY),
                () -> h.saveGraphics(Map.of(Setting.SHADOW_QUALITY, FOLLOW)),
                () -> null,
                () -> h.effective().get("sg.ShadowQuality")));

        list.add(cycle("dir_shadow_method", Category.LIGHTING, g2Zh, g2En,
                "日月阴影算法", "Sun & Moon Shadow Method",
                "虚拟阴影贴图 (VSM) 综合性价比最高；硬件光追阴影边缘极其精准无漏光，但 GPU 开销更大。",
                "Virtual Shadow Maps (VSM) offer best balance. Hardware ray-traced shadows are crispest at higher GPU cost.",
                "vsm raytraced shadow sun moon 虚拟阴影 光追阴影",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.DIRECTIONAL_SHADOWS), Setting.DIRECTIONAL_SHADOWS.effectiveKey,
                List.of(FOLLOW, 0, 1),
                () -> h.graphics().get(Setting.DIRECTIONAL_SHADOWS),
                v -> h.saveGraphics(Map.of(Setting.DIRECTIONAL_SHADOWS, v)),
                v -> v == FOLLOW ? follow(h) : v == 1 ? txt(h, "硬件光追阴影", "Ray-Traced Shadows") : txt(h, "虚拟阴影 (VSM)", "Virtual Shadow Maps"),
                () -> h.graphics().isOverridden(Setting.DIRECTIONAL_SHADOWS),
                () -> h.saveGraphics(Map.of(Setting.DIRECTIONAL_SHADOWS, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "需开启硬件光追", "Requires Hardware Ray Tracing") : null,
                () -> {
                    String raw = h.effective().get("DirectionalRayShadows");
                    if (raw == null) return null;
                    return "1".equals(raw) ? txt(h, "硬件光追阴影", "Ray-Traced") : "VSM";
                }));

        list.add(cycle("contact_shadows", Category.LIGHTING, g2Zh, g2En,
                "近景接触阴影", "Contact Shadows",
                "为草丛、台阶、栅栏与细小方块边缘补足近距离屏幕空间阴影，显著增强地面立体层次。",
                "Adds crisp screen-space micro-shadows for grass, steps, fences, and small block details.",
                "contact shadow micro detail 接触阴影 细节 立体",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.CONTACT_SHADOWS), Setting.CONTACT_SHADOWS.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.CONTACT_SHADOWS),
                v -> h.saveGraphics(Map.of(Setting.CONTACT_SHADOWS, v)),
                v -> v == FOLLOW ? txt(h, "默认（开）", "Default (On)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.CONTACT_SHADOWS),
                () -> h.saveGraphics(Map.of(Setting.CONTACT_SHADOWS, FOLLOW)),
                () -> null,
                () -> {
                    String raw = h.effective().get("mcue.Shadows.ContactLength");
                    if (raw == null) return null;
                    return raw.startsWith("0") ? off(h) : on(h) + " (" + raw + " cm)";
                }));

        list.add(cycle("sun_shadow_smooth", Category.LIGHTING, g2Zh, g2En,
                "日影移动平滑度", "Sun Shadow Movement",
                "节能步进会缓存地形阴影以提升帧率；完全连续则每帧实时更新日影角度，日出日落丝滑无跳格。",
                "Stepped caches VSM pages for speed; Continuous moves sun shadows smoothly every frame.",
                "sun angle step smooth vsm cache 日影 平滑 跳动",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.SUN_SHADOW_SMOOTH), Setting.SUN_SHADOW_SMOOTH.effectiveKey,
                List.of(FOLLOW, 0, 1),
                () -> h.graphics().get(Setting.SUN_SHADOW_SMOOTH),
                v -> h.saveGraphics(Map.of(Setting.SUN_SHADOW_SMOOTH, v)),
                v -> v == FOLLOW ? follow(h) : v == 1 ? txt(h, "完全连续", "Continuous") : txt(h, "节能步进", "Cached Step"),
                () -> h.graphics().isOverridden(Setting.SUN_SHADOW_SMOOTH),
                () -> h.saveGraphics(Map.of(Setting.SUN_SHADOW_SMOOTH, FOLLOW)),
                () -> null,
                () -> {
                    String raw = h.effective().get("SunShadowSmooth");
                    return raw == null ? null : ("1".equals(raw) ? txt(h, "完全连续", "Continuous") : txt(h, "节能步进", "Cached Step"));
                }));

        String g3Zh = "方块动态光源", g3En = "Block Point Lights";
        list.add(cycle("block_lights_tier", Category.LIGHTING, g3Zh, g3En,
                "动态光源数量与质量", "Dynamic Block Lights",
                "控制火把、灯笼、岩浆、海晶灯等发光方块产生真实动态点光源的数量上限。自动匹配当前硬件 MegaLights 或延迟光照路径。",
                "Controls how many nearby torches, lanterns, lava, and glowstone cast real dynamic lights across MegaLights or deferred paths.",
                "torch lantern lava megalights point light budget 火把 灯笼 点光源 岩浆",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.MEGA_LIGHT_BUDGET, Setting.FALLBACK_LIGHT_BUDGET, Setting.MEGA_LIGHT_SAMPLES), null,
                List.of(FOLLOW, 0, 1, 2, 3, 4),
                () -> inferBlockLightTier(h),
                v -> applyBlockLightTier(h, v),
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 0 -> off(h);
                    case 1 -> txt(h, "低", "Low");
                    case 2 -> txt(h, "中", "Medium");
                    case 3 -> txt(h, "高", "High");
                    default -> txt(h, "极高", "Ultra");
                },
                () -> h.graphics().isOverridden(Setting.MEGA_LIGHT_BUDGET)
                        || h.graphics().isOverridden(Setting.FALLBACK_LIGHT_BUDGET),
                () -> h.saveGraphics(Map.of(
                        Setting.MEGA_LIGHT_BUDGET, FOLLOW,
                        Setting.FALLBACK_LIGHT_BUDGET, FOLLOW,
                        Setting.MEGA_LIGHT_SAMPLES, FOLLOW)),
                () -> null,
                () -> {
                    if (h.effective().isEmpty()) return null;
                    boolean mega = "1".equals(h.effective().get("MegaLights"));
                    return mega
                            ? "MegaLights: " + h.effective().getOrDefault("mcue.PointLights.Max", "-")
                            : txt(h, "延迟光源: ", "Deferred: ") + h.effective().getOrDefault("mcue.PointLights.FallbackMax", "-");
                }));

        list.add(cycle("megalights", Category.LIGHTING, g3Zh, g3En,
                "MegaLights 硬件多光源", "MegaLights",
                "利用 UE5 硬件光追同时高效照亮数百至上千盏火把和灯笼并投射柔和阴影。关闭后使用传统延迟点光源。",
                "Uses UE5 hardware ray tracing to render hundreds of shadowed block lights efficiently.",
                "megalights hardware point lights 多光源 硬件点光",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.MEGA_LIGHTS), Setting.MEGA_LIGHTS.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.MEGA_LIGHTS),
                v -> h.saveGraphics(Map.of(Setting.MEGA_LIGHTS, v)),
                v -> v == FOLLOW ? follow(h) : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.MEGA_LIGHTS),
                () -> h.saveGraphics(Map.of(Setting.MEGA_LIGHTS, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "需开启硬件光追", "Requires Hardware Ray Tracing") : null,
                () -> reportRawOnOff(h, "MegaLights")));

        list.add(slider("block_light_intensity", Category.LIGHTING, g3Zh, g3En,
                "方块光照与自发光亮度", "Block Light & Emissive Strength",
                "调节火把、灯笼、岩浆、荧石等发光方块的实际照度与表面自发光亮度（25%–250%，默认 100%）。",
                "Adjusts illumination and surface glow of torches, lanterns, lava, and emissive blocks (25%-250%).",
                "torch glow emissive brightness lantern 火把 亮度 发光 岩浆",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.BLOCK_LIGHT_INTENSITY), Setting.BLOCK_LIGHT_INTENSITY.effectiveKey,
                25, 250, 5,
                () -> h.graphics().getOrDefault(Setting.BLOCK_LIGHT_INTENSITY, 100),
                v -> h.saveGraphics(Map.of(Setting.BLOCK_LIGHT_INTENSITY, v)),
                v -> v + "%",
                () -> h.graphics().isOverridden(Setting.BLOCK_LIGHT_INTENSITY),
                () -> h.saveGraphics(Map.of(Setting.BLOCK_LIGHT_INTENSITY, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "BlockLightIntensity")));

        String g4Zh = "实验性光照特性", g4En = "Experimental Lighting";
        list.add(cycle("experimental_glass_lighting", Category.LIGHTING, g4Zh, g4En,
                "染色玻璃透光（实验性）", "Stained Glass Light Filter (Experimental)",
                "【实验性 · 极高开销】使太阳光与附近方块光源穿过 16 色染色玻璃和玻璃板时按厚度逐通道过滤色光。当前实现会对受影响光源按 RGB 拆分，多光源场景可能明显降低帧率，默认关闭。",
                "[Experimental · Very High Cost] Filters sunlight and nearby block lights by RGB transmittance and thickness through stained glass blocks and panes. Splits affected lights per RGB channel and can heavily impact FPS in multi-light scenes. Off by default.",
                "glass stained colored light filter experimental 玻璃 染色玻璃 彩光 透光 实验",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.GLASS_LIGHTING), Setting.GLASS_LIGHTING.effectiveKey,
                List.of(0, 1),
                () -> h.graphics().getOrDefault(Setting.GLASS_LIGHTING, 0),
                v -> h.saveGraphics(Map.of(Setting.GLASS_LIGHTING, v == 1 ? 1 : FOLLOW)),
                v -> v == 1 ? txt(h, "开启（实验性）", "On (Experimental)") : txt(h, "关闭（默认）", "Off (Default)"),
                () -> h.graphics().getOrDefault(Setting.GLASS_LIGHTING, 0) != 0,
                () -> h.saveGraphics(Map.of(Setting.GLASS_LIGHTING, FOLLOW)),
                () -> null,
                () -> reportRawOnOff(h, "mcue.Glass.Lighting")));
    }

    // -------------------------------------------------------------------------
    // 3. SKY & WEATHER
    // -------------------------------------------------------------------------
    private static void addSky(List<OptionEntry> list, Host h) {
        String g1Zh = "体积云", g1En = "Volumetric Clouds";
        list.add(cycle("cloud_quality", Category.SKY, g1Zh, g1En,
                "体积云质量", "Volumetric Cloud Quality",
                "控制主世界实时体积云的渲染精度。关闭后改由天空穹顶绘制轻量化云层，可显著省出 GPU 性能。",
                "Controls Overworld volumetric cloud ray-marching density. Turning off uses lightweight sky-dome clouds.",
                "cloud volumetric sky 体积云 云 云层 性能",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.CLOUD_SCALE), Setting.CLOUD_SCALE.effectiveKey,
                List.of(FOLLOW, 0, 35, 65, 100),
                () -> {
                    int s = h.graphics().get(Setting.CLOUD_SCALE);
                    if (s == FOLLOW || s == 0 || s == 35 || s == 65 || s == 100) return s;
                    return s < 50 ? 35 : (s < 85 ? 65 : 100);
                },
                v -> h.saveGraphics(Map.of(Setting.CLOUD_SCALE, v)),
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 0 -> off(h);
                    case 35 -> txt(h, "低 (35%)", "Low (35%)");
                    case 65 -> txt(h, "中 (65%)", "Medium (65%)");
                    default -> txt(h, "高 (100%)", "High (100%)");
                },
                () -> h.graphics().isOverridden(Setting.CLOUD_SCALE),
                () -> h.saveGraphics(Map.of(Setting.CLOUD_SCALE, FOLLOW)),
                () -> null,
                () -> {
                    if ("0".equals(h.effective().get("r.VolumetricCloud"))) return off(h);
                    return reportPercentFloat(h, "CloudSamples");
                }));

        list.add(cycle("cloud_layers", Category.SKY, g1Zh, g1En,
                "云层结构", "Cloud Layer Structure",
                "自由选择单层低空积云、双层云幕，或包含高空丝状卷云的完整三层大气云系。",
                "Choose between single-layer cumulus, dual-layer cloud decks, or full 3-layer clouds with high-altitude cirrus.",
                "cirrus cumulus mid layer cloud 卷云 积云 三层云 云层",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.CLOUD_LAYERS), Setting.CLOUD_LAYERS.effectiveKey,
                List.of(FOLLOW, 1, 2, 3),
                () -> h.graphics().get(Setting.CLOUD_LAYERS),
                v -> h.saveGraphics(Map.of(Setting.CLOUD_LAYERS, v)),
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 1 -> txt(h, "单层积云", "Single Cumulus");
                    case 2 -> txt(h, "双层（积云 + 中层云）", "Dual (Cumulus + Mid)");
                    default -> txt(h, "完整三层（含高空卷云）", "Full 3-Layer (+ Cirrus)");
                },
                () -> h.graphics().isOverridden(Setting.CLOUD_LAYERS),
                () -> h.saveGraphics(Map.of(Setting.CLOUD_LAYERS, FOLLOW)),
                () -> isCloudsDisabled(h) ? txt(h, "体积云已关闭", "Volumetric clouds disabled") : null,
                () -> {
                    String raw = h.effective().get("CloudLayers");
                    return raw == null ? null : (h.chinese() ? raw + " 层" : raw + " layers");
                }));

        list.add(cycle("cloud_shadows", Category.SKY, g1Zh, g1En,
                "云层地面阴影", "Cloud Shadows on Terrain",
                "让天空中漂浮的云朵向大地与水面投射缓缓移动的云影，极大增强辽阔感。",
                "Projects moving volumetric cloud shadows onto terrain, water, and atmosphere.",
                "cloud shadow sky 云影 阴影",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.CLOUD_SHADOWS), Setting.CLOUD_SHADOWS.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.CLOUD_SHADOWS),
                v -> h.saveGraphics(Map.of(Setting.CLOUD_SHADOWS, v)),
                v -> v == FOLLOW ? follow(h) : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.CLOUD_SHADOWS),
                () -> h.saveGraphics(Map.of(Setting.CLOUD_SHADOWS, FOLLOW)),
                () -> isCloudsDisabled(h) ? txt(h, "体积云已关闭", "Volumetric clouds disabled") : null,
                () -> reportRawOnOff(h, "CloudShadows")));

        list.add(slider("cloud_coverage", Category.SKY, g1Zh, g1En,
                "晴天云量", "Fair-Weather Cloud Coverage",
                "控制晴天时的低空云覆盖比例（0% 万里无云 ～ 100% 浓云密布，默认 50%）。雨雪天气会自动接管云量。",
                "Controls fair-weather low cloud coverage (0% clear sky to 100% overcast, default 50%).",
                "cloud coverage clear overcast 云量 晴天 多云",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.CLOUD_COVERAGE), Setting.CLOUD_COVERAGE.effectiveKey,
                0, 100, 5,
                () -> h.graphics().getOrDefault(Setting.CLOUD_COVERAGE, 50),
                v -> h.saveGraphics(Map.of(Setting.CLOUD_COVERAGE, v)),
                v -> v + "%",
                () -> h.graphics().isOverridden(Setting.CLOUD_COVERAGE),
                () -> h.saveGraphics(Map.of(Setting.CLOUD_COVERAGE, FOLLOW)),
                () -> isCloudsDisabled(h) ? txt(h, "体积云已关闭", "Volumetric clouds disabled") : null,
                () -> reportPercentFloat(h, "mcue.Clouds.Coverage")));

        String g2Zh = "大气与雾效", g2En = "Atmosphere & Fog";
        list.add(cycle("volumetric_fog", Category.SKY, g2Zh, g2En,
                "体积雾与丁达尔光", "Volumetric Fog & God Rays",
                "在主世界空气中模拟阳光穿透林间与云隙的立体光柱与近景雾气。自动映射为最优体素精度。",
                "Renders 3D sunlight shafts and atmospheric depth fog in the Overworld.",
                "volumetric fog god rays tyndall 体积雾 丁达尔 光柱 耶稣光",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.FOG_GRID), Setting.FOG_GRID.effectiveKey,
                List.of(FOLLOW, 0, 24, 16, 12, 8),
                () -> {
                    int g = h.graphics().get(Setting.FOG_GRID);
                    if (g == FOLLOW || g == 0 || g == 24 || g == 16 || g == 12 || g == 8) return g;
                    return g > 20 ? 24 : (g > 14 ? 16 : (g > 10 ? 12 : 8));
                },
                v -> h.saveGraphics(Map.of(Setting.FOG_GRID, v)),
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 0 -> off(h);
                    case 24 -> txt(h, "低（省性能）", "Low");
                    case 16 -> txt(h, "中", "Medium");
                    case 12 -> txt(h, "高", "High");
                    default -> txt(h, "极高（最精细）", "Ultra");
                },
                () -> h.graphics().isOverridden(Setting.FOG_GRID),
                () -> h.saveGraphics(Map.of(Setting.FOG_GRID, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "体积雾在硬件光照模式下启用", "Volumetric fog requires hardware lighting") : null,
                () -> {
                    if ("0".equals(h.effective().get("r.VolumetricFog")) || "0".equals(h.effective().get("HWRT"))) {
                        return off(h);
                    }
                    String g = h.effective().get("r.VolumetricFog.GridPixelSize");
                    return g == null ? null : (g + " px");
                }));

        list.add(slider("fog_density", Category.SKY, g2Zh, g2En,
                "大气雾浓度", "Atmospheric Fog Density",
                "调节远景空气雾与体积雾的浓淡程度（0% 极度通透看清远方建筑 ～ 200% 浓郁雾气，默认 100%）。",
                "Scales distant aerial haze and fog density (0% crystal clear view to 200% thick mist, default 100%).",
                "fog haze mist distance visibility 雾 浓度 通透 远景 能见度",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.FOG_DENSITY), Setting.FOG_DENSITY.effectiveKey,
                0, 200, 5,
                () -> h.graphics().getOrDefault(Setting.FOG_DENSITY, 100),
                v -> h.saveGraphics(Map.of(Setting.FOG_DENSITY, v)),
                v -> v == 0 ? txt(h, "关闭 (0%)", "Off (0%)") : v + "%",
                () -> h.graphics().isOverridden(Setting.FOG_DENSITY),
                () -> h.saveGraphics(Map.of(Setting.FOG_DENSITY, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "FogDensity")));

        String g3Zh = "天气与地面响应", g3En = "Weather & Surface Response";
        list.add(cycle("rain_wetness", Category.SKY, g3Zh, g3En,
                "雨后地面打湿与积水", "Rain Wetness & Puddles",
                "控制下雨时露天地面变暗变湿与形成镜面水洼反光的效果。如果觉得雨地太光滑反光，可切为「仅表面微湿」或关闭。",
                "Controls rain surface darkening and glossy puddle reflections on exposed ground.",
                "rain wet puddle reflection gloss 雨 积水 水洼 打湿 反光",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.RAIN_WETNESS), Setting.RAIN_WETNESS.effectiveKey,
                List.of(FOLLOW, 2, 1, 0),
                () -> h.graphics().get(Setting.RAIN_WETNESS),
                v -> h.saveGraphics(Map.of(Setting.RAIN_WETNESS, v)),
                v -> switch (v) {
                    case FOLLOW -> txt(h, "默认（完整积水反光）", "Default (Full Puddles)");
                    case 2 -> txt(h, "完整积水反光", "Full Puddles");
                    case 1 -> txt(h, "仅表面微湿（无水洼）", "Wet Surface Only");
                    default -> off(h);
                },
                () -> h.graphics().isOverridden(Setting.RAIN_WETNESS),
                () -> h.saveGraphics(Map.of(Setting.RAIN_WETNESS, FOLLOW)),
                () -> null,
                () -> {
                    String raw = h.effective().get("RainWetness");
                    if (raw == null) return null;
                    return switch (raw) {
                        case "0" -> off(h);
                        case "1" -> txt(h, "仅微湿", "Wet Only");
                        default -> txt(h, "完整积水", "Full Puddles");
                    };
                }));

        list.add(cycle("puddle_depressions", Category.SKY, g3Zh, g3En,
                "水洼凹陷", "Puddle Depressions",
                "让积水下的砖面显出浅凹。水面反光和脚步波纹仍沿用原有表面，碰撞与行走高度由 Minecraft 决定。默认关闭。",
                "Adds a shallow depth effect beneath puddles. The reflective surface and footstep ripples stay at the original level, and Minecraft still determines collision and walking height. Off by default.",
                "rain puddle depression parallax water basin 雨 积水 水洼 凹陷 水底 视差",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.PUDDLE_DEPRESSIONS), Setting.PUDDLE_DEPRESSIONS.effectiveKey,
                List.of(0, 1),
                () -> h.graphics().getOrDefault(Setting.PUDDLE_DEPRESSIONS, 0),
                v -> h.saveGraphics(Map.of(Setting.PUDDLE_DEPRESSIONS, v)),
                v -> onOff(h, v),
                () -> h.graphics().isOverridden(Setting.PUDDLE_DEPRESSIONS),
                () -> h.saveGraphics(Map.of(Setting.PUDDLE_DEPRESSIONS, FOLLOW)),
                () -> null,
                () -> reportRawOnOff(h, Setting.PUDDLE_DEPRESSIONS.effectiveKey)));

        list.add(slider("precip_density", Category.SKY, g3Zh, g3En,
                "雨雪粒子密度", "Rain & Snow Density",
                "控制下雨和下雪时周围 3D 雨丝与飘雪的密集程度（0% 关闭雨雪颗粒，100% 默认，150% 暴雨暴雪）。",
                "Scales 3D rain streak and drifting snowflake count (0% disables precipitation particles).",
                "rain snow storm precipitation particles 雨 雪 下雨 下雪 粒子",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.PRECIP_DENSITY), Setting.PRECIP_DENSITY.effectiveKey,
                0, 150, 10,
                () -> h.graphics().getOrDefault(Setting.PRECIP_DENSITY, 100),
                v -> h.saveGraphics(Map.of(Setting.PRECIP_DENSITY, v)),
                v -> v == 0 ? off(h) : v + "%",
                () -> h.graphics().isOverridden(Setting.PRECIP_DENSITY),
                () -> h.saveGraphics(Map.of(Setting.PRECIP_DENSITY, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "PrecipDensity")));
    }

    // -------------------------------------------------------------------------
    // 4. WATER (P1 / P2 / P3)
    // -------------------------------------------------------------------------
    private static void addWater(List<OptionEntry> list, Host h) {
        String g1Zh = "水体物理模拟（性能）", g1En = "Water Simulation & Optics";
        list.add(cycle("water_preset", Category.WATER, g1Zh, g1En,
                "水体综合质量", "Water Quality Preset",
                "一键组合水体 P1 逐面材质、P2 水下物理光学/焦散/光柱，以及 P3 动态涟漪/FFT 几何波浪/瀑布水雾。",
                "Quickly configures face-aware water (P1), underwater optics/caustics (P2), and waves/ripples (P3).",
                "water preset p1 p2 p3 waves caustics 水体 水 预设 波浪 焦散",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.WATER_P1, Setting.WATER_P2, Setting.WATER_P3, Setting.WATER_WAVES,
                        Setting.WATER_VOLUME, Setting.WATER_CAUSTICS, Setting.WATER_PARTICLES), null,
                List.of(FOLLOW, 0, 1, 2),
                () -> inferWaterPreset(h),
                v -> applyWaterPreset(h, v),
                v -> switch (v) {
                    case FOLLOW -> follow(h);
                    case 0 -> txt(h, "基础（最省性能）", "Basic (Fast)");
                    case 1 -> txt(h, "精细（水下光学 + 焦散）", "Detailed (Optics + Caustics)");
                    default -> txt(h, "逼真（完整波浪与交互）", "Realistic (Full Waves + P3)");
                },
                () -> h.graphics().isOverridden(Setting.WATER_P1)
                        || h.graphics().isOverridden(Setting.WATER_P2)
                        || h.graphics().isOverridden(Setting.WATER_P3)
                        || h.graphics().isOverridden(Setting.WATER_WAVES),
                () -> applyWaterPreset(h, FOLLOW),
                () -> null, () -> null));

        list.add(cycle("water_p1", Category.WATER, g1Zh, g1En,
                "逐面高级水体 (P1)", "Face-Aware Water (P1)",
                "区分水面、玻璃贴面、跌水瀑布、岸湿与入水白沫；关闭后退回基础水体着色。",
                "Enables shore distance, flow alignment, waterfalls, aeration foam, and wet shores.",
                "water p1 foam waterfall shore 逐面 水体 白沫 瀑布",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.WATER_P1), Setting.WATER_P1.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.WATER_P1),
                v -> h.saveGraphics(Map.of(Setting.WATER_P1, v)),
                v -> v == FOLLOW ? txt(h, "默认（开）", "Default (On)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.WATER_P1),
                () -> h.saveGraphics(Map.of(Setting.WATER_P1, FOLLOW)),
                () -> null,
                () -> reportRawOnOff(h, "mc.Water.P1")));

        list.add(cycle("water_p2", Category.WATER, g1Zh, g1En,
                "水下物理光学与半浸水线 (P2)", "Underwater Optics & Waterline (P2)",
                "逐像素计算真实水中光程、RGB 吸收/散射与半浸入水面时的真实水线分割。",
                "Per-pixel underwater optical depth, RGB extinction/scattering, and half-submerged waterline.",
                "water p2 underwater optics waterline 水下 光学 水线",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.WATER_P2), Setting.WATER_P2.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.WATER_P2),
                v -> h.saveGraphics(Map.of(Setting.WATER_P2, v)),
                v -> v == FOLLOW ? txt(h, "默认（开）", "Default (On)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.WATER_P2),
                () -> h.saveGraphics(Map.of(Setting.WATER_P2, FOLLOW)),
                () -> null,
                () -> reportRawOnOff(h, "mc.Water.P2")));

        list.add(cycle("water_p3", Category.WATER, g1Zh, g1En,
                "水面交互涟漪与瀑布水雾 (P3)", "Ripples & Waterfall Mist (P3)",
                "模拟玩家、生物、船尾迹、雨滴和瀑布在浅水激起的实时涟漪，以及瀑布落点雾气与飞溅水花。",
                "Simulates real-time water ripples from players, boats, rain, and waterfalls with mist and splashes.",
                "water p3 ripple boat waterfall mist splash 涟漪 船 尾迹 瀑布 水雾",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.WATER_P3), Setting.WATER_P3.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.WATER_P3),
                v -> h.saveGraphics(Map.of(Setting.WATER_P3, v)),
                v -> v == FOLLOW ? follow(h) : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.WATER_P3),
                () -> h.saveGraphics(Map.of(Setting.WATER_P3, FOLLOW)),
                () -> null,
                () -> reportRawOnOff(h, "WaterP3")));

        list.add(cycle("water_waves", Category.WATER, g1Zh, g1En,
                "水面几何波浪起伏", "3D Water Wave Displacement",
                "在开阔深水顶面启用 FFT 垂直波浪起伏（近岸自动锁定平齐防穿模）。",
                "Enables physical FFT vertical wave displacement on open deep water while locking shore edges.",
                "water waves wpo fft displacement 波浪 起伏 水面",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.WATER_WAVES), Setting.WATER_WAVES.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.WATER_WAVES),
                v -> h.saveGraphics(Map.of(Setting.WATER_WAVES, v)),
                v -> v == FOLLOW ? follow(h) : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.WATER_WAVES),
                () -> h.saveGraphics(Map.of(Setting.WATER_WAVES, FOLLOW)),
                () -> isWaterP3Disabled(h) ? txt(h, "需先开启「水面交互 (P3)」", "Requires Water Interaction (P3)") : null,
                () -> reportRawOnOff(h, "WaterWaves")));

        list.add(cycle("water_volume", Category.WATER, g1Zh, g1En,
                "水下太阳光柱", "Underwater Sun Shafts",
                "在阳光照射的水体中计算半分辨率体积散射，呈现清澈的水下丁达尔光束。",
                "Renders volumetric underwater god rays from sunlight and nearby submerged lights.",
                "water volume sun shaft god ray 水下 光柱 体积光",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.WATER_VOLUME), Setting.WATER_VOLUME.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.WATER_VOLUME),
                v -> h.saveGraphics(Map.of(Setting.WATER_VOLUME, v)),
                v -> v == FOLLOW ? txt(h, "默认（开）", "Default (On)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.WATER_VOLUME),
                () -> h.saveGraphics(Map.of(Setting.WATER_VOLUME, FOLLOW)),
                () -> h.graphics().get(Setting.WATER_P2) == 0 ? txt(h, "需先开启「水下物理光学 (P2)」", "Requires Water Optics (P2)") : null,
                () -> reportRawOnOff(h, "mc.Water.P2.Volume")));

        list.add(cycle("water_caustics", Category.WATER, g1Zh, g1En,
                "水底阳光焦散", "Underwater Caustics",
                "在受阳光照射的水底方块表面投射动态折射波纹光网。",
                "Projects animated solar caustic patterns onto sunlit underwater terrain.",
                "caustics water sunlight 水底 焦散 波光",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.WATER_CAUSTICS), Setting.WATER_CAUSTICS.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.WATER_CAUSTICS),
                v -> h.saveGraphics(Map.of(Setting.WATER_CAUSTICS, v)),
                v -> v == FOLLOW ? txt(h, "默认（开）", "Default (On)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.WATER_CAUSTICS),
                () -> h.saveGraphics(Map.of(Setting.WATER_CAUSTICS, FOLLOW)),
                () -> h.graphics().get(Setting.WATER_P2) == 0 ? txt(h, "需先开启「水下物理光学 (P2)」", "Requires Water Optics (P2)") : null,
                () -> reportRawOnOff(h, "mc.Water.P2.Caustics")));

        list.add(cycle("water_particles", Category.WATER, g1Zh, g1En,
                "水下浮游微粒与气泡柱", "Plankton & Bubble Columns",
                "在潜入水中时显示悬浮微生物微粒，以及灵魂沙/岩浆块产生的 3D 气泡柱。",
                "Renders floating underwater plankton particles and 3D Niagara bubble columns.",
                "plankton bubbles underwater particles 浮游 气泡 水下粒子",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, false,
                Set.of(Setting.WATER_PARTICLES), Setting.WATER_PARTICLES.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.WATER_PARTICLES),
                v -> h.saveGraphics(Map.of(Setting.WATER_PARTICLES, v)),
                v -> v == FOLLOW ? txt(h, "默认（开）", "Default (On)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.WATER_PARTICLES),
                () -> h.saveGraphics(Map.of(Setting.WATER_PARTICLES, FOLLOW)),
                () -> null,
                () -> reportRawOnOff(h, "mc.Water.P2.Plankton")));

        String g2Zh = "水体外观偏好（切档保留）", g2En = "Water Appearance (Kept Across Presets)";
        list.add(slider("water_clarity", Category.WATER, g2Zh, g2En,
                "水体清澈度（透光）", "Water Clarity (Transparency)",
                "向右拉让更多光线穿透水体，水底更清澈通透（100% 为默认物理吸收，200%+ 为热带清泉效果）。",
                "Higher values let more light penetrate the water for a clearer view of the seabed (100% is physical default).",
                "water clarity absorption transparent clear 清澈 透明 水底 吸收",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.WATER_ABSORPTION), Setting.WATER_ABSORPTION.effectiveKey,
                50, 400, 10,
                () -> BridgeGraphicsSettings.clarityFromAbsorption(h.graphics().getOrDefault(Setting.WATER_ABSORPTION, 100)),
                v -> h.saveGraphics(Map.of(Setting.WATER_ABSORPTION, BridgeGraphicsSettings.absorptionFromClarity(v))),
                v -> v == 100 ? txt(h, "100%（默认）", "100% (Default)") : v + "%",
                () -> h.graphics().isOverridden(Setting.WATER_ABSORPTION),
                () -> h.saveGraphics(Map.of(Setting.WATER_ABSORPTION, FOLLOW)),
                () -> null,
                () -> {
                    String raw = h.effective().get("WaterAbsorption");
                    if (raw == null) return null;
                    try {
                        int abs = Integer.parseInt(raw);
                        return BridgeGraphicsSettings.clarityFromAbsorption(abs) + "% ("
                                + txt(h, "吸收 ", "Abs ") + abs + "%)";
                    } catch (NumberFormatException ignored) {
                        return null;
                    }
                }));

        list.add(slider("water_haze", Category.WATER, g2Zh, g2En,
                "水下光雾强度", "Underwater Haze & Scattering",
                "控制水体散射产生的朦胧水雾与光柱浓郁度。0% 如玻璃水般无雾，100% 为默认写实效果。",
                "Controls underwater scattering haze and light-shaft intensity. 0% is crystal clear; 100% is default.",
                "water scattering haze fog underwater 水雾 浑浊 散射",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.WATER_SCATTERING), Setting.WATER_SCATTERING.effectiveKey,
                0, 200, 5,
                () -> h.graphics().getOrDefault(Setting.WATER_SCATTERING, 100),
                v -> h.saveGraphics(Map.of(Setting.WATER_SCATTERING, v)),
                v -> v == 100 ? txt(h, "100%（默认）", "100% (Default)") : v + "%",
                () -> h.graphics().isOverridden(Setting.WATER_SCATTERING),
                () -> h.saveGraphics(Map.of(Setting.WATER_SCATTERING, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "WaterScattering")));

        list.add(cycle("water_lens_fx", Category.WATER, g2Zh, g2En,
                "出水镜头水膜与水线折射", "Lens Water Film & Distortion",
                "潜出水面时的短暂镜头水膜效果，以及半浸入水面时的水线微弱折射扭曲。",
                "Brief water film on the lens after surfacing and subtle waterline refraction distortion.",
                "water film distortion lens 镜头 水膜 折射 水线",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.WATER_LENS_FX), Setting.WATER_LENS_FX.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.WATER_LENS_FX),
                v -> h.saveGraphics(Map.of(Setting.WATER_LENS_FX, v)),
                v -> v == FOLLOW ? txt(h, "默认（开）", "Default (On)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.WATER_LENS_FX),
                () -> h.saveGraphics(Map.of(Setting.WATER_LENS_FX, FOLLOW)),
                () -> h.graphics().get(Setting.WATER_P2) == 0 ? txt(h, "需先开启「水下物理光学 (P2)」", "Requires Water Optics (P2)") : null,
                () -> reportRawOnOff(h, "mc.Water.P2.Film")));

        list.add(slider("water_labpbr_normals", Category.WATER, g2Zh, g2En,
                "资源包水法线混合", "LabPBR Water Normal Blend",
                "使用带高清法线的 LabPBR 资源包时，将资源包水法线混入 UE 物理水波的比例（默认 0% 纯物理波纹）。",
                "Blends high-resolution LabPBR resource-pack water normals into the physical FFT water waves.",
                "labpbr water normal pack 资源包 法线 水波",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.WATER_LABPBR_NORMALS), Setting.WATER_LABPBR_NORMALS.effectiveKey,
                0, 100, 5,
                () -> h.graphics().getOrDefault(Setting.WATER_LABPBR_NORMALS, 0),
                v -> h.saveGraphics(Map.of(Setting.WATER_LABPBR_NORMALS, v)),
                v -> v + "%",
                () -> h.graphics().isOverridden(Setting.WATER_LABPBR_NORMALS),
                () -> h.saveGraphics(Map.of(Setting.WATER_LABPBR_NORMALS, FOLLOW)),
                () -> null,
                () -> reportPercentFloat(h, "mcue.Water.LabPbrNormalWeight")));
    }

    // -------------------------------------------------------------------------
    // 5. COLOR & POST-PROCESSING
    // -------------------------------------------------------------------------
    private static void addColor(List<OptionEntry> list, Host h) {
        String g1Zh = "曝光与明暗可读性", g1En = "Exposure & Legibility";
        list.add(cycle("exposure_mode", Category.COLOR, g1Zh, g1En,
                "曝光模式", "Exposure Mode",
                "智能自动按昼夜、天气与室内屋顶遮挡锚定测光，避免抬头变黑或进洞闪瞎；柔和进一步收紧亮度波动；固定则完全关闭相机测光波动。",
                "Auto adapts smoothly to day/night and interiors. Gentle reduces swing further. Fixed disables camera metering pumping.",
                "exposure auto brightness eye adaptation 曝光 自动曝光 测光 忽明忽暗",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.EXPOSURE_MODE), Setting.EXPOSURE_MODE.effectiveKey,
                List.of(FOLLOW, 1, 2, 0),
                () -> h.graphics().get(Setting.EXPOSURE_MODE),
                v -> h.saveGraphics(Map.of(Setting.EXPOSURE_MODE, v)),
                v -> switch (v) {
                    case FOLLOW -> txt(h, "默认（智能自动）", "Default (Auto)");
                    case 2 -> txt(h, "柔和自动", "Gentle Auto");
                    case 0 -> txt(h, "固定曝光", "Fixed Exposure");
                    default -> txt(h, "智能自动", "Smart Auto");
                },
                () -> h.graphics().isOverridden(Setting.EXPOSURE_MODE),
                () -> h.saveGraphics(Map.of(Setting.EXPOSURE_MODE, FOLLOW)),
                () -> null,
                () -> {
                    String raw = h.effective().get("ExposureMode");
                    if (raw == null) return null;
                    return switch (raw) {
                        case "0" -> txt(h, "固定", "Fixed");
                        case "2" -> txt(h, "柔和", "Gentle");
                        default -> txt(h, "自动", "Auto");
                    };
                }));

        list.add(slider("exposure_bias", Category.COLOR, g1Zh, g1En,
                "画面亮度（曝光补偿）", "Brightness (Exposure Bias)",
                "整体画面曝光微调（-2.0 至 +2.0 EV）。所有曝光模式下均生效，拖动可实时预览画面明暗。",
                "Overall brightness offset (-2.0 to +2.0 EV). Applies in all exposure modes.",
                "brightness exposure gamma ev 亮度 曝光 变亮 变暗",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.EXPOSURE_BIAS), Setting.EXPOSURE_BIAS.effectiveKey,
                0, 40, 1,
                () -> h.graphics().getOrDefault(Setting.EXPOSURE_BIAS, 20),
                v -> h.saveGraphics(Map.of(Setting.EXPOSURE_BIAS, v)),
                VoxGlintOptionCatalog::formatEv,
                () -> h.graphics().isOverridden(Setting.EXPOSURE_BIAS),
                () -> h.saveGraphics(Map.of(Setting.EXPOSURE_BIAS, FOLLOW)),
                () -> null,
                () -> {
                    String raw = h.effective().get("ExposureBias");
                    if (raw == null) return null;
                    try { return formatEv(Integer.parseInt(raw)); } catch (NumberFormatException ignored) { return null; }
                }));

        list.add(slider("local_exposure", Category.COLOR, g1Zh, g1En,
                "明暗平衡（局部曝光）", "Shadow & Highlight Balance",
                "柔和压制刺眼高光并提亮死黑阴影，防止看窗外或进出洞口时高光一片白、暗部一片黑（默认 60%）。",
                "Compresses harsh highlights and lifts crushed shadows simultaneously (default 60%).",
                "local exposure hdr shadow highlight 局部曝光 过曝 死黑 平衡",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.LOCAL_EXPOSURE), Setting.LOCAL_EXPOSURE.effectiveKey,
                0, 100, 5,
                () -> h.graphics().getOrDefault(Setting.LOCAL_EXPOSURE, 60),
                v -> h.saveGraphics(Map.of(Setting.LOCAL_EXPOSURE, v)),
                v -> v == 0 ? off(h) : v + "%",
                () -> h.graphics().isOverridden(Setting.LOCAL_EXPOSURE),
                () -> h.saveGraphics(Map.of(Setting.LOCAL_EXPOSURE, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "LocalExposure")));

        list.add(slider("cave_brightness", Category.COLOR, g1Zh, g1En,
                "夜晚与洞穴保底亮度", "Night & Cave Minimum Light",
                "独立控制无光源矿洞与深夜的最低环境光可见度，不破坏白天画面对比度（0% 纯黑写实，100% 标准，200% 方便挖矿探险）。",
                "Boosts minimum ambient legibility in unlit caves and nights without washing out daylight (0%-200%).",
                "cave night dark ambient floor mining 洞穴 夜晚 矿洞 太黑 保底亮度",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.CAVE_BRIGHTNESS), Setting.CAVE_BRIGHTNESS.effectiveKey,
                0, 200, 10,
                () -> h.graphics().getOrDefault(Setting.CAVE_BRIGHTNESS, 100),
                v -> h.saveGraphics(Map.of(Setting.CAVE_BRIGHTNESS, v)),
                v -> v == 0 ? txt(h, "0%（纯黑写实）", "0% (Pitch Dark)")
                        : (v == 100 ? txt(h, "100%（标准）", "100% (Default)") : v + "%"),
                () -> h.graphics().isOverridden(Setting.CAVE_BRIGHTNESS),
                () -> h.saveGraphics(Map.of(Setting.CAVE_BRIGHTNESS, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "CaveBrightness")));

        String g2Zh = "色彩与调色风格", g2En = "Color Grading";
        list.add(cycle("color_style", Category.COLOR, g2Zh, g2En,
                "色彩风格预设", "Color Style Preset",
                "快速切换整体画面色感，也可通过下方饱和度、对比度和色温自由微调。",
                "Quickly switch overall color mood, or fine-tune saturation, contrast, and temperature below.",
                "color grading style cinematic vibrant warm cool 色彩 调色 风格 滤镜 鲜艳 暖调",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.SATURATION, Setting.CONTRAST, Setting.COLOR_TEMP), null,
                List.of(0, 1, 2, 3),
                () -> inferColorStyle(h),
                v -> applyColorStyle(h, v),
                v -> {
                    int detected = inferColorStyle(h);
                    if (detected == -1) return txt(h, "自定义", "Custom");
                    return switch (v) {
                        case 1 -> txt(h, "暖阳电影", "Warm Cinematic");
                        case 2 -> txt(h, "清透鲜艳", "Vibrant & Crisp");
                        case 3 -> txt(h, "冷冽高对比", "Cool High-Contrast");
                        default -> txt(h, "写实自然（默认）", "Natural (Default)");
                    };
                },
                () -> h.graphics().isOverridden(Setting.SATURATION)
                        || h.graphics().isOverridden(Setting.CONTRAST)
                        || h.graphics().isOverridden(Setting.COLOR_TEMP),
                () -> applyColorStyle(h, 0),
                () -> null, () -> null));

        list.add(slider("saturation", Category.COLOR, g2Zh, g2En,
                "画面饱和度", "Color Saturation",
                "调整色彩鲜艳程度（50% 淡雅 ～ 100% 自然 ～ 150% 浓烈鲜艳）。",
                "Controls color vividness (50% muted, 100% natural, 150% vivid).",
                "saturation vivid color 饱和度 鲜艳 色彩",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.SATURATION), Setting.SATURATION.effectiveKey,
                50, 150, 2,
                () -> h.graphics().getOrDefault(Setting.SATURATION, 100),
                v -> h.saveGraphics(Map.of(Setting.SATURATION, v)),
                v -> v + "%",
                () -> h.graphics().isOverridden(Setting.SATURATION),
                () -> h.saveGraphics(Map.of(Setting.SATURATION, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "Saturation")));

        list.add(slider("contrast", Category.COLOR, g2Zh, g2En,
                "画面对比度", "Color Contrast",
                "调整亮部与暗部之间的反差力度（70% 柔和 ～ 100% 默认 ～ 130% 强烈对比）。",
                "Controls tonal contrast between highlights and shadows (70%-130%).",
                "contrast tone 对比度 反差",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.CONTRAST), Setting.CONTRAST.effectiveKey,
                70, 130, 2,
                () -> h.graphics().getOrDefault(Setting.CONTRAST, 100),
                v -> h.saveGraphics(Map.of(Setting.CONTRAST, v)),
                v -> v + "%",
                () -> h.graphics().isOverridden(Setting.CONTRAST),
                () -> h.saveGraphics(Map.of(Setting.CONTRAST, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "Contrast")));

        list.add(slider("color_temp", Category.COLOR, g2Zh, g2En,
                "画面色温", "Color Temperature",
                "调节画面冷暖倾向（6500K 中性，调高偏暖阳琥珀，调低偏清冷蓝调）。",
                "Adjusts color warmth (6500K neutral; higher is warmer, lower is cooler).",
                "temperature white balance warm cool 色温 冷暖 白平衡",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.COLOR_TEMP), Setting.COLOR_TEMP.effectiveKey,
                4500, 8500, 100,
                () -> h.graphics().getOrDefault(Setting.COLOR_TEMP, 6500),
                v -> h.saveGraphics(Map.of(Setting.COLOR_TEMP, v)),
                v -> v == 6500 ? txt(h, "6500K（中性）", "6500K (Neutral)")
                        : (v > 6500 ? v + "K " + txt(h, "暖", "Warm") : v + "K " + txt(h, "冷", "Cool")),
                () -> h.graphics().isOverridden(Setting.COLOR_TEMP),
                () -> h.saveGraphics(Map.of(Setting.COLOR_TEMP, FOLLOW)),
                () -> null,
                () -> {
                    String raw = h.effective().get("ColorTemp");
                    return raw == null ? null : raw + "K";
                }));

        String g3Zh = "镜头后处理特效", g3En = "Camera Post Effects";
        list.add(slider("bloom", Category.COLOR, g3Zh, g3En,
                "泛光 (Bloom) 强度", "Bloom Intensity",
                "控制火把、岩浆、阳光高光与灯笼周围的柔和光晕强度。0% 完全关闭泛光，100% 为默认。",
                "Controls glow halo around bright lights, lava, and sunlit highlights. 0% disables bloom.",
                "bloom glow halo glare 泛光 辉光 光晕 刺眼",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.BLOOM_INTENSITY), Setting.BLOOM_INTENSITY.effectiveKey,
                0, 200, 5,
                () -> h.graphics().getOrDefault(Setting.BLOOM_INTENSITY, 100),
                v -> h.saveGraphics(Map.of(Setting.BLOOM_INTENSITY, v)),
                v -> v == 0 ? off(h) : v + "%",
                () -> h.graphics().isOverridden(Setting.BLOOM_INTENSITY),
                () -> h.saveGraphics(Map.of(Setting.BLOOM_INTENSITY, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "BloomIntensity")));

        list.add(slider("vignette", Category.COLOR, g3Zh, g3En,
                "镜头暗角 (Vignette)", "Lens Vignette",
                "控制画面四角微微压暗的电影感暗角强度。0% 关闭暗角，40% 为 UE 默认。",
                "Subtle darkening toward screen corners. 0% disables vignette; 40% is Unreal default.",
                "vignette corner dark lens 暗角 镜头",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, true,
                Set.of(Setting.VIGNETTE_INTENSITY), Setting.VIGNETTE_INTENSITY.effectiveKey,
                0, 100, 5,
                () -> h.graphics().getOrDefault(Setting.VIGNETTE_INTENSITY, 40),
                v -> h.saveGraphics(Map.of(Setting.VIGNETTE_INTENSITY, v)),
                v -> v == 0 ? off(h) : v + "%",
                () -> h.graphics().isOverridden(Setting.VIGNETTE_INTENSITY),
                () -> h.saveGraphics(Map.of(Setting.VIGNETTE_INTENSITY, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "VignetteIntensity")));

        list.add(slider("motion_blur", Category.COLOR, g3Zh, g3En,
                "动态模糊", "Motion Blur",
                "快速转头或高速飞行时的镜头运动模糊效果（默认关闭，晕 3D 玩家建议保持关闭）。",
                "Camera motion blur during rapid turns or flight (off by default).",
                "motion blur camera 3d 动态模糊 运动模糊 晕",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false, false,
                Set.of(Setting.MOTION_BLUR), Setting.MOTION_BLUR.effectiveKey,
                0, 100, 10,
                () -> h.graphics().getOrDefault(Setting.MOTION_BLUR, 0),
                v -> h.saveGraphics(Map.of(Setting.MOTION_BLUR, v)),
                v -> v == 0 ? off(h) : v + "%",
                () -> h.graphics().isOverridden(Setting.MOTION_BLUR),
                () -> h.saveGraphics(Map.of(Setting.MOTION_BLUR, FOLLOW)),
                () -> null,
                () -> reportPercentInt(h, "MotionBlur")));
    }

    // -------------------------------------------------------------------------
    // 6. MATERIALS & SURFACE
    // -------------------------------------------------------------------------
    private static void addMaterials(List<OptionEntry> list, Host h) {
        String g1Zh = "自动法线与贴图凹凸（原版 / 普通资源包）", g1En = "Automatic Texture Relief";
        list.add(cycle("relief_preset", Category.MATERIALS, g1Zh, g1En,
                "自动贴图凹凸预设", "Auto Relief Preset",
                "为缺少 LabPBR 法线贴图的原版方块自动生成立体凹凸（如圆石、木板、砖块）。修改后点「完成」将重载资源包。",
                "Generates normal-map relief for opaque textures lacking LabPBR normal maps. Reloads resources on Done.",
                "normal map bump relief pbr labpbr 法线 凹凸 立体 材质",
                Impact.LOW, ApplyTiming.RELOAD_PACKS, Persistence.KEPT_ON_PRESET, false, Set.of(), null,
                List.of(0, 1, 2, 3),
                () -> inferReliefPreset(h),
                v -> applyReliefPreset(h, v),
                v -> {
                    int detected = inferReliefPreset(h);
                    if (detected == -1) return txt(h, "自定义", "Custom");
                    return switch (v) {
                        case 0 -> off(h);
                        case 1 -> txt(h, "轻微凹凸", "Subtle");
                        case 2 -> txt(h, "标准凹凸", "Standard");
                        default -> txt(h, "深邃立体", "Deep Relief");
                    };
                },
                () -> h.bridge().gradientNormals() != BridgeSettings.Toggle.STARTUP
                        || !h.bridge().relief().equals(TerrainReliefSettings.DEFAULT),
                () -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        BridgeSettings.Toggle.STARTUP, TerrainReliefSettings.DEFAULT, h.bridge().connectedGlass())),
                () -> null, () -> null));

        list.add(slider("relief_strength", Category.MATERIALS, g1Zh, g1En,
                "凹凸强度", "Relief Strength",
                "控制自动法线的深度幅度（0%–200%，默认 100%）。",
                "Controls generated normal-map depth (0%-200%, default 100%).",
                "relief strength bump depth 凹凸 强度 法线",
                Impact.NONE, ApplyTiming.RELOAD_PACKS, Persistence.KEPT_ON_PRESET, false, false, Set.of(), null,
                0, TerrainReliefSettings.MAX_STRENGTH_PERCENT, 5,
                () -> h.bridge().relief().strengthPercent(),
                v -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        v > 0 ? BridgeSettings.Toggle.ON : BridgeSettings.Toggle.OFF,
                        h.bridge().relief().withStrength(v), h.bridge().connectedGlass())),
                v -> v + "%",
                () -> h.bridge().relief().strengthPercent() != TerrainReliefSettings.DEFAULT.strengthPercent(),
                () -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief().withStrength(TerrainReliefSettings.DEFAULT.strengthPercent()),
                        h.bridge().connectedGlass())),
                () -> !isReliefEnabled(h) ? txt(h, "先在上方启用「自动贴图凹凸」", "Enable Auto Relief above first") : null,
                () -> null));

        list.add(slider("relief_threshold", Category.MATERIALS, g1Zh, g1En,
                "弱色差滤除阈值", "Contrast Noise Threshold",
                "提高阈值会滤掉贴图上细小的杂色起伏，让大块砖缝和木纹更干净（默认 2.5%）。",
                "Higher values ignore subtle color noise so brick seams and planks look cleaner (default 2.5%).",
                "relief threshold noise contrast 色差 阈值 噪点",
                Impact.NONE, ApplyTiming.RELOAD_PACKS, Persistence.KEPT_ON_PRESET, false, false, Set.of(), null,
                0, TerrainReliefSettings.MAX_THRESHOLD_TENTHS_PERCENT, 1,
                () -> h.bridge().relief().thresholdTenthsPercent(),
                v -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief().withThreshold(v), h.bridge().connectedGlass())),
                v -> String.format(Locale.ROOT, "%.1f%%", v / 10.0),
                () -> h.bridge().relief().thresholdTenthsPercent() != TerrainReliefSettings.DEFAULT.thresholdTenthsPercent(),
                () -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief().withThreshold(TerrainReliefSettings.DEFAULT.thresholdTenthsPercent()),
                        h.bridge().connectedGlass())),
                () -> !isReliefEnabled(h) ? txt(h, "先在上方启用「自动贴图凹凸」", "Enable Auto Relief above first") : null,
                () -> null));

        list.add(slider("relief_tilt", Category.MATERIALS, g1Zh, g1En,
                "最大法线倾角", "Maximum Normal Tilt",
                "限制生成法线相对原始方块表面的最大倾斜角度（5°–45°，默认 25°）。",
                "Clamps maximum generated normal angle relative to the flat face (5°-45°, default 25°).",
                "relief tilt angle normal 倾角 角度 法线",
                Impact.NONE, ApplyTiming.RELOAD_PACKS, Persistence.KEPT_ON_PRESET, false, false, Set.of(), null,
                TerrainReliefSettings.MIN_TILT_DEGREES, TerrainReliefSettings.MAX_TILT_DEGREES, 1,
                () -> h.bridge().relief().maxTiltDegrees(),
                v -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief().withMaxTilt(v), h.bridge().connectedGlass())),
                v -> v + "°",
                () -> h.bridge().relief().maxTiltDegrees() != TerrainReliefSettings.DEFAULT.maxTiltDegrees(),
                () -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief().withMaxTilt(TerrainReliefSettings.DEFAULT.maxTiltDegrees()),
                        h.bridge().connectedGlass())),
                () -> !isReliefEnabled(h) ? txt(h, "先在上方启用「自动贴图凹凸」", "Enable Auto Relief above first") : null,
                () -> null));

        String g2Zh = "方块几何与纹理清晰度", g2En = "Geometry & Texture Filtering";
        list.add(cycle("connected_glass", Category.MATERIALS, g2Zh, g2En,
                "无缝连接玻璃", "Connected Glass",
                "自动消除相邻原版玻璃和染色玻璃之间的边框接缝，使落地窗与玻璃幕墙更加通透。关闭则保留原版每格边框。",
                "Seamlessly merges adjacent vanilla glass and stained glass panes/blocks without inner borders.",
                "glass connected ctm seamless 玻璃 无缝 连接 边框",
                Impact.NONE, ApplyTiming.RELOAD_WORLD, Persistence.KEPT_ON_PRESET, false, Set.of(), null,
                List.of(1, 0),
                () -> h.bridge().connectedGlass() ? 1 : 0,
                v -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief(), v == 1)),
                v -> onOff(h, v),
                () -> !h.bridge().connectedGlass(),
                () -> h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief(), true)),
                () -> null, () -> null));

        var opts = h.minecraft().options;
        list.add(slider("vanilla_mipmap", Category.MATERIALS, g2Zh, g2En,
                "多级纹理 (Mipmap)", "Mipmap Levels",
                "同步 Minecraft 多级纹理层级，减少远处方块纹理的闪烁和摩尔纹。修改后退出将重载贴图。",
                "Synced with Minecraft mipmap levels to eliminate shimmering on distant blocks.",
                "mipmap texture alias shimmer 多级纹理 闪烁 摩尔纹",
                Impact.NONE, ApplyTiming.RELOAD_PACKS, Persistence.VANILLA_SYNC, false, false, Set.of(), null,
                0, 4, 1,
                () -> opts.mipmapLevels().get(),
                v -> { opts.mipmapLevels().set(v); h.saveVanillaOptions(); },
                v -> v == 0 ? off(h) : Integer.toString(v),
                () -> false, () -> { opts.mipmapLevels().set(4); h.saveVanillaOptions(); },
                () -> null, () -> null));

        list.add(cycle("vanilla_filtering", Category.MATERIALS, g2Zh, g2En,
                "纹理过滤模式", "Texture Filtering",
                "同步 UE 地形贴图采样过滤模式。启用「各向异性」可在沿长路斜视时保持清晰砖缝。",
                "Controls terrain atlas filtering. Anisotropic keeps oblique road/brick seams sharp.",
                "anisotropic filtering rgss texture 纹理过滤 各向异性 砖缝",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.VANILLA_SYNC, false, Set.of(), null,
                List.of(0, 1, 2),
                () -> opts.textureFiltering().get().ordinal(),
                v -> {
                    TextureFilteringMethod[] vals = TextureFilteringMethod.values();
                    opts.textureFiltering().set(vals[Math.clamp(v, 0, vals.length - 1)]);
                    h.saveVanillaOptions();
                },
                v -> switch (v) {
                    case 1 -> "RGSS";
                    case 2 -> txt(h, "各向异性过滤", "Anisotropic");
                    default -> off(h);
                },
                () -> false, () -> { opts.textureFiltering().set(TextureFilteringMethod.NONE); h.saveVanillaOptions(); },
                () -> null, () -> null));

        list.add(cycle("vanilla_aniso", Category.MATERIALS, g2Zh, g2En,
                "各向异性等级", "Anisotropic Level",
                "UE 支持 2×、4×、8× 各向异性地形采样过滤。",
                "Unreal terrain anisotropic sampling factor (2x, 4x, or 8x).",
                "anisotropic 2x 4x 8x 各向异性 等级",
                Impact.LOW, ApplyTiming.INSTANT, Persistence.VANILLA_SYNC, false, Set.of(), null,
                List.of(1, 2, 3),
                () -> Math.clamp(opts.maxAnisotropyBit().get(), 1, 3),
                v -> { opts.maxAnisotropyBit().set(v); h.saveVanillaOptions(); },
                v -> (1 << v) + "×",
                () -> false, () -> { opts.maxAnisotropyBit().set(3); h.saveVanillaOptions(); },
                () -> opts.textureFiltering().get() != TextureFilteringMethod.ANISOTROPIC
                        ? txt(h, "需先将「纹理过滤模式」设为各向异性", "Set Texture Filtering to Anisotropic first") : null,
                () -> null));
    }

    // -------------------------------------------------------------------------
    // 7. PHOTO, INTERFACE & EXPERT
    // -------------------------------------------------------------------------
    private static void addPhotoAndAdvanced(List<OptionEntry> list, Host h) {
        String g1Zh = "路径追踪摄影（游戏中按 Ctrl + Home）", g1En = "Path-Traced Photography (Ctrl + Home)";
        list.add(action("photo_capture_now", Category.PHOTO_ADVANCED, g1Zh, g1En,
                "立即拍摄路径追踪照片", "Capture Path-Traced Photo Now",
                "关闭设置界面并定格当前机位，使用 UE5 硬件路径追踪渲染一张无 HUD 的影视级高清静帧。",
                "Closes menus and renders a HUD-free offline path-traced photo from the current camera pose.",
                "photo capture screenshot path tracing 拍照 截图 路径追踪 摄影",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.ACTION, false,
                () -> {
                    try {
                        h.graphics().triggerPhotoCapture();
                        h.closeForPhoto();
                    } catch (IOException e) {
                        h.notifyError("无法触发拍照", "Could not trigger photo capture", e);
                    }
                },
                () -> {
                    if (h.minecraft().level == null) return txt(h, "进入存档世界后才可拍摄照片", "Load into a world first");
                    if (isRayTracingUnavailable(h)) return txt(h, "路径追踪拍照需要 DX12 SM6 硬件光追", "Requires DX12 SM6 hardware ray tracing");
                    return null;
                }));

        list.add(action("photo_open_dir", Category.PHOTO_ADVANCED, g1Zh, g1En,
                "打开照片文件夹", "Open Photos Folder",
                "在系统文件管理器中打开路径追踪照片保存目录。",
                "Opens the PathTracing screenshot output directory in Explorer.",
                "folder directory screenshot photo 文件夹 目录 照片",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.ACTION, false,
                () -> {
                    try {
                        Path dir = BridgeGraphicsSettings.photoDirectory();
                        Files.createDirectories(dir);
                        Util.getPlatform().openPath(dir);
                    } catch (IOException e) {
                        h.notifyError("无法打开照片文件夹", "Could not open photo folder", e);
                    }
                }, () -> null));

        list.add(cycle("photo_res", Category.PHOTO_ADVANCED, g1Zh, g1En,
                "照片分辨率（长边）", "Photo Resolution (Long Edge)",
                "离线路径追踪照片的输出长边像素，自动保持当前窗口纵横比（默认 2560）。",
                "Long-edge resolution in pixels for path-traced photos, preserving camera aspect ratio.",
                "photo resolution 1080p 2k 4k 分辨率 照片 像素",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.PHOTO_LONG_EDGE), Setting.PHOTO_LONG_EDGE.effectiveKey,
                List.of(FOLLOW, 1920, 2560, 3200, 3840),
                () -> h.graphics().get(Setting.PHOTO_LONG_EDGE),
                v -> h.saveGraphics(Map.of(Setting.PHOTO_LONG_EDGE, v)),
                v -> switch (v) {
                    case FOLLOW -> txt(h, "默认 (2K · 2560)", "Default (2560)");
                    case 1920 -> "1080p (1920)";
                    case 2560 -> "2K (2560)";
                    case 3200 -> "3K (3200)";
                    default -> "4K (3840)";
                },
                () -> h.graphics().isOverridden(Setting.PHOTO_LONG_EDGE),
                () -> h.saveGraphics(Map.of(Setting.PHOTO_LONG_EDGE, FOLLOW)),
                () -> null, () -> h.effective().get("mcue.Photo.LongEdge")));

        list.add(cycle("photo_samples", Category.PHOTO_ADVANCED, g1Zh, g1En,
                "路径追踪采样数", "Path Tracing Samples",
                "每像素路径追踪采样次数。采样越多噪点越少、材质与光影越细腻，但渲染耗时线性增加。",
                "Samples per pixel for offline path tracing. More samples reduce noise at longer render times.",
                "photo samples spp path tracing 采样 噪点 路径追踪",
                Impact.EXTREME, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.PHOTO_SAMPLES), Setting.PHOTO_SAMPLES.effectiveKey,
                List.of(FOLLOW, 64, 128, 256, 512, 1024),
                () -> h.graphics().get(Setting.PHOTO_SAMPLES),
                v -> h.saveGraphics(Map.of(Setting.PHOTO_SAMPLES, v)),
                v -> switch (v) {
                    case FOLLOW -> txt(h, "默认 (256)", "Default (256)");
                    case 64 -> txt(h, "快速 (64)", "Fast (64)");
                    case 128 -> txt(h, "均衡 (128)", "Balanced (128)");
                    case 256 -> txt(h, "标准 (256)", "Standard (256)");
                    case 512 -> txt(h, "精细 (512)", "High (512)");
                    default -> txt(h, "极致 (1024)", "Ultra (1024)");
                },
                () -> h.graphics().isOverridden(Setting.PHOTO_SAMPLES),
                () -> h.saveGraphics(Map.of(Setting.PHOTO_SAMPLES, FOLLOW)),
                () -> null, () -> h.effective().get("mcue.Photo.Samples")));

        list.add(cycle("photo_exr", Category.PHOTO_ADVANCED, g1Zh, g1En,
                "同时保存线性 HDR (.exr)", "Also Save Linear HDR (.exr)",
                "关闭后仅保存常规 sRGB PNG 图片，可大幅节省磁盘空间；开启则额外输出供后期调色用的 EXR 原片。",
                "Disable to save PNG only and save disk space. Enable to also export linear HDR .exr for grading.",
                "photo exr hdr png format 格式 空间",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.PHOTO_SAVE_EXR), Setting.PHOTO_SAVE_EXR.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.PHOTO_SAVE_EXR),
                v -> h.saveGraphics(Map.of(Setting.PHOTO_SAVE_EXR, v)),
                v -> v == FOLLOW ? txt(h, "默认（PNG + EXR）", "Default (PNG + EXR)")
                        : (v == 1 ? txt(h, "PNG + EXR", "PNG + EXR") : txt(h, "仅 PNG（省空间）", "PNG Only")),
                () -> h.graphics().isOverridden(Setting.PHOTO_SAVE_EXR),
                () -> h.saveGraphics(Map.of(Setting.PHOTO_SAVE_EXR, FOLLOW)),
                () -> null, () -> reportRawOnOff(h, "mcue.Photo.SaveExr")));

        list.add(cycle("photo_denoiser", Category.PHOTO_ADVANCED, g1Zh, g1En,
                "路径追踪 AI 降噪", "Path Tracing Denoiser",
                "自动消除低中采样下的光线追踪噪点（推荐保持开启）。",
                "Removes Monte Carlo ray-tracing noise in captured photos (recommended).",
                "photo denoiser noise 降噪 噪点",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.PHOTO_DENOISER), Setting.PHOTO_DENOISER.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.PHOTO_DENOISER),
                v -> h.saveGraphics(Map.of(Setting.PHOTO_DENOISER, v)),
                v -> v == FOLLOW ? txt(h, "默认（开）", "Default (On)") : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.PHOTO_DENOISER),
                () -> h.saveGraphics(Map.of(Setting.PHOTO_DENOISER, FOLLOW)),
                () -> null, () -> reportRawOnOff(h, "mcue.Photo.Denoiser")));

        String g2Zh = "界面与性能监控", g2En = "Overlay & HUD";
        list.add(cycle("debug_overlay", Category.PHOTO_ADVANCED, g2Zh, g2En,
                "右上角 UE 性能浮窗", "UE Performance Overlay",
                "实时开关 UE 窗口右上角的帧率、区段与视距监控浮窗，并记住下次启动状态。释放鼠标后仍可按 F8 临时切换。",
                "Immediately toggles Unreal's top-right FPS/section overlay and persists it for next launch. F8 still works.",
                "fps overlay debug panel f8 性能 浮窗 监控 帧率显示",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.KEPT_ON_PRESET, false,
                Set.of(Setting.DEBUG_OVERLAY), null,
                List.of(1, 0),
                () -> {
                    int ov = h.graphics().get(Setting.DEBUG_OVERLAY);
                    if (ov != FOLLOW) return ov;
                    return h.bridge().debugPanel() == BridgeSettings.Toggle.OFF ? 0 : 1;
                },
                v -> {
                    h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(),
                            v == 1 ? BridgeSettings.Toggle.ON : BridgeSettings.Toggle.OFF,
                            h.bridge().gradientNormals(), h.bridge().relief(), h.bridge().connectedGlass()));
                    h.saveGraphics(Map.of(Setting.DEBUG_OVERLAY, v));
                },
                v -> onOff(h, v),
                () -> h.bridge().debugPanel() != BridgeSettings.Toggle.STARTUP,
                () -> {
                    h.saveBridge(new BridgeSettings(h.bridge().hudMaxFps(), BridgeSettings.Toggle.STARTUP,
                            h.bridge().gradientNormals(), h.bridge().relief(), h.bridge().connectedGlass()));
                    h.saveGraphics(Map.of(Setting.DEBUG_OVERLAY, 1));
                },
                () -> null, () -> null));

        list.add(slider("hud_max_fps", Category.PHOTO_ADVANCED, g2Zh, g2En,
                "HUD 共享纹理刷新上限", "HUD Refresh Cap",
                "限制 Minecraft 透明界面通过共享纹理推送给 UE 的帧率上限（0 为跟随画质档，重启桥接后生效）。",
                "Caps shared-texture HUD transfer FPS on next launch (leftmost follows quality preset).",
                "hud ui refresh fps limit 界面 刷新率 上限",
                Impact.LOW, ApplyTiming.RESTART_UE, Persistence.KEPT_ON_PRESET, false, false, Set.of(), null,
                9, 250, 5,
                () -> h.bridge().hudMaxFps() == 0 ? 9 : h.bridge().hudMaxFps(),
                v -> h.saveBridge(new BridgeSettings(v <= 9 ? 0 : v, h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief(), h.bridge().connectedGlass())),
                v -> v <= 9 ? txt(h, "跟随画质档", "Auto (Follow Preset)") : v + " FPS",
                () -> h.bridge().hudMaxFps() != 0,
                () -> h.saveBridge(new BridgeSettings(0, h.bridge().debugPanel(),
                        h.bridge().gradientNormals(), h.bridge().relief(), h.bridge().connectedGlass())),
                () -> null, () -> null));

        String g3Zh = "配置档案管理", g3En = "Configuration Profiles";
        list.add(action("profile_export", Category.PHOTO_ADVANCED, g3Zh, g3En,
                "导出当前配置档案", "Export Current Profile",
                "将当前的画质档、所有详细设置与偏好保存为独立配置文件，方便备份或分享给其他玩家。",
                "Exports your current quality tier, graphics overrides, and visual preferences into voxglint-profiles.",
                "export save backup profile 导出 备份 分享 配置",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.ACTION, false,
                () -> {
                    try {
                        Path file = VoxGlintProfiles.exportProfile(h.quality(), h.graphics(), h.bridge());
                        h.notifyInfo("已导出配置：" + file.getFileName(), "Exported profile: " + file.getFileName());
                    } catch (IOException e) {
                        h.notifyError("导出配置失败", "Failed to export profile", e);
                    }
                }, () -> null));

        list.add(action("profile_import", Category.PHOTO_ADVANCED, g3Zh, g3En,
                "导入最新配置档案", "Import Latest Profile",
                "从 voxglint-profiles 文件夹中加载最近修改的 .properties 配置文件并立即应用。",
                "Loads and applies the most recently modified .properties file in voxglint-profiles.",
                "import load restore profile 导入 恢复 配置",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.ACTION, false,
                () -> {
                    try {
                        var res = VoxGlintProfiles.importLatestProfile(h.graphics());
                        if (res == null) {
                            h.notifyInfo("未找到可导入的配置文件", "No profile files found in voxglint-profiles");
                        } else {
                            h.setQuality(res.quality());
                            h.setBridge(res.bridgeSettings());
                            h.notifyInfo("已导入最新配置档案", "Imported latest profile");
                            h.rebuildList();
                        }
                    } catch (IOException e) {
                        h.notifyError("导入配置失败", "Failed to import profile", e);
                    }
                }, () -> null));

        list.add(action("profile_open_dir", Category.PHOTO_ADVANCED, g3Zh, g3En,
                "打开配置档案文件夹", "Open Profiles Folder",
                "打开 voxglint-profiles 文件夹以管理或拖入其他玩家分享的配置档案。",
                "Opens the voxglint-profiles directory in Explorer.",
                "folder profile directory 文件夹 配置",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.ACTION, false,
                () -> {
                    try {
                        Path dir = VoxGlintProfiles.directory();
                        Files.createDirectories(dir);
                        Util.getPlatform().openPath(dir);
                    } catch (IOException e) {
                        h.notifyError("无法打开配置目录", "Could not open profiles folder", e);
                    }
                }, () -> null));

        String g4Zh = "专家 / 引擎微调（高级用户）", g4En = "Expert / Engine Tuning";
        list.add(new OptionEntry("expert_fold", Category.PHOTO_ADVANCED, g4Zh, g4En,
                "引擎底层参数微调（13 项）", "Engine Low-Level Tuning (13 items)",
                "包含 TSR 历史倍率、SMRT 阴影射线、反射反弹/粗糙度、BLAS 光追区块粒度与精确光源预算等引擎内部参数。普通玩家无需修改。",
                "Exposes internal UE5 parameters: TSR history, SMRT rays, reflection bounces/roughness, BLAS region size, and light budgets.",
                "expert advanced cvar blas smrt history bounces roughness 专家 高级 底层 引擎",
                Impact.NONE, ApplyTiming.INSTANT, Persistence.ACTION, ControlKind.FOLD_TOGGLE,
                false, false, Set.of(), null, 0, 1, 1, List.of(),
                () -> h.expertExpanded() ? 1 : 0,
                v -> { h.setExpertExpanded(v == 1); h.rebuildList(); },
                v -> v == 1 ? txt(h, "收起 ▲", "Collapse ▲") : txt(h, "展开 ▼", "Expand ▼"),
                () -> h.graphics().hasDomainOverrides(BridgeGraphicsSettings.Domain.EXPERT),
                () -> {}, () -> null, () -> null,
                () -> { h.setExpertExpanded(!h.expertExpanded()); h.rebuildList(); }));

        // Expert items (visible when expanded or when matched by search)
        addExpertItems(list, h, g4Zh, g4En);
    }

    private static void addExpertItems(List<OptionEntry> list, Host h, String gZh, String gEn) {
        list.add(slider("exp_history_scale", Category.PHOTO_ADVANCED, gZh, gEn,
                "TSR 历史缓冲分辨率", "TSR History Resolution",
                "历史缓冲相对输出分辨率的比例（100%–200%）。提高可减少运动鬼影，增加显存和 GPU 开销。",
                "TSR history buffer resolution relative to output (100%-200%). Higher improves motion stability at VRAM/GPU cost.",
                "tsr history buffer ghosting 历史 缓冲 鬼影",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.HISTORY_SCALE), Setting.HISTORY_SCALE.effectiveKey,
                100, 200, 10,
                () -> h.graphics().getOrDefault(Setting.HISTORY_SCALE, 100),
                v -> h.saveGraphics(Map.of(Setting.HISTORY_SCALE, v)),
                v -> h.graphics().isOverridden(Setting.HISTORY_SCALE) ? v + "%" : follow(h),
                () -> h.graphics().isOverridden(Setting.HISTORY_SCALE),
                () -> h.saveGraphics(Map.of(Setting.HISTORY_SCALE, FOLLOW)),
                () -> null, () -> reportPercentInt(h, "r.TSR.History.ScreenPercentage")));

        list.add(slider("exp_shadow_rays", Category.PHOTO_ADVANCED, gZh, gEn,
                "方向光 VSM 阴影采样射线", "Directional Shadow Rays (SMRT)",
                "使用虚拟阴影贴图时每像素追踪的软阴影射线数（1–8）。提高可减少软阴影噪点。",
                "SMRT rays per pixel for sun/moon virtual shadow maps (1-8).",
                "smrt shadow rays vsm noise 射线 阴影采样",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.SHADOW_RAYS), Setting.SHADOW_RAYS.effectiveKey,
                1, 8, 1,
                () -> h.graphics().getOrDefault(Setting.SHADOW_RAYS, effectiveInt(h, "r.Shadow.Virtual.SMRT.RayCountDirectional", 4)),
                v -> h.saveGraphics(Map.of(Setting.SHADOW_RAYS, v)),
                v -> h.graphics().isOverridden(Setting.SHADOW_RAYS) ? Integer.toString(v) : follow(h),
                () -> h.graphics().isOverridden(Setting.SHADOW_RAYS),
                () -> h.saveGraphics(Map.of(Setting.SHADOW_RAYS, FOLLOW)),
                () -> null, () -> h.effective().get("r.Shadow.Virtual.SMRT.RayCountDirectional")));

        list.add(slider("exp_refl_bounces", Category.PHOTO_ADVANCED, gZh, gEn,
                "硬件反射最大反弹次数", "Reflection Max Bounces",
                "Lumen 硬件命中点反射的递归反弹上限（1–8）。设为 1 可为中端光追卡节省明显开销。",
                "Max bounces for Lumen hardware Hit Lighting reflections (1-8). 1 saves significant GPU cost.",
                "reflection bounces lumen rt 反射 反弹",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.REFLECTION_BOUNCES), Setting.REFLECTION_BOUNCES.effectiveKey,
                1, 8, 1,
                () -> h.graphics().getOrDefault(Setting.REFLECTION_BOUNCES, 2),
                v -> h.saveGraphics(Map.of(Setting.REFLECTION_BOUNCES, v)),
                v -> h.graphics().isOverridden(Setting.REFLECTION_BOUNCES) ? Integer.toString(v) : follow(h),
                () -> h.graphics().isOverridden(Setting.REFLECTION_BOUNCES),
                () -> h.saveGraphics(Map.of(Setting.REFLECTION_BOUNCES, FOLLOW)),
                () -> !isHardwareReflectionsActive(h) ? txt(h, "需开启 Lumen 硬件反射", "Requires Lumen hardware reflections") : null,
                () -> h.effective().get("r.Lumen.Reflections.MaxBounces")));

        list.add(slider("exp_refl_roughness", Category.PHOTO_ADVANCED, gZh, gEn,
                "反射粗糙度追踪上限", "Reflection Max Roughness",
                "仅粗糙度低于此阈值的表面才会发射 Lumen 硬件反射射线（0%–100%）。",
                "Traces Lumen hardware reflection rays on surfaces up to this roughness threshold (0%-100%).",
                "roughness trace lumen reflection 粗糙度 反射",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.REFLECTION_ROUGHNESS), Setting.REFLECTION_ROUGHNESS.effectiveKey,
                0, 100, 5,
                () -> h.graphics().getOrDefault(Setting.REFLECTION_ROUGHNESS, 40),
                v -> h.saveGraphics(Map.of(Setting.REFLECTION_ROUGHNESS, v)),
                v -> h.graphics().isOverridden(Setting.REFLECTION_ROUGHNESS) ? v + "%" : follow(h),
                () -> h.graphics().isOverridden(Setting.REFLECTION_ROUGHNESS),
                () -> h.saveGraphics(Map.of(Setting.REFLECTION_ROUGHNESS, FOLLOW)),
                () -> !isHardwareReflectionsActive(h) ? txt(h, "需开启 Lumen 硬件反射", "Requires Lumen hardware reflections") : null,
                () -> reportPercentFloat(h, "r.Lumen.Reflections.MaxRoughnessToTrace")));

        list.add(cycle("exp_refl_screen_traces", Category.PHOTO_ADVANCED, gZh, gEn,
                "反射屏幕空间追踪", "Reflection Screen Traces",
                "允许 Lumen 硬件反射优先采样屏幕中已有颜色以提速；关闭后更多由硬件光追直接命中计算。",
                "Lets Lumen hardware reflections start with on-screen traces before hardware rays.",
                "screen traces lumen reflection 屏幕追踪",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true,
                Set.of(Setting.REFLECTION_SCREEN_TRACES), Setting.REFLECTION_SCREEN_TRACES.effectiveKey,
                List.of(FOLLOW, 1, 0),
                () -> h.graphics().get(Setting.REFLECTION_SCREEN_TRACES),
                v -> h.saveGraphics(Map.of(Setting.REFLECTION_SCREEN_TRACES, v)),
                v -> v == FOLLOW ? follow(h) : onOff(h, v),
                () -> h.graphics().isOverridden(Setting.REFLECTION_SCREEN_TRACES),
                () -> h.saveGraphics(Map.of(Setting.REFLECTION_SCREEN_TRACES, FOLLOW)),
                () -> !isHardwareReflectionsActive(h) ? txt(h, "需开启 Lumen 硬件反射", "Requires Lumen hardware reflections") : null,
                () -> reportRawOnOff(h, "r.Lumen.Reflections.ScreenTraces")));

        list.add(cycle("exp_region_edge", Category.PHOTO_ADVANCED, gZh, gEn,
                "光追区块粒度 (Region / BLAS)", "RT Region Size (BLAS)",
                "改变地形常规绘制与光追 BLAS 共用的 Region 边长（默认 4×4）。2×2 可显著降低挖放方块时的局部 BLAS 重建卡顿，但在高视距下组件数更多。下次启动 UE 生效。",
                "Changes shared raster/BLAS region edge (default 4x4). 2x2 lowers edit rebuild stalls at more components. Applies after restart.",
                "region blas chunk edge stutter 区块 粒度 光追 重启",
                Impact.MEDIUM, ApplyTiming.RESTART_UE, Persistence.PRESET_CONTROLLED, true,
                Set.of(Setting.REGION_EDGE), Setting.REGION_EDGE.effectiveKey,
                List.of(FOLLOW, 1, 2, 4),
                () -> h.graphics().get(Setting.REGION_EDGE),
                v -> h.saveGraphics(Map.of(Setting.REGION_EDGE, v)),
                v -> v == FOLLOW ? follow(h) : v + "×" + v + (h.chinese() ? " 区块" : " chunks"),
                () -> h.graphics().isOverridden(Setting.REGION_EDGE),
                () -> h.saveGraphics(Map.of(Setting.REGION_EDGE, FOLLOW)),
                () -> null,
                () -> {
                    String raw = h.effective().get("RegionChunks");
                    return raw == null ? null : raw + "×" + raw;
                }));

        list.add(slider("exp_terrain_range", Category.PHOTO_ADVANCED, gZh, gEn,
                "地形光追半径", "Terrain Ray-Tracing Radius",
                "限制进入光追场景的地形水平半径（区块），普通可见视距不受影响。最左为跟随已加载地形。",
                "Limits horizontal radius (chunks) of terrain included in ray tracing without shrinking raster view distance.",
                "terrain range rt radius chunk 光追半径 地形",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.TERRAIN_RANGE), Setting.TERRAIN_RANGE.effectiveKey,
                0, 32, 1,
                () -> h.graphics().get(Setting.TERRAIN_RANGE) == FOLLOW ? 0 : h.graphics().get(Setting.TERRAIN_RANGE),
                v -> h.saveGraphics(Map.of(Setting.TERRAIN_RANGE, v <= 0 ? FOLLOW : v)),
                v -> v <= 0 ? txt(h, "跟随已加载地形", "Loaded Terrain") : v + (h.chinese() ? " 区块" : " chunks"),
                () -> h.graphics().isOverridden(Setting.TERRAIN_RANGE),
                () -> h.saveGraphics(Map.of(Setting.TERRAIN_RANGE, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "需开启硬件光追", "Requires Hardware Ray Tracing") : null,
                () -> {
                    String raw = h.effective().get("mcue.RayTracing.TerrainRange");
                    return raw == null ? null : ("0".equals(raw) ? txt(h, "全部已加载", "All Loaded") : raw + (h.chinese() ? " 区块" : " chunks"));
                }));

        list.add(slider("exp_dynamic_range", Category.PHOTO_ADVANCED, gZh, gEn,
                "动态对象光追距离", "Dynamic RT Distance",
                "限制生物、箱子、掉落物等动态实体加入光追场景的距离（格）。0 停用。",
                "Max distance in blocks for entities, chests, and items to enter ray tracing. 0 disables.",
                "dynamic entity raytracing distance 动态对象 实体 光追距离",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.DYNAMIC_RANGE), Setting.DYNAMIC_RANGE.effectiveKey,
                0, 36, 2,
                () -> h.graphics().getOrDefault(Setting.DYNAMIC_RANGE, 32),
                v -> h.saveGraphics(Map.of(Setting.DYNAMIC_RANGE, v)),
                v -> h.graphics().isOverridden(Setting.DYNAMIC_RANGE) ? (v == 0 ? off(h) : v + (h.chinese() ? " 格" : " blocks")) : follow(h),
                () -> h.graphics().isOverridden(Setting.DYNAMIC_RANGE),
                () -> h.saveGraphics(Map.of(Setting.DYNAMIC_RANGE, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "需开启硬件光追", "Requires Hardware Ray Tracing") : null,
                () -> h.effective().get("DynamicRange")));

        list.add(slider("exp_dynamic_budget", Category.PHOTO_ADVANCED, gZh, gEn,
                "动态对象光追部件预算", "Dynamic RT Part Budget",
                "同时提交到硬件光追场景的最大动态部件数（0–256，默认 96）。",
                "Maximum dynamic object parts submitted to ray tracing simultaneously (0-256, default 96).",
                "dynamic budget parts rt 部件 预算",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.DYNAMIC_BUDGET), Setting.DYNAMIC_BUDGET.effectiveKey,
                0, 256, 8,
                () -> h.graphics().getOrDefault(Setting.DYNAMIC_BUDGET, 96),
                v -> h.saveGraphics(Map.of(Setting.DYNAMIC_BUDGET, v)),
                v -> h.graphics().isOverridden(Setting.DYNAMIC_BUDGET) ? Integer.toString(v) : follow(h),
                () -> h.graphics().isOverridden(Setting.DYNAMIC_BUDGET),
                () -> h.saveGraphics(Map.of(Setting.DYNAMIC_BUDGET, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "需开启硬件光追", "Requires Hardware Ray Tracing") : null,
                () -> h.effective().get("DynamicBudget")));

        list.add(cycle("exp_mega_samples", Category.PHOTO_ADVANCED, gZh, gEn,
                "MegaLights 每像素采样数", "MegaLights Samples / Pixel",
                "MegaLights 生效时每像素追踪的直接光照样本数（1 / 2 / 4）。",
                "Direct lighting samples per pixel when MegaLights is active (1 / 2 / 4).",
                "megalights samples spp 采样",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true,
                Set.of(Setting.MEGA_LIGHT_SAMPLES), Setting.MEGA_LIGHT_SAMPLES.effectiveKey,
                List.of(FOLLOW, 1, 2, 4),
                () -> h.graphics().get(Setting.MEGA_LIGHT_SAMPLES),
                v -> h.saveGraphics(Map.of(Setting.MEGA_LIGHT_SAMPLES, v)),
                v -> v == FOLLOW ? follow(h) : Integer.toString(v),
                () -> h.graphics().isOverridden(Setting.MEGA_LIGHT_SAMPLES),
                () -> h.saveGraphics(Map.of(Setting.MEGA_LIGHT_SAMPLES, FOLLOW)),
                () -> !"1".equals(h.effective().get("MegaLights")) && !h.effective().isEmpty()
                        ? txt(h, "MegaLights 当前未激活", "MegaLights is currently inactive") : null,
                () -> h.effective().get("r.MegaLights.NumSamplesPerPixel")));

        list.add(slider("exp_mega_budget", Category.PHOTO_ADVANCED, gZh, gEn,
                "点光源精确上限（MegaLights）", "Exact Point Light Cap (MegaLights)",
                "MegaLights 路径下的最大动态点光源数量（0–2048）。",
                "Exact max dynamic block lights on the MegaLights path (0-2048).",
                "point light budget megalights 点光源 预算",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.MEGA_LIGHT_BUDGET), Setting.MEGA_LIGHT_BUDGET.effectiveKey,
                0, 2048, 64,
                () -> h.graphics().getOrDefault(Setting.MEGA_LIGHT_BUDGET, effectiveInt(h, "mcue.PointLights.Max", 1024)),
                v -> h.saveGraphics(Map.of(Setting.MEGA_LIGHT_BUDGET, v)),
                v -> h.graphics().isOverridden(Setting.MEGA_LIGHT_BUDGET) ? Integer.toString(v) : follow(h),
                () -> h.graphics().isOverridden(Setting.MEGA_LIGHT_BUDGET),
                () -> h.saveGraphics(Map.of(Setting.MEGA_LIGHT_BUDGET, FOLLOW)),
                () -> !"1".equals(h.effective().get("MegaLights")) && !h.effective().isEmpty()
                        ? txt(h, "当前未走 MegaLights 路径", "MegaLights path inactive") : null,
                () -> h.effective().get("mcue.PointLights.Max")));

        list.add(slider("exp_fallback_budget", Category.PHOTO_ADVANCED, gZh, gEn,
                "点光源精确上限（延迟回退）", "Exact Point Light Cap (Deferred)",
                "软件/延迟路径下的最大动态点光源数量（0–128）。",
                "Exact max dynamic block lights on the deferred fallback path (0-128).",
                "point light budget deferred fallback 延迟 点光源 预算",
                Impact.HIGH, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true, false,
                Set.of(Setting.FALLBACK_LIGHT_BUDGET), Setting.FALLBACK_LIGHT_BUDGET.effectiveKey,
                0, 128, 4,
                () -> h.graphics().getOrDefault(Setting.FALLBACK_LIGHT_BUDGET, 48),
                v -> h.saveGraphics(Map.of(Setting.FALLBACK_LIGHT_BUDGET, v)),
                v -> h.graphics().isOverridden(Setting.FALLBACK_LIGHT_BUDGET) ? Integer.toString(v) : follow(h),
                () -> h.graphics().isOverridden(Setting.FALLBACK_LIGHT_BUDGET),
                () -> h.saveGraphics(Map.of(Setting.FALLBACK_LIGHT_BUDGET, FOLLOW)),
                () -> "1".equals(h.effective().get("MegaLights"))
                        ? txt(h, "当前正在使用 MegaLights，延迟上限未使用", "MegaLights active; deferred cap unused") : null,
                () -> h.effective().get("mcue.PointLights.FallbackMax")));

        list.add(cycle("exp_fog_grid_exact", Category.PHOTO_ADVANCED, gZh, gEn,
                "体积雾精确体素格宽", "Exact Volumetric Fog Grid",
                "直接指定 r.VolumetricFog.GridPixelSize（格宽越小越精细，0 关闭）。",
                "Direct control over r.VolumetricFog.GridPixelSize (smaller = finer, 0 = off).",
                "fog grid pixel size froxel 体积雾 格宽",
                Impact.MEDIUM, ApplyTiming.INSTANT, Persistence.PRESET_CONTROLLED, true,
                Set.of(Setting.FOG_GRID), Setting.FOG_GRID.effectiveKey,
                List.of(FOLLOW, 0, 8, 12, 16, 24, 32),
                () -> h.graphics().get(Setting.FOG_GRID),
                v -> h.saveGraphics(Map.of(Setting.FOG_GRID, v)),
                v -> v == FOLLOW ? follow(h) : (v == 0 ? off(h) : v + " px"),
                () -> h.graphics().isOverridden(Setting.FOG_GRID),
                () -> h.saveGraphics(Map.of(Setting.FOG_GRID, FOLLOW)),
                () -> isSoftwarePath(h) ? txt(h, "需开启硬件光照", "Requires hardware lighting") : null,
                () -> h.effective().get("r.VolumetricFog.GridPixelSize")));
    }

    // -------------------------------------------------------------------------
    // Composite Preset Mappers
    // -------------------------------------------------------------------------
    static int presetIndex(BridgeQuality quality) {
        return switch (quality) {
            case Performance -> 0;
            case Balanced -> 1;
            case High, Startup, Baseline -> 2;
            case Ultra -> 3;
        };
    }

    static BridgeQuality presetFromIndex(int index) {
        return switch (index) {
            case 0 -> BridgeQuality.Performance;
            case 1 -> BridgeQuality.Balanced;
            case 3 -> BridgeQuality.Ultra;
            default -> BridgeQuality.High;
        };
    }

    static String qualityName(BridgeQuality quality, boolean zh) {
        return switch (quality) {
            case Performance -> zh ? "性能" : "Performance";
            case Balanced -> zh ? "均衡" : "Balanced";
            case High -> zh ? "高" : "High";
            case Ultra -> zh ? "极高" : "Ultra";
            case Baseline -> zh ? "默认高" : "High";
            case Startup -> zh ? "启动档" : "Startup";
        };
    }

    private static int inferBlockLightTier(Host h) {
        int mega = h.graphics().get(Setting.MEGA_LIGHT_BUDGET);
        int fb = h.graphics().get(Setting.FALLBACK_LIGHT_BUDGET);
        if (mega == FOLLOW && fb == FOLLOW) return FOLLOW;
        int ref = mega != FOLLOW ? mega : (fb * 16);
        if (ref == 0) return 0;
        if (ref <= 256) return 1;
        if (ref <= 512) return 2;
        if (ref <= 1024) return 3;
        return 4;
    }

    private static void applyBlockLightTier(Host h, int tier) {
        var map = new LinkedHashMap<Setting, Integer>();
        switch (tier) {
            case FOLLOW -> {
                map.put(Setting.MEGA_LIGHT_BUDGET, FOLLOW);
                map.put(Setting.FALLBACK_LIGHT_BUDGET, FOLLOW);
                map.put(Setting.MEGA_LIGHT_SAMPLES, FOLLOW);
            }
            case 0 -> {
                map.put(Setting.MEGA_LIGHT_BUDGET, 0);
                map.put(Setting.FALLBACK_LIGHT_BUDGET, 0);
            }
            case 1 -> {
                map.put(Setting.MEGA_LIGHT_BUDGET, 256);
                map.put(Setting.FALLBACK_LIGHT_BUDGET, 24);
                map.put(Setting.MEGA_LIGHT_SAMPLES, 1);
            }
            case 2 -> {
                map.put(Setting.MEGA_LIGHT_BUDGET, 512);
                map.put(Setting.FALLBACK_LIGHT_BUDGET, 48);
                map.put(Setting.MEGA_LIGHT_SAMPLES, 2);
            }
            case 3 -> {
                map.put(Setting.MEGA_LIGHT_BUDGET, 1024);
                map.put(Setting.FALLBACK_LIGHT_BUDGET, 80);
                map.put(Setting.MEGA_LIGHT_SAMPLES, 2);
            }
            default -> {
                map.put(Setting.MEGA_LIGHT_BUDGET, 2048);
                map.put(Setting.FALLBACK_LIGHT_BUDGET, 128);
                map.put(Setting.MEGA_LIGHT_SAMPLES, 4);
            }
        }
        h.saveGraphics(map);
    }

    private static int inferWaterPreset(Host h) {
        var g = h.graphics();
        if (!g.isOverridden(Setting.WATER_P1) && !g.isOverridden(Setting.WATER_P2)
                && !g.isOverridden(Setting.WATER_P3) && !g.isOverridden(Setting.WATER_WAVES)) {
            return FOLLOW;
        }
        if (g.getOrDefault(Setting.WATER_P3, 0) == 1) return 2;
        if (g.getOrDefault(Setting.WATER_P2, 0) == 1) return 1;
        return 0;
    }

    private static void applyWaterPreset(Host h, int preset) {
        var map = new LinkedHashMap<Setting, Integer>();
        switch (preset) {
            case FOLLOW -> {
                for (Setting s : List.of(Setting.WATER_P1, Setting.WATER_P2, Setting.WATER_P3,
                        Setting.WATER_WAVES, Setting.WATER_VOLUME, Setting.WATER_CAUSTICS, Setting.WATER_PARTICLES)) {
                    map.put(s, FOLLOW);
                }
            }
            case 0 -> {
                map.put(Setting.WATER_P1, 1);
                map.put(Setting.WATER_P2, 0);
                map.put(Setting.WATER_P3, 0);
                map.put(Setting.WATER_WAVES, 0);
                map.put(Setting.WATER_VOLUME, 0);
                map.put(Setting.WATER_CAUSTICS, 0);
                map.put(Setting.WATER_PARTICLES, 0);
            }
            case 1 -> {
                map.put(Setting.WATER_P1, 1);
                map.put(Setting.WATER_P2, 1);
                map.put(Setting.WATER_P3, 0);
                map.put(Setting.WATER_WAVES, 0);
                map.put(Setting.WATER_VOLUME, 1);
                map.put(Setting.WATER_CAUSTICS, 1);
                map.put(Setting.WATER_PARTICLES, 1);
            }
            default -> {
                map.put(Setting.WATER_P1, 1);
                map.put(Setting.WATER_P2, 1);
                map.put(Setting.WATER_P3, 1);
                map.put(Setting.WATER_WAVES, 1);
                map.put(Setting.WATER_VOLUME, 1);
                map.put(Setting.WATER_CAUSTICS, 1);
                map.put(Setting.WATER_PARTICLES, 1);
            }
        }
        h.saveGraphics(map);
    }

    static int inferColorStyle(Host h) {
        return inferColorStyle(
                h.graphics().getOrDefault(Setting.SATURATION, 100),
                h.graphics().getOrDefault(Setting.CONTRAST, 100),
                h.graphics().getOrDefault(Setting.COLOR_TEMP, 6500));
    }

    static int inferColorStyle(int sat, int con, int temp) {
        if (sat == 100 && con == 100 && temp == 6500) return 0;
        if (sat == 106 && con == 106 && temp == 7100) return 1;
        if (sat == 118 && con == 104 && temp == 6400) return 2;
        if (sat == 94 && con == 112 && temp == 5900) return 3;
        return -1;
    }

    private static void applyColorStyle(Host h, int style) {
        switch (style) {
            case 1 -> h.saveGraphics(Map.of(Setting.SATURATION, 106, Setting.CONTRAST, 106, Setting.COLOR_TEMP, 7100));
            case 2 -> h.saveGraphics(Map.of(Setting.SATURATION, 118, Setting.CONTRAST, 104, Setting.COLOR_TEMP, 6400));
            case 3 -> h.saveGraphics(Map.of(Setting.SATURATION, 94, Setting.CONTRAST, 112, Setting.COLOR_TEMP, 5900));
            default -> h.saveGraphics(Map.of(Setting.SATURATION, FOLLOW, Setting.CONTRAST, FOLLOW, Setting.COLOR_TEMP, FOLLOW));
        }
    }

    static int inferReliefPreset(Host h) {
        if (!isReliefEnabled(h)) return 0;
        var r = h.bridge().relief();
        if (r.strengthPercent() == 60 && r.thresholdTenthsPercent() == 30 && r.maxTiltDegrees() == 18) return 1;
        if (r.equals(TerrainReliefSettings.DEFAULT)) return 2;
        if (r.strengthPercent() == 150 && r.thresholdTenthsPercent() == 15 && r.maxTiltDegrees() == 35) return 3;
        return -1;
    }

    private static void applyReliefPreset(Host h, int preset) {
        BridgeSettings b = h.bridge();
        switch (preset) {
            case 0 -> h.saveBridge(new BridgeSettings(b.hudMaxFps(), b.debugPanel(),
                    BridgeSettings.Toggle.OFF, b.relief(), b.connectedGlass()));
            case 1 -> h.saveBridge(new BridgeSettings(b.hudMaxFps(), b.debugPanel(),
                    BridgeSettings.Toggle.ON, new TerrainReliefSettings(60, 30, 18), b.connectedGlass()));
            case 2 -> h.saveBridge(new BridgeSettings(b.hudMaxFps(), b.debugPanel(),
                    BridgeSettings.Toggle.ON, TerrainReliefSettings.DEFAULT, b.connectedGlass()));
            default -> h.saveBridge(new BridgeSettings(b.hudMaxFps(), b.debugPanel(),
                    BridgeSettings.Toggle.ON, new TerrainReliefSettings(150, 15, 35), b.connectedGlass()));
        }
    }

    // -------------------------------------------------------------------------
    // Helpers & Formatters
    // -------------------------------------------------------------------------
    private static boolean isReliefEnabled(Host h) {
        return !h.bridge().effectiveRelief().equals(TerrainReliefSettings.DISABLED);
    }

    private static boolean isRayTracingUnavailable(Host h) {
        return "0".equals(h.effective().get("RayTracingAvailable"));
    }

    private static boolean isSoftwarePath(Host h) {
        if (h.graphics().get(Setting.HARDWARE_LIGHTING) == 0) return true;
        if (isRayTracingUnavailable(h)) return true;
        return !h.effective().isEmpty() && "0".equals(h.effective().get("HWRT"));
    }

    private static boolean isHardwareReflectionsActive(Host h) {
        if (isSoftwarePath(h)) return false;
        if (h.graphics().get(Setting.HARDWARE_REFLECTIONS) == 0) return false;
        return h.effective().isEmpty() || "1".equals(h.effective().get("HardReflections"));
    }

    private static boolean isCloudsDisabled(Host h) {
        if (h.graphics().get(Setting.CLOUD_SCALE) == 0) return true;
        return !h.effective().isEmpty() && "0".equals(h.effective().get("r.VolumetricCloud"));
    }

    private static boolean isWaterP3Disabled(Host h) {
        if (h.graphics().get(Setting.WATER_P3) == 0) return true;
        if (h.graphics().get(Setting.WATER_P3) == 1) return false;
        return !h.effective().isEmpty() && "0".equals(h.effective().get("WaterP3"));
    }

    private static int effectiveInt(Host h, String key, int fallback) {
        String raw = h.effective().get(key);
        if (raw == null) return fallback;
        try {
            return (int) Math.round(Double.parseDouble(raw));
        } catch (NumberFormatException ignored) {
            return fallback;
        }
    }

    static String formatEv(int value) {
        int tenths = value - 20;
        String sign = tenths > 0 ? "+" : tenths < 0 ? "-" : "";
        int abs = Math.abs(tenths);
        return sign + (abs / 10) + "." + (abs % 10) + " EV";
    }

    private static String reportEffectiveQuality(Host h) {
        if (h.effective().isEmpty()) return null;
        return reportHwrt(h);
    }

    private static String reportHwrt(Host h) {
        if (h.effective().isEmpty()) return null;
        return "1".equals(h.effective().get("HWRT"))
                ? txt(h, "硬件光追（Lumen Hit Lighting）", "Hardware RT (Lumen)")
                : txt(h, "软件兼容（SSR + MC 补光）", "Software Fallback (SSR)");
    }

    private static String reportAa(Host h) {
        String raw = h.effective().get("r.AntiAliasingMethod");
        if (raw == null) return null;
        return switch (raw) {
            case "4" -> "TSR";
            case "2" -> "TAA";
            case "1" -> "FXAA";
            case "0" -> off(h);
            default -> raw;
        };
    }

    private static String reportRenderScale(Host h) {
        String raw = h.effective().get("r.ScreenPercentage");
        if (raw == null) return null;
        if ("0".equals(raw)) return txt(h, "自动", "Auto");
        try {
            int pct = (int) Math.round(Double.parseDouble(raw));
            int outW = h.minecraft().getWindow().getWidth();
            int outH = h.minecraft().getWindow().getHeight();
            int inW = Math.max(1, Math.round(outW * pct / 100.0f));
            int inH = Math.max(1, Math.round(outH * pct / 100.0f));
            return pct + "% (" + inW + "×" + inH + ")";
        } catch (NumberFormatException ignored) {
            return raw + "%";
        }
    }

    private static String reportRawOnOff(Host h, String key) {
        String raw = h.effective().get(key);
        if (raw == null) return null;
        return "0".equals(raw) ? off(h) : on(h);
    }

    private static String reportPercentInt(Host h, String key) {
        String raw = h.effective().get(key);
        return raw == null ? null : raw + "%";
    }

    private static String reportPercentFloat(Host h, String key) {
        String raw = h.effective().get(key);
        if (raw == null) return null;
        try {
            return Math.round(Double.parseDouble(raw) * 100.0) + "%";
        } catch (NumberFormatException ignored) {
            return raw;
        }
    }

    private static String txt(Host h, String zh, String en) { return h.chinese() ? zh : en; }

    private static String frameMultiplierLabel(Host host, int mode) {
        return switch (mode) {
            case FOLLOW -> txt(host, "默认（2×）", "Default (2x)");
            case 0 -> off(host);
            case 7 -> txt(host, "动态", "Dynamic");
            case 8 -> txt(host, "自动", "Auto");
            default -> mode + "×";
        };
    }

    private static String follow(Host h) { return txt(h, "跟随画质档", "Follow Preset"); }
    private static String on(Host h) { return txt(h, "开启", "On"); }
    private static String off(Host h) { return txt(h, "关闭", "Off"); }
    private static String onOff(Host h, int v) { return v == 0 ? off(h) : on(h); }

    private static OptionEntry cycle(String id, Category cat, String gZh, String gEn,
                                     String nameZh, String nameEn, String descZh, String descEn, String keywords,
                                     Impact impact, ApplyTiming timing, Persistence persistence, boolean expertOnly,
                                     Set<Setting> backing, String effectiveKey, List<Integer> values,
                                     IntSupplier getter, IntConsumer setter, IntFunction<String> formatter,
                                     Supplier<Boolean> modified, Runnable resetter,
                                     Supplier<String> disabled, Supplier<String> reported) {
        return new OptionEntry(id, cat, gZh, gEn, nameZh, nameEn, descZh, descEn, keywords,
                impact, timing, persistence, ControlKind.CYCLE, expertOnly, false, backing, effectiveKey,
                0, 0, 1, values, getter, setter, formatter, modified, resetter, disabled, reported, null);
    }

    private static OptionEntry slider(String id, Category cat, String gZh, String gEn,
                                      String nameZh, String nameEn, String descZh, String descEn, String keywords,
                                      Impact impact, ApplyTiming timing, Persistence persistence,
                                      boolean expertOnly, boolean livePreview, Set<Setting> backing, String effectiveKey,
                                      int min, int max, int step,
                                      IntSupplier getter, IntConsumer setter, IntFunction<String> formatter,
                                      Supplier<Boolean> modified, Runnable resetter,
                                      Supplier<String> disabled, Supplier<String> reported) {
        return new OptionEntry(id, cat, gZh, gEn, nameZh, nameEn, descZh, descEn, keywords,
                impact, timing, persistence, ControlKind.SLIDER, expertOnly, livePreview, backing, effectiveKey,
                min, max, step, List.of(), getter, setter, formatter, modified, resetter, disabled, reported, null);
    }

    private static OptionEntry action(String id, Category cat, String gZh, String gEn,
                                      String nameZh, String nameEn, String descZh, String descEn, String keywords,
                                      Impact impact, ApplyTiming timing, Persistence persistence, boolean expertOnly,
                                      Runnable action, Supplier<String> disabled) {
        return new OptionEntry(id, cat, gZh, gEn, nameZh, nameEn, descZh, descEn, keywords,
                impact, timing, persistence, ControlKind.ACTION, expertOnly, false, Set.of(), null,
                0, 0, 1, List.of(), () -> 0, v -> {}, v -> "", () -> false, () -> {}, disabled, () -> null, action);
    }

    static Set<Setting> settingsForCategory(List<OptionEntry> all, Category category) {
        EnumSet<Setting> set = EnumSet.noneOf(Setting.class);
        for (OptionEntry entry : all) {
            if (entry.category == category) {
                set.addAll(entry.backingSettings);
            }
        }
        return set;
    }

    static Component summaryButtonLabel(boolean chinese) {
        BridgeQuality q = BridgeQuality.read();
        BridgeGraphicsSettings g = BridgeGraphicsSettings.read();
        Map<String, String> eff = BridgeGraphicsSettings.effective();
        String base = chinese ? "VoxGlint 渲染设置" : "VoxGlint Settings";
        String tier = qualityName(presetFromIndex(presetIndex(q)), chinese);
        if (g.hasQualityOrExpertOverrides()) {
            tier = chinese ? "自定义" : "Custom";
        }
        String rt = "";
        if (!eff.isEmpty()) {
            rt = "1".equals(eff.get("HWRT"))
                    ? (chinese ? " · 光追" : " · RT")
                    : (chinese ? " · 兼容" : " · SW");
        }
        return Component.literal(base + " (" + tier + rt + ")…");
    }
}
