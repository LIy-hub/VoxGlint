#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture.h"
#include "BridgeWaterSim.generated.h"

class FBridgeWaterViewExtension;
class FTextureResource;
class UMaterialInstanceDynamic;
class UWorld;

// Material-visible texture backed by an RHI texture with a writable mip chain.
UCLASS(Transient)
class UBridgeWaterTexture final : public UTexture
{
    GENERATED_BODY()

public:
    void Init(int32 InSize, int32 InSlices, EPixelFormat InFormat, int32 InMips = 0);
    virtual FTextureResource* CreateResource() override;
    virtual ETextureClass GetTextureClass() const override;
    virtual EMaterialValueType GetMaterialType() const override;
    virtual float GetSurfaceWidth() const override { return float(Size); }
    virtual float GetSurfaceHeight() const override { return float(Size); }
    virtual float GetSurfaceDepth() const override { return 0.0f; }
    virtual uint32 GetSurfaceArraySize() const override { return Slices; }

private:
    int32 Size = 0;
    int32 Slices = 0;
    int32 Mips = 0;
    EPixelFormat Format = PF_Unknown;
};

// X/Z in Minecraft coordinates. Fixed units let both quality tiers scroll on exact texels.
struct FBridgeRippleDomain
{
    static constexpr int32 UnitsPerBlock = 32;
    static constexpr int32 TileSize = 8;

    FIntPoint Origin = FIntPoint::ZeroValue; // texels
    int32 Size = 0;
    int32 TexelUnits = 4;

    static FBridgeRippleDomain Around(const FVector& EyeMC, int32 Tier);
    float GetTexelMeters() const { return float(TexelUnits) / UnitsPerBlock; }
    float GetWidthMeters() const { return Size * GetTexelMeters(); }
    FVector2d GetOriginMeters() const { return FVector2d(Origin.X, Origin.Y) * GetTexelMeters(); }
};

// Shape is radius (m), vertical velocity impulse (m/s), foam coverage, and surface Y minus MinY.
// A nonzero heading selects a boat source. All positions are relative to the ripple domain.
struct FBridgeRippleImpulse
{
    FVector2f Position;
    FVector2f Heading;
    FVector4f Shape;
};

struct FBridgeRippleFrame
{
    FBridgeRippleDomain Domain;
    FIntPoint GridOrigin = FIntPoint::ZeroValue;
    FTextureResource* Grid = nullptr;
    FTextureResource* Field = nullptr;
    TArray<FBridgeRippleImpulse> Impulses;
    float DeltaSeconds = 0.0f;
    bool bActive = false;
};

class FBridgeWaterSim
{
public:
    explicit FBridgeWaterSim(UWorld* World);
    void Bind(UMaterialInstanceDynamic& Material) const;
    UTexture* GetCausticTexture() const { return Caustics.Get(); }
    void Update(UMaterialInstanceDynamic& Material, float EnvClock, float DeltaSeconds,
        const FVector2f& WindVector, float WindStrength,
        float Rain, const FVector3f& SunDirection, float SunPower);
    void UpdateRipples(UMaterialInstanceDynamic& Material, FBridgeRippleFrame&& Frame);
    void Reset();

private:
    TStrongObjectPtr<UBridgeWaterTexture> Displacement;
    TStrongObjectPtr<UBridgeWaterTexture> Derivatives;
    TStrongObjectPtr<UBridgeWaterTexture> SlopeMoments;
    TStrongObjectPtr<UBridgeWaterTexture> Caustics;
    TStrongObjectPtr<UBridgeWaterTexture> RippleSurface;
    TSharedPtr<FBridgeWaterViewExtension, ESPMode::ThreadSafe> Extension;
    double SimulationTime = 0.0;
    float LastEnvClock = 0.0f;
    bool bStarted = false;
};
