#pragma once

#include "CoreMinimal.h"
#include "RenderGraphFwd.h"
#include "RenderResource.h"

class FSceneViewFamily;
class UDirectionalLightComponent;
class UMaterialInstanceDynamic;
class UTexture;
class UTextureRenderTarget2D;

struct FBridgeWaterLocalLight
{
    FVector Position;
    FVector3f Radiance;
    float RadiusM;
};

struct FBridgeWaterSunInputs
{
    FVector Eye;
    FVector GridOrigin;
    FVector3f Direction;
    uint64 GridRevision = 0;
    FVector3f Radiance;
    FTextureResource* Grid = nullptr;
    FTextureResource* CausticPattern = nullptr;
    UTexture* GridTexture = nullptr;
    UTexture* CausticTexture = nullptr;
    UDirectionalLightComponent* Light = nullptr;
    int32 Tier = 0;
    bool bNearWater = false;
    TArray<FBridgeWaterLocalLight, TInlineAllocator<4>> LocalLights;
};

using FWaterCasterPositions = TArray<FVector3f>;

struct FBridgeWaterSunFrame
{
    TSharedPtr<const FWaterCasterPositions, ESPMode::ThreadSafe> Casters;
    FTextureResource* Grid = nullptr;
    FTextureResource* Pattern = nullptr;
    FTextureResource* AirShadow = nullptr;
    FTextureResource* WaterShadow = nullptr;
    FVector Origin = FVector::ZeroVector;
    FVector3f CenterM = FVector3f::ZeroVector;
    FVector3f SunAir = FVector3f::UpVector;
    FVector3f SunWater = FVector3f::UpVector;
    FVector3f Radiance = FVector3f::ZeroVector;
    FVector2f PatternPhaseM = FVector2f::ZeroVector;
    uint64 GridRevision = 0;
    float RadiusM = 64.0f;
    uint64 Revision = 0;
    int32 Steps = 0;
    bool bCaustics = false;

    bool IsActive() const { return AirShadow && WaterShadow && Grid; }
};

struct FBridgeWaterSunTextures
{
    FRDGTextureRef Grid = nullptr;
    FRDGTextureRef Pattern = nullptr;
    FRDGTextureRef AirShadow = nullptr;
    FRDGTextureRef WaterShadow = nullptr;
};

class FBridgeWaterSun
{
public:
    FBridgeWaterSun();
    ~FBridgeWaterSun();

    void CommitSection(const FIntVector& Section, const FVector3f& RegionOffset, TConstArrayView<FVector3f> Positions);
    void RemoveSection(const FIntVector& Section);
    FBridgeWaterSunFrame Update(const FBridgeWaterSunInputs& Input, bool bVolume, bool bCaustics);
    void Reset();

private:
    void SetLightFunction(UDirectionalLightComponent& Light, bool bEnabled);

    TMap<FIntVector, FWaterCasterPositions> SectionCasters;
    TSharedPtr<const FWaterCasterPositions, ESPMode::ThreadSafe> CombinedCasters;
    TStrongObjectPtr<UTextureRenderTarget2D> AirShadow;
    TStrongObjectPtr<UTextureRenderTarget2D> WaterShadow;
    TStrongObjectPtr<UMaterialInstanceDynamic> LightFunction;
    TWeakObjectPtr<UDirectionalLightComponent> BoundLight;
    FVector CachedGridOrigin = FVector::ZeroVector;
    FIntVector CachedCenterCell = FIntVector::ZeroValue;
    uint64 Revision = 0;
    uint64 CombinedRevision = MAX_uint64;
    bool bTriedMaterial = false;
};

class FBridgeWaterSunRenderState
{
public:
    FBridgeWaterSunTextures Prepare(FRDGBuilder& Graph, const FSceneViewFamily& Family, const FBridgeWaterSunFrame& Frame);

private:
    TRefCountPtr<FRDGPooledBuffer> CasterBuffer;
    uint64 GeometryRevision = MAX_uint64;
    uint64 GridRevision = MAX_uint64;
    FVector CachedOrigin = FVector::ZeroVector;
    FVector3f CachedSun = FVector3f::ZeroVector;
    FVector3f CachedCenter = FVector3f::ZeroVector;
    float CachedRadius = 0.0f;
};
