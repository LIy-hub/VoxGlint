#pragma once

#include "CoreMinimal.h"
#include "BridgeWire.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class UDirectionalLightComponent;
class ULightComponent;
class ULocalLightComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class USkyAtmosphereComponent;
class UTexture2D;

class FBridgeGlassLighting
{
public:
    struct FLocalShadowCounts
    {
        int32 RayTraced = 0;
        int32 ShadowMapped = 0;
    };

    bool Receive(const FIntVector& Section, BridgeWire::FReader Reader, int32 Size);
    void Drop(const FIntVector& Section);
    void Reset();
    void Update(AActor& Owner, const FVector& CameraCm, UDirectionalLightComponent& Sun,
        const USkyAtmosphereComponent* Atmosphere, TConstArrayView<ULocalLightComponent*> Lights,
        uint64 LocalLightsRevision, bool bHardwareShadows);
    bool IsActive() const { return !Sections.IsEmpty() || !Replacements.IsEmpty() || VolumeTexture.IsValid(); }
    bool IsEnabled() const;
    bool NeedsUpdate() const { return (IsEnabled() && !Sections.IsEmpty()) || !Replacements.IsEmpty() || VolumeTexture.IsValid(); }
    bool HasSections() const { return !Sections.IsEmpty() || !Pending.IsEmpty(); }
    bool Replaces(ULightComponent& Light) const;
    int32 PendingHandoffCount() const { return PendingHandoffs; }
    FLocalShadowCounts LocalShadowCounts() const;
    int32 PlanarGridCount() const { return PackedPlanes; }
    void Forget(ULocalLightComponent& Light);

private:
    struct FVolume
    {
        FVector3f Min, Max, Extinction;
        int32 GridAxis = INDEX_NONE;
        int32 FirstGridCell = INDEX_NONE;
        bool operator==(const FVolume& Other) const = default;
    };

    struct FPendingSection
    {
        uint16 Next = 0;
        uint16 Parts = 0;
        TArray<FVolume> Volumes;
    };

    struct FNode
    {
        FBox3f Bounds{ForceInit};
        int32 Escape = 0;
        int32 Volume = INDEX_NONE;
        int32 GridAxis = INDEX_NONE;
        int32 FirstGridCell = INDEX_NONE;
    };

    struct FEmitterSample
    {
        TStrongObjectPtr<ULightComponent> Light;
        TStrongObjectPtr<UMaterialInstanceDynamic> Filter;
        FVector4f LastRay{ForceInit};
        uint64 UploadedRevision = MAX_uint64;
    };

    struct FReplacement
    {
        TArray<FEmitterSample> Samples;
        bool bSun = false;
        bool bSourceSuppressed = false;
        uint8 ChannelMask = 0;
        bool bHardwareShadows = false;
        float IndirectIntensity = 0.0f;
        float DiffuseScale = 1.0f;
        float SpecularScale = 1.0f;
        bool bAffectsTranslucency = false;
        bool bAffectsReflection = false;
        bool bAffectsGi = false;
    };

    void Rebuild(AActor& Owner, const FVector& CameraCm);
    void BuildNodes(FVolume* Begin, FVolume* End, FVolume* Root);
    bool AppendPlanarGrid(const TArray<FVolume>& Section, const FVector3f& Offset,
        TArray<FVolume>& Gathered, TArray<FVector4f>& GridData);
    bool OverlapsLocalLight(const ULocalLightComponent& Light) const;
    bool OverlapsGridCell(const FNode& Node, const FVector3f& Center, float RadiusSquared) const;
    bool UpdateReplacement(AActor& Owner, ULightComponent& Source, bool bSun, bool bHardwareShadows,
        const FLinearColor& AtmosphereTransmission = FLinearColor::White);
    static bool TryActivateReplacement(ULightComponent& Source, FReplacement& Replacement);
    static void Release(ULightComponent& Source, FReplacement& Replacement, bool bRestore);

    TMap<FIntVector, TArray<FVolume>> Sections;
    TMap<FIntVector, FPendingSection> Pending;
    TMap<ULightComponent*, FReplacement> Replacements;
    TArray<FNode> Nodes;
    TBitArray<> GridOccupancy;
    TStrongObjectPtr<UMaterialInterface> Base;
    TStrongObjectPtr<UTexture2D> VolumeTexture;
    FVector AnchorCm = FVector::ZeroVector;
    uint64 Revision = 0;
    uint64 BuiltRevision = MAX_uint64;
    uint64 LastLocalLightsRevision = MAX_uint64;
    uint64 LastLocalVolumesRevision = MAX_uint64;
    int32 PendingHandoffs = 0;
    int32 PackedPlanes = 0;
    int32 LastLocalRayShadowBudget = MAX_int32;
    bool bLocalHardwareShadows = false;
    bool bLocalUsesRayShadows = false;
    bool bTriedMaterial = false;
    bool bReportedVolumeLimit = false;
};
