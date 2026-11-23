#include "BridgeViewer.h"
#include "BridgeWaterSun.h"
#include "BridgeQuality.h"
#include "MinecraftWeatherSubsystem.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "Containers/BitArray.h"
#include "Curves/CurveFloat.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureLightProfile.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Math/Float16.h"
#include "Math/Float16Color.h"
#include "ProceduralMeshComponent.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "RenderUtils.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "TextureResource.h"
#include "UObject/ConstructorHelpers.h"

DEFINE_LOG_CATEGORY_STATIC(LogMinecraftBridgeEnvironment, Log, All);

namespace
{
    constexpr float WeatherCloudLayerHeightKm = 6.4f;
    TAutoConsoleVariable<int32> CVarPointLightBudget(TEXT("mcue.PointLights.Max"), 1024,
        TEXT("Maximum nearby Minecraft point lights with hardware MegaLights (0..2048)."));
    TAutoConsoleVariable<int32> CVarFallbackLightBudget(TEXT("mcue.PointLights.FallbackMax"), 48,
        TEXT("Maximum nearby Minecraft point lights with deferred lighting (0..128)."));
    TAutoConsoleVariable<float> CVarFaceLightSpecularScale(TEXT("mcue.PointLights.FaceSpecularScale"), -1.0f,
        TEXT("Specular contribution of face-offset terrain light proxies (-1 = auto, 0..1 = override)."));
    TAutoConsoleVariable<float> CVarContactShadowLength(TEXT("mcue.Shadows.ContactLength"), 65.0f,
        TEXT("World-space screen-space contact shadow reach in cm for directional sun/moon light (0 disables)."));
    TAutoConsoleVariable<float> CVarContactShadowIntensity(TEXT("mcue.Shadows.ContactIntensity"), 0.72f,
        TEXT("Contact shadow occlusion intensity for contact-shadow-casting geometry (0..1)."));
    TAutoConsoleVariable<float> CVarVsmLightAngleStep(TEXT("mcue.Shadows.LightAngleStep"), 0.0f,
        TEXT("Sun and moon angular step in degrees for cached terrain shadows (0 = continuous, max 0.5)."));
    TAutoConsoleVariable<float> CVarCloudCoverage(TEXT("mcue.Clouds.Coverage"), 0.5f,
        TEXT("Fair-weather low cumulus coverage (0..1). Rain and thunder override it."));
    TAutoConsoleVariable<float> CVarMidCloudAmount(TEXT("mcue.Clouds.MidLayer"), 1.0f,
        TEXT("Middle cloud bank density (0 = off, 1 = default, up to 2). Balanced and above."));
    TAutoConsoleVariable<float> CVarCirrusAmount(TEXT("mcue.Clouds.Cirrus"), 1.0f,
        TEXT("High-altitude cirrus amount (0 = off, 1 = default, up to 4 = denser). High/Ultra only."));
#if !UE_BUILD_SHIPPING
    TAutoConsoleVariable<int32> CVarNativeCloudPreview(TEXT("mcue.Clouds.PreviewNative"), 0,
        TEXT("Cloud material preview: 0 = layered Epic preset, 1 = original single-layer Epic preset."));
#endif

    constexpr uint8 FaceSource = 0x40;
    constexpr uint8 FaceMask = 0x3f;
    constexpr uint8 CoveredFace = 0x80;
    constexpr int8 CenterSource = -1;
    const FIntVector FaceNormals[] = {{0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}};

    TAutoConsoleVariable<int32> CVarWaterP1(TEXT("mc.Water.P1"), 1,
        TEXT("Enable face-aware water rendering. 0 selects the P0 material response."));
    TAutoConsoleVariable<int32> CVarWaterDebug(TEXT("mc.Water.Debug"), 0,
        TEXT("Water debug: 1 face, 2 flow, 3 shore distance, 4 height validity, 5 aeration, 6 shore wetness, 7 rain, 8 ripple height, 9 ripple foam, 10 WPO."));
    TAutoConsoleVariable<int32> CVarWaterP3(TEXT("mc.Water.P3"), 1,
        TEXT("Enable water interaction simulation and surface effects on High and Ultra."));
    TAutoConsoleVariable<int32> CVarWaterP3Wpo(TEXT("mc.Water.P3.WPO"), 1,
        TEXT("Enable shore-locked vertical water displacement on High and Ultra."));
#if !UE_BUILD_SHIPPING
    TAutoConsoleVariable<int32> CVarTerrainPuddleMotion(TEXT("mcue.TerrainPuddles.Motion"), 1,
        TEXT("Development comparison of terrain puddles with wind waves and footsteps (0 disables both)."));
#endif
    TAutoConsoleVariable<int32> CVarP2FlatInterface(TEXT("mc.Water.P2.FlatInterface"), 0,
        TEXT("Use the exact mesh normal for a flat-interface optics comparison."));
    TAutoConsoleVariable<int32> CVarWaterInterface(TEXT("mc.Water.UnderwaterInterface"), 1,
        TEXT("Water-to-air transmission and total internal reflection for visible free surfaces."));
    constexpr int32 PrecipGridSize = 256;
    constexpr int32 PrecipGridHalfChunks = 8; // 8 chunks = 128 blocks radius
    constexpr uint8 PrecipNone = 0;
    constexpr uint8 PrecipRain = 1;
    constexpr uint8 PrecipSnow = 2;

    bool TerrainPuddleMotionEnabled(int32 Tier)
    {
#if !UE_BUILD_SHIPPING
        if (CVarTerrainPuddleMotion.GetValueOnGameThread() == 0)
        {
            return false;
        }
#endif
        return BridgeQuality::RainWetnessMode() >= 2 && BridgeQuality::WaterP3Enabled(Tier) &&
            CVarWaterP3.GetValueOnGameThread() != 0;
    }

    float AuthoritativeSurfaceRain(const BridgeWire::FEnvironmentState& State)
    {
        using namespace BridgeWire;
        const bool bRain = (State.Flags & WeatherCapabilityKnown) != 0
            ? (State.Flags & (WeatherCapabilityAllowed | WeatherRaining)) == (WeatherCapabilityAllowed | WeatherRaining)
            : State.Dimension == EDimension::Overworld && State.GlobalRain > 0.2f;
        return bRain ? State.GlobalRain : 0.0f;
    }

    void UploadPrecipitationTexture(UTexture2D& Texture, TArray<FFloat16Color>&& Pixels)
    {
        if (!Texture.GetResource())
        {
            return;
        }
        struct FGridUpload
        {
            FUpdateTextureRegion2D Region{0, 0, 0, 0, PrecipGridSize, PrecipGridSize};
            TArray<FFloat16Color> Data;
        };
        FGridUpload* Upload = new FGridUpload;
        Upload->Data = MoveTemp(Pixels);
        Texture.UpdateTextureRegions(0, 1, &Upload->Region, PrecipGridSize * sizeof(FFloat16Color),
            sizeof(FFloat16Color), reinterpret_cast<uint8*>(Upload->Data.GetData()),
            [Upload](uint8*, const FUpdateTextureRegion2D*) { delete Upload; });
    }

    FIntVector SlotKey(const FIntVector& Block, int8 Slot)
    {
        // Height is bounded by the world message. X/Z retain the full world-border range.
        static_assert(8 + BridgeWire::MaxMaterialPatches <= 64);
        return FIntVector(Block.X, Block.Y * 64 + Slot + 1, Block.Z);
    }

    float Hash01(uint32 Seed)
    {
        Seed = (Seed ^ 61u) ^ (Seed >> 16);
        Seed *= 9u;
        Seed ^= Seed >> 4;
        Seed *= 0x27d4eb2du;
        Seed ^= Seed >> 15;
        return float(Seed & 0x00FFFFFFu) / float(0x01000000u);
    }

    void SetOptionalCVarInt(const TCHAR* Name, int32 Value)
    {
        if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            static const FName Tag(TEXT("MinecraftBridgeAtmosphere"));
            Var->Set(Value, ECVF_SetByGameOverride, Tag);
        }
    }

    void SetOptionalCVarFloat(const TCHAR* Name, float Value)
    {
        if (IConsoleVariable* Var = IConsoleManager::Get().FindConsoleVariable(Name))
        {
            static const FName Tag(TEXT("MinecraftBridgeAtmosphere"));
            Var->Set(Value, ECVF_SetByGameOverride, Tag);
        }
    }
}

void ABridgeViewer::CreateEnvironmentComponents()
{
    TerrainSun = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("MinecraftSun"));
    TerrainMoon = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("MinecraftMoon"));
    TerrainAtmosphere = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("MinecraftAtmosphere"));
    TerrainSkyLight = CreateDefaultSubobject<USkyLightComponent>(TEXT("MinecraftSkyLight"));
    TerrainClouds = CreateDefaultSubobject<UVolumetricCloudComponent>(TEXT("MinecraftClouds"));
    TerrainFog = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("MinecraftHeightFog"));
    TerrainSkyDome = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MinecraftSkyDome"));
    RainMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MinecraftRainMesh"));
    SnowMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MinecraftSnowMesh"));
    LightningMesh = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("MinecraftLightningMesh"));
    LightningLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("MinecraftLightningLight"));

    USceneComponent* Components[] = {
        TerrainSun, TerrainMoon, TerrainAtmosphere, TerrainSkyLight, TerrainClouds, TerrainFog,
        TerrainSkyDome, RainMesh, SnowMesh, LightningMesh, LightningLight
    };
    for (USceneComponent* Component : Components)
    {
        Component->SetupAttachment(RootComponent);
        Component->SetMobility(EComponentMobility::Movable);
        Component->SetVisibility(false);
    }

    for (UDirectionalLightComponent* Light : {TerrainSun.Get(), TerrainMoon.Get()})
    {
        Light->SetCastShadows(true);
        Light->SetCastRaytracedShadows(ECastRayTracedShadow::Disabled);
        Light->SetAtmosphereSunLight(true);
        Light->bAllowMegaLights = false;
        Light->ContactShadowLengthInWS = true;
        Light->ContactShadowLength = 65.0f;
        Light->ContactShadowCastingIntensity = 0.72f;
        Light->ContactShadowNonCastingIntensity = 0.0f;
    }

    // Physical sunlight: keep color near neutral white so SkyAtmosphere drives authentic sunset reds/golds.
    TerrainSun->SetAtmosphereSunLightIndex(0);
    TerrainSun->SetLightSourceAngle(0.58f);
    TerrainSun->SetLightColor(FLinearColor(1.0f, 0.985f, 0.96f));
    TerrainSun->SetAtmosphereSunDiskColorScale(FLinearColor(1.0f, 0.95f, 0.88f));
    TerrainSun->bCastCloudShadows = true;
    TerrainSun->CloudShadowOnSurfaceStrength = 0.68f;
    TerrainSun->CloudShadowOnAtmosphereStrength = 0.55f;
    TerrainSun->CloudShadowExtent = 8.5f;
    TerrainSun->SetVolumetricScatteringIntensity(1.65f);

    TerrainMoon->SetAtmosphereSunLightIndex(1);
    TerrainMoon->SetLightSourceAngle(0.55f);
    TerrainMoon->SetLightColor(FLinearColor(0.56f, 0.72f, 1.0f));
    // SkyDome renders the detailed cratered 8-phase moon disk; suppress the engine's harsh flat disk.
    TerrainMoon->SetAtmosphereSunDiskColorScale(FLinearColor::Black);
    TerrainMoon->SetVolumetricScatteringIntensity(0.45f);

    // Physical Earth-scale atmosphere with enriched ozone absorption (deep cobalt zenith + burning amber sunsets).
    TerrainAtmosphere->TransformMode = ESkyAtmosphereTransformMode::PlanetTopAtComponentTransform;
    TerrainAtmosphere->SetRelativeLocation(FVector(0.0, 0.0, -16000.0));
    TerrainAtmosphere->SetRayleighScatteringScale(0.0365f);
    TerrainAtmosphere->SetMieScatteringScale(0.0056f);
    TerrainAtmosphere->SetMieAnisotropy(0.84f);
    TerrainAtmosphere->SetOtherAbsorptionScale(0.0036f);
    TerrainAtmosphere->SetMultiScatteringFactor(1.18f);
    TerrainAtmosphere->SetHeightFogContribution(1.0f);
    TerrainAtmosphere->SetAerialPespectiveViewDistanceScale(1.15f);

    // Real-time captured skylight with cloud AO so lakes mirror sunset clouds and overcast shifts accurately.
    TerrainSkyLight->SetRealTimeCapture(true);
    TerrainSkyLight->bCloudAmbientOcclusion = true;
    TerrainSkyLight->CloudAmbientOcclusionStrength = 0.52f;
    TerrainSkyLight->bLowerHemisphereIsBlack = false;
    TerrainSkyLight->SetLowerHemisphereColor(FLinearColor(0.04f, 0.05f, 0.07f));
    TerrainSkyLight->CubemapResolution = 256;

    TerrainFog->SetRelativeLocation(FVector(0.0, 0.0, 6400.0));
    TerrainFog->SetVolumetricFogDistance(20000.0f);
    TerrainFog->SetVolumetricFogScatteringDistribution(0.58f);

    // Low cumulus, middle banks and cirrus occupy different parts of a 1.6–8.0 km shell.
    TerrainClouds->SetLayerBottomAltitude(1.6f);
    TerrainClouds->SetLayerHeight(WeatherCloudLayerHeightKm);
    TerrainClouds->SetTracingStartMaxDistance(85.0f);
    TerrainClouds->SetTracingMaxDistance(62.0f);
    TerrainClouds->SetStopTracingTransmittanceThreshold(0.005f);
    TerrainClouds->SetbUsePerSampleAtmosphericLightTransmittance(true);
    TerrainClouds->SetViewSampleCountScale(1.0f);
    TerrainClouds->SetShadowViewSampleCountScale(1.0f);

    // SkyDome sphere (Material has IsSky=true, TwoSided=true). Bind in CDO so Shipping Cook always packages SM_SkySphere.
    static ConstructorHelpers::FObjectFinder<UStaticMesh> SkySphereMesh(TEXT("/Engine/EngineSky/SM_SkySphere.SM_SkySphere"));
    if (SkySphereMesh.Succeeded())
    {
        TerrainSkyDome->SetStaticMesh(SkySphereMesh.Object);
    }
    TerrainSkyDome->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    TerrainSkyDome->SetCastShadow(false);
    TerrainSkyDome->bAffectDistanceFieldLighting = false;
    TerrainSkyDome->bAffectDynamicIndirectLighting = false;
    TerrainSkyDome->SetRelativeScale3D(FVector(45000.0f));

    for (UProceduralMeshComponent* EffectMesh : {RainMesh.Get(), SnowMesh.Get(), LightningMesh.Get()})
    {
        EffectMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        EffectMesh->SetCastShadow(false);
        EffectMesh->bAffectDistanceFieldLighting = false;
        EffectMesh->bAffectDynamicIndirectLighting = false;
        EffectMesh->bCastDynamicShadow = false;
    }

    LightningLight->SetIntensityUnits(ELightUnits::Lumens);
    LightningLight->SetLightColor(FLinearColor(0.68f, 0.82f, 1.0f));
    LightningLight->SetCastShadows(false);
    LightningLight->SetAttenuationRadius(18000.0f);
    LightningLight->SetSourceRadius(300.0f);
    LightningLight->SetVolumetricScatteringIntensity(3.5f);
}

void ABridgeViewer::ConfigureEnvironment()
{
    if (UMaterialInterface* Cloud = LoadObject<UMaterialInterface>(nullptr,
        TEXT("/Game/Bridge/MI_EpicCloudLayers.MI_EpicCloudLayers")))
    {
        UMaterial* Base = Cloud->GetMaterial();
        if (Base && Base->MaterialDomain == MD_Volume && Base->GetUsageByFlag(MATUSAGE_VolumetricCloud))
        {
            Cloud->EnsureIsComplete();
            if (Cloud->IsComplete())
            {
                CloudMaterial = UMaterialInstanceDynamic::Create(Cloud, this);
                TerrainClouds->SetMaterial(CloudMaterial);
            }
        }
    }
    if (!CloudMaterial)
    {
        UMaterialInterface* Stock = LoadObject<UMaterialInterface>(nullptr,
            TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst"));
        TerrainClouds->SetMaterial(Stock);
        TerrainClouds->SetLayerHeight(4.4f);
        UE_LOG(LogMinecraftBridgeEnvironment, Warning, TEXT("Stacked Epic cloud material unavailable. Using %s."), *GetPathNameSafe(Stock));
    }

    if (UStaticMesh* SkySphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineSky/SM_SkySphere.SM_SkySphere")))
    {
        TerrainSkyDome->SetStaticMesh(SkySphere);
    }
    if (UMaterialInterface* SkyBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Bridge/M_McSkyDome.M_McSkyDome")))
    {
        SkyBase->EnsureIsComplete();
        SkyDomeMaterial = UMaterialInstanceDynamic::Create(SkyBase, this);
        TerrainSkyDome->SetMaterial(0, SkyDomeMaterial);
    }

    if (UMaterialInterface* PrecipBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Bridge/M_McPrecipitation.M_McPrecipitation")))
    {
        PrecipBase->EnsureIsComplete();
        RainMaterial = UMaterialInstanceDynamic::Create(PrecipBase, this);
        RainMaterial->SetScalarParameterValue(TEXT("PrecipMode"), 0.0f);
        RainMaterial->SetVectorParameterValue(TEXT("PrecipTint"), FLinearColor(0.76f, 0.84f, 0.94f, 0.52f));
        RainMesh->SetMaterial(0, RainMaterial);

        SnowMaterial = UMaterialInstanceDynamic::Create(PrecipBase, this);
        SnowMaterial->SetScalarParameterValue(TEXT("PrecipMode"), 1.0f);
        SnowMaterial->SetVectorParameterValue(TEXT("PrecipTint"), FLinearColor(0.94f, 0.97f, 1.0f, 0.82f));
        SnowMesh->SetMaterial(0, SnowMaterial);
    }

    if (UMaterialInterface* BoltBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Bridge/M_McLightning.M_McLightning")))
    {
        BoltBase->EnsureIsComplete();
        LightningMaterial = UMaterialInstanceDynamic::Create(BoltBase, this);
        LightningMesh->SetMaterial(0, LightningMaterial);
    }

    // Create 256x256 FloatRGBA precipitation height + biome grid texture (512 KiB).
    PrecipGridTexture = UTexture2D::CreateTransient(PrecipGridSize, PrecipGridSize, PF_FloatRGBA);
    if (PrecipGridTexture)
    {
        PrecipGridTexture->SRGB = false;
        PrecipGridTexture->Filter = TF_Nearest;
        PrecipGridTexture->AddressX = TA_Clamp;
        PrecipGridTexture->AddressY = TA_Clamp;
        PrecipGridTexture->NeverStream = true;
        PrecipGridTexture->UpdateResource();
    }
    InitializeTerrainWetnessTextures(PrecipGridSize);

    // Build procedural 2D IES mask for single-face flush emitters (e.g. magma blocks in flat ground).
    CreateBlockLightProfiles();

    // Seed deterministic rain/snow particle pools around the camera.
    InitializeWaterGrid();

    constexpr int32 MaxRainPool = 2000;
    constexpr int32 MaxSnowPool = 1400;
    RainParticles.Reset(MaxRainPool);
    for (int32 I = 0; I < MaxRainPool; ++I)
    {
        const float Angle = Hash01(uint32(I * 3 + 11)) * 2.0f * UE_PI;
        const float Radius = FMath::Sqrt(Hash01(uint32(I * 3 + 17))) * 2600.0f; // 26m radius
        const float HeightZ = (Hash01(uint32(I * 3 + 23)) - 0.35f) * 2800.0f;
        RainParticles.Add({
            FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, HeightZ),
            0.82f + 0.38f * Hash01(uint32(I * 5 + 31)),
            Hash01(uint32(I * 7 + 43)) * 6.283185f
        });
    }
    SnowParticles.Reset(MaxSnowPool);
    for (int32 I = 0; I < MaxSnowPool; ++I)
    {
        const float Angle = Hash01(uint32(I * 3 + 101)) * 2.0f * UE_PI;
        const float Radius = FMath::Sqrt(Hash01(uint32(I * 3 + 107))) * 3000.0f; // 30m radius
        const float HeightZ = (Hash01(uint32(I * 3 + 113)) - 0.35f) * 2600.0f;
        SnowParticles.Add({
            FVector(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, HeightZ),
            0.70f + 0.60f * Hash01(uint32(I * 5 + 127)),
            Hash01(uint32(I * 7 + 139)) * 6.283185f
        });
    }

    CreateExposureAssets();

    // Calibrated post-processing baseline: center-weighted metering, perceptual bias curve,
    // tight histogram precision, and bilateral local exposure for simultaneous highlight/shadow clarity.
    FPostProcessSettings& Post = Camera->GetCameraComponent()->PostProcessSettings;
    Post.bOverride_AutoExposureMethod = true;
    Post.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
    Post.bOverride_HistogramLogMin = Post.bOverride_HistogramLogMax = true;
    Post.HistogramLogMin = -1.0f;
    Post.HistogramLogMax = 15.0f;
    Post.bOverride_AutoExposureLowPercent = Post.bOverride_AutoExposureHighPercent = true;
    Post.AutoExposureLowPercent = 45.0f;
    Post.AutoExposureHighPercent = 82.0f;
    Post.bOverride_AutoExposureMeterMask = ExposureMeterMask != nullptr;
    Post.AutoExposureMeterMask = ExposureMeterMask;
    Post.bOverride_AutoExposureBiasCurve = ExposureBiasCurve != nullptr;
    Post.AutoExposureBiasCurve = ExposureBiasCurve;
    Post.bOverride_AutoExposureMinBrightness = Post.bOverride_AutoExposureMaxBrightness = true;
    Post.AutoExposureMinBrightness = 2.0f;
    Post.AutoExposureMaxBrightness = 12.6f;
    Post.bOverride_AutoExposureBias = true;
    Post.AutoExposureBias = 0.0f;
    Post.bOverride_AutoExposureSpeedUp = Post.bOverride_AutoExposureSpeedDown = true;
    Post.AutoExposureSpeedUp = 4.0f;
    Post.AutoExposureSpeedDown = 2.5f;
    Post.bOverride_BloomIntensity = true;
    Post.BloomIntensity = 0.28f;
    Post.bOverride_BloomThreshold = true;
    Post.BloomThreshold = 1.35f;
    Post.bOverride_LocalExposureMethod = true;
    Post.LocalExposureMethod = ELocalExposureMethod::Bilateral;
    Post.bOverride_LocalExposureHighlightContrastScale = true;
    Post.bOverride_LocalExposureShadowContrastScale = true;
    Post.bOverride_LocalExposureDetailStrength = true;
    Post.LocalExposureDetailStrength = 1.0f;
    Post.bOverride_LocalExposureBlurredLuminanceBlend = true;
    Post.LocalExposureBlurredLuminanceBlend = 0.45f;
    Post.bOverride_LocalExposureBlurredLuminanceKernelSizePercent = true;
    Post.LocalExposureBlurredLuminanceKernelSizePercent = 40.0f;
    Post.bOverride_LocalExposureHighlightThreshold = true;
    Post.LocalExposureHighlightThreshold = 0.35f;
    Post.bOverride_LocalExposureShadowThreshold = true;
    Post.LocalExposureShadowThreshold = 0.35f;
    Post.bOverride_LumenFrontLayerTranslucencyReflections = true;
    Post.LumenFrontLayerTranslucencyReflections = true;
    UpdateExposureSettings(0.0f, WeatherSim.EnvBaseEV, true, true, true);
    RefreshEnvironmentQuality();
}

void ABridgeViewer::CreateExposureAssets()
{
    // Perceptual exposure compensation curve (EV100 -> EV bias):
    // Keeps nights and unlit caves naturally dark instead of over-boosting to grey,
    // keeps interiors/dusk clear, keeps bright days vibrant, and rolls off extreme glare.
    ExposureBiasCurve = NewObject<UCurveFloat>(this);
    if (ExposureBiasCurve)
    {
        FRichCurve& Curve = ExposureBiasCurve->FloatCurve;
        const auto AddKey = [&Curve](float EV, float Bias)
        {
            const FKeyHandle Handle = Curve.AddKey(EV, Bias);
            Curve.SetKeyInterpMode(Handle, RCIM_Cubic);
            Curve.SetKeyTangentMode(Handle, RCTM_Auto);
        };
        AddKey(-2.0f, -0.35f);
        AddKey( 1.5f, -0.30f);
        AddKey( 3.0f, -0.15f);
        AddKey( 4.8f,  0.05f);
        AddKey( 8.2f,  0.10f);
        AddKey(10.3f,  0.35f);
        AddKey(12.2f,  0.10f);
        AddKey(14.5f, -0.25f);
    }

    // Center-weighted metering mask slightly below horizon (u=0.50, v=0.56) so top-edge sky
    // and screen-border blocks/torches do not violently pump exposure as the player looks around.
    constexpr int32 MaskSize = 64;
    TArray<uint8> Pixels;
    Pixels.SetNumUninitialized(MaskSize * MaskSize * 4);
    for (int32 Y = 0; Y < MaskSize; ++Y)
    {
        const float V = (float(Y) + 0.5f) / float(MaskSize); // 0 = top (sky), 1 = bottom (ground)
        const float SkyDamp = FMath::Lerp(0.45f, 1.0f, FMath::SmoothStep(0.0f, 0.34f, V));
        for (int32 X = 0; X < MaskSize; ++X)
        {
            const float U = (float(X) + 0.5f) / float(MaskSize);
            const float Dx = (U - 0.50f) / 0.42f;
            const float Dy = (V - 0.56f) / 0.36f;
            const float CenterWeight = FMath::Exp(-0.5f * (Dx * Dx + Dy * Dy));
            const float Weight = FMath::Clamp(FMath::Lerp(0.22f, 1.0f, CenterWeight) * SkyDamp, 0.14f, 1.0f);
            const uint8 ByteVal = uint8(FMath::Clamp(FMath::RoundToInt(Weight * 255.0f), 0, 255));
            const int32 Idx = (Y * MaskSize + X) * 4;
            Pixels[Idx + 0] = ByteVal;
            Pixels[Idx + 1] = ByteVal;
            Pixels[Idx + 2] = ByteVal;
            Pixels[Idx + 3] = 255;
        }
    }
    ExposureMeterMask = UTexture2D::CreateTransient(MaskSize, MaskSize, PF_B8G8R8A8, NAME_None,
        TConstArrayView64<uint8>(Pixels.GetData(), Pixels.Num()));
    if (ExposureMeterMask)
    {
        ExposureMeterMask->SRGB = false;
        ExposureMeterMask->Filter = TF_Bilinear;
        ExposureMeterMask->AddressX = TA_Clamp;
        ExposureMeterMask->AddressY = TA_Clamp;
        ExposureMeterMask->NeverStream = true;
        ExposureMeterMask->UpdateResource();
    }
}

float ABridgeViewer::SampleRoofCoverage() const
{
    if (!bPrecipGridReady || ChunkPrecipData.IsEmpty() || CameraFrames == 0)
    {
        // Heightmap unavailable: allow full indoor/cave adaptation range safely.
        return 1.0f;
    }
    const int32 CamX = FMath::FloorToInt(CameraPositionMC.X);
    const int32 CamZ = FMath::FloorToInt(CameraPositionMC.Z);
    const double CamY = CameraPositionMC.Y;
    int32 Valid = 0;
    int32 Covered = 0;
    for (int32 Dz = -1; Dz <= 1; ++Dz)
    {
        for (int32 Dx = -1; Dx <= 1; ++Dx)
        {
            int32 TopY = MinY;
            uint8 BiomeType = PrecipNone;
            if (SamplePrecipColumn(CamX + Dx, CamZ + Dz, TopY, BiomeType))
            {
                ++Valid;
                if (double(TopY) > CamY + 1.6)
                {
                    ++Covered;
                }
            }
        }
    }
    return Valid > 0 ? float(Covered) / float(Valid) : 1.0f;
}

float ABridgeViewer::SampleWaterSkyExposure() const
{
    constexpr uint32 KnownWater = (1u << 26) | (1u << 27);
    constexpr uint32 OpenSky = 1u << 28;
    constexpr uint32 SaturatedDepth = 1u << 31;
    const FVector2d EyeInCellCenters(CameraPositionMC.X - 0.5, CameraPositionMC.Z - 0.5);
    const FIntPoint Corner(FMath::FloorToInt32(EyeInCellCenters.X), FMath::FloorToInt32(EyeInCellCenters.Y));
    const auto Sample = [this](FIntPoint Position) -> float
    {
        const uint32 Cell = WaterColumn(Position);
        if ((Cell & KnownWater) == KnownWater)
        {
            const double Surface = MinY + double(Cell & 2047u) + double((Cell >> 11) & 63u) / 64.0;
            const double Floor = FMath::CeilToDouble(Surface) - double((Cell >> 17) & 31u);
            const double Depth = Surface - CameraPositionMC.Y;
            // Match the layer containing the eye. A higher, open pool cannot light a separate cave.
            if (Depth >= -0.65 && (CameraPositionMC.Y >= Floor || (Cell & SaturatedDepth) != 0))
            {
                return (Cell & OpenSky) != 0 ? float(FMath::Exp(-0.045 * FMath::Max(0.0, Depth))) : 0.0f;
            }
        }
        int32 RoofY = MinY;
        uint8 BiomeType = PrecipNone;
        return SamplePrecipColumn(Position.X, Position.Y, RoofY, BiomeType) &&
            double(RoofY) <= CameraPositionMC.Y + 1.6 ? 1.0f : 0.0f;
    };
    return FMath::BiLerp(Sample(Corner), Sample(Corner + FIntPoint(1, 0)),
        Sample(Corner + FIntPoint(0, 1)), Sample(Corner + FIntPoint(1, 1)),
        float(EyeInCellCenters.X - Corner.X), float(EyeInCellCenters.Y - Corner.Y));
}

void ABridgeViewer::UpdateExposureSettings(float DeltaSeconds, float EnvBaseEV, bool bOverworld, bool bAir, bool bSnap, float WaterWeight)
{
    if (!Camera)
    {
        return;
    }
    const float TargetRoof = (bOverworld && bAir) ? FMath::Lerp(SampleRoofCoverage(), 1.0f, WaterWeight) : 1.0f;
    if (bSnap || !WeatherSim.bEnvEvInitialized || DeltaSeconds <= 0.0f)
    {
        WeatherSim.UnderRoofBlend = TargetRoof;
        WeatherSim.SmoothedFixedEV = EnvBaseEV;
        WeatherSim.bEnvEvInitialized = true;
    }
    else
    {
        WeatherSim.UnderRoofBlend = FMath::FInterpTo(WeatherSim.UnderRoofBlend, TargetRoof, DeltaSeconds, 1.8f);
        WeatherSim.SmoothedFixedEV = FMath::FInterpTo(WeatherSim.SmoothedFixedEV, EnvBaseEV, DeltaSeconds, 1.5f);
    }
    WeatherSim.EnvBaseEV = EnvBaseEV;

    const int32 Mode = BridgeQuality::ExposureMode(); // 0=Fixed, 1=Auto, 2=Gentle
    const float UserBiasEV = BridgeQuality::ExposureBias();
    const float LocalAmt = BridgeQuality::LocalExposureStrength() * 0.01f;

    FPostProcessSettings& Post = Camera->GetCameraComponent()->PostProcessSettings;
    Post.LocalExposureHighlightContrastScale = FMath::Lerp(1.0f, 0.65f, LocalAmt);
    Post.LocalExposureShadowContrastScale = FMath::Lerp(1.0f, 0.62f, LocalAmt);

    if (Mode == 0)
    {
        // Fixed scene-driven exposure: zero camera-angle pumping, smooth day/night/weather tracking.
        const float FixedEV = WeatherSim.SmoothedFixedEV;
        const float CurveBias = ExposureBiasCurve ? ExposureBiasCurve->GetFloatValue(FixedEV) : 0.0f;
        Post.AutoExposureMinBrightness = FixedEV;
        Post.AutoExposureMaxBrightness = FixedEV;
        Post.bOverride_AutoExposureBiasCurve = false;
        Post.AutoExposureBiasCurve = nullptr;
        Post.AutoExposureBias = CurveBias + UserBiasEV;
        Post.AutoExposureSpeedUp = 4.0f;
        Post.AutoExposureSpeedDown = 4.0f;
        return;
    }

    const bool bGentle = (Mode == 2);
    const float IndoorAlpha = FMath::SmoothStep(0.22f, 0.78f, WeatherSim.UnderRoofBlend);
    float MinEV = 2.0f;
    float MaxEV = 12.6f;
    if (bOverworld && bAir)
    {
        const float OutdoorMinEV = FMath::Max(2.0f, EnvBaseEV - (bGentle ? 1.5f : 2.6f));
        const float IndoorMinEV = bGentle ? 2.4f : 2.0f;
        MinEV = FMath::Lerp(OutdoorMinEV, IndoorMinEV, IndoorAlpha);
        MaxEV = FMath::Clamp(EnvBaseEV + (bGentle ? 0.9f : 1.4f), bGentle ? 4.8f : 5.6f, 12.6f);
    }
    else if (bOverworld && !bAir)
    {
        MinEV = FMath::Max(2.0f, EnvBaseEV - (bGentle ? 1.4f : 2.2f));
        MaxEV = FMath::Clamp(EnvBaseEV + (bGentle ? 1.0f : 1.6f), 4.6f, 11.2f);
    }
    else
    {
        // Nether / End: protect against lava lakes crushing dark cavern visibility.
        MinEV = bGentle ? 2.6f : 2.2f;
        MaxEV = FMath::Clamp(EnvBaseEV + (bGentle ? 1.6f : 2.6f), 5.6f, 7.2f);
    }
    if (WaterWeight > 0.0f && bOverworld && bAir)
    {
        const float WaterMinEV = FMath::Max(2.0f, EnvBaseEV - (bGentle ? 1.4f : 2.2f));
        const float WaterMaxEV = FMath::Clamp(EnvBaseEV + (bGentle ? 1.0f : 1.6f), 4.6f, 11.2f);
        MinEV = FMath::Lerp(MinEV, WaterMinEV, WaterWeight);
        MaxEV = FMath::Lerp(MaxEV, WaterMaxEV, WaterWeight);
    }
    MinEV = FMath::Min(MinEV, MaxEV - 0.5f);

    Post.AutoExposureMinBrightness = MinEV;
    Post.AutoExposureMaxBrightness = MaxEV;
    Post.bOverride_AutoExposureBiasCurve = ExposureBiasCurve != nullptr;
    Post.AutoExposureBiasCurve = ExposureBiasCurve;
    Post.AutoExposureBias = UserBiasEV;
    Post.AutoExposureLowPercent = bGentle ? 50.0f : 45.0f;
    Post.AutoExposureHighPercent = bGentle ? 80.0f : 82.0f;
    // Prevent lightning flashes from slamming auto-exposure down into black.
    const float BaseUp = bGentle ? 3.2f : 4.0f;
    const float BaseDown = bGentle ? 2.0f : 2.5f;
    Post.AutoExposureSpeedUp = WeatherSim.SkyFlash > 0.08f ? 0.35f : BaseUp;
    Post.AutoExposureSpeedDown = BaseDown;
}

void ABridgeViewer::RefreshEnvironmentQuality()
{
    BridgeQuality::Tick();
    const uint32 Revision = BridgeQuality::Revision();
    if (AppliedQualityRevision == Revision)
    {
        return;
    }
    AppliedQualityRevision = Revision;
    if (const auto* Collection = LoadObject<UMaterialParameterCollection>(
        nullptr, TEXT("/Game/Bridge/MPC_McWaterOptics.MPC_McWaterOptics"), nullptr, LOAD_NoWarn))
    {
        if (auto* Instance = GetWorld()->GetParameterCollectionInstance(Collection))
        {
            const FVector2f Scale = BridgeQuality::WaterOpticalScale();
            Instance->SetVectorParameterValue(TEXT("WaterOpticalScale"), FLinearColor(Scale.X, Scale.Y, 0.0f, 0.0f));
        }
    }
    const bool bHardware = BridgeQuality::HardwareLighting();
    const bool bHardwareGi = BridgeQuality::HardwareGiEnabled();
    const bool bHardwareReflections = BridgeQuality::HardwareReflectionsEnabled();
    const int32 Tier = BridgeQuality::PresetTier(); // 0=Perf, 1=Balanced, 2=High, 3=Ultra

    FPostProcessSettings& Post = Camera->GetCameraComponent()->PostProcessSettings;
    // Streamed region proxies have no mesh distance fields for software Lumen.
    Post.bOverride_ReflectionMethod = !bHardwareReflections;
    Post.ReflectionMethod = EReflectionMethod::ScreenSpace;
    Post.bOverride_DynamicGlobalIlluminationMethod = !bHardwareGi;
    Post.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
    Post.LumenFrontLayerTranslucencyReflections = BridgeQuality::TranslucencyReflectionsEnabled();

    // User visual post-process preferences (Bloom, Color grading, Vignette, Motion Blur).
    Post.bOverride_BloomIntensity = true;
    Post.BloomIntensity = 0.28f * BridgeQuality::BloomScale();
    const float Sat = BridgeQuality::ColorSaturation();
    const float Con = BridgeQuality::ColorContrast();
    Post.bOverride_ColorSaturation = true;
    Post.ColorSaturation = FVector4(Sat, Sat, Sat, 1.0f);
    Post.bOverride_ColorContrast = true;
    Post.ColorContrast = FVector4(Con, Con, Con, 1.0f);
    Post.bOverride_WhiteTemp = true;
    Post.WhiteTemp = BridgeQuality::ColorTemp();
    Post.bOverride_VignetteIntensity = true;
    Post.VignetteIntensity = BridgeQuality::VignetteIntensity();
    const float BlurAmt = BridgeQuality::MotionBlurAmount();
    Post.bOverride_MotionBlurAmount = true;
    Post.MotionBlurAmount = BlurAmt;

    const auto ShadowMethod = BridgeQuality::DirectionalRayShadows() == 1 ?
        ECastRayTracedShadow::Enabled : ECastRayTracedShadow::Disabled;
    TerrainSun->SetCastRaytracedShadows(ShadowMethod);
    TerrainMoon->SetCastRaytracedShadows(ShadowMethod);

    // Tiered MSFS/RDR2 volumetric cloud, cloud-shadow, sky-capture & reflection quality.
    const float CloudSamples = BridgeQuality::CloudSamples();
    const bool bCloudShadows = BridgeQuality::CloudShadowsEnabled(Tier) && CloudSamples > 0.0f;
    TerrainClouds->SetViewSampleCountScale(FMath::Max(0.01f, CloudSamples));
    SetOptionalCVarFloat(TEXT("mcue.Shadows.LightAngleStep"), BridgeQuality::SunShadowSmooth(Tier) ? 0.0f : 0.125f);
    SetOptionalCVarInt(TEXT("r.VolumetricCloud.ShadowMap.MaxResolution"), Tier >= 3 ? 512 : 256);
    SetOptionalCVarInt(TEXT("r.VolumetricCloud.SkyAO.MaxResolution"), Tier >= 3 ? 2048 : 256);
    if (Tier >= 3)
    {
        // Ultra: keep the widest cloud shell and three-layer detail, but time-slice the sky capture
        // and cap cloud shadow/raymarch budgets so SkyLight/cloud passes do not stall mid-range GPUs.
        TerrainClouds->SetTracingStartMaxDistance(100.0f);
        TerrainClouds->SetTracingMaxDistance(85.0f);
        TerrainClouds->SetShadowViewSampleCountScale(1.0f);
        TerrainClouds->SetStopTracingTransmittanceThreshold(0.003f);
        TerrainSun->bCastCloudShadows = bCloudShadows;
        TerrainSun->CloudShadowExtent = 12.0f;
        TerrainSun->CloudShadowOnSurfaceStrength = 0.74f;
        TerrainSun->CloudShadowOnAtmosphereStrength = 0.72f;
        TerrainSkyLight->CubemapResolution = 256;
        SetOptionalCVarInt(TEXT("r.VolumetricCloud.ShadowMap"), bCloudShadows ? 1 : 0);
        SetOptionalCVarInt(TEXT("r.VolumetricFog.GridSizeZ"), 128);
        SetOptionalCVarInt(TEXT("r.SkyLight.RealTimeReflectionCapture.TimeSlice"), 1);
        SetOptionalCVarFloat(TEXT("r.VolumetricCloud.ViewRaySampleMaxCount"), 192.0f);
    }
    else if (Tier == 2)
    {
        // High uses all three cloud bands and directional cloud shadows.
        TerrainClouds->SetTracingStartMaxDistance(85.0f);
        TerrainClouds->SetTracingMaxDistance(62.0f);
        TerrainClouds->SetShadowViewSampleCountScale(1.0f);
        TerrainClouds->SetStopTracingTransmittanceThreshold(0.005f);
        TerrainSun->bCastCloudShadows = bCloudShadows;
        TerrainSun->CloudShadowExtent = 8.5f;
        TerrainSun->CloudShadowOnSurfaceStrength = 0.68f;
        TerrainSun->CloudShadowOnAtmosphereStrength = 0.55f;
        TerrainSkyLight->CubemapResolution = 256;
        SetOptionalCVarInt(TEXT("r.VolumetricCloud.ShadowMap"), bCloudShadows ? 1 : 0);
        SetOptionalCVarInt(TEXT("r.VolumetricFog.GridSizeZ"), 64);
        SetOptionalCVarInt(TEXT("r.SkyLight.RealTimeReflectionCapture.TimeSlice"), 1);
        SetOptionalCVarFloat(TEXT("r.VolumetricCloud.ViewRaySampleMaxCount"), 160.0f);
    }
    else if (Tier == 1)
    {
        // Balanced: sculpted cumulus + soft cloud shadows, trimmed raymarch cost.
        TerrainClouds->SetTracingStartMaxDistance(65.0f);
        TerrainClouds->SetTracingMaxDistance(42.0f);
        TerrainClouds->SetShadowViewSampleCountScale(0.55f);
        TerrainClouds->SetStopTracingTransmittanceThreshold(0.01f);
        TerrainSun->bCastCloudShadows = bCloudShadows;
        TerrainSun->CloudShadowExtent = 6.0f;
        TerrainSun->CloudShadowOnSurfaceStrength = 0.52f;
        TerrainSun->CloudShadowOnAtmosphereStrength = 0.25f;
        TerrainSkyLight->CubemapResolution = 128;
        SetOptionalCVarInt(TEXT("r.VolumetricCloud.ShadowMap"), bCloudShadows ? 1 : 0);
        SetOptionalCVarInt(TEXT("r.VolumetricFog.GridSizeZ"), 64);
        SetOptionalCVarInt(TEXT("r.SkyLight.RealTimeReflectionCapture.TimeSlice"), 1);
        SetOptionalCVarFloat(TEXT("r.VolumetricCloud.ViewRaySampleMaxCount"), 96.0f);
    }
    else
    {
        // Performance: volumetric cloud disabled unless custom cloud scale enabled, max FPS.
        TerrainSun->bCastCloudShadows = bCloudShadows;
        TerrainSkyLight->CubemapResolution = 128;
        SetOptionalCVarInt(TEXT("r.VolumetricCloud.ShadowMap"), bCloudShadows ? 1 : 0);
        SetOptionalCVarInt(TEXT("r.SkyLight.RealTimeReflectionCapture.TimeSlice"), 1);
    }

    NextPointLightUpdate = 0;
    if (Environment)
    {
        PresentEnvironment(*Environment);
    }
    else
    {
        UpdateExposureSettings(0.0f, WeatherSim.EnvBaseEV, true, true, false);
        UpdateTerrainLightingParameters();
    }
    UE_LOG(LogMinecraftBridgeEnvironment, Display,
        TEXT("Bridge lighting: tier=%d hardGi=%d hardRefl=%d MegaLights=%d cloudSamples=%.2f expMode=%d expBias=%.1f localExp=%d."),
        Tier, bHardwareGi, bHardwareReflections, BridgeQuality::MegaLights(), CloudSamples,
        BridgeQuality::ExposureMode(), BridgeQuality::ExposureBias(), BridgeQuality::LocalExposureStrength());
}

bool ABridgeViewer::ApplyEnvironmentState(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != EnvironmentStateBytes)
    {
        return false;
    }
    FEnvironmentState Next;
    Next.Scene = Reader.U32();
    Next.Dimension = static_cast<EDimension>(Reader.Byte());
    Next.Medium = static_cast<ECameraMedium>(Reader.Byte());
    Next.MoonPhase = Reader.Byte();
    Next.Flags = Reader.Byte();
    Next.SunAngle = Reader.Float();
    Next.GlobalRain = Reader.Float();
    Next.Thunder = Reader.Float();
    Next.LocalRain = Reader.Float();
    Next.LocalSnow = Reader.Float();
    Next.SkyFlash = Reader.Float();
    const uint32 SkyRgb = Reader.U32();
    Next.SkyColor = FColor(uint8(SkyRgb >> 16), uint8(SkyRgb >> 8), uint8(SkyRgb));
    Next.FogRgb.X = Reader.Float();
    Next.FogRgb.Y = Reader.Float();
    Next.FogRgb.Z = Reader.Float();
    Next.GameTimeSec = Reader.Float();

    const auto IsUnit = [](float Value) { return FMath::IsFinite(Value) && Value >= 0.0f && Value <= 1.0f; };
    if (Next.Dimension > EDimension::Other || Next.Medium > ECameraMedium::InLava || Next.MoonPhase > 7 ||
        (SkyRgb >> 24) != 0 || !(Next.SunAngle >= 0.0f && Next.SunAngle <= 2.0f * UE_PI) ||
        !IsUnit(Next.GlobalRain) || !IsUnit(Next.Thunder) || !IsUnit(Next.LocalRain) ||
        !IsUnit(Next.LocalSnow) || !IsUnit(Next.SkyFlash) ||
        !IsUnit(Next.FogRgb.X) || !IsUnit(Next.FogRgb.Y) || !IsUnit(Next.FogRgb.Z) ||
        !FMath::IsFinite(Next.GameTimeSec) || Next.GameTimeSec < 0.0f)
    {
        return false;
    }
    if (Next.Scene == SceneSerial && Height != 0)
    {
        PresentEnvironment(Next);
    }
    else if (Next.Scene > SceneSerial && (!PendingEnvironment || Next.Scene >= PendingEnvironment->Scene))
    {
        PendingEnvironment = Next;
    }
    return true;
}

bool ABridgeViewer::ApplyChunkPrecipitation(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != ChunkPrecipitationBytes)
    {
        return false;
    }
    const uint32 Scene = Reader.U32();
    const int32 ChunkX = Reader.I32();
    const int32 ChunkZ = Reader.I32();
    if (Scene != SceneSerial || Height == 0)
    {
        return true;
    }
    if (ChunkX < -MaxChunkCoordinate || ChunkX > MaxChunkCoordinate ||
        ChunkZ < -MaxChunkCoordinate || ChunkZ > MaxChunkCoordinate)
    {
        return false;
    }
    const FIntPoint Chunk(ChunkX, ChunkZ);
    TArray<uint16>& Cells = ChunkPrecipData.FindOrAdd(Chunk);
    Cells.SetNumUninitialized(256);
    for (int32 I = 0; I < 256; ++I)
    {
        const uint16 Cell = Reader.U16();
        Cells[I] = IsKnownPrecipColumn(Cell, Height) ? Cell : 0;
    }
    bTerrainWetHistoryDirty |= TerrainWetness.ReplaceChunk(Chunk, Cells);
    bPrecipGridDirty = true;
    return true;
}

bool ABridgeViewer::ApplyLightningStrike(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size != LightningStrikeBytes)
    {
        return false;
    }
    const uint32 Scene = Reader.U32();
    const uint32 EntityId = Reader.U32();
    const double X = Reader.Double();
    const double Y = Reader.Double();
    const double Z = Reader.Double();
    const uint32 Seed = Reader.U32();
    const float Remaining = Reader.Float();
    if (Scene != SceneSerial || Height == 0)
    {
        return true;
    }
    const FVector PositionMC(X, Y, Z);
    if (PositionMC.ContainsNaN() || PositionMC.GetAbsMax() > MaxBlockCoordinate ||
        !FMath::IsFinite(Remaining) || Remaining <= 0.0f || Remaining > 5.0f)
    {
        return false;
    }
    if (UMinecraftWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UMinecraftWeatherSubsystem>();
        Weather && !Weather->RecordLightning(Scene, EntityId, Seed, PositionMC, Remaining))
    {
        return true;
    }
    for (FLightningStrike& Existing : ActiveLightningStrikes)
    {
        if (Existing.EntityId == EntityId)
        {
            Existing.PositionMC = PositionMC;
            Existing.Seed = Seed;
            Existing.RemainingSeconds = Remaining;
            Existing.TotalSeconds = FMath::Max(0.12f, Remaining);
            return true;
        }
    }
    if (ActiveLightningStrikes.Num() >= 8)
    {
        ActiveLightningStrikes.RemoveAt(0);
    }
    ActiveLightningStrikes.Add({EntityId, PositionMC, Seed, Remaining, FMath::Max(0.12f, Remaining)});
    WeatherSim.SkyFlash = FMath::Max(WeatherSim.SkyFlash, 0.95f);
    return true;
}

void ABridgeViewer::PresentEnvironment(const BridgeWire::FEnvironmentState& State)
{
    if (Environment && Environment->Dimension != State.Dimension)
    {
        TerrainWetness.ResetMoisture();
        InvalidateTerrainWetHistory();
        ResetTerrainPuddleFootsteps();
    }
    const bool bFirstForScene = !WeatherSim.bInitialized || !Environment || Environment->Scene != State.Scene ||
        Environment->Dimension != State.Dimension;
    Environment = State;
    if (bFirstForScene)
    {
        WeatherSim.bInitialized = true;
        WeatherSim.Overcast = State.GlobalRain;
        WeatherSim.Thunder = State.Thunder;
        WeatherSim.LocalRain = State.LocalRain;
        WeatherSim.LocalSnow = State.LocalSnow;
        WeatherSim.WindStrength = FMath::Lerp(0.16f, 0.85f, FMath::Max(State.GlobalRain, State.Thunder));
        WeatherSim.SkyFlash = State.SkyFlash;
        WeatherSim.Clock = State.GameTimeSec;
        WeatherSim.LastGameTimeSec = State.GameTimeSec;
    }
    else
    {
        if (State.SkyFlash > WeatherSim.SkyFlash)
        {
            WeatherSim.SkyFlash = State.SkyFlash;
        }
        if (WeatherSim.LastGameTimeSec < 0.0f || FMath::Abs(State.GameTimeSec - WeatherSim.LastGameTimeSec) > 3.0f)
        {
            WeatherSim.Clock = State.GameTimeSec;
        }
        WeatherSim.LastGameTimeSec = State.GameTimeSec;
    }
    TickEnvironmentEffects(0.0f);
}

void ABridgeViewer::TickEnvironmentEffects(float DeltaSeconds)
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftEnvironment);
    if (!Environment)
    {
        return;
    }
    const float Dt = FMath::Clamp(DeltaSeconds, 0.0f, 0.1f);
    const auto& State = *Environment;
    SurfaceRainRate = AuthoritativeSurfaceRain(State);
    const bool bOverworld = State.Dimension == BridgeWire::EDimension::Overworld;
    const bool bWater = State.Medium == BridgeWire::ECameraMedium::Underwater;
    const bool bP2 = WaterOptics && FBridgeWaterOptics::IsEnabled();
    const bool bAir = State.Medium == BridgeWire::ECameraMedium::Other || (bP2 && bWater);
    const bool bHardware = BridgeQuality::HardwareLighting();
    const bool bHardwareGi = BridgeQuality::HardwareGiEnabled();
    const int32 Tier = BridgeQuality::PresetTier();

    // Advance deterministic atmosphere clock smoothly between MC ticks.
    if (Dt > 0.0f)
    {
        WeatherSim.Clock += Dt;
    }

    // Smooth global overcast/thunder & local biome rain/snow transitions (no abrupt pop across desert/snow borders!).
    const float TargetOvercast = bOverworld ? State.GlobalRain : 0.0f;
    const float TargetThunder = bOverworld ? State.Thunder : 0.0f;
    const float TargetLocalRain = bOverworld ? State.LocalRain : 0.0f;
    const float TargetLocalSnow = bOverworld ? State.LocalSnow : 0.0f;

    if (Dt > 0.0f)
    {
        WeatherSim.Overcast = FMath::FInterpTo(WeatherSim.Overcast, TargetOvercast, Dt, 0.55f);
        WeatherSim.Thunder = FMath::FInterpTo(WeatherSim.Thunder, TargetThunder, Dt, 0.55f);
        WeatherSim.LocalRain = FMath::FInterpTo(WeatherSim.LocalRain, TargetLocalRain, Dt, 1.35f);
        WeatherSim.LocalSnow = FMath::FInterpTo(WeatherSim.LocalSnow, TargetLocalSnow, Dt, 1.35f);

        const float TargetWind = FMath::Lerp(0.16f, 0.78f, WeatherSim.Overcast) + WeatherSim.Thunder * 0.22f;
        WeatherSim.WindStrength = FMath::FInterpTo(WeatherSim.WindStrength, FMath::Clamp(TargetWind, 0.0f, 1.0f), Dt, 0.6f);

        // Decay sky flash smoothly
        WeatherSim.SkyFlash = FMath::Max(State.SkyFlash, FMath::Max(0.0f, WeatherSim.SkyFlash - Dt * 4.5f));
    }

    const double Altitude = FMath::Cos(double(State.SunAngle));
    const FVector TowardSun(0.0, FMath::Sin(double(State.SunAngle)), Altitude);
    const FVector TowardMoon = -TowardSun;
    WeatherSim.SunDirection = FVector3f(TowardSun.GetSafeNormal());
    WeatherSim.MoonDirection = FVector3f(TowardMoon.GetSafeNormal());

    const float Overcast = WeatherSim.Overcast;
    const float Thunder = WeatherSim.Thunder;
#if !UE_BUILD_SHIPPING
    const bool bPreviewNative = CVarNativeCloudPreview.GetValueOnGameThread() != 0;
    if (CloudMaterial && bPreviewNative != bNativeCloudPreviewRequested)
    {
        bNativeCloudPreviewRequested = bPreviewNative;
        UMaterialInterface* Selected = CloudMaterial;
        if (bPreviewNative)
        {
            UMaterialInterface* Native = LoadObject<UMaterialInterface>(nullptr,
                TEXT("/Engine/EngineSky/VolumetricClouds/m_SimpleVolumetricCloud_Inst.m_SimpleVolumetricCloud_Inst"));
            if (Native)
            {
                Native->EnsureIsComplete();
                if (Native->IsComplete())
                {
                    Selected = Native;
                }
            }
            UE_CLOG(Selected == CloudMaterial.Get(), LogMinecraftBridgeEnvironment, Warning,
                TEXT("Single-layer preview unavailable. Using the layered Epic preset."));
        }
        TerrainClouds->SetMaterial(Selected);
        TerrainClouds->SetLayerHeight(Selected != CloudMaterial.Get() ? 4.4f : WeatherCloudLayerHeightKm);
        UE_LOG(LogMinecraftBridgeEnvironment, Display, TEXT("Cloud material preview applied: %s"), *GetPathNameSafe(Selected));
    }
#endif
    const bool bShowVolClouds = bOverworld && TerrainClouds->GetMaterial() != nullptr && BridgeQuality::CloudSamples() > 0.0f &&
        (Tier > 0 || BridgeQuality::HasCustomCloudScale());

    TerrainAtmosphere->SetVisibility(bOverworld);
    TerrainSkyDome->SetVisibility(bOverworld && SkyDomeMaterial != nullptr);
    if (Camera && bOverworld)
    {
        TerrainSkyDome->SetWorldLocation(Camera->GetActorLocation());
    }
    // Software Lumen has no mesh distance fields to occlude a captured skylight inside caves.
    TerrainSkyLight->SetVisibility(bOverworld && bHardwareGi);
    TerrainClouds->SetVisibility(bShowVolClouds);
    TerrainSun->SetVisibility(bOverworld && Altitude > -0.16);
    TerrainMoon->SetVisibility(bOverworld && Altitude < 0.14);

    if (bOverworld)
    {
        const float ContactLength = FMath::Max(0.0f, CVarContactShadowLength.GetValueOnGameThread());
        const float ContactIntensity = FMath::Clamp(CVarContactShadowIntensity.GetValueOnGameThread(), 0.0f, 1.0f);
        for (UDirectionalLightComponent* Light : {TerrainSun.Get(), TerrainMoon.Get()})
        {
            if (!FMath::IsNearlyEqual(Light->ContactShadowLength, ContactLength)
                || !FMath::IsNearlyEqual(Light->ContactShadowCastingIntensity, ContactIntensity))
            {
                Light->ContactShadowLength = ContactLength;
                Light->ContactShadowCastingIntensity = ContactIntensity;
                Light->MarkRenderStateDirty();
            }
        }
        FVector LightDirection = TowardSun;
        const float Step = FMath::Clamp(CVarVsmLightAngleStep.GetValueOnGameThread(), 0.0f, 0.5f);
        if (Step > 0.0f && BridgeQuality::DirectionalRayShadows() != 1)
        {
            // Rotating a directional light invalidates its entire VSM clipmap. Use a finer
            // grid near the horizon where tall casters have long, visibly moving shadows.
            const float Degrees = FMath::Abs(Altitude) < 0.25 ? Step * 0.25f : Step;
            const double Angle = FMath::GridSnap(double(State.SunAngle), FMath::DegreesToRadians(double(Degrees)));
            LightDirection = FVector(0.0, FMath::Sin(Angle), FMath::Cos(Angle));
        }
        TerrainSun->SetRelativeRotation((-LightDirection).Rotation());
        TerrainMoon->SetRelativeRotation(LightDirection.Rotation());

        // Calibrated physical sunlight (~6000 lux noon -> EV 11.3): compresses the day/night/interior
        // dynamic range to ~9 stops so exteriors, interiors, torches and nights stay balanced without blowouts.
        const float SunElevCurve = FMath::SmoothStep(-0.14f, 0.14f, float(Altitude));
        const float SunWeatherAtten = FMath::Lerp(1.0f, 0.22f, Overcast) * FMath::Lerp(1.0f, 0.65f, Thunder);
        TerrainSun->SetIntensity(6000.0f * SunElevCurve * SunWeatherAtten);

        // Warm low-angle Rayleigh glow amplification for vivid golden-hour cloud rims & lake reflections.
        const float SunsetFactor = FMath::Exp(-FMath::Square(float(Altitude) - 0.07f) * 42.0f) * (1.0f - Overcast * 0.75f);
        TerrainSun->SetLightColor(FMath::Lerp(FLinearColor(1.0f, 0.985f, 0.96f), FLinearColor(1.0f, 0.72f, 0.44f), SunsetFactor * 0.58f));

        // Slightly boost volumetric god-ray scattering around golden hour and broken clouds.
        const float GoldenHour = FMath::Exp(-FMath::Square(float(Altitude) - 0.10f) * 32.0f);
        TerrainSun->SetVolumetricScatteringIntensity(
            FMath::Lerp(1.35f, 2.75f, GoldenHour) * FMath::Lerp(1.0f, 0.40f, Overcast));

        // Calibrated moonlight (~24 lux full moon -> EV 3.5, ~14 lux new moon -> EV 2.8) so clear and
        // rainy nights remain legible and harmonize with block point lights without a 17-stop cliff.
        const float PhaseIllum = 0.58f + 0.42f * (0.5f + 0.5f * FMath::Cos(float(State.MoonPhase) * (UE_TWO_PI / 8.0f)));
        const float MoonElevCurve = FMath::SmoothStep(-0.10f, 0.22f, -float(Altitude));
        TerrainMoon->SetIntensity(24.0f * PhaseIllum * MoonElevCurve * FMath::Lerp(1.0f, 0.52f, Overcast));

        // Physical atmosphere parameters react naturally to humidity/rain without fighting SkyAtmosphere!
        TerrainAtmosphere->SetMieScatteringScale(FMath::Lerp(0.0056f, 0.0145f, Overcast) + Thunder * 0.0035f);
        TerrainAtmosphere->SetMieAnisotropy(FMath::Lerp(0.84f, 0.68f, Overcast));
        TerrainAtmosphere->SetAerialPespectiveViewDistanceScale(FMath::Lerp(1.12f, 1.75f, Overcast));

        const FLinearColor BiomeSky = FLinearColor::FromSRGBColor(State.SkyColor);
        const FLinearColor SubtleTint = FMath::Lerp(FLinearColor::White, BiomeSky, 0.08f);
        const float SkyWeatherDim = FMath::Lerp(1.0f, 0.62f, Overcast) * FMath::Lerp(1.0f, 0.72f, Thunder);
        const float FlashBoost = 1.0f + WeatherSim.SkyFlash * 0.85f;
        TerrainAtmosphere->SetSkyLuminanceFactor(SubtleTint * (SkyWeatherDim * FlashBoost));

        // Keep SkyLight cloud AO strong when fair/broken clouds are overhead, softened under solid overcast.
        TerrainSkyLight->CloudAmbientOcclusionStrength = FMath::Lerp(0.55f, 0.30f, Overcast);
    }

    // Physically coupled ExponentialHeightFog + VolumetricFog:
    // Let SkyAtmosphere paint the overworld fog (warm sunward, cool anti-solar, deep night) instead of flat white!
    UpdatePrecipGridTexture();
    UpdateTerrainWetHistory(Dt, float(Altitude));
    UpdateTerrainPuddleFootsteps(Dt, TerrainPuddleMotionEnabled(Tier));
    UpdateWaterGrid();
    if (WaterOptics && Camera && CameraFrames > 0)
    {
        const FIntPoint Column(FMath::FloorToInt32(CameraPositionMC.X), FMath::FloorToInt32(CameraPositionMC.Z));
        const float Daylight = bOverworld ? FMath::SmoothStep(-0.18f, 0.65f, float(Altitude)) : 0.0f;
        const float Sky = bOverworld ? SampleWaterSkyExposure() : 0.0f;
        const float WaterAmbient = 0.34f + (6.0f + 180.0f * Daylight) * Sky * FMath::Lerp(1.0f, 0.34f, Overcast);
        const FVector3f Ambient = FVector3f(0.56f, 0.82f, 1.0f) * WaterAmbient;
        WaterOptics->UpdateCamera(Camera->GetActorLocation(), bWater, WaterColumn(Column), MinY,
            Dt, Ambient, WeatherSim.Clock, Tier);
        FBridgeWaterSunInputs Sun;
        Sun.Eye = Camera->GetActorLocation();
        Sun.GridOrigin = BridgeWire::ToUnreal(WaterGridOrigin.X, MinY, WaterGridOrigin.Y) * BridgeWire::BlockSize;
        Sun.GridRevision = WaterGridRevision;
        Sun.Direction = WeatherSim.SunDirection;
        const FLinearColor SunColor = TerrainSun->GetLightColor();
        Sun.Radiance = FVector3f(SunColor.R, SunColor.G, SunColor.B) * (TerrainSun->Intensity * 0.145f);
        Sun.GridTexture = bWaterGridReady ? WaterGridTexture.Get() : nullptr;
        Sun.Grid = Sun.GridTexture ? Sun.GridTexture->GetResource() : nullptr;
        Sun.CausticTexture = WaterSim ? WaterSim->GetCausticTexture() : nullptr;
        Sun.CausticPattern = Sun.CausticTexture ? Sun.CausticTexture->GetResource() : nullptr;
        Sun.Light = bOverworld ? TerrainSun.Get() : nullptr;
        Sun.Tier = Tier;
        Sun.bNearWater = WaterOptics->IsEyeWet();
        if (bWaterGridReady && !Sun.bNearWater && Tier > 0)
        {
            for (int32 Z = -4; Z <= 4 && !Sun.bNearWater; ++Z)
            {
                for (int32 X = -4; X <= 4; ++X)
                {
                    if ((WaterColumn(Column + FIntPoint(X, Z) * 4) & (3u << 26)) == (3u << 26))
                    {
                        Sun.bNearWater = true;
                        break;
                    }
                }
            }
        }
        if (Tier > 0 && WaterOptics->IsEyeWet())
        {
            struct FCandidate { FBridgeWaterLocalLight Optical; float Distance; };
            TArray<FCandidate, TInlineAllocator<4>> Nearby;
            for (const auto& Pair : ActivePointLights)
            {
                const ULocalLightComponent* Light = PointLightPool[Pair.Value];
                const FVector Location = Light->GetComponentLocation();
                const float Squared = FVector::DistSquared(Location, Sun.Eye);
                if (Squared >= FMath::Square(FMath::Min(1400.0f, Light->AttenuationRadius)) ||
                    (Nearby.Num() == 4 && Squared >= Nearby.Last().Distance)) continue;
                const FLinearColor Color = Light->GetLightColor();
                const float Power = FMath::Min(240.0f, Light->Intensity * 0.45f);
                Nearby.Add({{Location, FVector3f(Color.R, Color.G, Color.B) * Power,
                    Light->AttenuationRadius * 0.01f}, Squared});
                Nearby.Sort([](const FCandidate& A, const FCandidate& B) { return A.Distance < B.Distance; });
                if (Nearby.Num() > 4) Nearby.Pop(EAllowShrinking::No);
            }
            for (const FCandidate& Candidate : Nearby) Sun.LocalLights.Add(Candidate.Optical);
        }
        WaterOptics->UpdateLighting(Sun);
        if (WaterParticles)
        {
            WaterParticles->Tick(Dt, Sun.Eye, WaterOptics->IsEyeWet(), Tier, [this](const FVector& P)
            {
                const uint32 Sample = WaterColumn(FIntPoint(FMath::FloorToInt32(-P.Y * 0.01), FMath::FloorToInt32(P.X * 0.01)));
                if ((Sample & (3u << 26)) != (3u << 26)) return false;
                const float Top = float(MinY + int32(Sample & 2047u)) + ((Sample >> 11) & 63u) / 64.0f;
                const float Bottom = (Sample & (1u << 31)) != 0 ? float(MinY) : FMath::CeilToFloat(Top) - float((Sample >> 17) & 31u);
                const float HeightM = P.Z * 0.01;
                return HeightM > Bottom + 0.32f && HeightM < Top - 0.38f;
            });
        }
    }
    const float WaterWeight = bP2 ? WaterOptics->GetExposureWeight() : 0.0f;
    const FLinearColor McFogColor(State.FogRgb.X, State.FogRgb.Y, State.FogRgb.Z);
    if (bOverworld && bAir)
    {
        const float EffectiveViewCm = float(FMath::Max(6, int32(ViewDistance))) * 1600.0f;
        const float ClearStart = FMath::Clamp(EffectiveViewCm * 0.24f, 2400.0f, 11000.0f);
        const float RainStart = FMath::Clamp(EffectiveViewCm * 0.08f, 900.0f, 2600.0f);
        const float AirFogStart = FMath::Lerp(ClearStart, RainStart, Overcast);

        // Near-black inscattering base so SkyAtmosphere's HeightFogContribution=1.0 owns 100% of directional fog color.
        TerrainFog->SetFogInscatteringColor(FLinearColor(0.0006f, 0.0009f, 0.0014f));
        const float GoldenHalo = FMath::SmoothStep(-0.10f, 0.08f, float(Altitude)) *
            (1.0f - FMath::SmoothStep(0.18f, 0.55f, float(Altitude))) * (1.0f - Overcast * 0.85f);
        TerrainFog->SetDirectionalInscatteringColor(FLinearColor(0.28f, 0.12f, 0.045f) * GoldenHalo);
        TerrainFog->SetDirectionalInscatteringExponent(5.5f);
        TerrainFog->SetDirectionalInscatteringStartDistance(AirFogStart * 0.8f);

        const float FogMul = BridgeQuality::FogDensityScale();
        const float ClearDensity = FMath::Clamp(0.0019f * (12.0f / FMath::Max(6.0f, float(ViewDistance))), 0.0010f, 0.0030f);
        const float RainDensity = 0.0125f + Thunder * 0.0042f;
        TerrainFog->SetFogDensity(FMath::Lerp(ClearDensity, RainDensity, Overcast) * FogMul);
        TerrainFog->SetFogHeightFalloff(FMath::Lerp(0.088f, 0.036f, Overcast));
        TerrainFog->SetStartDistance(AirFogStart);
        TerrainFog->SetFogMaxOpacity(FMath::Lerp(0.75f, 0.90f, Overcast) * FMath::Clamp(FogMul, 0.0f, 1.0f));

        static const IConsoleVariable* VolumetricFog = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VolumetricFog"));
        const bool bLocalMist = BridgeQuality::WaterP3Enabled(Tier) && CVarWaterP3.GetValueOnGameThread() != 0 && !NearbyWaterfalls.IsEmpty();
        const bool bUseVolFog = (bHardware || bLocalMist) && VolumetricFog && VolumetricFog->GetBool();
        TerrainFog->SetVolumetricFog(bUseVolFog);
        TerrainFog->SetVolumetricFogDistance(FMath::Lerp(22000.0f, 15000.0f, Overcast));
        TerrainFog->SetVolumetricFogExtinctionScale((FMath::Lerp(0.38f, 1.25f, Overcast) + Thunder * 0.25f) * FogMul);
        TerrainFog->SetVolumetricFogScatteringDistribution(FMath::Lerp(0.62f, 0.34f, Overcast));
    }
    else
    {
        const float Daylight = bOverworld ? FMath::Max(0.0f, float(Altitude)) : 0.0f;
        float FogScale = 1.2f;
        if (!bAir)
        {
            // Scale underwater fog with surface daylight so diving/surfacing never causes an 8-stop blackout or flashbang.
            FogScale = bWater
                ? FMath::Lerp(2.2f, 180.0f, FMath::Pow(Daylight, 0.75f)) * FMath::Lerp(1.0f, 0.40f, Overcast)
                : 26.0f;
        }
        else if (State.Dimension == BridgeWire::EDimension::Nether)
        {
            FogScale = 1.4f;
        }
        else if (State.Dimension == BridgeWire::EDimension::End)
        {
            FogScale = 1.2f;
        }
        const float DimFogMul = bAir ? FMath::Clamp(BridgeQuality::FogDensityScale(), 0.15f, 2.0f) : 1.0f;
        TerrainFog->SetFogInscatteringColor(McFogColor * FogScale);
        TerrainFog->SetDirectionalInscatteringColor(FLinearColor::Black);
        TerrainFog->SetFogDensity((bAir ? 0.045f : bWater ? 0.78f : 7.0f) * DimFogMul);
        TerrainFog->SetFogHeightFalloff(0.001f);
        TerrainFog->SetStartDistance(bAir ? 470.0f : 0.0f);
        TerrainFog->SetFogMaxOpacity(1.0f);
        TerrainFog->SetVolumetricFog(false);
    }
    TerrainFog->SetVisibility(true);

    // Compute analytic environment baseline EV100 (calibrated to actual SkyAtmosphere + terrain luminance)
    // to anchor auto-exposure bounds and drive fixed exposure mode seamlessly.
    float EnvBaseEV = 3.8f;
    if (bOverworld)
    {
        const float DayCurve = FMath::SmoothStep(-0.14f, 0.16f, float(Altitude));
        const float HighSunBoost = FMath::SmoothStep(0.15f, 0.75f, float(Altitude));
        const float ClearEV = FMath::Lerp(3.0f, 8.5f, DayCurve) + 1.8f * HighSunBoost;
        const float WeatherDrop = Overcast * 1.5f * DayCurve + Thunder * 0.45f * DayCurve
            + Overcast * 0.45f * (1.0f - DayCurve);
        EnvBaseEV = ClearEV - WeatherDrop;
        if (bP2) EnvBaseEV = FMath::Lerp(EnvBaseEV, FMath::Max(2.5f, EnvBaseEV - 1.7f * DayCurve), WaterWeight);
        if (!bAir)
        {
            EnvBaseEV = bWater ? FMath::Max(2.5f, EnvBaseEV - 1.7f * DayCurve) : 6.4f;
        }
    }
    else if (State.Dimension == BridgeWire::EDimension::Nether)
    {
        EnvBaseEV = bAir ? 4.0f : 6.4f;
    }
    else
    {
        EnvBaseEV = 3.8f;
    }

    FPostProcessSettings& Post = Camera->GetCameraComponent()->PostProcessSettings;
    Post.bOverride_SceneColorTint = !bAir;
    Post.SceneColorTint = bWater ? FLinearColor(0.74f, 0.92f, 1.0f) : FLinearColor(1.0f, 0.62f, 0.31f);

    UpdateExposureSettings(Dt, EnvBaseEV, bOverworld, bAir, !WeatherSim.bEnvEvInitialized, WaterWeight);
    TickLightning(Dt);
    TickPrecipitation(Dt);
    UpdateTerrainLightingParameters();
    PushEnvironmentMaterialParameters(Dt);
}

void ABridgeViewer::UpdatePrecipGridTexture()
{
    if (!PrecipGridTexture || CameraFrames == 0)
    {
        return;
    }
    if (ChunkPrecipData.IsEmpty())
    {
        InvalidatePrecipGrid();
        return;
    }
    const int32 CenterChunkX = FMath::FloorToInt(CameraPositionMC.X / 16.0);
    const int32 CenterChunkZ = FMath::FloorToInt(CameraPositionMC.Z / 16.0);
    const FIntPoint DesiredOrigin(CenterChunkX - PrecipGridHalfChunks, CenterChunkZ - PrecipGridHalfChunks);
    if (!bPrecipGridDirty && DesiredOrigin == PrecipGridOriginChunk)
    {
        return;
    }
    bTerrainWetHistoryDirty |= !bPrecipGridReady || DesiredOrigin != PrecipGridOriginChunk;
    PrecipGridOriginChunk = DesiredOrigin;
    bPrecipGridDirty = false;
    bPrecipGridReady = true;

    TArray<FFloat16Color> Pixels;
    Pixels.SetNumZeroed(PrecipGridSize * PrecipGridSize);
    FFloat16Color* Dest = Pixels.GetData();

    for (int32 CzOff = 0; CzOff < PrecipGridHalfChunks * 2; ++CzOff)
    {
        const int32 ChunkZ = DesiredOrigin.Y + CzOff;
        for (int32 CxOff = 0; CxOff < PrecipGridHalfChunks * 2; ++CxOff)
        {
            const int32 ChunkX = DesiredOrigin.X + CxOff;
            const TArray<uint16>* Cells = ChunkPrecipData.Find(FIntPoint(ChunkX, ChunkZ));
            if (!Cells || Cells->Num() != 256)
            {
                continue;
            }
            const uint16* Src = Cells->GetData();
            for (int32 Lz = 0; Lz < 16; ++Lz)
            {
                const int32 Py = CzOff * 16 + Lz;
                FFloat16Color* Row = Dest + Py * PrecipGridSize + CxOff * 16;
                for (int32 Lx = 0; Lx < 16; ++Lx)
                {
                    const uint16 Cell = Src[Lz * 16 + Lx];
                    if (Cell == 0)
                    {
                        continue;
                    }
                    const int32 RelY = int32(Cell & BridgeWire::PrecipHeightMask);
                    const uint8 Type = BridgeWire::PrecipType(Cell);
                    const float TopZCm = float(MinY + RelY) * float(BridgeWire::BlockSize);
                    if (FMath::Abs(TopZCm) <= 65504.0f)
                    {
                        Row[Lx] = FFloat16Color(FLinearColor(TopZCm, 0.5f * float(Type), 1.0f, 1.0f));
                    }
                }
            }
        }
    }

    UploadPrecipitationTexture(*PrecipGridTexture, MoveTemp(Pixels));
}

void ABridgeViewer::InvalidatePrecipGrid()
{
    if (bPrecipGridReady && PrecipGridTexture)
    {
        // Existing material instances may retain the texture, so clear the old columns as well as invalidating its metadata.
        TArray<FFloat16Color> Pixels;
        Pixels.SetNumZeroed(PrecipGridSize * PrecipGridSize);
        UploadPrecipitationTexture(*PrecipGridTexture, MoveTemp(Pixels));
    }
    InvalidateTerrainWetHistory();
    PrecipGridOriginChunk = FIntPoint(INT32_MIN, INT32_MIN);
    bPrecipGridDirty = false;
    bPrecipGridReady = false;
}

void ABridgeViewer::DropPrecipitationChunk(FIntPoint Chunk)
{
    bTerrainWetHistoryDirty |= TerrainWetness.DropChunk(Chunk);
    if (ChunkPrecipData.Remove(Chunk))
    {
        if (ChunkPrecipData.IsEmpty())
        {
            InvalidatePrecipGrid();
        }
        else
        {
            bPrecipGridDirty = true;
        }
    }
}

uint16 ABridgeViewer::GetPrecipColumnCode(int32 BlockX, int32 BlockZ) const
{
    const FIntPoint Chunk(BlockX >> 4, BlockZ >> 4);
    const TArray<uint16>* Cells = ChunkPrecipData.Find(Chunk);
    return Cells ? (*Cells)[(BlockZ & 15) * 16 + (BlockX & 15)] : 0;
}

bool ABridgeViewer::SamplePrecipColumn(int32 BlockX, int32 BlockZ, int32& OutTopY, uint8& OutType) const
{
    const uint16 Cell = GetPrecipColumnCode(BlockX, BlockZ);
    if (Cell == 0)
    {
        return false;
    }
    OutTopY = MinY + int32(Cell & BridgeWire::PrecipHeightMask);
    OutType = BridgeWire::PrecipType(Cell);
    return true;
}

void ABridgeViewer::TickPrecipitation(float DeltaSeconds)
{
    if (!Environment || Environment->Dimension != BridgeWire::EDimension::Overworld ||
        Environment->Medium != BridgeWire::ECameraMedium::Other || CameraFrames == 0 || !Camera)
    {
        RainMesh->SetVisibility(false);
        SnowMesh->SetVisibility(false);
        return;
    }

    // Drive precipitation from global rain when grid data exists so biome boundaries are visible from a distance!
    const float GlobalPrecip = WeatherSim.Overcast;
    const bool bHasGrid = bPrecipGridReady && !ChunkPrecipData.IsEmpty();
    const bool bWantRain = (bHasGrid ? GlobalPrecip > 0.04f : WeatherSim.LocalRain > 0.04f) && RainMaterial != nullptr;
    const bool bWantSnow = (bHasGrid ? GlobalPrecip > 0.04f : WeatherSim.LocalSnow > 0.04f) && SnowMaterial != nullptr;
    if (!bWantRain && !bWantSnow)
    {
        RainMesh->SetVisibility(false);
        SnowMesh->SetVisibility(false);
        return;
    }

    const float PrecipScale = BridgeQuality::PrecipDensityScale();
    if (PrecipScale <= 0.001f)
    {
        RainMesh->SetVisibility(false);
        SnowMesh->SetVisibility(false);
        return;
    }
    const int32 Tier = BridgeQuality::PresetTier();
    static constexpr int32 TierRainCounts[] = {360, 750, 1250, 1950};
    static constexpr int32 TierSnowCounts[] = {240, 520, 880, 1350};
    const int32 MaxRain = FMath::Min(RainParticles.Num(), FMath::RoundToInt(TierRainCounts[FMath::Clamp(Tier, 0, 3)] * PrecipScale));
    const int32 MaxSnow = FMath::Min(SnowParticles.Num(), FMath::RoundToInt(TierSnowCounts[FMath::Clamp(Tier, 0, 3)] * PrecipScale));

    const FVector CamUE = Camera->GetActorLocation();
    const FVector CamFwd = Camera->GetActorForwardVector();
    const FVector CamRight = Camera->GetActorRightVector();
    const FVector CamUp = Camera->GetActorUpVector();
    const double SunDay = FMath::Clamp(FMath::Cos(double(Environment->SunAngle)), 0.0, 1.0);
    // Match precipitation emissive luminance to current sky EV so streaks stay bright silver in day storms
    // and softly moonlit at night (UProceduralMeshComponent clamps vertex color to [0,1], so luminance is a material scalar).
    const float PrecipLum = FMath::Lerp(2.4f, 180.0f, FMath::Pow(float(SunDay), 0.72f) * FMath::Lerp(1.0f, 0.42f, WeatherSim.Overcast))
        * (1.0f + WeatherSim.SkyFlash * 1.25f);
    if (RainMaterial)
    {
        RainMaterial->SetScalarParameterValue(TEXT("PrecipLuminance"), PrecipLum * 0.85f);
    }
    if (SnowMaterial)
    {
        SnowMaterial->SetScalarParameterValue(TEXT("PrecipLuminance"), PrecipLum * 1.15f);
    }
    const float LightMul = 1.0f;

    const auto WrapBox = [](FVector& Offset, float XYRadius, float ZMin, float ZMax)
    {
        const float SpanZ = ZMax - ZMin;
        while (Offset.Z < ZMin) Offset.Z += SpanZ;
        while (Offset.Z > ZMax) Offset.Z -= SpanZ;
        if (Offset.X * Offset.X + Offset.Y * Offset.Y > XYRadius * XYRadius)
        {
            Offset.X = -Offset.X * 0.92;
            Offset.Y = -Offset.Y * 0.92;
        }
    };

    // --- Build roof-occluded, biome-aware Rain Streaks ---
    if (bWantRain)
    {
        const float Intensity = bHasGrid ? GlobalPrecip : WeatherSim.LocalRain;
        const int32 ActiveCount = FMath::Clamp(FMath::RoundToInt(MaxRain * Intensity), 0, MaxRain);
        const FVector WindDriftUE(180.0 * WeatherSim.WindStrength, 90.0 * WeatherSim.WindStrength, -2100.0);
        TArray<FVector> Verts;
        TArray<int32> Tris;
        TArray<FVector2D> UVs;
        TArray<FLinearColor> Colors;
        Verts.Reserve(ActiveCount * 4);
        Tris.Reserve(ActiveCount * 6);
        UVs.Reserve(ActiveCount * 4);
        Colors.Reserve(ActiveCount * 4);

        for (int32 I = 0; I < ActiveCount; ++I)
        {
            FPrecipParticle& P = RainParticles[I];
            P.OffsetUE += WindDriftUE * (P.SpeedScale * DeltaSeconds);
            WrapBox(P.OffsetUE, 2600.0f, -900.0f, 1900.0f);

            const FVector WorldPos = CamUE + P.OffsetUE;
            // Frustum cull behind camera for fast CPU generation
            if (FVector::DotProduct(P.OffsetUE, CamFwd) < -150.0)
            {
                continue;
            }
            const int32 McX = FMath::FloorToInt(-WorldPos.Y * 0.01);
            const int32 McZ = FMath::FloorToInt(WorldPos.X * 0.01);
            int32 TopY = MinY;
            uint8 BiomeType = WeatherSim.LocalRain > 0.04f ? PrecipRain : PrecipNone;
            if (SamplePrecipColumn(McX, McZ, TopY, BiomeType))
            {
                if (BiomeType != PrecipRain)
                {
                    continue; // Dry in deserts, snowing in snow biomes!
                }
                const double RoofZ = double(TopY) * BridgeWire::BlockSize;
                if (WorldPos.Z < RoofZ + 30.0)
                {
                    continue; // Stopped by roofs, glass ceilings, eaves & trees!
                }
            }
            else if (WeatherSim.LocalRain <= 0.04f)
            {
                continue;
            }

            const FVector Local = P.OffsetUE;
            const double Dist = Local.Size();
            const FVector StreakDir = WindDriftUE.GetSafeNormal();
            const FVector Side = FVector::CrossProduct(StreakDir, (WorldPos - CamUE).GetSafeNormal()).GetSafeNormal(0.001, CamRight);
            // Distance-compensated streak thickness keeps rain crisp under TSR without subpixel dropout.
            const double HalfW = FMath::Clamp(1.35 + Dist * 0.0009, 1.35, 3.2);
            const double HalfL = 56.0 * P.SpeedScale;
            const int32 Base = Verts.Num();
            Verts.Append({
                Local - Side * HalfW - StreakDir * HalfL,
                Local + Side * HalfW - StreakDir * HalfL,
                Local + Side * HalfW + StreakDir * HalfL,
                Local - Side * HalfW + StreakDir * HalfL
            });
            UVs.Append({FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1), FVector2D(0, 1)});
            const float DistFade = 1.0f - FMath::SmoothStep(1500.0f, 2600.0f, float(Dist));
            const float StreakBright = 0.95f * LightMul;
            const FLinearColor Col(0.80f * StreakBright, 0.88f * StreakBright, 0.98f * StreakBright, 0.62f * Intensity * DistFade);
            Colors.Append({Col, Col, Col, Col});
            Tris.Append({Base, Base + 1, Base + 2, Base, Base + 2, Base + 3});
        }
        if (Verts.Num() > 0)
        {
            RainMesh->SetWorldLocation(CamUE);
            RainMesh->CreateMeshSection_LinearColor(0, Verts, Tris, {}, UVs, Colors, {}, false);
            RainMesh->SetVisibility(true);
        }
        else
        {
            RainMesh->SetVisibility(false);
        }
    }
    else
    {
        RainMesh->SetVisibility(false);
    }

    // --- Build roof-occluded, biome-aware Drifting Snowflakes ---
    if (bWantSnow)
    {
        const float Intensity = bHasGrid ? GlobalPrecip : WeatherSim.LocalSnow;
        const int32 ActiveCount = FMath::Clamp(FMath::RoundToInt(MaxSnow * Intensity), 0, MaxSnow);
        TArray<FVector> Verts;
        TArray<int32> Tris;
        TArray<FVector2D> UVs;
        TArray<FLinearColor> Colors;
        Verts.Reserve(ActiveCount * 4);
        Tris.Reserve(ActiveCount * 6);
        UVs.Reserve(ActiveCount * 4);
        Colors.Reserve(ActiveCount * 4);

        for (int32 I = 0; I < ActiveCount; ++I)
        {
            FPrecipParticle& P = SnowParticles[I];
            const float FlutterX = FMath::Sin(WeatherSim.Clock * 1.4f + P.Phase) * 55.0f;
            const float FlutterY = FMath::Cos(WeatherSim.Clock * 1.1f + P.Phase * 1.3f) * 55.0f;
            P.OffsetUE += FVector(FlutterX + 65.0 * WeatherSim.WindStrength, FlutterY + 35.0 * WeatherSim.WindStrength, -175.0 * P.SpeedScale) * DeltaSeconds;
            WrapBox(P.OffsetUE, 3000.0f, -850.0f, 1750.0f);

            const FVector WorldPos = CamUE + P.OffsetUE;
            if (FVector::DotProduct(P.OffsetUE, CamFwd) < -150.0)
            {
                continue;
            }
            const int32 McX = FMath::FloorToInt(-WorldPos.Y * 0.01);
            const int32 McZ = FMath::FloorToInt(WorldPos.X * 0.01);
            int32 TopY = MinY;
            uint8 BiomeType = WeatherSim.LocalSnow > 0.04f ? PrecipSnow : PrecipNone;
            if (SamplePrecipColumn(McX, McZ, TopY, BiomeType))
            {
                if (BiomeType != PrecipSnow)
                {
                    continue;
                }
                const double RoofZ = double(TopY) * BridgeWire::BlockSize;
                if (WorldPos.Z < RoofZ + 8.0)
                {
                    continue;
                }
            }
            else if (WeatherSim.LocalSnow <= 0.04f)
            {
                continue;
            }

            const FVector Local = P.OffsetUE;
            const double HalfS = 3.6 + 2.4 * P.SpeedScale;
            const int32 Base = Verts.Num();
            Verts.Append({
                Local - CamRight * HalfS - CamUp * HalfS,
                Local + CamRight * HalfS - CamUp * HalfS,
                Local + CamRight * HalfS + CamUp * HalfS,
                Local - CamRight * HalfS + CamUp * HalfS
            });
            UVs.Append({FVector2D(0, 0), FVector2D(1, 0), FVector2D(1, 1), FVector2D(0, 1)});
            const float DistFade = 1.0f - FMath::SmoothStep(1800.0f, 3000.0f, float(P.OffsetUE.Size()));
            const FLinearColor Col(0.95f * LightMul, 0.97f * LightMul, 1.0f * LightMul, 0.85f * Intensity * DistFade);
            Colors.Append({Col, Col, Col, Col});
            Tris.Append({Base, Base + 1, Base + 2, Base, Base + 2, Base + 3});
        }
        if (Verts.Num() > 0)
        {
            SnowMesh->SetWorldLocation(CamUE);
            SnowMesh->CreateMeshSection_LinearColor(0, Verts, Tris, {}, UVs, Colors, {}, false);
            SnowMesh->SetVisibility(true);
        }
        else
        {
            SnowMesh->SetVisibility(false);
        }
    }
    else
    {
        SnowMesh->SetVisibility(false);
    }
}

void ABridgeViewer::TickLightning(float DeltaSeconds)
{
    if (ActiveLightningStrikes.IsEmpty() || !LightningMaterial || !Camera)
    {
        LightningMesh->SetVisibility(false);
        LightningLight->SetVisibility(false);
        WeatherSim.LightningPosIntensity = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    for (int32 I = ActiveLightningStrikes.Num() - 1; I >= 0; --I)
    {
        ActiveLightningStrikes[I].RemainingSeconds -= DeltaSeconds;
        if (ActiveLightningStrikes[I].RemainingSeconds <= 0.0f)
        {
            ActiveLightningStrikes.RemoveAtSwap(I);
        }
    }
    if (ActiveLightningStrikes.IsEmpty())
    {
        LightningMesh->SetVisibility(false);
        LightningLight->SetVisibility(false);
        WeatherSim.LightningPosIntensity = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const FVector CamUE = Camera->GetActorLocation();
    TArray<FVector> Verts;
    TArray<int32> Tris;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> Colors;
    FVector AnchorUE = BridgeWire::ToUnreal(ActiveLightningStrikes[0].PositionMC.X,
        ActiveLightningStrikes[0].PositionMC.Y, ActiveLightningStrikes[0].PositionMC.Z) * BridgeWire::BlockSize;

    auto EmitRibbon = [&](const TArray<FVector>& Points, float HalfWidthCm, float Alpha)
    {
        for (int32 S = 0; S + 1 < Points.Num(); ++S)
        {
            const FVector A = Points[S];
            const FVector B = Points[S + 1];
            const FVector Seg = (B - A).GetSafeNormal();
            const FVector ToCam = (CamUE - (AnchorUE + (A + B) * 0.5)).GetSafeNormal();
            const FVector Side = FVector::CrossProduct(Seg, ToCam).GetSafeNormal(0.001, FVector::RightVector);
            const float T0 = float(S) / float(Points.Num() - 1);
            const float T1 = float(S + 1) / float(Points.Num() - 1);
            const float W0 = HalfWidthCm * FMath::Lerp(0.75f, 1.35f, T0);
            const float W1 = HalfWidthCm * FMath::Lerp(0.75f, 1.35f, T1);
            const int32 Base = Verts.Num();
            Verts.Append({A - Side * W0, A + Side * W0, B + Side * W1, B - Side * W1});
            UVs.Append({FVector2D(0, T0), FVector2D(1, T0), FVector2D(1, T1), FVector2D(0, T1)});
            const FLinearColor C(1.0f, 1.0f, 1.0f, Alpha);
            Colors.Append({C, C, C, C});
            Tris.Append({Base, Base + 1, Base + 2, Base, Base + 2, Base + 3});
        }
    };

    float StrongestFlash = 0.0f;
    FVector StrongestPosUE = AnchorUE;

    for (const BridgeWire::FLightningStrike& Strike : ActiveLightningStrikes)
    {
        const FVector StrikeUE = BridgeWire::ToUnreal(Strike.PositionMC.X, Strike.PositionMC.Y, Strike.PositionMC.Z) * BridgeWire::BlockSize;
        const float LifeFrac = FMath::Clamp(Strike.RemainingSeconds / FMath::Max(0.05f, Strike.TotalSeconds), 0.0f, 1.0f);
        // High-frequency realistic double-flicker envelope
        const float Flicker = 0.65f + 0.35f * FMath::Sin(LifeFrac * 38.0f + float(Strike.Seed & 15u));
        const float Alpha = FMath::Pow(LifeFrac, 0.55f) * Flicker;
        if (Alpha > StrongestFlash)
        {
            StrongestFlash = Alpha;
            StrongestPosUE = StrikeUE;
        }

        // Main jagged bolt from ground impact up into cloud base (1400m ceiling)
        constexpr int32 Segments = 18;
        const FVector RelBase = StrikeUE - AnchorUE;
        TArray<FVector> MainPath;
        MainPath.Reserve(Segments + 1);
        FVector Drift = FVector::ZeroVector;
        for (int32 K = 0; K <= Segments; ++K)
        {
            const float Frac = float(K) / float(Segments);
            const float Z = Frac * 145000.0f; // 1.45 km up to cloud base
            if (K > 0 && K < Segments)
            {
                const float Jitter = 1600.0f * FMath::Sin(Frac * UE_PI);
                Drift.X += (Hash01(Strike.Seed ^ uint32(K * 73 + 1)) - 0.5f) * Jitter;
                Drift.Y += (Hash01(Strike.Seed ^ uint32(K * 97 + 3)) - 0.5f) * Jitter;
            }
            MainPath.Add(RelBase + FVector(Drift.X, Drift.Y, Z));
        }
        const float DistToCamCm = float(FVector::Dist(StrikeUE, CamUE));
        const float MainHalfW = FMath::Clamp(DistToCamCm * 0.0085f, 75.0f, 240.0f);
        EmitRibbon(MainPath, MainHalfW, Alpha);

        // 5 jagged descending branches off the upper/mid trunk
        for (int32 B = 0; B < 5; ++B)
        {
            const int32 StartIdx = 3 + int32(Hash01(Strike.Seed ^ uint32(B * 211 + 19)) * 10.0f);
            TArray<FVector> Branch;
            FVector Cur = MainPath[FMath::Clamp(StartIdx, 2, Segments - 2)];
            Branch.Add(Cur);
            const float DirAngle = Hash01(Strike.Seed ^ uint32(B * 307 + 7)) * UE_TWO_PI;
            const FVector OutDir(FMath::Cos(DirAngle), FMath::Sin(DirAngle), -0.82f);
            for (int32 Step = 1; Step <= 7; ++Step)
            {
                Cur += OutDir * (4400.0f + 1800.0f * Hash01(Strike.Seed ^ uint32(B * 401 + Step * 17)));
                Cur.X += (Hash01(Strike.Seed ^ uint32(B * 509 + Step * 31)) - 0.5f) * 1600.0f;
                Cur.Y += (Hash01(Strike.Seed ^ uint32(B * 613 + Step * 47)) - 0.5f) * 1600.0f;
                Branch.Add(Cur);
            }
            EmitRibbon(Branch, MainHalfW * 0.42f, Alpha * 0.72f);
        }
    }

    const float EnvLumScale = FMath::Exp2(FMath::Clamp(WeatherSim.EnvBaseEV, 2.0f, 12.0f) - 9.0f);
    LightningMesh->SetWorldLocation(AnchorUE);
    LightningMesh->CreateMeshSection_LinearColor(0, Verts, Tris, {}, UVs, Colors, {}, false);
    LightningMesh->SetVisibility(true);
    LightningMaterial->SetScalarParameterValue(TEXT("LightningIntensity"), StrongestFlash * FMath::Clamp(EnvLumScale * 1.6f, 0.25f, 3.0f));

    LightningLight->SetWorldLocation(StrongestPosUE + FVector(0.0, 0.0, 1800.0));
    LightningLight->SetIntensity(2400000.0f * FMath::Clamp(EnvLumScale, 0.15f, 2.5f) * StrongestFlash);
    LightningLight->SetVisibility(StrongestFlash > 0.05f);
    WeatherSim.LightningPosIntensity = FVector4f(float(StrongestPosUE.X), float(StrongestPosUE.Y), float(StrongestPosUE.Z), StrongestFlash);
}

void ABridgeViewer::PushEnvironmentMaterialParameters(float DeltaSeconds)
{
    const bool bMinecraftWet = Environment && Environment->Medium == BridgeWire::ECameraMedium::Underwater;
    const bool bWaterEye = CVarWaterInterface.GetValueOnGameThread() != 0 &&
        (FBridgeWaterOptics::IsInterfaceEnabled() && WaterOptics ? WaterOptics->IsEyeWet() : bMinecraftWet);
    const float QualityTier = float(BridgeQuality::PresetTier());
    const float WindSpeed = FMath::Lerp(0.018f, 0.068f, WeatherSim.WindStrength);
    const FLinearColor WindVec(WindSpeed, WindSpeed * 0.42f, WeatherSim.WindStrength, 0.0f);
    const FVector2f WindDirection = FVector2f(WindVec.R, WindVec.G).GetSafeNormal();
    const bool bPuddleMotion = TerrainPuddleMotionEnabled(int32(QualityTier));
    const bool bPuddleWindWaves = bPuddleMotion && BridgeQuality::WaterWavesEnabled() &&
        CVarWaterP3Wpo.GetValueOnGameThread() != 0;
    const FLinearColor SunDirParam(WeatherSim.SunDirection.X, WeatherSim.SunDirection.Y, WeatherSim.SunDirection.Z,
        FMath::Max(0.0f, WeatherSim.SunDirection.Z));
    const FLinearColor MoonDirParam(WeatherSim.MoonDirection.X, WeatherSim.MoonDirection.Y, WeatherSim.MoonDirection.Z,
        FMath::Max(0.0f, WeatherSim.MoonDirection.Z));
    const FIntPoint GridOrigin = bPrecipGridReady ? PrecipGridOriginChunk * 16 : FIntPoint::ZeroValue;
    const FLinearColor GridOriginParam(
        float(GridOrigin.X),
        float(GridOrigin.Y),
        float(PrecipGridSize),
        float(MinY));
    const float GridValid = bPrecipGridReady ? 1.0f : 0.0f;
    const FLinearColor WaterGridParam(float(WaterGridOrigin.X), float(WaterGridOrigin.Y), float(MinY),
        bWaterGridReady ? 1.0f : 0.0f);
    const float MoonPhase = Environment ? float(Environment->MoonPhase) : 0.0f;

    const float EnvMiddleGrey = 0.18f * FMath::Exp2(FMath::Clamp(WeatherSim.EnvBaseEV, 2.0f, 12.6f));
    const float CloudFlashScale = FMath::Clamp(EnvMiddleGrey * 0.28f, 1.5f, 60.0f);
    const float CloudFallbackLum = FMath::Clamp(EnvMiddleGrey * 1.35f, 1.4f, 650.0f);
    const float SkyFlashLum = FMath::Clamp(EnvMiddleGrey * 1.6f, 2.5f, 450.0f);

    if (CloudMaterial)
    {
        CloudMaterial->SetScalarParameterValue(TEXT("EnvClock"), WeatherSim.Clock);
        CloudMaterial->SetScalarParameterValue(TEXT("CloudWeather"), WeatherSim.Overcast);
        CloudMaterial->SetScalarParameterValue(TEXT("StormClouds"),
            FMath::Clamp(WeatherSim.Overcast * 0.35f + WeatherSim.Thunder * 0.65f, 0.0f, 1.0f));
        const int32 CloudLayers = BridgeQuality::CloudLayers();
        const float EffectiveCloudTier = CloudLayers <= 1 ? 0.0f : (CloudLayers == 2 ? 1.0f : FMath::Max(2.0f, QualityTier));
        CloudMaterial->SetScalarParameterValue(TEXT("CloudQualityTier"), EffectiveCloudTier);
        const float Flash = FMath::Clamp(FMath::Max(WeatherSim.SkyFlash, WeatherSim.LightningPosIntensity.W), 0.0f, 1.0f);
        CloudMaterial->SetVectorParameterValue(TEXT("Storm_LightningColor"),
            FLinearColor(2.0f, 1.8f, 2.0f, Flash * CloudFlashScale));
        CloudMaterial->SetScalarParameterValue(TEXT("CloudCoverage"),
            FMath::Clamp(CVarCloudCoverage.GetValueOnGameThread(), 0.0f, 1.0f));
        CloudMaterial->SetScalarParameterValue(TEXT("MidCloudAmount"),
            FMath::Clamp(CVarMidCloudAmount.GetValueOnGameThread(), 0.0f, 2.0f));
        CloudMaterial->SetScalarParameterValue(TEXT("CirrusAmount"),
            FMath::Clamp(CVarCirrusAmount.GetValueOnGameThread(), 0.0f, 4.0f));
        CloudMaterial->SetVectorParameterValue(TEXT("WindVector"), WindVec);
    }

    if (SkyDomeMaterial)
    {
        SkyDomeMaterial->SetScalarParameterValue(TEXT("EnvClock"), WeatherSim.Clock);
        SkyDomeMaterial->SetScalarParameterValue(TEXT("CloudWeather"), WeatherSim.Overcast);
        SkyDomeMaterial->SetScalarParameterValue(TEXT("MoonPhase"), MoonPhase);
        SkyDomeMaterial->SetScalarParameterValue(TEXT("SkyFlash"), WeatherSim.SkyFlash);
        // The sky dome owns celestial lighting. All visible cloud shapes come from the Epic preset.
        SkyDomeMaterial->SetScalarParameterValue(TEXT("CloudQualityTier"), FMath::Max(1.0f, QualityTier));
        SkyDomeMaterial->SetScalarParameterValue(TEXT("NightSkyScale"), 28.0f);
        SkyDomeMaterial->SetScalarParameterValue(TEXT("CloudFallbackLum"), CloudFallbackLum);
        SkyDomeMaterial->SetScalarParameterValue(TEXT("SkyFlashLum"), SkyFlashLum);
        SkyDomeMaterial->SetVectorParameterValue(TEXT("SunDirection"), SunDirParam);
        SkyDomeMaterial->SetVectorParameterValue(TEXT("MoonDirection"), MoonDirParam);
        SkyDomeMaterial->SetVectorParameterValue(TEXT("WindVector"), WindVec);
    }

    for (int32 Layer = 0; Layer < TerrainMaterials.Num(); ++Layer)
    {
        UMaterialInstanceDynamic* Mat = TerrainMaterials[Layer];
        if (!Mat)
        {
            continue;
        }
        Mat->SetScalarParameterValue(TEXT("EnvClock"), WeatherSim.Clock);
        Mat->SetScalarParameterValue(TEXT("WaterP1"), CVarWaterP1.GetValueOnGameThread() != 0 ? 1.0f : 0.0f);
        Mat->SetScalarParameterValue(TEXT("WaterDebug"), float(CVarWaterDebug.GetValueOnGameThread()));
        Mat->SetScalarParameterValue(TEXT("LocalRain"), WeatherSim.LocalRain);
        Mat->SetScalarParameterValue(TEXT("LocalSnow"), WeatherSim.LocalSnow);
        Mat->SetScalarParameterValue(TEXT("PrecipGridValid"), GridValid);
        Mat->SetVectorParameterValue(TEXT("PrecipGridOrigin"), GridOriginParam);
        if (PrecipGridTexture)
        {
            Mat->SetTextureParameterValue(TEXT("PrecipGrid"), PrecipGridTexture);
        }
        if (Layer == McTerrain::Water)
        {
            Mat->SetScalarParameterValue(TEXT("WindStrength"), WeatherSim.WindStrength);
            Mat->SetScalarParameterValue(TEXT("WaterEyeInWater"), bWaterEye ? 1.0f : 0.0f);
            Mat->SetScalarParameterValue(TEXT("WaterP2FlatInterface"), CVarP2FlatInterface.GetValueOnGameThread() != 0 ? 1.0f : 0.0f);
            Mat->SetScalarParameterValue(TEXT("WaterP2"), FBridgeWaterOptics::IsEnabled() ? 1.0f : 0.0f);
            Mat->SetScalarParameterValue(TEXT("UseSceneCaustics"), WaterOptics && WaterOptics->UseSceneCaustics() ? 1.0f : 0.0f);
            Mat->SetVectorParameterValue(TEXT("WaterGridInfo"), WaterGridParam);
            Mat->SetScalarParameterValue(TEXT("GlobalWaterRain"), WeatherSim.Overcast);
            const bool bP3 = BridgeQuality::WaterP3Enabled(int32(QualityTier)) && CVarWaterP3.GetValueOnGameThread() != 0;
            Mat->SetScalarParameterValue(TEXT("WaterP3Enabled"), bP3 ? 1.0f : 0.0f);
            Mat->SetScalarParameterValue(TEXT("WaterWpoEnabled"), bP3 && BridgeQuality::WaterWavesEnabled() && CVarWaterP3Wpo.GetValueOnGameThread() != 0 ? 1.0f : 0.0f);
            Mat->SetScalarParameterValue(TEXT("WaterSkyFallback"),
                bP3 && Environment->Dimension == BridgeWire::EDimension::Overworld && !TerrainSkyLight->IsVisible() ? 1.0f : 0.0f);
            Mat->SetVectorParameterValue(TEXT("WaterSkyFlash"), FLinearColor(
                WeatherSim.SkyFlash, FMath::Clamp(EnvMiddleGrey * 1.6f, 1.0f, 320.0f), 0.0f, 0.0f));
            FVector Strike = FVector::ZeroVector;
            const float StrikePower = WeatherSim.LightningPosIntensity.W;
            if (StrikePower > 0.0f && Camera)
            {
                Strike = (LightningLight->GetComponentLocation() - Camera->GetActorLocation()) * 0.01;
            }
            Mat->SetVectorParameterValue(TEXT("WaterStrike"), FLinearColor(float(Strike.X), float(Strike.Y), float(Strike.Z), StrikePower));
            if (WaterGridTexture) Mat->SetTextureParameterValue(TEXT("WaterGrid"), WaterGridTexture);
            if (WaterFieldTexture) Mat->SetTextureParameterValue(TEXT("WaterField"), WaterFieldTexture);
            if (WaterSim)
            {
                const bool bOverworld = Environment->Dimension == BridgeWire::EDimension::Overworld;
                const float SunPower = bOverworld
                    ? FMath::SmoothStep(0.02f, 0.60f, float(WeatherSim.SunDirection.Z)) * (1.0f - 0.92f * WeatherSim.Overcast)
                    : 0.0f;
                WaterSim->Update(*Mat, WeatherSim.Clock, DeltaSeconds, FVector2f(WindVec.R, WindVec.G),
                    WeatherSim.WindStrength,
                    CVarWaterP1.GetValueOnGameThread() ? WeatherSim.Overcast : WeatherSim.LocalRain,
                    FVector3f(WeatherSim.SunDirection), SunPower);
                UpdateWaterInteractions(*Mat, DeltaSeconds, bP3 ? FMath::Max(2, int32(QualityTier)) : 0,
                    Environment->Dimension == BridgeWire::EDimension::Overworld ? WeatherSim.Overcast : 0.0f);
            }
        }
        else
        {
            const int32 WetMode = BridgeQuality::RainWetnessMode();
            const auto Moisture = TerrainWetness.GetLegacyMoisture();
            Mat->SetScalarParameterValue(TEXT("SurfaceWetness"), WetMode >= 1 ? Moisture.Wetness : 0.0f);
            Mat->SetScalarParameterValue(TEXT("PuddleLevel"), WetMode >= 2 ? Moisture.Puddles : 0.0f);
            if (Layer == McTerrain::Solid)
            {
                Mat->SetScalarParameterValue(TEXT("TerrainWetHistoryValid"), WetMode >= 1 && bTerrainWetHistoryReady ? 1.0f : 0.0f);
                Mat->SetScalarParameterValue(TEXT("TerrainWetPuddlesEnabled"), WetMode >= 2 ? 1.0f : 0.0f);
                Mat->SetScalarParameterValue(TEXT("TerrainPuddleDepression"), BridgeQuality::PuddleDepressionsEnabled() ? 1.0f : 0.0f);
                Mat->SetVectorParameterValue(TEXT("TerrainPuddleWind"), FLinearColor(WindDirection.X, WindDirection.Y,
                    bPuddleWindWaves ? WeatherSim.WindStrength : 0.0f, 0.0f));
                Mat->SetScalarParameterValue(TEXT("SurfaceRainRate"), SurfaceRainRate);
                Mat->SetScalarParameterValue(TEXT("TerrainPuddleClock"), float(TerrainPuddleSeconds));
                const int32 FootstepCount = bPuddleMotion && TerrainPuddleFootstepTexture &&
                    TerrainFootstepOriginChunk == PrecipGridOriginChunk && bTerrainWetHistoryReady ? TerrainPuddleImpacts.Num() : 0;
                Mat->SetScalarParameterValue(TEXT("TerrainPuddleFootstepCount"), float(FootstepCount));
                Mat->SetScalarParameterValue(TEXT("TerrainPuddleFootstepsEnabled"), FootstepCount > 0 ? 1.0f : 0.0f);
                if (TerrainWetHistoryTexture) Mat->SetTextureParameterValue(TEXT("TerrainWetHistory"), TerrainWetHistoryTexture);
                if (TerrainPuddleFootstepTexture) Mat->SetTextureParameterValue(TEXT("TerrainPuddleFootsteps"), TerrainPuddleFootstepTexture);
            }
        }
    }
}

void ABridgeViewer::ResetEnvironment()
{
    Environment.Reset();
    SectionLights.Reset();
    ChunkPrecipData.Reset();
    TerrainWetness.Reset();
    SurfaceRainRate = 0.0f;
    ResetTerrainPuddleFootsteps();
    InvalidatePrecipGrid();
    if (UMinecraftWeatherSubsystem* Weather = GetWorld()->GetSubsystem<UMinecraftWeatherSubsystem>())
    {
        Weather->ResetMinecraftWorld(Height != 0);
    }
    ResetWaterGrid();
    ResetWaterInteractions();
    ActiveLightningStrikes.Reset();
    WeatherSim = {};
    if (WaterSim)
    {
        WaterSim->Reset();
    }

    USceneComponent* Components[] = {
        TerrainSun, TerrainMoon, TerrainAtmosphere, TerrainSkyLight, TerrainClouds, TerrainFog,
        TerrainSkyDome, RainMesh, SnowMesh, LightningMesh, LightningLight
    };
    for (USceneComponent* Component : Components)
    {
        if (Component)
        {
            Component->SetVisibility(false);
        }
    }
    for (const auto& Pair : ActivePointLights)
    {
        RecyclePointLight(Pair.Value);
    }
    ActivePointLights.Reset();
    NextPointLightUpdate = 0.0;
    if (Camera)
    {
        Camera->GetCameraComponent()->PostProcessSettings.bOverride_SceneColorTint = false;
    }
}

void ABridgeViewer::UpdateTerrainLightingParameters()
{
    const bool bOverworld = !Environment || Environment->Dimension == BridgeWire::EDimension::Overworld;
    bHardwareSceneLighting = bOverworld && BridgeQuality::HardwareGiEnabled();
    const float Daylight = (Environment && bOverworld) ? FMath::Max(0.0f, FMath::Cos(Environment->SunAngle)) : 0.0f;
    const float Weather = bOverworld ? WeatherSim.Overcast : 0.0f;
    const float AmbientBoost = FMath::Pow(2.0f, FMath::Max(0.0f, BridgeQuality::ExposureBias()) * 0.55f)
        * BridgeQuality::CaveBrightnessScale();
    const float LightScale = BridgeQuality::BlockLightScale();

    if (bOverworld)
    {
        AmbientFloorColor = FLinearColor(0.032f, 0.038f, 0.052f) * AmbientBoost;
    }
    else if (Environment->Dimension == BridgeWire::EDimension::Nether)
    {
        AmbientFloorColor = FLinearColor(0.42f, 0.17f, 0.10f) * AmbientBoost;
    }
    else if (Environment->Dimension == BridgeWire::EDimension::End)
    {
        AmbientFloorColor = FLinearColor(0.36f, 0.30f, 0.46f) * AmbientBoost;
    }
    else
    {
        AmbientFloorColor = FLinearColor(0.24f, 0.24f, 0.28f) * AmbientBoost;
    }

    // Keep emissive blocks clean and textured at night/in caves/Nether (48 cd/m^2) while gently boosting
    // sun-exposed emissives outdoors so lava/glowstone/lanterns remain visibly radiant in bright daylight.
    EmissiveBaseLuminance = 48.0f * LightScale;
    EmissiveDayBoostLuminance = bOverworld
        ? FMath::Lerp(0.0f, 220.0f, FMath::Pow(Daylight, 0.75f) * FMath::Lerp(1.0f, 0.35f, Weather)) * LightScale
        : 0.0f;

    FallbackBlockFill = bOverworld ? 6.5f : 5.5f;
    const float SkyLuminance = bOverworld
        ? FMath::Lerp(2.2f, 210.0f, FMath::Pow(Daylight, 0.72f)) * FMath::Lerp(1.0f, 0.45f, Weather)
        : 0.0f;
    FallbackSkyFill = FLinearColor(0.56f * SkyLuminance, 0.74f * SkyLuminance, SkyLuminance);
    for (UMaterialInstanceDynamic* Material : TerrainMaterials)
    {
        ApplyLightFallback(*Material);
    }
    UpdateDynamicLighting();
}

void ABridgeViewer::CreateBlockLightProfiles()
{
    // 256x256 2D IES mask for single-face flush emitters (e.g., magma blocks / sea lanterns embedded
    // in flat ground, lava pools, or lit furnace fronts). Hovering a small-radius point light 20 cm
    // off the exposed face illuminates adjacent coplanar ground and wall bases from 0 cm through the
    // standard PBR / MegaLights / Path Tracing pipeline, while this square-pyramid nadir mask cuts out
    // the emitter's own 1x1 face so dark non-emissive texels (magma crust, obsidian body) stay crisp.
    // A smooth grazing-angle boost compensates Lambert cos(theta) foreshortening on coplanar neighbors
    // without over-brightening overhead surfaces or walls.
    constexpr int32 Size = 256;
    constexpr float HoverBlocks = 0.20f;
    constexpr float InnerHalf = 0.492f;
    constexpr float OuterHalf = 0.515f;

    UTextureLightProfile* Profile = NewObject<UTextureLightProfile>(this, NAME_None, RF_Transient);
    if (!Profile)
    {
        return;
    }
    Profile->Brightness = -1.0f;
    Profile->TextureMultiplier = 1.0f;
    Profile->SRGB = false;
    Profile->Filter = TF_Bilinear;
    Profile->AddressX = TA_Clamp;
    Profile->AddressY = TA_Wrap;
    Profile->NeverStream = true;
    Profile->LODGroup = TEXTUREGROUP_IESLightProfile;
    Profile->bNotOfflineProcessed = true;

    FTexturePlatformData* PlatformData = new FTexturePlatformData();
    PlatformData->SizeX = Size;
    PlatformData->SizeY = Size;
    PlatformData->SetNumSlices(1);
    PlatformData->PixelFormat = PF_R16F;

    FTexture2DMipMap* Mip = new FTexture2DMipMap(Size, Size, 1);
    PlatformData->Mips.Add(Mip);
    Mip->BulkData.Lock(LOCK_READ_WRITE);
    FFloat16* Dest = reinterpret_cast<FFloat16*>(Mip->BulkData.Realloc(Size * Size * sizeof(FFloat16)));
    for (int32 Y = 0; Y < Size; ++Y)
    {
        const float V = (float(Y) + 0.5f) / float(Size);
        const float Phi = (V - 0.5f) * UE_TWO_PI;
        const float SquareScale = FMath::Max(FMath::Abs(FMath::Cos(Phi)), FMath::Abs(FMath::Sin(Phi)));
        const float AlphaOne = FMath::Atan((HoverBlocks * SquareScale) / OuterHalf);
        const float AlphaZero = FMath::Atan((HoverBlocks * SquareScale) / InnerHalf);
        FFloat16* Row = Dest + Y * Size;
        for (int32 X = 0; X < Size; ++X)
        {
            const float U = (float(X) + 0.5f) / float(Size);
            const float Angle = (U - 0.5f) * UE_PI;
            const float SelfCutout = 1.0f - FMath::SmoothStep(AlphaOne, AlphaZero, Angle);
            const float GrazingBoost = 1.0f + 1.8f * FMath::SmoothStep(-0.22f, AlphaOne, Angle);
            Row[X] = FFloat16(SelfCutout * GrazingBoost);
        }
    }
    Mip->BulkData.Unlock();

    Profile->SetPlatformData(PlatformData);
    Profile->UpdateResource();
    FlushFaceIesProfile = Profile;
}

void ABridgeViewer::ApplyLightFallback(UMaterialInstanceDynamic& Material, bool bUnlitParticle) const
{
    // Unlit particles depend on the Minecraft lightmap in every lighting mode.
    const bool bUsePackedLight = bUnlitParticle || !bHardwareSceneLighting;
    Material.SetScalarParameterValue(TEXT("FallbackBlockFill"), bUsePackedLight ? FallbackBlockFill : 0.0f);
    Material.SetVectorParameterValue(TEXT("FallbackSkyFill"), bUsePackedLight ? FallbackSkyFill : FLinearColor::Black);
    Material.SetVectorParameterValue(TEXT("AmbientFloor"), AmbientFloorColor);
    Material.SetScalarParameterValue(TEXT("EmissiveBase"), EmissiveBaseLuminance);
    Material.SetScalarParameterValue(TEXT("EmissiveDayBoost"), EmissiveDayBoostLuminance);
}

void ABridgeViewer::RecyclePointLight(int32 Slot)
{
    ULocalLightComponent* Light = PointLightPool[Slot];
    GlassLighting.Forget(*Light);
    Light->SetVisibility(false);
    if (Light->IsA<URectLightComponent>())
    {
        FreeFaceLights.Add(Slot);
    }
    else if (Light->IESTexture != nullptr)
    {
        FreeFlushLights.Add(Slot);
    }
    else
    {
        FreePointLights.Add(Slot);
    }
}

namespace
{
    // The terrain atlas keeps its mips resident for filter changes. AtlasPatch has already applied
    // Minecraft's real frame (including interpolation) to these bytes on the game thread.
    struct FEmissionAtlasSampler
    {
        UTexture2D* Albedo;
        UTexture2D* Pbr;
        FTexture2DMipMap* ColorMip = nullptr;
        FTexture2DMipMap* MaskMip = nullptr;
        const FColor* Colors = nullptr;
        const FColor* Masks = nullptr;
        TMap<BridgeWire::FEmitterFootprint, FLinearColor> SharedSamples;

        FEmissionAtlasSampler(UTexture2D* A, UTexture2D* P) : Albedo(A), Pbr(P) {}
        ~FEmissionAtlasSampler()
        {
            if (ColorMip) ColorMip->BulkData.Unlock();
            if (MaskMip) MaskMip->BulkData.Unlock();
        }
        bool Open()
        {
            if (ColorMip) return Colors && Masks;
            auto* A = Albedo ? Albedo->GetPlatformData() : nullptr;
            auto* P = Pbr ? Pbr->GetPlatformData() : nullptr;
            if (!A || !P || A->Mips.IsEmpty() || P->Mips.IsEmpty()) return false;
            auto& C = A->Mips[0];
            auto& M = P->Mips[0];
            if (C.SizeX != M.SizeX || C.SizeY != M.SizeY || C.BulkData.GetBulkDataSize() != int64(C.SizeX)*C.SizeY*4 ||
                M.BulkData.GetBulkDataSize() != C.BulkData.GetBulkDataSize() || !C.BulkData.IsBulkDataLoaded() || !M.BulkData.IsBulkDataLoaded()) return false;
            ColorMip = &C;
            MaskMip = &M;
            Colors = static_cast<const FColor*>(C.BulkData.LockReadOnly());
            Masks = static_cast<const FColor*>(M.BulkData.LockReadOnly());
            return Colors && Masks;
        }
        FLinearColor Read(const BridgeWire::FEmitterFootprint& Key)
        {
            if (const auto* Cached = SharedSamples.Find(Key)) return *Cached;
            FVector2f UV[4];
            for (int32 I=0; I<4; ++I)
                UV[I] = FVector2f(Key.UV[I] >> 16, Key.UV[I] & 65535) / 65535.f;
            FLinearColor Sum(0,0,0,0);
            constexpr int Edge = 8;
            for (int Y=0; Y<Edge; ++Y) for (int X=0; X<Edge; ++X)
            {
                const float S = (X+.5f)/Edge, T = (Y+.5f)/Edge;
                const FVector2f At = UV[0]*(1-FMath::Max(S,T)) + UV[2]*FMath::Min(S,T) +
                    (S>T ? UV[1]*(S-T) : UV[3]*(T-S));
                const int32 Index = FMath::Clamp(int32(At.Y*ColorMip->SizeY),0,ColorMip->SizeY-1)*ColorMip->SizeX +
                    FMath::Clamp(int32(At.X*ColorMip->SizeX),0,ColorMip->SizeX-1);
                const FLinearColor RGB = FLinearColor::FromSRGBColor(Colors[Index]);
                float Mask = Key.Mode == 3 ? 1.f : Masks[Index].B / 255.f;
                if (Key.Mode >= 2)
                {
                    const float YLinear = .2126f*RGB.R + .7152f*RGB.G + .0722f*RGB.B;
                    const float Value = .5f*(FMath::Max3(RGB.R,RGB.G,RGB.B) + YLinear);
                    if (Key.Mode == 4)
                    {
                        const float Reference = FMath::Max(1.f / 255, (1.f-Mask)/.75f);
                        const float Ring = FMath::SmoothStep(.89f,1.005f,YLinear / Reference);
                        Mask = .018f + FMath::Min(1.f,4.f*Mask) * .30f * Ring * Ring;
                    }
                    else if (Key.Mode == 3)
                    {
                        Mask = FMath::SmoothStep(.032f,.74f,Value);
                    }
                    else
                    {
                        Mask *= .035f + .965f * FMath::SmoothStep(.015f,.80f,Value);
                    }
                }
                Sum += RGB * (Mask * RGB.A);
            }
            return SharedSamples.Add(Key,Sum / float(Edge*Edge));
        }
    };
}

void ABridgeViewer::UpdatePointLights()
{
    RefreshEnvironmentQuality();
    if (BridgeQuality::IsTraceRequested())
    {
        BridgeQuality::TraceFrame(Environment.IsSet() && CameraFrames > 0 && LoadedSections > 0 &&
            PendingSections.IsEmpty() && DecodeJobs.IsEmpty() && GlassLighting.PendingHandoffCount() == 0, SceneSerial,
            Camera ? Camera->GetActorTransform() : FTransform::Identity, ActivePointLights.Num());
    }
    const double Now = FPlatformTime::Seconds();
    if (!Environment || CameraFrames == 0 || Now < NextPointLightUpdate ||
        (SectionLights.IsEmpty() && ActivePointLights.IsEmpty()))
    {
        return;
    }
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftUpdatePointLights);
    NextPointLightUpdate = Now + 0.14;
    ++PointLightRevision;
    const bool bMegaLights = BridgeQuality::MegaLights();
    const int32 Budget = bMegaLights ? FMath::Clamp(CVarPointLightBudget.GetValueOnGameThread(), 0, 2048)
        : FMath::Clamp(CVarFallbackLightBudget.GetValueOnGameThread(), 0, 128);
    const float RawFaceSpecular = CVarFaceLightSpecularScale.GetValueOnGameThread();
    const float FaceSpecularScale = RawFaceSpecular >= 0.0f
        ? FMath::Clamp(RawFaceSpecular, 0.0f, 1.0f)
        : (BridgeQuality::HardwareReflectionsEnabled() ? 0.18f : 0.55f);

    enum class ELightProxyKind : uint8
    {
        Point,
        FlushIes,
        Rect
    };
    const auto LightKindOf = [](const ULocalLightComponent* Light) -> ELightProxyKind
    {
        if (Light->IsA<URectLightComponent>())
        {
            return ELightProxyKind::Rect;
        }
        return Light->IESTexture != nullptr ? ELightProxyKind::FlushIes : ELightProxyKind::Point;
    };

    struct FCandidate
    {
        const BridgeWire::FSectionLight* Source;
        FIntVector Key;
        double Score;
        int8 Face;
        FLinearColor Color;
        float Power;
    };
    const auto Better = [](const FCandidate& A, const FCandidate& B)
    {
        if (A.Score != B.Score) return A.Score < B.Score;
        if (A.Key.X != B.Key.X) return A.Key.X < B.Key.X;
        if (A.Key.Y != B.Key.Y) return A.Key.Y < B.Key.Y;
        return A.Key.Z < B.Key.Z;
    };
    const auto Worse = [&Better](const FCandidate& A, const FCandidate& B) { return Better(B, A); };
    TArray<FCandidate> Candidates;
    Candidates.Reserve(Budget);
    FEmissionAtlasSampler Sampler(TerrainAtlases[int32(BridgeWire::ETerrainAtlas::Albedo)].Texture.Get(),
        TerrainAtlases[int32(BridgeWire::ETerrainAtlas::Pbr)].Texture.Get());
    const bool bHasCameraForward = !CameraForwardMC.IsNearlyZero();
    const FVector CamFwdDir = bHasCameraForward ? CameraForwardMC.GetSafeNormal() : FVector::ZeroVector;
    if (Budget > 0)
    {
        constexpr double RangeSq = FMath::Square(80.0);
        constexpr double RearPenaltyRangeSq = FMath::Square(8.0);
        for (const auto& Pair : SectionLights)
        {
            const FVector SectionCenter = FVector(Pair.Key) * 16.0 + FVector(8.0);
            const FVector Delta = (SectionCenter - CameraPositionMC).GetAbs();
            const FVector DistanceToBounds = (Delta - FVector(8.0)).ComponentMax(FVector::ZeroVector);
            if (DistanceToBounds.SizeSquared() > RangeSq)
            {
                continue;
            }
            for (const BridgeWire::FSectionLight& Source : Pair.Value)
            {
                const FVector ToBlock = (FVector(Source.Block) + FVector(0.5)) - CameraPositionMC;
                const double DistanceSq = ToBlock.SizeSquared();
                if (DistanceSq > RangeSq)
                {
                    continue;
                }
                double ViewPenalty = 1.0;
                if (bHasCameraForward && DistanceSq > RearPenaltyRangeSq)
                {
                    const double InvDist = FMath::InvSqrt(DistanceSq);
                    if (FVector::DotProduct(ToBlock, CamFwdDir) * InvDist < -0.15)
                    {
                        ViewPenalty = 1.35;
                    }
                }
                const double Score = ((DistanceSq + 1.0) * ViewPenalty) / FMath::Square(double(Source.Level));
                const auto Offer = [&](int8 Face, double Priority, FLinearColor Color = FLinearColor::White, float Power = 1.f)
                {
                    const FIntVector Key = SlotKey(Source.Block, Face);
                    const double Hysteresis = ActivePointLights.Contains(Key) ? 0.89 : 1.0;
                    const FCandidate Candidate{&Source, Key, Score * Hysteresis * Priority, Face, Color, Power};
                    if (Candidates.Num() < Budget)
                    {
                        Candidates.HeapPush(Candidate, Worse);
                    }
                    else if (Better(Candidate, Candidates.HeapTop()))
                    {
                        Candidates.HeapPopDiscard(Worse, EAllowShrinking::No);
                        Candidates.HeapPush(Candidate, Worse);
                    }
                };
                if (Source.bMaterial)
                {
                    for (int32 I = 0; I < Source.Patches.Num(); ++I)
                    {
                        const auto& Patch = Source.Patches[I];
                        float Power = Patch.Power;
                        FLinearColor Color = FLinearColor::FromSRGBColor(Patch.Color);
                        if (Patch.SampleArea > 0 && Sampler.Open())
                        {
                            Color = Sampler.Read(Patch.Footprint) * FLinearColor::FromSRGBColor(Patch.SampleTint);
                            const float Peak = FMath::Max3(Color.R,Color.G,Color.B);
                            Power = Peak * Patch.SampleArea;
                            if (Power < 1.e-8f) continue;
                            Color = Color / Peak;
                            Color.A = 1.f;
                        }
                        Offer(6 + I, 1.0 / FMath::Max(0.001f, Power), Color, Power);
                    }
                    continue;
                }
                if (Source.Flags == 0)
                {
                    Offer(CenterSource, 1.0);
                    continue;
                }
                double Priority = 1.0;
                for (int8 Face : {1, 2, 3, 4, 5, 0})
                {
                    if (Source.Flags & (1u << Face))
                    {
                        Offer(Face, Priority);
                        Priority = 1.65;
                    }
                }
            }
        }
    }
    Candidates.Sort(Better);
    TSet<FIntVector> Selected;
    TMap<FIntVector, int32> SelectedPerBlock;
    Selected.Reserve(Candidates.Num());
    for (const FCandidate& Candidate : Candidates)
    {
        Selected.Add(Candidate.Key);
        ++SelectedPerBlock.FindOrAdd(Candidate.Source->Block);
    }
    for (auto It = ActivePointLights.CreateIterator(); It; ++It)
    {
        if (!Selected.Contains(It.Key()))
        {
            RecyclePointLight(It.Value());
            It.RemoveCurrent();
        }
    }

    constexpr float FaceRectOffsetBlocks = 0.015f;
    constexpr float FaceRectSizeCm = 94.0f;
    constexpr float FlushIesHoverBlocks = 0.20f;
    constexpr float BaseCandela = 240.0f;

    int32 NewComponents = 0;
    for (int32 Rank = 0; Rank < Candidates.Num(); ++Rank)
    {
        const FCandidate& Candidate = Candidates[Rank];
        const auto& Source = *Candidate.Source;
        const int8 Face = Candidate.Face;
        const BridgeWire::FEmitterPatch* Patch = Source.bMaterial ? &Source.Patches[Face - 6] : nullptr;
        const bool bFaceLight = Patch ? Patch->Width > 0 : Face != CenterSource;
        const bool bFlushIes = bFaceLight && !Patch && (Source.Flags & CoveredFace) == 0 &&
            FMath::CountBits(uint32(Source.Flags & FaceMask)) == 1u && FlushFaceIesProfile != nullptr;
        // A cover can be closer than the IES light's hover distance. Use a flush area light so its rays still meet the cover.
        const ELightProxyKind WantKind = !bFaceLight
            ? ELightProxyKind::Point
            : (bFlushIes ? ELightProxyKind::FlushIes : ELightProxyKind::Rect);

        const int32* Existing = ActivePointLights.Find(Candidate.Key);
        ULocalLightComponent* Light = nullptr;
        bool bRegister = false;
        if (Existing)
        {
            ULocalLightComponent* Current = PointLightPool[*Existing];
            if (LightKindOf(Current) == WantKind)
            {
                Light = Current;
            }
            else
            {
                RecyclePointLight(*Existing);
                ActivePointLights.Remove(Candidate.Key);
            }
        }
        if (!Light)
        {
            TArray<int32>& FreeLights = WantKind == ELightProxyKind::Rect
                ? FreeFaceLights
                : (WantKind == ELightProxyKind::FlushIes ? FreeFlushLights : FreePointLights);
            if (!FreeLights.IsEmpty())
            {
                const int32 Slot = FreeLights.Pop(EAllowShrinking::No);
                Light = PointLightPool[Slot];
                ActivePointLights.Add(Candidate.Key, Slot);
            }
            else
            {
                if (NewComponents >= 64)
                {
                    continue;
                }
                if (WantKind == ELightProxyKind::Rect)
                {
                    auto* RectLight = NewObject<URectLightComponent>(this);
                    RectLight->SetSourceWidth(FaceRectSizeCm);
                    RectLight->SetSourceHeight(FaceRectSizeCm);
                    RectLight->SetBarnDoorAngle(88.0f);
                    RectLight->SetBarnDoorLength(0.1f);
                    Light = RectLight;
                }
                else
                {
                    auto* PointLight = NewObject<UPointLightComponent>(this);
                    if (WantKind == ELightProxyKind::FlushIes)
                    {
                        PointLight->SetIESTexture(FlushFaceIesProfile);
                        PointLight->SetUseIESBrightness(false);
                    }
                    Light = PointLight;
                }
                Light->SetupAttachment(RootComponent);
                Light->SetMobility(EComponentMobility::Movable);
                Light->SetIntensityUnits(ELightUnits::Candelas);
                Light->SetCastRaytracedShadows(ECastRayTracedShadow::Disabled);
                Light->MegaLightsShadowMethod = EMegaLightsShadowMethod::RayTracing;
                Light->SetVolumetricScatteringIntensity(0.65f);
                ActivePointLights.Add(Candidate.Key, PointLightPool.Add(Light));
                bRegister = true;
                ++NewComponents;
            }
        }

        FVector Position = FVector(Source.Block) + FVector(0.5, 0.72, 0.5);
        if (Patch)
        {
            Position = FVector(Source.Block) + FVector(Patch->Center) + FVector(Patch->Normal) * (bFaceLight ? 0.005 : 0.02);
            const FRotator Rotation = FRotationMatrix::MakeFromXY(
                BridgeWire::ToUnreal(Patch->Normal.X, Patch->Normal.Y, Patch->Normal.Z),
                BridgeWire::ToUnreal(Patch->WidthAxis.X, Patch->WidthAxis.Y, Patch->WidthAxis.Z)).Rotator();
            if (!Light->GetRelativeRotation().Equals(Rotation)) Light->SetRelativeRotation(Rotation);
        }
        else if (bFaceLight)
        {
            const double NormalOffset = WantKind == ELightProxyKind::FlushIes
                ? (0.5 + double(FlushIesHoverBlocks))
                : (0.5 + double(FaceRectOffsetBlocks));
            Position = FVector(Source.Block) + FVector(0.5) + FVector(FaceNormals[Face]) * NormalOffset;
            const FRotator Rotation = BridgeWire::ToUnreal(FaceNormals[Face]).Rotation();
            if (Light->GetRelativeRotation() != Rotation)
            {
                Light->SetRelativeRotation(Rotation);
            }
        }
        const FVector Location = BridgeWire::ToUnreal(Position.X, Position.Y, Position.Z) * BridgeWire::BlockSize;
        if (Location != Light->GetRelativeLocation())
        {
            Light->SetRelativeLocation(Location);
            UE_CLOG(bTrace, LogMinecraftBridgeEnvironment, VeryVerbose,
                TEXT("Point light scene=%u block=%s face=%d kind=%u position=%s."),
                SceneSerial, *Source.Block.ToString(), int32(Face), uint32(WantKind), *Location.ToString());
        }
        const float Level = Source.Level / 15.0f;
        const float LevelCurve = FMath::Pow(Level, 1.25f);
        const int32 SelectedFaces = SelectedPerBlock.FindChecked(Source.Block);
        const float FaceShare = WantKind == ELightProxyKind::Rect
            ? 1.0f / (1.0f + 0.35f * float(FMath::Max(0, SelectedFaces - 1)))
            : 1.0f;
        if (auto* Rect = Cast<URectLightComponent>(Light))
        {
            Rect->SetSourceWidth(Patch ? Patch->Width * BridgeWire::BlockSize : FaceRectSizeCm);
            Rect->SetSourceHeight(Patch ? Patch->Height * BridgeWire::BlockSize : FaceRectSizeCm);
        }
        Light->SetLightColor(Patch ? Candidate.Color : FLinearColor::FromSRGBColor(Source.Color));
        Light->SetIntensity(BaseCandela * BridgeQuality::BlockLightScale() * LevelCurve * (Patch ? Candidate.Power : FaceShare));
        Light->SetAttenuationRadius(480.0f + 1220.0f * Level);
        if (auto* PointLight = Cast<UPointLightComponent>(Light))
        {
            PointLight->SetSourceRadius(Patch ? Patch->Height * BridgeWire::BlockSize :
                (WantKind == ELightProxyKind::FlushIes ? 10.0f : (10.0f + 10.0f * Level)));
        }
        Light->SetSpecularScale(bFaceLight ? FaceSpecularScale : 1.0f);
        Light->SetCastShadows(bMegaLights || Rank < 8);
        if (bRegister)
        {
            Light->RegisterComponent();
        }
        Light->SetVisibility(!GlassLighting.Replaces(*Light));
    }
}

bool ABridgeViewer::ApplySectionGlass(BridgeWire::FReader Reader, int32 Size)
{
    if (Size < BridgeWire::SectionGlassHeaderBytes || Reader.U32() != SceneSerial)
    {
        return false;
    }
    const int32 X = Reader.I32();
    const int32 Y = Reader.I32();
    const int32 Z = Reader.I32();
    const FIntVector Section(X, Y, Z);
    return IsSectionInWorld(Section) && GlassLighting.Receive(Section, Reader, Size - 16);
}

void ABridgeViewer::UpdateGlassLighting()
{
    if (!GlassLighting.NeedsUpdate() || !Camera || !TerrainSun)
    {
        return;
    }
    TArray<ULocalLightComponent*, TInlineAllocator<64>> Lights;
    Lights.Reserve(ActivePointLights.Num());
    for (const auto& Pair : ActivePointLights)
    {
        Lights.Add(PointLightPool[Pair.Value]);
    }
    GlassLighting.Update(*this, Camera->GetActorLocation(), *TerrainSun, TerrainAtmosphere, Lights,
        PointLightRevision, BridgeQuality::HardwareLighting());
}

bool ABridgeViewer::ApplySectionLights(BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    if (Size < SectionLightsHeaderBytes || Reader.U32() != SceneSerial)
    {
        return false;
    }
    const int32 X = Reader.I32();
    const int32 Y = Reader.I32();
    const int32 Z = Reader.I32();
    const FIntVector Section(X, Y, Z);
    const int32 Count = Reader.U16();
    if (!IsSectionInWorld(Section) || !TerrainColumns.Contains(FIntPoint(X, Z)) ||
        Count > MaxSectionLights || Size < SectionLightsHeaderBytes + Count * SectionLightBytes)
    {
        return false;
    }
    if (Count == 0)
    {
        if (Size != SectionLightsHeaderBytes) return false;
        if (SectionLights.Remove(Section) > 0)
        {
            NextPointLightUpdate = 0.0;
        }
        return true;
    }

    TBitArray<TInlineAllocator<MaxSectionLights / 32>> Seen(false, MaxSectionLights);
    const FIntVector Origin = Section * 16;
    TArray<FSectionLight> Lights;
    Lights.Reserve(Count);
    const uint8* End = Reader.Data + (Size - SectionLightsHeaderBytes);
    for (int32 I = 0; I < Count; ++I)
    {
        if (End - Reader.Data < SectionLightBytes) return false;
        const uint16 Packed = Reader.U16();
        const uint8 R = Reader.Byte();
        const uint8 G = Reader.Byte();
        const uint8 B = Reader.Byte();
        const uint8 EncodedLevel = Reader.Byte();
        const bool bMaterial = (EncodedLevel & 0x80) != 0;
        const bool bSampled = (EncodedLevel & 0x40) != 0;
        const uint8 Level = EncodedLevel & 0x3f;
        const uint8 Flags = Reader.Byte();
        const uint8 Faces = Flags & FaceMask;
        if (Packed >= MaxSectionLights || Level == 0 || Level > 15 || (bSampled && !bMaterial) || Seen[Packed] ||
            (Flags != 0 && ((Flags & FaceSource) == 0 || Faces == 0)) ||
            ((Flags & CoveredFace) != 0 && FMath::CountBits(uint32(Faces)) != 1u))
        {
            return false;
        }
        Seen[Packed] = true;
        const FIntVector Block = Origin + FIntVector(Packed & 15, (Packed >> 4) & 15, Packed >> 8);
        FSectionLight& Source = Lights.Add_GetRef({Block, FColor(R, G, B), Level, Flags, bMaterial, {}});
        if (bMaterial)
        {
            if (Reader.Data == End) return false;
            const int32 N = Reader.Byte();
            if (bSampled && End - Reader.Data < 4) return false;
            const uint32 Samples = bSampled ? Reader.U32() : 0;
            if ((bSampled && Samples == 0) || (N < 32 && (Samples >> N) != 0)) return false;
            if (N > MaxMaterialPatches || End - Reader.Data < N * MaterialPatchBytes) return false;
            Source.Patches.Reserve(N);
            for (int32 P = 0; P < N; ++P)
            {
                if (End - Reader.Data < MaterialPatchBytes) return false;
                FEmitterPatch Patch;
                for (int32 Axis = 0; Axis < 3; ++Axis) Patch.Center[Axis] = int16(Reader.U16()) / 1024.0f;
                for (int32 Axis = 0; Axis < 3; ++Axis) Patch.Normal[Axis] = int8(Reader.Byte()) / 127.0f;
                for (int32 Axis = 0; Axis < 3; ++Axis) Patch.WidthAxis[Axis] = int8(Reader.Byte()) / 127.0f;
                Patch.Width = Reader.U16() / 1024.0f;
                Patch.Height = Reader.U16() / 1024.0f;
                const uint8 Red = Reader.Byte(), Green = Reader.Byte(), Blue = Reader.Byte();
                Patch.Color = FColor(Red, Green, Blue);
                Patch.Power = Reader.Float();
                if (!FMath::IsNearlyEqual(Patch.Normal.SizeSquared(), 1.0f, 0.03f) ||
                    !FMath::IsNearlyEqual(Patch.WidthAxis.SizeSquared(), 1.0f, 0.03f) ||
                    FMath::Abs(FVector3f::DotProduct(Patch.Normal, Patch.WidthAxis)) > 0.03f ||
                    Patch.Height <= 0 || !(Patch.Power > 0 && Patch.Power <= 262144.0f)) return false;
                if ((Samples & (1u << P)) != 0)
                {
                    if (End - Reader.Data < MaterialSampleBytes) return false;
                    for (uint32& UV : Patch.Footprint.UV) UV = Reader.U32();
                    Patch.Footprint.Mode = Reader.Byte();
                    const uint8 TR = Reader.Byte(), TG = Reader.Byte(), TB = Reader.Byte();
                    Patch.SampleTint = FColor(TR,TG,TB);
                    Patch.SampleArea = Reader.Float();
                    if (Patch.Footprint.Mode > 4 || !(Patch.SampleArea > 0 && Patch.SampleArea <= 262144.f)) return false;
                }
                Source.Patches.Add(Patch);
                UE_CLOG(bTrace, LogMinecraftBridgeEnvironment, VeryVerbose,
                    TEXT("Material emitter block=%s patch=%d center=%s size=%.3fx%.3f rgb=%02X%02X%02X power=%.5f."),
                    *Block.ToString(), P, *Patch.Center.ToString(), Patch.Width, Patch.Height, Red, Green, Blue, Patch.Power);
            }
        }
        UE_CLOG(bTrace, LogMinecraftBridgeEnvironment, VeryVerbose,
            TEXT("Section light scene=%u block=(%d,%d,%d) rgb=%02X%02X%02X radius=%u flags=0x%02X."),
            SceneSerial, Block.X, Block.Y, Block.Z, uint32(R), uint32(G), uint32(B), uint32(Level), uint32(Flags));
    }
    if (Reader.Data != End) return false;
    SectionLights.Add(Section, MoveTemp(Lights));
    NextPointLightUpdate = 0.0;
    return true;
}

void ABridgeViewer::TraceEnvironment() const
{
    if (!Environment)
    {
        return;
    }
    int32 LightCount = 0;
    for (const auto& Pair : SectionLights)
    {
        LightCount += Pair.Value.Num();
    }
    const auto& State = *Environment;
    const auto Moisture = TerrainWetness.GetLegacyMoisture();
    UE_LOG(LogMinecraftBridgeEnvironment, Display,
        TEXT("SceneLighting scene=%u dimension=%u sunAngle=%.5f globalRain=%.3f thunder=%.3f localRain=%.3f localSnow=%.3f wetness=%.3f puddles=%.3f flash=%.3f moonPhase=%u medium=%u envEV=%.2f roof=%.2f expMode=%d expBias=%.1f localExp=%d precipChunks=%d bolts=%d lightSections=%d sources=%d active=%d pool=%d. surfaceRain=%.3f wetHistory=%d puddleSteps=%llu activeFootsteps=%d."),
        State.Scene, uint32(State.Dimension), State.SunAngle, State.GlobalRain, State.Thunder,
        State.LocalRain, State.LocalSnow, Moisture.Wetness, Moisture.Puddles, WeatherSim.SkyFlash,
        uint32(State.MoonPhase), uint32(State.Medium), WeatherSim.EnvBaseEV, WeatherSim.UnderRoofBlend,
        BridgeQuality::ExposureMode(), BridgeQuality::ExposureBias(), BridgeQuality::LocalExposureStrength(),
        ChunkPrecipData.Num(), ActiveLightningStrikes.Num(),
        SectionLights.Num(), LightCount, ActivePointLights.Num(), PointLightPool.Num(),
        SurfaceRainRate, bTerrainWetHistoryReady, TerrainPuddleStepCount, TerrainPuddleImpacts.Num());
}
