#include "BridgeViewportClient.h"

#include "BridgeFrameGeneration.h"
#include "BridgePhotoCapture.h"
#include "BridgeQuality.h"
#include "BridgeViewer.h"
#include "BridgeWindowsInput.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Console.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Input/DragAndDrop.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SceneView.h"
#include "Slate/SceneViewport.h"
#include "UnrealClient.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
#if !UE_BUILD_SHIPPING
    FAutoConsoleCommandWithWorld PhotoCommand(
        TEXT("mcue.Photo.Capture"),
        TEXT("Render a path-traced still and save PNG and EXR."),
        FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
        {
            if (UBridgeViewportClient* Client = World ? Cast<UBridgeViewportClient>(World->GetGameViewport()) : nullptr)
            {
                Client->RequestPhoto();
            }
        }));
#endif

    class SBridgeFileDrop final : public SCompoundWidget
    {
    public:
        SLATE_BEGIN_ARGS(SBridgeFileDrop) {}
            SLATE_ARGUMENT(TWeakObjectPtr<UBridgeViewportClient>, Owner)
        SLATE_END_ARGS()

        void Construct(const FArguments& Args) { Owner = Args._Owner; }

        FReply OnDragOver(const FGeometry&, const FDragDropEvent& Event) override
        {
            return GetFiles(Event) ? FReply::Handled() : FReply::Unhandled();
        }

        FReply OnDrop(const FGeometry&, const FDragDropEvent& Event) override
        {
            const TSharedPtr<FExternalDragOperation> Files = GetFiles(Event);
            return Files && Owner->ForwardDroppedFiles(Files->GetFiles()) ? FReply::Handled() : FReply::Unhandled();
        }

    private:
        TSharedPtr<FExternalDragOperation> GetFiles(const FDragDropEvent& Event) const
        {
            const UBridgeViewportClient* Client = Owner.Get();
            const TSharedPtr<FExternalDragOperation> Operation = Event.GetOperationAs<FExternalDragOperation>();
            return Client && Operation && Operation->HasFiles() && Client->CanDropFilesAt(Event.GetScreenSpacePosition())
                ? Operation : nullptr;
        }

        TWeakObjectPtr<UBridgeViewportClient> Owner;
    };

    FIntRect WindowBounds(const SWindow& Window)
    {
        const FVector2D Position = Window.GetPositionInScreen();
        const FVector2D Size = Window.GetClientSizeInScreen();
        const FIntPoint Min(FMath::RoundToInt(Position.X), FMath::RoundToInt(Position.Y));
        return FIntRect(Min, Min + FIntPoint(FMath::RoundToInt(Size.X), FMath::RoundToInt(Size.Y)));
    }

    int32 MouseButton(const FKey& Key)
    {
        if (Key == EKeys::LeftMouseButton) return 0;
        if (Key == EKeys::RightMouseButton) return 1;
        if (Key == EKeys::MiddleMouseButton) return 2;
        if (Key == EKeys::ThumbMouseButton) return 3;
        if (Key == EKeys::ThumbMouseButton2) return 4;
        return INDEX_NONE;
    }

    uint8 PointerModifiers()
    {
#if PLATFORM_WINDOWS
        return FBridgeWindowsInput::Modifiers();
#else
        return 0;
#endif
    }
}

void UBridgeViewportClient::Init(FWorldContext& WorldContext, UGameInstance* OwningGameInstance, bool bCreateNewAudioDevice)
{
    BridgeFrameGeneration::PrepareGameViewport();
    Super::Init(WorldContext, OwningGameInstance, bCreateNewAudioDevice);
    bDebugPanelVisible = !FParse::Param(FCommandLine::Get(), TEXT("BridgeHideDebugPanel"));
    AppliedDebugOverlayRevision = BridgeQuality::DebugOverlayRevision();
    AppliedPhotoTrigger = BridgeQuality::PhotoTriggerCounter();
#if PLATFORM_WINDOWS
    if (FSlateApplication::IsInitialized() && !FSlateApplication::Get().IsRenderingOffScreen())
    {
        const TSharedPtr<GenericApplication> App = FSlateApplication::Get().GetPlatformApplication();
        if (App)
        {
            WindowsInput = MakeShared<FBridgeWindowsInput>(StaticCastSharedRef<FWindowsApplication>(App.ToSharedRef()), this);
        }
    }
#endif
}

void UBridgeViewportClient::SetViewer(ABridgeViewer* InViewer)
{
    CancelPhoto(TEXT("viewer changed"));
    ReleaseControl(TEXT("viewer changed"));
    Viewer = InViewer;
    if (!FileDropLayer)
    {
        FileDropLayer = SNew(SBridgeFileDrop).Owner(this);
        AddViewportWidgetContent(FileDropLayer.ToSharedRef());
    }
    if (!bInitialWindowHandled && !InitialWindowBounds)
    {
        if (const TSharedPtr<SWindow> GameWindow = GetWindow())
        {
            InitialWindowBounds = WindowBounds(*GameWindow);
        }
    }
    SetMouseCaptureMode(EMouseCaptureMode::CaptureDuringMouseDown);
    SetHideCursorDuringCapture(false);
}

void UBridgeViewportClient::SetGameplayMouse(bool bGameplay)
{
    bGameplayMouse = bGameplay;
    PendingMouseX = 0.0f;
    bHasPendingMouseX = false;
    LastPointer.Reset();
    if (APlayerController* Player = GetWorld()->GetFirstPlayerController())
    {
        Player->bShowMouseCursor = !bGameplay;
        if (bGameplay)
        {
            Player->SetInputMode(FInputModeGameOnly().SetConsumeCaptureMouseDown(false));
        }
        else
        {
            Player->SetInputMode(FInputModeGameAndUI().SetHideCursorDuringCapture(false));
        }
    }
    FSlateApplication::Get().QueryCursor();
}

bool UBridgeViewportClient::HasUiRect(FViewport* InViewport) const
{
    return InViewport && InViewport->GetSizeXY() == UiViewportSize && !UiRect.IsEmpty();
}

FIntPoint UBridgeViewportClient::PointerPosition(FViewport* InViewport) const
{
    FIntPoint Cursor;
    InViewport->GetMousePos(Cursor);
    return Cursor;
}

void UBridgeViewportClient::SendPointer(FIntPoint Cursor)
{
    using namespace BridgeWire;
    const auto Encode = [](double Normalized)
    {
        return uint16(FMath::RoundToInt(FMath::Clamp(Normalized * PointerScale, -32768.0, 32767.0)));
    };
    const uint32 X = Encode((Cursor.X - UiRect.Min.X + 0.5) / UiRect.Width());
    const uint32 Y = Encode((Cursor.Y - UiRect.Min.Y + 0.5) / UiRect.Height());
    const uint32 Position = (X << 16) | Y;
    if (!LastPointer || *LastPointer != Position)
    {
        Viewer->AddControlEvent({EInputAction::PointerMove, Position, 0});
        LastPointer = Position;
    }
}

bool UBridgeViewportClient::TryStartControl()
{
#if PLATFORM_WINDOWS
    ABridgeViewer* Target = Viewer.Get();
    const TSharedPtr<SWindow> GameWindow = GetWindow();
    const TSharedPtr<FGenericWindow> Native = GameWindow ? GameWindow->GetNativeWindow() : nullptr;
    const HWND Handle = Native ? static_cast<HWND>(Native->GetOSWindowHandle()) : nullptr;
    if (bCaptured || IsConsoleActive() || IsRenderingPhoto() || !Target || !WindowsInput || !Handle)
    {
        return false;
    }
    WindowsInput->SetWindow(Native);
    if (!WindowsInput->HasForeground() || !Target->StartControl())
    {
        return false;
    }
    bCaptured = true;
    bAutoCaptureOnFocus = false;
    SetGameplayMouse(Target->IsGameplayInput());
    return true;
#else
    return false;
#endif
}

bool UBridgeViewportClient::SendWheel(float Amount, bool bHorizontal, FIntPoint Position)
{
    using namespace BridgeWire;
    ABridgeViewer* Target = Viewer.Get();
    if (!Target || (!bCaptured && (!Target->IsSingleWindow() ||
        !(Target->IsScreenInput() || Target->IsGameplayInput()) || !TryStartControl())))
    {
        return false;
    }
    if (ReleaseIfRequested() || !FMath::IsFinite(Amount) || Amount == 0)
    {
        return true;
    }
    const bool bScreen = Target->IsScreenInput();
    if (bScreen)
    {
        if (!HasUiRect(Viewport) || !UiRect.Contains(Position))
        {
            return true;
        }
        SendPointer(Position);
    }
    else if (!Target->IsGameplayInput())
    {
        return true;
    }
    const float Delta = FMath::Clamp(Amount, -MaxWheelDelta, MaxWheelDelta);
    uint32 Bits;
    FMemory::Memcpy(&Bits, &Delta, sizeof(Bits));
    return Target->AddControlEvent({EInputAction::Scroll, Bits,
        uint8((bHorizontal ? ScrollHorizontal : 0) | (bScreen ? ScrollScreen : 0))});
}

bool UBridgeViewportClient::ForwardHorizontalWheel(float Amount, FIntPoint ScreenPosition)
{
    if (!Viewport || Viewport->GetSizeXY() == FIntPoint::ZeroValue)
    {
        return false;
    }
    const FVector2D Local = Viewport->VirtualDesktopPixelToViewport(ScreenPosition) * FVector2D(Viewport->GetSizeXY());
    return SendWheel(Amount, true, Local.IntPoint());
}

bool UBridgeViewportClient::CanDropFilesAt(FVector2D ScreenPosition) const
{
    const ABridgeViewer* Target = Viewer.Get();
    return Target && Target->IsScreenInput() && HasUiRect(Viewport) &&
        UiRect.Contains((Viewport->VirtualDesktopPixelToViewport(ScreenPosition.IntPoint()) *
            FVector2D(Viewport->GetSizeXY())).IntPoint());
}

bool UBridgeViewportClient::ForwardDroppedFiles(const TArray<FString>& Files)
{
    ABridgeViewer* Target = Viewer.Get();
    return Target && Target->AddDroppedFiles(Files);
}

bool UBridgeViewportClient::ForwardKeyboard(const BridgeWire::FInputEvent& Event)
{
    ABridgeViewer* Target = Viewer.Get();
    return bCaptured && Target && PendingRelease.IsEmpty() && Target->AddControlEvent(Event);
}

bool UBridgeViewportClient::CaptureFromKeyboard()
{
    const ABridgeViewer* Target = Viewer.Get();
    return bAutoCaptureOnFocus && Target && Target->IsSingleWindow() && TryStartControl();
}

bool UBridgeViewportClient::IsConsoleActive() const
{
    return ViewportConsole && ViewportConsole->ConsoleActive();
}

bool UBridgeViewportClient::ToggleBridgeConsole()
{
    if (!ViewportConsole)
    {
        return false;
    }

    ReleaseControl(TEXT("UE console shortcut"));
    bAutoCaptureOnFocus = true;
    ViewportConsole->FakeGotoState(IsConsoleActive() ? NAME_None : FName(TEXT("Typing")));
    return true;
}

void UBridgeViewportClient::RequestPhoto()
{
    bPhotoRequested = true;
}

void UBridgeViewportClient::StartRequestedPhoto()
{
    bPhotoRequested = false;
    ABridgeViewer* Target = Viewer.Get();
    UWorld* GameWorld = GetWorld();
    APlayerController* Player = GameWorld ? GameWorld->GetFirstPlayerController() : nullptr;
    ULocalPlayer* LocalPlayer = Player ? Player->GetLocalPlayer() : nullptr;
    if (Target && LocalPlayer && Viewport)
    {
        if (!Photo)
        {
            Photo = MakeShared<FBridgePhotoCapture>();
        }
        if (Photo->Start(*Target, *LocalPlayer, *Viewport))
        {
            bResumeControlAfterPhoto = bCaptured;
            ReleaseControl(TEXT("path-traced photo"));
        }
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("Photo shortcut received without an active Minecraft viewport."));
    }
}

bool UBridgeViewportClient::IsRenderingPhoto() const
{
    return Photo && Photo->IsRendering();
}

void UBridgeViewportClient::CancelPhoto(const TCHAR* Reason)
{
    bPhotoRequested = false;
    bResumeControlAfterPhoto = false;
    if (Photo)
    {
        Photo->Cancel(Reason);
    }
}

void UBridgeViewportClient::RequestControlRelease(const TCHAR* Reason)
{
    if (bCaptured)
    {
        PendingRelease = Reason;
    }
}

bool UBridgeViewportClient::ReleaseIfRequested()
{
    if (PendingRelease.IsEmpty())
    {
        return false;
    }
    const FString Reason = MoveTemp(PendingRelease);
    PendingRelease.Reset();
    ReleaseControl(*Reason);
    return true;
}

bool UBridgeViewportClient::InputKey(const FInputKeyEventArgs& EventArgs)
{
    if (IsConsoleActive())
    {
        return Super::InputKey(EventArgs);
    }
    using namespace BridgeWire;
    ABridgeViewer* Target = Viewer.Get();
    const bool bPressed = EventArgs.Event == IE_Pressed || EventArgs.Event == IE_DoubleClick;
    if (Target && !bCaptured && EventArgs.Key == EKeys::F8)
    {
        if (EventArgs.Event == IE_Pressed)
        {
            bDebugPanelVisible = !bDebugPanelVisible;
        }
        return true;
    }
    const int32 Button = MouseButton(EventArgs.Key);
    if (!bCaptured && Target && EventArgs.Viewport && Button != INDEX_NONE && bPressed && !EventArgs.bIsTouchEvent)
    {
        const bool bScreen = Target->IsScreenInput();
        const bool bCanStart = bScreen ? HasUiRect(EventArgs.Viewport) && UiRect.Contains(PointerPosition(EventArgs.Viewport))
                                      : Target->IsGameplayInput();
        if (bCanStart)
        {
            TryStartControl();
        }
    }
    if (bCaptured)
    {
        if (ReleaseIfRequested())
        {
            return true;
        }
        if (Target && Button != INDEX_NONE && !EventArgs.bIsTouchEvent)
        {
            if (Target->IsScreenInput() && HasUiRect(EventArgs.Viewport))
            {
                const FIntPoint Position = PointerPosition(EventArgs.Viewport);
                if ((bPressed && UiRect.Contains(Position)) || EventArgs.Event == IE_Released)
                {
                    SendPointer(Position);
                    Target->AddControlEvent({bPressed ? EInputAction::PointerDown : EInputAction::PointerUp,
                        uint32(Button), PointerModifiers()});
                }
            }
            else if (Target->IsScreenInput() && EventArgs.Event == IE_Released)
            {
                Target->AddControlEvent({EInputAction::PointerUp, uint32(Button), PointerModifiers()});
            }
            else if (Target->IsGameplayInput() && (bPressed || EventArgs.Event == IE_Released))
            {
                Target->AddControlEvent({bPressed ? EInputAction::GameplayDown : EInputAction::GameplayUp,
                    uint32(Button), PointerModifiers()});
            }
            return true;
        }
        if (!EventArgs.Key.IsMouseButton() && !EventArgs.Key.IsGamepadKey())
        {
            return true;
        }
    }
    if (Target && Target->IsSingleWindow() && (EventArgs.Key == EKeys::F11 ||
        (EventArgs.Key == EKeys::Enter && FSlateApplication::Get().GetModifierKeys().IsAltDown())))
    {
        // The engine's shortcuts otherwise change the window mode independently of Minecraft.
        return true;
    }
    return Super::InputKey(EventArgs);
}

void UBridgeViewportClient::FlushPendingMouseLook()
{
    if (!bHasPendingMouseX)
    {
        return;
    }
    const float DX = PendingMouseX;
    PendingMouseX = 0.0f;
    bHasPendingMouseX = false;
    if (bGameplayMouse && DX != 0.0f)
    {
        if (ABridgeViewer* Target = Viewer.Get())
        {
            Target->AddControlLook(DX, 0.0f);
        }
    }
}

bool UBridgeViewportClient::InputAxis(const FInputKeyEventArgs& EventArgs)
{
    if (EventArgs.Key == EKeys::MouseWheelAxis && EventArgs.Viewport &&
        SendWheel(EventArgs.AmountDepressed, false, PointerPosition(EventArgs.Viewport)))
    {
        return true;
    }
    if (bGameplayMouse)
    {
        if (ABridgeViewer* Target = Viewer.Get())
        {
            if (EventArgs.Key == EKeys::MouseX)
            {
                // Combine the paired MouseX/MouseY axes emitted by FSceneViewport into one atomic look update.
                PendingMouseX += EventArgs.AmountDepressed;
                bHasPendingMouseX = true;
                return true;
            }
            if (EventArgs.Key == EKeys::MouseY)
            {
                const float DX = bHasPendingMouseX ? PendingMouseX : 0.0f;
                PendingMouseX = 0.0f;
                bHasPendingMouseX = false;
                // UE's mouse axis is positive up. Minecraft's cursor delta is positive down.
                Target->AddControlLook(DX, -EventArgs.AmountDepressed);
                return true;
            }
        }
    }
    return Super::InputAxis(EventArgs);
}

bool UBridgeViewportClient::InputTouch(FViewport* InViewport, FTouchId TouchId, ETouchType::Type Type,
    const FVector2D& TouchLocation, float Force, uint64 Timestamp)
{
    using BridgeWire::EInputAction;
    ABridgeViewer* Target = Viewer.Get();
    if (!Target || ReleaseIfRequested())
    {
        return false;
    }
    if (Type == ETouchType::Began && !ActiveTouch)
    {
        const bool bScreen = Target->IsScreenInput();
        const FIntPoint Position = TouchLocation.IntPoint();
        if (bScreen ? !HasUiRect(InViewport) || !UiRect.Contains(Position) : !Target->IsGameplayInput())
        {
            return false;
        }
        if (!bCaptured && !TryStartControl())
        {
            return false;
        }
        ActiveTouch.Emplace(FActiveTouch{TouchId, TouchLocation, bScreen});
        if (bScreen)
        {
            SendPointer(Position);
        }
        Target->AddControlEvent({bScreen ? EInputAction::PointerDown : EInputAction::GameplayDown, 0, PointerModifiers()});
    }
    else if (ActiveTouch && ActiveTouch->Id == TouchId)
    {
        const bool bScreen = ActiveTouch->bScreen;
        if (bScreen && Target->IsScreenInput() && HasUiRect(InViewport))
        {
            SendPointer(TouchLocation.IntPoint());
        }
        if (!bScreen && Target->IsGameplayInput() && Type == ETouchType::Moved)
        {
            const FVector2D Delta = TouchLocation - ActiveTouch->Position;
            Target->AddControlLook(Delta.X, Delta.Y);
        }
        ActiveTouch->Position = TouchLocation;
        if (Type == ETouchType::Ended)
        {
            Target->AddControlEvent({bScreen ? EInputAction::PointerUp : EInputAction::GameplayUp, 0, PointerModifiers()});
            ActiveTouch.Reset();
            return true;
        }
    }
    return ActiveTouch.IsSet() || Super::InputTouch(InViewport, TouchId, Type, TouchLocation, Force, Timestamp);
}

EMouseCursor::Type UBridgeViewportClient::GetCursor(FViewport* InViewport, int32 X, int32 Y)
{
    return bGameplayMouse ? EMouseCursor::None : EMouseCursor::Default;
}

void UBridgeViewportClient::MouseMove(FViewport* InViewport, int32 X, int32 Y)
{
    ABridgeViewer* Target = Viewer.Get();
    if (bCaptured && !ActiveTouch && Target && Target->IsScreenInput() && HasUiRect(InViewport))
    {
        SendPointer(FIntPoint(X, Y));
    }
    Super::MouseMove(InViewport, X, Y);
}

void UBridgeViewportClient::CapturedMouseMove(FViewport* InViewport, int32 X, int32 Y)
{
    MouseMove(InViewport, X, Y);
}

void UBridgeViewportClient::LostFocus(FViewport* InViewport)
{
    bResumeControlAfterPhoto = false;
    bAutoCaptureOnFocus = true;
    ReleaseControl(TEXT("viewport lost focus"));
    Super::LostFocus(InViewport);
}

void UBridgeViewportClient::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);
    const uint32 OverlayRev = BridgeQuality::DebugOverlayRevision();
    if (OverlayRev != AppliedDebugOverlayRevision)
    {
        AppliedDebugOverlayRevision = OverlayRev;
        const int32 Override = BridgeQuality::DebugOverlayOverride();
        if (Override != INDEX_NONE)
        {
            bDebugPanelVisible = Override != 0;
        }
    }
    const int32 PhotoTrig = BridgeQuality::PhotoTriggerCounter();
    if (PhotoTrig > AppliedPhotoTrigger)
    {
        AppliedPhotoTrigger = PhotoTrig;
        ScheduledPhotoAt = FPlatformTime::Seconds() + 0.35;
    }
    if (ScheduledPhotoAt > 0.0 && FPlatformTime::Seconds() >= ScheduledPhotoAt)
    {
        ScheduledPhotoAt = 0.0;
        RequestPhoto();
    }
#if PLATFORM_WINDOWS
    if (WindowsInput)
    {
        WindowsInput->PollPhotoShortcut();
    }
#endif
    if (bPhotoRequested)
    {
        StartRequestedPhoto();
    }
    if (Photo)
    {
        Photo->Tick();
        if (bResumeControlAfterPhoto && !Photo->IsRendering())
        {
            bResumeControlAfterPhoto = false;
            if (const ABridgeViewer* Target = Viewer.Get(); Target && Target->IsGameplayInput())
            {
                TryStartControl();
            }
        }
    }
    if (IsConsoleActive())
    {
        return;
    }
    FlushPendingMouseLook();
    if (ReleaseIfRequested())
    {
        return;
    }
    ABridgeViewer* Target = Viewer.Get();
    if (!Target || !Viewport || !Viewport->HasFocus())
    {
        ReleaseControl(TEXT("viewport focus lost"));
        return;
    }
#if PLATFORM_WINDOWS
    if (WindowsInput && bCaptured && !WindowsInput->HasForeground())
    {
        bAutoCaptureOnFocus = true;
        ReleaseControl(TEXT("native window lost focus"));
        return;
    }
#endif
    if (!bCaptured && (!Target->IsScreenInput() || !CaptureFromKeyboard()))
    {
        return;
    }
    if (Target->IsGameplayInput() != bGameplayMouse)
    {
        SetGameplayMouse(Target->IsGameplayInput());
    }
#if PLATFORM_WINDOWS
    if (WindowsInput)
    {
        WindowsInput->SetTextInputEnabled(Target->IsTextInput());
    }
#endif
    if (Target->IsScreenInput() && !ActiveTouch && HasUiRect(Viewport))
    {
        // Slate stops reporting moves after the cursor leaves an uncaptured viewport.
        const FIntPoint Screen = FSlateApplication::Get().GetCursorPos().IntPoint();
        const FVector2D Local = Viewport->VirtualDesktopPixelToViewport(Screen) * FVector2D(Viewport->GetSizeXY());
        SendPointer(Local.IntPoint());
    }
}

void UBridgeViewportClient::ReleaseControl(const TCHAR* Reason)
{
    if (!bCaptured)
    {
        return;
    }
    bCaptured = false;
    bGameplayMouse = false;
    PendingMouseX = 0.0f;
    bHasPendingMouseX = false;
    LastPointer.Reset();
    ActiveTouch.Reset();
    PendingRelease.Reset();
#if PLATFORM_WINDOWS
    if (WindowsInput)
    {
        WindowsInput->Reset();
    }
#endif
    if (ABridgeViewer* Target = Viewer.Get())
    {
        Target->StopControl(Reason);
    }
    SetMouseCaptureMode(EMouseCaptureMode::CaptureDuringMouseDown);
    SetMouseLockMode(EMouseLockMode::DoNotLock);
    if (UWorld* GameWorld = GetWorld())
    {
        if (APlayerController* Player = GameWorld->GetFirstPlayerController())
        {
            Player->bShowMouseCursor = true;
            if (ULocalPlayer* Local = Player->GetLocalPlayer())
            {
                Local->GetSlateOperations() = FReply::Handled().ReleaseMouseCapture().ReleaseMouseLock();
            }
        }
    }
}

void UBridgeViewportClient::DrawMinecraftUi(UCanvas* Canvas)
{
    UiRect = FIntRect(0, 0, 0, 0);
    ABridgeViewer* Target = Viewer.Get();
    const UTexture2D* Ui = Target ? Target->GetUiTexture() : nullptr;
    const bool bGpuUi = Target && Target->HasGpuUi();
    if ((bGpuUi || (Ui && Ui->GetResource())) && Viewport && Canvas && Canvas->SceneView && Canvas->SizeX > 0 && Canvas->SizeY > 0)
    {
        // UE has already set the origin and dimensions to the letterboxed player view.
        UiViewportSize = Viewport->GetSizeXY();
        const FIntPoint Origin = Canvas->SceneView->UnscaledViewRect.Min;
        UiRect = FIntRect(Origin, Origin + FIntPoint(Canvas->SizeX, Canvas->SizeY));
        if (bGpuUi)
        {
            Target->DrawGpuUi(Canvas);
        }
        else
        {
            FCanvasTileItem Overlay(FVector2D::ZeroVector, Ui->GetResource(), FVector2D(Canvas->SizeX, Canvas->SizeY),
                FVector2D(0, 1), FVector2D(1, 0), FLinearColor::White);
            Overlay.BlendMode = Target->IsUiOpaque() ? SE_BLEND_Opaque :
                BridgeFrameGeneration::WritesHudCoverage() ? SE_BLEND_AlphaBlend : SE_BLEND_Translucent;
            Canvas->DrawItem(Overlay);
        }
        bDrewMinecraftUi = true;
    }
    if (Target)
    {
        Target->SetDebugPanelBottom(DrawDebugPanel(Canvas, *Target));
    }
    if (Canvas && Canvas->SceneView && Photo)
    {
        const FString PhotoStatus = Photo->GetStatus();
        if (!PhotoStatus.IsEmpty())
        {
            FCanvasTextItem Text(FVector2D(10.0, 10.0), FText::AsCultureInvariant(PhotoStatus),
                UEngine::GetSmallFont(), FLinearColor::White);
            Text.EnableShadow(FLinearColor::Black);
            Canvas->DrawItem(Text);
        }
    }
}

uint16 UBridgeViewportClient::DrawDebugPanel(UCanvas* Canvas, const ABridgeViewer& Target) const
{
    if (!bDebugPanelVisible || !Target.IsDrawingTerrain() || !Canvas || !Canvas->SceneView ||
        Canvas->SizeX <= 0 || Canvas->SizeY <= 0)
    {
        return 0;
    }
    const UFont* Font = UEngine::GetSmallFont();
    if (!Font)
    {
        return 0;
    }

    const ABridgeViewer::FDebugStats& Stats = Target.GetDebugStats();
    const FString Lines[] = {
        Stats.FrameMs > 0 ? FString::Printf(TEXT("UE %.0f FPS  |  %.1f ms"), 1000.0 / Stats.FrameMs, Stats.FrameMs)
                          : TEXT("UE -- FPS  |  -- ms"),
        FString::Printf(TEXT("Sections: %u loaded, %u queued | Regions: %u"), Stats.LoadedSections, Stats.PendingSections, Stats.LoadedRegions),
        FString::Printf(TEXT("View: %u chunks  |  %u columns"), uint32(Target.GetViewDistance()), uint32(Stats.ReceivedColumns))
    };
    constexpr double Margin = 6.0;
    constexpr double Padding = 5.0;
    double TextWidth = 0;
    double LineHeight = 0;
    for (const FString& Line : Lines)
    {
        double Width, Height;
        Canvas->StrLen(Font, Line, Width, Height);
        TextWidth = FMath::Max(TextWidth, Width);
        LineHeight = FMath::Max(LineHeight, Height + 2);
    }
    const double Width = TextWidth + 2 * Padding;
    const double Height = UE_ARRAY_COUNT(Lines) * LineHeight + 2 * Padding;
    const double MaxScale = FMath::Min(Canvas->SizeX / (Width + 2 * Margin), Canvas->SizeY / (Height + 2 * Margin));
    const double Scale = FMath::Min(FMath::Clamp(Canvas->SizeY / 540.0, 1.0, 2.5), MaxScale);
    const FVector2D Origin(Canvas->SizeX - (Width + Margin) * Scale, Margin * Scale);
    FCanvasTileItem Background(Origin, FVector2D(Width, Height) * Scale, FLinearColor(0.055f, 0.055f, 0.055f, 0.62f));
    Background.BlendMode = SE_BLEND_Translucent;
    Canvas->DrawItem(Background);

    FVector2D Position = Origin + FVector2D(Padding, Padding) * Scale;
    for (const FString& Line : Lines)
    {
        FCanvasTextItem Text(Position, FText::AsCultureInvariant(Line), Font, FLinearColor(0.88f, 0.88f, 0.88f));
        Text.Scale = FVector2D(Scale, Scale);
        Canvas->DrawItem(Text);
        Position.Y += LineHeight * Scale;
    }
    // The Minecraft UI texture fills this same view. Report the fraction used by the panel plus spacing.
    const double Bottom = (Height + 2 * Margin) * Scale / Canvas->SizeY;
    return uint16(FMath::CeilToInt(FMath::Min(Bottom, 1.0) * MAX_uint16));
}

FIntPoint UBridgeViewportClient::GetBridgeViewportSize() const
{
    return Viewport ? Viewport->GetSizeXY() : FIntPoint::ZeroValue;
}

void UBridgeViewportClient::ApplyInitialMinecraftWindow(FIntPoint Position, FIntPoint Size)
{
    FSceneViewport* SceneViewport = GetGameViewport();
    const TSharedPtr<SWindow> GameWindow = GetWindow();
    if (bInitialWindowHandled || !SceneViewport || !GameWindow)
    {
        return;
    }
    bInitialWindowHandled = true;
    if (GameWindow->IsWindowMaximized() || (InitialWindowBounds && WindowBounds(*GameWindow) != *InitialWindowBounds))
    {
        return;
    }
    SceneViewport->ResizeFrame(Size.X, Size.Y, EWindowMode::Windowed);
    GameWindow->MoveWindowTo(FVector2D(Position));
}

void UBridgeViewportClient::SetMinecraftFullscreen(bool bFullscreen)
{
    if (const TSharedPtr<SWindow> GameWindow = GetWindow())
    {
        const EWindowMode::Type Mode = bFullscreen ? EWindowMode::WindowedFullscreen : EWindowMode::Windowed;
        if (GameWindow->GetWindowMode() != Mode)
        {
            GameWindow->SetWindowMode(Mode);
            LastPointer.Reset();
            UE_LOG(LogTemp, Display, TEXT("Minecraft window mode: %s."), bFullscreen ? TEXT("Borderless fullscreen") : TEXT("Windowed"));
        }
    }
}

uint64 UBridgeViewportClient::PollMinecraftPresentation()
{
#if PLATFORM_WINDOWS
    const TSharedPtr<SWindow> GameWindow = GetWindow();
    const TSharedPtr<FGenericWindow> Native = GameWindow ? GameWindow->GetNativeWindow() : nullptr;
    if (!bDrewMinecraftUi || !WindowsInput || !Native || !Native->GetOSWindowHandle())
    {
        return 0;
    }
    if (!bHandoffFenceStarted)
    {
        WindowsInput->SetWindow(Native);
        // The previous frame queued the Minecraft canvas draw and Slate present.
        HandoffFence.BeginFence(FRenderCommandFence::ESyncDepth::RHIThread);
        bHandoffFenceStarted = true;
    }
    if (HandoffFence.IsFenceComplete() && WindowsInput->RevealWindow())
    {
        UE_LOG(LogTemp, Display, TEXT("Minecraft UI presented; UE window ready for handoff."));
        return reinterpret_cast<UPTRINT>(Native->GetOSWindowHandle());
    }
#endif
    return 0;
}

void UBridgeViewportClient::ResetMinecraftPresentation()
{
    bDrewMinecraftUi = false;
    bHandoffFenceStarted = false;
    bAutoCaptureOnFocus = true;
}

void UBridgeViewportClient::BeginDestroy()
{
    Photo.Reset();
    WindowsInput.Reset();
    if (FileDropLayer)
    {
        RemoveViewportWidgetContent(FileDropLayer.ToSharedRef());
        FileDropLayer.Reset();
    }
    Super::BeginDestroy();
}

bool UBridgeViewportClient::WindowCloseRequested()
{
    if (!Super::WindowCloseRequested())
    {
        return false;
    }
    if (ABridgeViewer* Target = Viewer.Get())
    {
        Target->CloseMinecraft();
    }
    return true;
}
