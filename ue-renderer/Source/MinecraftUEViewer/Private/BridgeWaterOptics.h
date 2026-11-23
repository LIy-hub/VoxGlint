#pragma once

#include "CoreMinimal.h"

namespace McTerrain { struct FSectionData; }
class FBridgeUnderwaterView;
class FBridgeWaterSun;
struct FBridgeWaterSunInputs;
class UWorld;

class FBridgeWaterOptics
{
public:
    explicit FBridgeWaterOptics(UWorld* World);
    ~FBridgeWaterOptics();

    static bool IsEnabled();
    static bool IsInterfaceEnabled();
    bool UseSceneCaustics() const { return bSceneCaustics; }
    void UpdateLighting(const FBridgeWaterSunInputs& Input);

    void CommitSection(const FIntVector& Section, int32 RegionWidth, int32 MinY, const McTerrain::FSectionData& Data);
    void RemoveSection(const FIntVector& Section);
    void Reset();
    void ResetCamera();
    void UpdateCamera(const FVector& EyeCm, bool bMinecraftWet, uint32 Column, int32 MinY,
        float DeltaSeconds, const FVector3f& Ambient, float Clock, int32 Tier);

    bool IsEyeWet() const { return bEyeWet; }
    float GetExposureWeight() const { return ExposureWeight; }

private:
    struct FWaterQuad
    {
        FVector3f Vertices[4];
        FBox3f Bounds{ForceInit};
        FColor Tint;
        uint8 Role = 0;
    };

    TMap<FIntVector, TArray<FWaterQuad>> Sections;
    TSharedPtr<FBridgeUnderwaterView, ESPMode::ThreadSafe> Extension;
    TUniquePtr<FBridgeWaterSun> Sun;
    FVector LastEye = FVector::ZeroVector;
    FVector3f Tint = FVector3f(0.0497f, 0.183f, 0.955f);
    float ExposureWeight = 0.0f;
    float FilmSeconds = 0.0f;
    bool bHasCamera = false;
    bool bEyeWet = false;
    bool bFilmArmed = false;
    bool bSceneCaustics = false;
};
