#include "BridgeViewer.h"

#include "BridgeViewportClient.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogMinecraftInput, Log, All);

void ABridgeViewer::CloseMinecraft()
{
    if (!Client || !bSingleWindow || bCloseSent)
    {
        return;
    }
    if (FileDropOffset != 0 && (!FlushFileDrop() || FileDropOffset != 0))
    {
        return;
    }
    int32 Sent = 0;
    if (InputOffset > 0 && (!Client->Send(InputBytes + InputOffset, InputSize - InputOffset, Sent) ||
        InputOffset + Sent != InputSize))
    {
        return;
    }
    InputOffset = 0;
    bInputPending = false;
    constexpr uint8 CloseFrame[] = {0, 0, 0, 1, uint8(BridgeWire::EMessage::Close)};
    bCloseSent = Client->Send(CloseFrame, sizeof(CloseFrame), Sent) && Sent == int32(sizeof(CloseFrame));
}

bool ABridgeViewer::ApplyInputAvailability(BridgeWire::FReader Reader, int32 Size)
{
    if (Size != 1)
    {
        return false;
    }
    const uint8 Mode = Reader.Byte();
    if (Mode > uint8(BridgeWire::EInputMode::Keyboard))
    {
        return false;
    }
    InputMode = static_cast<BridgeWire::EInputMode>(Mode);
    if (!IsGameplayInput())
    {
        InputLook = FVector2f::ZeroVector;
        PendingLookInputAt = 0;
        bInputDirty |= InputCapture != 0;
    }
    UE_CLOG(bTrace, LogMinecraftInput, Display, TEXT("Minecraft input mode %u."), uint32(Mode));
    if (InputMode == BridgeWire::EInputMode::Unavailable)
    {
        if (UBridgeViewportClient* Viewport = InputViewport.Get())
        {
            Viewport->ReleaseControl(TEXT("Minecraft gameplay unavailable"));
        }
    }
    return true;
}

bool ABridgeViewer::StartControl()
{
    if (!Client || !bReady || (bSingleWindow && !PresentingWindow) || InputMode == BridgeWire::EInputMode::Unavailable ||
        (IsGameplayInput() ? CameraFrames == 0 && Height != 0 : !UiTexture))
    {
        return false;
    }
    if (++CaptureSerial == 0)
    {
        ++CaptureSerial; // Zero is reserved for releasing control.
    }
    InputCapture = CaptureSerial;
    bLocalLookSynced = false;
    bLocalLookDirty = false;
    PollControlShm();
    if (!IsZeroLatencyLookReady())
    {
        PublishLocalLook(0, false);
    }
    bInputDirty = true;
    UE_LOG(LogMinecraftInput, Display, TEXT("Control captured (%u)."), InputCapture);
    return true;
}

void ABridgeViewer::StopControl(const TCHAR* Reason)
{
    if (InputCapture == 0)
    {
        return;
    }
    UE_LOG(LogMinecraftInput, Display, TEXT("Control released (%u): %s."), InputCapture, Reason);
    InputCapture = 0;
    bLocalLookSynced = false;
    bLocalLookDirty = false;
    PublishLocalLook(0, false);
    InputLook = FVector2f::ZeroVector;
    PendingLookInputAt = 0;
    InputEvents.Reset();
    // A partially written frame must finish before its release, but an unsent one can be discarded.
    if (bInputPending && InputOffset == 0)
    {
        ReleasedUiSlots |= InputBytes[BridgeWire::ReleasedUiSlotsOffset];
        bInputPending = false;
    }
    bInputDirty = true;
}

void ABridgeViewer::AddControlLook(float DX, float DY)
{
    if (!InputCapture || !IsGameplayInput() || (!DX && !DY) || !FMath::IsFinite(DX) || !FMath::IsFinite(DY))
    {
        return;
    }
    using namespace BridgeWire;
    const double Now = FPlatformTime::Seconds();
    if (bTrace)
    {
        LastLookInputAt = Now;
        if (PendingLookInputAt == 0)
        {
            PendingLookInputAt = Now;
        }
    }
    PollControlShm();
    if (IsZeroLatencyLookReady())
    {
        // Exact match to Minecraft 26.2 MouseHandler::turnPlayer + Entity::turn (including GCD step size).
        const double Factor = McState.MouseFactor;
        const double DeltaX = double(DX) * Factor * ((McState.Flags & BridgeShm::FlagInvertX) ? -1.0 : 1.0);
        const double DeltaY = double(DY) * Factor * ((McState.Flags & BridgeShm::FlagInvertY) ? -1.0 : 1.0);
        LocalYawDeg += float(DeltaX) * 0.15f;
        LocalPitchDeg = FMath::Clamp(LocalPitchDeg + float(DeltaY) * 0.15f, -90.0f, 90.0f);
        bLocalLookDirty = true;
        if (++NextLookSerial == 0)
        {
            ++NextLookSerial;
        }
        if (bTrace)
        {
            LookStamps[NextLookSerial % UE_ARRAY_COUNT(LookStamps)] = {NextLookSerial, Now};
            PendingLookInputAt = 0;
        }
        PublishLocalLook(NextLookSerial, true);
        return;
    }
    InputLook.X = FMath::Clamp(InputLook.X + DX, -MaxMouseDelta, MaxMouseDelta);
    InputLook.Y = FMath::Clamp(InputLook.Y + DY, -MaxMouseDelta, MaxMouseDelta);
    bInputDirty = true;
}

bool ABridgeViewer::AddControlEvent(const BridgeWire::FInputEvent& Event)
{
    if (!InputCapture)
    {
        return false;
    }
    using BridgeWire::EInputAction;
    if (InputEvents.Num() >= BridgeWire::MaxPendingInputEvents)
    {
        if (UBridgeViewportClient* Viewport = InputViewport.Get())
        {
            Viewport->RequestControlRelease(TEXT("input queue full"));
        }
        return true;
    }
    InputEvents.Add(Event);
    bInputDirty = true;
    UE_CLOG(bTrace && IsGameplayInput() && Event.Action < EInputAction::Character, LogMinecraftInput,
        Display, TEXT("Key scan=0x%03x action=%u modifiers=0x%02x."),
        Event.Value, uint32(Event.Action), uint32(Event.Flags));
    UE_CLOG(bTrace && Event.Action >= EInputAction::PointerDown, LogMinecraftInput,
        Display, TEXT("Mouse action=%u value=0x%08x flags=0x%02x."), uint32(Event.Action), Event.Value, uint32(Event.Flags));
    return true;
}

bool ABridgeViewer::FlushInput(bool bControlOnly)
{
    if (!Client)
    {
        return true;
    }
    if (!FlushSessionReply())
    {
        return false;
    }
    if (bSessionReplyPending || !bReady || !bWindowReceived)
    {
        return true;
    }
    if (FileDropOffset != 0)
    {
        if (!FlushFileDrop())
        {
            return false;
        }
        if (FileDropOffset != 0)
        {
            return true;
        }
    }
    const double Now = FPlatformTime::Seconds();
    if (!bControlOnly)
    {
        if (UBridgeViewportClient* Viewport = InputViewport.Get())
        {
            const FIntPoint Size = Viewport->GetBridgeViewportSize();
            if (BridgeWire::IsUiSize(Size.X, Size.Y))
            {
                const uint32 Packed = (uint32(Size.X) << 16) | uint32(Size.Y);
                bInputDirty |= ReportedViewport != Packed;
                ReportedViewport = Packed;
            }
            if (bSingleWindow && !PresentingWindow)
            {
                PresentingWindow = Viewport->PollMinecraftPresentation();
                bInputDirty |= PresentingWindow != 0;
            }
        }
        if (ReadyScene != SceneSerial && bSceneMeshesReceived && CameraFrames > 0 && GetPendingSectionCount() == 0)
        {
            ReadyScene = SceneSerial;
            bInputDirty = true;
        }
        SampleDebugStats(Now);
    }
    for (int32 I = 0; I < 2; ++I)
    {
        if (!bInputPending)
        {
            if (!bInputDirty && TerrainReleases.IsEmpty() && (!InputCapture || Now < NextInputHeartbeat))
            {
                return bControlOnly || FlushFileDrop();
            }
            EncodeInput(Now);
        }
        int32 Sent = 0;
        if (!Client->Send(InputBytes + InputOffset, InputSize - InputOffset, Sent))
        {
            const auto Error = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
            return Error == SE_EWOULDBLOCK;
        }
        InputOffset += Sent;
        if (InputOffset < InputSize)
        {
            return true;
        }
        if (bTrace && EncodedLookSerial != 0 && EncodedLookInputAt > 0)
        {
            LookStamps[EncodedLookSerial % UE_ARRAY_COUNT(LookStamps)] = {EncodedLookSerial, EncodedLookInputAt};
        }
        if (EncodedTerrainReleaseCount > 0)
        {
            TerrainReleases.RemoveAt(0, EncodedTerrainReleaseCount, EAllowShrinking::No);
            EncodedTerrainReleaseCount = 0;
        }
        bInputPending = false;
        InputOffset = 0;
    }
    return true;
}

bool ABridgeViewer::AddDroppedFiles(const TArray<FString>& Files)
{
    using namespace BridgeWire;
    if (!Client || !bReady || (bSingleWindow && !PresentingWindow) || !IsScreenInput() ||
        Files.IsEmpty() || Files.Num() > MaxDroppedFiles || !FileDropBytes.IsEmpty())
    {
        return false;
    }
    TArray<uint8> Frame;
    Frame.SetNumUninitialized(10);
    Frame[4] = uint8(EMessage::FileDrop);
    WriteBigEndian(Frame.GetData() + 5, SceneSerial);
    Frame[9] = uint8(Files.Num());
    for (const FString& File : Files)
    {
        const FTCHARToUTF8 Path(*File);
        const int32 Length = Path.Length();
        if (Length == 0 || Length > MaxFilePathBytes || Frame.Num() + 2 + Length > 4 + MaxFileDropBytes)
        {
            return false;
        }
        Frame.Add(uint8(Length >> 8));
        Frame.Add(uint8(Length));
        Frame.Append(reinterpret_cast<const uint8*>(Path.Get()), Length);
    }
    WriteBigEndian(Frame.GetData(), Frame.Num() - 4);
    FileDropBytes = MoveTemp(Frame);
    return true;
}

bool ABridgeViewer::FlushFileDrop()
{
    if (FileDropBytes.IsEmpty())
    {
        return true;
    }
    int32 Sent = 0;
    if (!Client->Send(FileDropBytes.GetData() + FileDropOffset, FileDropBytes.Num() - FileDropOffset, Sent))
    {
        return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode() == SE_EWOULDBLOCK;
    }
    FileDropOffset += Sent;
    if (FileDropOffset == FileDropBytes.Num())
    {
        FileDropOffset = 0;
        FileDropBytes.Reset();
    }
    return true;
}

void ABridgeViewer::EncodeInput(double Now)
{
    using namespace BridgeWire;
    const int32 EventCount = FMath::Min(MaxInputEventsPerFrame, InputEvents.Num());
    EncodedTerrainReleaseCount = FMath::Min(MaxTerrainReleases, TerrainReleases.Num());
    const int32 PayloadSize = InputBaseSize + InputEventSize * EventCount + 8 * EncodedTerrainReleaseCount;
    InputSize = 4 + PayloadSize;
    WriteBigEndian(InputBytes, PayloadSize);
    InputBytes[4] = uint8(EMessage::Input);
    WriteBigEndian(InputBytes + 5, InputCapture);
    WriteFloat(InputBytes + 9, InputLook.X);
    WriteFloat(InputBytes + 13, InputLook.Y);
    WriteBigEndian(InputBytes + 17, ReportedViewport);
    WriteBigEndian(InputBytes + 21, ReadyScene);
    InputBytes[ReleasedUiSlotsOffset] = ReleasedUiSlots;
    InputBytes[26] = uint8(EventCount);
    WriteBigEndian(InputBytes + 27, SceneSerial);
    WriteFloat(InputBytes + 31, DebugStats.FrameMs);
    WriteBigEndian(InputBytes + 35, DebugStats.LoadedSections);
    WriteBigEndian(InputBytes + 39, DebugStats.PendingSections);
    InputBytes[43] = uint8(DebugStats.ReceivedColumns >> 8);
    InputBytes[44] = uint8(DebugStats.ReceivedColumns);
    InputBytes[45] = ViewDistance;
    InputBytes[46] = uint8(FpsLimit >> 8);
    InputBytes[47] = uint8(FpsLimit);
    InputBytes[48] = bVSync ? 1 : 0;
    InputBytes[49] = uint8(DebugPanelBottom >> 8);
    InputBytes[50] = uint8(DebugPanelBottom);
    EncodedLookSerial = 0;
    if (InputLook.X != 0.0f || InputLook.Y != 0.0f)
    {
        if (++NextLookSerial == 0)
        {
            ++NextLookSerial;
        }
        EncodedLookSerial = NextLookSerial;
    }
    EncodedLookInputAt = PendingLookInputAt;
    WriteBigEndian(InputBytes + 51, EncodedLookSerial);
    WriteBigEndian(InputBytes + 55, uint32(PresentingWindow >> 32));
    WriteBigEndian(InputBytes + 59, uint32(PresentingWindow));
    WriteBigEndian(InputBytes + 63, AtlasReplyScene);
    InputBytes[67] = uint8(AtlasReplyStatus);
    InputBytes[68] = uint8(EncodedTerrainReleaseCount);
    uint8* Cursor = InputBytes + 4 + InputBaseSize;
    for (int32 I = 0; I < EventCount; ++I)
    {
        const BridgeWire::FInputEvent& Event = InputEvents[I];
        Cursor[0] = uint8(Event.Action);
        WriteBigEndian(Cursor + 1, Event.Value);
        Cursor[5] = Event.Flags;
        Cursor += InputEventSize;
    }
    for (int32 Index = 0; Index < EncodedTerrainReleaseCount; ++Index)
    {
        const uint64 Token = TerrainReleases[Index];
        WriteBigEndian(Cursor, uint32(Token >> 32));
        WriteBigEndian(Cursor + 4, uint32(Token));
        Cursor += 8;
    }
    if (EventCount > 0)
    {
        InputEvents.RemoveAt(0, EventCount, EAllowShrinking::No);
    }
    ReleasedUiSlots = 0;
    InputLook = FVector2f::ZeroVector;
    PendingLookInputAt = 0;
    bInputDirty = !InputEvents.IsEmpty();
    bInputPending = true;
    NextInputHeartbeat = Now + 0.1;
}
