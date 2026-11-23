#include "BridgePhotoCapture.h"

#include "BridgeViewer.h"
#include "Async/Async.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneCaptureComponent2D.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/LocalPlayer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "PostProcess/PostProcessMaterialInputs.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"
#include "RHIGPUReadback.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneViewExtension.h"

DEFINE_LOG_CATEGORY_STATIC(LogBridgePhoto, Log, All);

namespace
{
    TAutoConsoleVariable<int32> PhotoLongEdge(TEXT("mcue.Photo.LongEdge"), 2560,
        TEXT("Long edge of a path-traced photo in pixels, preserving the camera aspect ratio."));
    TAutoConsoleVariable<int32> PhotoSamples(TEXT("mcue.Photo.Samples"), 256,
        TEXT("Spatial samples per pixel of a path-traced photo."));
    TAutoConsoleVariable<int32> PhotoSaveExr(TEXT("mcue.Photo.SaveExr"), 1,
        TEXT("Also save a scene-linear HDR .exr alongside the sRGB .png."));
    TAutoConsoleVariable<int32> PhotoDenoiser(TEXT("mcue.Photo.Denoiser"), 1,
        TEXT("Enable path-tracing denoiser for captured photos."));

    bool ReadImage(FRHIGPUTextureReadback& Readback, FImage& Image)
    {
        int32 RowPitch = 0;
        const auto* Pixels = static_cast<const uint8*>(Readback.Lock(RowPitch));
        if (!Pixels || RowPitch < Image.SizeX)
        {
            if (Pixels)
            {
                Readback.Unlock();
            }
            return false;
        }
        const int64 BytesPerPixel = ERawImageFormat::GetBytesPerPixel(Image.Format);
        const int64 RowBytes = Image.SizeX * BytesPerPixel;
        for (int32 Y = 0; Y < Image.SizeY; ++Y)
        {
            FMemory::Memcpy(Image.RawData.GetData() + Y * RowBytes,
                Pixels + Y * RowPitch * BytesPerPixel, RowBytes);
        }
        Readback.Unlock();
        return true;
    }

    bool WritePhoto(const FString& Basename, const FImage& Color, const FImage* LinearColor)
    {
        IFileManager& Files = IFileManager::Get();
        const FString Png = Basename + TEXT(".png");
        const FString TempPng = Basename + TEXT(".tmp.png");
        if (!FImageUtils::SaveImageByExtension(*TempPng, Color) || !Files.Move(*Png, *TempPng, false))
        {
            Files.Delete(*TempPng, false, true);
            return false;
        }
        if (LinearColor)
        {
            const FString Exr = Basename + TEXT(".exr");
            const FString TempExr = Basename + TEXT(".tmp.exr");
            if (!FImageUtils::SaveImageByExtension(*TempExr, *LinearColor) || !Files.Move(*Exr, *TempExr, false))
            {
                Files.Delete(*Png, false, true);
                Files.Delete(*TempExr, false, true);
                return false;
            }
        }
        return true;
    }
}

struct FBridgePhotoCapture::FReadbacks
{
    struct FImages
    {
        FImage Color;
        FImage LinearColor;
        bool bHasLinear = false;
        bool bValid = false;
    };

    FRHIGPUTextureReadback Png{TEXT("BridgePhotoPng")};
    FRHIGPUTextureReadback Exr{TEXT("BridgePhotoExr")};
    TOptional<FImages> Images;
    EPixelFormat HdrFormat = PF_Unknown;
    float PreExposure = 1.0f;
    bool bPngQueued = false;
    bool bSaveExr = true;
};

class FBridgePhotoCapture::FViewExtension final : public ISceneViewExtension
{
public:
    FViewExtension(FGameTime InTime, int32 InSamples, FIntPoint InSize, bool bInDenoiser,
        TSharedPtr<FReadbacks, ESPMode::ThreadSafe> InReadbacks = nullptr)
        : Time(InTime), Samples(InSamples), Size(InSize), bDenoiser(bInDenoiser), Buffers(MoveTemp(InReadbacks))
    {
    }

    void SetupViewFamily(FSceneViewFamily& Family) override
    {
        Family.Time = Time;
    }

    void SetupView(FSceneViewFamily&, FSceneView& View) override
    {
        View.bIsOfflineRender = true;
        View.AntiAliasingMethod = AAM_None;
        View.FinalPostProcessSettings.PathTracingSamplesPerPixel = Samples;
        View.FinalPostProcessSettings.PathTracingEnableDenoiser = bDenoiser;
        View.FinalPostProcessSettings.FilmGrainIntensity = 0.0f;
    }

    void SubscribeToPostProcessingPass(EPostProcessingPass Pass, const FSceneView&,
        FPostProcessingPassDelegateArray& Callbacks, bool bIsPassEnabled) override
    {
        if (Buffers && Buffers->bSaveExr && Pass == EPostProcessingPass::BeforeDOF && bIsPassEnabled)
        {
            Callbacks.Add(FAfterPassCallbackDelegate::CreateRaw(this, &FViewExtension::ReadSceneColor));
        }
    }

    FGameTime Time;

private:
    FScreenPassTexture ReadSceneColor(FRDGBuilder& GraphBuilder, const FSceneView& View,
        const FPostProcessMaterialInputs& Inputs)
    {
        const FScreenPassTexture Output = Inputs.ReturnUntouchedSceneColorForPostProcessing(GraphBuilder);
        if (Output.IsValid() && Output.ViewRect.Size() == Size &&
            (Output.Texture->Desc.Format == PF_FloatRGBA || Output.Texture->Desc.Format == PF_A32B32G32R32F))
        {
            Buffers->HdrFormat = Output.Texture->Desc.Format;
            Buffers->PreExposure = View.State ? View.State->GetPreExposure() : 1.0f;
            AddEnqueueCopyPass(GraphBuilder, &Buffers->Exr, Output.Texture, FResolveRect(
                Output.ViewRect.Min.X, Output.ViewRect.Min.Y, Output.ViewRect.Max.X, Output.ViewRect.Max.Y));
        }
        return Output;
    }

    const int32 Samples;
    const FIntPoint Size;
    const bool bDenoiser;
    const TSharedPtr<FReadbacks, ESPMode::ThreadSafe> Buffers;
};

FBridgePhotoCapture::FBridgePhotoCapture() = default;

FBridgePhotoCapture::~FBridgePhotoCapture()
{
    ReleaseScene();
}

bool FBridgePhotoCapture::Start(ABridgeViewer& Viewer, ULocalPlayer& Player, FViewport& Viewport)
{
    if (IsRendering() || Readbacks || Save.IsValid())
    {
        return false;
    }
    UWorld* World = Viewer.GetWorld();
    const IConsoleVariable* PathTracing = IConsoleManager::Get().FindConsoleVariable(TEXT("r.PathTracing"));
    if (!World || !World->Scene || !IsRayTracingEnabled() || !GRHISupportsRayTracingShaders ||
        !FDataDrivenShaderPlatformInfo::GetSupportsPathTracing(World->Scene->GetShaderPlatform()) ||
        !PathTracing || PathTracing->GetInt() == 0)
    {
        SetStatus(TEXT("Photo needs hardware path tracing (DX12/SM6)."), true);
        return false;
    }
    FSceneViewProjectionData Projection;
    const APlayerCameraManager* Camera = Player.PlayerController ? Player.PlayerController->PlayerCameraManager : nullptr;
    if (!Viewer.IsPhotoReady() || !Camera ||
        !Player.GetProjectionData(&Viewport, Projection) || Projection.GetConstrainedViewRect().IsEmpty())
    {
        SetStatus(TEXT("Photo needs a loaded Minecraft scene."), true);
        return false;
    }
    const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ScreenShotDir() / TEXT("PathTracing"));
    if (!IFileManager::Get().MakeDirectory(*Directory, true))
    {
        SetStatus(TEXT("Could not create the photo directory."), true);
        return false;
    }
    Basename = Directory / FString::Printf(TEXT("MCUE_%s_%s"),
        *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), *FGuid::NewGuid().ToString(EGuidFormats::Short));
    const FIntPoint ViewSize = Projection.GetConstrainedViewRect().Size();
    const int32 LongEdge = FMath::Clamp(PhotoLongEdge.GetValueOnGameThread(), 320, 4096);
    const double Scale = double(LongEdge) / FMath::Max(ViewSize.X, ViewSize.Y);
    Size = FIntPoint(FMath::Max(1, FMath::RoundToInt(ViewSize.X * Scale)),
        FMath::Max(1, FMath::RoundToInt(ViewSize.Y * Scale)));
    Samples = FMath::Clamp(PhotoSamples.GetValueOnGameThread(), 1, 1024);
    bSaveExr = PhotoSaveExr.GetValueOnGameThread() != 0;
    bDenoiser = PhotoDenoiser.GetValueOnGameThread() != 0;
    Submitted = 0;

    ColorTarget.Reset(NewObject<UTextureRenderTarget2D>());
    ColorTarget->TargetGamma = 2.2f;
    ColorTarget->InitCustomFormat(Size.X, Size.Y, PF_B8G8R8A8, true);
    Capture.Reset(NewObject<USceneCaptureComponent2D>(&Viewer));
    Capture->TextureTarget = ColorTarget.Get();
    Capture->CaptureSource = SCS_FinalColorLDR;
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->bAlwaysPersistRenderingState = true;
    Capture->bExcludeFromSceneTextureExtents = true;
    Capture->bUseRayTracingIfEnabled = true;
    Capture->bUseCustomProjectionMatrix = true;
    Capture->CustomProjectionMatrix = Projection.ProjectionMatrix;
    const FMinimalViewInfo& CameraView = Camera->GetCameraCacheView();
    Capture->FOVAngle = CameraView.FOV;
    Capture->PostProcessSettings = CameraView.PostProcessSettings;
    Capture->PostProcessBlendWeight = CameraView.PostProcessBlendWeight;
    Capture->SetWorldLocationAndRotation(Projection.ViewOrigin, CameraView.Rotation);
    Capture->ShowFlags.SetPathTracing(true);
    Capture->ShowFlags.SetMotionBlur(false);
    Capture->ShowFlags.SetScreenPercentage(false);
    Capture->ShowFlags.SetAntiAliasing(false);
    ViewExtension = MakeShared<FViewExtension, ESPMode::ThreadSafe>(World->GetTime(), Samples, Size, bDenoiser);
    Capture->SceneViewExtensions.Add(ViewExtension);
    Capture->RegisterComponentWithWorld(World);
    UE_LOG(LogBridgePhoto, Display, TEXT("Rendering %dx%d path-traced photo, %d samples per pixel (exr=%d denoiser=%d)."),
        Size.X, Size.Y, Samples, int32(bSaveExr), int32(bDenoiser));
    return true;
}

void FBridgePhotoCapture::Tick()
{
    if (Capture.IsValid())
    {
        ++Submitted;
        if (Submitted < Samples)
        {
            Capture->CaptureScene();
            return;
        }

        Readbacks = MakeShared<FReadbacks, ESPMode::ThreadSafe>();
        Readbacks->bSaveExr = bSaveExr;
        ViewExtension = MakeShared<FViewExtension, ESPMode::ThreadSafe>(ViewExtension->Time, Samples, Size, bDenoiser, Readbacks);
        Capture->SceneViewExtensions.Reset();
        Capture->SceneViewExtensions.Add(ViewExtension);
        Capture->CaptureScene();

        FTextureRenderTargetResource* Resource = ColorTarget->GameThread_GetRenderTargetResource();
        ENQUEUE_RENDER_COMMAND(BridgePhotoReadColor)(
            [Buffers = Readbacks, Resource, Extent = Size](FRHICommandListImmediate& RHICmdList)
            {
                if (FRHITexture* Texture = Resource->GetRenderTargetTexture())
                {
                    Buffers->Png.EnqueueCopy(RHICmdList, Texture,
                        FIntVector::ZeroValue, 0, FIntVector(Extent.X, Extent.Y, 1));
                    Buffers->bPngQueued = true;
                }
            });
        ReadbackFence.BeginFence();
        ReadbackDeadline = FPlatformTime::Seconds() + 30.0;
        ReleaseScene();
        return;
    }
    if (Readbacks && ReadbackFence.IsFenceComplete())
    {
        if (!Readbacks->Images)
        {
            const bool bNeedExr = Readbacks->bSaveExr;
            if (!Readbacks->bPngQueued || (bNeedExr && Readbacks->HdrFormat == PF_Unknown) ||
                FPlatformTime::Seconds() > ReadbackDeadline)
            {
                Readbacks.Reset();
                ColorTarget.Reset();
                SetStatus(TEXT("Photo render did not produce the expected image buffers."), true);
                return;
            }
            if (!Readbacks->Png.IsReady() || (bNeedExr && !Readbacks->Exr.IsReady()))
            {
                return;
            }
            Readbacks->Images.Emplace();
            ENQUEUE_RENDER_COMMAND(BridgePhotoCopyPixels)(
                [Buffers = Readbacks, Extent = Size](FRHICommandListImmediate&)
                {
                    FReadbacks::FImages& Images = Buffers->Images.GetValue();
                    Images.Color.Init(Extent.X, Extent.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
                    Images.bHasLinear = Buffers->bSaveExr;
                    bool bOk = ReadImage(Buffers->Png, Images.Color);
                    if (Buffers->bSaveExr)
                    {
                        Images.LinearColor.Init(Extent.X, Extent.Y,
                            Buffers->HdrFormat == PF_FloatRGBA ? ERawImageFormat::RGBA16F : ERawImageFormat::RGBA32F,
                            EGammaSpace::Linear);
                        bOk = bOk && ReadImage(Buffers->Exr, Images.LinearColor);
                    }
                    Images.bValid = bOk;
                });
            ReadbackFence.BeginFence();
            return;
        }
        FReadbacks::FImages Images = MoveTemp(Readbacks->Images.GetValue());
        const float ExposureScale = 1.0f / FMath::Max(Readbacks->PreExposure, UE_SMALL_NUMBER);
        Readbacks.Reset();
        ColorTarget.Reset();
        if (!Images.bValid)
        {
            SetStatus(TEXT("Could not read the photo pixels from the GPU."), true);
            return;
        }
        Save = Async(EAsyncExecution::ThreadPool,
            [File = Basename, Images = MoveTemp(Images), ExposureScale]() mutable
            {
                for (FColor& Pixel : Images.Color.AsBGRA8())
                {
                    Pixel.A = 255;
                }
                if (Images.bHasLinear)
                {
                    FImage SceneLinear;
                    Images.LinearColor.CopyTo(SceneLinear, ERawImageFormat::RGBA32F, EGammaSpace::Linear);
                    for (FLinearColor& Pixel : SceneLinear.AsRGBA32F())
                    {
                        Pixel *= ExposureScale;
                        Pixel.A = 1.0f;
                    }
                    return WritePhoto(File, Images.Color, &SceneLinear);
                }
                return WritePhoto(File, Images.Color, nullptr);
            });
    }
    if (Save.IsValid() && Save.IsReady())
    {
        const bool bSaved = Save.Get();
        Save = TFuture<bool>();
        if (bSaved)
        {
            UE_LOG(LogBridgePhoto, Display, TEXT("Saved photo: %s (%s)"), *Basename, bSaveExr ? TEXT("PNG+EXR") : TEXT("PNG"));
            SetStatus(FString::Printf(TEXT("Photo saved: %s (%s)"), *FPaths::GetCleanFilename(Basename),
                bSaveExr ? TEXT("PNG + EXR") : TEXT("PNG")));
        }
        else
        {
            UE_LOG(LogBridgePhoto, Warning, TEXT("Could not write photo: %s"), *Basename);
            SetStatus(TEXT("Could not save the rendered photo."), true);
        }
    }
}

void FBridgePhotoCapture::ReleaseScene()
{
    if (Capture.IsValid())
    {
        Capture->DestroyComponent();
        Capture.Reset();
    }
    ViewExtension.Reset();
}

void FBridgePhotoCapture::Cancel(const TCHAR* Reason)
{
    if (IsRendering())
    {
        ReleaseScene();
        ColorTarget.Reset();
        SetStatus(FString::Printf(TEXT("Photo cancelled: %s."), Reason), true);
    }
}

FString FBridgePhotoCapture::GetStatus() const
{
    if (IsRendering())
    {
        return FString::Printf(TEXT("Rendering path-traced photo  %d / %d"), Submitted, Samples);
    }
    if (Readbacks || Save.IsValid())
    {
        return bSaveExr ? TEXT("Saving PNG and EXR...") : TEXT("Saving PNG...");
    }
    return FPlatformTime::Seconds() < StatusUntil ? Status : FString();
}

void FBridgePhotoCapture::SetStatus(FString Message, bool bError)
{
    if (bError)
    {
        UE_LOG(LogBridgePhoto, Warning, TEXT("%s"), *Message);
    }
    Status = MoveTemp(Message);
    StatusUntil = FPlatformTime::Seconds() + 7.0;
}
