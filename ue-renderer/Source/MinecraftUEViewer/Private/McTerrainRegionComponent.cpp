#include "McTerrainRegionComponent.h"
#include "BridgeQuality.h"

#include "Engine/Engine.h"
#include "McTerrainVertexFactory.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Materials/Material.h"
#include "MeshBatch.h"
#include "PrimitiveSceneProxy.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "RayTracingGeometry.h"
#include "RenderUtils.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"

#if RHI_RAYTRACING
#include "RayTracingInstance.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMinecraftTerrainRegion, Log, All);

namespace
{
    using namespace McTerrain;

    // Keep the shared index buffer's ray-traceable primitives contiguous.
    constexpr ELayer IndexLayerOrder[] = {Solid, Cutout, CutoutNoShadow, Foliage, Translucent, Vegetation, Water};
    static_assert(UE_ARRAY_COUNT(IndexLayerOrder) == LayerCount);

    const FString& RayStatisticsPath()
    {
        static const FString Path = []
        {
            FString Result;
            FParse::Value(FCommandLine::Get(), TEXT("BridgeRegionStatsFile="), Result);
            return Result;
        }();
        return Path;
    }

    TAtomic<uint64> RayBlasBuilds{0};
    TAtomic<uint64> RaySubmitted{0};
    TAtomic<uint64> RayOutOfRange{0};

    struct FQuadRange
    {
        int32 Start;
        int32 Count;
    };

    struct FSectionRange : FQuadRange
    {
        int32 FirstIndex = 0;
        FVector Center = FVector::ZeroVector;
    };

    constexpr int32 VerticesPerQuad = 4;
    constexpr int32 IndicesPerQuad = 6;
    constexpr int32 TrianglesPerQuad = 2;
    constexpr int32 MinPageQuads = 512;
    constexpr int32 MaxRegularPageQuads = 8192;
    constexpr uint64 MaxComparisonBytes = 64 * 1024 * 1024;
    uint64 ComparisonBytes = 0; // Game-thread snapshots only.

    enum EStreamChange : uint8
    {
        PositionChanged = 1 << 0,
        UVChanged = 1 << 1,
        TangentChanged = 1 << 2,
        ColorChanged = 1 << 3,
        FaceDataChanged = 1 << 4,
        AllStreams = PositionChanged | UVChanged | TangentChanged | ColorChanged | FaceDataChanged
    };

    struct FRegionUpload
    {
        int32 Slot;
        TSharedPtr<const FSectionData, ESPMode::ThreadSafe> Data;
        uint8 Changed[LayerCount]{};
    };

    uint64 AllocatedBytes(const FSectionData& Data)
    {
        uint64 Bytes = sizeof(FSectionData);
        for (const FLayerData& Layer : Data.Layers)
        {
            Bytes += Layer.Positions.GetAllocatedSize() + Layer.UVs.GetAllocatedSize() +
                Layer.Tangents.GetAllocatedSize() + Layer.Colors.GetAllocatedSize() + Layer.WetFaces.GetAllocatedSize();
        }
        return Bytes;
    }

    template <typename T>
    bool SameBytes(const TArray<T>& A, const TArray<T>& B)
    {
        return A.Num() == B.Num() && (A.IsEmpty() ||
            FMemory::Memcmp(A.GetData(), B.GetData(), A.Num() * sizeof(T)) == 0);
    }

    constexpr uint32 ShoreZMask = (1u << 18) - 1;
    constexpr uint32 ShoreTimeMask = (1u << 28) - 1;
    constexpr uint32 ShoreDecayTicks = 8 * 16;

    bool IsShoreSegment(const uint32* Segment)
    {
        return Segment[0] != Segment[2] || (Segment[1] & ShoreZMask) != (Segment[3] & ShoreZMask);
    }

    uint32 ShoreRetiredAt(const uint32* Segment)
    {
        return (Segment[1] >> 18) | ((Segment[3] >> 18) << 14);
    }

    void RetainShoreHistory(const FLayerData& Previous, FLayerData& Current, uint32 Now)
    {
        if (Previous.WetFaces.IsEmpty() || !SameBytes(Previous.Positions, Current.Positions)) return;
        TArray<FIndexedFaceData> Combined;
        Combined.Reserve(FMath::Max(Previous.WetFaces.Num(), Current.WetFaces.Num()));
        int32 Next = 0;
        for (const FIndexedFaceData& Old : Previous.WetFaces)
        {
            while (Next < Current.WetFaces.Num() && Current.WetFaces[Next].Quad < Old.Quad)
            {
                Combined.Add(Current.WetFaces[Next++]);
            }
            FIndexedFaceData Face{Old.Quad, {}};
            if (Next < Current.WetFaces.Num() && Current.WetFaces[Next].Quad == Old.Quad)
            {
                Face = Current.WetFaces[Next++];
            }
            for (int32 I = 0; I < 8; I += 4)
            {
                const uint32* OldSegment = Old.Data.Words + I;
                if (!IsShoreSegment(OldSegment)) continue;
                const uint32 Before = ShoreRetiredAt(OldSegment);
                if (Before && ((Now - Before) & ShoreTimeMask) >= ShoreDecayTicks) continue;

                const uint32 A = OldSegment[1] & ShoreZMask;
                const uint32 B = OldSegment[3] & ShoreZMask;
                int32 Free = INDEX_NONE;
                bool bAlreadyWet = false;
                for (int32 J = 0; J < 8; J += 4)
                {
                    const uint32* NewSegment = Face.Data.Words + J;
                    if (!IsShoreSegment(NewSegment))
                    {
                        if (Free == INDEX_NONE) Free = J;
                    }
                    else if (NewSegment[0] == OldSegment[0] && (NewSegment[1] & ShoreZMask) == A &&
                        NewSegment[2] == OldSegment[2] && (NewSegment[3] & ShoreZMask) == B)
                    {
                        bAlreadyWet = true;
                    }
                }
                if (!bAlreadyWet && Free != INDEX_NONE)
                {
                    const uint32 Stamp = Before ? Before : FMath::Max(Now, 1u);
                    uint32* NewSegment = Face.Data.Words + Free;
                    NewSegment[0] = OldSegment[0];
                    NewSegment[1] = A | ((Stamp & ((1u << 14) - 1)) << 18);
                    NewSegment[2] = OldSegment[2];
                    NewSegment[3] = B | ((Stamp >> 14) << 18);
                }
            }
            if (IsShoreSegment(Face.Data.Words) || IsShoreSegment(Face.Data.Words + 4)) Combined.Add(Face);
        }
        for (; Next < Current.WetFaces.Num(); ++Next) Combined.Add(Current.WetFaces[Next]);
        Current.WetFaces = MoveTemp(Combined);
    }

    // Shipped terrain uses UV0 and vertex alpha for masked coverage. Light UV and RGB tint only shade it.
    bool SameCoverage(const FLayerData& Old, const FLayerData& New, uint8 Changed)
    {
        if (Changed & PositionChanged)
        {
            return false;
        }
        if ((Changed & (UVChanged | ColorChanged)) == 0)
        {
            return true;
        }
        for (int32 Index = 0; Index < New.Positions.Num(); ++Index)
        {
            if (((Changed & UVChanged) && (Old.UVs[Index].U != New.UVs[Index].U || Old.UVs[Index].V != New.UVs[Index].V)) ||
                ((Changed & ColorChanged) && Old.Colors[Index].A != New.Colors[Index].A))
            {
                return false;
            }
        }
        return true;
    }

    bool LayerCastsShadow(int32 Layer)
    {
        return Layer == Solid || Layer == Cutout || Layer == Foliage;
    }

    void IncludeMaskedFaces(FShadowAtlasCoverage& Coverage, const TArray<FTerrainUVs>& UVs)
    {
        for (int32 I = 0; I < UVs.Num(); I += VerticesPerQuad)
        {
            FIntPoint Min(UVs[I].U, UVs[I].V);
            FIntPoint Max = Min;
            for (int32 V = 1; V < VerticesPerQuad; ++V)
            {
                const FIntPoint UV(UVs[I + V].U, UVs[I + V].V);
                Min = Min.ComponentMin(UV);
                Max = Max.ComponentMax(UV);
            }
            Coverage.Include(Min, Max, FIntPoint(MAX_uint16, MAX_uint16));
        }
    }

    class FVertexPage
    {
    public:
        FVertexPage(FRHICommandListBase& RHICmdList, ERHIFeatureLevel::Type FeatureLevel, int32 InCapacity, bool bInWater)
            : Capacity(InCapacity), VertexFactory(FeatureLevel), bWater(bInWater)
        {
            const uint32 Vertices = Capacity * VerticesPerQuad;
            const auto MakeStream = [&](FVertexBuffer& Buffer, const TCHAR* Name, uint32 Stride, EPixelFormat Format)
            {
                Buffer.InitResource(RHICmdList);
                Buffer.VertexBufferRHI = RHICmdList.CreateBuffer(
                    FRHIBufferCreateDesc::CreateVertex(Name, Vertices * Stride)
                    .AddUsage(BUF_Static | BUF_ShaderResource).DetermineInitialState());
                return RHICmdList.CreateShaderResourceView(Buffer.VertexBufferRHI,
                    FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(Format));
            };
            PositionSRV = MakeStream(Positions, TEXT("MinecraftTerrain.Positions"), sizeof(FVector3f), PF_R32_FLOAT);
            EmptyFaceData = RHICmdList.CreateShaderResourceView(Positions.VertexBufferRHI,
                FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R32_UINT));
            VertexFactory.BindFaceData(EmptyFaceData, 0);
            UVSRV = MakeStream(UVs, TEXT("MinecraftTerrain.UVs"), sizeof(FTerrainUVs), PF_G16R16);
            TangentSRV = MakeStream(Tangents, TEXT("MinecraftTerrain.Tangents"), sizeof(FTangents), PF_R8G8B8A8_SNORM);
            ColorSRV = MakeStream(Colors, TEXT("MinecraftTerrain.Colors"), sizeof(FColor), PF_R8G8B8A8);

            constexpr EVertexStreamUsage Manual = EVertexStreamUsage::ManualFetch;
            FLocalVertexFactory::FDataType Data;
            Data.PositionComponent = FVertexStreamComponent(&Positions, 0, sizeof(FVector3f), VET_Float3);
            Data.PositionComponentSRV = PositionSRV;
            Data.TangentBasisComponents[0] = FVertexStreamComponent(&Tangents, 0, sizeof(FTangents), VET_PackedNormal, Manual);
            Data.TangentBasisComponents[1] = FVertexStreamComponent(&Tangents, sizeof(FPackedNormal), sizeof(FTangents), VET_PackedNormal, Manual);
            Data.TangentsSRV = TangentSRV;
            Data.TextureCoordinates.Add(FVertexStreamComponent(&UVs, 0, sizeof(FTerrainUVs), VET_UShort2N, Manual));
            Data.TextureCoordinates.Add(FVertexStreamComponent(&UVs, STRUCT_OFFSET(FTerrainUVs, BlockLight),
                sizeof(FTerrainUVs), VET_UShort2N, Manual));
            Data.TextureCoordinatesSRV = UVSRV;
            Data.NumTexCoords = 2;
            Data.ColorComponent = FVertexStreamComponent(&Colors, 0, sizeof(FColor), VET_Color, Manual);
            Data.ColorComponentsSRV = ColorSRV;
            Data.ColorIndexMask = ~0u;
            VertexFactory.SetData(RHICmdList, Data);
            VertexFactory.InitResource(RHICmdList);
        }

        ~FVertexPage()
        {
            VertexFactory.ReleaseResource();
            Positions.ReleaseResource();
            UVs.ReleaseResource();
            Tangents.ReleaseResource();
            Colors.ReleaseResource();
        }

        int32 Allocate(int32 Count)
        {
            for (int32 Index = 0; Index < Free.Num(); ++Index)
            {
                FQuadRange& Range = Free[Index];
                if (Range.Count >= Count)
                {
                    const int32 Start = Range.Start;
                    Range.Start += Count;
                    Range.Count -= Count;
                    if (Range.Count == 0)
                    {
                        Free.RemoveAt(Index, EAllowShrinking::No);
                    }
                    return Start;
                }
            }
            if (Count > Capacity - HighWater)
            {
                return INDEX_NONE;
            }
            const int32 Start = HighWater;
            HighWater += Count;
            return Start;
        }

        void FreeRange(FQuadRange Range)
        {
            WetRanges.Remove(Range.Start);
            ReleaseDryFaceData();
            int32 Index = 0;
            while (Index < Free.Num() && Free[Index].Start < Range.Start)
            {
                ++Index;
            }
            if (Index > 0 && Free[Index - 1].Start + Free[Index - 1].Count == Range.Start)
            {
                --Index;
                Range.Start = Free[Index].Start;
                Range.Count += Free[Index].Count;
                Free.RemoveAt(Index, EAllowShrinking::No);
            }
            if (Index < Free.Num() && Range.Start + Range.Count == Free[Index].Start)
            {
                Range.Count += Free[Index].Count;
                Free.RemoveAt(Index, EAllowShrinking::No);
            }
            if (Range.Start + Range.Count == HighWater)
            {
                HighWater = Range.Start;
            }
            else
            {
                Free.Insert(Range, Index);
            }
        }

        void Upload(FRHICommandListBase& RHICmdList, int32 Start, const FLayerData& Data, uint8 Changed)
        {
            const auto UploadStream = [&](FVertexBuffer& Buffer, const auto& Values)
            {
                const uint32 Stride = Values.GetTypeSize();
                const uint32 Bytes = Values.Num() * Stride;
                void* Target = RHICmdList.LockBuffer(Buffer.VertexBufferRHI, Start * VerticesPerQuad * Stride, Bytes, RLM_WriteOnly);
                FMemory::Memcpy(Target, Values.GetData(), Bytes);
                RHICmdList.UnlockBuffer(Buffer.VertexBufferRHI);
            };
            if (Changed & PositionChanged) UploadStream(Positions, Data.Positions);
            if (Changed & UVChanged) UploadStream(UVs, Data.UVs);
            if (Changed & TangentChanged) UploadStream(Tangents, Data.Tangents);
            if (Changed & ColorChanged) UploadStream(Colors, Data.Colors);
            if ((Changed & FaceDataChanged) == 0) return;
            if (Data.WetFaces.IsEmpty()) WetRanges.Remove(Start);
            else WetRanges.Add(Start);
            ReleaseDryFaceData();
            if (WetRanges.IsEmpty()) return;

            if (!FaceBuffer)
            {
                const uint32 Bytes = Capacity * sizeof(FFaceData);
                FaceBuffer = RHICmdList.CreateBuffer(FRHIBufferCreateDesc::CreateVertex(TEXT("MinecraftTerrain.WetFaces"), Bytes)
                    .AddUsage(BUF_Static | BUF_ShaderResource).DetermineInitialState());
                void* Initial = RHICmdList.LockBuffer(FaceBuffer, 0, Bytes, RLM_WriteOnly);
                FMemory::Memzero(Initial, Bytes);
                RHICmdList.UnlockBuffer(FaceBuffer);
                FaceData = RHICmdList.CreateShaderResourceView(FaceBuffer,
                    FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R32_UINT));
                VertexFactory.BindFaceData(FaceData, bWater ? 2 : 1);
            }

            const uint32 Bytes = Data.Positions.Num() / VerticesPerQuad * sizeof(FFaceData);
            if (Bytes > 0)
            {
                TArray<FFaceData> UploadFaces;
                UploadFaces.SetNumZeroed(Data.Positions.Num() / VerticesPerQuad);
                for (const FIndexedFaceData& Face : Data.WetFaces) UploadFaces[Face.Quad] = Face.Data;
                void* Target = RHICmdList.LockBuffer(FaceBuffer, Start * sizeof(FFaceData), Bytes, RLM_WriteOnly);
                FMemory::Memcpy(Target, UploadFaces.GetData(), Bytes);
                RHICmdList.UnlockBuffer(FaceBuffer);
            }
        }

        const int32 Capacity;
        int32 HighWater = 0;
        int32 FirstIndex = 0;
        int32 NumQuads = 0;
        TMap<int32, FSectionRange> Sections;
        FVertexBuffer Positions;
        FMcTerrainVertexFactory VertexFactory;

    private:
        void ReleaseDryFaceData()
        {
            if (WetRanges.IsEmpty() && FaceBuffer)
            {
                VertexFactory.BindFaceData(EmptyFaceData, 0);
                FaceData.SafeRelease();
                FaceBuffer.SafeRelease();
            }
        }

        const bool bWater;
        TSet<int32> WetRanges;
        FBufferRHIRef FaceBuffer;
        FShaderResourceViewRHIRef EmptyFaceData, FaceData;
        TArray<FQuadRange> Free;
        FVertexBuffer UVs;
        FVertexBuffer Tangents;
        FVertexBuffer Colors;
        FShaderResourceViewRHIRef PositionSRV, UVSRV, TangentSRV, ColorSRV;
    };
}

class FMcTerrainRegionRenderData
{
public:
    ~FMcTerrainRegionRenderData()
    {
        check(IsInRenderingThread());
#if RHI_RAYTRACING
        Geometry.ReleaseResource();
#endif
        IndexBuffer.ReleaseResource();
    }

    void Apply(FRHICommandListBase& RHICmdList, ERHIFeatureLevel::Type FeatureLevel,
        TConstArrayView<FRegionUpload> Updates, bool bInvalidateShadow)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftTerrainUpload);
        bool bLayoutChanged = false;
        bool bRayLayoutChanged = false;
        bool bPositionsChanged = false;
        for (const FRegionUpload& Update : Updates)
        {
            FSlot& Slot = Slots.FindOrAdd(Update.Slot);
            for (int32 Layer = 0; Layer < LayerCount; ++Layer)
            {
                const FLayerData& Data = Update.Data->Layers[Layer];
                const int32 Count = Data.Positions.Num() / VerticesPerQuad;
                FVertexPage*& Page = Slot.Pages[Layer];
                const uint8 Changed = Update.Changed[Layer];
                if (Page)
                {
                    FSectionRange& Range = Page->Sections.FindChecked(Update.Slot);
                    if (Range.Count == Count)
                    {
                        Range.Center = Update.Data->Bounds.GetCenter();
                        Page->Upload(RHICmdList, Range.Start, Data, Changed);
                        bPositionsChanged |= Layer != Water && (Changed & PositionChanged) != 0;
                        continue;
                    }
                    Page->FreeRange(Range);
                    Page->Sections.Remove(Update.Slot);
                    Page = nullptr;
                    bLayoutChanged = true;
                    bRayLayoutChanged |= Layer != Water;
                }
                if (Count > 0)
                {
                    int32 Start = INDEX_NONE;
                    for (const TUniquePtr<FVertexPage>& Candidate : Pages[Layer])
                    {
                        Start = Candidate->Allocate(Count);
                        if (Start != INDEX_NONE)
                        {
                            Page = Candidate.Get();
                            break;
                        }
                    }
                    if (!Page)
                    {
                        const int32 RegularCapacity = FMath::Clamp(MinPageQuads << FMath::Min(Pages[Layer].Num(), 4),
                            MinPageQuads, MaxRegularPageQuads);
                        const int32 Capacity = FMath::Max(RegularCapacity, int32(FMath::RoundUpToPowerOfTwo(uint32(Count))));
                        Page = Pages[Layer].Emplace_GetRef(MakeUnique<FVertexPage>(RHICmdList, FeatureLevel, Capacity, Layer == Water)).Get();
                        Start = Page->Allocate(Count);
                    }
                    Page->Sections.Add(Update.Slot, FSectionRange{{Start, Count}, 0, Update.Data->Bounds.GetCenter()});
                    Page->Upload(RHICmdList, Start, Data, AllStreams);
                    bLayoutChanged = true;
                    bRayLayoutChanged |= Layer != Water;
                }
            }
            if (!Update.Data->Bounds.IsValid)
            {
                Slots.Remove(Update.Slot);
            }
        }
        if (bLayoutChanged)
        {
            RebuildIndexRanges(RHICmdList);
        }
        bool bRebuilt = false;
        double BuildMs = 0;
#if RHI_RAYTRACING
        if (IsRayTracingAllowed() && (bRayLayoutChanged || bPositionsChanged || !bInitializedGeometry))
        {
            const double Start = FPlatformTime::Seconds();
            RebuildRayTracingGeometry(RHICmdList);
            bInitializedGeometry = true;
            bRebuilt = true;
            BuildMs = (FPlatformTime::Seconds() - Start) * 1000.0;
        }
#endif
        static const bool bTrace = FParse::Param(FCommandLine::Get(), TEXT("BridgeRegionTrace"));
        if (bTrace)
        {
            UE_LOG(LogMinecraftTerrainRegion, Display,
                TEXT("TerrainRegion updates=%d layout=%d positions=%d shadowNotify=%d blas=%d blasCpuMs=%.3f"),
                Updates.Num(), bLayoutChanged, bPositionsChanged, bInvalidateShadow, bRebuilt, BuildMs);
        }
    }

    TArray<TUniquePtr<FVertexPage>> Pages[LayerCount];
    FIndexBuffer IndexBuffer;
#if RHI_RAYTRACING
    FRayTracingGeometry Geometry;
    bool bInitializedGeometry = false;
#endif

private:
    struct FSlot
    {
        FVertexPage* Pages[LayerCount]{};
    };

    void RebuildIndexRanges(FRHICommandListBase& RHICmdList)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftTerrainIndices);
        int32 IndexCount = 0;
        for (ELayer Layer : IndexLayerOrder)
        {
            Pages[Layer].RemoveAll([](const TUniquePtr<FVertexPage>& Page) { return Page->Sections.IsEmpty(); });
            for (const TUniquePtr<FVertexPage>& Page : Pages[Layer])
            {
                Page->Sections.ValueSort([](const FSectionRange& A, const FSectionRange& B) { return A.Start < B.Start; });
                Page->FirstIndex = IndexCount;
                for (auto& Pair : Page->Sections)
                {
                    FSectionRange& Range = Pair.Value;
                    Range.FirstIndex = IndexCount;
                    IndexCount += Range.Count * IndicesPerQuad;
                }
                Page->NumQuads = (IndexCount - Page->FirstIndex) / IndicesPerQuad;
            }
        }
        if (IndexCount == 0)
        {
            IndexBuffer.IndexBufferRHI.SafeRelease();
            return;
        }
        const uint32 Bytes = IndexCount * sizeof(uint32);
        if (!IndexBuffer.IndexBufferRHI || Bytes > IndexBuffer.IndexBufferRHI->GetSize())
        {
            if (!IndexBuffer.IsInitialized())
            {
                IndexBuffer.InitResource(RHICmdList);
            }
            const uint32 Capacity = FMath::RoundUpToPowerOfTwo(FMath::Max(Bytes, 4096u));
            IndexBuffer.IndexBufferRHI = RHICmdList.CreateBuffer(
                FRHIBufferCreateDesc::CreateIndex<uint32>(TEXT("MinecraftTerrain.Indices"), Capacity / sizeof(uint32))
                .AddUsage(BUF_Static | BUF_ShaderResource).DetermineInitialState());
        }
        uint32* Indices = static_cast<uint32*>(RHICmdList.LockBuffer(IndexBuffer.IndexBufferRHI, 0, Bytes, RLM_WriteOnly));
        for (ELayer Layer : IndexLayerOrder)
        {
            for (const TUniquePtr<FVertexPage>& Page : Pages[Layer])
            {
                for (const auto& Pair : Page->Sections)
                {
                    const FSectionRange& Range = Pair.Value;
                    for (uint32 V = Range.Start * VerticesPerQuad, End = V + Range.Count * VerticesPerQuad;
                        V < End; V += VerticesPerQuad)
                    {
                        *Indices++ = V;
                        *Indices++ = V + 1;
                        *Indices++ = V + 2;
                        *Indices++ = V;
                        *Indices++ = V + 2;
                        *Indices++ = V + 3;
                    }
                }
            }
        }
        RHICmdList.UnlockBuffer(IndexBuffer.IndexBufferRHI);
    }

#if RHI_RAYTRACING
    void RebuildRayTracingGeometry(FRHICommandListBase& RHICmdList)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftTerrainBLAS);
        Geometry.ReleaseResource();
        if (!IndexBuffer.IndexBufferRHI)
        {
            Geometry.SetInitializer(FRayTracingGeometryInitializer{});
            return;
        }
        FRayTracingGeometryInitializer Initializer;
        Initializer.DebugName = FName(TEXT("MinecraftTerrainRegion"));
        Initializer.IndexBuffer = IndexBuffer.IndexBufferRHI;
        Initializer.GeometryType = RTGT_Triangles;
        Initializer.bFastBuild = true;
        Initializer.bAllowUpdate = false;
        for (ELayer Layer : IndexLayerOrder)
        {
            if (Layer == Water) continue;
            for (const TUniquePtr<FVertexPage>& Page : Pages[Layer])
            {
                FRayTracingGeometrySegment& Segment = Initializer.Segments.AddDefaulted_GetRef();
                Segment.VertexBuffer = Page->Positions.VertexBufferRHI;
                Segment.VertexBufferStride = sizeof(FVector3f);
                Segment.MaxVertices = Page->HighWater * VerticesPerQuad;
                Segment.FirstPrimitive = Page->FirstIndex / 3;
                Segment.NumPrimitives = Page->NumQuads * TrianglesPerQuad;
                // Mixed shadow flags need the material hit group, even for an opaque surface.
                Initializer.TotalPrimitiveCount += Segment.NumPrimitives;
            }
        }
        if (Initializer.Segments.IsEmpty())
        {
            Geometry.SetInitializer(FRayTracingGeometryInitializer{});
            return;
        }
        Geometry.SetInitializer(MoveTemp(Initializer));
        Geometry.InitResource(RHICmdList);
        if (!RayStatisticsPath().IsEmpty()) ++RayBlasBuilds;
    }
#endif

    TMap<int32, FSlot> Slots;
};

namespace
{
    class FMcTerrainSceneProxy final : public FPrimitiveSceneProxy
    {
    public:
        FMcTerrainSceneProxy(const UMcTerrainRegionComponent& Component,
            TSharedPtr<FMcTerrainRegionRenderData, ESPMode::ThreadSafe> InData)
            : FPrimitiveSceneProxy(&Component), Data(MoveTemp(InData)),
              MaterialRelevance(Component.GetMaterialRelevance(GetScene().GetShaderPlatform()))
        {
            for (int32 Layer = 0; Layer < LayerCount; ++Layer)
            {
                UMaterialInterface* Material = Component.GetMaterial(Layer);
                Materials[Layer] = (Material ? Material : UMaterial::GetDefaultMaterial(MD_Surface))->GetRenderProxy();
            }
            // All raster batches share the transform and bounds of this registered region.
            EnableGPUSceneSupportFlags();
            // Positions and cutout coverage change only on bridge updates, which invalidate the
            // transform even when the bounds stay the same. WPO is handled separately by UE.
            bHasDeformableMesh = false;
PRAGMA_DISABLE_DEPRECATION_WARNINGS
            SetCanSkipRedundantTransformUpdates(false);
PRAGMA_ENABLE_DEPRECATION_WARNINGS
        }

        virtual SIZE_T GetTypeHash() const override
        {
            static int32 Unique;
            return reinterpret_cast<SIZE_T>(&Unique);
        }

        virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }

        virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
        {
            FPrimitiveViewRelevance Result;
            Result.bDrawRelevance = IsShown(View);
            Result.bShadowRelevance = IsShadowCast(View);
            Result.bDynamicRelevance = true;
            Result.bRenderInMainPass = ShouldRenderInMainPass();
            Result.bRenderInDepthPass = ShouldRenderInDepthPass();
            Result.bRenderCustomDepth = ShouldRenderCustomDepth();
            Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
            MaterialRelevance.SetPrimitiveViewRelevance(Result);
            // UE can defer opaque depth writes to the velocity pass for the whole region.
            Result.bVelocityRelevance = DrawsVelocity() && Result.bOpaque && Result.bRenderInMainPass;
            return Result;
        }

        virtual bool CanBeOccluded() const override { return !MaterialRelevance.bDisableDepthTest; }

        virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
            uint32 VisibilityMap, FMeshElementCollector& Collector) const override
        {
            if (!Data->IndexBuffer.IndexBufferRHI)
            {
                return;
            }
            for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
            {
                if (!(VisibilityMap & (1u << ViewIndex)))
                {
                    continue;
                }
                TArray<TPair<const FVertexPage*, const FSectionRange*>, TInlineAllocator<32>> TranslucentSections;
                for (int32 Layer = 0; Layer < LayerCount; ++Layer)
                {
                    for (const TUniquePtr<FVertexPage>& Page : Data->Pages[Layer])
                    {
                        if (Layer == Translucent)
                        {
                            for (const auto& Pair : Page->Sections)
                            {
                                TranslucentSections.Emplace(Page.Get(), &Pair.Value);
                            }
                            continue;
                        }
                        FMeshBatch& Batch = Collector.AllocateMesh();
                        FillBatch(Batch, *Page, Layer, Page->FirstIndex, Page->NumQuads, 0, Page->HighWater);
                        Collector.AddMesh(ViewIndex, Batch);
                    }
                }
                const FVector Origin = GetLocalToWorld().InverseTransformPosition(Views[ViewIndex]->ViewMatrices.GetViewOrigin());
                TranslucentSections.Sort([&](const auto& A, const auto& B)
                {
                    return FVector::DistSquared(A.Value->Center, Origin) > FVector::DistSquared(B.Value->Center, Origin);
                });
                for (const auto& Pair : TranslucentSections)
                {
                    const FSectionRange& Range = *Pair.Value;
                    FMeshBatch& Batch = Collector.AllocateMesh();
                    FillBatch(Batch, *Pair.Key, Translucent, Range.FirstIndex, Range.Count, Range.Start, Range.Start + Range.Count);
                    Collector.AddMesh(ViewIndex, Batch);
                }
            }
        }

#if RHI_RAYTRACING
        virtual bool IsRayTracingRelevant() const override { return true; }
        virtual bool HasRayTracingRepresentation() const override { return true; }

        virtual void GetDynamicRayTracingInstances(FRayTracingInstanceCollector& Collector) override
        {
            if (!Data->Geometry.IsValid())
            {
                return;
            }
            const auto& Views = Collector.GetViews();
            const int32 Range = BridgeQuality::TerrainRayRangeOnRenderThread();
            const double LimitSquared = FMath::Square(Range * 16.0 * BridgeWire::BlockSize);
            const FBox WorldBounds = GetBounds().GetBox();
            const bool bTrace = !RayStatisticsPath().IsEmpty();
            uint32 Accepted = Collector.GetVisibilityMap();
            if (Range > 0)
            {
                for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
                {
                    const uint32 ViewBit = 1u << ViewIndex;
                    if (!(Accepted & ViewBit)) continue;
                    const FVector Origin = Views[ViewIndex]->ViewMatrices.GetViewOrigin();
                    // Restrict XY distance to the region's nearest point. Geometry remains available to rasterization.
                    const double X = FMath::Max3(WorldBounds.Min.X - Origin.X, Origin.X - WorldBounds.Max.X, 0.0);
                    const double Y = FMath::Max3(WorldBounds.Min.Y - Origin.Y, Origin.Y - WorldBounds.Max.Y, 0.0);
                    if (FMath::Square(X) + FMath::Square(Y) > LimitSquared)
                    {
                        Accepted &= ~ViewBit;
                        if (bTrace) ++RayOutOfRange;
                    }
                }
            }
            if (!Accepted) return;
            auto& Uniform = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
            FPrimitiveUniformShaderParametersBuilder Builder;
            BuildUniformShaderParameters(Builder);
            Uniform.Set(Collector.GetRHICommandList(), Builder);
            FRayTracingInstance Instance;
            Instance.Geometry = &Data->Geometry;
            Instance.InstanceTransforms.Add(GetLocalToWorld());
            for (ELayer Layer : IndexLayerOrder)
            {
                if (Layer == Water) continue;
                for (const TUniquePtr<FVertexPage>& Page : Data->Pages[Layer])
                {
                    const int32 Segment = Instance.Materials.Num();
                    FMeshBatch& Batch = Instance.Materials.AddDefaulted_GetRef();
                    FillBatch(Batch, *Page, Layer, Page->FirstIndex, Page->NumQuads, 0, Page->HighWater);
                    Batch.Elements[0].PrimitiveUniformBufferResource = &Uniform.UniformBuffer;
                    Batch.SegmentIndex = Segment;
                }
            }
            for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
            {
                if (Accepted & (1u << ViewIndex))
                {
                    Collector.AddRayTracingInstance(ViewIndex, Instance);
                    if (bTrace) ++RaySubmitted;
                }
            }
        }
#endif

    private:
        void FillBatch(FMeshBatch& Batch, const FVertexPage& Page, int32 Layer,
            int32 FirstIndex, int32 Quads, int32 FirstQuad, int32 EndQuad) const
        {
            Batch.VertexFactory = &Page.VertexFactory;
            Batch.MaterialRenderProxy = Materials[Layer];
            Batch.ReverseCulling = IsLocalToWorldDeterminantNegative();
            Batch.Type = PT_TriangleList;
            Batch.DepthPriorityGroup = SDPG_World;
            Batch.CastShadow = LayerCastsShadow(Layer);
            Batch.CastRayTracedShadow = Batch.CastShadow;
            Batch.bCanApplyViewModeOverrides = false;
            FMeshBatchElement& Element = Batch.Elements[0];
            Element.IndexBuffer = &Data->IndexBuffer;
            Element.FirstIndex = FirstIndex;
            Element.NumPrimitives = Quads * TrianglesPerQuad;
            Element.MinVertexIndex = FirstQuad * VerticesPerQuad;
            Element.MaxVertexIndex = EndQuad * VerticesPerQuad - 1;
        }

        TSharedPtr<FMcTerrainRegionRenderData, ESPMode::ThreadSafe> Data;
        const FMaterialRelevance MaterialRelevance;
        const FMaterialRenderProxy* Materials[LayerCount]{};
    };
}

void McTerrain::TraceRayTracingRegions(double Now, uint32 Scene, int32 Regions, int32 Sections, int32 Pending,
    uint32 SectionRebuilds, uint32 DroppedColumns)
{
    const FString& Path = RayStatisticsPath();
    static double NextWrite = 0;
    if (Path.IsEmpty() || Now < NextWrite) return;
    NextWrite = Now + 0.5;
    const FString Line = FString::Printf(TEXT("Scene=%u RegionChunks=%d Regions=%d Sections=%d Pending=%d ")
        TEXT("Rebuilds=%u Drops=%u BlasBuilds=%llu RaySubmitted=%llu RayOutOfRange=%llu\n"),
        Scene, BridgeQuality::TerrainRegionEdge(), Regions, Sections, Pending, SectionRebuilds, DroppedColumns,
        RayBlasBuilds.Load(), RaySubmitted.Load(), RayOutOfRange.Load());
    FFileHelper::SaveStringToFile(Line, *Path);
}

UMcTerrainRegionComponent::UMcTerrainRegionComponent()
{
    SetMobility(EComponentMobility::Movable);
    SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetCastShadow(true);
    bAffectDistanceFieldLighting = false;
    bUseAsOccluder = true;
    PrimaryComponentTick.bCanEverTick = false;
}

FPrimitiveSceneProxy* UMcTerrainRegionComponent::CreateSceneProxy()
{
    return RenderData ? new FMcTerrainSceneProxy(*this, RenderData) : nullptr;
}

void UMcTerrainRegionComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
    ReleaseComparisons();
    RenderData.Reset();
    Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UMcTerrainRegionComponent::BeginDestroy()
{
    ReleaseComparisons();
    Super::BeginDestroy();
}

void UMcTerrainRegionComponent::ReleaseComparisons()
{
    for (const auto& Pair : Comparisons)
    {
        ComparisonBytes -= AllocatedBytes(*Pair.Value);
    }
    Comparisons.Reset();
}

void McTerrain::FShadowAtlasCoverage::Include(FIntPoint Min, FIntPoint Max, FIntPoint CoordinateScale)
{
    constexpr int32 Edge = 128;
    const auto Cell = [](int32 Coord, int32 Scale) { return FMath::Clamp(Coord * Edge / Scale, 0, Edge - 1); };
    if (Cells.IsEmpty()) Cells.Init(false, Edge * Edge);
    const int32 X = Cell(Min.X, CoordinateScale.X);
    const int32 Width = Cell(Max.X, CoordinateScale.X) - X + 1;
    const int32 LastRow = Cell(Max.Y, CoordinateScale.Y);
    for (int32 Y = Cell(Min.Y, CoordinateScale.Y); Y <= LastRow; ++Y)
    {
        Cells.SetRange(Y * Edge + X, Width, true);
    }
}

bool McTerrain::FShadowAtlasCoverage::Intersects(const FShadowAtlasCoverage& Other) const
{
    return !Cells.IsEmpty() && !Other.Cells.IsEmpty() && static_cast<bool>(TConstDualSetBitIterator<>(Cells, Other.Cells));
}

void UMcTerrainRegionComponent::InvalidateMaskedShadows(const McTerrain::FShadowAtlasCoverage* Changed)
{
    if (IsRegistered() && !MaskedShadowUVs.IsEmpty() && (!Changed || MaskedShadowUVs.Intersects(*Changed)))
    {
        MarkRenderTransformDirty();
    }
}

FBoxSphereBounds UMcTerrainRegionComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    return RegionBounds.IsValid ? FBoxSphereBounds(RegionBounds).TransformBy(LocalToWorld)
        : FBoxSphereBounds(LocalToWorld.GetTranslation(), FVector::ZeroVector, 0);
}

void UMcTerrainRegionComponent::UpdateSections(TArray<McTerrain::FSectionUpdate>&& Updates)
{
    TArray<FRegionUpload> Uploads;
    Uploads.Reserve(Updates.Num());
    bool bInvalidateShadow = false;
    const uint32 ShoreTime = uint32(GetWorld()->GetTimeSeconds() * 16.0) & ShoreTimeMask;
    for (FSectionUpdate& Update : Updates)
    {
        const TSharedPtr<const FSectionData, ESPMode::ThreadSafe> Old = Comparisons.FindRef(Update.Slot);
        if (Old)
        {
            ComparisonBytes -= AllocatedBytes(*Old);
            Comparisons.Remove(Update.Slot);
        }
        if (Old)
        {
            for (int32 Layer = 0; Layer < LayerCount; ++Layer)
            {
                if (Layer != Water) RetainShoreHistory(Old->Layers[Layer], Update.Data.Layers[Layer], ShoreTime);
            }
        }
        FRegionUpload Upload{Update.Slot, MakeShared<FSectionData, ESPMode::ThreadSafe>(MoveTemp(Update.Data))};
        const FSectionData& New = *Upload.Data;
        const FBox* OldBounds = SectionBounds.Find(Update.Slot);
        const bool bBoundsChanged = New.Bounds.IsValid != !!OldBounds ||
            (OldBounds && !(New.Bounds.Min == OldBounds->Min && New.Bounds.Max == OldBounds->Max));
        bool bChanged = bBoundsChanged;
        bInvalidateShadow |= bBoundsChanged;
        for (int32 Layer = 0; Layer < LayerCount; ++Layer)
        {
            const auto& Next = New.Layers[Layer];
            uint8 Changed = 0;
            if (Old)
            {
                const auto& Prev = Old->Layers[Layer];
                if (!SameBytes(Prev.Positions, Next.Positions)) Changed |= PositionChanged;
                if (!SameBytes(Prev.UVs, Next.UVs)) Changed |= UVChanged;
                if (!SameBytes(Prev.Tangents, Next.Tangents)) Changed |= TangentChanged;
                if (!SameBytes(Prev.Colors, Next.Colors)) Changed |= ColorChanged;
                if (!SameBytes(Prev.WetFaces, Next.WetFaces)) Changed |= FaceDataChanged;
                if ((Changed & PositionChanged) || (Changed && LayerCastsShadow(Layer) && !SameCoverage(Prev, Next, Changed)))
                {
                    bInvalidateShadow = true;
                }
            }
            else if (!Next.Positions.IsEmpty() || OldBounds)
            {
                Changed = AllStreams;
                bInvalidateShadow = true;
            }
            if ((Changed & UVChanged) && (Layer == Cutout || Layer == Foliage))
            {
                // Keeping the union across edits can only cause extra invalidations, never stale shadows.
                IncludeMaskedFaces(MaskedShadowUVs, Next.UVs);
            }
            Upload.Changed[Layer] = Changed;
            bChanged |= Changed != 0;
        }
        if (New.Bounds.IsValid)
        {
            SectionBounds.Add(Update.Slot, New.Bounds);
            const uint64 Bytes = AllocatedBytes(New);
            if (ComparisonBytes + Bytes <= MaxComparisonBytes)
            {
                Comparisons.Add(Update.Slot, Upload.Data);
                ComparisonBytes += Bytes;
            }
        }
        else
        {
            SectionBounds.Remove(Update.Slot);
        }
        if (bChanged)
        {
            Uploads.Add(MoveTemp(Upload));
        }
    }
    if (Uploads.IsEmpty())
    {
        return;
    }
    RegionBounds.Init();
    for (const auto& Pair : SectionBounds)
    {
        RegionBounds += Pair.Value;
    }
    if (!RenderData)
    {
        RenderData = MakeShareable(new FMcTerrainRegionRenderData(), [](FMcTerrainRegionRenderData* Data)
        {
            ENQUEUE_RENDER_COMMAND(DeleteMinecraftTerrainRegion)([Data](FRHICommandListBase&) { delete Data; });
        });
    }
    const ERHIFeatureLevel::Type FeatureLevel = GetWorld()->GetFeatureLevel();
    ENQUEUE_RENDER_COMMAND(UploadMinecraftTerrainRegion)(
        [Data = RenderData, Uploads = MoveTemp(Uploads), FeatureLevel, bInvalidateShadow](FRHICommandListImmediate& RHICmdList)
        {
            Data->Apply(RHICmdList, FeatureLevel, Uploads, bInvalidateShadow);
        });
    if (IsRegistered() && bInvalidateShadow)
    {
        MarkRenderTransformDirty();
    }
}
