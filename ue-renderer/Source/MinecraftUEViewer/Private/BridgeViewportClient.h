#pragma once

#include "Engine/GameViewportClient.h"
#include "BridgeWire.h"
#include "RenderCommandFence.h"
#include "BridgeViewportClient.generated.h"

class ABridgeViewer;
class FBridgePhotoCapture;
class FBridgeWindowsInput;

UCLASS()
class UBridgeViewportClient : public UGameViewportClient
{
    GENERATED_BODY()

public:
    void SetViewer(ABridgeViewer* InViewer);
    void ReleaseControl(const TCHAR* Reason);
    void RequestControlRelease(const TCHAR* Reason);
    bool ForwardKeyboard(const BridgeWire::FInputEvent& Event);
    bool CaptureFromKeyboard();
    bool IsConsoleActive() const;
    bool ToggleBridgeConsole();
    void RequestPhoto();
    bool IsRenderingPhoto() const;
    void CancelPhoto(const TCHAR* Reason);
    bool ForwardHorizontalWheel(float Amount, FIntPoint ScreenPosition);
    bool CanDropFilesAt(FVector2D ScreenPosition) const;
    bool ForwardDroppedFiles(const TArray<FString>& Files);
    void DrawMinecraftUi(UCanvas* Canvas);
    void ApplyInitialMinecraftWindow(FIntPoint Position, FIntPoint Size);
    void SetMinecraftFullscreen(bool bFullscreen);
    FIntPoint GetBridgeViewportSize() const;
    uint64 PollMinecraftPresentation();
    void ResetMinecraftPresentation();

    virtual void Init(FWorldContext& WorldContext, UGameInstance* OwningGameInstance, bool bCreateNewAudioDevice = true) override;
    virtual bool InputKey(const FInputKeyEventArgs& EventArgs) override;
    virtual bool InputAxis(const FInputKeyEventArgs& EventArgs) override;
    virtual bool InputTouch(FViewport* InViewport, FTouchId TouchId, ETouchType::Type Type,
        const FVector2D& TouchLocation, float Force, uint64 Timestamp) override;
    virtual EMouseCursor::Type GetCursor(FViewport* InViewport, int32 X, int32 Y) override;
    virtual void MouseMove(FViewport* InViewport, int32 X, int32 Y) override;
    virtual void CapturedMouseMove(FViewport* InViewport, int32 X, int32 Y) override;
    virtual void LostFocus(FViewport* InViewport) override;
    virtual void Tick(float DeltaTime) override;
    virtual bool WindowCloseRequested() override;
    virtual void BeginDestroy() override;
    bool IsControlCaptured() const { return bCaptured; }
    void FlushPendingMouseLook();

private:
    bool TryStartControl();
    void StartRequestedPhoto();
    bool ReleaseIfRequested();
    void SetGameplayMouse(bool bGameplay);
    bool SendWheel(float Amount, bool bHorizontal, FIntPoint Position);
    bool HasUiRect(FViewport* InViewport) const;
    void SendPointer(FIntPoint Cursor);
    FIntPoint PointerPosition(FViewport* InViewport) const;
    uint16 DrawDebugPanel(UCanvas* Canvas, const ABridgeViewer& Target) const;
    struct FActiveTouch
    {
        FTouchId Id;
        FVector2D Position;
        bool bScreen;
    };
    TWeakObjectPtr<ABridgeViewer> Viewer;
    TSharedPtr<FBridgePhotoCapture> Photo;
    TSharedPtr<FBridgeWindowsInput> WindowsInput;
    TSharedPtr<SWidget> FileDropLayer;
    FString PendingRelease;
    FIntRect UiRect{0, 0, 0, 0};
    FIntPoint UiViewportSize = FIntPoint::ZeroValue;
    TOptional<uint32> LastPointer;
    TOptional<FActiveTouch> ActiveTouch;
    TOptional<FIntRect> InitialWindowBounds;
    FRenderCommandFence HandoffFence;
    bool bDrewMinecraftUi = false;
    bool bHandoffFenceStarted = false;
    bool bAutoCaptureOnFocus = true;
    bool bInitialWindowHandled = false;
    bool bCaptured = false;
    bool bGameplayMouse = false;
    bool bPhotoRequested = false;
    bool bResumeControlAfterPhoto = false;
    bool bDebugPanelVisible = true;
    uint32 AppliedDebugOverlayRevision = 0;
    int32 AppliedPhotoTrigger = 0;
    double ScheduledPhotoAt = 0.0;
    float PendingMouseX = 0.0f;
    bool bHasPendingMouseX = false;
};
