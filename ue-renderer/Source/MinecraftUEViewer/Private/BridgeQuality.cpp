#include "BridgeQuality.h"

#include "BridgeFrameGeneration.h"
#include "BridgeUpscaling.h"

#include "Algo/AllOf.h"

#include "GPUProfiler.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "RenderTimer.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "Scalability.h"
#include "SceneViewExtension.h"
#include "UnrealClient.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMinecraftBridgeQuality, Log, All);

namespace
{
    constexpr const TCHAR* PresetNames[] = {TEXT("Performance"), TEXT("Balanced"), TEXT("High"), TEXT("Ultra")};
    constexpr int32 Baseline = INDEX_NONE;
    constexpr int32 Invalid = -2;
    enum EQualityField
    {
        ShadowQuality, GlobalIlluminationQuality, ReflectionQuality, RenderScale, HistoryScale,
        HardwareTracing, HitLighting, LightingMode, MegaLightsAllowed, MegaLightSamples, MegaLightBudget,
        VolumetricCloud, VolumetricFog, FogGrid, ShadowRays, FallbackLightBudget, TerrainRange,
        ReflectionBounces, ReflectionRoughness, ReflectionScreenTraces, WaterRefractionDownsample, ReflectionDownsample,
        LumenProbeStochasticInterpolation,
        AntiAliasingMethod, DynamicResMode, DynamicResBudget, TonemapperSharpen,
        CloudCoverageCvar, CloudMidLayerCvar, CloudCirrusCvar, ContactShadowLengthCvar,
        WaterP1Cvar, WaterP2Cvar, WaterVolumeCvar, WaterCausticsCvar, WaterPlanktonCvar, WaterBubblesCvar,
        WaterFilmCvar, WaterDistortionCvar, WaterP3Cvar, WaterWavesCvar, WaterLabPbrNormalsCvar,
        PhotoLongEdgeCvar, PhotoSamplesCvar, PhotoSaveExrCvar, PhotoDenoiserCvar, GlassLightingCvar,
        CloudScale, CloudLayersField, CloudShadowsField, SunShadowSmoothField,
        FogDensityField, RainWetnessField, PuddleDepressionsField, PrecipDensityField, BlockLightIntensityField,
        RegionEdge, DynamicRange, DynamicBudget, HardwareGi, HardwareReflections, TranslucencyReflectionsField,
        DirectionalShadows, ContactShadowsField, DynamicResField, DynamicResTargetFpsField,
        ExposureModeField, ExposureBiasField, LocalExposureField, CaveBrightnessField,
        BloomIntensityField, SaturationField, ContrastField, ColorTempField, VignetteIntensityField, MotionBlurField,
        WaterAbsorption, WaterScattering, WaterParticlesField, WaterLensFxField,
        DebugOverlayField, PhotoTriggerField, UpscalerField, UpscalerQualityField,
        FrameGenBackendField, FrameGenMultiplierField, FieldCount
    };
    constexpr int32 GroupFieldCount = RenderScale;
    constexpr const TCHAR* Fields[] = {
        TEXT("sg.ShadowQuality"), TEXT("sg.GlobalIlluminationQuality"), TEXT("sg.ReflectionQuality"),
        TEXT("r.ScreenPercentage"), TEXT("r.TSR.History.ScreenPercentage"),
        TEXT("r.Lumen.HardwareRayTracing"), TEXT("r.Lumen.HardwareRayTracing.HitLighting.Allowed"),
        TEXT("r.Lumen.HardwareRayTracing.LightingMode"), TEXT("r.MegaLights.Allowed"),
        TEXT("r.MegaLights.NumSamplesPerPixel"), TEXT("mcue.PointLights.Max"),
        TEXT("r.VolumetricCloud"), TEXT("r.VolumetricFog"), TEXT("r.VolumetricFog.GridPixelSize"),
        TEXT("r.Shadow.Virtual.SMRT.RayCountDirectional"), TEXT("mcue.PointLights.FallbackMax"),
        TEXT("mcue.RayTracing.TerrainRange"), TEXT("r.Lumen.Reflections.MaxBounces"),
        TEXT("r.Lumen.Reflections.MaxRoughnessToTrace"), TEXT("r.Lumen.Reflections.ScreenTraces"),
        TEXT("r.Water.SingleLayer.RefractionDownsampleFactor"), TEXT("r.Lumen.Reflections.DownsampleFactor"),
        TEXT("r.Lumen.ScreenProbeGather.StochasticInterpolation"),
        TEXT("r.AntiAliasingMethod"), TEXT("r.DynamicRes.OperationMode"), TEXT("r.DynamicRes.FrameTimeBudget"),
        TEXT("r.Tonemapper.Sharpen"),
        TEXT("mcue.Clouds.Coverage"), TEXT("mcue.Clouds.MidLayer"), TEXT("mcue.Clouds.Cirrus"),
        TEXT("mcue.Shadows.ContactLength"),
        TEXT("mc.Water.P1"), TEXT("mc.Water.P2"), TEXT("mc.Water.P2.Volume"), TEXT("mc.Water.P2.Caustics"),
        TEXT("mc.Water.P2.Plankton"), TEXT("mc.Water.P2.Bubbles"), TEXT("mc.Water.P2.Film"),
        TEXT("mc.Water.P2.Distortion"), TEXT("mc.Water.P3"), TEXT("mc.Water.P3.WPO"),
        TEXT("mcue.Water.LabPbrNormalWeight"),
        TEXT("mcue.Photo.LongEdge"), TEXT("mcue.Photo.Samples"), TEXT("mcue.Photo.SaveExr"),
        TEXT("mcue.Photo.Denoiser"), TEXT("mcue.Glass.Lighting")
    };
    constexpr int32 Values[][UE_ARRAY_COUNT(Fields)] = {
        {1, 1, 1, 62, 100, 0, 0, 1, 0, 2,  512, 0, 0, 16, 4},
        {2, 2, 2, 70, 100, 1, 1, 1, 1, 2,  512, 1, 1, 16, 4},
        {3, 2, 3, 75, 100, 1, 1, 1, 1, 4, 1024, 1, 1,  8, 8},
        {3, 3, 3, 90, 100, 1, 1, 1, 1, 4, 2048, 1, 1,  8, 8}
    };
    constexpr float CloudSampleScales[] = {0.0f, 0.65f, 1.00f, 1.50f};
    static_assert(UE_ARRAY_COUNT(Values) == UE_ARRAY_COUNT(PresetNames));
    static_assert(UE_ARRAY_COUNT(Fields) == CloudScale);
    TAutoConsoleVariable<int32> CVarTerrainRayRange(TEXT("mcue.RayTracing.TerrainRange"), 0,
        TEXT("Terrain ray tracing instance radius in Minecraft chunks. Zero uses the loaded terrain range."),
        ECVF_RenderThreadSafe);

    struct FDetailSpec
    {
        const TCHAR* Key;
        EQualityField Field;
        int32 Min;
        int32 Max;
    };

    constexpr FDetailSpec DetailSpecs[] = {
        {TEXT("renderScale"), RenderScale, 50, 100},
        {TEXT("historyScale"), HistoryScale, 100, 200},
        {TEXT("aaMethod"), AntiAliasingMethod, 0, 4},
        {TEXT("dynamicRes"), DynamicResField, 0, 1},
        {TEXT("dynamicResTargetFps"), DynamicResTargetFpsField, 30, 240},
        {TEXT("sharpen"), TonemapperSharpen, 0, 200},
        {TEXT("shadowQuality"), ShadowQuality, 1, 3},
        {TEXT("giQuality"), GlobalIlluminationQuality, 1, 3},
        {TEXT("reflectionQuality"), ReflectionQuality, 1, 3},
        {TEXT("shadowRays"), ShadowRays, 1, 8},
        {TEXT("contactShadows"), ContactShadowsField, 0, 1},
        {TEXT("sunShadowSmooth"), SunShadowSmoothField, 0, 1},
        {TEXT("hardwareLighting"), HardwareTracing, 0, 1},
        {TEXT("hardwareGi"), HardwareGi, 0, 1},
        {TEXT("hardwareReflections"), HardwareReflections, 0, 1},
        {TEXT("translucencyReflections"), TranslucencyReflectionsField, 0, 1},
        {TEXT("reflectionBounces"), ReflectionBounces, 1, 8},
        {TEXT("reflectionRoughness"), ReflectionRoughness, 0, 100},
        {TEXT("reflectionScreenTraces"), ReflectionScreenTraces, 0, 1},
        {TEXT("directionalShadows"), DirectionalShadows, 0, 1},
        {TEXT("megaLights"), MegaLightsAllowed, 0, 1},
        {TEXT("megaLightSamples"), MegaLightSamples, 1, 4},
        {TEXT("megaLightBudget"), MegaLightBudget, 0, 2048},
        {TEXT("fallbackLightBudget"), FallbackLightBudget, 0, 128},
        {TEXT("blockLightIntensity"), BlockLightIntensityField, 25, 250},
        {TEXT("glassLighting"), GlassLightingCvar, 0, 1},
        {TEXT("cloudScale"), CloudScale, 0, 100},
        {TEXT("cloudCoverage"), CloudCoverageCvar, 0, 100},
        {TEXT("cloudLayers"), CloudLayersField, 1, 3},
        {TEXT("cloudShadows"), CloudShadowsField, 0, 1},
        {TEXT("fogGrid"), FogGrid, 0, 32},
        {TEXT("fogDensity"), FogDensityField, 0, 200},
        {TEXT("rainWetness"), RainWetnessField, 0, 2},
        {TEXT("puddleDepressions"), PuddleDepressionsField, 0, 1},
        {TEXT("precipDensity"), PrecipDensityField, 0, 150},
        {TEXT("regionEdge"), RegionEdge, 1, 4},
        {TEXT("terrainRange"), TerrainRange, 1, 32},
        {TEXT("dynamicRange"), DynamicRange, 0, 36},
        {TEXT("dynamicBudget"), DynamicBudget, 0, 256},
        {TEXT("exposureMode"), ExposureModeField, 0, 2},
        {TEXT("exposureBias"), ExposureBiasField, 0, 40},
        {TEXT("localExposure"), LocalExposureField, 0, 100},
        {TEXT("caveBrightness"), CaveBrightnessField, 0, 200},
        {TEXT("bloomIntensity"), BloomIntensityField, 0, 200},
        {TEXT("saturation"), SaturationField, 50, 150},
        {TEXT("contrast"), ContrastField, 70, 130},
        {TEXT("colorTemp"), ColorTempField, 4500, 8500},
        {TEXT("vignetteIntensity"), VignetteIntensityField, 0, 100},
        {TEXT("motionBlur"), MotionBlurField, 0, 100},
        {TEXT("waterP1"), WaterP1Cvar, 0, 1},
        {TEXT("waterP2"), WaterP2Cvar, 0, 1},
        {TEXT("waterP3"), WaterP3Cvar, 0, 1},
        {TEXT("waterWaves"), WaterWavesCvar, 0, 1},
        {TEXT("waterVolume"), WaterVolumeCvar, 0, 1},
        {TEXT("waterCaustics"), WaterCausticsCvar, 0, 1},
        {TEXT("waterParticles"), WaterParticlesField, 0, 1},
        {TEXT("waterLensFx"), WaterLensFxField, 0, 1},
        {TEXT("waterAbsorption"), WaterAbsorption, 25, 200},
        {TEXT("waterScattering"), WaterScattering, 0, 200},
        {TEXT("waterLabPbrNormals"), WaterLabPbrNormalsCvar, 0, 100},
        {TEXT("photoLongEdge"), PhotoLongEdgeCvar, 720, 4096},
        {TEXT("photoSamples"), PhotoSamplesCvar, 32, 1024},
        {TEXT("photoSaveExr"), PhotoSaveExrCvar, 0, 1},
        {TEXT("photoDenoiser"), PhotoDenoiserCvar, 0, 1},
        {TEXT("debugOverlay"), DebugOverlayField, 0, 1},
        {TEXT("photoTrigger"), PhotoTriggerField, 0, 1000000},
        {TEXT("upscaler"), UpscalerField, 1, 3},
        {TEXT("upscalerQuality"), UpscalerQualityField, 1, 2},
        {TEXT("frameGenBackend"), FrameGenBackendField, 1, 3},
        {TEXT("frameGenMultiplier"), FrameGenMultiplierField, 0, 8}
    };

    struct FDetailValues
    {
        int32 Values[FieldCount];

        FDetailValues()
        {
            for (int32& Value : Values) Value = INDEX_NONE;
        }

        bool operator==(const FDetailValues&) const = default;
    };

    int32 Parse(const FString& Raw)
    {
        const FString Value = Raw.TrimStartAndEnd();
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(PresetNames); ++Index)
        {
            if (Value.Equals(PresetNames[Index], ESearchCase::IgnoreCase) || Value == FString::FromInt(Index))
            {
                return Index;
            }
        }
        return Value.Equals(TEXT("Baseline"), ESearchCase::IgnoreCase) ? Baseline : Invalid;
    }

    struct FSettingsFileStamp
    {
        int64 Size = -1;
        uint64 WriteTime = 0;
        bool operator==(const FSettingsFileStamp&) const = default;
    };

    FSettingsFileStamp ReadSettingsFileStamp(const FString& Path)
    {
#if PLATFORM_WINDOWS
        // UE truncates its Windows file timestamps to seconds. Settings can be replaced several times in that interval.
        WIN32_FILE_ATTRIBUTE_DATA Data;
        if (::GetFileAttributesExW(*Path, GetFileExInfoStandard, &Data) &&
            !(Data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            const int64 Size = (int64(Data.nFileSizeHigh) << 32) | Data.nFileSizeLow;
            const uint64 WriteTime = (uint64(Data.ftLastWriteTime.dwHighDateTime) << 32) | Data.ftLastWriteTime.dwLowDateTime;
            return {Size, WriteTime};
        }
#endif
        IFileManager& Files = IFileManager::Get();
        const int64 Size = Files.FileSize(*Path);
        return {Size, Size >= 0 ? uint64(Files.GetTimeStamp(*Path).GetTicks()) : 0};
    }

    struct FQualityState
    {
        FString Path;
        FString LastValue;
        FString DetailsPath;
        double NextRead = 0;
        double NextContentRead = 0;
        FSettingsFileStamp LastQualityStamp;
        FSettingsFileStamp LastDetailsStamp;
        int32 LaunchPreset = Baseline;
        int32 ActivePreset = Invalid;
        int32 TerrainRegionEdge = 4;
        int32 LastDebugOverlay = INDEX_NONE;
        uint32 DebugOverlayRevision = 0;
        int32 InitialPhotoTrigger = 0;
        int32 PhotoTrigger = 0;
        uint32 Revision = 0;
        bool bOwnsTemporaryScalability = false;
        bool bOverridesScalability = false;
        bool bWrotePhotoDir = false;
        int32 PreviousGroups[GroupFieldCount]{};
        FDetailValues ActiveDetails;
        FDetailValues AppliedFields;
        float AppliedRenderPercentage = INDEX_NONE;
        BridgeUpscaling::FSettings Upscaling;
        BridgeUpscaling::EMethod ReportedUpscaler = BridgeUpscaling::EMethod::Pending;
        BridgeFrameGeneration::FStatus FrameGeneration;
    };

    FDetailValues ReadDetails(const FQualityState& State, bool* bRetryRead = nullptr);

    FQualityState& Quality()
    {
        static FQualityState State = []
        {
            FQualityState Value;
            if (!FParse::Value(FCommandLine::Get(), TEXT("BridgeQualityFile="), Value.Path))
            {
                Value.Path = FPlatformMisc::GetEnvironmentVariable(TEXT("MCUE_QUALITY_FILE"));
            }
            if (!Value.Path.IsEmpty())
            {
                Value.Path = FPaths::ConvertRelativePathToFull(Value.Path);
                Value.DetailsPath = FPaths::Combine(FPaths::GetPath(Value.Path), TEXT("minecraftue-graphics.txt"));
                const FDetailValues Initial = ReadDetails(Value);
                const int32 ConfiguredEdge = Initial.Values[RegionEdge];
                if (ConfiguredEdge != INDEX_NONE) Value.TerrainRegionEdge = ConfiguredEdge;
                if (Initial.Values[PhotoTriggerField] != INDEX_NONE)
                {
                    Value.InitialPhotoTrigger = Initial.Values[PhotoTriggerField];
                    Value.PhotoTrigger = Value.InitialPhotoTrigger;
                }
            }
            FString Launch;
            if (FParse::Value(FCommandLine::Get(), TEXT("BridgeQuality="), Launch))
            {
                Value.LaunchPreset = Parse(Launch);
                if (Value.LaunchPreset == Invalid)
                {
                    UE_LOG(LogMinecraftBridgeQuality, Warning, TEXT("Unknown BridgeQuality '%s'; keeping baseline."), *Launch);
                    Value.LaunchPreset = Baseline;
                }
            }
            return Value;
        }();
        return State;
    }

    bool Enabled(const TCHAR* Name)
    {
        const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        return Variable && Variable->GetBool();
    }

    void BeginTemporaryScalability(FQualityState& State)
    {
        for (int32 Index = 0; Index < GroupFieldCount; ++Index)
        {
            State.PreviousGroups[Index] = IConsoleManager::Get().FindConsoleVariable(Fields[Index])->GetInt();
        }
        if (Scalability::IsTemporaryQualityLevelActive())
        {
            return;
        }
        // Leave unrelated groups eligible for normal engine writes while protecting the saved baseline.
        Scalability::FQualityLevels Unmanaged(false);
        Unmanaged.ResolutionQuality = -1;
        Unmanaged.ViewDistanceQuality = -1;
        Unmanaged.AntiAliasingQuality = -1;
        Unmanaged.ShadowQuality = -1;
        Unmanaged.GlobalIlluminationQuality = -1;
        Unmanaged.ReflectionQuality = -1;
        Unmanaged.PostProcessQuality = -1;
        Unmanaged.TextureQuality = -1;
        Unmanaged.EffectsQuality = -1;
        Unmanaged.FoliageQuality = -1;
        Unmanaged.ShadingQuality = -1;
        Unmanaged.LandscapeQuality = -1;
        Scalability::ToggleTemporaryQualityLevels(true, Unmanaged);
        State.bOwnsTemporaryScalability = true;
    }

    FDetailValues ReadDetails(const FQualityState& State, bool* bRetryRead)
    {
        FDetailValues Details;
        if (State.DetailsPath.IsEmpty()) return Details;
        const int64 Bytes = IFileManager::Get().FileSize(*State.DetailsPath);
        if (Bytes < 0) return Details;
        if (Bytes > 8192)
        {
            return State.ActiveDetails;
        }
        FString Text;
        if (!FFileHelper::LoadFileToString(Text, *State.DetailsPath))
        {
            if (bRetryRead) *bRetryRead = true;
            return State.ActiveDetails;
        }
        TArray<FString> Lines;
        Text.ParseIntoArrayLines(Lines);
        for (const FString& Line : Lines)
        {
            FString Key;
            FString Value;
            if (!Line.Split(TEXT("="), &Key, &Value)) continue;
            Key.TrimStartAndEndInline();
            Value.TrimStartAndEndInline();
            int32 Number = 0;
            if (!LexTryParseString(Number, *Value) || FString::FromInt(Number) != Value) continue;
            for (const FDetailSpec& Spec : DetailSpecs)
            {
                if (Key != Spec.Key || Number < Spec.Min || Number > Spec.Max) continue;
                if ((Spec.Field == MegaLightSamples && Number == 3) || (Spec.Field == RegionEdge && Number == 3) ||
                    (Spec.Field == AntiAliasingMethod && Number == 3) ||
                    (Spec.Field == FogGrid && Number > 0 && Number < 8) ||
                    (Spec.Field == FrameGenMultiplierField && Number == 1))
                {
                    break;
                }
                Details.Values[Spec.Field] = Number;
                break;
            }
        }
        return Details;
    }

    FDetailValues FieldsFor(const FQualityState& State, int32 Preset, const FDetailValues& Details)
    {
        FDetailValues Chosen;
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
        {
            Chosen.Values[Index] = Details.Values[Index] != INDEX_NONE
                ? Details.Values[Index] : (Preset == Baseline || Index >= FallbackLightBudget)
                    ? INDEX_NONE : Values[Preset][Index];
        }
        Chosen.Values[WaterRefractionDownsample] = Preset >= 2 ? 1 : INDEX_NONE;
        if (Preset == 1 || Preset == 2) Chosen.Values[ReflectionDownsample] = 2;
        // Blend adjacent Lumen probes in High to reduce grain in indirect light.
        if (Preset == 2) Chosen.Values[LumenProbeStochasticInterpolation] = 0;
        if (Details.Values[ReflectionQuality] != INDEX_NONE)
        {
            Chosen.Values[ReflectionDownsample] = Details.Values[ReflectionQuality] >= 3 ? 1 : 2;
        }
        const int32 RequestedMode = Details.Values[HardwareTracing];
        const bool bHardwareAvailable = BridgeQuality::HardwareRayTracingAvailable();
        if (RequestedMode == 0 || (RequestedMode == 1 && !bHardwareAvailable))
        {
            Chosen.Values[HardwareTracing] = 0;
            Chosen.Values[HitLighting] = 0;
            Chosen.Values[MegaLightsAllowed] = 0;
        }
        else if (RequestedMode == 1)
        {
            Chosen.Values[HitLighting] = 1;
            Chosen.Values[LightingMode] = 1;
            for (int32 Index : {GlobalIlluminationQuality, ReflectionQuality})
            {
                const int32 Target = Chosen.Values[Index] != INDEX_NONE ? Chosen.Values[Index] :
                    State.bOverridesScalability ? State.PreviousGroups[Index] :
                    IConsoleManager::Get().FindConsoleVariable(Fields[Index])->GetInt();
                Chosen.Values[Index] = FMath::Max(2, Target);
            }
        }
        if (Chosen.Values[MegaLightsAllowed] == 1 && (!bHardwareAvailable || Chosen.Values[HardwareTracing] == 0))
        {
            Chosen.Values[MegaLightsAllowed] = 0;
        }
        if (State.Upscaling.Requested != BridgeUpscaling::EMethod::TSR)
        {
            Chosen.Values[AntiAliasingMethod] = AAM_TSR;
            Chosen.Values[DynamicResMode] = 0;
        }
        else if (Details.Values[DynamicResField] == 1)
        {
            Chosen.Values[DynamicResMode] = 1;
            const int32 TargetFps = Details.Values[DynamicResTargetFpsField] != INDEX_NONE
                ? Details.Values[DynamicResTargetFpsField] : 60;
            Chosen.Values[DynamicResBudget] = TargetFps;
            Chosen.Values[RenderScale] = INDEX_NONE;
        }
        else if (Details.Values[DynamicResField] == 0)
        {
            Chosen.Values[DynamicResMode] = 0;
            Chosen.Values[DynamicResBudget] = INDEX_NONE;
        }
        if (Details.Values[ContactShadowsField] != INDEX_NONE)
        {
            Chosen.Values[ContactShadowLengthCvar] = Details.Values[ContactShadowsField] ? 65 : 0;
        }
        if (Details.Values[CloudScale] != INDEX_NONE)
        {
            Chosen.Values[VolumetricCloud] = Details.Values[CloudScale] > 0;
        }
        if (Details.Values[CloudLayersField] != INDEX_NONE)
        {
            const int32 Layers = Details.Values[CloudLayersField];
            Chosen.Values[CloudMidLayerCvar] = Layers >= 2 ? 100 : 0;
            Chosen.Values[CloudCirrusCvar] = Layers >= 3 ? 100 : 0;
        }
        if (Details.Values[FogGrid] != INDEX_NONE)
        {
            Chosen.Values[VolumetricFog] = Details.Values[FogGrid] > 0;
            if (Details.Values[FogGrid] == 0)
            {
                // A disabled fog volume still retains the preset's grid for a later restore.
                Chosen.Values[FogGrid] = Preset == Baseline ? INDEX_NONE : Values[Preset][FogGrid];
            }
        }
        if (Details.Values[WaterParticlesField] != INDEX_NONE)
        {
            Chosen.Values[WaterPlanktonCvar] = Details.Values[WaterParticlesField];
            Chosen.Values[WaterBubblesCvar] = Details.Values[WaterParticlesField];
        }
        if (Details.Values[WaterLensFxField] != INDEX_NONE)
        {
            Chosen.Values[WaterFilmCvar] = Details.Values[WaterLensFxField];
            Chosen.Values[WaterDistortionCvar] = Details.Values[WaterLensFxField];
        }
        return Chosen;
    }

    void ApplyFields(FQualityState& State, const FDetailValues& Chosen)
    {
        const bool bNewGroups = Chosen.Values[ShadowQuality] != INDEX_NONE ||
            Chosen.Values[GlobalIlluminationQuality] != INDEX_NONE || Chosen.Values[ReflectionQuality] != INDEX_NONE;
        if (bNewGroups && !State.bOverridesScalability)
        {
            BeginTemporaryScalability(State);
        }
        if (bNewGroups || (State.bOverridesScalability && !State.bOwnsTemporaryScalability))
        {
            for (int32 Index = 0; Index < GroupFieldCount; ++Index)
            {
                IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Fields[Index]);
                const int32 Target = Chosen.Values[Index] == INDEX_NONE ? State.PreviousGroups[Index] : Chosen.Values[Index];
                if (Variable->GetInt() != Target) Variable->SetWithCurrentPriority(Target);
            }
        }
        if (!bNewGroups && State.bOwnsTemporaryScalability)
        {
            Scalability::ToggleTemporaryQualityLevels(false);
            State.bOwnsTemporaryScalability = false;
        }
        State.bOverridesScalability = bNewGroups;

        static const FName Tag(TEXT("MinecraftBridgeQuality"));
        const float RenderPercentage = State.Upscaling.RenderPercentage > 0.0f
            ? State.Upscaling.RenderPercentage : float(Chosen.Values[RenderScale]);
        // FSR applies its quality mode through r.ScreenPercentage and restores it on disable.
        if (State.Upscaling.Active != BridgeUpscaling::EMethod::FSR && RenderPercentage != State.AppliedRenderPercentage)
        {
            if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Fields[RenderScale]))
            {
                if (RenderPercentage > 0.0f)
                {
                    Variable->Set(RenderPercentage, ECVF_SetByGameOverride, Tag);
                }
                else
                {
                    Variable->Unset(ECVF_SetByGameOverride, Tag);
                }
            }
            State.AppliedRenderPercentage = RenderPercentage;
        }
        for (int32 Index = GroupFieldCount; Index < UE_ARRAY_COUNT(Fields); ++Index)
        {
            if (Index == RenderScale || Chosen.Values[Index] == State.AppliedFields.Values[Index]) continue;
            if (IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Fields[Index]))
            {
                const int32 Target = Chosen.Values[Index];
                if (Target != INDEX_NONE)
                {
                    if (Index == ReflectionRoughness || Index == TonemapperSharpen ||
                        Index == CloudCoverageCvar || Index == CloudMidLayerCvar ||
                        Index == CloudCirrusCvar || Index == WaterLabPbrNormalsCvar)
                    {
                        Variable->Set(Target * 0.01f, ECVF_SetByGameOverride, Tag);
                    }
                    else if (Index == DynamicResBudget)
                    {
                        Variable->Set(1000.0f / FMath::Max(1, Target), ECVF_SetByGameOverride, Tag);
                    }
                    else if (Index == ContactShadowLengthCvar)
                    {
                        Variable->Set(float(Target), ECVF_SetByGameOverride, Tag);
                    }
                    else
                    {
                        Variable->Set(Target, ECVF_SetByGameOverride, Tag);
                    }
                }
                else if (State.AppliedFields.Values[Index] != INDEX_NONE)
                {
                    Variable->Unset(ECVF_SetByGameOverride, Tag);
                }
            }
        }
        State.AppliedFields = Chosen;
    }

    class FBridgeQualityTrace final : public FSceneViewExtensionBase
    {
    public:
        explicit FBridgeQualityTrace(const FAutoRegister& AutoRegister) : FSceneViewExtensionBase(AutoRegister) {}

        void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
            FPostProcessingPassDelegateArray& Callbacks, bool) override
        {
            if (Pass == EPostProcessingPass::BeforeDOF && !View.bIsSceneCapture)
            {
                Callbacks.Add(FPostProcessingPassDelegate::CreateRaw(this, &FBridgeQualityTrace::ReadSizes));
            }
        }

        TAtomic<uint64> Dimensions{0};

    private:
        FScreenPassTexture ReadSizes(FRDGBuilder& Graph, const FSceneView& View, const FPostProcessMaterialInputs& Inputs)
        {
            const FIntPoint Input = Inputs.GetInput(EPostProcessMaterialInput::SceneColor).ViewRect.Size();
            const FIntPoint Output = View.UnscaledViewRect.Size();
            Dimensions.Store(uint64(uint16(Input.X)) | (uint64(uint16(Input.Y)) << 16) |
                (uint64(uint16(Output.X)) << 32) | (uint64(uint16(Output.Y)) << 48));
            return Inputs.ReturnUntouchedSceneColorForPostProcessing(Graph);
        }
    };

#if !UE_BUILD_SHIPPING && CSV_PROFILER
    class FBridgePerformanceCapture
    {
    public:
        explicit FBridgePerformanceCapture(const FString& InRoot)
            : Root(InRoot), RequestFile(Root / TEXT("capture.request")) {}

        void Tick(bool bReady, uint32 Scene, uint64 Dims, const FTransform& Camera, int32 Lights)
        {
            const double Now = FPlatformTime::Seconds();
            const auto& State = Quality();
            const bool bChanged = LastScene != Scene || LastRevision != State.Revision || LastDims != Dims;
            LastScene = Scene;
            LastRevision = State.Revision;
            LastDims = Dims;
            if (!bReady || !Dims || bChanged)
            {
                WarmUntil = Now + 5.0;
            }

            FCsvProfiler* Csv = FCsvProfiler::Get();
            const bool bCsvBusy = Csv->IsCapturing() || Csv->IsWritingFile();
            if (!ActiveDir.IsEmpty())
            {
                bChangedDuringCapture |= bChanged;
                if (!bCsvBusy && GFrameCounter > StartFrame + 1)
                {
                    if (!bTraceStopRequested)
                    {
                        bTraceStopped = FTraceAuxiliary::Stop();
                        bTraceStopRequested = true;
                    }
                    if (bTraceStopped && FTraceAuxiliary::IsConnected())
                    {
                        return;
                    }
                    RestoreTraceChannels();
                    const bool bHasFiles = bTraceStopped && IFileManager::Get().FileSize(*(ActiveDir / TEXT("timing.utrace"))) > 0 &&
                        IFileManager::Get().FileSize(*(ActiveDir / TEXT("frames.csv"))) > 0;
                    const TCHAR* Status = !bHasFiles ? TEXT("incomplete") :
                        bChangedDuringCapture ? TEXT("scene_settings_or_resolution_changed") : TEXT("complete");
                    const bool bSaved = FFileHelper::SaveStringToFile(Status, *(ActiveDir / TEXT("status.txt")));
                    UE_LOG(LogMinecraftBridgeQuality, Display, TEXT("BridgePerformance %s: %s"),
                        bSaved ? Status : TEXT("status_write_failed"), *ActiveDir);
                    ActiveDir.Reset();
                }
                return;
            }

            if (Now < WarmUntil || Now < NextPoll)
            {
                return;
            }
            NextPoll = Now + 0.25;
            auto& Files = IFileManager::Get();
            if (!Files.FileExists(*RequestFile))
            {
                return;
            }
            if (bCsvBusy || FTraceAuxiliary::IsConnected())
            {
                return; // Keep the request pending while another capture owns a profiler.
            }
            if (!Files.Delete(*RequestFile, true))
            {
                UE_LOG(LogMinecraftBridgeQuality, Warning, TEXT("Cannot consume profiling request: %s"), *RequestFile);
                return;
            }

            const FString Dir = Root / FString::Printf(TEXT("capture-%s-%s"),
                *FDateTime::UtcNow().ToString(TEXT("%Y%m%dT%H%M%SZ")), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
            if (!Files.MakeDirectory(*Dir, true) || !WriteMetadata(Dir, Scene, Dims, Camera, Lights))
            {
                UE_LOG(LogMinecraftBridgeQuality, Error, TEXT("Cannot write performance capture metadata: %s"), *Dir);
                return;
            }

            FTraceAuxiliary::FOptions TraceOptions;
            TraceOptions.bExcludeTail = true;
            EnableTraceChannels();
            if (!FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *(Dir / TEXT("timing.utrace")),
                nullptr, &TraceOptions))
            {
                RestoreTraceChannels();
                UE_LOG(LogMinecraftBridgeQuality, Error, TEXT("Cannot start performance trace: %s"), *Dir);
                return;
            }
            ActiveDir = Dir;
            StartFrame = GFrameCounter;
            bChangedDuringCapture = false;
            bTraceStopRequested = false;
            Csv->BeginCapture(FrameCount, Dir, TEXT("frames.csv"));
            UE_LOG(LogMinecraftBridgeQuality, Display, TEXT("BridgePerformance capturing %d frames: %s"),
                FrameCount, *Dir);
        }

    private:
        static constexpr int32 FrameCount = 240;

        void EnableTraceChannels()
        {
            TStringBuilder<256> Active;
            FTraceAuxiliary::GetActiveChannelsString(Active);
            TArray<FString> Previous;
            FString(Active.ToString()).ParseIntoArray(Previous, TEXT(","));
            constexpr const TCHAR* Channels[] = {TEXT("cpu"), TEXT("gpu"), TEXT("frame"), TEXT("bookmark")};
            TArray<FString> Added;
            for (const TCHAR* Channel : Channels)
            {
                if (!Previous.Contains(Channel)) Added.Emplace(Channel);
            }
            AddedTraceChannels = FString::Join(Added, TEXT(","));
            if (!AddedTraceChannels.IsEmpty()) FTraceAuxiliary::EnableChannels(*AddedTraceChannels);
        }

        void RestoreTraceChannels()
        {
            if (!AddedTraceChannels.IsEmpty()) FTraceAuxiliary::DisableChannels(*AddedTraceChannels);
            AddedTraceChannels.Reset();
        }

        static bool WriteMetadata(const FString& Dir, uint32 Scene, uint64 Dims, const FTransform& Camera, int32 Lights)
        {
            const auto& State = Quality();
            const TCHAR* Label = State.ActivePreset >= 0 ? PresetNames[State.ActivePreset] : TEXT("Baseline");
            FString Info = FString::Printf(
                TEXT("engine=%s\npreset=%s\nscene=%u\nqualityRevision=%u\nframes=%d\ninput=%ux%u\noutput=%ux%u\n")
                TEXT("hardwareGi=%d\nhardwareReflections=%d\nmegaLights=%d\nactiveBlockLightProxies=%d\n")
                TEXT("cameraUE=%s\nrotationUE=%s\n"),
                *FEngineVersion::Current().ToString(), Label, Scene, State.Revision, FrameCount,
                uint32(Dims & 0xffff), uint32((Dims >> 16) & 0xffff), uint32((Dims >> 32) & 0xffff), uint32(Dims >> 48),
                BridgeQuality::HardwareGiEnabled(), BridgeQuality::HardwareReflectionsEnabled(), BridgeQuality::MegaLights(),
                Lights, *Camera.GetLocation().ToString(), *Camera.Rotator().ToString());
            constexpr const TCHAR* ExtraFields[] = {TEXT("r.RDG.AsyncCompute"), TEXT("r.Lumen.AsyncCompute"),
                TEXT("r.Lumen.DiffuseIndirect.Allow"), TEXT("r.Lumen.Reflections.Allow"), TEXT("r.VSync"),
                TEXT("r.DynamicRes.OperationMode"), TEXT("r.OneFrameThreadLag"), TEXT("r.GTSyncType"), TEXT("t.MaxFPS"),
                TEXT("mcue.Shadows.LightAngleStep"), TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectional"),
                TEXT("r.VolumetricCloud.ShadowMap.MaxResolution"), TEXT("r.VolumetricCloud.SkyAO.MaxResolution"),
                TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor")};
            const auto AppendField = [&Info](const TCHAR* Name)
            {
                if (const auto* Variable = IConsoleManager::Get().FindConsoleVariable(Name))
                {
                    Info += FString::Printf(TEXT("%s=%s\n"), Name, *Variable->GetString());
                }
            };
            for (const TCHAR* Field : Fields) AppendField(Field);
            for (const TCHAR* Field : ExtraFields) AppendField(Field);
            return FFileHelper::SaveStringToFile(Info, *(Dir / TEXT("settings.txt")),
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }

        const FString Root;
        const FString RequestFile;
        FString ActiveDir;
        FString AddedTraceChannels;
        double WarmUntil = 0;
        double NextPoll = 0;
        uint64 LastDims = 0;
        uint64 StartFrame = 0;
        uint32 LastScene = 0;
        uint32 LastRevision = MAX_uint32;
        bool bChangedDuringCapture = false;
        bool bTraceStopRequested = false;
        bool bTraceStopped = false;
    };
#endif
}

int32 BridgeQuality::StartupFrameGenerationBackend()
{
    return ReadDetails(Quality()).Values[FrameGenBackendField];
}

void BridgeQuality::Tick()
{
    FQualityState& State = Quality();
    const double Now = FPlatformTime::Seconds();
    if (Now < State.NextRead)
    {
        return;
    }
    State.NextRead = Now + 0.10;
    bool bReadSettings = State.ActivePreset == Invalid;
    if (!State.Path.IsEmpty())
    {
        const FSettingsFileStamp QualityStamp = ReadSettingsFileStamp(State.Path);
        const FSettingsFileStamp DetailsStamp = ReadSettingsFileStamp(State.DetailsPath);
        bReadSettings = bReadSettings || Now >= State.NextContentRead ||
            QualityStamp != State.LastQualityStamp || DetailsStamp != State.LastDetailsStamp;
        State.LastQualityStamp = QualityStamp;
        State.LastDetailsStamp = DetailsStamp;
    }

    int32 Selected = bReadSettings ? State.LaunchPreset : State.ActivePreset;
    bool bRetryRead = false;
    if (bReadSettings && !State.Path.IsEmpty())
    {
        const int64 Bytes = IFileManager::Get().FileSize(*State.Path);
        FString Text;
        bRetryRead = Bytes >= 0 && Bytes <= 128 && !FFileHelper::LoadFileToString(Text, *State.Path);
        const bool bReadError = Bytes > 128 || bRetryRead;
        Text.TrimStartAndEndInline();
        if (bReadError || !Text.IsEmpty())
        {
            Selected = bReadError ? Invalid : Parse(Text);
            if (Selected == Invalid)
            {
                if (bReadError) Text = TEXT("<unreadable>");
                UE_CLOG(Text != State.LastValue, LogMinecraftBridgeQuality, Warning,
                    TEXT("Invalid value in BridgeQualityFile; keeping the current quality."));
                State.LastValue = MoveTemp(Text);
                Selected = State.ActivePreset == Invalid ? State.LaunchPreset : State.ActivePreset;
            }
        }
    }
    const FDetailValues Details = bReadSettings ? ReadDetails(State, &bRetryRead) : State.ActiveDetails;
    if (bReadSettings)
    {
        State.NextContentRead = bRetryRead ? Now : Now + 1.0;
    }

    const BridgeFrameGeneration::FStatus FrameGeneration = BridgeFrameGeneration::Update(
        Details.Values[FrameGenBackendField], Details.Values[FrameGenMultiplierField]);
    const bool bFrameGenerationChanged = FrameGeneration != State.FrameGeneration;
    const FString FrameGenerationTrace = BridgeFrameGeneration::TraceStatus();
    using BridgeUpscaling::EMethod;
    const EMethod Observed = BridgeUpscaling::ObservedMethod();
    const bool bSettingsChanged = Selected != State.ActivePreset || Details != State.ActiveDetails;
    const bool bRuntimeFallback = !bSettingsChanged && State.Upscaling.Active != EMethod::TSR &&
        Observed != EMethod::Pending && Observed != State.Upscaling.Active;
    if (!bSettingsChanged && !bRuntimeFallback && !bFrameGenerationChanged &&
        Observed == State.ReportedUpscaler && FrameGenerationTrace.IsEmpty())
    {
        return;
    }
    if (bSettingsChanged || bRuntimeFallback)
    {
        if (bSettingsChanged)
        {
            const int32 NextDebugOverlay = Details.Values[DebugOverlayField];
            if (NextDebugOverlay != INDEX_NONE && NextDebugOverlay != State.LastDebugOverlay)
            {
                State.LastDebugOverlay = NextDebugOverlay;
                ++State.DebugOverlayRevision;
            }
            const int32 NextPhotoTrigger = Details.Values[PhotoTriggerField];
            if (NextPhotoTrigger != INDEX_NONE && NextPhotoTrigger > State.PhotoTrigger)
            {
                State.PhotoTrigger = NextPhotoTrigger;
            }
            State.Upscaling = BridgeUpscaling::Resolve(Details.Values[UpscalerField], Details.Values[UpscalerQualityField]);
        }
        else
        {
            UE_LOG(LogMinecraftBridgeQuality, Warning, TEXT("%s was not active in the game view; falling back to TSR."),
                BridgeUpscaling::Name(State.Upscaling.Active));
            State.Upscaling.Active = EMethod::TSR;
            State.Upscaling.RenderPercentage = 0.0f;
        }
        BridgeUpscaling::Apply(State.Upscaling);
        ApplyFields(State, FieldsFor(State, Selected, Details));
        State.ActiveDetails = Details;
        State.ActivePreset = Selected;
        ++State.Revision;
    }
    State.ReportedUpscaler = BridgeUpscaling::ObservedMethod();
    State.FrameGeneration = FrameGeneration;
    FString Effective;
    for (const TCHAR* Field : Fields)
    {
        if (const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Field))
        {
            Effective += FString::Printf(TEXT(" %s=%s"), Field, *Variable->GetString());
        }
    }
    const TCHAR* Label = Selected == Baseline ? TEXT("Baseline") : PresetNames[Selected];
    const FString Upscaling = FString::Printf(
        TEXT(" Upscaler=%s RequestedUpscaler=%s RenderedUpscaler=%s UpscalerQuality=%d UpscalerFallback=%d"),
        BridgeUpscaling::Name(State.Upscaling.Active), BridgeUpscaling::Name(State.Upscaling.Requested),
        BridgeUpscaling::Name(State.ReportedUpscaler), State.Upscaling.Quality,
        State.Upscaling.Active != State.Upscaling.Requested);
    UE_CLOG(bSettingsChanged || bRuntimeFallback, LogMinecraftBridgeQuality, Display,
        TEXT("BridgeQuality %s:%s%s"), Label, *Effective, *Upscaling);
    UE_CLOG(bFrameGenerationChanged, LogMinecraftBridgeQuality, Display,
        TEXT("FrameGeneration %s modes=%d multiplier=%d restart=%d"),
        BridgeFrameGeneration::Name(FrameGeneration.Backend), FrameGeneration.SupportedModes,
        FrameGeneration.Multiplier, FrameGeneration.bRestartRequired);
    if (!State.Path.IsEmpty() && IFileManager::Get().DirectoryExists(*FPaths::GetPath(State.Path)))
    {
        if (!State.bWrotePhotoDir)
        {
            const FString PhotoDir = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir() / TEXT("PathTracing"));
            FFileHelper::SaveStringToFile(PhotoDir + TEXT("\n"),
                *FPaths::Combine(FPaths::GetPath(State.Path), TEXT("minecraftue-photo-dir.txt")),
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            State.bWrotePhotoDir = true;
        }
        const IConsoleVariable* ShadowBias = IConsoleManager::Get().FindConsoleVariable(
            TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectional"));
        const FVector2f WaterScale = WaterOpticalScale();
        const int32 Tier = PresetTier();
        const FString Status = FString::Printf(
            TEXT("%s HWRT=%d MegaLights=%d CloudSamples=%.2f\n%s\n")
            TEXT("DetailsVersion=2 RendererPid=%u ShadowLodBias=%.2f RayTracingAvailable=%d HardGi=%d HardReflections=%d ")
            TEXT("TranslucencyReflections=%d DirectionalRayShadows=%d SunShadowSmooth=%d RegionChunks=%d DynamicRange=%d DynamicBudget=%d ")
            TEXT("ExposureMode=%d ExposureBias=%d LocalExposure=%d CaveBrightness=%d BlockLightIntensity=%d ")
            TEXT("BloomIntensity=%d Saturation=%d Contrast=%d ColorTemp=%d VignetteIntensity=%d MotionBlur=%d ")
            TEXT("CloudLayers=%d CloudShadows=%d FogDensity=%d RainWetness=%d PuddleDepressions=%d PrecipDensity=%d ")
            TEXT("WaterAbsorption=%d WaterScattering=%d WaterP3=%d WaterWaves=%d%s\n")
            TEXT("FrameGenBackend=%s FrameGenModes=%d FrameGenMultiplier=%d FrameGenRestartRequired=%d\n"),
            Label, HardwareLighting(), MegaLights(), CloudSamples(), *Effective,
            FPlatformProcess::GetCurrentProcessId(), ShadowBias ? ShadowBias->GetFloat() : 0.0f,
            HardwareRayTracingAvailable(), HardwareGiEnabled(), HardwareReflectionsEnabled(),
            TranslucencyReflectionsEnabled(), DirectionalRayShadows(), SunShadowSmooth(Tier),
            TerrainRegionEdge(), DynamicRayRange(), DynamicRayBudget(),
            ExposureMode(), ExposureBiasSetting(), LocalExposureStrength(),
            FMath::RoundToInt(CaveBrightnessScale() * 100.0f), FMath::RoundToInt(BlockLightScale() * 100.0f),
            FMath::RoundToInt(BloomScale() * 100.0f), FMath::RoundToInt(ColorSaturation() * 100.0f),
            FMath::RoundToInt(ColorContrast() * 100.0f), FMath::RoundToInt(ColorTemp()),
            FMath::RoundToInt(VignetteIntensity() * 100.0f), FMath::RoundToInt(MotionBlurAmount() * 200.0f),
            CloudLayers(), CloudShadowsEnabled(Tier), FMath::RoundToInt(FogDensityScale() * 100.0f),
            RainWetnessMode(), PuddleDepressionsEnabled(), FMath::RoundToInt(PrecipDensityScale() * 100.0f),
            FMath::RoundToInt(WaterScale.X * 100.0f), FMath::RoundToInt(WaterScale.Y * 100.0f),
            WaterP3Enabled(Tier), WaterWavesEnabled(), *Upscaling,
            BridgeFrameGeneration::Name(FrameGeneration.Backend), FrameGeneration.SupportedModes,
            FrameGeneration.Multiplier, FrameGeneration.bRestartRequired);
        FFileHelper::SaveStringToFile(Status + FrameGenerationTrace, *(State.Path + TEXT(".effective")));
    }
}

uint32 BridgeQuality::Revision()
{
    return Quality().Revision;
}

int32 BridgeQuality::HudFpsLimit()
{
    const int32 Preset = Quality().ActivePreset;
    return Preset == 0 || Preset == 1 ? 120 : 165;
}

int32 BridgeQuality::PresetTier()
{
    const int32 Preset = Quality().ActivePreset;
    return Preset >= 0 ? Preset : 2;
}

bool BridgeQuality::HasCustomCloudScale()
{
    return Quality().ActiveDetails.Values[CloudScale] != INDEX_NONE;
}

float BridgeQuality::CloudSamples()
{
    const auto& State = Quality();
    const int32 Custom = State.ActiveDetails.Values[CloudScale];
    if (Custom != INDEX_NONE)
    {
        return Custom * 0.01f;
    }
    const int32 Preset = State.ActivePreset;
    return Preset >= 0 ? CloudSampleScales[Preset] : 1.00f;
}

int32 BridgeQuality::CloudLayers()
{
    const int32 Selected = Quality().ActiveDetails.Values[CloudLayersField];
    if (Selected != INDEX_NONE)
    {
        return Selected;
    }
    const int32 Tier = PresetTier();
    return Tier >= 2 ? 3 : (Tier == 1 ? 2 : 1);
}

bool BridgeQuality::CloudShadowsEnabled(int32 Tier)
{
    const int32 Selected = Quality().ActiveDetails.Values[CloudShadowsField];
    return Selected != INDEX_NONE ? (Selected != 0) : (Tier >= 1);
}

bool BridgeQuality::HardwareRayTracingAvailable()
{
#if RHI_RAYTRACING
    return IsRayTracingEnabled() && GRHISupportsRayTracingShaders;
#else
    return false;
#endif
}

bool BridgeQuality::HardwareLighting()
{
    return HardwareRayTracingAvailable() && Enabled(TEXT("r.Lumen.HardwareRayTracing")) &&
        Enabled(TEXT("r.Lumen.HardwareRayTracing.HitLighting.Allowed"));
}

bool BridgeQuality::HardwareGiEnabled()
{
    return HardwareLighting() && Quality().ActiveDetails.Values[HardwareGi] != 0 &&
        Enabled(TEXT("r.Lumen.DiffuseIndirect.Allow"));
}

bool BridgeQuality::HardwareReflectionsEnabled()
{
    return HardwareLighting() && Quality().ActiveDetails.Values[HardwareReflections] != 0 &&
        Enabled(TEXT("r.Lumen.Reflections.Allow"));
}

bool BridgeQuality::TranslucencyReflectionsEnabled()
{
    if (!HardwareReflectionsEnabled())
    {
        return false;
    }
    const int32 Selected = Quality().ActiveDetails.Values[TranslucencyReflectionsField];
    return Selected != 0;
}

int32 BridgeQuality::DirectionalRayShadows()
{
    const int32 Selected = Quality().ActiveDetails.Values[DirectionalShadows];
    return Selected == 1 && !HardwareLighting() ? 0 : Selected;
}

bool BridgeQuality::SunShadowSmooth(int32 Tier)
{
    const int32 Selected = Quality().ActiveDetails.Values[SunShadowSmoothField];
    return Selected != INDEX_NONE ? (Selected != 0) : (Tier >= 3);
}

int32 BridgeQuality::TerrainRegionEdge()
{
    return Quality().TerrainRegionEdge;
}

int32 BridgeQuality::TerrainRayRangeOnRenderThread()
{
    return CVarTerrainRayRange.GetValueOnRenderThread();
}

int32 BridgeQuality::DynamicRayRange()
{
    const int32 Selected = Quality().ActiveDetails.Values[DynamicRange];
    return Selected == INDEX_NONE ? 32 : Selected;
}

int32 BridgeQuality::DynamicRayBudget()
{
    const int32 Selected = Quality().ActiveDetails.Values[DynamicBudget];
    return Selected == INDEX_NONE ? 96 : Selected;
}

int32 BridgeQuality::ExposureMode()
{
    const int32 Selected = Quality().ActiveDetails.Values[ExposureModeField];
    return Selected == INDEX_NONE ? 1 : Selected;
}

int32 BridgeQuality::ExposureBiasSetting()
{
    const int32 Selected = Quality().ActiveDetails.Values[ExposureBiasField];
    return Selected == INDEX_NONE ? 20 : Selected;
}

float BridgeQuality::ExposureBias()
{
    return (ExposureBiasSetting() - 20) * 0.1f;
}

int32 BridgeQuality::LocalExposureStrength()
{
    const int32 Selected = Quality().ActiveDetails.Values[LocalExposureField];
    return Selected == INDEX_NONE ? 60 : Selected;
}

float BridgeQuality::CaveBrightnessScale()
{
    const int32 Selected = Quality().ActiveDetails.Values[CaveBrightnessField];
    return Selected == INDEX_NONE ? 1.0f : (Selected * 0.01f);
}

float BridgeQuality::BlockLightScale()
{
    const int32 Selected = Quality().ActiveDetails.Values[BlockLightIntensityField];
    return Selected == INDEX_NONE ? 1.0f : (Selected * 0.01f);
}

float BridgeQuality::FogDensityScale()
{
    const int32 Selected = Quality().ActiveDetails.Values[FogDensityField];
    return Selected == INDEX_NONE ? 1.0f : (Selected * 0.01f);
}

int32 BridgeQuality::RainWetnessMode()
{
    const int32 Selected = Quality().ActiveDetails.Values[RainWetnessField];
    return Selected == INDEX_NONE ? 2 : Selected;
}

bool BridgeQuality::PuddleDepressionsEnabled()
{
    return Quality().ActiveDetails.Values[PuddleDepressionsField] == 1;
}

float BridgeQuality::PrecipDensityScale()
{
    const int32 Selected = Quality().ActiveDetails.Values[PrecipDensityField];
    return Selected == INDEX_NONE ? 1.0f : (Selected * 0.01f);
}

float BridgeQuality::BloomScale()
{
    const int32 Selected = Quality().ActiveDetails.Values[BloomIntensityField];
    return Selected == INDEX_NONE ? 1.0f : (Selected * 0.01f);
}

float BridgeQuality::ColorSaturation()
{
    const int32 Selected = Quality().ActiveDetails.Values[SaturationField];
    return Selected == INDEX_NONE ? 1.0f : (Selected * 0.01f);
}

float BridgeQuality::ColorContrast()
{
    const int32 Selected = Quality().ActiveDetails.Values[ContrastField];
    return Selected == INDEX_NONE ? 1.0f : (Selected * 0.01f);
}

float BridgeQuality::ColorTemp()
{
    const int32 Selected = Quality().ActiveDetails.Values[ColorTempField];
    return Selected == INDEX_NONE ? 6500.0f : float(Selected);
}

float BridgeQuality::VignetteIntensity()
{
    const int32 Selected = Quality().ActiveDetails.Values[VignetteIntensityField];
    return Selected == INDEX_NONE ? 0.40f : (Selected * 0.01f);
}

float BridgeQuality::MotionBlurAmount()
{
    const int32 Selected = Quality().ActiveDetails.Values[MotionBlurField];
    return Selected == INDEX_NONE ? 0.0f : (Selected * 0.005f);
}

FVector2f BridgeQuality::WaterOpticalScale()
{
    const auto& Details = Quality().ActiveDetails;
    const int32 Absorption = Details.Values[WaterAbsorption];
    const int32 Scattering = Details.Values[WaterScattering];
    return FVector2f(
        Absorption == INDEX_NONE ? 1.0f : Absorption / 100.0f,
        Scattering == INDEX_NONE ? 1.0f : Scattering / 100.0f);
}

bool BridgeQuality::WaterP3Enabled(int32 Tier)
{
    const int32 Selected = Quality().ActiveDetails.Values[WaterP3Cvar];
    return Selected != INDEX_NONE ? (Selected != 0) : (Tier >= 2);
}

bool BridgeQuality::WaterWavesEnabled()
{
    const int32 Selected = Quality().ActiveDetails.Values[WaterWavesCvar];
    return Selected != 0;
}

int32 BridgeQuality::DebugOverlayOverride()
{
    return Quality().LastDebugOverlay;
}

uint32 BridgeQuality::DebugOverlayRevision()
{
    return Quality().DebugOverlayRevision;
}

int32 BridgeQuality::PhotoTriggerCounter()
{
    return Quality().PhotoTrigger;
}

bool BridgeQuality::MegaLights()
{
    return HardwareLighting() && Enabled(TEXT("r.MegaLights.EnableForProject")) && Enabled(TEXT("r.MegaLights.Allowed"));
}

bool BridgeQuality::IsTraceRequested()
{
    static const bool bRequested = []
    {
        const bool bTrace = FParse::Param(FCommandLine::Get(), TEXT("BridgeQualityTrace"));
#if UE_BUILD_SHIPPING
        FString Path;
        return bTrace && FParse::Value(FCommandLine::Get(), TEXT("BridgeQualityTraceCsv="), Path) && !Path.IsEmpty();
#elif CSV_PROFILER
        FString Path;
        return bTrace || (FParse::Value(FCommandLine::Get(), TEXT("BridgePerformanceDir="), Path) && !Path.IsEmpty());
#else
        return bTrace;
#endif
    }();
    return bRequested;
}

void BridgeQuality::TraceFrame(bool bSceneReady, uint32 Scene, const FTransform& Camera, int32 ActiveLights)
{
    static const bool bTrace = FParse::Param(FCommandLine::Get(), TEXT("BridgeQualityTrace"));
    static const auto ViewTrace = FSceneViewExtensions::NewExtension<FBridgeQualityTrace>();
    const uint64 Dims = ViewTrace->Dimensions.Load();
#if !UE_BUILD_SHIPPING && CSV_PROFILER
    static const FString ProfileRoot = []
    {
        FString Path;
        FParse::Value(FCommandLine::Get(), TEXT("BridgePerformanceDir="), Path);
        return Path;
    }();
    if (!ProfileRoot.IsEmpty())
    {
        static FBridgePerformanceCapture Capture(ProfileRoot);
        Capture.Tick(bSceneReady, Scene, Dims, Camera, ActiveLights);
    }
#endif
    if (!bTrace)
    {
        return;
    }
    static const FString ShotDirectory = []
    {
        FString Directory;
        FParse::Value(FCommandLine::Get(), TEXT("BridgeQualityTraceShots="), Directory);
        return Directory;
    }();
    static const FString CsvPath = []
    {
        FString Path;
        FParse::Value(FCommandLine::Get(), TEXT("BridgeQualityTraceCsv="), Path);
        return Path;
    }();
    struct FFrameSample { double Frame, Game, Render, Gpu; };
    static TArray<FFrameSample> Samples;
#if !UE_BUILD_SHIPPING && CSV_PROFILER
    static const int32 GpuEventFrames = []
    {
        int32 Frames = 0;
        FParse::Value(FCommandLine::Get(), TEXT("BridgeGpuEventFrames="), Frames);
        return FMath::Clamp(Frames, 0, 1200);
    }();
    static uint32 GpuEventRevision = MAX_uint32;
#endif
    static uint32 Batch = 0;
    static uint32 TraceRevision = MAX_uint32;
    static uint32 ShotRevision = MAX_uint32;
    static double WarmUntil = 0;
    static double LastFrame = 0;
    static TArray<double> FrameMs;
    static double SumGameMs = 0, SumRenderMs = 0, SumGpuMs = 0;
    const double Now = FPlatformTime::Seconds();
    static const FString CaptureDir = []
    {
        FString Directory;
        FParse::Value(FCommandLine::Get(), TEXT("BridgeWaterP2CaptureDir="), Directory);
        return Directory;
    }();
    static double NextRequestRead = 0.0;
    static FString LastRequest;
    if (bSceneReady && !CaptureDir.IsEmpty() && Now >= NextRequestRead)
    {
        NextRequestRead = Now + 0.065;
        const FString RequestFile = FPaths::Combine(CaptureDir, TEXT("capture-request.txt"));
        const int64 Bytes = IFileManager::Get().FileSize(*RequestFile);
        FString Request;
        if (Bytes > 0 && Bytes <= 64 && FFileHelper::LoadFileToString(Request, *RequestFile) &&
            Request != LastRequest && Algo::AllOf(Request, [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('-'); }))
        {
            LastRequest = Request;
            FScreenshotRequest::RequestScreenshot(FPaths::Combine(CaptureDir, Request + TEXT(".png")), false, false);
        }
    }
    if (!bSceneReady || TraceRevision != Revision())
    {
        TraceRevision = Revision();
        WarmUntil = Now + 5.0;
        LastFrame = Now;
        FrameMs.Reset(240);
        Samples.Reset(240);
        SumGameMs = SumRenderMs = SumGpuMs = 0;
        return;
    }
    const double Interval = Now - LastFrame;
    LastFrame = Now;
    if (Now < WarmUntil || Dims == 0)
    {
        return;
    }
#if !UE_BUILD_SHIPPING && CSV_PROFILER
    if (ProfileRoot.IsEmpty() && GpuEventFrames > 0 && !CsvPath.IsEmpty() &&
        GpuEventRevision != TraceRevision && Quality().ActivePreset >= 2)
    {
        GpuEventRevision = TraceRevision;
        FCsvProfiler::Get()->BeginCapture(GpuEventFrames, FPaths::GetPath(CsvPath),
            FString::Printf(TEXT("gpu-events-%03u.csv"), TraceRevision));
    }
#endif
    double GpuMs = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
    if (!CsvPath.IsEmpty())
    {
        // Query completed GPU frames, so a slow render cannot repeat the previous GPU timing on faster game ticks.
        static FRHIGPUFrameTimeHistory::FState GpuHistory;
        uint64 Cycles = 0;
        uint64 LastCompleted = 0;
        while (GpuHistory.PopFrameCycles(Cycles) != FRHIGPUFrameTimeHistory::EResult::Empty)
        {
            if (Cycles != 0) LastCompleted = Cycles;
        }
        if (LastCompleted == 0)
        {
            return;
        }
        GpuMs = FPlatformTime::ToMilliseconds64(LastCompleted);
    }
    FrameMs.Add(Interval * 1000.0);
    const FFrameSample Sample{Interval * 1000.0, FPlatformTime::ToMilliseconds64(GGameThreadTime),
        FPlatformTime::ToMilliseconds64(GRenderThreadTime), GpuMs};
    SumGameMs += Sample.Game;
    SumRenderMs += Sample.Render;
    SumGpuMs += Sample.Gpu;
    if (!CsvPath.IsEmpty()) Samples.Add(Sample);
    if (FrameMs.Num() == 240)
    {
        FrameMs.Sort();
        const auto& State = Quality();
        const TCHAR* Label = State.ActivePreset >= 0 ? PresetNames[State.ActivePreset] : TEXT("Baseline");
        const auto* History = IConsoleManager::Get().FindConsoleVariable(TEXT("r.TSR.History.ScreenPercentage"));
        UE_LOG(LogMinecraftBridgeQuality, Display,
            TEXT("BridgeQualityMetrics %s input=%ux%u output=%ux%u history=%d frames=240 frameP50Ms=%.3f frameP95Ms=%.3f frameP99Ms=%.3f gameMeanMs=%.3f renderMeanMs=%.3f gpuMeanMs=%.3f"),
            Label, uint32(Dims & 0xffff), uint32((Dims >> 16) & 0xffff), uint32((Dims >> 32) & 0xffff), uint32(Dims >> 48),
            History ? History->GetInt() : -1, FrameMs[119], FrameMs[227], FrameMs[237],
            SumGameMs / 240.0, SumRenderMs / 240.0, SumGpuMs / 240.0);
        if (!CsvPath.IsEmpty())
        {
            FString Rows;
            if (!IFileManager::Get().FileExists(*CsvPath))
            {
                Rows = TEXT("batch,revision,preset,input_width,input_height,output_width,output_height,frame,frame_ms,game_ms,render_ms,gpu_ms\n");
            }
            ++Batch;
            for (int32 Index = 0; Index < Samples.Num(); ++Index)
            {
                const FFrameSample& Row = Samples[Index];
                Rows += FString::Printf(TEXT("%u,%u,%s,%u,%u,%u,%u,%d,%.6f,%.6f,%.6f,%.6f\n"),
                    Batch, TraceRevision, Label, uint32(Dims & 0xffff), uint32((Dims >> 16) & 0xffff),
                    uint32((Dims >> 32) & 0xffff), uint32(Dims >> 48), Index + 1,
                    Row.Frame, Row.Game, Row.Render, Row.Gpu);
            }
            FFileHelper::SaveStringToFile(Rows, *CsvPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
                &IFileManager::Get(), FILEWRITE_Append);
            Samples.Reset();
        }
        if (ShotRevision != TraceRevision && !ShotDirectory.IsEmpty() && IFileManager::Get().DirectoryExists(*ShotDirectory))
        {
            ShotRevision = TraceRevision;
            FScreenshotRequest::RequestScreenshot(FPaths::Combine(ShotDirectory,
                FString::Printf(TEXT("%03u-%s.png"), ShotRevision, Label)), false, false);
        }
        FrameMs.Reset();
        SumGameMs = SumRenderMs = SumGpuMs = 0;
    }
}
