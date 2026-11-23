#pragma once

#include "BridgeWire.h"
#include "Components/MeshComponent.h"
#include "PackedNormal.h"
#include "McTerrainRegionComponent.generated.h"

namespace McTerrain
{
    enum ELayer : int32 { Solid, Cutout, CutoutNoShadow, Foliage, Water, Translucent, Vegetation, LayerCount };
    static_assert(Translucent + 1 == BridgeWire::TerrainLayers);

    inline FIntPoint RegionOf(const FIntVector& Section, int32 Width)
    {
        const int32 Mask = Width - 1;
        return FIntPoint((Section.X - (Section.X & Mask)) / Width, (Section.Z - (Section.Z & Mask)) / Width);
    }

    inline int32 SlotOf(const FIntVector& Section, int32 MinSectionY, int32 Width)
    {
        const int32 Mask = Width - 1;
        return (Section.Y - MinSectionY) * Width * Width + (Section.Z & Mask) * Width + (Section.X & Mask);
    }

    void TraceRayTracingRegions(double Now, uint32 Scene, int32 Regions, int32 Sections, int32 Pending,
        uint32 SectionRebuilds, uint32 DroppedColumns);

    struct FTerrainUVs
    {
        uint16 U;
        uint16 V;
        uint16 BlockLight;
        uint16 SkyLight;
    };

    // Conservative UV coverage for atlas-driven cutout shadows. Empty until a masked face is used.
    struct FShadowAtlasCoverage
    {
        void Include(FIntPoint Min, FIntPoint Max, FIntPoint CoordinateScale);
        bool Intersects(const FShadowAtlasCoverage& Other) const;
        bool IsEmpty() const { return Cells.IsEmpty(); }

    private:
        TBitArray<> Cells;
    };

    struct FTangents
    {
        FPackedNormal X;
        FPackedNormal Z;
    };

    // Planar streams match the local vertex factory's typed SRVs and DXR's float3 positions.
    static_assert(sizeof(FVector3f) + sizeof(FTerrainUVs) + sizeof(FTangents) + sizeof(FColor) == 32);

    // One GPU record per wet quad, separate from the 32-byte vertex streams.
    struct FFaceData
    {
        uint32 Words[8]{};
    };

    struct FIndexedFaceData
    {
        int32 Quad;
        FFaceData Data;
    };

    struct FLayerData
    {
        TArray<FVector3f> Positions;
        TArray<FTerrainUVs> UVs;
        TArray<FTangents> Tangents;
        TArray<FColor> Colors;
        TArray<FIndexedFaceData> WetFaces;
    };

    struct FWaterfallContact
    {
        FVector PositionMC;
        float Drop;
        bool bPoolSurface;
    };

    struct FSectionData
    {
        FLayerData Layers[LayerCount];
        TArray<FWaterfallContact> Waterfalls;
        FBox Bounds{ForceInit};
        int32 SeparatedOverlays = 0;
        bool bValid = false;
    };

    struct FSectionUpdate
    {
        int32 Slot;
        FSectionData Data;
    };
}

class FMcTerrainRegionRenderData;

UCLASS()
class UMcTerrainRegionComponent final : public UMeshComponent
{
    GENERATED_BODY()

public:
    UMcTerrainRegionComponent();
    virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
    virtual void BeginDestroy() override;
    virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
    virtual int32 GetNumMaterials() const override { return McTerrain::LayerCount; }

    bool HasSection(int32 Slot) const { return SectionBounds.Contains(Slot); }
    int32 GetSectionCount() const { return SectionBounds.Num(); }
    void UpdateSections(TArray<McTerrain::FSectionUpdate>&& Updates);
    void InvalidateMaskedShadows(const McTerrain::FShadowAtlasCoverage* Changed = nullptr);

private:
    void ReleaseComparisons();

    TMap<int32, FBox> SectionBounds;
    TMap<int32, TSharedPtr<const McTerrain::FSectionData, ESPMode::ThreadSafe>> Comparisons;
    FBox RegionBounds{ForceInit};
    McTerrain::FShadowAtlasCoverage MaskedShadowUVs;
    TSharedPtr<FMcTerrainRegionRenderData, ESPMode::ThreadSafe> RenderData;
};
