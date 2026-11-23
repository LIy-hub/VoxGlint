#pragma once

#include "CoreMinimal.h"

namespace BridgeQuality
{
    int32 StartupFrameGenerationBackend();
    void Tick();
    bool IsTraceRequested();
    void TraceFrame(bool bSceneReady, uint32 Scene, const FTransform& Camera, int32 ActiveLights);
    uint32 Revision();
    int32 HudFpsLimit();
    float CloudSamples();
    bool HasCustomCloudScale();
    int32 CloudLayers();
    bool CloudShadowsEnabled(int32 Tier);
    int32 PresetTier();
    bool HardwareLighting();
    bool HardwareRayTracingAvailable();
    bool HardwareGiEnabled();
    bool HardwareReflectionsEnabled();
    bool TranslucencyReflectionsEnabled();
    int32 DirectionalRayShadows();
    bool SunShadowSmooth(int32 Tier);
    int32 TerrainRegionEdge();
    int32 TerrainRayRangeOnRenderThread();
    int32 DynamicRayRange();
    int32 DynamicRayBudget();
    int32 ExposureMode();
    int32 ExposureBiasSetting();
    float ExposureBias();
    int32 LocalExposureStrength();
    float CaveBrightnessScale();
    float BlockLightScale();
    float FogDensityScale();
    int32 RainWetnessMode();
    bool PuddleDepressionsEnabled();
    float PrecipDensityScale();
    float BloomScale();
    float ColorSaturation();
    float ColorContrast();
    float ColorTemp();
    float VignetteIntensity();
    float MotionBlurAmount();
    FVector2f WaterOpticalScale();
    bool WaterP3Enabled(int32 Tier);
    bool WaterWavesEnabled();
    int32 DebugOverlayOverride();
    uint32 DebugOverlayRevision();
    int32 PhotoTriggerCounter();
    bool MegaLights();
}
