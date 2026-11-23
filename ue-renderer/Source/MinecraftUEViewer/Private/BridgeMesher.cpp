#include "BridgeViewer.h"
#include "BridgeQuality.h"

#include "Async/Async.h"
#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureLODSettings.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "TextureResource.h"

#include <tuple>

namespace
{
    using namespace BridgeWire;

    const TCHAR* const AtlasParameters[] = {TEXT("VoxelAtlas"), TEXT("NormalAtlas"), TEXT("PbrAtlas")};
    static_assert(UE_ARRAY_COUNT(AtlasParameters) == int32(ETerrainAtlas::Count));

    struct FQuadSurface
    {
        FVector3f Corners[4];

        explicit FQuadSurface(const FVector3f* Vertices)
        {
            // Match cyclic permutations while preserving the face's winding.
            int32 First = 0;
            for (int32 I = 1; I < 4; ++I)
            {
                const FVector3f& A = Vertices[I];
                const FVector3f& B = Vertices[First];
                if (std::tie(A.X, A.Y, A.Z) < std::tie(B.X, B.Y, B.Z))
                {
                    First = I;
                }
            }
            for (int32 I = 0; I < 4; ++I)
            {
                const FVector3f& Corner = Vertices[(First + I) % 4];
                // FVector compares signed zeros equal and hashes the underlying bytes.
                Corners[I] = FVector3f(Corner.X == 0 ? 0.0 : Corner.X, Corner.Y == 0 ? 0.0 : Corner.Y,
                    Corner.Z == 0 ? 0.0 : Corner.Z);
            }
        }

        bool operator==(const FQuadSurface& Other) const
        {
            return Corners[0] == Other.Corners[0] && Corners[1] == Other.Corners[1] &&
                Corners[2] == Other.Corners[2] && Corners[3] == Other.Corners[3];
        }

        friend uint32 GetTypeHash(const FQuadSurface& Surface)
        {
            using UE::Math::GetTypeHash;
            return HashCombine(HashCombine(GetTypeHash(Surface.Corners[0]), GetTypeHash(Surface.Corners[1])),
                HashCombine(GetTypeHash(Surface.Corners[2]), GetTypeHash(Surface.Corners[3])));
        }
    };

    struct FTriangleBasis
    {
        FVector3f Normal;
        FVector3f U = FVector3f::ZeroVector;
        FVector3f V = FVector3f::ZeroVector;
    };

    FTriangleBasis TriangleBasis(const FVector3f& A, const FVector3f& B, const FVector2f& Du, const FVector2f& Dv)
    {
        // The MC-to-UE transform reflects one axis, so front normals reverse the cross product.
        FTriangleBasis Basis{FVector3f::CrossProduct(B, A)};
        const float Determinant = Du.X * Dv.Y - Dv.X * Du.Y;
        if (FMath::Abs(Determinant) > 1.e-12f)
        {
            Basis.U = (A * Dv.Y - B * Du.Y) / Determinant;
            Basis.V = (B * Du.X - A * Dv.X) / Determinant;
        }
        return Basis;
    }

    FVector3f FaceNormal(uint8 Face, const FVector3f& Geometric)
    {
        static const FVector3f Cardinal[] = {
            {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}
        };
        static_assert(UE_ARRAY_COUNT(Cardinal) == GeometricNormal);
        return Face < GeometricNormal ? Cardinal[Face] :
            Geometric.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UpVector);
    }

    bool IsUnitTerrainTop(const FVector3f (&Positions)[4], const FVector3f& Normal)
    {
        if (Normal.Z < 0.99999f)
        {
            return false;
        }
        constexpr float ToleranceCm = 0.02f;
        constexpr float Span = float(BlockSize);
        const FVector3f& Corner = Positions[0];
        const FVector3f U = Positions[1] - Corner;
        const FVector3f V = Positions[3] - Corner;
        const bool bEdgesSpanOneBlock =
            (FMath::IsNearlyEqual(FMath::Abs(U.X), Span, ToleranceCm) && FMath::IsNearlyZero(U.Y, ToleranceCm) &&
             FMath::IsNearlyEqual(FMath::Abs(V.Y), Span, ToleranceCm) && FMath::IsNearlyZero(V.X, ToleranceCm)) ||
            (FMath::IsNearlyEqual(FMath::Abs(U.Y), Span, ToleranceCm) && FMath::IsNearlyZero(U.X, ToleranceCm) &&
             FMath::IsNearlyEqual(FMath::Abs(V.X), Span, ToleranceCm) && FMath::IsNearlyZero(V.Y, ToleranceCm));
        return bEdgesSpanOneBlock && FMath::IsNearlyZero(U.Z, ToleranceCm) && FMath::IsNearlyZero(V.Z, ToleranceCm) &&
            Positions[2].Equals(Corner + U + V, ToleranceCm) &&
            FMath::IsNearlyEqual(Corner.X, FMath::GridSnap(Corner.X, Span), ToleranceCm) &&
            FMath::IsNearlyEqual(Corner.Y, FMath::GridSnap(Corner.Y, Span), ToleranceCm);
    }

    bool IsWallHuggingQuad(uint8 Face, const FVector3f* Positions)
    {
        if (Face < 2 || Face >= GeometricNormal)
        {
            return false;
        }
        const float Coordinate = Face <= 3 ? Positions[0].X : Positions[0].Y;
        const float Span = float(BlockSize);
        const float Remainder = FMath::Fmod(Coordinate, Span);
        const float Local = Remainder < 0.0f ? Remainder + Span : Remainder;
        return Local < 12.0f || Local > Span - 12.0f;
    }

    FVector3f AdjustVegetationNormal(const FVector3f& Normal, uint8 Face, const FVector3f* Positions)
    {
        // Keep flat horizontal foliage (lily pads, petals, undersides) on their exact authored normal.
        if (Face <= 1 || FMath::Abs(Normal.Z) > 0.7f)
        {
            return Normal;
        }
        if (IsWallHuggingQuad(Face, Positions))
        {
            return (Normal * 0.85f + FVector3f::UpVector * 0.52f).GetSafeNormal();
        }
        return (Normal * 0.22f + FVector3f::UpVector * 0.975f).GetSafeNormal();
    }

    McTerrain::FTangents PackTangents(const FVector3f& Normal, const FVector3f& U, const FVector3f& V)
    {
        FVector3f Tangent = (U - Normal * FVector3f::DotProduct(Normal, U)).GetSafeNormal();
        if (Tangent.IsNearlyZero())
        {
            const FVector3f Axis = FMath::Abs(Normal.Z) < 0.9f ? FVector3f::UpVector : FVector3f::RightVector;
            Tangent = FVector3f::CrossProduct(Axis, Normal).GetSafeNormal();
        }
        const float Sign = FVector3f::DotProduct(FVector3f::CrossProduct(Normal, Tangent), V) < 0 ? -1.0f : 1.0f;
        return {FPackedNormal(Tangent), FPackedNormal(FVector4f(Normal.X, Normal.Y, Normal.Z, Sign))};
    }

    uint16 EncodeUNorm16(float Value)
    {
        return uint16(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 65535.0f));
    }

    uint32 PackBytes(const uint8* Data)
    {
        return uint32(Data[0]) | uint32(Data[1]) << 8 | uint32(Data[2]) << 16 | uint32(Data[3]) << 24;
    }

    struct FWireShoreSegment
    {
        FIntVector A;
        FIntVector B;
    };

    FIntVector ReadFixedPosition(BridgeWire::FReader& Reader)
    {
        const int32 X = int16(Reader.U16());
        const int32 Y = int16(Reader.U16());
        const int32 Z = int16(Reader.U16());
        return FIntVector(Z, -X, Y);
    }

    void PackShorePoint(uint32* Words, FIntVector Point)
    {
        Words[0] = uint32(uint16(Point.X)) | uint32(uint16(Point.Y)) << 16;
        Words[1] = uint32(Point.Z) & ((1u << 18) - 1);
    }

    McTerrain::FSectionData DecodeSection(const FIntVector& Section, int32 MinSectionY, int32 RegionWidth,
        const TArray<uint8>& Payload)
    {
        TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftTerrainDecode);
        McTerrain::FSectionData Result;
        BridgeWire::FReader Reader{Payload.GetData()};
        int32 Counts[TerrainLayers];
        for (int32& Count : Counts)
        {
            Count = Reader.U32();
        }

        int32 Quads = 0;
        for (const int32 Count : Counts) Quads += Count / 4;
        FReader Metadata{Reader.Data + Quads * SectionQuadBytes};
        const uint32 WaterQuads = Metadata.U32();
        const uint32 ShoreCount = Metadata.U32();
        const uint8* WaterRecord = Metadata.Data;
        Metadata.Data += WaterQuads * WaterFaceBytes;
        TMap<int32, TArray<FWireShoreSegment, TInlineAllocator<MaxShoreSegmentsPerQuad>>> Shores[TerrainLayers];
        for (uint32 I = 0; I < ShoreCount; ++I)
        {
            const int32 Layer = Metadata.Byte();
            const uint32 Quad = Metadata.U32();
            if (Layer >= TerrainLayers || Layer == McTerrain::Water || Quad >= uint32(Counts[Layer] / 4))
            {
                return Result;
            }
            const FIntVector A = ReadFixedPosition(Metadata);
            const FIntVector B = ReadFixedPosition(Metadata);
            // Source section plus a one-cell halo, so that packed region XY always fits int16.
            if (A.GetAbsMax() > 18 * 64 || B.GetAbsMax() > 18 * 64 || A == B)
            {
                return Result;
            }
            auto& Segments = Shores[Layer].FindOrAdd(Quad);
            if (Segments.Num() >= MaxShoreSegmentsPerQuad)
            {
                return Result;
            }
            Segments.Add({A, B});
        }

        const int32 Mask = RegionWidth - 1;
        const FVector3f Offset(ToUnreal((Section.X & Mask) * 16, (Section.Y - MinSectionY) * 16,
            (Section.Z & Mask) * 16) * BlockSize);
        const FIntVector FixedOffset((Section.Z & Mask) * 1024, -(Section.X & Mask) * 1024,
            (Section.Y - MinSectionY) * 1024);
        TSet<FQuadSurface> SolidFaces;
        const bool bCheckOverlays = Counts[McTerrain::Cutout] > 0 || Counts[McTerrain::CutoutNoShadow] > 0;
        if (bCheckOverlays)
        {
            SolidFaces.Reserve(Counts[McTerrain::Solid] / 4);
        }
        for (int32 Layer = 0; Layer < TerrainLayers; ++Layer)
        {
            auto& LayerData = Result.Layers[Layer];
            // Coincident overlays from Cutout may already be here when we reach CutoutNoShadow.
            const int32 Capacity = Counts[Layer] + LayerData.Positions.Num();
            LayerData.Positions.Reserve(Capacity);
            LayerData.UVs.Reserve(Capacity);
            LayerData.Tangents.Reserve(Capacity);
            LayerData.Colors.Reserve(Capacity);
            for (int32 Start = 0; Start < Counts[Layer]; Start += 4)
            {
                const uint8 RawFace = Reader.Byte();
                const uint8 EmissionMode = (RawFace & FaceEmissionMask) >> 3;
                const bool bHasEmissionLevel = (RawFace & 0x40) != 0;
                if (EmissionMode > 5)
                {
                    return Result;
                }
                const uint8 Face = RawFace & FaceIndexMask;
                const bool bVegetationFlag = (RawFace & FaceVegetationFlag) != 0;
                if (Face > GeometricNormal || (bVegetationFlag && Layer != McTerrain::CutoutNoShadow))
                {
                    return Result;
                }
                FVector3f Positions[4];
                FVector2f UVs[4];
                FColor Colors[4];
                uint8 Light[4];
                for (int32 V = 0; V < 4; ++V)
                {
                    const float X = Reader.Float();
                    const float Y = Reader.Float();
                    const float Z = Reader.Float();
                    const float U = Reader.Float();
                    const float T = Reader.Float();
                    if (!(FMath::Abs(X) <= 1024 && FMath::Abs(Y) <= 1024 && FMath::Abs(Z) <= 1024 &&
                        FMath::IsFinite(U) && FMath::IsFinite(T)))
                    {
                        return Result;
                    }
                    const uint8 R = Reader.Byte();
                    const uint8 G = Reader.Byte();
                    const uint8 B = Reader.Byte();
                    const uint8 A = Reader.Byte();
                    Positions[V] = FVector3f(Z, -X, Y) * float(BlockSize);
                    UVs[V] = FVector2f(U, T);
                    // Match CreateMeshSection_LinearColor's previous color conversion.
                    Colors[V] = FLinearColor(FColor(R, G, B, A)).ToFColor(false);
                    Light[V] = Reader.Byte();
                }

                const FVector3f Diagonal = Positions[2] - Positions[0];
                const FVector2f UVDiagonal = UVs[2] - UVs[0];
                const auto First = TriangleBasis(Positions[1] - Positions[0], Diagonal, UVs[1] - UVs[0], UVDiagonal);
                const auto Second = TriangleBasis(Diagonal, Positions[3] - Positions[0], UVDiagonal, UVs[3] - UVs[0]);
                const FVector3f Geometric = FaceNormal(Face, First.Normal + Second.Normal);

                int32 Bucket = Layer;
                FVector3f PositionOffset = Offset;
                if (Layer == McTerrain::Solid && bCheckOverlays)
                {
                    SolidFaces.Emplace(Positions);
                }
                else if ((Layer == McTerrain::Cutout || Layer == McTerrain::CutoutNoShadow) &&
                    SolidFaces.Contains(FQuadSurface(Positions)))
                {
                    Bucket = McTerrain::CutoutNoShadow;
                    ++Result.SeparatedOverlays;
                    PositionOffset += Geometric * 0.1f;
                }
                else if (bVegetationFlag)
                {
                    Bucket = McTerrain::Vegetation;
                }
                const bool bVegetation = Bucket == McTerrain::Vegetation;
                const FVector3f Normal = bVegetation ? AdjustVegetationNormal(Geometric, Face, Positions) : Geometric;
                const auto SharedTangents = PackTangents(Normal, First.U + Second.U, First.V + Second.V);
                auto& Data = Result.Layers[Bucket];
                bool bCanDisplace = false;
                if (Layer == McTerrain::Water)
                {
                    const uint8 Mode = WaterRecord[0];
                    if ((Mode & 0xc0) || ((Mode >> 2) & 3) == 3 || (WaterRecord[6] & ~3) || (WaterRecord[7] & ~15) ||
                        WaterRecord[8] > 64 || WaterRecord[9] > 64 || WaterRecord[10] > 64 || WaterRecord[11] > 64 ||
                        WaterRecord[2] == 128 || WaterRecord[3] == 128)
                    {
                        return Result;
                    }
                    auto& Record = Data.WetFaces.AddDefaulted_GetRef();
                    Record.Quad = Data.Positions.Num() / 4;
                    Record.Data.Words[0] = PackBytes(WaterRecord);
                    Record.Data.Words[1] = PackBytes(WaterRecord + 4);
                    Record.Data.Words[2] = PackBytes(WaterRecord + 8);
                    Record.Data.Words[3] = PackBytes(WaterRecord + 12);
                    const uint8 Role = Mode & 3;
                    const uint8 Flow = (Mode >> 2) & 3;
                    // The upper and reverse-wound underside share the same displacement.
                    // The material further locks every vertex near a non-flat or obstructed column.
                    bCanDisplace = (Mode & 15) == 0 && FMath::Abs(Normal.Z) > 0.99999f &&
                        FMath::IsNearlyEqual(Positions[0].Z, Positions[1].Z, 0.01f) &&
                        FMath::IsNearlyEqual(Positions[0].Z, Positions[2].Z, 0.01f) &&
                        FMath::IsNearlyEqual(Positions[0].Z, Positions[3].Z, 0.01f);
                    const bool bImpact = Role == 0 && Normal.Z > 0.0f && WaterRecord[13] >= 180;
                    const bool bToe = Flow == 2 && Role == 1 && WaterRecord[15] == 0 && WaterRecord[12] >= 2;
                    if (bImpact || bToe)
                    {
                        FVector3f Center = (Positions[0] + Positions[1] + Positions[2] + Positions[3]) * 0.25f;
                        if (bToe)
                        {
                            Center -= Normal * 12.0f;
                            Center.Z = FMath::Min(FMath::Min(Positions[0].Z, Positions[1].Z),
                                FMath::Min(Positions[2].Z, Positions[3].Z));
                        }
                        Result.Waterfalls.Add({
                            FVector(Section.X * 16 - double(Center.Y) / BlockSize, Section.Y * 16 + double(Center.Z) / BlockSize,
                                Section.Z * 16 + double(Center.X) / BlockSize),
                            float(WaterRecord[12]), bImpact});
                    }
                    WaterRecord += WaterFaceBytes;
                    for (FColor& Color : Colors)
                    {
                        Color.A = Mode | (bCanDisplace ? 64 : 0);
                    }
                }
                else if (const auto* Segments = Shores[Layer].Find(Start / 4))
                {
                    auto& Record = Data.WetFaces.AddDefaulted_GetRef();
                    Record.Quad = Data.Positions.Num() / 4;
                    for (int32 I = 0; I < Segments->Num(); ++I)
                    {
                        const auto& Segment = (*Segments)[I];
                        PackShorePoint(Record.Data.Words + I * 4, Segment.A + FixedOffset);
                        PackShorePoint(Record.Data.Words + I * 4 + 2, Segment.B + FixedOffset);
                    }
                }
                if (Bucket == McTerrain::Solid)
                {
                    // The opaque terrain material can safely clamp bottom parallax within a complete block top.
                    const uint8 PuddleTop = IsUnitTerrainTop(Positions, Geometric) ? 255 : 0;
                    for (FColor& Color : Colors)
                    {
                        Color.A = PuddleTop;
                    }
                }
                for (int32 V = 0; V < 4; ++V)
                {
                    const FVector3f Position = Positions[V] + PositionOffset;
                    Data.Positions.Add(Position);
                    Data.UVs.Add({EncodeUNorm16(UVs[V].X), EncodeUNorm16(UVs[V].Y),
                        uint16((EmissionMode + (bHasEmissionLevel ? 8 : 0)) * 4096 + (Light[V] & 15) * 128), uint16((Light[V] >> 4) * 4369)});
                    if (V == 1 || V == 3)
                    {
                        const auto& Basis = V == 1 ? First : Second;
                        const FVector3f CornerNormal = FaceNormal(Face, Basis.Normal);
                        Data.Tangents.Add(PackTangents(
                            bVegetation ? AdjustVegetationNormal(CornerNormal, Face, Positions) : CornerNormal,
                            Basis.U, Basis.V));
                    }
                    else
                    {
                        Data.Tangents.Add(SharedTangents);
                    }
                    Data.Colors.Add(Colors[V]);
                    Result.Bounds += FVector(Position);
                    if (bCanDisplace)
                    {
                        Result.Bounds += FVector(Position) + FVector(0.0, 0.0, 8.0);
                        Result.Bounds += FVector(Position) - FVector(0.0, 0.0, 8.0);
                    }
                }
            }
        }
        Result.bValid = true;
        return Result;
    }
}

bool ABridgeViewer::ApplyAtlasRows(BridgeWire::ETerrainAtlas Kind, BridgeWire::FReader Reader, int32 Size)
{
    using namespace BridgeWire;
    const int32 Index = int32(Kind);
    FBridgeTerrainAtlasUpload& Atlas = TerrainAtlases[Index];
    const FBridgeTerrainAtlasUpload& Albedo = TerrainAtlases[0];
    const bool bAlbedo = Kind == ETerrainAtlas::Albedo;
    if (bAtlasStaging && bAlbedo && Size >= AtlasRowsHeaderBytes &&
        Reader.Data[0] == 0 && Reader.Data[1] == 0 && Reader.Data[8] == 0 && Reader.Data[9] == 0)
    {
        for (FBridgeTerrainAtlasUpload& Staged : TerrainAtlases)
        {
            Staged = {};
        }
        bAtlasStaging = false;
    }
    if (Height == 0 || Size < AtlasRowsHeaderBytes || Atlas.IsComplete() ||
        (Index > 0 && !TerrainAtlases[Index - 1].IsComplete()))
    {
        return false;
    }
    const int32 Mip = Reader.U16();
    const int32 MipCount = Reader.U16();
    const int32 Width = Reader.U16();
    const int32 HeightPx = Reader.U16();
    const int32 FirstRow = Reader.U16();
    const int32 Rows = Reader.U16();
    const int64 PixelBytes = int64(Rows) * Width * 4;
    if (Width <= 0 || HeightPx <= 0 || Width > MaxAtlasEdge || HeightPx > MaxAtlasEdge ||
        MipCount < 1 || MipCount > MaxAtlasMips || Mip >= MipCount || Mip != Atlas.NextMip ||
        Rows <= 0 || FirstRow != Atlas.NextRow || FirstRow + Rows > HeightPx ||
        PixelBytes > AtlasPacketBytes || Size != AtlasRowsHeaderBytes + PixelBytes)
    {
        return false;
    }
    if (bAlbedo && Mip == 0 && FirstRow == 0)
    {
        if (bAtlasUseDisk)
        {
            return false;
        }
        if (AtlasReadCancelled)
        {
            *AtlasReadCancelled = true;
        }
        PreparedAtlasFile.Reset();
        SetAtlasReply(EAtlasCacheStatus::Rejected);
    }
    if (!Atlas.Texture)
    {
        if ((FMath::Min(Width, HeightPx) >> (MipCount - 1)) == 0 ||
            (!bAlbedo && (MipCount != Albedo.MipCount || Width != Albedo.Texture->GetSizeX() ||
                HeightPx != Albedo.Texture->GetSizeY())))
        {
            return false;
        }
        StartTerrainTexture(Atlas, Width, HeightPx, MipCount, bAlbedo);
    }

    FTexturePlatformData& Data = *Atlas.Texture->GetPlatformData();
    if (MipCount != Atlas.MipCount || Width != (Data.SizeX >> Mip) || HeightPx != (Data.SizeY >> Mip))
    {
        return false;
    }
    if (FirstRow == 0)
    {
        Data.Mips.Add(new FTexture2DMipMap(Width, HeightPx, 1));
    }
    FByteBulkData& Pixels = Data.Mips[Mip].BulkData;
    void* Destination = Pixels.Lock(LOCK_READ_WRITE);
    if (FirstRow == 0)
    {
        Destination = Pixels.Realloc(Width * HeightPx * 4);
    }
    FMemory::Memcpy(static_cast<uint8*>(Destination) + FirstRow * Width * 4, Reader.Data, PixelBytes);
    Pixels.Unlock();
    Atlas.NextRow += Rows;
    if (Atlas.NextRow < HeightPx)
    {
        return true;
    }
    Atlas.NextRow = 0;
    if (++Atlas.NextMip < Atlas.MipCount)
    {
        return true;
    }

    UE_LOG(LogTemp, Display, TEXT("Received Minecraft %s %dx%d, mips=%d scene=%u."),
        AtlasParameters[Index], Data.SizeX, Data.SizeY, Atlas.MipCount, SceneSerial);
    if (Kind == ETerrainAtlas::Pbr)
    {
        FinishTerrainAtlases();
    }
    return true;
}

void ABridgeViewer::StartTerrainTexture(FBridgeTerrainAtlasUpload& Atlas, int32 Width, int32 HeightPx,
    int32 MipCount, bool bAlbedo)
{
    Atlas.Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
    auto* Data = new FTexturePlatformData();
    Data->SizeX = Width;
    Data->SizeY = HeightPx;
    Data->PixelFormat = PF_B8G8R8A8;
    Data->SetNumSlices(1);
    Atlas.Texture->SetPlatformData(Data);
    Atlas.Texture->bNotOfflineProcessed = true;
    Atlas.Texture->NeverStream = true;
    Atlas.Texture->SRGB = bAlbedo;
    Atlas.Texture->AddressX = TA_Clamp;
    Atlas.Texture->AddressY = TA_Clamp;
    Atlas.MipCount = MipCount;
}

void ABridgeViewer::FinishTerrainAtlases()
{
    for (const FBridgeTerrainAtlasUpload& Upload : TerrainAtlases)
    {
        if (!Upload.IsComplete())
        {
            return;
        }
    }
    if (!AtlasOfferIdentity.IsEmpty())
    {
        ReusableAtlasIdentity = AtlasOfferIdentity;
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(TerrainAtlases); ++Index)
        {
            ReusableTerrainAtlases[Index] = TerrainAtlases[Index];
        }
    }
    BuildTerrainMaterials();
}

void ABridgeViewer::BuildTerrainMaterials()
{
    using namespace BridgeWire;
    TerrainMaterials.Reset(McTerrain::LayerCount);
    for (UMaterialInterface* Base : TerrainMaterialBases)
    {
        TerrainMaterials.Add(UMaterialInstanceDynamic::Create(Base, this));
    }
    if (WaterSim)
    {
        WaterSim->Bind(*TerrainMaterials[McTerrain::Water]);
    }
    UpdateTerrainFiltering();
    UpdateTerrainLightingParameters();
    const FBridgeTerrainAtlasUpload& Atlas = TerrainAtlases[0];
    UE_LOG(LogTemp, Display, TEXT("Received Minecraft block atlases %dx%d, mips=%d scene=%u."),
        Atlas.Texture->GetSizeX(), Atlas.Texture->GetSizeY(), Atlas.MipCount, SceneSerial);
}

void ABridgeViewer::UpdateTerrainFiltering()
{
    using namespace BridgeWire;
    if (TerrainMaterials.IsEmpty())
    {
        return;
    }

    const bool bUseAF = !bTerrainNoAF && !bTerrainMip0 && TerrainFiltering == ETerrainFiltering::Anisotropic &&
        RequestedAnisotropy >= 2;
    const bool bUseRgss = !bTerrainMip0 && TerrainFiltering == ETerrainFiltering::Rgss;
    const int32 Anisotropy = bUseAF ? 1 << FMath::FloorLog2(uint32(RequestedAnisotropy)) : 1;
    const TextureGroup Group = Anisotropy == 8 ? TEXTUREGROUP_Project03 : Anisotropy == 4 ? TEXTUREGROUP_Project02 :
        Anisotropy == 2 ? TEXTUREGROUP_Project01 : TEXTUREGROUP_Pixels2D;
    const auto& GroupSettings = UDeviceProfileManager::Get().GetActiveProfile()->GetTextureLODSettings()->GetTextureLODGroup(Group);
    const bool bSamplerConfigured = bUseAF && GroupSettings.MaxAniso == Anisotropy &&
        GroupSettings.Filter == ETextureSamplerFilter::AnisotropicLinear;
    int32 TextureUploads = 0;
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(TerrainAtlases); ++Index)
    {
        UTexture2D* Texture = TerrainAtlases[Index].Texture;
        const bool bHasResource = Texture->GetResource() != nullptr;
        const bool bUpdateResource = !bHasResource || Texture->LODGroup != Group;
        if (bUpdateResource)
        {
            Texture->Filter = bSamplerConfigured ? TF_Default : TF_Trilinear;
            Texture->LODGroup = Group;
            Texture->UpdateResource();
            ++TextureUploads;
        }
        const FName Parameter(AtlasParameters[Index]);
        const auto BindTexture = [Parameter, Texture, bRebind = bUpdateResource && bHasResource](UMaterialInstanceDynamic* Material)
        {
            if (bRebind)
            {
                Material->SetTextureParameterValue(Parameter, nullptr);
            }
            Material->SetTextureParameterValue(Parameter, Texture);
        };
        for (UMaterialInstanceDynamic* Material : TerrainMaterials)
        {
            BindTexture(Material);
        }
    }
    UE_CLOG(bUseAF && !bSamplerConfigured, LogTemp, Warning, TEXT("Minecraft terrain atlas AF group lacks the requested sampler."));

    static const FName FilterParameter(TEXT("TerrainFilter"));
    static const FName SizeParameter(TEXT("AtlasDimensions"));
    const auto& Albedo = TerrainAtlases[0];
    const FLinearColor Dimensions(Albedo.Texture->GetSizeX(), Albedo.Texture->GetSizeY(), 0, 0);
    const float Mode = bTerrainMip0 ? 3.0f : bSamplerConfigured ? 2.0f : bUseRgss ? 1.0f : 0.0f;
    for (UMaterialInstanceDynamic* Material : TerrainMaterials)
    {
        Material->SetScalarParameterValue(FilterParameter, Mode);
        Material->SetVectorParameterValue(SizeParameter, Dimensions);
    }
    for (const auto& Pair : TerrainRegions)
    {
        Pair.Value->InvalidateMaskedShadows();
    }
    UE_LOG(LogTemp, Display, TEXT("Minecraft terrain filtering: scene=%u requested=%u configuredAF=%u effective=%s af=%d mips=%d textureUploads=%d."),
        SceneSerial, uint32(TerrainFiltering), uint32(RequestedAnisotropy),
        bTerrainMip0 ? TEXT("mip0-point") : bSamplerConfigured ? TEXT("anisotropic") : bUseRgss ? TEXT("rgss") : TEXT("trilinear"),
        bSamplerConfigured ? Anisotropy : 1, Albedo.MipCount, TextureUploads);
}

bool ABridgeViewer::IsSectionInWorld(const FIntVector& Section) const
{
    using BridgeWire::MaxChunkCoordinate;
    return Section.X >= -MaxChunkCoordinate && Section.X <= MaxChunkCoordinate &&
        Section.Z >= -MaxChunkCoordinate && Section.Z <= MaxChunkCoordinate &&
        Section.Y >= MinY / 16 && Section.Y < (MinY + Height) / 16;
}

bool ABridgeViewer::ApplyTerrainShm(BridgeWire::FReader Descriptor, int32 Size)
{
    using namespace BridgeWire;
    using namespace BridgeShm;
    if (Size != TerrainShmBytes || TerrainMaterials.IsEmpty() || Descriptor.U32() != SceneSerial)
    {
        return false;
    }
    const uint64 TokenHigh = Descriptor.U32();
    const uint64 Token = (TokenHigh << 32) | Descriptor.U32();
    const uint32 Offset = Descriptor.U32();
    const uint32 MeshBytes = Descriptor.U32();
    const uint32 LightBytes = Descriptor.U32();
    const uint8 Priority = Descriptor.Byte();
    if (Token <= LastTerrainToken || Priority > 1 || MeshBytes < SectionHeaderBytes + SectionWaterHeaderBytes ||
        MeshBytes >= MaxSectionFrame || LightBytes < SectionLightsHeaderBytes || LightBytes > MaxMaterialLightBytes ||
        MeshBytes + LightBytes > MaxTerrainBodyBytes)
    {
        return false;
    }
    const uint32 BodyBytes = MeshBytes + LightBytes;
    const uint8* Data = TerrainShm.Resolve(Offset, BodyBytes);
    const uint32 PriorityStart = TerrainHeaderBytes + TerrainBulkBytes;
    const uint32 LargeStart = PriorityStart + TerrainPriorityBytes;
    if (!Data || (Offset < PriorityStart && Offset + BodyBytes > PriorityStart) ||
        (Offset >= PriorityStart && Offset < LargeStart && (Priority == 0 || Offset + BodyBytes > LargeStart)) ||
        (Offset >= LargeStart && (Offset != LargeStart || BodyBytes <= TerrainBulkBytes)) ||
        PendingSections.Num() >= MaxTerrainLeases)
    {
        return false;
    }
    for (const auto& Pair : PendingSections)
    {
        const FPendingSection& Pending = Pair.Value;
        if (Pending.Offset < Offset + BodyBytes && Offset < Pending.Offset + Pending.MeshBytes + Pending.LightBytes)
        {
            return false;
        }
    }
    FReader Reader{Data};
    const int32 X = Reader.I32();
    const int32 Y = Reader.I32();
    const int32 Z = Reader.I32();
    const FIntVector Section(X, Y, Z);
    const FIntPoint Column(X, Z);
    if (!IsSectionInWorld(Section) ||
        (!TerrainColumns.Contains(Column) && TerrainColumns.Num() >= MaxChunks))
    {
        return false;
    }
    const uint8* LayerData = Reader.Data;
    uint64 Total = 0;
    uint32 WaterVertices = 0;
    for (int32 Layer = 0; Layer < TerrainLayers; ++Layer)
    {
        const uint32 Count = Reader.U32();
        if (Count % 4 != 0)
        {
            return false;
        }
        Total += Count;
        if (Layer == McTerrain::Water) WaterVertices = Count;
    }
    const uint64 GeometryBytes = SectionHeaderBytes + Total / 4 * SectionQuadBytes;
    if (Total > MaxSectionVertices || MeshBytes < GeometryBytes + SectionWaterHeaderBytes)
    {
        return false;
    }
    FReader Metadata{LayerData - 12 + GeometryBytes};
    const uint64 WaterCount = Metadata.U32();
    const uint64 ShoreCount = Metadata.U32();
    if (WaterCount != WaterVertices / 4 || ShoreCount > (Total - WaterVertices) / 4 * MaxShoreSegmentsPerQuad ||
        MeshBytes != GeometryBytes + SectionWaterHeaderBytes + WaterCount * WaterFaceBytes + ShoreCount * ShoreSegmentBytes)
    {
        return false;
    }

    FReader Lights{Data + MeshBytes};
    if (Lights.U32() != SceneSerial || Lights.I32() != X || Lights.I32() != Y || Lights.I32() != Z)
    {
        return false;
    }

    TerrainColumns.Add(Column);
    if (const FPendingSection* Previous = PendingSections.Find(Section))
    {
        ReleaseTerrainLease(Previous->Token);
    }
    PendingSections.Add(Section, {Token, Offset, int32(MeshBytes), int32(LightBytes), Priority != 0, FPlatformTime::Seconds()});
    LastTerrainToken = Token;
    ++SectionFrames;
    return true;
}

void ABridgeViewer::ReleaseTerrainLease(uint64 Token)
{
    if (Client)
    {
        TerrainReleases.Add(Token);
    }
}

void ABridgeViewer::ClearPendingSections()
{
    for (const auto& Pair : PendingSections)
    {
        ReleaseTerrainLease(Pair.Value.Token);
    }
    PendingSections.Reset();
    ConsecutiveUrgentCommits = 0;
}

bool ABridgeViewer::ApplyDrop(BridgeWire::FReader Reader, int32 Size)
{
    if (Size <= 0 || Size % 8 != 0 || Size / 8 > BridgeWire::MaxChunks)
    {
        return false;
    }
    for (int32 Offset = 0; Offset < Size; Offset += 8)
    {
        const int32 X = Reader.I32();
        const int32 Z = Reader.I32();
        const FIntPoint Column(X, Z);
        DropBlockEffects(Column);
        if (WaterParticles) WaterParticles->ReplaceChunk(Column, {});
        DropPrecipitationChunk(Column);
        if (ChunkWaterData.Remove(Column)) DirtyWaterChunks.Add(Column);
        for (int32 Y = MinY / 16; GlassLighting.HasSections() && Y < (MinY + Height) / 16; ++Y)
        {
            GlassLighting.Drop(FIntVector(X, Y, Z));
        }
        if (!TerrainColumns.Remove(Column))
        {
            continue;
        }
        for (int32 Y = MinY / 16; Y < (MinY + Height) / 16; ++Y)
        {
            const FIntVector Section(X, Y, Z);
            if (WaterfallSections.Remove(Section)) WaterfallScanIn = 0.0f;
            if (SectionLights.Remove(Section) > 0)
            {
                NextPointLightUpdate = 0.0;
            }
            FPendingSection Pending;
            if (PendingSections.RemoveAndCopyValue(Section, Pending))
            {
                ReleaseTerrainLease(Pending.Token);
            }
            DecodingSections.Remove(Section);
            if (HasTerrainSection(Section))
            {
                PendingRemoval.Add(Section);
            }
        }
        ++DropFrames;
    }
    return true;
}

bool ABridgeViewer::ApplySceneReady(BridgeWire::FReader Reader, int32 Size)
{
    if (Size != 4 || TerrainMaterials.IsEmpty() || Reader.U32() != SceneSerial)
    {
        return false;
    }
    bSceneMeshesReceived = true;
    return true;
}

bool ABridgeViewer::HasTerrainSection(const FIntVector& Section) const
{
    const int32 Width = BridgeQuality::TerrainRegionEdge();
    const auto* Region = TerrainRegions.Find(McTerrain::RegionOf(Section, Width));
    return Region && (*Region)->HasSection(McTerrain::SlotOf(Section, MinY / 16, Width));
}

int32 ABridgeViewer::GetPendingSectionCount() const
{
    return PendingSections.Num() + DecodingSections.Num() + PendingRemoval.Num();
}

bool ABridgeViewer::UploadPendingSections()
{
    TRACE_CPUPROFILER_EVENT_SCOPE(MinecraftTerrainCommit);
    constexpr int32 UploadBudgetBytes = 2 * 1024 * 1024;
    const double StartAt = FPlatformTime::Seconds();
    const double StopAt = StartAt + 0.002;
    const int32 RegionWidth = BridgeQuality::TerrainRegionEdge();
    TMap<FIntPoint, TArray<McTerrain::FSectionUpdate>> Commits;
    struct FCommitTrace { FIntVector Section; uint64 Token; double ReceivedAt; };
    TArray<FCommitTrace, TInlineAllocator<4>> Traces;

    for (int32 I = 0; I < 32 && !PendingRemoval.IsEmpty() && FPlatformTime::Seconds() < StartAt + 0.00025; ++I)
    {
        auto First = PendingRemoval.CreateIterator();
        Commits.FindOrAdd(McTerrain::RegionOf(*First, RegionWidth)).Add({
            McTerrain::SlotOf(*First, MinY / 16, RegionWidth), {}});
        if (WaterOptics) WaterOptics->RemoveSection(*First);
        First.RemoveCurrent();
    }

    int32 UploadedBytes = 0;
    while (!DecodeJobs.IsEmpty() && FPlatformTime::Seconds() < StartAt + 0.001)
    {
        int32 Near = INDEX_NONE;
        int32 Bulk = INDEX_NONE;
        int32 Stale = INDEX_NONE;
        for (int32 Index = 0; Index < DecodeJobs.Num(); ++Index)
        {
            const FSectionDecodeJob& Job = DecodeJobs[Index];
            if (!Job.Result.IsReady())
            {
                continue;
            }
            const uint64* Revision = DecodingSections.Find(Job.Section);
            if (!Revision || *Revision != Job.Revision || PendingSections.Contains(Job.Section))
            {
                if (Revision && *Revision == Job.Revision)
                {
                    DecodingSections.Remove(Job.Section);
                }
                Stale = Index;
                break;
            }
            if (UploadedBytes == 0 || UploadedBytes + Job.PayloadBytes <= UploadBudgetBytes)
            {
                int32& Candidate = Job.bUrgent ? Near : Bulk;
                if (Candidate == INDEX_NONE || Job.Revision < DecodeJobs[Candidate].Revision)
                {
                    Candidate = Index;
                }
            }
        }
        if (Stale != INDEX_NONE)
        {
            DecodeJobs.RemoveAtSwap(Stale, EAllowShrinking::No);
            continue;
        }
        const int32 Selected = Near != INDEX_NONE && (Bulk == INDEX_NONE || ConsecutiveUrgentCommits < 3) ? Near : Bulk;
        if (Selected == INDEX_NONE)
        {
            break;
        }
        FSectionDecodeJob& Job = DecodeJobs[Selected];
        DecodingSections.Remove(Job.Section);
        McTerrain::FSectionData Data = Job.Result.Consume();
        if (!Data.bValid)
        {
            UE_LOG(LogTemp, Warning, TEXT("Rejected invalid Minecraft section mesh %s."), *Job.Section.ToString());
            return false;
        }
        UE_CLOG(bTrace, LogTemp, VeryVerbose,
            TEXT("Minecraft section %s decoded: solid=%d cutout=%d cutoutNoShadow=%d vegetation=%d foliage=%d water=%d translucent=%d overlays=%d."),
            *Job.Section.ToString(), Data.Layers[McTerrain::Solid].Positions.Num(),
            Data.Layers[McTerrain::Cutout].Positions.Num(), Data.Layers[McTerrain::CutoutNoShadow].Positions.Num(),
            Data.Layers[McTerrain::Vegetation].Positions.Num(), Data.Layers[McTerrain::Foliage].Positions.Num(),
            Data.Layers[McTerrain::Water].Positions.Num(), Data.Layers[McTerrain::Translucent].Positions.Num(),
            Data.SeparatedOverlays);
        PendingRemoval.Remove(Job.Section);
        if (!Data.Waterfalls.IsEmpty())
        {
            WaterfallSections.Add(Job.Section, MoveTemp(Data.Waterfalls));
            WaterfallScanIn = 0.0f;
        }
        else if (WaterfallSections.Remove(Job.Section))
        {
            WaterfallScanIn = 0.0f;
        }
        if (WaterOptics) WaterOptics->CommitSection(Job.Section, RegionWidth, MinY, Data);
        if (Data.Bounds.IsValid || HasTerrainSection(Job.Section))
        {
            Commits.FindOrAdd(McTerrain::RegionOf(Job.Section, RegionWidth)).Add({
                McTerrain::SlotOf(Job.Section, MinY / 16, RegionWidth), MoveTemp(Data)});
        }
        if (bTrace && Job.bUrgent)
        {
            Traces.Add({Job.Section, Job.Token, Job.ReceivedAt});
        }
        ConsecutiveUrgentCommits = Job.bUrgent ? FMath::Min(ConsecutiveUrgentCommits + 1, 3) : 0;
        ++SectionRebuilds;
        UploadedBytes += Job.PayloadBytes;
        DecodeJobs.RemoveAtSwap(Selected, EAllowShrinking::No);
    }

    for (auto& Pair : Commits)
    {
        UMcTerrainRegionComponent* Region = TerrainRegions.FindRef(Pair.Key);
        if (!Region)
        {
            if (!Pair.Value.ContainsByPredicate([](const McTerrain::FSectionUpdate& Update) { return Update.Data.Bounds.IsValid; }))
            {
                continue;
            }
            Region = NewObject<UMcTerrainRegionComponent>(this);
            Region->SetupAttachment(RootComponent);
            const int32 Blocks = RegionWidth * 16;
            Region->SetRelativeLocation(BridgeWire::ToUnreal(Pair.Key.X * Blocks, MinY, Pair.Key.Y * Blocks) * BridgeWire::BlockSize);
            for (int32 Layer = 0; Layer < McTerrain::LayerCount; ++Layer)
            {
                Region->SetMaterial(Layer, TerrainMaterials[Layer]);
            }
            TerrainRegions.Add(Pair.Key, Region);
        }
        LoadedSections -= Region->GetSectionCount();
        Region->UpdateSections(MoveTemp(Pair.Value));
        LoadedSections += Region->GetSectionCount();
        if (Region->GetSectionCount() == 0)
        {
            Region->DestroyComponent();
            TerrainRegions.Remove(Pair.Key);
        }
        else if (!Region->IsRegistered())
        {
            Region->RegisterComponent();
        }
    }

    for (const auto& Trace : Traces)
    {
        UE_LOG(LogTemp, Display, TEXT("V27 terrain near commit scene=%u token=%llu section=%s sinceDescriptorMs=%.3f."),
            SceneSerial, Trace.Token, *Trace.Section.ToString(), (FPlatformTime::Seconds() - Trace.ReceivedAt) * 1000.0);
    }

    for (const bool bUrgent : {true, false})
    {
        // Separate quotas reserve dispatch capacity for edits while allowing a bulk job to advance every tick.
        constexpr int32 NearDecodeJobs = 4;
        constexpr int32 BulkDecodeJobs = 24;
        const int32 MaxJobs = bUrgent ? NearDecodeJobs : BulkDecodeJobs;
        const int32 Budget = (bUrgent ? 2 : 6) * 1024 * 1024;
        int32 ActiveJobs = 0;
        int32 ActiveBytes = 0;
        for (const auto& Job : DecodeJobs)
        {
            if (Job.bUrgent == bUrgent)
            {
                ++ActiveJobs;
                ActiveBytes += Job.PayloadBytes;
            }
        }
        for (int32 Dispatched = 0; !PendingSections.IsEmpty() && ActiveJobs < MaxJobs &&
            (Dispatched == 0 || FPlatformTime::Seconds() < StopAt); ++Dispatched)
        {
            FIntVector Section;
            const FPendingSection* Next = nullptr;
            for (const auto& Pair : PendingSections)
            {
                if (Pair.Value.bUrgent == bUrgent && (!Next || Pair.Value.Token < Next->Token))
                {
                    Section = Pair.Key;
                    Next = &Pair.Value;
                }
            }
            if (!Next || (ActiveJobs != 0 && ActiveBytes + Next->MeshBytes > Budget))
            {
                break;
            }
            const uint8* Data = TerrainShm.Resolve(Next->Offset, Next->MeshBytes + Next->LightBytes);
            if (!Data || !ApplySectionLights({Data + Next->MeshBytes}, Next->LightBytes))
            {
                UE_LOG(LogTemp, Warning, TEXT("Rejected invalid Minecraft section lights %s."), *Section.ToString());
                return false;
            }
            TArray<uint8> Payload;
            Payload.Append(Data + 12, Next->MeshBytes - 12);
            const int32 Bytes = Payload.Num();
            const uint64 Revision = ++NextDecodeRevision;
            const uint64 Token = Next->Token;
            const double ReceivedAt = Next->ReceivedAt;
            if (bTrace && bUrgent)
            {
                int32 BulkQueued = 0;
                for (const auto& Pair : PendingSections)
                {
                    BulkQueued += !Pair.Value.bUrgent;
                }
                UE_LOG(LogTemp, Display, TEXT("V27 terrain near dispatch scene=%u token=%llu section=%s bulkPending=%d sinceDescriptorMs=%.3f."),
                    SceneSerial, Token, *Section.ToString(), BulkQueued, (FPlatformTime::Seconds() - ReceivedAt) * 1000.0);
            }
            ReleaseTerrainLease(Token); // All subsequent work owns the local copy.
            PendingSections.Remove(Section);
            DecodingSections.Add(Section, Revision);
            ++ActiveJobs;
            ActiveBytes += Bytes;
            DecodeJobs.Add({Section, Revision, Token, Bytes, bUrgent, ReceivedAt,
                AsyncPool(*GThreadPool,
                    [Section, MinSectionY = MinY / 16, RegionWidth, Payload = MoveTemp(Payload)]
                    {
                        return DecodeSection(Section, MinSectionY, RegionWidth, Payload);
                    },
                    nullptr, bUrgent ? EQueuedWorkPriority::High : EQueuedWorkPriority::Normal)});
        }
    }
    return true;
}
