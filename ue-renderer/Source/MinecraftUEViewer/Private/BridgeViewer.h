#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMemory.h"
#include "BridgeAtlasCache.h"
#include "BridgeDynamic.h"
#include "BridgeGlassLighting.h"
#include "BridgeShmProtocol.h"
#include "BridgeSurfaceWetness.h"
#include "BridgeWaterSim.h"
#include "BridgeWaterOptics.h"
#include "BridgeWaterParticles.h"
#include "BridgeWire.h"
#include "McTerrainRegionComponent.h"
#include "BridgeViewer.generated.h"

class ACameraActor;
class FBridgeGlassCapture;
class UBridgeViewportClient;
class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class ULocalLightComponent;
class UPointLightComponent;
class URectLightComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UStaticMeshComponent;
class UVolumetricCloudComponent;
class FSocket;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class UCurveFloat;
class UMinecraftWeatherSubsystem;
class UTexture2D;
class UTextureLightProfile;
class UCanvas;

USTRUCT()
struct FBridgeTerrainAtlasUpload
{
    GENERATED_BODY()

    UPROPERTY()
    TObjectPtr<UTexture2D> Texture;

    int32 MipCount = 0;
    int32 NextMip = 0;
    int32 NextRow = 0;

    bool IsComplete() const { return Texture && NextMip == MipCount; }
};

UCLASS()
class ABridgeViewer : public AActor
{
    GENERATED_BODY()

public:
    ABridgeViewer();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

    bool StartControl();
    void CloseMinecraft();
    void StopControl(const TCHAR* Reason);
    void AddControlLook(float DX, float DY);
    bool AddControlEvent(const BridgeWire::FInputEvent& Event);
    bool AddDroppedFiles(const TArray<FString>& Files);
    bool IsSingleWindow() const { return bSingleWindow; }
    bool IsGameplayInput() const { return InputMode == BridgeWire::EInputMode::Gameplay; }
    bool IsScreenInput() const { return InputMode == BridgeWire::EInputMode::Screen || IsTextInput(); }
    bool IsTextInput() const { return InputMode == BridgeWire::EInputMode::Text; }
    const UTexture2D* GetUiTexture() const { return UiTexture.Get(); }
    bool HasGpuUi() const;
    void DrawGpuUi(UCanvas* Canvas);
    bool IsUiOpaque() const { return bUiOpaque; }

    struct FDebugStats
    {
        float FrameMs = 0;
        uint32 LoadedSections = 0;
        uint32 LoadedRegions = 0;
        uint32 PendingSections = 0;
        uint16 ReceivedColumns = 0;
    };
    const FDebugStats& GetDebugStats() const { return DebugStats; }
    uint8 GetViewDistance() const { return ViewDistance; }
    bool IsDrawingTerrain() const { return Height > 0 && CameraFrames > 0 && !bUiOpaque; }
    bool IsPhotoReady() const { return IsDrawingTerrain() && LoadedSections > 0; }
    void SetDebugPanelBottom(uint16 Bottom);
    const TOptional<BridgeWire::FEnvironmentState>& GetEnvironmentState() const { return Environment; }
    const TMap<FIntVector, TArray<BridgeWire::FSectionLight>>& GetSectionLights() const { return SectionLights; }

private:
    friend class UMinecraftWeatherSubsystem;
#if !UE_BUILD_SHIPPING
    friend void PollBridgeWeatherProbe(const UMinecraftWeatherSubsystem& Weather, const ABridgeViewer& Viewer);
#endif

    void OpenListener();
    void PollConnection();
    void PresentCamera(float DeltaSeconds);
    void ResetCameraTrace();
    bool ReadFrames();
    bool ReplayPhotoFrames();
    bool ApplyFrame(const uint8* Data, int32 Size);
    void Disconnect();
    void ClearTerrain();

    bool ApplyReset(BridgeWire::FReader Reader, int32 Size);
    bool ApplyWorld(BridgeWire::FReader Reader, int32 Size);
    bool ApplyWindow(BridgeWire::FReader Reader, int32 Size);
    bool ApplyWindowMode(BridgeWire::FReader Reader, int32 Size);
    bool ApplyVideoSettings(BridgeWire::FReader Reader, int32 Size);
    void UpdateFrameRateLimit();
    bool ApplyAtlasRows(BridgeWire::ETerrainAtlas Kind, BridgeWire::FReader Reader, int32 Size);
    bool ApplyAtlasPatch(BridgeWire::FReader Reader, int32 Size);
    bool ApplyAtlasReference(BridgeWire::FReader Reader, int32 Size);
    bool ApplyAtlasUse(BridgeWire::FReader Reader, int32 Size);
    bool PollAtlasCache();
    void ResetAtlasOffer();
    void SetAtlasReply(BridgeWire::EAtlasCacheStatus Status);
    void BuildTerrainMaterials();
    void StartTerrainTexture(FBridgeTerrainAtlasUpload& Atlas, int32 Width, int32 HeightPx,
        int32 MipCount, bool bAlbedo);
    void FinishTerrainAtlases();
    void RefreshEnvironmentQuality();
    uint32 AppliedQualityRevision = MAX_uint32;
    void UpdateTerrainFiltering();
    bool ApplyTerrainShm(BridgeWire::FReader Reader, int32 Size);
    void ReleaseTerrainLease(uint64 Token);
    void ClearPendingSections();
    bool IsSectionInWorld(const FIntVector& Section) const;
    bool ApplySectionLights(BridgeWire::FReader Reader, int32 Size);
    bool ApplySectionGlass(BridgeWire::FReader Reader, int32 Size);
    void UpdateGlassLighting();
    bool ApplyEnvironmentState(BridgeWire::FReader Reader, int32 Size);
    bool ApplyChunkPrecipitation(BridgeWire::FReader Reader, int32 Size);
    bool ApplyLightningStrike(BridgeWire::FReader Reader, int32 Size);
    void CreateEnvironmentComponents();
    void ConfigureEnvironment();
    void CreateExposureAssets();
    void CreateBlockLightProfiles();
    float SampleRoofCoverage() const;
    float SampleWaterSkyExposure() const;
    void UpdateExposureSettings(float DeltaSeconds, float EnvBaseEV, bool bOverworld, bool bAir, bool bSnap, float WaterWeight = 0.0f);
    void PresentEnvironment(const BridgeWire::FEnvironmentState& State);
    void TickEnvironmentEffects(float DeltaSeconds);
    void ResetEnvironment();
    void UpdatePointLights();
    void RecyclePointLight(int32 Slot);
    void UpdateTerrainLightingParameters();
    void UpdatePrecipGridTexture();
    void InitializeTerrainWetnessTextures(int32 GridSize);
    void DropPrecipitationChunk(FIntPoint Chunk);
    void UpdateTerrainWetHistory(float DeltaSeconds, float SunAltitude);
    void InvalidateTerrainWetHistory();
    void UpdateTerrainPuddleFootsteps(float DeltaSeconds, bool bEnabled);
    void ResetTerrainPuddleFootsteps();
    bool TrackTerrainPuddleFootsteps();
    bool IsTerrainPuddleContact(const FVector& PositionMC) const;
    FBridgeSurfaceWetness::FMoisture SampleTerrainMoisture(const FVector& PositionMC) const;
    void InitializeWaterGrid();
    void ResetWaterGrid();
    void UpdateWaterGrid();
    void UpdateWaterInteractions(UMaterialInstanceDynamic& Material, float DeltaSeconds, int32 Tier, float StormRain);
    void ResetWaterInteractions();
    void FindWaterfallImpacts(const FVector& EyeMC, int32 Limit);
    bool ApplyChunkWaterGrid(BridgeWire::FReader Reader, int32 Size);
    bool ApplyChunkWaterBubbles(BridgeWire::FReader Reader, int32 Size);
    uint32 WaterColumn(FIntPoint Block) const;
    uint16 GetPrecipColumnCode(int32 BlockX, int32 BlockZ) const;
    bool SamplePrecipColumn(int32 BlockX, int32 BlockZ, int32& OutTopY, uint8& OutType) const;
    void InvalidatePrecipGrid();
    void TickPrecipitation(float DeltaSeconds);
    void TickLightning(float DeltaSeconds);
    void PushEnvironmentMaterialParameters(float DeltaSeconds);
    void ApplyLightFallback(UMaterialInstanceDynamic& Material, bool bUnlitParticle = false) const;
    void TraceEnvironment() const;
    bool ApplyDrop(BridgeWire::FReader Reader, int32 Size);
    bool ApplySceneReady(BridgeWire::FReader Reader, int32 Size);
    bool ApplyCamera(const BridgeShm::FCameraSample& Sample, bool bApplyTransform, bool bNewSample = true);
    bool ApplyCrackAtlas(BridgeWire::FReader Reader, int32 Size);
    bool ApplyCrackUpdate(BridgeWire::FReader Reader, int32 Size);
    void LoadDynamicMaterials();
    bool ApplyDynamicTextureRows(BridgeWire::FReader Reader, int32 Size);
    bool ApplyDynamicMesh(BridgeWire::FReader Reader, int32 Size);
    bool ApplyInstanceBatch(BridgeWire::FReader Reader, int32 Size);
    void PresentDynamicMeshes();
    void ResetDynamicMeshes();
    void UpdateDynamicLighting();
    UMaterialInstanceDynamic* GetDynamicMaterial(uint32 Key);
    bool ApplyInputAvailability(BridgeWire::FReader Reader, int32 Size);
    bool ApplyControlShm(BridgeWire::FReader Reader, int32 Size);
    void EncodeSessionReply();
    bool FlushSessionReply();
    void ResetShmPositionState();
    void PollControlShm();
    void PublishLocalLook(uint32 LookSerial, bool bActive);
    bool IsZeroLatencyLookReady() const;
    bool ApplyUi(BridgeWire::FReader Reader, int32 Size);
    void UploadUi();
    bool ApplyGpuUi(BridgeWire::FReader Reader, int32 Size);
    void PollGpuUi();
    void ResetGpuUi();
    void TraceGpuUi(double Now);

    bool FlushInput(bool bControlOnly = false);
    bool FlushFileDrop();
    void EncodeInput(double Now);
    void SampleDebugStats(double Now);

    bool UploadPendingSections();
    bool HasTerrainSection(const FIntVector& Section) const;
    int32 GetPendingSectionCount() const;
    void TickBlockEffects();
    void DrawBlockSelectionOutline();
    void ResetBlockEffects();
    void DropBlockEffects(const FIntPoint& Column);
    void RemoveCrack(const FIntVector& Block);
    void Trace();

    FSocket* Listener = nullptr;
    FSocket* Client = nullptr;
    uint64 SessionNonce = 0;
    bool bSessionResetReceived = false;
    bool bSessionReplyPending = false;
    double ConnectionStartedAt = 0;
    int32 SessionReplyOffset = 0;
    uint8 SessionReply[5 + BridgeWire::ResetPayloadBytes]{};
    uint64 LastCameraSequence = 0;
    bool bHadLocalCamera = false;
    bool bHasAuthoritativeCamera = false;
    BridgeShm::FCameraSample LastAuthoritativeCamera{};
    double LastCameraAppliedAt = 0;
    TArray<double> CameraIntervals;
    uint32 CameraRenderFrames = 0;
    uint32 CameraReuseFrames = 0;
    uint32 CameraCoalescedPoses = 0;
    uint32 NextLookSerial = 0;
    uint32 EncodedLookSerial = 0;
    uint32 LastCameraLookSerial = 0;
    uint32 CameraDuringLookFrames = 0;
    uint32 CameraDuringLookReuseFrames = 0;
    double LastLookInputAt = 0;
    double PendingLookInputAt = 0;
    double EncodedLookInputAt = 0;
    struct FLookStamp { uint32 Serial = 0; double At = 0; };
    FLookStamp LookStamps[256]{};
    TArray<double> LookRoundTrips;
    BridgeShm::FControlShmClient ControlShm;
    BridgeShm::FMcToUeState McState{};
    BridgeShm::FSelectionOutline SelectionOutline{};
    uint64 SelectionOutlineSequence = 0;
    bool bHasMcState = false;
    bool bLocalLookSynced = false;
    bool bLocalLookDirty = false;
    uint64 LastAckOverrideEpoch = 0;
    uint32 LastTeleportEpoch = 0;
    uint64 LastShmTickId = 0;
    uint64 LastShmTickTimeNs = 0;
    double LastShmTickArrivalAt = 0.0;
    double EstimatedTickIntervalSec = 0.05;
    double FrozenShmAlpha = 1.0;
    bool bHasShmTickPose = false;
    bool bSnapShmTickPose = false;
    bool bWasShmPaused = false;
    FVector ShmPrevEyeMC = FVector::ZeroVector;
    FVector ShmCurrEyeMC = FVector::ZeroVector;
    float LocalYawDeg = 0.0f;
    float LocalPitchDeg = 0.0f;
    TArray<uint8> ReceiveBuffer;
    TArray<uint8> DeferredPhotoFrames;
    int32 ReadOffset = 0;
    bool bReady = false;
    bool bWindowReceived = false;
    bool bSingleWindow = false;
    bool bCloseSent = false;
    bool bPendingUiOpaque = false;
    bool bUiOpaque = false;
    uint32 SceneSerial = 0;
    uint32 ReadyScene = 0;
    bool bSceneMeshesReceived = false;
    uint32 ReportedViewport = 0;
    uint64 PresentingWindow = 0;
    uint8 ReleasedUiSlots = 0;
    int32 PendingUiSlot = INDEX_NONE;
    FIntPoint PendingUiSize = FIntPoint::ZeroValue;
    FPlatformMemory::FSharedMemoryRegion* UiMemory = nullptr;
    FGuid UiMemoryId;
    struct FUiUpload;
    TSharedPtr<FUiUpload> UiUploads[BridgeWire::UiSlots];
    struct FGpuUi;
    TSharedPtr<FGpuUi> GpuUi;
    int32 LastTraceUiFrames = 0;
    double LastTraceAt = 0;
    double UiReadbackMs = 0;
    double UiUploadMs = 0;
    BridgeWire::EInputMode InputMode = BridgeWire::EInputMode::Unavailable;
    TWeakObjectPtr<UBridgeViewportClient> InputViewport;
    uint32 InputCapture = 0;
    uint32 CaptureSerial = 0;
    FVector2f InputLook = FVector2f::ZeroVector;
    TArray<BridgeWire::FInputEvent> InputEvents;
    double NextInputHeartbeat = 0;
    bool bInputDirty = false;
    bool bInputPending = false;
    int32 InputOffset = 0;
    int32 InputSize = 0;
    uint8 InputBytes[BridgeWire::MaxInputFrameBytes]{};
    TArray<uint8> FileDropBytes;
    int32 FileDropOffset = 0;
    FDebugStats DebugStats;
    struct FDisplaySettings
    {
        float FpsLimit;
        int32 VSync;
    };
    TOptional<FDisplaySettings> PreviousDisplaySettings;
    double DisplaySettingsStartedAt = 0;
    uint16 RequestedFpsLimit = 0;
    uint16 FpsLimit = 0;
    bool bVSync = false;
    bool bThrottleWhenAfk = false;
    uint8 ViewDistance = 0;
    uint16 DebugPanelBottom = 0;
    double NextDebugSample = 0;
    int32 MinY = 0;
    int32 Height = 0;
    TOptional<BridgeWire::FEnvironmentState> Environment;
    TOptional<BridgeWire::FEnvironmentState> PendingEnvironment;
    TMap<FIntVector, TArray<BridgeWire::FSectionLight>> SectionLights;
    bool bHardwareSceneLighting = false;
    FBridgeGlassLighting GlassLighting;
    TSharedPtr<FBridgeGlassCapture, ESPMode::ThreadSafe> GlassCapture;
    float FallbackBlockFill = 0;
    FLinearColor FallbackSkyFill = FLinearColor::Black;
    FLinearColor AmbientFloorColor = FLinearColor::Black;
    float EmissiveBaseLuminance = 48.0f;
    float EmissiveDayBoostLuminance = 0.0f;
    BridgeDynamic::FSpace DynamicSpaces[BridgeWire::DynamicSpaceCount];
    BridgeDynamic::FTextureUpload DynamicTextureUpload;
    int32 DynamicTexturePixels = 0;
    float HandVerticalFov = 70.0f;
    BridgeWire::ELocalPlayerView LocalPlayerView = BridgeWire::ELocalPlayerView::None;
    FVector PlayerFeetMC = FVector::ZeroVector;
    FVector PlayerEyeOffsetMC = FVector::ZeroVector;
    bool bPlayerGroundedForFootsteps = false;
    UPROPERTY()
    TObjectPtr<USceneComponent> LocalPlayerRoot;
    UPROPERTY()
    TArray<TObjectPtr<UMaterialInterface>> DynamicMaterialBases;

    UPROPERTY()
    TArray<TObjectPtr<UMaterialInterface>> ParticleMaterialBases;
    UPROPERTY()
    TMap<uint32, TObjectPtr<UTexture2D>> DynamicTextures;

    UPROPERTY()
    TMap<uint32, TObjectPtr<UProceduralMeshComponent>> RigidDynamicComponents;
    UPROPERTY()
    TMap<uint32, TObjectPtr<UMaterialInstanceDynamic>> DynamicMaterials;
    UPROPERTY()
    TMap<uint32, TObjectPtr<UProceduralMeshComponent>> DynamicComponents;
    TSet<FIntPoint> TerrainColumns;
    struct FPendingSection
    {
        uint64 Token;
        uint32 Offset;
        int32 MeshBytes;
        int32 LightBytes;
        bool bUrgent;
        double ReceivedAt;
    };

    BridgeShm::FTerrainShmClient TerrainShm;
    uint64 LastTerrainToken = 0;
    TArray<uint64> TerrainReleases;
    int32 EncodedTerrainReleaseCount = 0;
    TMap<FIntVector, FPendingSection> PendingSections;
    TSet<FIntVector> PendingRemoval;
    struct FSectionDecodeJob
    {
        FIntVector Section;
        uint64 Revision;
        uint64 Token;
        int32 PayloadBytes;
        bool bUrgent;
        double ReceivedAt;
        TFuture<McTerrain::FSectionData> Result;
    };
    TArray<FSectionDecodeJob> DecodeJobs;
    TMap<FIntVector, uint64> DecodingSections;
    uint64 NextDecodeRevision = 0;
    int32 ConsecutiveUrgentCommits = 0;
    int32 LoadedSections = 0;
    BridgeWire::ETerrainFiltering TerrainFiltering = BridgeWire::ETerrainFiltering::None;
    uint8 RequestedAnisotropy = 1;
    bool bTerrainNoAF = false;
    bool bTerrainMip0 = false;

    UPROPERTY()
    TMap<FIntPoint, TObjectPtr<UMcTerrainRegionComponent>> TerrainRegions;

    UPROPERTY()
    TObjectPtr<ACameraActor> Camera;

    UPROPERTY()
    TObjectPtr<UDirectionalLightComponent> TerrainSun;

    UPROPERTY()
    TObjectPtr<UDirectionalLightComponent> TerrainMoon;

    UPROPERTY()
    TObjectPtr<USkyAtmosphereComponent> TerrainAtmosphere;

    UPROPERTY()
    TObjectPtr<USkyLightComponent> TerrainSkyLight;

    UPROPERTY()
    TObjectPtr<UVolumetricCloudComponent> TerrainClouds;

    UPROPERTY()
    TObjectPtr<UExponentialHeightFogComponent> TerrainFog;

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> TerrainSkyDome;

    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> CloudMaterial;

#if !UE_BUILD_SHIPPING
    bool bNativeCloudPreviewRequested = false;
#endif

    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> SkyDomeMaterial;

    UPROPERTY()
    TObjectPtr<UTexture2D> PrecipGridTexture;

    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> TerrainWetHistoryTexture;

    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> TerrainPuddleFootstepTexture;

    UPROPERTY(Transient)
    TObjectPtr<UTextureLightProfile> FlushFaceIesProfile;

    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> WaterGridTexture;

    UPROPERTY(Transient)
    TObjectPtr<UTexture2D> WaterFieldTexture;

    UPROPERTY()
    TObjectPtr<UProceduralMeshComponent> RainMesh;

    UPROPERTY()
    TObjectPtr<UProceduralMeshComponent> SnowMesh;

    UPROPERTY()
    TObjectPtr<UProceduralMeshComponent> LightningMesh;

    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> RainMaterial;

    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> SnowMaterial;

    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> LightningMaterial;

    UPROPERTY()
    TObjectPtr<UPointLightComponent> LightningLight;

    UPROPERTY()
    TObjectPtr<UCurveFloat> ExposureBiasCurve;

    UPROPERTY()
    TObjectPtr<UTexture2D> ExposureMeterMask;

    TMap<FIntPoint, TArray<uint16>> ChunkPrecipData;
    TMap<FIntPoint, TArray<uint32>> ChunkWaterData;
    TSet<FIntPoint> DirtyWaterChunks;
    FIntPoint WaterGridOrigin = FIntPoint::ZeroValue;
    bool bWaterGridReady = false;
    uint64 WaterGridRevision = 0;
    TMap<FIntVector, TArray<McTerrain::FWaterfallContact>> WaterfallSections;
    TArray<McTerrain::FWaterfallContact> NearbyWaterfalls;
    float WaterfallScanIn = 0.0f;
    uint64 LastWaterfallGridRevision = 0;
    uint64 LastRippleGridRevision = 0;
    FIntPoint LastRippleCameraBlock{INT32_MIN, INT32_MIN};
    int32 LastRippleTier = 0;
    bool bRippleHasWater = false;
    struct FWaterMover
    {
        FVector Position = FVector::ZeroVector;
        FVector Velocity = FVector::ZeroVector;
        float Surface = 0.0f;
        float PeakFallSpeed = 0.0f;
        float EntryCooldown = 0.0f;
        bool bWet = false;
        bool bValid = false;
    };
    FWaterMover PlayerWaterMover;
    TMap<uint32, FWaterMover> EntityWaterMovers;
    FRandomStream RippleRainRandom{172693};
    float RainRippleCarry = 0.0f;
    float RainCrownIn = 0.0f;
    float RippleActivity = 0.0f;
    FIntPoint PrecipGridOriginChunk = FIntPoint(INT32_MIN, INT32_MIN);
    bool bPrecipGridDirty = false;
    bool bPrecipGridReady = false;
    FBridgeSurfaceWetness TerrainWetness;
    float SurfaceRainRate = 0.0f;
    bool bTerrainWetHistoryDirty = false;
    bool bTerrainWetHistoryReady = false;
#if !UE_BUILD_SHIPPING
    uint64 TerrainWetHistoryUploads = 0;
#endif

    static constexpr int32 MaxTerrainFootsteps = 8;
    struct FTerrainPuddleImpact
    {
        FVector MinecraftPosition;
        double SpawnSeconds;
    };
    TArray<FTerrainPuddleImpact, TInlineAllocator<MaxTerrainFootsteps>> TerrainPuddleImpacts;
    struct FTerrainFootstepMotion
    {
        FVector LastFeet = FVector::ZeroVector;
        double LastSampleSeconds = 0.0;
        double SinceStep = 0.0;
        uint32 CameraFrame = 0;
        uint32 TeleportEpoch = 0;
        BridgeWire::ELocalPlayerView View = BridgeWire::ELocalPlayerView::None;
        bool bTracked = false;
        bool bLeftFoot = true;
    } TerrainFootstepMotion;
    FIntPoint TerrainFootstepOriginChunk{INT32_MIN, INT32_MIN};
    double TerrainPuddleSeconds = 0.0;
    uint64 TerrainPuddleStepCount = 0;

    struct FWeatherSimulation
    {
        bool bInitialized = false;
        bool bEnvEvInitialized = false;
        float Overcast = 0.0f;
        float Thunder = 0.0f;
        float LocalRain = 0.0f;
        float LocalSnow = 0.0f;
        float WindStrength = 0.16f;
        float SkyFlash = 0.0f;
        float Clock = 0.0f;
        float LastGameTimeSec = -1.0f;
        float UnderRoofBlend = 0.0f;
        float EnvBaseEV = 10.5f;
        float SmoothedFixedEV = 10.5f;
        FVector3f SunDirection = FVector3f(0.0f, 0.0f, 1.0f);
        FVector3f MoonDirection = FVector3f(0.0f, 0.0f, -1.0f);
        FVector4f LightningPosIntensity = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
    };
    FWeatherSimulation WeatherSim;
    TUniquePtr<FBridgeWaterSim> WaterSim;
    TUniquePtr<FBridgeWaterOptics> WaterOptics;
    TUniquePtr<FBridgeWaterParticles> WaterParticles;
    TArray<BridgeWire::FLightningStrike> ActiveLightningStrikes;

    struct FPrecipParticle
    {
        FVector OffsetUE;
        float SpeedScale;
        float Phase;
    };
    TArray<FPrecipParticle> RainParticles;
    TArray<FPrecipParticle> SnowParticles;
    double LastWeatherApplyTime = 0.0;

    UPROPERTY()
    TArray<TObjectPtr<ULocalLightComponent>> PointLightPool;

    TMap<FIntVector, int32> ActivePointLights;
    TArray<int32> FreePointLights;
    TArray<int32> FreeFlushLights;
    TArray<int32> FreeFaceLights;
    double NextPointLightUpdate = 0.0;
    uint64 PointLightRevision = 0;

    UPROPERTY()
    TArray<TObjectPtr<UMaterialInterface>> TerrainMaterialBases;

    UPROPERTY()
    TArray<TObjectPtr<UMaterialInstanceDynamic>> TerrainMaterials;

    UPROPERTY()
    FBridgeTerrainAtlasUpload TerrainAtlases[3];
    UPROPERTY()
    FBridgeTerrainAtlasUpload ReusableTerrainAtlases[3];
    FString ReusableAtlasIdentity;
    FString AtlasOfferIdentity;
    uint32 AtlasReplyScene = 0;
    BridgeWire::EAtlasCacheStatus AtlasReplyStatus = BridgeWire::EAtlasCacheStatus::None;
    TSharedPtr<FThreadSafeBool, ESPMode::ThreadSafe> AtlasReadCancelled;
    TFuture<TUniquePtr<FBridgeAtlasFile>> AtlasRead;
    TUniquePtr<FBridgeAtlasFile> PreparedAtlasFile;
    int32 AtlasCacheIndex = 0;
    int32 AtlasCacheMip = 0;
    int32 AtlasCacheRow = 0;
    int32 AtlasCacheOffset = 0;
    bool bAtlasUseDisk = false;
    bool bAtlasStaging = false;
    uint32 AtlasPatchFrames = 0;
    uint32 BlockAtlasPatchRegions = 0;
    uint32 DynamicAtlasPatchRegions = 0;
    uint32 BlockAtlasPatchMips = 0;
    uint64 AtlasPatchPixelBytes = 0;

    UPROPERTY()
    TArray<TObjectPtr<UMaterialInstanceDynamic>> CrackMaterials;

    UPROPERTY()
    TMap<FIntVector, TObjectPtr<UProceduralMeshComponent>> CrackMeshes;

    struct FCrackState
    {
        int32 SourceId;
        uint8 Stage;
    };
    TMap<FIntVector, FCrackState> Cracks;

    UPROPERTY()
    TObjectPtr<UTexture2D> UiTexture;

    FVector CameraPositionMC = FVector::ZeroVector;
    FVector CameraForwardMC = FVector::ZeroVector;
    int32 SectionFrames = 0;
    int32 DropFrames = 0;
    int32 CameraFrames = 0;
    int32 UiFrames = 0;
    int32 SectionRebuilds = 0;
    bool bTrace = false;
    double NextTraceAt = 0;
};
