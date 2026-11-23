#include "BridgeViewer.h"

#include "BridgeGlassCapture.h"
#include "BridgeViewportClient.h"
#include "BridgeWeatherProbe.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "IPAddress.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "MinecraftWeatherSubsystem.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Widgets/SWindow.h"

DEFINE_LOG_CATEGORY_STATIC(LogMinecraftBridge, Log, All);

extern ENGINE_API float GAverageMS;

namespace
{
    using namespace BridgeWire;

    bool InRange(int32 Value, int32 Limit)
    {
        return Value >= -Limit && Value <= Limit;
    }

    void CloseSocket(FSocket*& Socket)
    {
        if (Socket)
        {
            Socket->Close();
            ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
            Socket = nullptr;
        }
    }

    FSocket* ListenOnLoopback(int32 LocalPort, const TCHAR* Description)
    {
        ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
        const TSharedRef<FInternetAddr> Address = Sockets->CreateInternetAddr();
        bool bValid = false;
        Address->SetIp(TEXT("127.0.0.1"), bValid);
        Address->SetPort(LocalPort);
        FSocket* Socket = Sockets->CreateSocket(NAME_Stream, Description, FNetworkProtocolTypes::IPv4);
        if (!bValid || !Socket || !Socket->SetNonBlocking(true) || !Socket->Bind(*Address) || !Socket->Listen(2))
        {
            UE_LOG(LogMinecraftBridge, Error, TEXT("Could not listen on 127.0.0.1:%d (socket error %d)."),
                LocalPort, int32(Sockets->GetLastErrorCode()));
            CloseSocket(Socket);
        }
        return Socket;
    }

    double Percentile(const TArray<double>& Sorted, double Fraction)
    {
        return Sorted.IsEmpty() ? 0.0 : Sorted[FMath::CeilToInt(Sorted.Num() * Fraction) - 1];
    }
}

ABridgeViewer::ABridgeViewer()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PrePhysics;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("TerrainRoot"));
    RootComponent->SetMobility(EComponentMobility::Movable);
    LocalPlayerRoot = CreateDefaultSubobject<USceneComponent>(TEXT("LocalPlayerRoot"));
    LocalPlayerRoot->SetupAttachment(RootComponent);
    LocalPlayerRoot->SetMobility(EComponentMobility::Movable);
    CreateEnvironmentComponents();
}

void ABridgeViewer::BeginPlay()
{
    Super::BeginPlay();
    if (UMinecraftWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UMinecraftWeatherSubsystem>())
    {
        Weather->Bridge = this;
    }
    TerrainMaterialBases.SetNum(McTerrain::LayerCount);
    TerrainMaterialBases[McTerrain::Solid] = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/Bridge/M_McSolid.M_McSolid"));
    TerrainMaterialBases[McTerrain::Cutout] = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/Bridge/M_McCutout.M_McCutout"));
    TerrainMaterialBases[McTerrain::CutoutNoShadow] = TerrainMaterialBases[McTerrain::Cutout];
    TerrainMaterialBases[McTerrain::Foliage] = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/Bridge/M_McFoliage.M_McFoliage"));
    TerrainMaterialBases[McTerrain::Water] = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/Bridge/M_McWater.M_McWater"));
    TerrainMaterialBases[McTerrain::Translucent] = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/Bridge/M_McTranslucent.M_McTranslucent"));
    TerrainMaterialBases[McTerrain::Vegetation] = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/Bridge/M_McVegetation.M_McVegetation"));
    if (TerrainMaterialBases.Contains(nullptr))
    {
        UE_LOG(LogMinecraftBridge, Error, TEXT("Could not load the terrain atlas materials."));
        return;
    }
    const bool bLogMaterials = FParse::Param(FCommandLine::Get(), TEXT("BridgeTrace"));
    for (UMaterialInterface* Material : TerrainMaterialBases)
    {
        UE_CLOG(bLogMaterials, LogMinecraftBridge, Display, TEXT("Preparing terrain shader %s."), *Material->GetName());
        Material->EnsureIsComplete();
    }
    Camera = GetWorld()->SpawnActor<ACameraActor>();
    Camera->GetCameraComponent()->SetConstraintAspectRatio(true);
    Camera->GetCameraComponent()->SetEnableFirstPersonFieldOfView(true);
    Camera->GetCameraComponent()->SetEnableFirstPersonScale(true);
    Camera->GetCameraComponent()->SetFirstPersonScale(0.4f);
    WaterSim = MakeUnique<FBridgeWaterSim>(GetWorld());
    WaterOptics = MakeUnique<FBridgeWaterOptics>(GetWorld());
    WaterParticles = MakeUnique<FBridgeWaterParticles>(GetWorld());
    GlassCapture = FBridgeGlassCapture::Create(GetWorld());
    LoadDynamicMaterials();
    ConfigureEnvironment();
    InputViewport = Cast<UBridgeViewportClient>(GetWorld()->GetGameViewport());
    if (UBridgeViewportClient* Viewport = InputViewport.Get())
    {
        Viewport->SetViewer(this);
    }
    bTrace = FParse::Param(FCommandLine::Get(), TEXT("BridgeTrace"));
    bTerrainNoAF = FParse::Param(FCommandLine::Get(), TEXT("BridgeTerrainNoAF"));
    bTerrainMip0 = FParse::Param(FCommandLine::Get(), TEXT("BridgeTerrainMip0"));
    if (bTrace)
    {
        CameraIntervals.Reserve(256);
        LookRoundTrips.Reserve(256);
    }
    OpenListener();
    UE_LOG(LogMinecraftBridge, Display, TEXT("Minecraft UE bridge protocol %u initialized."), BridgeWire::Version);
}

void ABridgeViewer::EndPlay(const EEndPlayReason::Type Reason)
{
    CloseMinecraft();
    Disconnect();
    if (UMinecraftWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UMinecraftWeatherSubsystem>();
        Weather && Weather->Bridge.Get() == this)
    {
        Weather->Bridge.Reset();
    }
    CloseSocket(Listener);
    WaterParticles.Reset();
    WaterOptics.Reset();
    WaterSim.Reset();
    GlassCapture.Reset();
    if (Camera)
    {
        Camera->Destroy();
    }
    Super::EndPlay(Reason);
}

void ABridgeViewer::Tick(float DeltaSeconds)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftBridgeTick);
    Super::Tick(DeltaSeconds);
    if (APlayerController* Player = GetWorld()->GetFirstPlayerController())
    {
        if (Camera && Player->GetViewTarget() != Camera)
        {
            Player->SetViewTarget(Camera);
        }
    }
    // Slate has already submitted both mouse axes. Flush any unpaired axis and get this batch on the wire before terrain.
    if (UBridgeViewportClient* Viewport = InputViewport.Get())
    {
        Viewport->FlushPendingMouseLook();
    }
    if (!FlushInput(true))
    {
        Disconnect();
        return;
    }
    PollConnection();
    if (const UBridgeViewportClient* Viewport = InputViewport.Get(); Viewport && Viewport->IsRenderingPhoto())
    {
        // Keep the HUD fences and input heartbeat live while the scene is held still.
        UploadUi();
        PollGpuUi();
        if (!FlushInput())
        {
            Disconnect();
        }
        return;
    }
    if (!PollAtlasCache())
    {
        Disconnect();
        return;
    }
    UpdateFrameRateLimit();
    UploadUi();
    PollGpuUi();
    if (!UploadPendingSections())
    {
        Disconnect();
        return;
    }
    PresentCamera(DeltaSeconds);
    TickEnvironmentEffects(DeltaSeconds);
#if !UE_BUILD_SHIPPING
    if (const UMinecraftWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UMinecraftWeatherSubsystem>())
    {
        PollBridgeWeatherProbe(*Weather, *this);
    }
#endif
    PresentDynamicMeshes();
    UpdatePointLights();
    UpdateGlassLighting();
    if (GlassCapture)
    {
        GlassCapture->Tick(Environment.IsSet() && CameraFrames > 0 && LoadedSections > 0 &&
            PendingSections.IsEmpty() && DecodeJobs.IsEmpty(), GlassLighting);
    }
    if (!FlushInput())
    {
        Disconnect();
        return;
    }
    Trace();
}

void ABridgeViewer::OpenListener()
{
    const FString ReadyFile = FPlatformMisc::GetEnvironmentVariable(TEXT("MCUE_READY_FILE"));
    const bool bManaged = !ReadyFile.IsEmpty();
    int32 WorldPort = BridgeWire::Port;
    FParse::Value(FCommandLine::Get(), TEXT("BridgePort="), WorldPort);
    if (!bManaged && (WorldPort < 1 || WorldPort > 65535))
    {
        UE_LOG(LogMinecraftBridge, Error, TEXT("BridgePort must be in the range 1 to 65535."));
        return;
    }
    WorldPort = bManaged ? 0 : WorldPort;
    Listener = ListenOnLoopback(WorldPort, TEXT("Minecraft Bridge"));
    if (!Listener)
    {
        if (bManaged)
        {
            FPlatformMisc::RequestExit(false, TEXT("MinecraftUEViewer.ListenerOpenFailed"));
        }
        return;
    }
    if (bManaged)
    {
        const TSharedRef<FInternetAddr> Address = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
        Listener->GetAddress(*Address);
        WorldPort = Address->GetPort();
        const FString Ready = FString::Printf(TEXT("%u %u %d\n"), FPlatformProcess::GetCurrentProcessId(),
            BridgeWire::Version, WorldPort);
        const FString Pending = ReadyFile + TEXT(".tmp");
        if (!FFileHelper::SaveStringToFile(Ready, *Pending, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            || !IFileManager::Get().Move(*ReadyFile, *Pending, false, false, false, true))
        {
            FPlatformMisc::RequestExit(false, TEXT("MinecraftUEViewer.ReadyFileWriteFailed"));
            return;
        }
    }
    UE_LOG(LogMinecraftBridge, Display, TEXT("Waiting for Minecraft on 127.0.0.1:%d (one bidirectional TCP channel)."), WorldPort);
}

void ABridgeViewer::PollConnection()
{
    bool bPending = false;
    if (Listener && !Client && Listener->HasPendingConnection(bPending) && bPending)
    {
        Client = Listener->Accept(TEXT("Minecraft Bridge Client"));
        if (Client)
        {
            if (!Client->SetNonBlocking(true))
            {
                Disconnect();
                return;
            }
            Client->SetNoDelay(true);
            ConnectionStartedAt = FPlatformTime::Seconds();
            UE_LOG(LogMinecraftBridge, Display, TEXT("Minecraft bridge connected."));
        }
    }
    if (Client && !ReadFrames())
    {
        Disconnect();
    }
    if (Client && !bReady && FPlatformTime::Seconds() - ConnectionStartedAt > 5.0)
    {
        UE_LOG(LogMinecraftBridge, Warning, TEXT("Minecraft session handshake timed out."));
        Disconnect();
    }
}

bool ABridgeViewer::ReadFrames()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftReceiveFrames);
    using BridgeWire::EMessage;
    UBridgeViewportClient* Viewport = InputViewport.Get();
    if ((!Viewport || !Viewport->IsRenderingPhoto()) && !ReplayPhotoFrames())
    {
        return false;
    }
    constexpr int32 BufferSize = 32 * 1024;
    constexpr int32 MaxDeferredPhotoBytes = 16 * 1024 * 1024;
    uint8 Scratch[BufferSize];
    // Keep draining the control/UI/terrain socket so terrain backlog cannot stall UI or input availability.
    for (int32 Read = 0; Read < 32 &&
        Client->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()); ++Read)
    {
        int32 Received = 0;
        if (!Client->Recv(Scratch, sizeof(Scratch), Received) || Received <= 0)
        {
            return false;
        }
        ReceiveBuffer.Append(Scratch, Received);
        while (ReceiveBuffer.Num() - ReadOffset >= 5)
        {
            const uint8* Frame = ReceiveBuffer.GetData() + ReadOffset;
            const uint32 Size = BridgeWire::ReadBigEndian(Frame);
            if (Size == 0 || Size > BridgeWire::MaxFrame)
            {
                UE_LOG(LogMinecraftBridge, Warning, TEXT("Rejected invalid bridge frame length %u."), Size);
                return false;
            }
            if (ReceiveBuffer.Num() - ReadOffset - 4 < int32(Size))
            {
                break;
            }
            if (Viewport && Viewport->IsRenderingPhoto())
            {
                const EMessage Type = static_cast<EMessage>(Frame[4]);
                if (Type != EMessage::Ui && Type != EMessage::GpuUi && Type != EMessage::InputAvailability)
                {
                    // Atlas reloads and world changes expect immediate replies or change the scene identity.
                    if (Type == EMessage::World || Type == EMessage::Close || Type == EMessage::AtlasReference ||
                        Type == EMessage::AtlasUse || Type == EMessage::AtlasRows ||
                        DeferredPhotoFrames.Num() + 4 + int32(Size) > MaxDeferredPhotoBytes)
                    {
                        Viewport->CancelPhoto(TEXT("Minecraft scene changed"));
                        if (!ReplayPhotoFrames())
                        {
                            return false;
                        }
                    }
                    else
                    {
                        DeferredPhotoFrames.Append(Frame, 4 + int32(Size));
                        ReadOffset += 4 + int32(Size);
                        continue;
                    }
                }
            }
            if (!ApplyFrame(Frame + 4, int32(Size)))
            {
                UE_LOG(LogMinecraftBridge, Warning, TEXT("Rejected bridge message type %u."), uint32(Frame[4]));
                return false;
            }
            ReadOffset += 4 + int32(Size);
        }
        if (ReadOffset == ReceiveBuffer.Num())
        {
            ReceiveBuffer.Reset();
            ReadOffset = 0;
        }
        else if (ReadOffset >= BufferSize)
        {
            ReceiveBuffer.RemoveAt(0, ReadOffset, EAllowShrinking::No);
            ReadOffset = 0;
        }
    }
    return true;
}

bool ABridgeViewer::ReplayPhotoFrames()
{
    for (int32 Offset = 0; Offset < DeferredPhotoFrames.Num();)
    {
        const uint8* Frame = DeferredPhotoFrames.GetData() + Offset;
        const int32 Size = int32(BridgeWire::ReadBigEndian(Frame));
        if (!ApplyFrame(Frame + 4, Size))
        {
            UE_LOG(LogMinecraftBridge, Warning, TEXT("Rejected deferred bridge message type %u."), uint32(Frame[4]));
            return false;
        }
        Offset += 4 + Size;
    }
    DeferredPhotoFrames.Empty();
    return true;
}

bool ABridgeViewer::ApplyFrame(const uint8* Data, int32 Size)
{
    const auto Type = static_cast<BridgeWire::EMessage>(Data[0]);
    BridgeWire::FReader Reader{Data + 1};
    --Size;
    if (!bReady)
    {
        return bSessionResetReceived
            ? Type == BridgeWire::EMessage::ControlShm && ApplyControlShm(Reader, Size)
            : Type == BridgeWire::EMessage::Reset && ApplyReset(Reader, Size);
    }
    switch (Type)
    {
        case BridgeWire::EMessage::TerrainShm: return ApplyTerrainShm(Reader, Size);
        case BridgeWire::EMessage::DropChunk: return ApplyDrop(Reader, Size);
        case BridgeWire::EMessage::AtlasRows: return ApplyAtlasRows(BridgeWire::ETerrainAtlas::Albedo, Reader, Size);
        case BridgeWire::EMessage::NormalAtlasRows: return ApplyAtlasRows(BridgeWire::ETerrainAtlas::Normal, Reader, Size);
        case BridgeWire::EMessage::PbrAtlasRows: return ApplyAtlasRows(BridgeWire::ETerrainAtlas::Pbr, Reader, Size);
        case BridgeWire::EMessage::AtlasReference: return ApplyAtlasReference(Reader, Size);
        case BridgeWire::EMessage::AtlasUse: return ApplyAtlasUse(Reader, Size);
        case BridgeWire::EMessage::AtlasPatch: return ApplyAtlasPatch(Reader, Size);
        case BridgeWire::EMessage::EnvironmentState: return ApplyEnvironmentState(Reader, Size);
        case BridgeWire::EMessage::ChunkWaterGrid: return ApplyChunkWaterGrid(Reader, Size);
        case BridgeWire::EMessage::ChunkWaterBubbles: return ApplyChunkWaterBubbles(Reader, Size);
        case BridgeWire::EMessage::SectionGlass: return ApplySectionGlass(Reader, Size);
        case BridgeWire::EMessage::ChunkPrecipitation: return ApplyChunkPrecipitation(Reader, Size);
        case BridgeWire::EMessage::LightningStrike: return ApplyLightningStrike(Reader, Size);
        case BridgeWire::EMessage::InputAvailability: return ApplyInputAvailability(Reader, Size);
        case BridgeWire::EMessage::Ui: return ApplyUi(Reader, Size);
        case BridgeWire::EMessage::GpuUi: return ApplyGpuUi(Reader, Size);
        case BridgeWire::EMessage::World: return ApplyWorld(Reader, Size);
        case BridgeWire::EMessage::SceneReady: return ApplySceneReady(Reader, Size);
        case BridgeWire::EMessage::Window: return ApplyWindow(Reader, Size);
        case BridgeWire::EMessage::WindowMode: return ApplyWindowMode(Reader, Size);
        case BridgeWire::EMessage::VideoSettings: return ApplyVideoSettings(Reader, Size);
        case BridgeWire::EMessage::CrackAtlas: return ApplyCrackAtlas(Reader, Size);
        case BridgeWire::EMessage::CrackUpdate: return ApplyCrackUpdate(Reader, Size);
        case BridgeWire::EMessage::DynamicTextureRows: return ApplyDynamicTextureRows(Reader, Size);
        case BridgeWire::EMessage::DynamicMesh: return ApplyDynamicMesh(Reader, Size);
        case BridgeWire::EMessage::InstanceBatch: return ApplyInstanceBatch(Reader, Size);
        case BridgeWire::EMessage::Close:
            if (Size != 0) return false;
            if (bSingleWindow) RequestEngineExit(TEXT("Minecraft quit"));
            return true;
        default: return false;
    }
}

bool ABridgeViewer::ApplyReset(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != ResetPayloadBytes || Reader.U32() != Magic)
    {
        return false;
    }
    const uint32 PeerVersion = Reader.U32();
    const uint64 High = Reader.U32();
    SessionNonce = (High << 32) | Reader.U32();
    if (PeerVersion != Version)
    {
        UE_LOG(LogMinecraftBridge, Error, TEXT("Minecraft bridge protocol mismatch: Fabric=%u, renderer=%u."),
            PeerVersion, Version);
        // A fresh loopback socket has room for this diagnostic response before rejection.
        EncodeSessionReply();
        FlushSessionReply();
        return false;
    }
    bSessionResetReceived = true;
    return true;
}

void ABridgeViewer::EncodeSessionReply()
{
    using namespace BridgeWire;
    WriteBigEndian(SessionReply, 1 + ResetPayloadBytes);
    SessionReply[4] = uint8(EMessage::Reset);
    WriteBigEndian(SessionReply + 5, Magic);
    WriteBigEndian(SessionReply + 9, Version);
    WriteBigEndian(SessionReply + 13, uint32(SessionNonce >> 32));
    WriteBigEndian(SessionReply + 17, uint32(SessionNonce));
    SessionReplyOffset = 0;
    bSessionReplyPending = true;
}

bool ABridgeViewer::FlushSessionReply()
{
    if (!bSessionReplyPending)
    {
        return true;
    }
    int32 Sent = 0;
    if (!Client->Send(SessionReply + SessionReplyOffset, int32(sizeof(SessionReply)) - SessionReplyOffset, Sent))
    {
        return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode() == SE_EWOULDBLOCK;
    }
    SessionReplyOffset += Sent;
    bSessionReplyPending = SessionReplyOffset < int32(sizeof(SessionReply));
    return true;
}

bool ABridgeViewer::ApplyVideoSettings(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != 6)
    {
        return false;
    }
    const uint8 Distance = Reader.Byte();
    const uint16 Limit = Reader.U16();
    const uint8 Flags = Reader.Byte();
    const auto Filtering = static_cast<ETerrainFiltering>(Reader.Byte());
    const uint8 Anisotropy = Reader.Byte();
    if (Distance < 2 || Distance > MaxViewDistance ||
        (Limit != 0 && (Limit < 10 || Limit > 250)) || (Flags & ~3) != 0 ||
        Filtering > ETerrainFiltering::Anisotropic || Anisotropy < 1 || Anisotropy > 8 ||
        (Filtering != ETerrainFiltering::Anisotropic && Anisotropy != 1))
    {
        return false;
    }

    IConsoleVariable* VSyncCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));
    if (!PreviousDisplaySettings)
    {
        PreviousDisplaySettings = FDisplaySettings{GEngine->GetMaxFPS(), VSyncCVar->GetInt()};
        DisplaySettingsStartedAt = FPlatformTime::Seconds();
    }
    VSyncCVar->SetWithCurrentPriority(int32(Flags & 1));
    ViewDistance = Distance;
    RequestedFpsLimit = Limit;
    bThrottleWhenAfk = (Flags & 2) != 0;
    bVSync = VSyncCVar->GetInt() != 0;
    bInputDirty = true;
    if (TerrainFiltering != Filtering || RequestedAnisotropy != Anisotropy)
    {
        TerrainFiltering = Filtering;
        RequestedAnisotropy = Anisotropy;
        UpdateTerrainFiltering();
    }
    UE_LOG(LogMinecraftBridge, Display, TEXT("Minecraft video settings: view=%u chunks, fpsLimit=%u (0=unlimited), vsync=%s, reduceFps=%s."),
        uint32(ViewDistance), uint32(RequestedFpsLimit), bVSync ? TEXT("on") : TEXT("off"),
        bThrottleWhenAfk ? TEXT("afk") : TEXT("minimized"));
    return true;
}

void ABridgeViewer::UpdateFrameRateLimit()
{
    if (!PreviousDisplaySettings)
    {
        return;
    }
    const UBridgeViewportClient* Viewport = InputViewport.Get();
    const TSharedPtr<SWindow> Window = Viewport ? Viewport->GetWindow() : nullptr;
    int32 Throttle = 0;
    if (Window && Window->IsWindowMinimized())
    {
        Throttle = 10;
    }
    else if (bThrottleWhenAfk && FSlateApplication::IsInitialized())
    {
        const FSlateApplication& Slate = FSlateApplication::Get();
        const double LastInput = FMath::Max(DisplaySettingsStartedAt, Slate.GetLastUserInteractionTime());
        const double IdleSeconds = FPlatformTime::Seconds() - LastInput;
        Throttle = IdleSeconds > 600.0 ? 10 : IdleSeconds > 60.0 ? 30 : 0;
    }

    const uint16 Limit = Throttle != 0 && (RequestedFpsLimit == 0 || Throttle < RequestedFpsLimit)
        ? uint16(Throttle) : RequestedFpsLimit;
    if (GEngine->GetMaxFPS() != Limit)
    {
        GEngine->SetMaxFPS(Limit);
    }
    if (FpsLimit != Limit)
    {
        FpsLimit = Limit;
        bInputDirty = true;
        UE_LOG(LogMinecraftBridge, Display, TEXT("UE effective frame limit: %u (0=unlimited)."), uint32(FpsLimit));
    }
}

bool ABridgeViewer::ApplyWorld(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != 12) return false;
    const uint32 Serial = Reader.U32();
    const int32 NewMinY = Reader.I32();
    const int32 NewHeight = Reader.I32();
    if (!InRange(NewMinY, 32000) || NewMinY % 16 != 0 || NewHeight < 0 || NewHeight > MaxHeight || NewHeight % 16 != 0)
    {
        return false;
    }
    SceneSerial = Serial;
    ReadyScene = 0;
    DebugStats = {};
    DebugPanelBottom = 0;
    NextDebugSample = 0;
    InputLook = FVector2f::ZeroVector;
    InputEvents.Reset();
    FileDropBytes.Reset();
    FileDropOffset = 0;
    bInputDirty = true;
    bLocalLookSynced = false;
    bLocalLookDirty = false;
    LastAckOverrideEpoch = 0;
    LastTeleportEpoch = 0;
    ResetShmPositionState();
    bHadLocalCamera = bHasAuthoritativeCamera = false;
    PublishLocalLook(0, false);
    MinY = NewMinY;
    Height = NewHeight;
    ClearTerrain();
    if (PendingEnvironment && PendingEnvironment->Scene <= SceneSerial)
    {
        if (PendingEnvironment->Scene == SceneSerial && Height != 0)
        {
            PresentEnvironment(*PendingEnvironment);
        }
        PendingEnvironment.Reset();
    }
    ResetCameraTrace();
    CameraFrames = SectionFrames = DropFrames = SectionRebuilds = 0;
    CameraPositionMC = CameraForwardMC = FVector::ZeroVector;
    UE_LOG(LogMinecraftBridge, Display, TEXT("World %u: minY=%d height=%d."), SceneSerial, MinY, Height);
    return true;
}

bool ABridgeViewer::ApplyControlShm(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != ControlShmBytes)
    {
        return false;
    }
    const FGuid ControlId{Reader.U32(), Reader.U32(), Reader.U32(), Reader.U32()};
    const FGuid TerrainId{Reader.U32(), Reader.U32(), Reader.U32(), Reader.U32()};
    if (!ControlShm.Map(ControlId, SessionNonce) || !TerrainShm.Map(TerrainId, SessionNonce))
    {
        UE_LOG(LogMinecraftBridge, Error, TEXT("Could not map Minecraft control and terrain shared memory (protocol %u)."),
            BridgeWire::Version);
        return false;
    }
    ControlShm.AdvertiseCapabilities(BridgeShm::CapWaterBubbles | BridgeShm::CapCoveredFaceLights |
        BridgeShm::CapVegetationShading | BridgeShm::CapMaterialLights | BridgeShm::CapTextureEmission |
        BridgeShm::CapGlassLighting | BridgeShm::CapMineralGlow |
        (ControlShm.HasSelectionOutline() ? BridgeShm::CapSelectionOutline : 0));
    EncodeSessionReply();
    bReady = true;
    UE_LOG(LogMinecraftBridge, Display, TEXT("Minecraft session ready: protocol=%u, control/camera and terrain mappings attached."), Version);
    return true;
}

void ABridgeViewer::ResetShmPositionState()
{
    LastShmTickId = 0;
    LastShmTickTimeNs = 0;
    LastShmTickArrivalAt = 0.0;
    EstimatedTickIntervalSec = 0.05;
    FrozenShmAlpha = 1.0;
    bHasShmTickPose = false;
    bSnapShmTickPose = false;
    bWasShmPaused = false;
    ShmPrevEyeMC = FVector::ZeroVector;
    ShmCurrEyeMC = FVector::ZeroVector;
}

void ABridgeViewer::PollControlShm()
{
    if (!ControlShm.IsMapped())
    {
        return;
    }
    BridgeShm::FMcToUeState NextState{};
    if (!ControlShm.PollState(NextState))
    {
        return;
    }
    const double Now = FPlatformTime::Seconds();
    McState = NextState;
    bHasMcState = true;
    if (McState.WorldSerial != SceneSerial || Height == 0)
    {
        bLocalLookSynced = false;
        bLocalLookDirty = false;
        ResetShmPositionState();
        return;
    }

    const bool bFiniteTickPose =
        (McState.Flags & BridgeShm::FlagInWorld) != 0 &&
        FMath::IsFinite(McState.PrevX) && FMath::IsFinite(McState.PrevY) && FMath::IsFinite(McState.PrevZ) &&
        FMath::IsFinite(McState.CurrX) && FMath::IsFinite(McState.CurrY) && FMath::IsFinite(McState.CurrZ) &&
        FMath::IsFinite(McState.PrevEyeHeight) && FMath::IsFinite(McState.CurrEyeHeight);
    const FVector NextPrevEye = bFiniteTickPose
        ? FVector(McState.PrevX, McState.PrevY + double(McState.PrevEyeHeight), McState.PrevZ)
        : FVector::ZeroVector;
    const FVector NextCurrEye = bFiniteTickPose
        ? FVector(McState.CurrX, McState.CurrY + double(McState.CurrEyeHeight), McState.CurrZ)
        : FVector::ZeroVector;

    const bool bTeleported = McState.TeleportEpoch != LastTeleportEpoch;
    if (bTeleported)
    {
        LastTeleportEpoch = McState.TeleportEpoch;
        if (WaterOptics) WaterOptics->ResetCamera();
        bSnapShmTickPose = true;
        FrozenShmAlpha = 1.0;
        if (bFiniteTickPose)
        {
            ShmPrevEyeMC = NextCurrEye;
            ShmCurrEyeMC = NextCurrEye;
            bHasShmTickPose = true;
            LastShmTickId = McState.TickId;
            LastShmTickTimeNs = McState.TickTimeNs;
            LastShmTickArrivalAt = Now;
        }
        else
        {
            ResetShmPositionState();
        }
    }
    else if (bFiniteTickPose && McState.TickId > 0 && McState.TickId != LastShmTickId)
    {
        const bool bLargeJump = FVector::DistSquared(NextPrevEye, NextCurrEye) > 100.0;
        if (!bHasShmTickPose || McState.TickId < LastShmTickId || bLargeJump)
        {
            ShmPrevEyeMC = bLargeJump ? NextCurrEye : NextPrevEye;
            ShmCurrEyeMC = NextCurrEye;
            bSnapShmTickPose = bLargeJump;
            EstimatedTickIntervalSec = 0.05;
            FrozenShmAlpha = bLargeJump ? 1.0 : 0.0;
            bHasShmTickPose = true;
        }
        else
        {
            if (McState.TickId == LastShmTickId + 1 && LastShmTickTimeNs != 0 && McState.TickTimeNs > LastShmTickTimeNs)
            {
                const double MeasuredInterval = FMath::Clamp(double(McState.TickTimeNs - LastShmTickTimeNs) * 1e-9, 0.035, 0.075);
                EstimatedTickIntervalSec = FMath::Lerp(EstimatedTickIntervalSec, MeasuredInterval, 0.15);
            }
            ShmPrevEyeMC = NextPrevEye;
            ShmCurrEyeMC = NextCurrEye;
            bSnapShmTickPose = false;
            FrozenShmAlpha = 0.0;
        }
        LastShmTickId = McState.TickId;
        LastShmTickTimeNs = McState.TickTimeNs;
        LastShmTickArrivalAt = Now;
    }

    const bool bPaused = (McState.Flags & BridgeShm::FlagIsPaused) != 0;
    if (bHasShmTickPose && !bSnapShmTickPose)
    {
        const double Interval = FMath::Clamp(EstimatedTickIntervalSec, 0.035, 0.075);
        if (bPaused && !bWasShmPaused)
        {
            FrozenShmAlpha = FMath::Clamp((Now - LastShmTickArrivalAt) / Interval, 0.0, 1.0);
        }
        else if (!bPaused && bWasShmPaused)
        {
            LastShmTickArrivalAt = Now - FrozenShmAlpha * Interval;
        }
    }
    bWasShmPaused = bPaused;

    const bool bGameplayLook = (McState.Flags & BridgeShm::FlagGameplayLook) != 0;
    if (!bLocalLookSynced || McState.OverrideEpoch > LastAckOverrideEpoch)
    {
        LocalYawDeg = FMath::IsFinite(McState.ForcedYaw) ? McState.ForcedYaw : 0.0f;
        LocalPitchDeg = FMath::IsFinite(McState.ForcedPitch) ? FMath::Clamp(McState.ForcedPitch, -90.0f, 90.0f) : 0.0f;
        LastAckOverrideEpoch = McState.OverrideEpoch;
        bLocalLookSynced = true;
        bLocalLookDirty = true;
        if (InputCapture != 0 && bGameplayLook)
        {
            PublishLocalLook(0, true);
        }
    }
    else if (!bGameplayLook)
    {
        if (FMath::IsFinite(McState.ForcedYaw))
        {
            LocalYawDeg = McState.ForcedYaw;
        }
        if (FMath::IsFinite(McState.ForcedPitch))
        {
            LocalPitchDeg = FMath::Clamp(McState.ForcedPitch, -90.0f, 90.0f);
        }
    }
}

bool ABridgeViewer::IsZeroLatencyLookReady() const
{
    return ControlShm.IsMapped() && bHasMcState && McState.WorldSerial == SceneSerial && Height != 0 &&
        InputCapture != 0 && bLocalLookSynced && (McState.Flags & BridgeShm::FlagGameplayLook) != 0 &&
        McState.MouseFactor > 0.0 && FMath::IsFinite(McState.MouseFactor);
}

void ABridgeViewer::PublishLocalLook(uint32 LookSerial, bool bActive)
{
    if (!ControlShm.IsMapped())
    {
        return;
    }
    BridgeShm::FUeToMcLook Look{};
    Look.AckOverrideEpoch = LastAckOverrideEpoch;
    Look.TimestampNs = uint64(FPlatformTime::Seconds() * 1e9);
    Look.WorldSerial = SceneSerial;
    Look.CaptureId = InputCapture;
    Look.LookSerial = LookSerial;
    Look.Yaw = LocalYawDeg;
    Look.Pitch = LocalPitchDeg;
    Look.Flags = bActive ? BridgeShm::LookFlagActive : 0u;
    ControlShm.WriteLook(Look);
}

void ABridgeViewer::PresentCamera(float DeltaSeconds)
{
    using namespace BridgeWire;
    PollControlShm();
    bool bZeroLatency = Camera && IsZeroLatencyLookReady();
    bool bNewPose = false;
    BridgeShm::FCameraSample NextPose{};
    uint64 Sequence = LastCameraSequence;
    if (ControlShm.PollCamera(NextPose, Sequence))
    {
        const uint32 Serial = ReadBigEndian(NextPose.Bytes);
        if (Serial == SceneSerial && Height != 0)
        {
            if (!ApplyCamera(NextPose, !bZeroLatency))
            {
                UE_LOG(LogMinecraftBridge, Warning, TEXT("Rejected invalid shared camera pose."));
                Disconnect();
                return;
            }
            if (bTrace && bHasAuthoritativeCamera)
            {
                CameraCoalescedPoses += uint32((Sequence - LastCameraSequence) / 2 - 1);
            }
            LastAuthoritativeCamera = NextPose;
            bHasAuthoritativeCamera = true;
            bNewPose = !bZeroLatency || LocalPlayerView == ELocalPlayerView::World;
        }
        // A pose may arrive before its reliable World message. Leave it available for the next frame.
        if (int32(Serial - SceneSerial) <= 0)
        {
            LastCameraSequence = Sequence;
        }
    }
    bZeroLatency = bZeroLatency && LocalPlayerView != ELocalPlayerView::World;
    if (!bZeroLatency && bHadLocalCamera && !bNewPose && bHasAuthoritativeCamera)
    {
        ApplyCamera(LastAuthoritativeCamera, true, false);
        bNewPose = true;
    }
    bHadLocalCamera = bZeroLatency;
    if (bZeroLatency)
    {
        const double Now = FPlatformTime::Seconds();
        const bool bShmEye = (McState.Flags & BridgeShm::FlagHasEyePose) != 0 &&
            FMath::IsFinite(McState.EyeX) && FMath::IsFinite(McState.EyeY) && FMath::IsFinite(McState.EyeZ);
        const FVector PreviousPositionMC = CameraPositionMC;
        if (bHasShmTickPose)
        {
            const bool bPaused = (McState.Flags & BridgeShm::FlagIsPaused) != 0;
            double Alpha = 1.0;
            if (!bSnapShmTickPose)
            {
                const double Interval = FMath::Clamp(EstimatedTickIntervalSec, 0.035, 0.075);
                Alpha = bPaused ? FrozenShmAlpha : FMath::Clamp((Now - LastShmTickArrivalAt) / Interval, 0.0, 1.0);
                if (!bPaused)
                {
                    FrozenShmAlpha = Alpha;
                }
            }
            CameraPositionMC = FMath::Lerp(ShmPrevEyeMC, ShmCurrEyeMC, Alpha);
        }
        else if (bShmEye)
        {
            CameraPositionMC = FVector(McState.EyeX, McState.EyeY, McState.EyeZ);
        }
        if (CameraFrames > 0 || bHasShmTickPose || bShmEye)
        {
            FVector ForwardMC, ForwardUE, UpUE;
            BridgeShm::ComputeCameraVectors(LocalYawDeg, LocalPitchDeg, ForwardMC, ForwardUE, UpUE);
            CameraForwardMC = ForwardMC;
            Camera->SetActorLocationAndRotation(
                BridgeWire::ToUnreal(CameraPositionMC.X, CameraPositionMC.Y, CameraPositionMC.Z) * BridgeWire::BlockSize,
                FRotationMatrix::MakeFromXZ(ForwardUE, UpUE).Rotator());
            if (McState.VerticalFov > 0.1f && McState.VerticalFov < 179.0f &&
                McState.AspectRatio >= 0.1f && McState.AspectRatio <= 10.0f)
            {
                const double HalfHorizontalFov = FMath::Atan(
                    FMath::Tan(FMath::DegreesToRadians(McState.VerticalFov * 0.5)) * McState.AspectRatio);
                Camera->GetCameraComponent()->SetFieldOfView(FMath::RadiansToDegrees(2.0 * HalfHorizontalFov));
                Camera->GetCameraComponent()->SetAspectRatio(McState.AspectRatio);
            }
            if (CameraFrames == 0 && (bHasShmTickPose || bShmEye))
            {
                ++CameraFrames;
            }
            if (bLocalLookDirty || !CameraPositionMC.Equals(PreviousPositionMC, 1e-6))
            {
                bNewPose = true;
                bLocalLookDirty = false;
            }
        }
    }
    if (LocalPlayerView != ELocalPlayerView::None)
    {
        // The camera and body use the same translation, including the locally interpolated first-person eye.
        const FVector Feet = bZeroLatency ? CameraPositionMC + PlayerEyeOffsetMC : PlayerFeetMC;
        LocalPlayerRoot->SetRelativeLocation(ToUnreal(Feet.X, Feet.Y, Feet.Z) * BlockSize);
    }
    if (bTrace && CameraFrames > 0)
    {
        ++CameraRenderFrames;
        CameraReuseFrames += !bNewPose;
        const double Now = FPlatformTime::Seconds();
        if (LastLookInputAt > 0 && Now - LastLookInputAt <= 0.1)
        {
            ++CameraDuringLookFrames;
            CameraDuringLookReuseFrames += !bNewPose;
        }
    }
    TickBlockEffects();
}

bool ABridgeViewer::ApplyCamera(const BridgeShm::FCameraSample& Sample, bool bApplyTransform, bool bNewSample)
{
    using namespace BridgeWire;
    FReader Reader{Sample.Bytes};
    if (Reader.U32() != SceneSerial || Height == 0)
    {
        return true;
    }
    const double X = Reader.Double();
    const double Y = Reader.Double();
    const double Z = Reader.Double();
    const float ForwardX = Reader.Float();
    const float ForwardY = Reader.Float();
    const float ForwardZ = Reader.Float();
    const float UpX = Reader.Float();
    const float UpY = Reader.Float();
    const float UpZ = Reader.Float();
    const float VerticalFov = Reader.Float();
    const float Aspect = Reader.Float();
    const uint32 LookSerial = Reader.U32();
    const uint8 PlayerView = Reader.Byte();
    const double FeetX = Reader.Double();
    const double FeetY = Reader.Double();
    const double FeetZ = Reader.Double();
    const uint32 LookCapture = Reader.U32();
    const uint8 MovementFlags = Reader.Byte();
    if (!FMath::IsFinite(X) || !FMath::IsFinite(Y) || !FMath::IsFinite(Z) ||
        !FMath::IsFinite(ForwardX) || !FMath::IsFinite(ForwardY) || !FMath::IsFinite(ForwardZ) ||
        !FMath::IsFinite(UpX) || !FMath::IsFinite(UpY) || !FMath::IsFinite(UpZ) ||
        !(VerticalFov > 0.1f && VerticalFov < 179.0f) || !(Aspect >= 0.1f && Aspect <= 10.0f) ||
        PlayerView > uint8(ELocalPlayerView::World) ||
        !(FMath::Abs(FeetX) <= MaxBlockCoordinate && FMath::Abs(FeetY) <= MaxBlockCoordinate &&
            FMath::Abs(FeetZ) <= MaxBlockCoordinate))
    {
        return false;
    }
    const FVector Forward = ToUnreal(ForwardX, ForwardY, ForwardZ);
    const FVector Up = ToUnreal(UpX, UpY, UpZ);
    if (!FMath::IsNearlyEqual(Forward.SizeSquared(), 1.0, 0.01) ||
        !FMath::IsNearlyEqual(Up.SizeSquared(), 1.0, 0.01) ||
        !FMath::IsNearlyZero(FVector::DotProduct(Forward, Up), 0.01))
    {
        return false;
    }
    LocalPlayerView = static_cast<ELocalPlayerView>(PlayerView);
    constexpr uint8 Grounded = BridgeShm::CameraMovementKnown | BridgeShm::CameraGroundedOnFoot;
    bPlayerGroundedForFootsteps = (MovementFlags & Grounded) == Grounded;
    PlayerFeetMC = FVector(FeetX, FeetY, FeetZ);
    PlayerEyeOffsetMC = PlayerFeetMC - FVector(X, Y, Z);
    if (bApplyTransform || LocalPlayerView == ELocalPlayerView::World)
    {
        CameraPositionMC = FVector(X, Y, Z);
        CameraForwardMC = FVector(ForwardX, ForwardY, ForwardZ);
        Camera->SetActorLocationAndRotation(ToUnreal(X, Y, Z) * BlockSize, FRotationMatrix::MakeFromXZ(Forward, Up).Rotator());
        const double HalfHorizontalFov = FMath::Atan(FMath::Tan(FMath::DegreesToRadians(VerticalFov * 0.5)) * Aspect);
        Camera->GetCameraComponent()->SetFieldOfView(FMath::RadiansToDegrees(2.0 * HalfHorizontalFov));
        Camera->GetCameraComponent()->SetAspectRatio(Aspect);
    }
    else if (CameraFrames == 0 && !bHasShmTickPose)
    {
        CameraPositionMC = FVector(X, Y, Z);
    }
    CameraFrames += bNewSample;
    if (bTrace && bNewSample)
    {
        const double Now = FPlatformTime::Seconds();
        if (LastCameraAppliedAt > 0)
        {
            CameraIntervals.Add((Now - LastCameraAppliedAt) * 1000.0);
        }
        LastCameraAppliedAt = Now;
        if (LookSerial != 0 && LookCapture == InputCapture && LookSerial != LastCameraLookSerial)
        {
            const FLookStamp& Stamp = LookStamps[LookSerial % UE_ARRAY_COUNT(LookStamps)];
            if (Stamp.Serial == LookSerial)
            {
                LookRoundTrips.Add((Now - Stamp.At) * 1000.0);
            }
            LastCameraLookSerial = LookSerial;
        }
    }
    return true;
}

void ABridgeViewer::Disconnect()
{
    if (UBridgeViewportClient* Viewport = InputViewport.Get())
    {
        Viewport->CancelPhoto(TEXT("Minecraft disconnected"));
        Viewport->ReleaseControl(TEXT("connection closed"));
        Viewport->ResetMinecraftPresentation();
    }
    UE_CLOG(Client, LogMinecraftBridge, Display, TEXT("Minecraft bridge disconnected."));
    CloseSocket(Client);
    bReady = bSessionResetReceived = bSessionReplyPending = false;
    SessionNonce = LastCameraSequence = LastTerrainToken = 0;
    ConnectionStartedAt = 0;
    SessionReplyOffset = 0;
    TerrainShm.Unmap();
    bHadLocalCamera = bHasAuthoritativeCamera = false;
    bWindowReceived = bSingleWindow = false;
    bCloseSent = false;
    MinY = Height = 0;
    ReportedViewport = SceneSerial = ReadyScene = 0;
    PresentingWindow = 0;
    ReleasedUiSlots = 0;
    PendingUiSlot = INDEX_NONE;
    ResetGpuUi();
    PublishLocalLook(0, false);
    ControlShm.Unmap();
    McState = {};
    bHasMcState = bLocalLookSynced = bLocalLookDirty = false;
    LastAckOverrideEpoch = 0;
    LastTeleportEpoch = 0;
    ResetShmPositionState();
    LocalYawDeg = LocalPitchDeg = 0.0f;
    if (UiMemory)
    {
        FPlatformMemory::UnmapNamedSharedMemoryRegion(UiMemory);
        UiMemory = nullptr;
    }
    UiMemoryId.Invalidate();
    LastTraceUiFrames = 0;
    LastTraceAt = UiReadbackMs = UiUploadMs = 0;
    InputMode = BridgeWire::EInputMode::Unavailable;
    InputCapture = 0;
    InputLook = FVector2f::ZeroVector;
    InputEvents.Reset();
    bInputDirty = bInputPending = false;
    InputOffset = 0;
    NextLookSerial = EncodedLookSerial = 0;
    NextInputHeartbeat = 0;
    DebugStats = {};
    DebugPanelBottom = 0;
    NextDebugSample = 0;
    ReceiveBuffer.Reset();
    DeferredPhotoFrames.Empty();
    ReadOffset = 0;
    if (PreviousDisplaySettings)
    {
        GEngine->SetMaxFPS(PreviousDisplaySettings->FpsLimit);
        IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"))->SetWithCurrentPriority(PreviousDisplaySettings->VSync);
        PreviousDisplaySettings.Reset();
    }
    ViewDistance = 0;
    RequestedFpsLimit = FpsLimit = 0;
    bVSync = bThrottleWhenAfk = false;
    TerrainFiltering = BridgeWire::ETerrainFiltering::None;
    RequestedAnisotropy = 1;
    PendingEnvironment.Reset();
    ClearTerrain();
    TerrainReleases.Reset();
    EncodedTerrainReleaseCount = 0;
    ResetCameraTrace();
    UiTexture = nullptr;
    SectionFrames = DropFrames = CameraFrames = UiFrames = SectionRebuilds = 0;
    CameraPositionMC = CameraForwardMC = FVector::ZeroVector;
}

void ABridgeViewer::ClearTerrain()
{
    ResetAtlasOffer();
    ResetBlockEffects();
    GlassLighting.Reset();
    if (WaterOptics) WaterOptics->Reset();
    if (WaterParticles) WaterParticles->Reset();
    ResetEnvironment();
    ResetDynamicMeshes();
    for (const auto& Pair : TerrainRegions)
    {
        Pair.Value->DestroyComponent();
    }
    TerrainRegions.Reset();
    LoadedSections = 0;
    TerrainColumns.Reset();
    ClearPendingSections();
    PendingRemoval.Reset();
    DecodingSections.Reset();
    for (FBridgeTerrainAtlasUpload& Atlas : TerrainAtlases)
    {
        Atlas = {};
    }
    AtlasPatchFrames = BlockAtlasPatchRegions = DynamicAtlasPatchRegions = BlockAtlasPatchMips = 0;
    AtlasPatchPixelBytes = 0;
    bSceneMeshesReceived = false;
    TerrainMaterials.Reset();
}

void ABridgeViewer::SampleDebugStats(double Now)
{
    if (Height == 0 || Now < NextDebugSample)
    {
        return;
    }
    NextDebugSample = Now + 0.25;
    DebugStats.FrameMs = FMath::IsFinite(GAverageMS) && GAverageMS > 0 ? GAverageMS : 0;
    DebugStats.LoadedSections = LoadedSections;
    DebugStats.LoadedRegions = TerrainRegions.Num();
    DebugStats.PendingSections = GetPendingSectionCount();
    DebugStats.ReceivedColumns = uint16(TerrainColumns.Num());
    McTerrain::TraceRayTracingRegions(Now, SceneSerial, TerrainRegions.Num(), LoadedSections,
        DebugStats.PendingSections, SectionRebuilds, DropFrames);
    bInputDirty = true;
}

void ABridgeViewer::SetDebugPanelBottom(uint16 Bottom)
{
    if (DebugPanelBottom != Bottom)
    {
        DebugPanelBottom = Bottom;
        bInputDirty = true;
    }
}

void ABridgeViewer::ResetCameraTrace()
{
    CameraIntervals.Reset();
    LookRoundTrips.Reset();
    CameraRenderFrames = CameraReuseFrames = CameraCoalescedPoses = 0;
    CameraDuringLookFrames = CameraDuringLookReuseFrames = 0;
    LastCameraAppliedAt = LastLookInputAt = PendingLookInputAt = EncodedLookInputAt = 0;
    LastCameraLookSerial = 0;
    for (FLookStamp& Stamp : LookStamps)
    {
        Stamp = {};
    }
}

void ABridgeViewer::Trace()
{
    const double Now = FPlatformTime::Seconds();
    if (!bTrace || !bReady || Now < NextTraceAt)
    {
        return;
    }
    NextTraceAt = Now + 1.0;
    if (Height > 0)
    {
        if (AtlasPatchFrames > 0)
        {
            UE_LOG(LogMinecraftBridge, Display,
                TEXT("AtlasPatch scene=%u packets=%u blockRegions=%u dynamicRegions=%u blockMips=0x%x pixelBytes=%llu"),
                SceneSerial, AtlasPatchFrames, BlockAtlasPatchRegions, DynamicAtlasPatchRegions, BlockAtlasPatchMips,
                AtlasPatchPixelBytes);
        }
        const auto& World = DynamicSpaces[BridgeWire::DynamicWorld];
        const auto& Hand = DynamicSpaces[BridgeWire::DynamicHand];
        const auto& Player = DynamicSpaces[BridgeWire::DynamicLocalPlayer];
        UE_LOG(LogMinecraftBridge, Display,
            TEXT("DynamicMeshes scene=%u worldBatches=%u worldInstances=%d worldMeshes=%u handBatches=%u handInstances=%d handMeshes=%u playerBatches=%u playerInstances=%d playerMeshes=%u playerView=%u textures=%d draws=%d"),
            SceneSerial, World.BatchFrames, World.LastInstances, World.MeshFrames,
            Hand.BatchFrames, Hand.LastInstances, Hand.MeshFrames, Player.BatchFrames, Player.LastInstances, Player.MeshFrames,
            uint8(LocalPlayerView), DynamicTextures.Num(), DynamicComponents.Num());
    }
    TraceGpuUi(Now);
    const double UiFps = LastTraceAt == 0 ? 0 : (UiFrames - LastTraceUiFrames) / (Now - LastTraceAt);
    LastTraceUiFrames = UiFrames;
    LastTraceAt = Now;
    UE_LOG(LogMinecraftBridge, Display,
        TEXT("Live chunks=%d sections=%d drops=%d cameras=%d ui=%d uiFps=%.1f uiSize=%dx%d readbackMs=%.2f uploadPrepMs=%.2f meshes=%d regions=%d uploads=%d pending=%d pos=(%.3f,%.3f,%.3f) forward=(%.3f,%.3f,%.3f) ueFps=%.1f ueFrameMs=%.2f"),
        TerrainColumns.Num(), SectionFrames, DropFrames, CameraFrames, UiFrames, UiFps,
        UiTexture ? UiTexture->GetSizeX() : 0, UiTexture ? UiTexture->GetSizeY() : 0, UiReadbackMs, UiUploadMs,
        LoadedSections, TerrainRegions.Num(), SectionRebuilds, GetPendingSectionCount(), CameraPositionMC.X, CameraPositionMC.Y, CameraPositionMC.Z,
        CameraForwardMC.X, CameraForwardMC.Y, CameraForwardMC.Z,
        DebugStats.FrameMs > 0 ? 1000.0 / DebugStats.FrameMs : 0, DebugStats.FrameMs);
    TraceEnvironment();
    if (CameraRenderFrames > 0)
    {
        CameraIntervals.Sort();
        LookRoundTrips.Sort();
        UE_LOG(LogMinecraftBridge, Display,
            TEXT("CameraTiming scene=%u frames=%u reused=%u lookFrames=%u lookReused=%u coalesced=%u poseSamples=%d poseP95Ms=%.2f poseMaxMs=%.2f lookSamples=%d lookP50Ms=%.2f lookP95Ms=%.2f lookMaxMs=%.2f lastLookSerial=%u"),
            SceneSerial, CameraRenderFrames, CameraReuseFrames, CameraDuringLookFrames, CameraDuringLookReuseFrames,
            CameraCoalescedPoses, CameraIntervals.Num(), Percentile(CameraIntervals, 0.95),
            CameraIntervals.IsEmpty() ? 0.0 : CameraIntervals.Last(), LookRoundTrips.Num(),
            Percentile(LookRoundTrips, 0.50), Percentile(LookRoundTrips, 0.95),
            LookRoundTrips.IsEmpty() ? 0.0 : LookRoundTrips.Last(), LastCameraLookSerial);
        CameraIntervals.Reset();
        LookRoundTrips.Reset();
        CameraRenderFrames = CameraReuseFrames = CameraCoalescedPoses = 0;
        CameraDuringLookFrames = CameraDuringLookReuseFrames = 0;
    }
}
