#include "BridgeGlassCapture.h"

#include "Algo/AllOf.h"
#include "BridgeGlassLighting.h"
#include "BridgeQuality.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Math/Float16Color.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RHIGPUReadback.h"
#include "UnrealClient.h"

namespace
{
    constexpr const TCHAR* LumenDiffuseIndirectName = TEXT("r.Lumen.DiffuseIndirect.Allow");

    int32 LumenDiffuseIndirectOnRenderThread()
    {
        static const auto* CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(LumenDiffuseIndirectName);
        return CVar ? CVar->GetValueOnRenderThread() : INDEX_NONE;
    }
}

struct FBridgeGlassCapture::FPendingReadback
{
    explicit FPendingReadback(const FString& Base) : Path(Base + TEXT(".linear.json")) {}

    FRHIGPUTextureReadback Pixels{TEXT("MinecraftGlassLightProbe")};
    const FString Path;
    FIntRect Region;
    FIntPoint ViewSize;
    EPixelFormat Format = PF_Unknown;
    float PreExposure = 1.f;
    float ExposureMinEv100 = 0.f;
    float ExposureMaxEv100 = 0.f;
    int32 LumenDiffuseIndirect = INDEX_NONE;
    int32 LocalExposureStrength = INDEX_NONE;
    int32 PlanarGrids = 0;
    FBridgeGlassLighting::FLocalShadowCounts GlassShadows;
    bool bViewUsesLumenMethod = false;
    bool bHardwareLighting = false;
    bool bMegaLights = false;
    bool bFixedExposure = false;
    bool bVirtualShadowMaps = false;
};

FBridgeGlassCapture::FBridgeGlassCapture(const FAutoRegister& AutoReg, UWorld* World, const FString& InDirectory, bool bInReportSettings)
    : FWorldSceneViewExtension(AutoReg, World), Directory(InDirectory),
        bTraceHandoffs(FParse::Param(FCommandLine::Get(), TEXT("BridgeGlassTraceHandoffs"))), bReportSettings(bInReportSettings)
{
}

TSharedPtr<FBridgeGlassCapture, ESPMode::ThreadSafe> FBridgeGlassCapture::Create(UWorld* World)
{
    FString Path;
    if (!FParse::Value(FCommandLine::Get(), TEXT("BridgeGlassCaptureDir="), Path) || Path.IsEmpty() ||
        !IFileManager::Get().MakeDirectory(*Path, true))
    {
        return nullptr;
    }
    int32 RequestedGi = INDEX_NONE;
    const bool bOverrideGi = FParse::Value(FCommandLine::Get(), TEXT("BridgeGlassQADiffuseGI="), RequestedGi) &&
        (RequestedGi == 0 || RequestedGi == 1);
    if (bOverrideGi)
    {
        if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(LumenDiffuseIndirectName))
        {
            CVar->Set(RequestedGi, ECVF_SetByCommandline);
        }
    }
    return FSceneViewExtensions::NewExtension<FBridgeGlassCapture>(World, Path, bOverrideGi);
}

void FBridgeGlassCapture::Tick(bool bSceneReady, const FBridgeGlassLighting& Lighting)
{
    const int32 PendingHandoffs = Lighting.PendingHandoffCount();
    const double Now = FPlatformTime::Seconds();
    if (bTraceHandoffs && LastPendingHandoffs != PendingHandoffs)
    {
        const FString Path = Directory / TEXT("light-handoffs.csv");
        const FString Row = FString::Printf(TEXT("%.6f,%d\n"), Now, PendingHandoffs);
        if (LastPendingHandoffs == INDEX_NONE)
        {
            FFileHelper::SaveStringToFile(TEXT("time_seconds,pending_sources\n") + Row, *Path,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
        else
        {
            FFileHelper::SaveStringToFile(Row, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
                &IFileManager::Get(), FILEWRITE_Append);
        }
        LastPendingHandoffs = PendingHandoffs;
    }
    if (!bSceneReady || PendingHandoffs != 0 || Now < NextCheck)
    {
        return;
    }
    NextCheck = Now + 0.065;
    const FString Path = Directory / TEXT("capture-request.txt");
    const int64 Length = IFileManager::Get().FileSize(*Path);
    FString Request;
    if (Length <= 0 || Length > 66 || !FFileHelper::LoadFileToString(Request, *Path))
    {
        return;
    }
    Request.TrimStartAndEndInline();
    if (Request.IsEmpty() || Request.Len() > 64 || Request == LastRequest ||
        !Algo::AllOf(Request, [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('-'); }))
    {
        return;
    }
    LastRequest = Request;
    const FString Basename = Directory / Request;
    const bool bHardwareLighting = BridgeQuality::HardwareLighting();
    const bool bMegaLights = BridgeQuality::MegaLights();
    const bool bFixedExposure = BridgeQuality::ExposureMode() == 0;
    const int32 ExposureStrength = BridgeQuality::LocalExposureStrength();
    const int32 PlanarGrids = Lighting.PlanarGridCount();
    const auto Shadows = Lighting.LocalShadowCounts();
    FScreenshotRequest::RequestScreenshot(Basename + TEXT(".png"), false, false);
    ENQUEUE_RENDER_COMMAND(MinecraftGlassRequestCapture)(
        [Self = StaticCastSharedRef<FBridgeGlassCapture>(AsShared()), Basename, bHardwareLighting, bMegaLights,
            bFixedExposure, ExposureStrength, PlanarGrids, Shadows](FRHICommandListImmediate&)
        {
            Self->Readback = MakeShared<FPendingReadback, ESPMode::ThreadSafe>(Basename);
            Self->Readback->bHardwareLighting = bHardwareLighting;
            Self->Readback->bMegaLights = bMegaLights;
            Self->Readback->bFixedExposure = bFixedExposure;
            Self->Readback->LocalExposureStrength = ExposureStrength;
            Self->Readback->PlanarGrids = PlanarGrids;
            Self->Readback->GlassShadows = Shadows;
        });
}

void FBridgeGlassCapture::SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView& View,
    FPostProcessingPassDelegateArray& Callbacks, bool bIsPassEnabled)
{
    if (Pass == EPostProcessingPass::BeforeDOF && View.bIsGameView && !View.bIsSceneCapture &&
        !View.bIsReflectionCapture && !View.bIsOfflineRender && Readback && Readback->Format == PF_Unknown)
    {
        Callbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FBridgeGlassCapture::CaptureHdr));
    }
}

FScreenPassTexture FBridgeGlassCapture::CaptureHdr(FRDGBuilder& Graph, const FSceneView& View,
    const FPostProcessMaterialInputs& Inputs)
{
    const FScreenPassTexture Output = Inputs.ReturnUntouchedSceneColorForPostProcessing(Graph);
    if (!Readback || Readback->Format != PF_Unknown || !Output.IsValid() ||
        (Output.Texture->Desc.Format != PF_FloatRGBA && Output.Texture->Desc.Format != PF_A32B32G32R32F))
    {
        return Output;
    }
    const FIntPoint Middle = Output.ViewRect.Min + Output.ViewRect.Size() / 2;
    const FIntPoint Margin(4, 4);
    Readback->Region = FIntRect((Middle - Margin).ComponentMax(Output.ViewRect.Min),
        (Middle + Margin + FIntPoint(1, 1)).ComponentMin(Output.ViewRect.Max));
    Readback->ViewSize = Output.ViewRect.Size();
    Readback->Format = Output.Texture->Desc.Format;
    Readback->PreExposure = View.State ? View.State->GetPreExposure() : 1.f;
    Readback->LumenDiffuseIndirect = LumenDiffuseIndirectOnRenderThread();
    Readback->bViewUsesLumenMethod = View.FinalPostProcessSettings.DynamicGlobalIlluminationMethod ==
        EDynamicGlobalIlluminationMethod::Lumen;
    Readback->bVirtualShadowMaps = UseVirtualShadowMaps(View.GetShaderPlatform(), View.GetFeatureLevel());
    Readback->ExposureMinEv100 = View.FinalPostProcessSettings.AutoExposureMinBrightness;
    Readback->ExposureMaxEv100 = View.FinalPostProcessSettings.AutoExposureMaxBrightness;
    // UE blends the two bounds independently into the view. Allow float rounding at an equal EV.
    constexpr float EqualExposureToleranceEv = 1.e-4f;
    Readback->bFixedExposure &= FMath::IsNearlyEqual(Readback->ExposureMinEv100, Readback->ExposureMaxEv100,
        EqualExposureToleranceEv);
    const FIntRect& Rect = Readback->Region;
    AddEnqueueCopyPass(Graph, &Readback->Pixels, Output.Texture, FResolveRect(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y));
    return Output;
}

void FBridgeGlassCapture::PreRenderViewFamily_RenderThread(FRDGBuilder&, FSceneViewFamily&)
{
    if (bReportSettings && !bSettingsReported)
    {
        bSettingsReported = true;
        const FString Actual = FString::Printf(TEXT("{\"lumen_diffuse_indirect_allow\":%d}\n"), LumenDiffuseIndirectOnRenderThread());
        FFileHelper::SaveStringToFile(Actual, *(Directory / TEXT("glass-qa-settings.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    if (!Readback || Readback->Format == PF_Unknown || !Readback->Pixels.IsReady())
    {
        return;
    }
    auto Completed = MoveTemp(Readback);
    int32 Pitch = 0;
    const void* Bytes = Completed->Pixels.Lock(Pitch);
    if (!Bytes)
    {
        return;
    }
    FVector Sum = FVector::ZeroVector;
    const FIntPoint Size = Completed->Region.Size();
    for (int32 Y = 0; Y < Size.Y; ++Y)
    {
        for (int32 X = 0; X < Size.X; ++X)
        {
            const int32 Index = Y * Pitch + X;
            const FLinearColor Color = Completed->Format == PF_FloatRGBA
                ? static_cast<const FFloat16Color*>(Bytes)[Index].GetFloats()
                : static_cast<const FLinearColor*>(Bytes)[Index];
            Sum += FVector(Color.R, Color.G, Color.B);
        }
    }
    Completed->Pixels.Unlock();
    const FVector Linear = Sum / (double(Size.X * Size.Y) * FMath::Max(Completed->PreExposure, UE_SMALL_NUMBER));
    const auto BoolValue = [](bool bSet) { return bSet ? TEXT("true") : TEXT("false"); };
    const FString Json = FString::Printf(TEXT("{\"rgb\":[%.9g,%.9g,%.9g],\"pre_exposure\":%.9g,\"sample_width\":%d,\"sample_height\":%d,"
        "\"view_width\":%d,\"view_height\":%d,\"lumen_diffuse_indirect_allow\":%d,\"view_uses_lumen_method\":%s,"
        "\"hardware_lighting\":%s,\"virtual_shadow_maps\":%s,\"mega_lights\":%s,\"fixed_exposure\":%s,\"local_exposure_strength\":%d,"
        "\"exposure_min_ev100\":%.9g,\"exposure_max_ev100\":%.9g,\"exposure_span_ev100\":%.9g,"
        "\"glass_ray_shadow_sources\":%d,\"glass_vsm_shadow_sources\":%d,\"glass_planar_grids\":%d}\n"),
        Linear.X, Linear.Y, Linear.Z, double(Completed->PreExposure), Size.X, Size.Y, Completed->ViewSize.X,
        Completed->ViewSize.Y, Completed->LumenDiffuseIndirect, BoolValue(Completed->bViewUsesLumenMethod),
        BoolValue(Completed->bHardwareLighting), BoolValue(Completed->bVirtualShadowMaps), BoolValue(Completed->bMegaLights),
        BoolValue(Completed->bFixedExposure), Completed->LocalExposureStrength, double(Completed->ExposureMinEv100),
        double(Completed->ExposureMaxEv100), double(Completed->ExposureMaxEv100) - double(Completed->ExposureMinEv100),
        Completed->GlassShadows.RayTraced,
        Completed->bVirtualShadowMaps ? Completed->GlassShadows.ShadowMapped : 0, Completed->PlanarGrids);
    const FString Temp = Completed->Path + TEXT(".tmp");
    if (FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        IFileManager::Get().Move(*Completed->Path, *Temp);
    }
}
