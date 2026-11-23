#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"

class FBridgeGlassLighting;

class FBridgeGlassCapture final : public FWorldSceneViewExtension
{
public:
    FBridgeGlassCapture(const FAutoRegister& AutoReg, UWorld* World, const FString& Directory, bool bReportSettings);
    static TSharedPtr<FBridgeGlassCapture, ESPMode::ThreadSafe> Create(UWorld* World);
    void Tick(bool bSceneReady, const FBridgeGlassLighting& Lighting);

    virtual void SetupViewFamily(FSceneViewFamily&) override {}
    virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
    virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}
    virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& Graph, FSceneViewFamily& Family) override;
    virtual void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
        FPostProcessingPassDelegateArray& Callbacks, bool bIsPassEnabled) override;

private:
    struct FPendingReadback;
    FScreenPassTexture CaptureHdr(FRDGBuilder& Graph, const FSceneView& View, const FPostProcessMaterialInputs& Inputs);

    const FString Directory;
    const bool bTraceHandoffs;
    const bool bReportSettings;
    FString LastRequest;
    double NextCheck = 0.0;
    int32 LastPendingHandoffs = INDEX_NONE;
    bool bSettingsReported = false;
    TSharedPtr<FPendingReadback, ESPMode::ThreadSafe> Readback;
};
