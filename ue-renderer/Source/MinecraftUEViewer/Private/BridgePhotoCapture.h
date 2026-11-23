#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "RenderCommandFence.h"
#include "UObject/StrongObjectPtr.h"

class ABridgeViewer;
class FViewport;
class ULocalPlayer;
class USceneCaptureComponent2D;
class UTextureRenderTarget2D;

class FBridgePhotoCapture
{
public:
    FBridgePhotoCapture();
    ~FBridgePhotoCapture();

    bool Start(ABridgeViewer& Viewer, ULocalPlayer& Player, FViewport& Viewport);
    void Tick();
    void Cancel(const TCHAR* Reason);
    bool IsRendering() const { return Capture.IsValid(); }
    FString GetStatus() const;

private:
    struct FReadbacks;
    class FViewExtension;

    void ReleaseScene();
    void SetStatus(FString Message, bool bError = false);

    TStrongObjectPtr<USceneCaptureComponent2D> Capture;
    TStrongObjectPtr<UTextureRenderTarget2D> ColorTarget;
    TSharedPtr<FViewExtension, ESPMode::ThreadSafe> ViewExtension;
    TSharedPtr<FReadbacks, ESPMode::ThreadSafe> Readbacks;
    FRenderCommandFence ReadbackFence;
    TFuture<bool> Save;
    FIntPoint Size = FIntPoint::ZeroValue;
    int32 Samples = 0;
    int32 Submitted = 0;
    bool bSaveExr = true;
    bool bDenoiser = true;
    double ReadbackDeadline = 0.0;
    double StatusUntil = 0.0;
    FString Basename;
    FString Status;
};
